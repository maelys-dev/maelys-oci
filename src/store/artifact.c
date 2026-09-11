/* SPDX-License-Identifier: MPL-2.0 */
/*
 * Public artifact API: resolve a sealed artifact under the ordered store
 * locks, hold an execution lease on it whose liveness the kernel answers
 * through an exclusive lock, revalidate its identity later and retire the
 * lease on release.
 */
#include "src/store/core.h"
#include "src/store/seal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define LEASE_TTL_SECONDS UINT64_C(604800)

/* One sealed member as it was at resolution. */
typedef struct member {
    char *path;
    dev_t device;
    ino_t inode;
    mode_t mode;
    off_t size;
    char digest[MAELYS_OCI_DIGEST_HEX_SIZE];
} member_t;

struct maelys_oci_artifact {
    char *store;
    char image_digest[OCI_DIGEST_SIZE];
    char platform[OCI_PLATFORM_SIZE];
    char *directory;
    member_t root;
    member_t rootfs_tar;
    member_t seal;
    char config_digest[OCI_DIGEST_SIZE];
    char *lease_path;
    maelys_sys_file_identity_t lease_identity;
    /* The exclusive lock on the lease; NULL until the lease is published. */
    maelys_sys_file_lock_t *lease_lock;
};

static int random_bytes(unsigned char *output, size_t size) {
    int descriptor = open("/dev/urandom", O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) return -1;
    size_t offset = 0u;
    while (offset < size) {
        ssize_t amount = read(descriptor, output + offset, size - offset);
        if (amount > 0) offset += (size_t)amount;
        else if (amount < 0 && errno == EINTR) continue;
        else break;
    }
    (void)maelys_sys_fd_close(&descriptor);
    return offset == size ? 0 : -1;
}

/* Removes the lease by identity while its lock is held, syncs the
 * directory and releases the lock. Safe on a handle without a lease. */
static void retire_lease(maelys_oci_artifact_t *artifact) {
    if (artifact->lease_lock && artifact->lease_path) {
        (void)maelys_sys_file_unlink_same(
            artifact->lease_path, &artifact->lease_identity);
        char leases[PATH_MAX];
        if (oci_snprintf(leases, sizeof(leases), "%s/leases",
                artifact->store) >= 0)
            (void)maelys_oci_store_fsync_directory(leases);
    }
    (void)maelys_sys_file_lock_release(&artifact->lease_lock);
}

/* Publishes the lease document then takes its exclusive lock: the store
 * lock held shared by the caller keeps gc, which needs it exclusively, out
 * of the window between the two. */
