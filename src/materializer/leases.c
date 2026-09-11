/* SPDX-License-Identifier: MPL-2.0 */
/*
 * Execution leases and temporary store state: validation of lease
 * documents, stale-lease and abandoned-staging candidates for gc.
 */
#include "src/materializer/internal.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

int lease_name_valid(const char *name) {
    size_t size = name ? strlen(name) : 0u;
    int valid = size == 37u && strcmp(name + 32u, ".json") == 0;
    for (size_t i = 0u; valid && i < 32u; ++i)
        valid = (name[i] >= '0' && name[i] <= '9') ||
            (name[i] >= 'a' && name[i] <= 'f');
    return valid;
}

int lease_document_valid(oci_document_t *lease, lease_view_t *out) {
    if (!oci_document_is_object(lease) || !out)
        return 0;
    memset(out, 0, sizeof(*out));
    const char *schema =
        oci_document_string_value(oci_document_get(lease, "schema"));
    const char *liveness =
        oci_document_string_value(oci_document_get(lease, "liveness"));
    if (!schema) return 0;
    out->acquisition = strcmp(schema, OCI_ACQUISITION_LEASE_SCHEMA) == 0;
    if ((out->acquisition && oci_document_object_size(lease) != 7u) ||
        (!out->acquisition && (strcmp(schema, OCI_SCHEMA_LEASE) != 0 ||
         oci_document_object_size(lease) != 8u))) return 0;
    out->blobs = oci_document_get(lease, "blobs");
    if (out->acquisition) {
        size_t count = oci_document_array_size(out->blobs);
        if (!oci_document_is_array(out->blobs) || count < 2u || count > OCI_LAYER_MAX + 2u)
            return 0;
        for (size_t i = 0u; i < count; ++i) {
            const char *digest = oci_document_string_value(oci_document_at(out->blobs, i));
            if (!oci_digest_valid(digest)) return 0;
            for (size_t j = 0u; j < i; ++j)
                if (strcmp(digest, oci_document_string_value(oci_document_at(out->blobs, j))) == 0)
                    return 0;
        }
    }
    out->manifest =
        oci_document_string_value(oci_document_get(lease, "manifestDigest"));
    out->platform =
        oci_document_string_value(oci_document_get(lease, "platform"));
    out->root_digest =
        oci_document_string_value(oci_document_get(lease, "rootDigest"));
    out->rootfs_tar_digest =
        oci_document_string_value(oci_document_get(lease, "rootfsTarDigest"));
    oci_document_t *created = oci_document_get(lease, "createdUnixSeconds");
    oci_document_t *expires = oci_document_get(lease, "expiresUnixSeconds");
    if (!liveness ||
        strcmp(liveness, MAELYS_OCI_LEASE_LIVENESS) != 0 ||
        !oci_digest_valid(out->manifest) || !out->platform ||
        oci_platform_to_directory(out->platform, (char[72]){0}) != 0 ||
        (!out->acquisition && (!oci_digest_valid(out->root_digest) ||
         !oci_digest_valid(out->rootfs_tar_digest))) ||
        (out->acquisition && strcmp(out->manifest,
            oci_document_string_value(oci_document_at(out->blobs, 0u))) != 0) ||
        !oci_document_is_integer(created) || oci_document_integer_value(created) < 0 ||
        !oci_document_is_integer(expires) ||
        oci_document_integer_value(expires) < oci_document_integer_value(created)) return 0;
    out->created = (uint64_t)oci_document_integer_value(created);
    out->expires = (uint64_t)oci_document_integer_value(expires);
    return 1;
}

/* Asks the kernel about a lease and lets go at once: 1 held elsewhere,
 * 0 nobody holds it, -1 unanswerable. The store lock the caller holds
 * decides what a 0 means: exclusive (gc), nobody can take the lease
 * before gc retires it; shared (verify), a resolution may be between its
 * publication and its lock, so 0 is only a warning there. */
static int lease_probe(const char *path) {
    maelys_sys_file_lock_t *lock = NULL;
    int probe = maelys_oci_store_lease_probe(path, &lock);
    (void)maelys_sys_file_lock_release(&lock);
    return probe;
}

