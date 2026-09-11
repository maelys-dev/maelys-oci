/* SPDX-License-Identifier: MPL-2.0 */
/*
 * Garbage collection and artifact removal. Both are plan/apply transactions:
 * the plan computes the exact candidates under the store lock and the apply
 * revalidates every candidate by identity before retiring it.
 */
#include "src/materializer/internal.h"

#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Every retirement removes by identity: the name is re-resolved by the
 * kernel at the last moment and only the object seen at planning goes. */
static int retire_blob(
    const char *store, const char *digest, uint64_t expected_size) {
    char source[PATH_MAX];
    char blobs[PATH_MAX];
    char removal[PATH_MAX];
    char destination[PATH_MAX];
    if (oci_snprintf(source, sizeof(source), "%s/blobs/sha256/%s", store,
            digest + OCI_DIGEST_PREFIX_SIZE) < 0 ||
        oci_snprintf(blobs, sizeof(blobs), "%s/blobs/sha256", store) < 0 ||
        oci_snprintf(removal, sizeof(removal), "%s/tmp/removal", store) < 0 ||
        oci_snprintf(destination, sizeof(destination), "%s/blob.XXXXXX",
            removal) < 0)
        return -1;
    char actual[OCI_DIGEST_SIZE];
    uint64_t size = 0u;
    maelys_sys_file_identity_t blob;
    if (safe_regular_digest(source, 0400, actual, &size) != 0 ||
        strcmp(actual, digest) != 0 || size != expected_size ||
        maelys_sys_file_path_identity(source, &blob) != MAELYS_SYS_OK)
        return -1;
    /* A placeholder reserves the removal name, then makes way. */
    maelys_sys_file_identity_t placeholder;
    int reserved = mkstemp(destination);
    if (reserved < 0 ||
        maelys_sys_file_verify(reserved, NULL, &placeholder, NULL) !=
            MAELYS_SYS_OK ||
        maelys_sys_fd_close(&reserved) != MAELYS_SYS_OK ||
        maelys_sys_file_unlink_same(destination, &placeholder) != MAELYS_SYS_OK)
        return -1;
    /* A blob is a regular file: the file variant, which checks that. */
    int moved = maelys_oci_store_publish_file_noreplace(source, destination);
    if (moved != 0 || fsync_directory(blobs) != 0 ||
        fsync_directory(removal) != 0) return -1;
    if (maelys_sys_file_unlink_same(destination, &blob) != MAELYS_SYS_OK ||
        fsync_directory(removal) != 0) return -1;
    return 0;
}

/* The plan saw nobody holding the lease under the exclusive store lock
 * that is still held; the kernel is asked once more before the file goes. */
static int retire_lease(
    const char *store, const char *relative, dev_t expected_device,
    ino_t expected_inode) {
    if (!relative || strncmp(relative, "leases/", 7u) != 0 ||
        strchr(relative + 7u, '/')) return -1;
    char path[PATH_MAX];
    char leases[PATH_MAX];
    if (oci_snprintf(path, sizeof(path), "%s/%s", store, relative) < 0 ||
        oci_snprintf(leases, sizeof(leases), "%s/leases", store) < 0) return -1;
    maelys_sys_file_identity_t expected = {0};
    expected.device = expected_device;
    expected.inode = expected_inode;
    maelys_sys_file_lock_t *lock = NULL;
    if (maelys_oci_store_lease_probe(path, &lock) != 0) return -1;
    int removed = maelys_sys_file_unlink_same(path, &expected) == MAELYS_SYS_OK;
    (void)maelys_sys_file_lock_release(&lock);
    return removed && fsync_directory(leases) == 0 ? 0 : -1;
}

