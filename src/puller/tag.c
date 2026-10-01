/* SPDX-License-Identifier: MPL-2.0 */
/* Read-only resolution of a mutable tag into the immutable digest every other
 * command requires. The registry's Docker-Content-Digest header is never
 * believed: the digest reported is the SHA-256 of the bytes received, and a
 * header that disagrees fails the resolution. Nothing is written, no store is
 * opened, no blob is fetched. */
#include "src/puller/internal.h"
#include "src/materializer/internal.h"
#include <stdlib.h>
#include <string.h>

/* The OCI tag grammar: [A-Za-z0-9_][A-Za-z0-9._-]{0,127}. */
static int tag_valid(const char *tag) {
    size_t length = tag ? strlen(tag) : 0u;
    if (!length || length > 128u) return 0;
    unsigned char first = (unsigned char)tag[0];
    if (!((first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z') ||
          (first >= '0' && first <= '9') || first == '_')) return 0;
    for (size_t i = 1u; i < length; ++i) {
        unsigned char byte = (unsigned char)tag[i];
        if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
            (byte >= '0' && byte <= '9') || byte == '.' || byte == '_' ||
            byte == '-') continue;
        return 0;
    }
    return 1;
}

int reference_parse_tagged(const char *text, pull_reference_t *out,
    char out_tag[PULL_TAG_SIZE]) {
    if (!out || !out_tag) return -1;
    memset(out, 0, sizeof(*out));
    out_tag[0] = '\0';
    if (!text || strnlen(text, PULL_TARGET_MAX) >= PULL_TARGET_MAX) return -1;
    if (strchr(text, '@')) return -1;
    /* The tag is after the last colon, which must follow the last slash:
     * a colon before it belongs to the authority's port. */
    const char *slash = strrchr(text, '/');
    const char *colon = strrchr(text, ':');
    if (!slash || !colon || colon < slash || colon == text) return -1;
    if (strlen(colon + 1u) >= PULL_TAG_SIZE || !tag_valid(colon + 1u)) return -1;
    const char *first_slash = memchr(text, '/', (size_t)(colon - text));
    if (!first_slash || first_slash == text || first_slash + 1u == colon)
        return -1;
    out->authority = strndup(text, (size_t)(first_slash - text));
    out->repository = strndup(first_slash + 1u,
        (size_t)(colon - first_slash - 1u));
    if (!out->authority || !out->repository ||
        !authority_valid(out->authority) ||
        !repository_valid(out->repository)) {
        reference_clear(out);
        return -1;
    }
    memcpy(out_tag, colon + 1u, strlen(colon + 1u) + 1u);
    return 0;
}

/* The variant is written only when the descriptor carries one: a member
 * whose value is NULL fails the whole object, which silently dropped every
 * platform without a variant — amd64 among them. */
static oci_document_t *platform_record(const oci_descriptor_t *descriptor) {
    char platform[OCI_PLATFORM_SIZE];
    if (!descriptor->os[0] || oci_snprintf(platform, sizeof(platform), "%s/%s",
            descriptor->os, descriptor->architecture) < 0)
        return NULL;
    oci_document_t *record = OCI_DOCUMENT_OBJECT(
        {"platform", oci_document_string(platform)},
        {"digest", oci_document_string(descriptor->digest)},
        {"mediaType", oci_document_string(descriptor->media_type)},
        {"supported", oci_document_boolean(
            oci_platform_parts_supported(descriptor->os, descriptor->architecture) &&
            oci_platform_variant_supported(descriptor->architecture, descriptor->variant))});
    if (record && descriptor->variant[0] &&
        oci_document_put(record, "variant",
            oci_document_string(descriptor->variant)) != 0) {
        oci_document_release(record);
        return NULL;
    }
    return record;
}

