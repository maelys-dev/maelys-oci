/* SPDX-License-Identifier: MPL-2.0 */
/* Bounded OCI graph resolution. Descriptors are authorities; media types,
 * sizes, digests and config platforms must agree at every edge. */
#include "src/puller/internal.h"
#include <stdlib.h>
#include <string.h>

static int resolve_config(
    pull_http_t *http, const pull_reference_t *reference,
    const char *platform, oci_manifest_t *selected,
    unsigned char **out_config, size_t *out_size) {
    char config_target[PULL_TARGET_MAX];
    if (oci_snprintf(config_target, sizeof(config_target), "/v2/%s/blobs/%s",
            reference->repository, selected->config.digest) < 0) {
        pull_report(http, OCI_ERROR_ARGUMENT, "repository name is too long");
        return -1;
    }
    unsigned char *config = NULL;
    size_t config_size = 0u;
    pull_headers_t config_headers = {0};
    int fetched = fetch_memory(http, reference, config_target, NULL,
        selected->config.digest, PULL_CONFIG_MAX, &config_headers,
        &config, &config_size);
    headers_clear(&config_headers);
    if (fetched != 0 || config_size != selected->config.size ||
        oci_config_parse(config, config_size, selected, http->error) != 0) {
        pull_report(http, OCI_ERROR_PROTOCOL,
            "image config is absent, altered, or has an invalid platform or rootfs DiffIDs");
        free(config);
        return -1;
    }
    char actual[OCI_PLATFORM_SIZE];
    if (oci_manifest_platform(selected, actual) != 0 ||
        (platform && strcmp(platform, actual) != 0)) {
        pull_report(http, OCI_ERROR_PROTOCOL,
            "selected manifest config disagrees with --platform");
        free(config);
        return -1;
    }
    memcpy(selected->manifest.os, selected->os, sizeof(selected->manifest.os));
    memcpy(selected->manifest.architecture, selected->architecture,
           sizeof(selected->manifest.architecture));
    *out_config = config;
    *out_size = config_size;
    return 0;
}

typedef struct resolution_walk {
    pull_http_t *http;
    const pull_reference_t *reference;
    const char *platform;
    char ancestors[PULL_INDEX_DEPTH_MAX + 1u][OCI_DIGEST_SIZE];
    size_t descriptors;
    size_t matches;
    pull_resolved_t *selected;
} resolution_walk_t;

void resolved_clear(pull_resolved_t *resolved) {
    oci_manifest_clear(&resolved->image);
    free(resolved->manifest_bytes);
    free(resolved->config_bytes);
    memset(resolved, 0, sizeof(*resolved));
}

static int platform_matches(const oci_descriptor_t *descriptor, const char *platform) {
    if (!descriptor->os[0]) return 1;
    char actual[OCI_PLATFORM_SIZE];
    return oci_platform_parts_supported(descriptor->os, descriptor->architecture) &&
        oci_platform_variant_supported(descriptor->architecture, descriptor->variant) &&
        oci_snprintf(actual, sizeof(actual), "%s/%s", descriptor->os,
            descriptor->architecture) >= 0 &&
        (!platform || strcmp(actual, platform) == 0);
}

