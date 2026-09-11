/* SPDX-License-Identifier: MPL-2.0 */
/*
 * Whole-store verification: closures, blobs, artifacts, leases, locks and
 * the store shape, accumulated into a report that verify and gc publish.
 */
#include "src/materializer/internal.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void digest_set_clear(digest_set_t *set) {
    free(set->items);
    memset(set, 0, sizeof(*set));
}

static int digest_set_contains(const digest_set_t *set, const char *hex) {
    for (size_t i = 0u; i < set->count; ++i)
        if (strcmp(set->items[i], hex) == 0) return 1;
    return 0;
}

int digest_set_add(digest_set_t *set, const char *digest) {
    const char *hex = oci_digest_valid(digest) ? digest + 7u : digest;
    if (!oci_digest_hex_valid(hex)) return -1;
    if (digest_set_contains(set, hex)) return 0;
    if (set->count == set->capacity) {
        size_t capacity = set->capacity ? set->capacity * 2u : 16u;
        if (capacity < set->capacity || capacity > SIZE_MAX / sizeof(*set->items))
            return -1;
        void *grown = realloc(set->items, capacity * sizeof(*set->items));
        if (!grown) return -1;
        set->items = grown;
        set->capacity = capacity;
    }
    memcpy(set->items[set->count++], hex, 65u);
    return 0;
}

int report_message(oci_document_t *array, const char *format, ...) {
    char message[1024];
    va_list arguments;
    va_start(arguments, format);
    int size = vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    return size > 0 && (size_t)size < sizeof(message) &&
        oci_document_append(array, oci_document_string(message)) == 0 ? 0 : -1;
}

oci_document_t *load_canonical_json_file(
    const char *path, size_t maximum, int require_read_only) {
    int descriptor = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    struct stat status;
    if (descriptor < 0 || fstat(descriptor, &status) != 0 ||
        !S_ISREG(status.st_mode) || status.st_size <= 0 ||
        (uintmax_t)status.st_size > maximum || status.st_uid != geteuid() ||
        status.st_nlink != 1 ||
        (status.st_mode & (S_IWGRP | S_IWOTH)) != 0 ||
        (require_read_only && (status.st_mode & 0777) != 0400)) {
        if (descriptor >= 0) (void)maelys_sys_fd_close(&descriptor);
        return NULL;
    }
    unsigned char *bytes = NULL;
    size_t size = 0u;
    int read_result = oci_read_regular_bounded(descriptor, maximum, &bytes, &size);
    (void)maelys_sys_fd_close(&descriptor);
    if (read_result != 0 || size < 2u || bytes[size - 1u] != '\n') {
        free(bytes);
        return NULL;
    }
    oci_document_t *document = load_json(bytes, size);
    char *canonical = document
        ? oci_document_dump(document) : NULL;
    size_t canonical_size = canonical ? strlen(canonical) : 0u;
    int matches = canonical && canonical_size + 1u == size &&
        memcmp(bytes, canonical, canonical_size) == 0;
    free(canonical);
    free(bytes);
    if (!matches) {
        oci_document_release(document);
        return NULL;
    }
    return document;
}

static int closure_directory_has_exact_members(const char *path) {
    DIR *directory = opendir(path);
    if (!directory) return 0;
    unsigned int closure_count = 0u;
    int valid = 1;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        if (oci_directory_entry_is_dot(entry->d_name)) continue;
        if (strcmp(entry->d_name, "closure.json") == 0) ++closure_count;
        else valid = 0;
    }
    if (closedir(directory) != 0) valid = 0;
    return valid && closure_count == 1u;
}