static int retire_temporary(
    const char *store, const char *relative, dev_t expected_device,
    ino_t expected_inode) {
    if (!relative ||
        (strncmp(relative, "tmp/import/", 11u) != 0 &&
         strncmp(relative, "tmp/removal/", 12u) != 0) ||
        strchr(relative + (relative[4] == 'i' ? 11u : 12u), '/')) return -1;
    char source[PATH_MAX];
    char import_path[PATH_MAX];
    char removal_path[PATH_MAX];
    if (oci_snprintf(source, sizeof(source), "%s/%s", store, relative) < 0 ||
        oci_snprintf(import_path, sizeof(import_path), "%s/tmp/import",
            store) < 0 ||
        oci_snprintf(removal_path, sizeof(removal_path), "%s/tmp/removal",
            store) < 0)
        return -1;
    struct stat status;
    if (lstat(source, &status) != 0 || S_ISLNK(status.st_mode) ||
        status.st_uid != geteuid() || status.st_dev != expected_device ||
        status.st_ino != expected_inode) return -1;
    maelys_sys_file_identity_t expected = {0};
    expected.device = expected_device;
    expected.inode = expected_inode;
    char retired[PATH_MAX];
    const char *target = source;
    if (strncmp(relative, "tmp/import/", 11u) == 0) {
        /* The reservation is a directory of ours: it makes way by identity
         * like everything else here. */
        maelys_sys_file_identity_t reservation;
        if (oci_snprintf(retired, sizeof(retired), "%s/recovery.XXXXXX",
                removal_path) < 0 || !mkdtemp(retired) ||
            maelys_sys_file_path_identity(retired, &reservation) !=
                MAELYS_SYS_OK ||
            maelys_sys_directory_rmdir_same(retired, &reservation) !=
                MAELYS_SYS_OK ||
            (S_ISREG(status.st_mode)
                ? maelys_oci_store_publish_file_noreplace(source, retired)
                : maelys_oci_store_publish_directory_noreplace(source, retired)) != 0 ||
            fsync_directory(import_path) != 0 ||
            fsync_directory(removal_path) != 0) return -1;
        target = retired;
        struct stat moved;
        if (lstat(target, &moved) != 0 || moved.st_dev != expected_device ||
            moved.st_ino != expected_inode) return -1;
    }
    int removed = S_ISDIR(status.st_mode)
        ? remove_filesystem_tree_contents(target) == 0 &&
            maelys_sys_directory_rmdir_same(target, &expected) == MAELYS_SYS_OK
        : maelys_sys_file_unlink_same(target, &expected) == MAELYS_SYS_OK;
    return removed && fsync_directory(removal_path) == 0 ? 0 : -1;
}

static int retire_candidate(
    const char *store, oci_document_t *candidate, const char *kind, uint64_t size) {
    const char *digest = oci_document_string_value(oci_document_get(candidate, "digest"));
    const char *path = oci_document_string_value(oci_document_get(candidate, "path"));
    oci_document_t *device = oci_document_get(candidate, "device");
    oci_document_t *inode = oci_document_get(candidate, "inode");
    if (strcmp(kind, "blob") == 0 && digest)
        return retire_blob(store, digest, size);
    if (!path || !oci_document_is_integer(device) || !oci_document_is_integer(inode)) return -1;
    if (strcmp(kind, "lease") == 0)
        return retire_lease(store, path, (dev_t)oci_document_integer_value(device),
            (ino_t)oci_document_integer_value(inode));
    if (strcmp(kind, "temporary") == 0)
        return retire_temporary(store, path, (dev_t)oci_document_integer_value(device),
            (ino_t)oci_document_integer_value(inode));
    return -1;
}

