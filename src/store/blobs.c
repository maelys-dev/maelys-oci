/* SPDX-License-Identifier: MPL-2.0 */
/* Local CAS publication. Callers hold the shared store lock throughout. */
#include "src/store/core.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int oci_store_files_equal(const char *left, const char *right) {
    int a = open(left, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    int b = open(right, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    if (a < 0 || b < 0) {
        if (a >= 0) (void)maelys_sys_fd_close(&a);
        if (b >= 0) (void)maelys_sys_fd_close(&b);
        return 0;
    }
    struct stat sa;
    struct stat sb;
    int equal = fstat(a, &sa) == 0 && fstat(b, &sb) == 0 &&
        S_ISREG(sa.st_mode) && S_ISREG(sb.st_mode) &&
        sa.st_size == sb.st_size;
    unsigned char left_bytes[64u * 1024u];
    unsigned char right_bytes[64u * 1024u];
    while (equal) {
        ssize_t left_size;
        do left_size = read(a, left_bytes, sizeof(left_bytes));
        while (left_size < 0 && errno == EINTR);
        ssize_t right_size;
        do right_size = read(b, right_bytes, sizeof(right_bytes));
        while (right_size < 0 && errno == EINTR);
        if (left_size < 0 || right_size < 0 || left_size != right_size ||
            (left_size > 0 && memcmp(left_bytes, right_bytes,
                                     (size_t)left_size) != 0)) {
            equal = 0;
            break;
        }
        if (left_size == 0) break;
    }
    (void)maelys_sys_fd_close(&a);
    (void)maelys_sys_fd_close(&b);
    return equal;
}

int oci_store_random_id(char out[33]) {
    unsigned char bytes[16];
    int fd = open("/dev/urandom", O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    size_t used = 0u;
    while (used < sizeof(bytes)) {
        ssize_t n = read(fd, bytes + used, sizeof(bytes) - used);
        if (n > 0) used += (size_t)n;
        else if (n < 0 && errno == EINTR) continue;
        else break;
    }
    (void)maelys_sys_fd_close(&fd);
    if (used != sizeof(bytes)) return -1;
    oci_bytes_to_hex(bytes, sizeof(bytes), out);
    return 0;
}

int oci_store_temporary_file(const char *store, char path[PATH_MAX]) {
    char temporary[PATH_MAX], imports[PATH_MAX], id[33];
    if (oci_snprintf(temporary, sizeof(temporary), "%s/tmp", store) < 0 ||
        oci_snprintf(imports, sizeof(imports), "%s/import", temporary) < 0 ||
        !maelys_oci_store_private_directory(temporary) ||
        !maelys_oci_store_private_directory(imports) ||
        oci_store_random_id(id) != 0 ||
        oci_snprintf(path, PATH_MAX, "%s/pull.%s", imports, id) < 0) return -1;
    return open(path, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
}

int oci_store_blob_path(const char *store, const char *digest, char path[PATH_MAX]) {
    char blobs[PATH_MAX], sha256[PATH_MAX];
    return store && oci_digest_valid(digest) &&
        oci_snprintf(blobs, sizeof(blobs), "%s/blobs", store) >= 0 &&
        oci_snprintf(sha256, sizeof(sha256), "%s/sha256", blobs) >= 0 &&
        maelys_oci_store_private_directory(blobs) &&
        maelys_oci_store_private_directory(sha256) &&
        oci_snprintf(path, PATH_MAX, "%s/%s", sha256,
            digest + OCI_DIGEST_PREFIX_SIZE) >= 0 ? 0 : -1;
}

int oci_store_blob_check(const char *store, const oci_descriptor_t *descriptor) {
    char path[PATH_MAX], digest[OCI_DIGEST_HEX_SIZE];
    struct stat status;
    if (oci_store_blob_path(store, descriptor->digest, path) != 0) return -1;
    if (lstat(path, &status) != 0) return errno == ENOENT ? 0 : -1;
    if (status.st_size < 0 || (uint64_t)status.st_size != descriptor->size) return -1;
    return maelys_oci_store_hash_immutable(path, 0400, digest, &status) == 0 &&
        strcmp(digest, descriptor->digest + OCI_DIGEST_PREFIX_SIZE) == 0 &&
        (uint64_t)status.st_size == descriptor->size ? 1 : -1;
}

int oci_store_blob_publish(const char *store,
    const oci_descriptor_t *descriptor, const char *staging) {
    char destination[PATH_MAX], digest[OCI_DIGEST_HEX_SIZE], parent[PATH_MAX];
    struct stat status;
    int fd = open(staging, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    int valid = fd >= 0 && fstat(fd, &status) == 0 &&
        S_ISREG(status.st_mode) && status.st_uid == geteuid() &&
        status.st_nlink == 1 && fchmod(fd, 0400) == 0 &&
        maelys_sys_file_sync(fd) == MAELYS_SYS_OK;
    if (fd >= 0) (void)maelys_sys_fd_close(&fd);
    if (!valid || maelys_oci_store_hash_immutable(staging, 0400, digest, &status) != 0 ||
        strcmp(digest, descriptor->digest + OCI_DIGEST_PREFIX_SIZE) != 0 ||
        (uint64_t)status.st_size != descriptor->size ||
        oci_store_blob_path(store, descriptor->digest, destination) != 0 ||
        oci_snprintf(parent, sizeof(parent), "%s/blobs/sha256", store) < 0)
        return -1;
    int published = maelys_oci_store_publish_file_noreplace(staging, destination);
    if (published == 1) {
        if (oci_store_blob_check(store, descriptor) != 1 ||
            !oci_store_files_equal(staging, destination) || unlink(staging) != 0)
            return -1;
    } else if (published != 0) return -1;
    return maelys_oci_store_fsync_directory(parent);
}

int oci_store_blob_read(const char *store, const char *digest, size_t maximum,
    unsigned char **out_bytes, size_t *out_size) {
    *out_bytes = NULL;
    *out_size = 0u;
    char path[PATH_MAX], observed[OCI_DIGEST_HEX_SIZE];
    struct stat status;
    if (oci_store_blob_path(store, digest, path) != 0) return -1;
    if (lstat(path, &status) != 0) return errno == ENOENT ? 0 : -1;
    if (status.st_size < 0 || (uint64_t)status.st_size > maximum ||
        maelys_oci_store_hash_immutable(path, 0400, observed, &status) != 0 ||
        strcmp(observed, digest + OCI_DIGEST_PREFIX_SIZE) != 0) return -1;
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    int result = fd >= 0 ? oci_read_regular_bounded(fd, maximum, out_bytes, out_size) : -1;
    if (fd >= 0) (void)maelys_sys_fd_close(&fd);
    if (!result) {
        maelys_oci_sha256_hex(*out_bytes, *out_size, observed);
        if (strcmp(observed, digest + OCI_DIGEST_PREFIX_SIZE) != 0 ||
            *out_size != (uint64_t)status.st_size) result = -1;
    }
    if (result) { free(*out_bytes); *out_bytes = NULL; *out_size = 0u; return -1; }
    return 1;
}
