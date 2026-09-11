/* SPDX-License-Identifier: MPL-2.0 */
/*
 * Store v2 preparation and migration witness, canonical file publication
 * and the artifact listing.
 */
#include "src/materializer/internal.h"
#include <maelys/sys/clock.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct legacy_artifact {
    char manifest_digest[72];
    char platform[72];
    char root_digest[72];
    char seal_digest[72];
} legacy_artifact_t;

int safe_regular_digest(
    const char *path, mode_t expected_permissions,
    char output[OCI_DIGEST_SIZE], uint64_t *out_size) {
    char hex[OCI_DIGEST_HEX_SIZE];
    struct stat status;
    if (maelys_oci_store_hash_immutable(path, expected_permissions, hex,
            &status) != 0 ||
        oci_snprintf(output, OCI_DIGEST_SIZE, OCI_DIGEST_PREFIX "%s", hex) < 0)
        return -1;
    if (out_size) *out_size = (uint64_t)status.st_size;
    return 0;
}

static int normalize_legacy_read_only_member(const char *path) {
    int descriptor = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    struct stat before;
    if (descriptor < 0 || fstat(descriptor, &before) != 0 ||
        !S_ISREG(before.st_mode) || before.st_uid != geteuid() ||
        before.st_nlink != 1 ||
        ((before.st_mode & 0777) != 0400 &&
         (before.st_mode & 0777) != 0600)) {
        if (descriptor >= 0) (void)maelys_sys_fd_close(&descriptor);
        return -1;
    }
    int result = 0;
    if ((before.st_mode & 0777) == 0600 &&
        (fchmod(descriptor, 0400) != 0 || fsync(descriptor) != 0)) result = -1;
    struct stat after;
    if (result == 0 &&
        (fstat(descriptor, &after) != 0 ||
         after.st_dev != before.st_dev || after.st_ino != before.st_ino ||
         !S_ISREG(after.st_mode) || after.st_uid != geteuid() ||
         (after.st_mode & 0777) != 0400 || after.st_nlink != 1)) result = -1;
    if (close(descriptor) != 0) result = -1;
    return result;
}

static int legacy_compare(const void *left, const void *right) {
    const legacy_artifact_t *a = left;
    const legacy_artifact_t *b = right;
    int digest = strcmp(a->manifest_digest, b->manifest_digest);
    return digest ? digest : strcmp(a->platform, b->platform);
}

