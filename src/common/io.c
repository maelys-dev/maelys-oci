/* SPDX-License-Identifier: MPL-2.0 */
#include "src/common/internal.h"

#include <maelys/sys/file.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int oci_snprintf(char *buffer, size_t capacity, const char *format, ...) {
    if (!buffer || capacity == 0u) return -1;
    va_list arguments;
    va_start(arguments, format);
    int written = vsnprintf(buffer, capacity, format, arguments);
    va_end(arguments);
    if (written < 0 || (size_t)written >= capacity) {
        buffer[0] = '\0';
        return -1;
    }
    return written;
}

int oci_hex_valid(const char *text, size_t digit_count) {
    if (!text) return 0;
    for (size_t i = 0u; i < digit_count; ++i) {
        char digit = text[i];
        if (!((digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f')))
            return 0;
    }
    return text[digit_count] == '\0';
}

int oci_directory_entry_is_dot(const char *name) {
    return name && name[0] == '.' &&
        (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'));
}

int oci_write_all(int descriptor, const void *bytes, size_t size) {
    const unsigned char *cursor = bytes;
    size_t offset = 0u;
    while (offset < size) {
        ssize_t amount = write(descriptor, cursor + offset, size - offset);
        if (amount > 0) offset += (size_t)amount;
        else if (amount < 0 && errno == EINTR) continue;
        else return -1;
    }
    return 0;
}

int oci_read_regular_bounded(
    int descriptor, size_t maximum, unsigned char **out_bytes,
    size_t *out_size) {
    /* A regular file no larger than maximum, then read within that size;
     * a file that grows meanwhile exceeds the buffer and is refused, a file
     * that shrinks is refused as well: the caller wants what it measured. */
    maelys_sys_file_expectations_t expectations = {0};
    expectations.bound_size = 1;
    expectations.maximum_size = maximum;
    maelys_sys_file_identity_t identity;
    if (maelys_sys_file_verify(descriptor, &expectations, &identity, NULL) !=
        MAELYS_SYS_OK) return -1;
    size_t size = (size_t)identity.size;
    unsigned char *bytes = malloc(size ? size : 1u);
    if (!bytes) return -1;
    size_t got = 0u;
    if (maelys_sys_file_read_bounded(descriptor, bytes, size, &got) !=
        MAELYS_SYS_OK || got != size) {
        free(bytes);
        return -1;
    }
    *out_bytes = bytes;
    *out_size = size;
    return 0;
}

int oci_write_file_exclusive(
    const char *path, const void *bytes, size_t size, mode_t final_mode) {
    return maelys_sys_file_write_exclusive(path, bytes, size, final_mode) ==
        MAELYS_SYS_OK ? 0 : -1;
}

void oci_bytes_to_hex(const unsigned char *bytes, size_t size, char *out_hex) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0u; i < size; ++i) {
        out_hex[i * 2u] = digits[bytes[i] >> 4u];
        out_hex[i * 2u + 1u] = digits[bytes[i] & 0x0fu];
    }
    out_hex[size * 2u] = '\0';
}

/* Streams at most maximum bytes from input to output while hashing them. */
int oci_copy_fd_hashed(
    int input, int output, uint64_t maximum,
    uint64_t *out_size, char out_digest[OCI_DIGEST_HEX_SIZE]) {
    maelys_oci_sha256_context_t hash;
    maelys_oci_sha256_init(&hash);
    uint64_t total = 0u;
    unsigned char buffer[64u * 1024u];
    for (;;) {
        uint64_t remaining = maximum - total;
        size_t wanted = remaining < sizeof(buffer)
            ? (size_t)remaining + 1u : sizeof(buffer);
        ssize_t amount = read(input, buffer, wanted);
        if (amount < 0 && errno == EINTR) continue;
        if (amount < 0) return -1;
        if (amount == 0) break;
        if ((uint64_t)amount > remaining) {
            errno = EFBIG;
            return -1;
        }
        total += (uint64_t)amount;
        maelys_oci_sha256_update(&hash, buffer, (size_t)amount);
        if (oci_write_all(output, buffer, (size_t)amount) != 0) return -1;
    }
    maelys_oci_sha256_finish(&hash, out_digest);
    *out_size = total;
    return 0;
}