static maelys_oci_result_t create_lease(
    maelys_oci_artifact_t *artifact, char **out_error) {
    unsigned char random[16];
    if (random_bytes(random, sizeof(random)) != 0) {
        maelys_oci_set_error(
            out_error, "cannot draw a random OCI lease identifier");
        return MAELYS_OCI_ERR_IO;
    }
    char id[33];
    oci_bytes_to_hex(random, sizeof(random), id);
    char leases[PATH_MAX];
    char destination[PATH_MAX];
    if (maelys_oci_store_ensure_private_directory(
            artifact->store, "leases", leases, sizeof(leases)) != 0 ||
        oci_snprintf(destination, sizeof(destination), "%s/%s.json",
            leases, id) < 0) {
        maelys_oci_set_error(out_error, "OCI lease namespace is unavailable");
        return MAELYS_OCI_ERR_UNSUPPORTED;
    }
    time_t now = time(NULL);
    char document[1024];
    int document_size = now >= 0 ? oci_snprintf(document, sizeof(document),
        "{\"createdUnixSeconds\":%llu,\"expiresUnixSeconds\":%llu,"
        "\"liveness\":\"" MAELYS_OCI_LEASE_LIVENESS "\","
        "\"manifestDigest\":\"%s\",\"platform\":\"%s\","
        "\"rootDigest\":\"sha256:%s\","
        "\"rootfsTarDigest\":\"sha256:%s\","
        "\"schema\":\"" MAELYS_OCI_LEASE_SCHEMA "\"}\n",
        (unsigned long long)now,
        (unsigned long long)now + LEASE_TTL_SECONDS,
        artifact->image_digest, artifact->platform, artifact->root.digest,
        artifact->rootfs_tar.digest) : -1;
    if (document_size <= 0) {
        maelys_oci_set_error(out_error, "cannot render the OCI lease document");
        return MAELYS_OCI_ERR_IO;
    }
    artifact->lease_path = strdup(destination);
    if (!artifact->lease_path) return MAELYS_OCI_ERR_MEMORY;
    if (maelys_sys_file_write_exclusive(destination, document,
            (size_t)document_size, 0400) != MAELYS_SYS_OK) {
        maelys_oci_set_error(out_error, "cannot durably create OCI lease");
        return MAELYS_OCI_ERR_IO;
    }
    maelys_sys_file_identity_t published;
    maelys_sys_file_identity_t locked;
    maelys_sys_file_expectations_t expectations;
    maelys_oci_store_lease_expectations(&expectations);
    if (maelys_sys_file_path_identity(destination, &published) != MAELYS_SYS_OK ||
        maelys_oci_store_lease_probe(destination, &artifact->lease_lock) != 0 ||
        maelys_sys_file_verify(maelys_sys_file_lock_fd(artifact->lease_lock),
            &expectations, &locked, NULL) != MAELYS_SYS_OK ||
        !maelys_sys_file_identity_same(&published, &locked)) {
        if (!artifact->lease_lock)
            (void)maelys_sys_file_unlink_same(destination, &published);
        artifact->lease_identity = published;
        retire_lease(artifact);
        maelys_oci_set_error(
            out_error, "cannot hold the kernel lock of the OCI lease");
        return MAELYS_OCI_ERR_IO;
    }
    artifact->lease_identity = locked;
    if (maelys_oci_store_fsync_directory(leases) != 0) {
        retire_lease(artifact);
        maelys_oci_set_error(out_error, "cannot durably publish OCI lease");
        return MAELYS_OCI_ERR_IO;
    }
    return MAELYS_OCI_OK;
}

/* Hashes one sealed member and requires it to be non-empty. */
static maelys_oci_result_t hash_member(
    const char *path, struct stat *out_status,
    char digest[MAELYS_OCI_DIGEST_HEX_SIZE], char **out_error) {
    if (maelys_oci_store_hash_immutable(path, 0400, digest, out_status) != 0 ||
        out_status->st_size <= 0) {
        maelys_oci_set_error(out_error,
            "OCI artifact member is absent, unsafe or mutable: %s", path);
        return MAELYS_OCI_ERR_UNSUPPORTED;
    }
    return MAELYS_OCI_OK;
}

static maelys_oci_result_t resolve_member(
    member_t *member, struct stat *out_status, char **out_error) {
    maelys_oci_result_t result =
        hash_member(member->path, out_status, member->digest, out_error);
    if (result == MAELYS_OCI_OK) {
        member->device = out_status->st_dev;
        member->inode = out_status->st_ino;
        member->mode = out_status->st_mode;
        member->size = out_status->st_size;
    }
    return result;
}

