/* SPDX-License-Identifier: MPL-2.0 */
#include "src/store/seal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int maelys_oci_seal_format(
    const maelys_oci_seal_t *seal, char *buffer, size_t capacity) {
    return oci_snprintf(buffer, capacity,
        MAELYS_OCI_ARTIFACT_SEAL_MAGIC "\n"
        "manifest=%s\n"
        "config=%s\n"
        "platform=%s\n"
        "root=%s\n"
        "root-size=%llu\n"
        "rootfs-tar=%s\n"
        "rootfs-tar-size=%llu\n"
        MAELYS_OCI_MATERIALIZER_SEAL_LINE,
        seal->manifest, seal->config, seal->platform, seal->root,
        (unsigned long long)seal->root_size, seal->rootfs_tar,
        (unsigned long long)seal->rootfs_tar_size);
}

/* Consumes "PREFIX" at the start of line and copies the digest that follows
 * into out when it is a well-formed sha256 reference. */
static int take_digest(const char *line, const char *prefix, char out[OCI_DIGEST_SIZE]) {
    size_t prefix_size = strlen(prefix);
    if (strncmp(line, prefix, prefix_size) != 0 ||
        !oci_digest_valid(line + prefix_size)) return -1;
    memcpy(out, line + prefix_size, OCI_DIGEST_SIZE);
    return 0;
}

static int take_size(const char *line, const char *prefix, uint64_t *out) {
    size_t prefix_size = strlen(prefix);
    if (strncmp(line, prefix, prefix_size) != 0) return -1;
    const char *digits = line + prefix_size;
    if (!digits[0] || strlen(digits) > 20u) return -1;
    uint64_t value = 0u;
    for (const char *cursor = digits; *cursor; ++cursor) {
        if (*cursor < '0' || *cursor > '9' ||
            value > (UINT64_MAX - (uint64_t)(*cursor - '0')) / 10u) return -1;
        value = value * 10u + (uint64_t)(*cursor - '0');
    }
    *out = value;
    return 0;
}

static int take_platform(const char *line, char out[OCI_PLATFORM_SIZE]) {
    static const char prefix[] = "platform=";
    size_t prefix_size = sizeof(prefix) - 1u;
    if (strncmp(line, prefix, prefix_size) != 0 ||
        strlen(line + prefix_size) >= OCI_PLATFORM_SIZE) return -1;
    char directory[OCI_PLATFORM_SIZE];
    if (oci_platform_to_directory(line + prefix_size, directory) != 0) return -1;
    memcpy(out, line + prefix_size, strlen(line + prefix_size) + 1u);
    return 0;
}

int maelys_oci_seal_parse(const char *path, maelys_oci_seal_t *out) {
    if (!path || !out) return -1;
    memset(out, 0, sizeof(*out));
    int descriptor = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) return -1;
    unsigned char *bytes = NULL;
    size_t size = 0u;
    int read_result = oci_read_regular_bounded(
        descriptor, MAELYS_OCI_SEAL_MAX_BYTES, &bytes, &size);
    (void)maelys_sys_fd_close(&descriptor);
    if (read_result != 0) return -1;
    char *text = malloc(size + 1u);
    if (!text) {
        free(bytes);
        return -1;
    }
    memcpy(text, bytes, size);
    text[size] = '\0';
    free(bytes);
    int result = -1;
    if (size == 0u || text[size - 1u] != '\n' || strlen(text) != size) goto done;
    size_t newlines = 0u;
    for (size_t i = 0u; i < size; ++i) if (text[i] == '\n') ++newlines;
    if (newlines != 9u) goto done;
    const char *lines[9];
    char *cursor = text;
    for (size_t i = 0u; i < 9u; ++i) {
        lines[i] = cursor;
        char *newline = strchr(cursor, '\n');
        *newline = '\0';
        cursor = newline + 1u;
    }
    if (strcmp(lines[0], MAELYS_OCI_ARTIFACT_SEAL_MAGIC) != 0 ||
        take_digest(lines[1], "manifest=", out->manifest) != 0 ||
        take_digest(lines[2], "config=", out->config) != 0 ||
        take_platform(lines[3], out->platform) != 0 ||
        take_digest(lines[4], "root=", out->root) != 0 ||
        take_size(lines[5], "root-size=", &out->root_size) != 0 ||
        take_digest(lines[6], "rootfs-tar=", out->rootfs_tar) != 0 ||
        take_size(lines[7], "rootfs-tar-size=", &out->rootfs_tar_size) != 0 ||
        strcmp(lines[8], "materializer=" MAELYS_OCI_MATERIALIZER_ID) != 0)
        goto done;
    result = 0;
done:
    if (result != 0) memset(out, 0, sizeof(*out));
    free(text);
    return result;
}