int oci_store_gc(
    const char *store_argument, int apply, uint64_t grace_seconds,
    oci_document_t **out_document, int *out_valid, oci_error_t *error) {
    *out_document = NULL;
    *out_valid = 0;
    char *store = NULL;
    if (maelys_oci_store_open_root(store_argument, 0, &store) != 0) {
        oci_error_report(error, OCI_ERROR_STATE,
            "store %s must be an existing absolute private directory owned "
            "by the caller", store_argument ? store_argument : "(null)");
        return -1;
    }
    store_report_t report;
    digest_set_t reachable = {0};
    oci_document_t *candidates = oci_document_array();
    if (!candidates || report_initialize(&report) != 0) {
        oci_document_release(candidates);
        free(store);
        oci_error_report(error, OCI_ERROR_MEMORY, "out of memory");
        return -1;
    }
    maelys_oci_store_lock_t lock = MAELYS_OCI_STORE_LOCK_INIT;
    int inspected = maelys_oci_store_lock_acquire(
        store, "store.lock", 1, &lock) == 0 &&
        inspect_store_locked(store, 1, grace_seconds, &report, &reachable, candidates) == 0 &&
        collect_stale_leases(store, grace_seconds, &report, candidates) == 0 &&
        scan_temporary_state(store, 1, grace_seconds, 0, &report,
            candidates) == 0;
    int valid = inspected && oci_document_array_size(report.errors) == 0u;
    uint64_t selected_bytes = 0u;
    size_t retired_count = 0u;
    for (size_t i = 0u; valid && i < oci_document_array_size(candidates); ++i) {
        oci_document_t *candidate = oci_document_at(candidates, i);
        const char *kind = oci_document_string_value(oci_document_get(candidate, "kind"));
        oci_document_t *bytes = oci_document_get(candidate, "bytes");
        uint64_t size = oci_document_is_integer(bytes)
            ? (uint64_t)oci_document_integer_value(bytes) : 0u;
        if (!kind || UINT64_MAX - selected_bytes < size ||
            selected_bytes + size > (uint64_t)INT64_MAX) {
            (void)report_message(report.errors,
                "garbage-collection byte accounting exceeds its canonical range");
            valid = 0;
            break;
        }
        selected_bytes += size;
        if (!apply) continue;
        if (retire_candidate(store, candidate, kind, size) != 0) {
            (void)report_message(report.errors,
                "failed to retire revalidated %s candidate", kind);
            valid = 0;
            break;
        }
        ++retired_count;
    }
    maelys_oci_store_lock_release(&lock);
    if (!inspected && oci_document_array_size(report.errors) == 0u)
        (void)report_message(report.errors, "cannot inspect the OCI store");
    oci_document_t *document = store_report_json(store, &report);
    if (document &&
        (oci_document_put(document, "mode",
            oci_document_string(apply ? "apply" : "plan")) != 0 ||
         oci_document_put(document, "changed",
            oci_document_boolean(apply && retired_count > 0u)) != 0 ||
         oci_document_put(document, "valid", oci_document_boolean(valid)) != 0 ||
         oci_document_put(document, "graceSeconds",
            oci_document_integer((int64_t)grace_seconds)) != 0 ||
         oci_document_set(document, "candidates", candidates) != 0 ||
         oci_document_put(document, "selectedBytes",
            oci_document_integer((int64_t)selected_bytes)) != 0 ||
         oci_document_put(document, "retired",
            oci_document_integer((int64_t)retired_count)) != 0)) {
        oci_document_release(document);
        document = NULL;
    }
    digest_set_clear(&reachable);
    report_clear(&report);
    oci_document_release(candidates);
    free(store);
    if (!document) {
        oci_error_report(error, OCI_ERROR_MEMORY,
            "cannot build the garbage-collection report");
        return -1;
    }
    *out_document = document;
    *out_valid = valid;
    return 0;
}

int store_target_has_lease(
    const char *store, const char *manifest, const char *platform,
    int *out_has_lease) {
    char leases_path[PATH_MAX];
    if (!out_has_lease ||
        oci_snprintf(leases_path, sizeof(leases_path), "%s/leases", store) < 0 ||
        !private_directory(leases_path)) return -1;
    *out_has_lease = 0;
    DIR *leases = opendir(leases_path);
    if (!leases) return -1;
    int result = 0;
    struct dirent *entry;
    while (result == 0 && (entry = readdir(leases)) != NULL) {
        if (oci_directory_entry_is_dot(entry->d_name)) continue;
        char path[PATH_MAX];
        if (!lease_name_valid(entry->d_name) ||
            oci_snprintf(path, sizeof(path), "%s/%s", leases_path,
                entry->d_name) < 0) {
            result = -1;
            break;
        }
        oci_document_t *lease = load_canonical_json_file(path, OCI_LEASE_MAX_BYTES, 1);
        lease_view_t view;
        if (!lease_document_valid(lease, &view)) {
            oci_document_release(lease);
            result = -1;
            break;
        }
        if (strcmp(view.manifest, manifest) == 0 &&
            strcmp(view.platform, platform) == 0) *out_has_lease = 1;
        oci_document_release(lease);
    }
    if (closedir(leases) != 0) result = -1;
    return result;
}

/* Finds the single platform directory of a manifest when none was given. */
static int single_platform_directory(
    const char *manifest_directory, char out[OCI_PLATFORM_SIZE]) {
    DIR *directory = opendir(manifest_directory);
    if (!directory) return -1;
    size_t count = 0u;
    int result = 0;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        if (oci_directory_entry_is_dot(entry->d_name)) continue;
        if (!oci_directory_entry_name_valid(entry->d_name) || ++count != 1u ||
            strlen(entry->d_name) >= OCI_PLATFORM_SIZE) {
            result = -1;
            break;
        }
        memcpy(out, entry->d_name, strlen(entry->d_name) + 1u);
    }
    if (closedir(directory) != 0 || count != 1u) result = -1;
    return result;
}