int scan_source_closures(
    const char *store, digest_set_t *reachable, store_report_t *report) {
    char sources_path[PATH_MAX];
    if (oci_snprintf(sources_path, sizeof(sources_path), "%s/sources", store) <= 0 ||
        !private_directory(sources_path)) return -1;
    DIR *sources = opendir(sources_path);
    if (!sources) return -1;
    int result = 0;
    struct dirent *digest_entry;
    while (result == 0 && (digest_entry = readdir(sources)) != NULL) {
        if (oci_directory_entry_is_dot(digest_entry->d_name)) continue;
        if (!oci_digest_hex_valid(digest_entry->d_name)) {
            (void)report_message(report->errors,
                "invalid source manifest directory %s", digest_entry->d_name);
            continue;
        }
        char digest_path[PATH_MAX];
        if (oci_snprintf(digest_path, sizeof(digest_path), "%s/%s", sources_path,
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
            char directory[PATH_MAX];
            char closure_path[PATH_MAX];
            if (!oci_directory_entry_name_valid(platform_entry->d_name) ||
                oci_snprintf(directory, sizeof(directory), "%s/%s", digest_path,
                    platform_entry->d_name) <= 0 || !private_directory(directory) ||
                !closure_directory_has_exact_members(directory) ||
                oci_snprintf(closure_path, sizeof(closure_path), "%s/closure.json",
                    directory) <= 0) {
                result = -1;
                break;
            }
            oci_document_t *closure = load_canonical_json_file(
                closure_path, OCI_JSON_MAX, 1);
            const char *schema = closure
                ? oci_document_string_value(oci_document_get(closure, "schema")) : NULL;
            const char *manifest_digest = closure
                ? oci_document_string_value(oci_document_get(closure, "manifestDigest")) : NULL;
            const char *platform = closure
                ? oci_document_string_value(oci_document_get(closure, "platform")) : NULL;
            char expected_manifest[72];
            char platform_directory[72];
            if (digest_from_directory_name(
                    digest_entry->d_name, expected_manifest) != 0 || !closure ||
                !oci_document_is_object(closure) || oci_document_object_size(closure) != 6u || !schema ||
                strcmp(schema, OCI_SCHEMA_CLOSURE) != 0 ||
                !manifest_digest || strcmp(manifest_digest, expected_manifest) != 0 ||
                !platform || oci_platform_to_directory(platform, platform_directory) != 0 ||
                strcmp(platform_directory, platform_entry->d_name) != 0) {
                (void)report_message(report->errors,
                    "invalid canonical closure %s/%s", digest_entry->d_name,
                    platform_entry->d_name);
                oci_document_release(closure);
                continue;
            }
            result = verify_source_closure(store, closure, reachable, report);
            char artifact[PATH_MAX];
            if (oci_snprintf(artifact, sizeof(artifact), "%s/objects/%s/%s", store,
                    digest_entry->d_name, platform_entry->d_name) <= 0) {
                result = -1;
            } else if (!private_directory(artifact)) {
                (void)report_message(report->warnings,
                    "source closure %s %s has no derived artifact",
                    expected_manifest, platform);
            }
            ++report->closures;
            oci_document_release(closure);
        }
        if (closedir(platforms) != 0) result = -1;
    }
    if (closedir(sources) != 0) result = -1;
    return result;
}

int scan_blobs(
    const char *store, const digest_set_t *reachable, int report_unreachable,
    store_report_t *report, oci_document_t *candidates) {
    char path[PATH_MAX];
    if (oci_snprintf(path, sizeof(path), "%s/blobs/sha256", store) <= 0 ||
        !private_directory(path)) return -1;
    DIR *directory = opendir(path);
    if (!directory) return -1;
    int result = 0;
    struct dirent *entry;
    while (result == 0 && (entry = readdir(directory)) != NULL) {
        if (oci_directory_entry_is_dot(entry->d_name)) continue;
        if (!oci_digest_hex_valid(entry->d_name)) {
            (void)report_message(
                report->errors, "invalid blob name %s", entry->d_name);
            continue;
        }
        char blob[PATH_MAX];
        char digest[72];
        uint64_t size = 0u;
        if (oci_snprintf(blob, sizeof(blob), "%s/%s", path, entry->d_name) <= 0 ||
            safe_regular_digest(blob, 0400, digest, &size) != 0 ||
            strcmp(digest + 7u, entry->d_name) != 0) {
            (void)report_message(
                report->errors, "blob %s fails content-address validation",
                entry->d_name);
            continue;
        }
        if (report->blobs == UINT64_MAX ||
            UINT64_MAX - report->blob_bytes < size) {
            (void)report_message(
                report->errors, "blob accounting exceeds its canonical range");
            result = -1;
            break;
        }
        ++report->blobs;
        report->blob_bytes += size;
        if (report_unreachable && !digest_set_contains(reachable, entry->d_name)) {
            char relative[PATH_MAX];
            if (oci_snprintf(relative, sizeof(relative), "blobs/sha256/%s",
                    entry->d_name) <= 0) {
                result = -1;
                break;
            }
            oci_document_t *candidate = OCI_DOCUMENT_OBJECT(
            {"kind", oci_document_string("blob")},
            {"path", oci_document_string(relative)},
            {"digest", oci_document_string(digest)},
            {"bytes", oci_document_integer((int64_t)size)});
            if (!candidate || oci_document_append(candidates, candidate) != 0) {
                result = -1;
            }
        }
    }
    if (closedir(directory) != 0) result = -1;
    return result;
}

static int seal_binds_artifact(
    const char *path, const char *manifest, const char *config,
    const char *platform, const char *root_digest, uint64_t root_size,
    const char *rootfs_tar_digest, uint64_t rootfs_tar_size) {
    maelys_oci_seal_t seal;
    return maelys_oci_seal_parse(path, &seal) == 0 &&
        strcmp(seal.manifest, manifest) == 0 &&
        strcmp(seal.config, config) == 0 &&
        strcmp(seal.platform, platform) == 0 &&
        strcmp(seal.root, root_digest) == 0 &&
        seal.root_size == root_size &&
        strcmp(seal.rootfs_tar, rootfs_tar_digest) == 0 &&
        seal.rootfs_tar_size == rootfs_tar_size;
}

static int migration_matches(
    oci_document_t *migration, const char *manifest, const char *platform,
    const char *root_digest, const char *seal_digest) {
    oci_document_t *legacy = oci_document_get(migration, "legacyDerived");
    if (!oci_document_is_array(legacy)) return 0;
    for (size_t i = 0u; i < oci_document_array_size(legacy); ++i) {
        oci_document_t *item = oci_document_at(legacy, i);
        const char *item_manifest =
            oci_document_string_value(oci_document_get(item, "manifestDigest"));
        const char *item_platform =
            oci_document_string_value(oci_document_get(item, "platform"));
        const char *item_root =
            oci_document_string_value(oci_document_get(item, "rootDigest"));
        const char *item_seal =
            oci_document_string_value(oci_document_get(item, "sealDigest"));
        if (item_manifest && item_platform && item_root && item_seal &&
            strcmp(item_manifest, manifest) == 0 &&
            strcmp(item_platform, platform) == 0 &&
            strcmp(item_root, root_digest) == 0 &&
            strcmp(item_seal, seal_digest) == 0) return 1;
    }
    return 0;
}

static int artifact_has_exact_members(const char *path) {
    DIR *directory = opendir(path);
    if (!directory) return 0;
    unsigned int mask = 0u;
    int valid = 1;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        if (oci_directory_entry_is_dot(entry->d_name)) continue;
        if (strcmp(entry->d_name, "root.ext4") == 0) mask |= 1u;
        else if (strcmp(entry->d_name, "artifact.json") == 0) mask |= 2u;
        else if (strcmp(entry->d_name, "artifact.seal") == 0) mask |= 4u;
        else if (strcmp(entry->d_name, "rootfs.tar") == 0) mask |= 8u;
        else valid = 0;
    }
    if (closedir(directory) != 0) valid = 0;
    return valid && mask == 15u;
}

