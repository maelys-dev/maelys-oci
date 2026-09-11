/* SPDX-License-Identifier: MPL-2.0 */
#include "src/common/internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static uint32_t rotate_right(uint32_t value, unsigned int count) {
    return (value >> count) | (value << (32u - count));
}

static uint32_t load_be32(const unsigned char *bytes) {
    return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) |
           ((uint32_t)bytes[2] << 8) | (uint32_t)bytes[3];
}

static void store_be32(unsigned char *bytes, uint32_t value) {
    bytes[0] = (unsigned char)(value >> 24);
    bytes[1] = (unsigned char)(value >> 16);
    bytes[2] = (unsigned char)(value >> 8);
    bytes[3] = (unsigned char)value;
}

static void transform(maelys_oci_sha256_context_t *context, const unsigned char block[64]) {
    static const uint32_t constants[64] = {
        0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
        0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
        0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
        0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
        0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
        0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
        0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
        0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
    };
    uint32_t words[64];
    for (size_t i = 0; i < 16u; ++i) words[i] = load_be32(block + i * 4u);
    for (size_t i = 16u; i < 64u; ++i) {
        uint32_t s0 = rotate_right(words[i - 15u], 7u) ^ rotate_right(words[i - 15u], 18u) ^
                      (words[i - 15u] >> 3u);
        uint32_t s1 = rotate_right(words[i - 2u], 17u) ^ rotate_right(words[i - 2u], 19u) ^
                      (words[i - 2u] >> 10u);
        words[i] = words[i - 16u] + s0 + words[i - 7u] + s1;
    }
    uint32_t a = context->state[0], b = context->state[1], c = context->state[2], d = context->state[3];
    uint32_t e = context->state[4], f = context->state[5], g = context->state[6], h = context->state[7];
    for (size_t i = 0; i < 64u; ++i) {
        uint32_t sigma1 = rotate_right(e, 6u) ^ rotate_right(e, 11u) ^ rotate_right(e, 25u);
        uint32_t choice = (e & f) ^ ((~e) & g);
        uint32_t first = h + sigma1 + choice + constants[i] + words[i];
        uint32_t sigma0 = rotate_right(a, 2u) ^ rotate_right(a, 13u) ^ rotate_right(a, 22u);
        uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        uint32_t second = sigma0 + majority;
        h = g; g = f; f = e; e = d + first; d = c; c = b; b = a; a = first + second;
    }
    context->state[0] += a; context->state[1] += b; context->state[2] += c; context->state[3] += d;
    context->state[4] += e; context->state[5] += f; context->state[6] += g; context->state[7] += h;
}

void maelys_oci_sha256_init(maelys_oci_sha256_context_t *context) {
    memset(context, 0, sizeof(*context));
    const uint32_t initial[8] = {
        0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,
        0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u
    };
    memcpy(context->state, initial, sizeof(initial));
}

void maelys_oci_sha256_update(
    maelys_oci_sha256_context_t *context,
    const unsigned char *bytes,
    size_t length) {
    context->bit_count += (uint64_t)length * 8u;
    while (length > 0u) {
        size_t available = sizeof(context->block) - context->block_length;
        size_t count = length < available ? length : available;
        memcpy(context->block + context->block_length, bytes, count);
        context->block_length += count;
        bytes += count;
        length -= count;
        if (context->block_length == sizeof(context->block)) {
            transform(context, context->block);
            context->block_length = 0u;
        }
    }
}

void maelys_oci_sha256_finish(
    maelys_oci_sha256_context_t *context,
    char out_hex[MAELYS_OCI_DIGEST_HEX_SIZE]) {
    unsigned char pad[128] = {0x80u};
    size_t pad_length = context->block_length < 56u
        ? 56u - context->block_length : 120u - context->block_length;
    uint64_t original_bits = context->bit_count;
    maelys_oci_sha256_update(context, pad, pad_length);
    unsigned char length_bytes[8];
    for (size_t i = 0; i < 8u; ++i) length_bytes[7u - i] = (unsigned char)(original_bits >> (i * 8u));
    maelys_oci_sha256_update(context, length_bytes, sizeof(length_bytes));
    unsigned char digest[32];
    for (size_t i = 0; i < 8u; ++i) store_be32(digest + i * 4u, context->state[i]);
    oci_bytes_to_hex(digest, sizeof(digest), out_hex);
}

void maelys_oci_sha256_hex(const void *data, size_t length, char out_hex[MAELYS_OCI_DIGEST_HEX_SIZE]) {
    maelys_oci_sha256_context_t context;
    maelys_oci_sha256_init(&context);
    maelys_oci_sha256_update(&context, data, length);
    maelys_oci_sha256_finish(&context, out_hex);
}

maelys_oci_result_t maelys_oci_sha256_file(
    const char *path,
    char out_hex[MAELYS_OCI_DIGEST_HEX_SIZE],
    char **out_error) {
    if (!path || !out_hex) return MAELYS_OCI_ERR_ARGUMENT;
    struct stat before;
    if (lstat(path, &before) != 0 || !S_ISREG(before.st_mode)) {
        maelys_oci_set_error(out_error, "cannot safely hash %s: %s",
            path, errno ? strerror(errno) : "not a regular file");
        return MAELYS_OCI_ERR_IO;
    }
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        maelys_oci_set_error(out_error, "cannot open %s for hashing: %s",
            path, strerror(errno));
        return MAELYS_OCI_ERR_IO;
    }
    struct stat opened;
    if (fstat(fd, &opened) != 0 || opened.st_dev != before.st_dev ||
        opened.st_ino != before.st_ino || !S_ISREG(opened.st_mode)) {
        maelys_oci_set_error(out_error,
            "file identity changed while opening %s for hashing", path);
        (void)maelys_sys_fd_close(&fd);
        return MAELYS_OCI_ERR_IO;
    }
    maelys_oci_sha256_context_t context;
    maelys_oci_sha256_init(&context);
    unsigned char buffer[64u * 1024u];
    maelys_oci_result_t result = MAELYS_OCI_OK;
    for (;;) {
        ssize_t count = read(fd, buffer, sizeof(buffer));
        if (count > 0) {
            maelys_oci_sha256_update(&context, buffer, (size_t)count);
            continue;
        }
        if (count == 0) break;
        if (errno == EINTR) continue;
        maelys_oci_set_error(out_error, "cannot hash %s: %s",
            path, strerror(errno));
        result = MAELYS_OCI_ERR_IO;
        break;
    }
    if (maelys_sys_fd_close(&fd) != MAELYS_SYS_OK &&
        result == MAELYS_OCI_OK) {
        maelys_oci_set_error(out_error, "cannot close %s after hashing", path);
        result = MAELYS_OCI_ERR_IO;
    }
    if (result == MAELYS_OCI_OK) maelys_oci_sha256_finish(&context, out_hex);
    return result;
}