static int store_v2_ready(const char *store) {
    char version[PATH_MAX];
    char migration[PATH_MAX];
    if (oci_snprintf(version, sizeof(version), "%s/store.version", store) < 0 ||
        oci_snprintf(migration, sizeof(migration), "%s/migration.json",
            store) < 0)
        return 0;
    int descriptor = open(version, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    char bytes[3] = {0};
    struct stat status;
    ssize_t amount = descriptor >= 0 ? read(descriptor, bytes, sizeof(bytes)) : -1;
    int valid = descriptor >= 0 && fstat(descriptor, &status) == 0 &&
        S_ISREG(status.st_mode) && status.st_uid == geteuid() &&
        (status.st_mode & 0777) == 0400 && amount == 2 &&
        bytes[0] == '2' && bytes[1] == '\n';
    if (descriptor >= 0) (void)maelys_sys_fd_close(&descriptor);
    struct stat migration_status;
    return valid && lstat(migration, &migration_status) == 0 &&
        S_ISREG(migration_status.st_mode) &&
        migration_status.st_uid == geteuid() &&
        (migration_status.st_mode & 0777) == 0400;
}

static char *resolve_artifact_directory(
    const char *store, const char *manifest_hex, const char *platform) {
    char objects[PATH_MAX];
    char manifest[PATH_MAX];
    char candidate[PATH_MAX];
    if (oci_snprintf(objects, sizeof(objects), "%s/objects", store) < 0 ||
        oci_snprintf(manifest, sizeof(manifest), "%s/%s", objects,
            manifest_hex) < 0 ||
        oci_snprintf(candidate, sizeof(candidate), "%s/%s", manifest,
            platform) < 0 ||
        !maelys_oci_store_private_directory(objects) ||
        !maelys_oci_store_private_directory(manifest) ||
        !maelys_oci_store_private_directory(candidate)) return NULL;
    char *canonical = realpath(candidate, NULL);
    if (!canonical || strcmp(canonical, candidate) != 0) {
        free(canonical);
        return NULL;
    }
    return canonical;
}

static maelys_oci_result_t validate_seal(
    const maelys_oci_artifact_t *artifact, const struct stat *root_status,
    const struct stat *rootfs_tar_status, char config_digest[OCI_DIGEST_SIZE],
    char **out_error) {
    maelys_oci_seal_t seal;
    if (maelys_oci_seal_parse(artifact->seal.path, &seal) != 0 ||
        strcmp(seal.manifest, artifact->image_digest) != 0 ||
        strcmp(seal.platform, artifact->platform) != 0 ||
        strcmp(seal.root + OCI_DIGEST_PREFIX_SIZE, artifact->root.digest) != 0 ||
        strcmp(seal.rootfs_tar + OCI_DIGEST_PREFIX_SIZE,
            artifact->rootfs_tar.digest) != 0) {
        maelys_oci_set_error(out_error,
            "OCI artifact seal is malformed or does not bind the requested target");
        return MAELYS_OCI_ERR_UNSUPPORTED;
    }
    if (seal.root_size != (uint64_t)root_status->st_size) {
        maelys_oci_set_error(
            out_error, "OCI artifact root size differs from its seal");
        return MAELYS_OCI_ERR_UNSUPPORTED;
    }
    if (seal.rootfs_tar_size != (uint64_t)rootfs_tar_status->st_size) {
        maelys_oci_set_error(
            out_error, "OCI portable root size differs from its seal");
        return MAELYS_OCI_ERR_UNSUPPORTED;
    }
    memcpy(config_digest, seal.config, OCI_DIGEST_SIZE);
    return MAELYS_OCI_OK;
}

static char *member_path(const char *directory, const char *name) {
    size_t size = strlen(directory) + strlen(name) + 2u;
    char *path = malloc(size);
    if (path) (void)oci_snprintf(path, size, "%s/%s", directory, name);
    return path;
}

/* Frees everything but the lease, which the caller has retired. */
static void destroy(maelys_oci_artifact_t *artifact) {
    free(artifact->store);
    free(artifact->directory);
    free(artifact->root.path);
    free(artifact->rootfs_tar.path);
    free(artifact->seal.path);
    free(artifact->lease_path);
    free(artifact);
}

/* Everything that happens under the ordered store locks. */
static maelys_oci_result_t resolve_locked(
    maelys_oci_artifact_t *artifact, const char *platform_directory,
    char **out_error) {
    artifact->directory = resolve_artifact_directory(artifact->store,
        artifact->image_digest + OCI_DIGEST_PREFIX_SIZE, platform_directory);
    if (!artifact->directory) {
        maelys_oci_set_error(out_error,
            "OCI artifact is not imported in the configured OCI store");
        return MAELYS_OCI_ERR_UNSUPPORTED;
    }
    artifact->root.path = member_path(artifact->directory, "root.ext4");
    artifact->rootfs_tar.path = member_path(artifact->directory, "rootfs.tar");
    artifact->seal.path = member_path(artifact->directory, "artifact.seal");
    if (!artifact->root.path || !artifact->rootfs_tar.path ||
        !artifact->seal.path)
        return MAELYS_OCI_ERR_MEMORY;
    struct stat root_status;
    struct stat rootfs_tar_status;
    struct stat seal_status;
    maelys_oci_result_t result =
        resolve_member(&artifact->root, &root_status, out_error);
    if (result == MAELYS_OCI_OK)
        result = resolve_member(
            &artifact->rootfs_tar, &rootfs_tar_status, out_error);
    if (result == MAELYS_OCI_OK)
        result = resolve_member(&artifact->seal, &seal_status, out_error);
    if (result == MAELYS_OCI_OK)
        result = validate_seal(artifact, &root_status, &rootfs_tar_status,
            artifact->config_digest, out_error);
    if (result == MAELYS_OCI_OK)
        result = create_lease(artifact, out_error);
    return result;
}

maelys_oci_result_t maelys_oci_artifact_resolve(
    const char *image_digest, const char *platform,
    maelys_oci_artifact_t **out_artifact, char **out_error) {
    if (!out_artifact) return MAELYS_OCI_ERR_ARGUMENT;
    *out_artifact = NULL;
    char platform_directory[OCI_PLATFORM_SIZE];
    if (!oci_digest_valid(image_digest) || !platform ||
        (strcmp(platform, "linux/arm64") != 0 &&
         strcmp(platform, "linux/amd64") != 0) ||
        oci_platform_to_directory(platform, platform_directory) != 0)
        return MAELYS_OCI_ERR_ARGUMENT;
    maelys_oci_artifact_t *artifact = calloc(1u, sizeof(*artifact));
    if (!artifact) return MAELYS_OCI_ERR_MEMORY;
    memcpy(artifact->image_digest, image_digest, OCI_DIGEST_SIZE);
    memcpy(artifact->platform, platform, strlen(platform) + 1u);
    char root[PATH_MAX];
    if (maelys_oci_store_default_path(root, sizeof(root)) != 0 ||
        maelys_oci_store_open_root(root, 0, &artifact->store) != 0 ||
        !store_v2_ready(artifact->store)) {
        destroy(artifact);
        maelys_oci_set_error(out_error,
            "OCI store is not initialized with its v2 layout and migration "
            "witness; re-import the image with this release");
        return MAELYS_OCI_ERR_UNSUPPORTED;
    }
    maelys_oci_store_lock_t store_lock = MAELYS_OCI_STORE_LOCK_INIT;
    maelys_oci_store_lock_t manifest_lock = MAELYS_OCI_STORE_LOCK_INIT;
    if (maelys_oci_store_lock_manifest(artifact->store,
            image_digest + OCI_DIGEST_PREFIX_SIZE, &store_lock,
            &manifest_lock) != 0) {
        destroy(artifact);
        maelys_oci_set_error(out_error, "cannot acquire OCI artifact locks");
        return MAELYS_OCI_ERR_IO;
    }
    maelys_oci_result_t result =
        resolve_locked(artifact, platform_directory, out_error);
    if (result != MAELYS_OCI_OK) retire_lease(artifact);
    maelys_oci_store_unlock_manifest(&store_lock, &manifest_lock);
    if (result != MAELYS_OCI_OK) {
        destroy(artifact);
        return result;
    }
    *out_artifact = artifact;
    return MAELYS_OCI_OK;
}

static maelys_oci_result_t revalidate_member(
    const member_t *member, const char *what, char **out_error) {
    struct stat status;
    char digest[MAELYS_OCI_DIGEST_HEX_SIZE];
    maelys_oci_result_t result =
        hash_member(member->path, &status, digest, out_error);
    if (result == MAELYS_OCI_OK &&
        (status.st_dev != member->device || status.st_ino != member->inode ||
         status.st_mode != member->mode || status.st_size != member->size ||
         strcmp(digest, member->digest) != 0)) {
        maelys_oci_set_error(out_error, "%s changed after preparation", what);
        result = MAELYS_OCI_ERR_UNSUPPORTED;
    }
    return result;
}

/* The lease is still ours when the lock is held, the locked descriptor
 * still meets every expectation and the path still names that file. */
static int lease_intact(const maelys_oci_artifact_t *artifact) {
    maelys_sys_file_identity_t locked;
    maelys_sys_file_identity_t at_path;
    maelys_sys_file_expectations_t expectations;
    maelys_oci_store_lease_expectations(&expectations);
    return artifact->lease_lock && artifact->lease_path &&
        maelys_sys_file_verify(maelys_sys_file_lock_fd(artifact->lease_lock),
            &expectations, &locked, NULL) == MAELYS_SYS_OK &&
        maelys_sys_file_identity_same(&locked, &artifact->lease_identity) &&
        maelys_sys_file_path_identity(artifact->lease_path, &at_path) ==
            MAELYS_SYS_OK &&
        maelys_sys_file_identity_same(&at_path, &artifact->lease_identity);
}

maelys_oci_result_t maelys_oci_artifact_revalidate(
    const maelys_oci_artifact_t *artifact, char **out_error) {
    if (!artifact) return MAELYS_OCI_ERR_ARGUMENT;
    maelys_oci_result_t result =
        revalidate_member(&artifact->root, "OCI root identity", out_error);
    if (result == MAELYS_OCI_OK)
        result = revalidate_member(&artifact->rootfs_tar,
            "OCI portable root identity", out_error);
    if (result == MAELYS_OCI_OK)
        result = revalidate_member(&artifact->seal,
            "OCI artifact seal identity", out_error);
    if (result == MAELYS_OCI_OK && !lease_intact(artifact)) {
        maelys_oci_set_error(
            out_error, "OCI execution lease changed after preparation");
        result = MAELYS_OCI_ERR_UNSUPPORTED;
    }
    return result;
}

void maelys_oci_artifact_release(maelys_oci_artifact_t **artifact) {
    if (!artifact || !*artifact) return;
    maelys_oci_artifact_t *handle = *artifact;
    *artifact = NULL;
    if (handle->lease_lock) {
        /* The same order as resolution and gc, so the retirement never
         * interleaves with a scan of the lease namespace. */
        maelys_oci_store_lock_t store_lock = MAELYS_OCI_STORE_LOCK_INIT;
        maelys_oci_store_lock_t manifest_lock = MAELYS_OCI_STORE_LOCK_INIT;
        int ordered = maelys_oci_store_lock_manifest(handle->store,
            handle->image_digest + OCI_DIGEST_PREFIX_SIZE, &store_lock,
            &manifest_lock) == 0;
        retire_lease(handle);
        if (ordered) maelys_oci_store_unlock_manifest(&store_lock, &manifest_lock);
    }
    destroy(handle);
}

const char *maelys_oci_artifact_image_digest(
    const maelys_oci_artifact_t *artifact) {
    return artifact ? artifact->image_digest : NULL;
}

const char *maelys_oci_artifact_platform(const maelys_oci_artifact_t *artifact) {
    return artifact ? artifact->platform : NULL;
}

const char *maelys_oci_artifact_root_path(const maelys_oci_artifact_t *artifact) {
    return artifact ? artifact->root.path : NULL;
}

const char *maelys_oci_artifact_rootfs_tar_path(
    const maelys_oci_artifact_t *artifact) {
    return artifact ? artifact->rootfs_tar.path : NULL;
}

const char *maelys_oci_artifact_root_digest(
    const maelys_oci_artifact_t *artifact) {
    return artifact ? artifact->root.digest : NULL;
}

const char *maelys_oci_artifact_rootfs_tar_digest(
    const maelys_oci_artifact_t *artifact) {
    return artifact ? artifact->rootfs_tar.digest : NULL;
}

const char *maelys_oci_artifact_config_digest(
    const maelys_oci_artifact_t *artifact) {
    return artifact ? artifact->config_digest : NULL;
}

const char *maelys_oci_artifact_lease_path(const maelys_oci_artifact_t *artifact) {
    return artifact ? artifact->lease_path : NULL;
}

int maelys_oci_artifact_root_identity(
    const maelys_oci_artifact_t *artifact,
    maelys_oci_file_identity_t *out_identity) {
    if (!artifact || !out_identity) return -1;
    out_identity->device = artifact->root.device;
    out_identity->inode = artifact->root.inode;
    out_identity->mode = artifact->root.mode;
    out_identity->size = (uint64_t)artifact->root.size;
    return 0;
}