int scan_leases(const char *store, store_report_t *report) {
    char leases_path[PATH_MAX];
    if (oci_snprintf(leases_path, sizeof(leases_path), "%s/leases", store) <= 0 ||
        !private_directory(leases_path)) return -1;
    DIR *leases = opendir(leases_path);
    if (!leases) return -1;
    int result = 0;
    struct dirent *entry;
    while (result == 0 && (entry = readdir(leases)) != NULL) {
        if (oci_directory_entry_is_dot(entry->d_name)) continue;
        char path[PATH_MAX];
        if (!lease_name_valid(entry->d_name) ||
            oci_snprintf(path, sizeof(path), "%s/%s", leases_path,
                entry->d_name) <= 0) {
            (void)report_message(report->errors,
                "invalid OCI lease entry %s", entry->d_name);
            continue;
        }
        oci_document_t *lease = load_canonical_json_file(path, OCI_LEASE_MAX_BYTES, 1);
        lease_view_t view;
        if (!lease_document_valid(lease, &view)) {
            (void)report_message(report->errors,
                "OCI lease %s is malformed", entry->d_name);
            oci_document_release(lease);
            continue;
        }
        char platform_name[72];
        char artifact_path[PATH_MAX];
        (void)oci_platform_to_directory(view.platform, platform_name);
        if (!view.acquisition && (oci_snprintf(artifact_path, sizeof(artifact_path), "%s/objects/%s/%s",
                store, view.manifest + 7u, platform_name) <= 0 ||
            !private_directory(artifact_path)))
            (void)report_message(report->errors,
                "OCI lease %s references an absent artifact", entry->d_name);
        else if (lease_probe(path) == 0)
            (void)report_message(report->warnings,
                "OCI lease %s has no live holder; gc retires it after its "
                "expiry and the grace period", entry->d_name);
        ++report->leases;
        oci_document_release(lease);
    }
    if (closedir(leases) != 0) result = -1;
    return result;
}

/* An uncertain holder retains all of the acquisition's complete objects.
 * Missing objects are expected while acquiring and are never fabricated. */
int scan_acquisition_roots(const char *store, int collecting, uint64_t grace_seconds,
    digest_set_t *reachable) {
    char directory[PATH_MAX];
    if (oci_snprintf(directory, sizeof(directory), "%s/leases", store) < 0 ||
        !private_directory(directory)) return -1;
    DIR *leases = opendir(directory);
    if (!leases) return -1;
    time_t now = time(NULL);
    int result = now < 0 ? -1 : 0;
    struct dirent *entry;
    while (!result && (entry = readdir(leases)) != NULL) {
        if (oci_directory_entry_is_dot(entry->d_name)) continue;
        char path[PATH_MAX];
        if (!lease_name_valid(entry->d_name) ||
            oci_snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name) < 0) {
            result = -1; break;
        }
        oci_document_t *lease = load_canonical_json_file(path, OCI_LEASE_MAX_BYTES, 1);
        lease_view_t view;
        if (!lease_document_valid(lease, &view)) result = -1;
        else if (view.acquisition) {
            int expired = collecting && (uint64_t)now >= view.created &&
                view.expires <= UINT64_MAX - grace_seconds &&
                (uint64_t)now >= view.expires + grace_seconds && lease_probe(path) == 0;
            for (size_t i = 0u; !expired && !result && i < oci_document_array_size(view.blobs); ++i)
                result = digest_set_add(reachable,
                    oci_document_string_value(oci_document_at(view.blobs, i)));
        }
        oci_document_release(lease);
    }
    if (closedir(leases) != 0) result = -1;
    return result;
}

int collect_stale_leases(
    const char *store, uint64_t grace_seconds, store_report_t *report,
    oci_document_t *candidates) {
    char leases_path[PATH_MAX];
    if (oci_snprintf(leases_path, sizeof(leases_path), "%s/leases", store) <= 0 ||
        !private_directory(leases_path)) return -1;
    DIR *leases = opendir(leases_path);
    if (!leases) return -1;
    time_t wall = time(NULL);
    int result = wall >= 0 ? 0 : -1;
    struct dirent *entry;
    while (result == 0 && (entry = readdir(leases)) != NULL) {
        if (oci_directory_entry_is_dot(entry->d_name)) continue;
        char path[PATH_MAX];
        char relative[PATH_MAX];
        if (!lease_name_valid(entry->d_name) ||
            oci_snprintf(path, sizeof(path), "%s/%s", leases_path,
                entry->d_name) <= 0 ||
            oci_snprintf(relative, sizeof(relative), "leases/%s",
                entry->d_name) <= 0) {
            result = -1;
            break;
        }
        oci_document_t *lease = load_canonical_json_file(path, OCI_LEASE_MAX_BYTES, 1);
        lease_view_t view;
        struct stat status;
        if (!lease_document_valid(lease, &view) ||
            lstat(path, &status) != 0 || !S_ISREG(status.st_mode) ||
            S_ISLNK(status.st_mode) || status.st_uid != geteuid() ||
            (status.st_mode & 0777) != 0400) {
            oci_document_release(lease);
            result = -1;
            break;
        }
        uint64_t created_at = view.created;
        uint64_t expires_at = view.expires;
        uint64_t now = (uint64_t)wall;
        int probe = lease_probe(path);
        if (now < created_at) {
            (void)report_message(report->warnings,
                "clock moved behind lease %s; retaining it", entry->d_name);
        } else if (probe < 0) {
            (void)report_message(report->warnings,
                "cannot ask the kernel about lease %s; retaining it",
                entry->d_name);
        } else if (probe == 0 && expires_at <= UINT64_MAX - grace_seconds &&
                   now >= expires_at + grace_seconds) {
            oci_document_t *candidate = OCI_DOCUMENT_OBJECT(
            {"kind", oci_document_string("lease")},
            {"path", oci_document_string(relative)},
            {"device", oci_document_integer((int64_t)status.st_dev)},
            {"inode", oci_document_integer((int64_t)status.st_ino)});
            if (!candidate ||
                oci_document_append(candidates, candidate) != 0) {
                result = -1;
            }
        } else if (probe == 0) {
            (void)report_message(report->warnings,
                "lease %s has no live holder; retained until its expiry "
                "and the grace period", entry->d_name);
        }
        oci_document_release(lease);
    }
    if (closedir(leases) != 0) result = -1;
    return result;
}