static int resolve_node(resolution_walk_t *walk, const char *digest,
    const oci_descriptor_t *expected, size_t depth) {
    if (depth > PULL_INDEX_DEPTH_MAX) {
        pull_report(walk->http, OCI_ERROR_PROTOCOL, "OCI index depth limit exceeded");
        return -1;
    }
    for (size_t i = 0u; i < depth; ++i) {
        if (strcmp(walk->ancestors[i], digest) == 0) {
            pull_report(walk->http, OCI_ERROR_PROTOCOL, "cyclic OCI descriptor graph");
            return -1;
        }
    }
    memcpy(walk->ancestors[depth], digest, OCI_DIGEST_SIZE);
    pull_resolved_t candidate = {0};
    pull_headers_t headers = {0};
    oci_descriptor_t *children = NULL;
    int result = -1;
    if (fetch_manifest_document(walk->http, walk->reference, digest,
            &candidate.manifest_bytes, &candidate.manifest_size, &headers) != 0 ||
        !headers.content_type ||
        !document_media_type_matches(candidate.manifest_bytes,
            candidate.manifest_size, headers.content_type) ||
        (expected && (candidate.manifest_size != expected->size ||
         !oci_media_type_equal(headers.content_type, expected->media_type)))) {
        pull_report(walk->http, OCI_ERROR_PROTOCOL,
            "registry manifest digest, size or media type does not match its descriptor");
        goto done;
    }
    if (index_header_media_type(headers.content_type)) {
        size_t count = 0u;
        if (depth == PULL_INDEX_DEPTH_MAX ||
            index_parse(candidate.manifest_bytes, candidate.manifest_size,
                &children, &count) != 0 ||
            count > PULL_MANIFEST_MAX - walk->descriptors) {
            pull_report(walk->http, OCI_ERROR_PROTOCOL,
                "invalid OCI index or descriptor/depth limit exceeded");
            goto done;
        }
        walk->descriptors += count;
        result = 0;
        for (size_t i = 0u; !result && i < count; ++i) {
            if (oci_descriptor_is_buildx_attestation(&children[i])) continue;
            /* An index descriptor constrains its complete subtree too. */
            if (expected && expected->os[0]) {
                if (children[i].os[0] &&
                    (strcmp(children[i].os, expected->os) != 0 ||
                     strcmp(children[i].architecture, expected->architecture) != 0)) continue;
                if (!children[i].os[0]) {
                    memcpy(children[i].os, expected->os, sizeof(children[i].os));
                    memcpy(children[i].architecture, expected->architecture, sizeof(children[i].architecture));
                    memcpy(children[i].variant, expected->variant, sizeof(children[i].variant));
                }
            }
            if (!platform_matches(&children[i], walk->platform)) continue;
            result = resolve_node(walk, children[i].digest, &children[i], depth + 1u);
        }
    } else if (manifest_header_media_type(headers.content_type)) {
        if (manifest_parse(candidate.manifest_bytes, candidate.manifest_size,
                &candidate.image) != 0) {
            pull_report(walk->http, OCI_ERROR_PROTOCOL,
                "selected image manifest is structurally invalid");
            goto done;
        }
        oci_descriptor_t *descriptor = &candidate.image.manifest;
        if (expected) *descriptor = *expected;
        memcpy(descriptor->digest, digest, OCI_DIGEST_SIZE);
        descriptor->size = candidate.manifest_size;
        size_t media_size = strcspn(headers.content_type, ";");
        while (media_size && (headers.content_type[media_size - 1u] == ' ' ||
            headers.content_type[media_size - 1u] == '\t')) --media_size;
        if (media_size >= sizeof(descriptor->media_type)) goto done;
        memcpy(descriptor->media_type, headers.content_type, media_size);
        descriptor->media_type[media_size] = '\0';
        if (resolve_config(walk->http, walk->reference, NULL, &candidate.image,
                &candidate.config_bytes, &candidate.config_size) != 0) goto done;
        if (!platform_matches(descriptor, walk->platform)) { result = 0; goto done; }
        if (++walk->matches != 1u) {
            pull_report(walk->http, OCI_ERROR_STATE,
                "the registry index requires one unambiguous --platform selector");
            goto done;
        }
        *walk->selected = candidate;
        memset(&candidate, 0, sizeof(candidate));
        result = 0;
    } else pull_report(walk->http, OCI_ERROR_PROTOCOL, "unsupported manifest media type");
done:
    free(children);
    headers_clear(&headers);
    resolved_clear(&candidate);
    return result;
}

int pull_resolve(pull_http_t *http, const pull_reference_t *reference,
    const char *platform, pull_resolved_t *out) {
    memset(out, 0, sizeof(*out));
    resolution_walk_t walk = {.http = http, .reference = reference,
        .platform = platform, .selected = out};
    int result = resolve_node(&walk, reference->digest, NULL, 0u);
    if (!result && walk.matches != 1u) {
        pull_report(http, OCI_ERROR_STATE,
            "the requested --platform is absent; one unambiguous platform is required");
        result = -1;
    }
    if (result) resolved_clear(out);
    return result;
}