static int collect_legacy_artifacts(
    const char *store, legacy_artifact_t **out_items, size_t *out_count) {
    *out_items = NULL;
    *out_count = 0u;
    char objects_path[PATH_MAX];
    if (oci_snprintf(objects_path, sizeof(objects_path), "%s/objects", store) <= 0)
        return -1;
    DIR *objects = opendir(objects_path);
    if (!objects) return errno == ENOENT ? 0 : -1;
    if (!private_directory(objects_path)) {
        (void)closedir(objects);
        return -1;
    }
    legacy_artifact_t *items = NULL;
    size_t count = 0u;
    size_t capacity = 0u;
    int result = 0;
    struct dirent *digest_entry;
    while (result == 0 && (digest_entry = readdir(objects)) != NULL) {
        if (oci_directory_entry_is_dot(digest_entry->d_name)) continue;
        if (!oci_digest_hex_valid(digest_entry->d_name)) {
            result = -1;
            break;
        }
        char digest_path[PATH_MAX];
        if (oci_snprintf(digest_path, sizeof(digest_path), "%s/%s", objects_path,
                digest_entry->d_name) <= 0 || !private_directory(digest_path)) {
            result = -1;
            break;
        }
        DIR *platforms = opendir(digest_path);
        if (!platforms) {
            result = -1;
            break;
        }
        struct dirent *platform_entry;
        while (result == 0 &&
               (platform_entry = readdir(platforms)) != NULL) {
            if (oci_directory_entry_is_dot(platform_entry->d_name)) continue;
            if (!oci_directory_entry_name_valid(platform_entry->d_name)) {
                result = -1;
                break;
            }
            char artifact[PATH_MAX];
            char metadata_path[PATH_MAX];
            char root_path[PATH_MAX];
            char seal_path[PATH_MAX];
            char manifest[72];
            if (oci_snprintf(artifact, sizeof(artifact), "%s/%s", digest_path,
                    platform_entry->d_name) <= 0 ||
                !private_directory(artifact) ||
                oci_snprintf(metadata_path, sizeof(metadata_path), "%s/artifact.json",
                    artifact) <= 0 ||
                oci_snprintf(root_path, sizeof(root_path), "%s/root.ext4", artifact) <= 0 ||
                oci_snprintf(seal_path, sizeof(seal_path), "%s/artifact.seal", artifact) <= 0 ||
                digest_from_directory_name(digest_entry->d_name, manifest) != 0) {
                result = -1;
                break;
            }
            if (normalize_legacy_read_only_member(metadata_path) != 0 ||
                normalize_legacy_read_only_member(seal_path) != 0) {
                result = -1;
                break;
            }
            oci_document_t *metadata = load_artifact_metadata(
                metadata_path, manifest, NULL);
            const char *platform = metadata
                ? oci_document_string_value(oci_document_get(metadata, "platform")) : NULL;
            const char *recorded_root = metadata
                ? oci_document_string_value(oci_document_get(metadata, "rootDigest")) : NULL;
            char platform_directory[72];
            char root_digest[72];
            char seal_digest[72];
            if (!metadata || !platform || !recorded_root ||
                oci_platform_to_directory(platform, platform_directory) != 0 ||
                strcmp(platform_directory, platform_entry->d_name) != 0 ||
                safe_regular_digest(root_path, 0400, root_digest, NULL) != 0 ||
                safe_regular_digest(seal_path, 0400, seal_digest, NULL) != 0 ||
                strcmp(recorded_root, root_digest) != 0) {
                oci_document_release(metadata);
                result = -1;
                break;
            }
            if (count == capacity) {
                size_t grown_capacity = capacity ? capacity * 2u : 8u;
                if (grown_capacity < capacity ||
                    grown_capacity > SIZE_MAX / sizeof(*items)) {
                    oci_document_release(metadata);
                    result = -1;
                    break;
                }
                legacy_artifact_t *grown = realloc(
                    items, grown_capacity * sizeof(*items));
                if (!grown) {
                    oci_document_release(metadata);
                    result = -1;
                    break;
                }
                items = grown;
                capacity = grown_capacity;
            }
            legacy_artifact_t *item = &items[count++];
            memcpy(item->manifest_digest, manifest, sizeof(manifest));
            memcpy(item->platform, platform, strlen(platform) + 1u);
            memcpy(item->root_digest, root_digest, sizeof(root_digest));
            memcpy(item->seal_digest, seal_digest, sizeof(seal_digest));
            oci_document_release(metadata);
        }
        if (closedir(platforms) != 0) result = -1;
    }
    if (closedir(objects) != 0) result = -1;
    if (result != 0) {
        free(items);
        return -1;
    }
    if (count > 1u) qsort(items, count, sizeof(*items), legacy_compare);
    *out_items = items;
    *out_count = count;
    return 0;
}

