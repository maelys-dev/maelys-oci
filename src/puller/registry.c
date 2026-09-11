/* SPDX-License-Identifier: MPL-2.0 */
#include "src/puller/internal.h"

#include <stdlib.h>
#include <string.h>

int registry_get(
    pull_http_t *http, const pull_reference_t *reference,
    const char *target, const char *accept,
    pull_headers_t *headers, pull_body_t *body) {
    const char *authorization = http->bearer_authorization ?
        http->bearer_authorization : http->basic_authorization;
    headers_clear(headers);
    if (body_reset(body) != 0) return -1;
    if (http_get_once(http, reference->authority, target, accept,
                      authorization, headers, body) != 0) return -1;
    if (headers->status != 401u) return 0;
    if (http->cross_authority) {
        pull_report(http, OCI_ERROR_ACCESS,
            "refusing an authentication challenge after a cross-authority redirect");
        return -1;
    }
    if (!headers->www_authenticate || acquire_bearer_token(
            http, headers->www_authenticate, reference) != 0) {
        if (http->helper_credentials_present &&
            !http->basic_authorization && !http->bearer_authorization) {
            pull_report(http, OCI_ERROR_UNSUPPORTED,
                "the Docker credential helper is intentionally unsupported; "
                "use --token-file");
        }
        return -1;
    }
    headers_clear(headers);
    if (body_reset(body) != 0) return -1;
    return http_get_once(http, reference->authority, target, accept,
                         http->bearer_authorization, headers, body);
}

int fetch_memory(
    pull_http_t *http, const pull_reference_t *reference,
    const char *target, const char *accept, const char *expected_digest,
    size_t maximum, pull_headers_t *out_headers,
    unsigned char **out_bytes, size_t *out_size) {
    *out_bytes = NULL;
    *out_size = 0u;
    if (http->store && expected_digest) {
        int cached = oci_store_blob_read(http->store, expected_digest, maximum,
            out_bytes, out_size);
        if (cached < 0) {
            pull_report(http, OCI_ERROR_STATE, "existing CAS object is unsafe or corrupt");
            return -1;
        }
        if (cached) {
            char media[OCI_MEDIA_TYPE_SIZE] = {0};
            if (accept) {
                maelys_json_document_t *document = oci_json_parse_object(
                    *out_bytes, *out_size, OCI_JSON_TOKENS_MAX);
                if (document) (void)oci_json_copy_string(document,
                    maelys_json_document_root(document), "mediaType", media,
                    sizeof(media), 0);
                maelys_json_document_release(document);
            }
            if (!accept || media[0]) {
                out_headers->status = 200u;
                out_headers->content_type = accept ? strdup(media) : NULL;
                if (!accept || out_headers->content_type) return 0;
            }
            free(*out_bytes); *out_bytes = NULL; *out_size = 0u;
        }
    }
    pull_body_t body = {.maximum = maximum, .fd = -1, .hash_active = 1};
    maelys_oci_sha256_init(&body.hash);
    if (registry_get(http, reference, target, accept,
                     out_headers, &body) != 0 ||
        out_headers->status != 200u || !body.received) {
        body_clear(&body);
        return -1;
    }
    char digest[OCI_DIGEST_HEX_SIZE];
    maelys_oci_sha256_finish(&body.hash, digest);
    if ((expected_digest &&
         strcmp(digest, expected_digest + OCI_DIGEST_PREFIX_SIZE) != 0) ||
        (out_headers->content_digest && expected_digest &&
         strcmp(out_headers->content_digest, expected_digest) != 0)) {
        body_clear(&body);
        return -1;
    }
    *out_bytes = body.bytes;
    *out_size = body.size;
    body.bytes = NULL;
    body_clear(&body);
    return 0;
}

int fetch_manifest_document(
    pull_http_t *http, const pull_reference_t *reference,
    const char *digest, unsigned char **out_bytes, size_t *out_size,
    pull_headers_t *out_headers) {
    static const char accept[] =
        "application/vnd.oci.image.index.v1+json, "
        "application/vnd.oci.image.manifest.v1+json, "
        "application/vnd.docker.distribution.manifest.list.v2+json, "
        "application/vnd.docker.distribution.manifest.v2+json";
    char target[PULL_TARGET_MAX];
    if (oci_snprintf(target, sizeof(target), "/v2/%s/manifests/%s",
            reference->repository, digest) < 0)
        return -1;
    return fetch_memory(http, reference, target, accept, digest,
                        OCI_JSON_MAX, out_headers, out_bytes, out_size);
}