int scan_artifacts(
    const char *store, oci_document_t *migration, store_report_t *report) {
    char objects_path[PATH_MAX];
    if (oci_snprintf(objects_path, sizeof(objects_path), "%s/objects", store) <= 0 ||
        !private_directory(objects_path)) return -1;
    DIR *objects = opendir(objects_path);
    if (!objects) return -1;
    int result = 0;
    struct dirent *digest_entry;
    while (result == 0 && (digest_entry = readdir(objects)) != NULL) {
        if (oci_directory_entry_is_dot(digest_entry->d_name)) continue;
        if (!oci_digest_hex_valid(digest_entry->d_name)) {
            (void)report_message(report->errors,
                "invalid artifact manifest directory %s", digest_entry->d_name);
            continue;
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
            char artifact[PATH_MAX];
            char metadata_path[PATH_MAX];
            char root_path[PATH_MAX];
            char rootfs_tar_path[PATH_MAX];
            char seal_path[PATH_MAX];
            char source_path[PATH_MAX];
            char manifest[72];
            if (!oci_directory_entry_name_valid(platform_entry->d_name) ||
                oci_snprintf(artifact, sizeof(artifact), "%s/%s", digest_path,
                    platform_entry->d_name) <= 0 || !private_directory(artifact) ||
                !artifact_has_exact_members(artifact) ||
                oci_snprintf(metadata_path, sizeof(metadata_path), "%s/artifact.json",
                    artifact) <= 0 ||
                oci_snprintf(root_path, sizeof(root_path), "%s/root.ext4", artifact) <= 0 ||
                oci_snprintf(rootfs_tar_path, sizeof(rootfs_tar_path),
                    "%s/rootfs.tar", artifact) <= 0 ||
                oci_snprintf(seal_path, sizeof(seal_path), "%s/artifact.seal", artifact) <= 0 ||
                oci_snprintf(source_path, sizeof(source_path), "%s/sources/%s/%s",
                    store, digest_entry->d_name, platform_entry->d_name) <= 0 ||
                digest_from_directory_name(digest_entry->d_name, manifest) != 0) {
                (void)report_message(report->errors,
                    "artifact %s/%s has unsafe shape", digest_entry->d_name,
                    platform_entry->d_name);
                continue;
            }
            int has_closure = private_directory(source_path);
            oci_document_t *metadata = load_canonical_json_file(
                metadata_path, OCI_JSON_MAX, 1);
            const char *platform = metadata
                ? oci_document_string_value(oci_document_get(metadata, "platform")) : NULL;
            const char *config = metadata
                ? oci_document_string_value(oci_document_get(metadata, "configDigest")) : NULL;
            const char *recorded_root = metadata
                ? oci_document_string_value(oci_document_get(metadata, "rootDigest")) : NULL;
            const char *recorded_tar = metadata
                ? oci_document_string_value(oci_document_get(metadata, "rootfsTarDigest")) : NULL;
            char platform_directory[72];
            char root_digest[72];
            char rootfs_tar_digest[72];
            char seal_digest[72];
            uint64_t root_size = 0u;
            uint64_t rootfs_tar_size = 0u;
            if (!metadata || !platform || !config || !recorded_root ||
                !recorded_tar ||
                oci_platform_to_directory(platform, platform_directory) != 0 ||
                strcmp(platform_directory, platform_entry->d_name) != 0 ||
                safe_regular_digest(root_path, 0400, root_digest, &root_size) != 0 ||
                safe_regular_digest(rootfs_tar_path, 0400,
                    rootfs_tar_digest, &rootfs_tar_size) != 0 ||
                safe_regular_digest(seal_path, 0400,
                    seal_digest, NULL) != 0 ||
                strcmp(recorded_root, root_digest) != 0 ||
                strcmp(recorded_tar, rootfs_tar_digest) != 0 ||
                !seal_binds_artifact(seal_path, manifest, config, platform,
                    root_digest, root_size,
                    rootfs_tar_digest, rootfs_tar_size)) {
                const char *schema = metadata
                    ? oci_document_string_value(oci_document_get(metadata, "schema"))
                    : NULL;
                if (artifact_schema_is_former(schema))
                    (void)report_message(report->errors,
                        "artifact %s/%s carries the former schema %s, which "
                        "this release does not migrate: recreate the store "
                        "and reimport its sources",
                        digest_entry->d_name, platform_entry->d_name, schema);
                else
                    (void)report_message(report->errors,
                        "artifact %s/%s fails seal or content verification",
                        digest_entry->d_name, platform_entry->d_name);
                oci_document_release(metadata);
                continue;
            }
            if (!has_closure) {
                if (migration_matches(migration, manifest, platform,
                        root_digest, seal_digest)) {
                    (void)report_message(report->warnings,
                        "legacy derived-only artifact %s %s has no source closure",
                        manifest, platform);
                } else {
                    (void)report_message(report->errors,
                        "artifact %s %s lacks a source closure and migration witness",
                        manifest, platform);
                }
            }
            ++report->artifacts;
            oci_document_release(metadata);
        }
        if (closedir(platforms) != 0) result = -1;
    }
    if (closedir(objects) != 0) result = -1;
    return result;
}