static oci_document_t *removal_document(
    int apply, int changed, const char *digest, const char *platform,
    const char *store, const char *artifact_directory) {
    char reference[96];
    if (oci_snprintf(reference, sizeof(reference), "oci@%s", digest) < 0)
        return NULL;
    return OCI_DOCUMENT_OBJECT(
            {"mode", oci_document_string(apply ? "apply" : "plan")},
            {"changed", oci_document_boolean(changed)},
            {"reference", oci_document_string(reference)},
            {"digest", oci_document_string(digest)},
            {"platform", oci_document_string(platform)},
            {"store", oci_document_string(store)},
            {"artifact", oci_document_string(artifact_directory)});
}

/* Retires the published artifact and its source closure through the
 * removal namespace so a crash never leaves a half-removed object. */
static int retire_artifact(
    const char *store, const char *digest, const char *platform_directory,
    const char *manifest_directory, const char *artifact_directory) {
    char removal_root[PATH_MAX];
    char removal[PATH_MAX];
    char retired_artifact[PATH_MAX];
    if (oci_snprintf(removal_root, sizeof(removal_root), "%s/tmp/removal",
            store) < 0 ||
        oci_snprintf(removal, sizeof(removal), "%s/remove.XXXXXX",
            removal_root) < 0 ||
        !mkdtemp(removal) || chmod(removal, 0700) != 0 ||
        oci_snprintf(retired_artifact, sizeof(retired_artifact),
            "%s/artifact", removal) < 0 ||
        maelys_oci_store_publish_directory_noreplace(
            artifact_directory, retired_artifact) != 0 ||
        fsync_directory(manifest_directory) != 0) return -1;
    char source_directory[PATH_MAX];
    char source_manifest_directory[PATH_MAX];
    char retired_source[PATH_MAX];
    if (oci_snprintf(source_manifest_directory,
            sizeof(source_manifest_directory), "%s/sources/%s", store,
            digest + OCI_DIGEST_PREFIX_SIZE) < 0 ||
        oci_snprintf(source_directory, sizeof(source_directory), "%s/%s",
            source_manifest_directory, platform_directory) < 0 ||
        oci_snprintf(retired_source, sizeof(retired_source), "%s/source",
            removal) < 0)
        return -1;
    struct stat source_status;
    if (lstat(source_directory, &source_status) == 0) {
        if (!private_directory(source_directory) ||
            maelys_oci_store_publish_directory_noreplace(
                source_directory, retired_source) != 0 ||
            fsync_directory(source_manifest_directory) != 0) return -1;
    } else if (errno != ENOENT) {
        return -1;
    }
    if (fsync_directory(removal_root) != 0 ||
        remove_filesystem_tree(removal) != 0 ||
        fsync_directory(removal_root) != 0) return -1;
    (void)rmdir(manifest_directory);
    (void)rmdir(source_manifest_directory);
    char objects_path[PATH_MAX];
    char sources_path[PATH_MAX];
    if (oci_snprintf(objects_path, sizeof(objects_path), "%s/objects",
            store) < 0 ||
        oci_snprintf(sources_path, sizeof(sources_path), "%s/sources",
            store) < 0 ||
        fsync_directory(objects_path) != 0 ||
        fsync_directory(sources_path) != 0) return -1;
    return 0;
}