int write_json_file(const char *path, oci_document_t *document) {
    int descriptor = open(path,
        O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (descriptor < 0) return -1;
    FILE *stream = fdopen(descriptor, "w");
    if (!stream) {
        (void)maelys_sys_fd_close(&descriptor);
        (void)unlink(path);
        return -1;
    }
    int result = oci_document_dump_file(document, stream) == 0 &&
        fputc('\n', stream) != EOF && fflush(stream) == 0 &&
        fchmod(descriptor, 0400) == 0 && fsync(descriptor) == 0 ? 0 : -1;
    if (fclose(stream) != 0) result = -1;
    if (result != 0) (void)unlink(path);
    return result;
}

int files_equal(const char *left, const char *right) {
    return oci_store_files_equal(left, right);
}

static int publish_canonical_file(
    const char *staging, const char *destination, const char *parent) {
    int publication = maelys_oci_store_publish_file_noreplace(
        staging, destination);
    if (publication == 0)
        return fsync_directory(parent);
    if (publication == 1 && files_equal(staging, destination)) {
        (void)unlink(staging);
        return 0;
    }
    return -1;
}

static int build_migration_json(
    const char *store, const char *path) {
    legacy_artifact_t *items = NULL;
    size_t count = 0u;
    if (collect_legacy_artifacts(store, &items, &count) != 0) return -1;
    oci_document_t *array = oci_document_array();
    oci_document_t *root = oci_document_object();
    int result = array && root ? 0 : -1;
    for (size_t i = 0u; result == 0 && i < count; ++i) {
        oci_document_t *item = OCI_DOCUMENT_OBJECT(
            {"manifestDigest", oci_document_string(items[i].manifest_digest)},
            {"platform", oci_document_string(items[i].platform)},
            {"rootDigest", oci_document_string(items[i].root_digest)},
            {"sealDigest", oci_document_string(items[i].seal_digest)});
        if (!item || oci_document_append(array, item) != 0) {
            result = -1;
        }
    }
    if (result == 0) {
        if (oci_document_set(root, "legacyDerived", array) != 0) result = -1;
    }
    if (result == 0 && oci_document_put(root, "schema",
            oci_document_string(OCI_SCHEMA_MIGRATION)) != 0)
        result = -1;
    oci_document_release(array);
    if (result == 0) result = write_json_file(path, root);
    oci_document_release(root);
    free(items);
    return result;
}

int exact_file(const char *path, const char *bytes, size_t size) {
    int descriptor = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    struct stat status;
    if (descriptor < 0 || fstat(descriptor, &status) != 0 ||
        !S_ISREG(status.st_mode) || status.st_uid != geteuid() ||
        (status.st_mode & 0777) != 0400 || status.st_size != (off_t)size) {
        if (descriptor >= 0) (void)maelys_sys_fd_close(&descriptor);
        return 0;
    }
    char buffer[32];
    ssize_t amount;
    do amount = read(descriptor, buffer, sizeof(buffer));
    while (amount < 0 && errno == EINTR);
    (void)maelys_sys_fd_close(&descriptor);
    return amount == (ssize_t)size && memcmp(buffer, bytes, size) == 0;
}

int migration_document_valid(oci_document_t *migration) {
    const char *schema = migration
        ? oci_document_string_value(oci_document_get(migration, "schema")) : NULL;
    oci_document_t *legacy = migration
        ? oci_document_get(migration, "legacyDerived") : NULL;
    if (!oci_document_is_object(migration) || oci_document_object_size(migration) != 2u ||
        !schema || strcmp(schema, OCI_SCHEMA_MIGRATION) != 0 ||
        !oci_document_is_array(legacy)) return 0;
    for (size_t i = 0u; i < oci_document_array_size(legacy); ++i) {
        oci_document_t *item = oci_document_at(legacy, i);
        const char *manifest =
            oci_document_string_value(oci_document_get(item, "manifestDigest"));
        const char *platform =
            oci_document_string_value(oci_document_get(item, "platform"));
        const char *root =
            oci_document_string_value(oci_document_get(item, "rootDigest"));
        const char *seal =
            oci_document_string_value(oci_document_get(item, "sealDigest"));
        if (!oci_document_is_object(item) || oci_document_object_size(item) != 4u ||
            !oci_digest_valid(manifest) || !platform ||
            oci_platform_to_directory(platform, (char[72]){0}) != 0 ||
            !oci_digest_valid(root) || !oci_digest_valid(seal)) return 0;
    }
    return 1;
}

int store_prepare_v2(const char *store) {
    uint64_t deadline;
    if (maelys_sys_deadline_after(30000u, &deadline) != MAELYS_SYS_OK) return -1;
    return store_prepare_v2_until(store, deadline);
}

int store_prepare_v2_until(const char *store, uint64_t deadline) {
    char locks[PATH_MAX];
    if (ensure_private_child(store, "locks", locks) != 0) return -1;
    maelys_oci_store_lock_t lock = MAELYS_OCI_STORE_LOCK_INIT;
    /* The common case only needs the shared lock. Initialization and
     * migration take the exclusive lock after releasing it, in that order. */
    if (maelys_oci_store_lock_until(store, "store.lock", 0, deadline, &lock) != 0) return -1;
    char current[PATH_MAX];
    struct stat status;
    if (oci_snprintf(current, sizeof(current), "%s/store.version", store) < 0) {
        maelys_oci_store_lock_release(&lock); return -1;
    }
    int initialized = lstat(current, &status) == 0;
    if (!initialized) {
        maelys_oci_store_lock_release(&lock);
        if (maelys_oci_store_lock_until(store, "store.lock", 1, deadline, &lock) != 0) return -1;
    }
    static const char *const directories[] = {
        "objects", "blobs", "sources", "leases", "tmp"
    };
    int result = 0;
    char path[PATH_MAX];
    for (size_t i = 0u; result == 0 &&
         i < sizeof(directories) / sizeof(directories[0]); ++i)
        result = initialized ?
            (oci_snprintf(path, sizeof(path), "%s/%s", store, directories[i]) >= 0 &&
             private_directory(path) ? 0 : -1) : ensure_private_child(store, directories[i], path);
    char blobs[PATH_MAX];
    char temporary[PATH_MAX];
    if (result == 0) result = ensure_private_child(store, "blobs", blobs);
    if (result == 0) result = ensure_private_child(blobs, "sha256", path);
    if (result == 0) result = ensure_private_child(store, "tmp", temporary);
    if (result == 0) result = ensure_private_child(temporary, "import", path);
    if (result == 0) result = ensure_private_child(temporary, "removal", path);

    char version[PATH_MAX];
    char migration[PATH_MAX];
    if (oci_snprintf(version, sizeof(version), "%s/store.version", store) <= 0 ||
        oci_snprintf(migration, sizeof(migration), "%s/migration.json", store) <= 0)
        result = -1;
    if (result == 0 && access(version, F_OK) == 0) {
        oci_document_t *migration_document = load_canonical_json_file(
            migration, OCI_JSON_MAX, 1);
        result = exact_file(version, "2\n", 2u) &&
            migration_document_valid(migration_document) ? 0 : -1;
        oci_document_release(migration_document);
    } else if (result == 0 && errno != ENOENT) {
        result = -1;
    } else if (result == 0) {
        char import_root[PATH_MAX];
        char migration_staging[PATH_MAX];
        char version_staging[PATH_MAX];
        if (oci_snprintf(import_root, sizeof(import_root), "%s/tmp/import", store) <= 0 ||
            oci_snprintf(migration_staging, sizeof(migration_staging),
                "%s/migration.XXXXXX", import_root) <= 0 ||
            oci_snprintf(version_staging, sizeof(version_staging),
                "%s/version.XXXXXX", import_root) <= 0) result = -1;
        int migration_fd = result == 0 ? mkstemp(migration_staging) : -1;
        if (migration_fd < 0 || close(migration_fd) != 0 ||
            unlink(migration_staging) != 0 ||
            build_migration_json(store, migration_staging) != 0 ||
            publish_canonical_file(migration_staging, migration, store) != 0)
            result = -1;
        int version_fd = result == 0 ? mkstemp(version_staging) : -1;
        if (result == 0 && (version_fd < 0 || close(version_fd) != 0 ||
            unlink(version_staging) != 0 ||
            oci_write_file_exclusive(version_staging, "2\n", 2u, 0400) != 0 ||
            publish_canonical_file(version_staging, version, store) != 0))
            result = -1;
        if (result != 0) {
            (void)unlink(migration_staging);
            (void)unlink(version_staging);
        }
    }
    if (result == 0) result = fsync_directory(store);
    maelys_oci_store_lock_release(&lock);
    return result;
}

static int list_platform_artifacts(
    const char *digest_path, const char *digest_name, oci_document_t *artifacts,
    oci_error_t *error) {
    DIR *platforms = opendir(digest_path);
    if (!platforms) return -1;
    char expected_digest[OCI_DIGEST_SIZE];
    int result = digest_from_directory_name(digest_name, expected_digest);
    struct dirent *platform_entry;
    while (result == 0 && (platform_entry = readdir(platforms)) != NULL) {
        if (oci_directory_entry_is_dot(platform_entry->d_name)) continue;
        char platform_path[PATH_MAX];
        char metadata_path[PATH_MAX];
        if (!oci_directory_entry_name_valid(platform_entry->d_name) ||
            oci_snprintf(platform_path, sizeof(platform_path), "%s/%s",
                digest_path, platform_entry->d_name) < 0 ||
            !private_directory(platform_path) ||
            oci_snprintf(metadata_path, sizeof(metadata_path),
                "%s/artifact.json", platform_path) < 0) {
            result = -1;
            break;
        }
        oci_document_t *metadata = load_artifact_metadata(
            metadata_path, expected_digest, NULL);
        const char *platform = metadata
            ? oci_document_string_value(oci_document_get(metadata, "platform")) : NULL;
        char platform_directory[OCI_PLATFORM_SIZE];
        char reference[96];
        if (!metadata || !platform ||
            oci_platform_to_directory(platform, platform_directory) != 0 ||
            strcmp(platform_directory, platform_entry->d_name) != 0 ||
            oci_snprintf(reference, sizeof(reference), "oci@%s",
                expected_digest) < 0) {
            int had_metadata = metadata != NULL;
            oci_document_release(metadata);
            /* A former artifact is named, never listed: this release does
             * not migrate it. */
            const char *former = NULL;
            oci_document_t *document = had_metadata ? NULL
                : load_former_artifact_schema(metadata_path, &former);
            if (former)
                oci_error_report(error, OCI_ERROR_STATE,
                    "artifact %s/%s carries the former schema %s, which this "
                    "release does not migrate: recreate the store and "
                    "reimport its sources", digest_name,
                    platform_entry->d_name, former);
            oci_document_release(document);
            result = -1;
            break;
        }
        oci_document_t *item = OCI_DOCUMENT_OBJECT(
            {"reference", oci_document_string(reference)},
            {"digest", oci_document_string(expected_digest)},
            {"platform", oci_document_string(platform)},
            {"metadata", metadata});
        if (!item || oci_document_append(artifacts, item) != 0) {
            result = -1;
        }
    }
    if (closedir(platforms) != 0) result = -1;
    return result;
}

static int list_artifacts(
    const char *store, oci_document_t *artifacts, oci_error_t *error) {
    char objects_path[PATH_MAX];
    if (oci_snprintf(objects_path, sizeof(objects_path), "%s/objects", store) < 0)
        return -1;
    DIR *objects = opendir(objects_path);
    if (!objects) return errno == ENOENT ? 0 : -1;
    int result = private_directory(objects_path) ? 0 : -1;
    struct dirent *digest_entry;
    while (result == 0 && (digest_entry = readdir(objects)) != NULL) {
        if (oci_directory_entry_is_dot(digest_entry->d_name)) continue;
        char digest_path[PATH_MAX];
        if (!oci_digest_hex_valid(digest_entry->d_name) ||
            oci_snprintf(digest_path, sizeof(digest_path), "%s/%s",
                objects_path, digest_entry->d_name) < 0 ||
            !private_directory(digest_path)) {
            result = -1;
            break;
        }
        result = list_platform_artifacts(
            digest_path, digest_entry->d_name, artifacts, error);
    }
    if (closedir(objects) != 0) result = -1;
    return result;
}

int oci_store_list(
    const char *store_argument, oci_document_t **out_document, oci_error_t *error) {
    *out_document = NULL;
    char *store = NULL;
    int opened = store_path_open(store_argument, &store, 1);
    if (opened < 0) {
        oci_error_report(error, OCI_ERROR_STATE,
            "store %s must be an absolute private directory owned by the "
            "caller", store_argument ? store_argument : "(null)");
        return -1;
    }
    oci_document_t *root = oci_document_object();
    oci_document_t *artifacts = oci_document_array();
    if (!root || !artifacts ||
        oci_document_put(root, "schema", oci_document_string(OCI_SCHEMA_STORE)) != 0 ||
        oci_document_put(root, "store", oci_document_string(store_argument)) != 0 ||
        oci_document_set(root, "artifacts", artifacts) != 0) {
        oci_document_release(root);
        oci_document_release(artifacts);
        free(store);
        oci_error_report(error, OCI_ERROR_MEMORY, "out of memory");
        return -1;
    }
    oci_document_release(artifacts); /* root owns the retained array. */
    int result = 0;
    if (opened == 0) {
        maelys_oci_store_lock_t store_lock = MAELYS_OCI_STORE_LOCK_INIT;
        if (maelys_oci_store_lock_acquire(
                store, "store.lock", 0, &store_lock) != 0) {
            oci_error_report(error, OCI_ERROR_IO,
                "cannot acquire the shared store lock of %s", store);
            result = -1;
        } else {
            result = list_artifacts(store, artifacts, error);
            if (result != 0 && error && error->kind == OCI_ERROR_NONE)
                oci_error_report(error, OCI_ERROR_STATE,
                    "store %s holds an artifact namespace with an unsafe or "
                    "unknown shape", store);
        }
        maelys_oci_store_lock_release(&store_lock);
    }
    free(store);
    if (result != 0) {
        oci_document_release(root);
        return -1;
    }
    *out_document = root;
    return 0;
}