/* ---- whole-store verification ---------------------------------------------- */

static oci_document_t *load_migration(const char *store, store_report_t *report) {
    char path[PATH_MAX];
    if (oci_snprintf(path, sizeof(path), "%s/migration.json", store) <= 0)
        return NULL;
    oci_document_t *migration = load_canonical_json_file(path, OCI_JSON_MAX, 1);
    if (!migration_document_valid(migration)) {
        (void)report_message(
            report->errors, "migration.json is absent or non-canonical");
        oci_document_release(migration);
        return NULL;
    }
    return migration;
}

int report_initialize(store_report_t *report) {
    memset(report, 0, sizeof(*report));
    report->errors = oci_document_array();
    report->warnings = oci_document_array();
    if (report->errors && report->warnings) return 0;
    oci_document_release(report->errors);
    oci_document_release(report->warnings);
    memset(report, 0, sizeof(*report));
    return -1;
}

void report_clear(store_report_t *report) {
    oci_document_release(report->errors);
    oci_document_release(report->warnings);
    memset(report, 0, sizeof(*report));
}

static int validate_store_version(const char *store, store_report_t *report) {
    char path[PATH_MAX];
    if (oci_snprintf(path, sizeof(path), "%s/store.version", store) <= 0 ||
        !exact_file(path, "2\n", 2u)) {
        (void)report_message(report->errors,
            "store.version is absent or is not the immutable v2 marker");
        return -1;
    }
    return 0;
}

