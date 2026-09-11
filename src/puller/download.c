/* SPDX-License-Identifier: MPL-2.0 */
/* Sequential bounded downloads into the store's private staging namespace.
 * A failed transfer leaves verified CAS objects reusable and discards its
 * partial file. No byte ranges, checkpoints, hardlinks or overwrites. */
#include "src/puller/internal.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int store_memory_blob(pull_http_t *http, const oci_descriptor_t *descriptor,
    const unsigned char *bytes, size_t size) {
    if (size != descriptor->size) return -1;
    int cached = oci_store_blob_check(http->store, descriptor);
    if (cached < 0) return -1;
    if (cached) { ++http->cached_blobs; return 0; }
    char temporary[PATH_MAX];
    int fd = oci_store_temporary_file(http->store, temporary);
    if (fd < 0) return -1;
    int result = oci_write_all(fd, bytes, size);
    if (maelys_sys_fd_close(&fd) != MAELYS_SYS_OK) result = -1;
    if (!result) result = oci_store_blob_publish(http->store, descriptor, temporary);
    if (!result) ++http->downloaded_blobs;
    if (result) (void)unlink(temporary);
    return result;
}

int fetch_file_blob(pull_http_t *http, const pull_reference_t *reference,
    const oci_descriptor_t *descriptor) {
    int cached = oci_store_blob_check(http->store, descriptor);
    if (cached < 0) {
        pull_report(http, OCI_ERROR_STATE, "existing CAS blob is unsafe or corrupt");
        return -1;
    }
    if (cached) { ++http->cached_blobs; return 0; }
    char target[PULL_TARGET_MAX], temporary[PATH_MAX];
    if (oci_snprintf(target, sizeof(target), "/v2/%s/blobs/%s",
            reference->repository, descriptor->digest) < 0) return -1;
    int fd = oci_store_temporary_file(http->store, temporary);
    if (fd < 0) return -1;
    pull_headers_t headers = {0};
    pull_body_t body = {.maximum = descriptor->size, .fd = fd,
        .expected_size = descriptor->size, .hash_active = 1};
    maelys_oci_sha256_init(&body.hash);
    int result = registry_get(http, reference, target, NULL, &headers, &body);
    char digest[OCI_DIGEST_HEX_SIZE];
    maelys_oci_sha256_finish(&body.hash, digest);
    if (headers.status != 200u || body.received != descriptor->size ||
        strcmp(digest, descriptor->digest + OCI_DIGEST_PREFIX_SIZE) != 0 ||
        (headers.content_digest &&
         strcmp(headers.content_digest, descriptor->digest) != 0)) result = -1;
    body.fd = -1;
    if (maelys_sys_fd_close(&fd) != MAELYS_SYS_OK) result = -1;
    if (!result) result = oci_store_blob_publish(http->store, descriptor, temporary);
    if (!result) ++http->downloaded_blobs;
    if (result) (void)unlink(temporary);
    headers_clear(&headers);
    body_clear(&body);
    return result;
}