static int temporary_name_valid(const char *name) {
    if (!name || !name[0] || strlen(name) > 255u ||
        strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return 0;
    for (const unsigned char *cursor = (const unsigned char *)name;
         *cursor; ++cursor) {
        if (!((*cursor >= 'a' && *cursor <= 'z') ||
              (*cursor >= 'A' && *cursor <= 'Z') ||
              (*cursor >= '0' && *cursor <= '9') ||
              *cursor == '-' || *cursor == '_' || *cursor == '.')) return 0;
    }
    return 1;
}

static int scan_temporary_namespace(
    const char *store, const char *namespace_name, int collect,
    uint64_t grace_seconds, int warn, store_report_t *report,
    oci_document_t *candidates) {
    char path[PATH_MAX];
    if (oci_snprintf(path, sizeof(path), "%s/tmp/%s", store,
            namespace_name) <= 0 || !private_directory(path)) return -1;
    DIR *directory = opendir(path);
    if (!directory) return -1;
    time_t wall = time(NULL);
    int result = wall >= 0 ? 0 : -1;
    struct dirent *entry;
    while (result == 0 && (entry = readdir(directory)) != NULL) {
        if (oci_directory_entry_is_dot(entry->d_name)) continue;
        char item[PATH_MAX];
        char relative[PATH_MAX];
        struct stat status;
        if (!temporary_name_valid(entry->d_name) ||
            oci_snprintf(item, sizeof(item), "%s/%s", path, entry->d_name) <= 0 ||
            oci_snprintf(relative, sizeof(relative), "tmp/%s/%s", namespace_name,
                entry->d_name) <= 0 || lstat(item, &status) != 0 ||
            S_ISLNK(status.st_mode) || status.st_uid != geteuid() ||
            (!S_ISDIR(status.st_mode) && !S_ISREG(status.st_mode)) ||
            (S_ISDIR(status.st_mode) && (status.st_mode & 0777) != 0700) ||
            (S_ISREG(status.st_mode) &&
             ((status.st_mode & (S_IWGRP | S_IWOTH)) != 0))) {
            (void)report_message(report->errors,
                "unsafe temporary store entry %s/%s", namespace_name,
                entry->d_name);
            continue;
        }
        if (warn)
            (void)report_message(report->warnings,
                "temporary %s state awaits recovery: %s",
                namespace_name, entry->d_name);
        uint64_t modified = status.st_mtime >= 0
            ? (uint64_t)status.st_mtime : UINT64_MAX;
        uint64_t now = (uint64_t)wall;
        if (collect && modified != UINT64_MAX && now >= modified &&
            modified <= UINT64_MAX - grace_seconds &&
            now >= modified + grace_seconds) {
            oci_document_t *candidate = OCI_DOCUMENT_OBJECT(
            {"kind", oci_document_string("temporary")},
            {"path", oci_document_string(relative)},
            {"bytes", oci_document_integer((int64_t)(S_ISREG(status.st_mode)
                    ? status.st_size : 0))},
            {"device", oci_document_integer((int64_t)status.st_dev)},
            {"inode", oci_document_integer((int64_t)status.st_ino)});
            if (!candidate ||
                oci_document_append(candidates, candidate) != 0) {
                result = -1;
            }
        }
    }
    if (closedir(directory) != 0) result = -1;
    return result;
}

int scan_temporary_state(
    const char *store, int collect, uint64_t grace_seconds, int warn,
    store_report_t *report, oci_document_t *candidates) {
    return scan_temporary_namespace(store, "import", collect, grace_seconds,
               warn, report, candidates) == 0 &&
        scan_temporary_namespace(store, "removal", collect, grace_seconds,
               warn, report, candidates) == 0 ? 0 : -1;
}