int oci_resolve_tag(const pull_options_t *options, oci_document_t **out_document,
    oci_error_t *error) {
    if (!out_document) return -1;
    *out_document = NULL;
    if (!options || (options->token_file && options->docker_config)) {
        oci_error_report(error, OCI_ERROR_ARGUMENT, "invalid resolve options");
        return -1;
    }
    pull_reference_t reference;
    char tag[PULL_TAG_SIZE];
    if (reference_parse_tagged(options->reference, &reference, tag) != 0) {
        oci_error_report(error, OCI_ERROR_ARGUMENT,
            "REFERENCE must be REGISTRY/REPOSITORY:TAG");
        return -1;
    }
    pull_http_t http = {0};
    unsigned char *bytes = NULL;
    size_t size = 0u;
    pull_headers_t headers = {0};
    oci_descriptor_t *children = NULL;
    oci_document_t *document = NULL;
    oci_document_t *platforms = NULL;
    int result = http_initialize(&http, options, &reference, error);
    if (result) goto done;
    result = fetch_manifest_tag(&http, &reference, tag, &bytes, &size, &headers);
    /* What answered is named before anything is said about its content: a
     * page served by another host is not a manifest that disagrees. */
    if (manifest_answer_refused(&http, &reference, &headers)) result = -1;
    if (result) goto done;
    char hex[MAELYS_OCI_DIGEST_HEX_SIZE];
    char digest[OCI_DIGEST_SIZE];
    maelys_oci_sha256_hex(bytes, size, hex);
    if (oci_snprintf(digest, sizeof(digest), OCI_DIGEST_PREFIX "%s", hex) < 0) {
        pull_report(&http, OCI_ERROR_MEMORY, "cannot format the manifest digest");
        result = -1; goto done;
    }
    /* The header is a claim; the bytes are the authority. */
    if (headers.content_digest &&
        strcmp(headers.content_digest, digest) != 0) {
        pull_report(&http, OCI_ERROR_PROTOCOL,
            "registry reports a digest the received bytes do not produce");
        result = -1; goto done;
    }
    if (!headers.content_type ||
        !document_media_type_matches(bytes, size, headers.content_type)) {
        pull_report(&http, OCI_ERROR_PROTOCOL,
            "manifest media type disagrees with its content");
        result = -1; goto done;
    }
    /* A body whose media type this materializer does not know is refused
     * rather than reported with an empty platform list: a digest is only
     * worth reporting for something that is an index or an image manifest. */
    if (!index_header_media_type(headers.content_type) &&
        !manifest_header_media_type(headers.content_type)) {
        pull_report(&http, OCI_ERROR_PROTOCOL,
            "the registry served neither an OCI index nor an image manifest");
        result = -1; goto done;
    }
    platforms = oci_document_array();
    if (!platforms) { result = -1; goto done; }
    if (index_header_media_type(headers.content_type)) {
        size_t count = 0u;
        if (index_parse(bytes, size, &children, &count) != 0 ||
            count > PULL_MANIFEST_MAX) {
            pull_report(&http, OCI_ERROR_PROTOCOL,
                "invalid OCI index or descriptor limit exceeded");
            result = -1; goto done;
        }
        for (size_t i = 0u; i < count; ++i) {
            if (oci_descriptor_is_buildx_attestation(&children[i])) continue;
            if (!children[i].os[0]) continue;  /* a descriptor naming no platform */
            oci_document_t *record = platform_record(&children[i]);
            if (!record || oci_document_append(platforms, record) != 0) {
                pull_report(&http, OCI_ERROR_MEMORY,
                    "cannot record the platforms the index offers");
                result = -1; goto done;
            }
        }
    } else {
        /* One image manifest: parsed so that a malformed one is refused,
         * and reported with no platform list, which only an index has. */
        oci_manifest_t image = {0};
        if (manifest_parse(bytes, size, &image) != 0) {
            oci_manifest_clear(&image);
            pull_report(&http, OCI_ERROR_PROTOCOL,
                "image manifest is structurally invalid");
            result = -1; goto done;
        }
        oci_manifest_clear(&image);
    }
    document = OCI_DOCUMENT_OBJECT(
        {"schema", oci_document_string(OCI_SCHEMA_RESOLUTION)},
        {"registry", oci_document_string(reference.authority)},
        {"repository", oci_document_string(reference.repository)},
        {"tag", oci_document_string(tag)},
        {"reference", oci_document_string(options->reference)},
        {"digest", oci_document_string(digest)},
        {"mediaType", oci_document_string(headers.content_type)},
        {"manifestBytes", oci_document_integer((int64_t)size)},
        {"platforms", platforms});
    platforms = NULL;
    if (!document) {
        pull_report(&http, OCI_ERROR_MEMORY, "cannot build the resolution");
        result = -1; goto done;
    }
    *out_document = document;
    document = NULL;
done:
    oci_document_release(platforms);
    oci_document_release(document);
    free(children);
    free(bytes);
    headers_clear(&headers);
    http_clear(&http);
    reference_clear(&reference);
    return result;
}
