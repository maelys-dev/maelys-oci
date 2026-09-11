/* SPDX-License-Identifier: MPL-2.0 */
#include "src/puller/internal.h"

#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>

static int lower_alphanumeric(unsigned char byte) {
    return (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9');
}

static int repository_valid(const char *repository) {
    size_t length = repository ? strlen(repository) : 0u;
    if (!length || length > 1024u || repository[0] == '/' ||
        repository[length - 1u] == '/') return 0;
    const unsigned char *cursor = (const unsigned char *)repository;
    while (*cursor) {
        const unsigned char *start = cursor;
        if (!lower_alphanumeric(*cursor)) return 0;
        while (*cursor && *cursor != '/') {
            if (lower_alphanumeric(*cursor)) { ++cursor; continue; }
            if (*cursor == '.' || *cursor == '_') {
                unsigned char separator = *cursor++;
                if (separator == '_' && *cursor == '_') ++cursor;
            } else if (*cursor == '-') {
                do ++cursor; while (*cursor == '-');
            } else return 0;
            if (!lower_alphanumeric(*cursor)) return 0;
        }
        if ((size_t)(cursor - start) > 255u) return 0;
        if (*cursor) ++cursor;
    }
    return 1;
}

int authority_valid(const char *authority) {
    size_t length = authority ? strlen(authority) : 0u;
    if (!length || length > 253u) return 0;
    const char *port = NULL;
    if (*authority == '[') {
        const char *end = strchr(authority, ']');
        char address[INET6_ADDRSTRLEN];
        struct in6_addr parsed;
        if (!end || (size_t)(end - authority - 1) >= sizeof(address)) return 0;
        memcpy(address, authority + 1, (size_t)(end - authority - 1));
        address[end - authority - 1] = '\0';
        if (inet_pton(AF_INET6, address, &parsed) != 1) return 0;
        if (end[1] && end[1] != ':') return 0;
        port = end[1] ? end + 2 : NULL;
    } else {
        const char *end = strchr(authority, ':');
        if (!end) end = authority + length;
        else port = end + 1;
        const char *label = authority;
        for (const char *cursor = authority; cursor <= end; ++cursor) {
            if (cursor == end || *cursor == '.') {
                size_t size = (size_t)(cursor - label);
                if (!size || size > 63u || label[0] == '-' ||
                    cursor[-1] == '-') return 0;
                label = cursor + 1;
            } else {
                unsigned char byte = (unsigned char)*cursor;
                if (!lower_alphanumeric(byte) && byte != '-' &&
                    !(byte >= 'A' && byte <= 'Z')) return 0;
            }
        }
    }
    if (port) {
        unsigned value = 0u;
        if (!*port || strlen(port) > 5u) return 0;
        for (; *port; ++port) {
            if (*port < '0' || *port > '9') return 0;
            value = value * 10u + (unsigned)(*port - '0');
        }
        if (!value || value > 65535u) return 0;
    }
    return 1;
}

void reference_clear(pull_reference_t *reference) {
    if (!reference) return;
    free(reference->authority);
    free(reference->repository);
    memset(reference, 0, sizeof(*reference));
}

int reference_parse(const char *text, pull_reference_t *out) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    if (!text || strnlen(text, PULL_TARGET_MAX) >= PULL_TARGET_MAX) return -1;
    const char *separator = text ? strstr(text, "@sha256:") : NULL;
    if (!separator || separator == text || strchr(separator + 1u, '@') ||
        !oci_digest_valid(separator + 1u)) {
        return -1;
    }
    const char *slash = memchr(text, '/', (size_t)(separator - text));
    if (!slash || slash == text || slash + 1u == separator) return -1;
    out->authority = strndup(text, (size_t)(slash - text));
    out->repository = strndup(
        slash + 1u, (size_t)(separator - slash - 1u));
    if (!out->authority || !out->repository ||
        !authority_valid(out->authority) ||
        !repository_valid(out->repository)) {
        reference_clear(out);
        return -1;
    }
    memcpy(out->digest, separator + 1u, OCI_DIGEST_SIZE);
    return 0;
}