static int store_root_member_allowed(const char *name) {
    static const char *const allowed[] = {
        "blobs", "leases", "locks", "migration.json", "objects",
        "sources", "store.version", "tmp", NULL
    };
    for (size_t i = 0u; allowed[i]; ++i)
        if (strcmp(allowed[i], name) == 0) return 1;
    return 0;
}

static int validate_lock_namespace(
    const char *store, store_report_t *report) {
    char path[PATH_MAX];
    if (oci_snprintf(path, sizeof(path), "%s/locks", store) <= 0 ||
        !private_directory(path)) return -1;
    DIR *directory = opendir(path);
    if (!directory) return -1;
    int result = 0;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        if (oci_directory_entry_is_dot(entry->d_name)) continue;
        size_t size = strlen(entry->d_name);
        int name_valid = strcmp(entry->d_name, "store.lock") == 0;
        if (size == 69u && strcmp(entry->d_name + 64u, ".lock") == 0) {
            name_valid = 1;
            for (size_t i = 0u; name_valid && i < 64u; ++i)
                name_valid = (entry->d_name[i] >= '0' &&
                              entry->d_name[i] <= '9') ||
                    (entry->d_name[i] >= 'a' && entry->d_name[i] <= 'f');
        }
        char member[PATH_MAX];
        struct stat status;
        if (!name_valid || oci_snprintf(member, sizeof(member), "%s/%s", path,
                entry->d_name) <= 0 || lstat(member, &status) != 0 ||
            !S_ISREG(status.st_mode) || S_ISLNK(status.st_mode) ||
            status.st_uid != geteuid() || (status.st_mode & 0777) != 0600 ||
            status.st_nlink != 1) {
            (void)report_message(report->errors,
                "unsafe or unknown lock entry %s", entry->d_name);
        }
    }
    if (closedir(directory) != 0) result = -1;
    return result;
}

static int validate_store_shape(
    const char *store, store_report_t *report) {
    DIR *root = opendir(store);
    if (!root) return -1;
    int result = 0;
    struct dirent *entry;
    while ((entry = readdir(root)) != NULL) {
        if (oci_directory_entry_is_dot(entry->d_name)) continue;
        if (!store_root_member_allowed(entry->d_name))
            (void)report_message(report->errors,
                "unknown published store entry %s", entry->d_name);
    }
    if (closedir(root) != 0) result = -1;
    char blobs[PATH_MAX];
    char tmp[PATH_MAX];
    if (oci_snprintf(blobs, sizeof(blobs), "%s/blobs", store) <= 0 ||
        oci_snprintf(tmp, sizeof(tmp), "%s/tmp", store) <= 0 ||
        !private_directory(blobs) || !private_directory(tmp)) return -1;
    DIR *blob_root = opendir(blobs);
    size_t blob_children = 0u;
    while (blob_root && (entry = readdir(blob_root)) != NULL) {
        if (oci_directory_entry_is_dot(entry->d_name)) continue;
        if (strcmp(entry->d_name, "sha256") != 0) {
            (void)report_message(report->errors,
                "unknown blob namespace %s", entry->d_name);
        } else {
            ++blob_children;
        }
    }
    if (!blob_root || closedir(blob_root) != 0 || blob_children != 1u)
        result = -1;
    DIR *tmp_root = opendir(tmp);
    unsigned int tmp_mask = 0u;
    while (tmp_root && (entry = readdir(tmp_root)) != NULL) {
        if (oci_directory_entry_is_dot(entry->d_name)) continue;
        if (strcmp(entry->d_name, "import") == 0) tmp_mask |= 1u;
        else if (strcmp(entry->d_name, "removal") == 0) tmp_mask |= 2u;
        else (void)report_message(report->errors,
            "unknown temporary namespace %s", entry->d_name);
    }
    if (!tmp_root || closedir(tmp_root) != 0 || tmp_mask != 3u) result = -1;
    if (validate_lock_namespace(store, report) != 0) result = -1;
    return result;
}

