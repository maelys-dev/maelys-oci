/* SPDX-License-Identifier: MPL-2.0 */
#include "src/store/core.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <time.h>
#include <maelys/sys/clock.h>

static int component_valid(const char *name) {
    if (!name || !name[0] || strlen(name) > 255u ||
        strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return 0;
    for (const unsigned char *cursor = (const unsigned char *)name;
         *cursor; ++cursor) {
        if (!((*cursor >= 'a' && *cursor <= 'z') ||
              (*cursor >= '0' && *cursor <= '9') ||
              *cursor == '-' || *cursor == '.')) return 0;
    }
    return 1;
}

int maelys_oci_store_default_path(char *output, size_t capacity) {
    if (!output || capacity == 0u) return -1;
    const char *override = getenv(MAELYS_OCI_ENV_STORE);
    const char *base = NULL;
    const char *suffix = NULL;
    if (override && override[0]) {
        base = override;
        suffix = "";
    } else {
        const char *xdg = getenv("XDG_DATA_HOME");
        const char *home = getenv("HOME");
        if (xdg && xdg[0] == '/') {
            base = xdg;
            suffix = "/maelys-oci";
        } else {
            base = home;
            suffix = "/.local/share/maelys-oci";
        }
    }
    return base && base[0] == '/' &&
        oci_snprintf(output, capacity, "%s%s", base, suffix) > 0 ? 0 : -1;
}

int maelys_oci_store_private_directory(const char *path) {
    struct stat status;
    return path && lstat(path, &status) == 0 && S_ISDIR(status.st_mode) &&
        !S_ISLNK(status.st_mode) && status.st_uid == geteuid() &&
        (status.st_mode & 0777) == 0700;
}

/* Walk every component with a held directory descriptor. The only aliases
 * accepted are the OS-owned macOS /tmp and /var entry points. User-created
 * symlinks are never followed, even when they lead to a private final root. */
static int open_owned_directory(
    const char *argument, int create, int private_leaf, char **out_canonical) {
    if (!out_canonical) return -1;
    *out_canonical = NULL;
    if (!argument || argument[0] != '/' || strlen(argument) >= PATH_MAX) return -1;
    char expanded[PATH_MAX];
    if (oci_snprintf(expanded, sizeof(expanded), "%s", argument) < 0) return -1;
#ifdef __APPLE__
    if (strncmp(argument, "/tmp/", 5u) == 0 || strncmp(argument, "/var/", 5u) == 0) {
        char prefix[5];
        memcpy(prefix, argument, 4u); prefix[4] = '\0';
        struct stat alias;
        char *target = realpath(prefix, NULL);
        int valid = lstat(prefix, &alias) == 0 && alias.st_uid == 0 && target &&
            ((strcmp(prefix, "/tmp") == 0 && strcmp(target, "/private/tmp") == 0) ||
             (strcmp(prefix, "/var") == 0 && strcmp(target, "/private/var") == 0));
        int formatted = valid ? oci_snprintf(expanded, sizeof(expanded), "%s%s",
            target, argument + 4u) : -1;
        free(target);
        if (formatted < 0) return -1;
    }
#endif
    size_t size = strlen(expanded);
    while (size > 1u && expanded[size - 1u] == '/') expanded[--size] = '\0';
    char *copy = strdup(expanded), *cursor = NULL;
    int directory = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    int result = copy && directory >= 0 && (size > 1u || !private_leaf) ? 0 : -1;
    char *name = copy ? strtok_r(copy + 1u, "/", &cursor) : NULL;
    while (result == 0 && name) {
        char *next = strtok_r(NULL, "/", &cursor);
        if (!strcmp(name, ".") || !strcmp(name, "..")) { result = -1; break; }
        int child = openat(directory, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (child < 0 && errno == ENOENT && create) {
            if ((mkdirat(directory, name, 0700) != 0 && errno != EEXIST) || fsync(directory) != 0) {
                result = -1; break;
            }
            child = openat(directory, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        }
        struct stat status;
        if (child < 0 || fstat(child, &status) != 0 || !S_ISDIR(status.st_mode) ||
            (status.st_uid != 0 && status.st_uid != geteuid()) ||
            ((status.st_mode & 0022) && !(status.st_uid == 0 && (status.st_mode & S_ISVTX))) ||
            (!next && private_leaf && (status.st_uid != geteuid() || (status.st_mode & 0777) != 0700))) {
            if (child >= 0) (void)maelys_sys_fd_close(&child);
            result = -1; break;
        }
        (void)maelys_sys_fd_close(&directory);
        directory = child;
        name = next;
    }
    if (directory >= 0) (void)maelys_sys_fd_close(&directory);
    free(copy);
    if (result == 0 && !(*out_canonical = strdup(expanded))) result = -1;
    return result;
}

int maelys_oci_store_open_root(const char *argument, int create, char **out_canonical) {
    return open_owned_directory(argument, create, 1, out_canonical);
}

int oci_store_validate_ancestors(const char *path) {
    if (!path || path[0] != '/' || strlen(path) >= PATH_MAX) return -1;
    char parent[PATH_MAX];
    if (oci_snprintf(parent, sizeof(parent), "%s", path) < 0) return -1;
    char *slash = strrchr(parent, '/');
    if (!slash || !slash[1]) return -1;
    if (slash == parent) slash[1] = '\0';
    else *slash = '\0';
    char *canonical = NULL;
    int result = open_owned_directory(parent, 0, 0, &canonical);
    free(canonical);
    return result;
}

int maelys_oci_store_ensure_private_directory(
    const char *parent, const char *name, char *output, size_t capacity) {
    if (!parent || !component_valid(name) || !output || capacity == 0u)
        return -1;
    if (oci_snprintf(output, capacity, "%s/%s", parent, name) < 0) return -1;
    if (mkdir(output, 0700) != 0 && errno != EEXIST) return -1;
    return maelys_oci_store_private_directory(output) ? 0 : -1;
}

int maelys_oci_store_fsync_directory(const char *path) {
    return maelys_sys_directory_sync(path) == MAELYS_SYS_OK ? 0 : -1;
}

void maelys_oci_store_lease_expectations(
    maelys_sys_file_expectations_t *out_expectations) {
    memset(out_expectations, 0, sizeof(*out_expectations));
    out_expectations->check_owner = 1;
    out_expectations->owner = geteuid();
    out_expectations->forbidden_mode_bits =
        (mode_t)((S_IRWXU | S_IRWXG | S_IRWXO) & ~S_IRUSR);
    out_expectations->require_single_link = 1;
    out_expectations->bound_size = 1;
    out_expectations->maximum_size = MAELYS_OCI_LEASE_MAX_BYTES;
}

int maelys_oci_store_lease_probe(
    const char *path, maelys_sys_file_lock_t **out_lock) {
    if (!path || !out_lock) return -1;
    *out_lock = NULL;
    maelys_sys_file_expectations_t expectations;
    maelys_oci_store_lease_expectations(&expectations);
    maelys_sys_file_lock_options_t options = {
        .exclusive = 1, .wait = 0, .create = 0, .writable = 0,
        .expectations = &expectations
    };
    maelys_sys_result_t result =
        maelys_sys_file_lock_acquire(path, &options, out_lock);
    if (result == MAELYS_SYS_OK) return 0;
    return result == MAELYS_SYS_ERR_BUSY ? 1 : -1;
}

int maelys_oci_store_lock_acquire(
    const char *store, const char *name, int exclusive,
    maelys_oci_store_lock_t *out_lock) {
    uint64_t deadline;
    if (maelys_sys_deadline_after(30000u, &deadline) != MAELYS_SYS_OK)
        return -1;
    return maelys_oci_store_lock_until(store, name, exclusive, deadline, out_lock);
}

int maelys_oci_store_lock_until(
    const char *store, const char *name, int exclusive, uint64_t deadline_ms,
    maelys_oci_store_lock_t *out_lock) {
    if (!store || !component_valid(name) || !out_lock) return -1;
    out_lock->handle = NULL;
    char locks[PATH_MAX];
    if (maelys_oci_store_ensure_private_directory(
            store, "locks", locks, sizeof(locks)) != 0) return -1;
    char path[PATH_MAX];
    if (oci_snprintf(path, sizeof(path), "%s/%s", locks, name) < 0) return -1;
    /* The lock file is ours, private and singly linked; maelys-system
     * checks it before and after the lock and re-resolves the path to the
     * locked inode. */
    maelys_sys_file_expectations_t expectations = {0};
    expectations.check_owner = 1;
    expectations.owner = geteuid();
    expectations.forbidden_mode_bits = S_IRWXG | S_IRWXO;
    expectations.require_single_link = 1;
    maelys_sys_file_lock_options_t options = {
        .exclusive = exclusive != 0, .wait = 0, .create = 1, .writable = 0,
        .expectations = &expectations
    };
    for (;;) {
        int expired = 0;
        if (maelys_sys_deadline_expired(deadline_ms, &expired) != MAELYS_SYS_OK ||
            expired) { errno = ETIMEDOUT; return -1; }
        maelys_sys_result_t result = maelys_sys_file_lock_acquire(
            path, &options, &out_lock->handle);
        if (result == MAELYS_SYS_OK) return 0;
        if (result != MAELYS_SYS_ERR_BUSY) return -1;
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 10000000};
        (void)nanosleep(&pause, NULL);
    }
}

void maelys_oci_store_lock_release(maelys_oci_store_lock_t *lock) {
    if (!lock) return;
    (void)maelys_sys_file_lock_release(&lock->handle);
}

static int publication_result(maelys_sys_result_t result) {
    if (result == MAELYS_SYS_OK) return 0;
    if (result == MAELYS_SYS_ERR_EXISTS) return 1;
    return -1;
}

int maelys_oci_store_publish_file_noreplace(
    const char *staging, const char *destination) {
    return publication_result(
        maelys_sys_file_publish_noreplace(staging, destination, NULL));
}

int maelys_oci_store_publish_directory_noreplace(
    const char *staging, const char *destination) {
    return publication_result(
        maelys_sys_directory_publish_noreplace(staging, destination, NULL));
}

int maelys_oci_store_lock_manifest(
    const char *store, const char *manifest_hex,
    maelys_oci_store_lock_t *out_store_lock,
    maelys_oci_store_lock_t *out_manifest_lock) {
    out_store_lock->handle = NULL;
    out_manifest_lock->handle = NULL;
    char lock_name[80];
    if (!oci_digest_hex_valid(manifest_hex) ||
        oci_snprintf(lock_name, sizeof(lock_name), "%s.lock", manifest_hex) < 0 ||
        maelys_oci_store_lock_acquire(store, "store.lock", 0, out_store_lock) != 0)
        return -1;
    if (maelys_oci_store_lock_acquire(store, lock_name, 1, out_manifest_lock) != 0) {
        maelys_oci_store_lock_release(out_store_lock);
        return -1;
    }
    return 0;
}

void maelys_oci_store_unlock_manifest(
    maelys_oci_store_lock_t *store_lock,
    maelys_oci_store_lock_t *manifest_lock) {
    maelys_oci_store_lock_release(manifest_lock);
    maelys_oci_store_lock_release(store_lock);
}

int maelys_oci_store_hash_immutable(
    const char *path, mode_t expected_mode,
    char out_hex[MAELYS_OCI_DIGEST_HEX_SIZE], struct stat *out_status) {
    int descriptor = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    struct stat before;
    if (descriptor < 0 || fstat(descriptor, &before) != 0 ||
        !S_ISREG(before.st_mode) || before.st_size < 0 ||
        before.st_uid != geteuid() ||
        (before.st_mode & 0777) != expected_mode || before.st_nlink != 1) {
        if (descriptor >= 0) (void)maelys_sys_fd_close(&descriptor);
        return -1;
    }
    maelys_oci_sha256_context_t hash;
    maelys_oci_sha256_init(&hash);
    unsigned char buffer[65536];
    uint64_t size = 0u;
    int result = 0;
    for (;;) {
        ssize_t count = read(descriptor, buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 || (count > 0 && (uint64_t)count > (uint64_t)before.st_size - size)) {
            result = -1; break;
        }
        if (!count) break;
        size += (uint64_t)count;
        maelys_oci_sha256_update(&hash, buffer, (size_t)count);
    }
    struct stat after, named;
    if (result != 0 || size != (uint64_t)before.st_size ||
        fstat(descriptor, &after) != 0 || lstat(path, &named) != 0 ||
        named.st_dev != before.st_dev || named.st_ino != before.st_ino ||
        after.st_mode != before.st_mode || after.st_size != before.st_size ||
        after.st_nlink != before.st_nlink || after.st_mtime != before.st_mtime ||
        after.st_ctime != before.st_ctime) result = -1;
    (void)maelys_sys_fd_close(&descriptor);
    if (result != 0) return -1;
    maelys_oci_sha256_finish(&hash, out_hex);
    if (out_status) *out_status = before;
    return 0;
}
