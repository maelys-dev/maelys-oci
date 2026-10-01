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
    int helper_only = http->helper_credentials_present &&
        !http->basic_authorization && !http->bearer_authorization;
    if (!headers->www_authenticate || acquire_bearer_token(
            http, headers->www_authenticate, reference) != 0) {
        if (helper_only) {
            pull_report(http, OCI_ERROR_UNSUPPORTED,
                "the Docker credential helper is intentionally unsupported; "
                "use --token-file");
        } else if (!headers->www_authenticate) {
            pull_report(http, OCI_ERROR_ACCESS,
                "registry %s answered HTTP 401 without a challenge; %s",
                reference->authority, authorization ?
                "the credentials given were refused" :
                "no credentials were given");
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
    http->cross_authority = 0;
    http->final_authority[0] = '\0';
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
                out_headers->from_cache = 1;
                out_headers->content_type = accept ? strdup(media) : NULL;
                if (!accept || out_headers->content_type) return 0;
            }
            free(*out_bytes); *out_bytes = NULL; *out_size = 0u;
        }
    }
    pull_body_t body = {.maximum = maximum, .fd = -1, .hash_active = 1};
    maelys_oci_sha256_init(&body.hash);
    if (registry_get(http, reference, target, accept,
                     out_headers, &body) != 0) {
        body_clear(&body);
        return -1;
    }
    if (out_headers->status != 200u) {
        /* The status is the registry's answer: name it, with the object
         * asked for. A manifest answered by another host after a redirect
         * is named by the caller, with that host. */
        unsigned status = out_headers->status;
        if (!(accept && http->cross_authority && http->final_authority[0]))
            pull_report(http,
                status == 404u ? OCI_ERROR_NOT_FOUND :
                status == 401u || status == 403u ? OCI_ERROR_ACCESS :
                status == 429u || status >= 500u ? OCI_ERROR_IO :
                OCI_ERROR_PROTOCOL,
                "registry %s answered HTTP %u for %s%s", reference->authority,
                status, target,
                status == 404u ? "; the repository, tag or digest does not "
                    "exist there" :
                status == 401u || status == 403u ? "; the credentials do "
                    "not grant pull on this repository" : "");
        body_clear(&body);
        return -1;
    }
    if (!body.received) {
        pull_report(http, OCI_ERROR_PROTOCOL,
            "registry %s answered an empty body for %s",
            reference->authority, target);
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

static const char manifest_accept[] =
        "application/vnd.oci.image.index.v1+json, "
        "application/vnd.oci.image.manifest.v1+json, "
        "application/vnd.docker.distribution.manifest.list.v2+json, "
        "application/vnd.docker.distribution.manifest.v2+json";

int fetch_manifest_document(
    pull_http_t *http, const pull_reference_t *reference,
    const char *digest, unsigned char **out_bytes, size_t *out_size,
    pull_headers_t *out_headers) {
    char target[PULL_TARGET_MAX];
    if (oci_snprintf(target, sizeof(target), "/v2/%s/manifests/%s",
            reference->repository, digest) < 0)
        return -1;
    return fetch_memory(http, reference, target, manifest_accept, digest,
                        OCI_JSON_MAX, out_headers, out_bytes, out_size);
}

int fetch_manifest_tag(
    pull_http_t *http, const pull_reference_t *reference,
    const char *tag, unsigned char **out_bytes, size_t *out_size,
    pull_headers_t *out_headers) {
    char target[PULL_TARGET_MAX];
    if (oci_snprintf(target, sizeof(target), "/v2/%s/manifests/%s",
            reference->repository, tag) < 0)
        return -1;
    /* No expected digest: a tag names no content. The caller hashes what it
     * received and compares that with the registry's claim. */
    return fetch_memory(http, reference, target, manifest_accept, NULL,
                        OCI_JSON_MAX, out_headers, out_bytes, out_size);
}