int inspect_store_locked(
    const char *store, int collect_candidates, uint64_t grace_seconds, store_report_t *report,
    digest_set_t *reachable, oci_document_t *candidates) {
    int result = 0;
    (void)validate_store_version(store, report);
    if (validate_store_shape(store, report) != 0) result = -1;
    oci_document_t *migration = load_migration(store, report);
    if (!migration) result = -1;
    if (scan_source_closures(store, reachable, report) != 0) result = -1;
    if (scan_acquisition_roots(store, collect_candidates, grace_seconds, reachable) != 0)
        result = -1;
    /* This test only spares an already broken store the noise of unreachable
     * blobs; it cannot see the errors scan_artifacts and scan_leases report
     * below. The guard that decides what is actually retired is in
     * oci_store_gc, which recomputes validity after the whole inspection. */
    if (scan_blobs(store, reachable, collect_candidates && result == 0 &&
            oci_document_array_size(report->errors) == 0u,
            report, candidates) != 0) result = -1;
    if (migration && scan_artifacts(store, migration, report) != 0) result = -1;
    if (scan_leases(store, report) != 0) result = -1;
    if (scan_temporary_state(
            store, 0, 0u, 1, report, candidates) != 0) result = -1;
    oci_document_release(migration);
    return result;
}

oci_document_t *store_report_json(
    const char *store, const store_report_t *report) {
    if (report->artifacts > (uint64_t)INT64_MAX ||
        report->blob_bytes > (uint64_t)INT64_MAX ||
        report->blobs > (uint64_t)INT64_MAX ||
        report->closures > (uint64_t)INT64_MAX ||
        report->leases > (uint64_t)INT64_MAX) return NULL;
    oci_document_t *counts = OCI_DOCUMENT_OBJECT(
            {"artifacts", oci_document_integer((int64_t)report->artifacts)},
            {"blobBytes", oci_document_integer((int64_t)report->blob_bytes)},
            {"blobs", oci_document_integer((int64_t)report->blobs)},
            {"closures", oci_document_integer((int64_t)report->closures)},
            {"leases", oci_document_integer((int64_t)report->leases)});
    oci_document_t *root = oci_document_object();
    if (!counts || !root ||
        oci_document_set(root, "counts", counts) != 0 ||
        oci_document_set(root, "errors", report->errors) != 0 ||
        oci_document_put(root, "schema",
            oci_document_string(OCI_SCHEMA_VERIFICATION)) != 0 ||
        oci_document_put(root, "store", oci_document_string(store)) != 0 ||
        oci_document_set(root, "warnings", report->warnings) != 0) {
        oci_document_release(counts);
        oci_document_release(root);
        return NULL;
    }
    oci_document_release(counts);
    return root;
}

int oci_store_verify(
    const char *store_argument, oci_document_t **out_document, int *out_valid,
    oci_error_t *error) {
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
        store, "store.lock", 0, &lock) == 0 &&
        inspect_store_locked(
            store, 0, 0u, &report, &reachable, candidates) == 0;
    maelys_oci_store_lock_release(&lock);
    if (!inspected && oci_document_array_size(report.errors) == 0u)
        (void)report_message(report.errors, "cannot inspect the OCI store");
    oci_document_t *document = store_report_json(store, &report);
    int valid = inspected && oci_document_array_size(report.errors) == 0u;
    digest_set_clear(&reachable);
    report_clear(&report);
    oci_document_release(candidates);
    free(store);
    if (!document ||
        oci_document_put(document, "valid", oci_document_boolean(valid)) != 0) {
        oci_document_release(document);
        oci_error_report(error, OCI_ERROR_MEMORY,
            "cannot build the verification report");
        return -1;
    }
    *out_document = document;
    *out_valid = valid;
    return 0;
}