int oci_store_remove(
    const char *store_argument, const char *reference,
    const char *platform_argument, int apply, oci_document_t **out_document,
    oci_error_t *error) {
    *out_document = NULL;
    char digest[OCI_DIGEST_SIZE];
    if (oci_reference_digest(reference, digest) != 0) {
        oci_error_report(error, OCI_ERROR_ARGUMENT,
            "REFERENCE must end with an immutable sha256 digest");
        return -1;
    }
    char platform_directory[OCI_PLATFORM_SIZE] = {0};
    if (platform_argument &&
        oci_platform_to_directory(platform_argument, platform_directory) != 0) {
        oci_error_report(error, OCI_ERROR_ARGUMENT,
            "--platform must be OS/ARCH with lowercase components");
        return -1;
    }
    char *store = NULL;
    if (store_path_open(store_argument, &store, 0) != 0 ||
        (apply && store_prepare_v2(store) != 0)) {
        free(store);
        oci_error_report(error, OCI_ERROR_STATE,
            "store %s must be an existing absolute private directory owned "
            "by the caller with a valid v2 layout",
            store_argument ? store_argument : "(null)");
        return -1;
    }
    maelys_oci_store_lock_t store_lock = MAELYS_OCI_STORE_LOCK_INIT;
    maelys_oci_store_lock_t manifest_lock = MAELYS_OCI_STORE_LOCK_INIT;
    if (maelys_oci_store_lock_manifest(store, digest + OCI_DIGEST_PREFIX_SIZE,
            &store_lock, &manifest_lock) != 0) {
        free(store);
        oci_error_report(error, OCI_ERROR_IO,
            "cannot acquire the ordered store locks");
        return -1;
    }
    int result = -1;
    char manifest_directory[PATH_MAX];
    char artifact_directory[PATH_MAX];
    char metadata_path[PATH_MAX];
    char platform[OCI_PLATFORM_SIZE] = {0};
    if (oci_snprintf(manifest_directory, sizeof(manifest_directory),
            "%s/objects/%s", store, digest + OCI_DIGEST_PREFIX_SIZE) < 0 ||
        !private_directory(manifest_directory)) {
        oci_error_report(error, OCI_ERROR_NOT_FOUND,
            "%s is not imported in %s", digest, store);
        goto done;
    }
    if (!platform_argument &&
        single_platform_directory(manifest_directory, platform_directory) != 0) {
        oci_error_report(error, OCI_ERROR_STATE,
            "%s has several platforms; select one with --platform", digest);
        goto done;
    }
    if (oci_snprintf(artifact_directory, sizeof(artifact_directory), "%s/%s",
            manifest_directory, platform_directory) < 0 ||
        oci_snprintf(metadata_path, sizeof(metadata_path), "%s/artifact.json",
            artifact_directory) < 0 ||
        !private_directory(artifact_directory)) {
        oci_error_report(error, OCI_ERROR_NOT_FOUND,
            "%s has no artifact for platform directory %s", digest,
            platform_directory);
        goto done;
    }
    oci_document_t *metadata = load_artifact_metadata(
        metadata_path, digest, platform_argument);
    if (!metadata) {
        const char *former = NULL;
        oci_document_t *document =
            load_former_artifact_schema(metadata_path, &former);
        if (former) {
            oci_error_report(error, OCI_ERROR_STATE,
                "artifact %s/%s carries the former schema %s, which this "
                "release does not migrate: recreate the store and reimport "
                "its sources", digest, platform_directory, former);
            oci_document_release(document);
            goto done;
        }
    }
    const char *metadata_platform = metadata
        ? oci_document_string_value(oci_document_get(metadata, "platform")) : NULL;
    char metadata_platform_directory[OCI_PLATFORM_SIZE];
    if (!metadata || !metadata_platform ||
        oci_platform_to_directory(metadata_platform,
            metadata_platform_directory) != 0 ||
        strcmp(metadata_platform_directory, platform_directory) != 0 ||
        strlen(metadata_platform) >= sizeof(platform)) {
        oci_document_release(metadata);
        oci_error_report(error, OCI_ERROR_STATE,
            "artifact %s/%s has unsafe or inconsistent metadata", digest,
            platform_directory);
        goto done;
    }
    memcpy(platform, metadata_platform, strlen(metadata_platform) + 1u);
    oci_document_release(metadata);
    int has_lease = 0;
    if (store_target_has_lease(store, digest, platform, &has_lease) != 0) {
        oci_error_report(error, OCI_ERROR_STATE,
            "cannot inspect the lease namespace of %s", store);
        goto done;
    }
    if (has_lease) {
        oci_error_report(error, OCI_ERROR_STATE,
            "%s %s is held by a live execution lease", digest, platform);
        goto done;
    }
    if (apply && retire_artifact(store, digest, platform_directory,
            manifest_directory, artifact_directory) != 0) {
        oci_error_report(error, OCI_ERROR_IO,
            "removal of %s %s failed closed; run gc to recover the removal "
            "namespace", digest, platform);
        goto done;
    }
    *out_document = removal_document(
        apply, apply, digest, platform, store, artifact_directory);
    if (!*out_document)
        oci_error_report(error, OCI_ERROR_MEMORY, "out of memory");
    else
        result = 0;
done:
    maelys_oci_store_unlock_manifest(&store_lock, &manifest_lock);
    free(store);
    return result;
}
