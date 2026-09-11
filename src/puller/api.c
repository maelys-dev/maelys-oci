/* SPDX-License-Identifier: MPL-2.0 */
#include "src/puller/internal.h"
#include "src/puller/tls_version.h"
#include <stdlib.h>
#include <string.h>

struct maelys_oci_pull_options {
    char *ca_file;
    char *token_file;
    char *docker_config;
    uint64_t timeout_ms;
};
struct maelys_oci_pull_result {
    oci_document_t *document;
    char *json;
};
maelys_oci_result_t maelys_oci_pull_options_create(maelys_oci_pull_options_t **out_options) {
    if (!out_options) return MAELYS_OCI_ERR_ARGUMENT;
    *out_options = calloc(1u, sizeof(**out_options));
    if (!*out_options) return MAELYS_OCI_ERR_MEMORY;
    (*out_options)->timeout_ms = MAELYS_OCI_PULL_TIMEOUT_MS;
    return MAELYS_OCI_OK;
}
void maelys_oci_pull_options_release(maelys_oci_pull_options_t **options) {
    if (!options || !*options) return;
    free((*options)->ca_file);
    free((*options)->token_file);
    free((*options)->docker_config);
    free(*options);
    *options = NULL;
}
static maelys_oci_result_t set_path(char **destination, const char *path) {
    if (path && (path[0] != '/' || strnlen(path, PATH_MAX) == PATH_MAX))
        return MAELYS_OCI_ERR_ARGUMENT;
    char *copy = path ? strdup(path) : NULL;
    if (path && !copy) return MAELYS_OCI_ERR_MEMORY;
    free(*destination);
    *destination = copy;
    return MAELYS_OCI_OK;
}
maelys_oci_result_t maelys_oci_pull_options_set_ca_file(maelys_oci_pull_options_t *options, const char *path) {
    return options ? set_path(&options->ca_file, path) : MAELYS_OCI_ERR_ARGUMENT;
}
maelys_oci_result_t maelys_oci_pull_options_set_token_file(maelys_oci_pull_options_t *options, const char *path) {
    return options && (!path || !options->docker_config) ?
        set_path(&options->token_file, path) : MAELYS_OCI_ERR_ARGUMENT;
}
maelys_oci_result_t maelys_oci_pull_options_set_docker_config(maelys_oci_pull_options_t *options, const char *path) {
    return options && (!path || !options->token_file) ?
        set_path(&options->docker_config, path) : MAELYS_OCI_ERR_ARGUMENT;
}
maelys_oci_result_t maelys_oci_pull_options_set_timeout_ms(maelys_oci_pull_options_t *options, uint64_t timeout_ms) {
    if (!options || !timeout_ms || timeout_ms > MAELYS_OCI_PULL_TIMEOUT_MAX_MS)
        return MAELYS_OCI_ERR_ARGUMENT;
    options->timeout_ms = timeout_ms;
    return MAELYS_OCI_OK;
}
static maelys_oci_result_t public_error(oci_error_kind_t kind) {
    switch (kind) {
    case OCI_ERROR_ARGUMENT: return MAELYS_OCI_ERR_ARGUMENT;
    case OCI_ERROR_MEMORY: return MAELYS_OCI_ERR_MEMORY;
    case OCI_ERROR_NOT_FOUND: return MAELYS_OCI_ERR_NOT_FOUND;
    case OCI_ERROR_ACCESS: return MAELYS_OCI_ERR_ACCESS;
    case OCI_ERROR_PROTOCOL: return MAELYS_OCI_ERR_PROTOCOL;
    case OCI_ERROR_STATE: return MAELYS_OCI_ERR_STATE;
    case OCI_ERROR_UNSUPPORTED: return MAELYS_OCI_ERR_UNSUPPORTED;
    default: return MAELYS_OCI_ERR_IO;
    }
}
maelys_oci_result_t maelys_oci_pull(const maelys_oci_pull_options_t *options,
    const char *store, const char *reference, const char *platform,
    maelys_oci_pull_result_t **out_result, char **out_error) {
    if (out_error) *out_error = NULL;
    if (!out_result) return MAELYS_OCI_ERR_ARGUMENT;
    *out_result = NULL;
    unsigned int tls_version = oci_mbedtls_runtime_version();
    if (!oci_mbedtls_version_secure(tls_version)) {
        maelys_oci_set_error(out_error,
            "Mbed TLS runtime %u.%u.%u is vulnerable to CVE-2025-27810; "
            "require 2.28.10+, 3.6.3+ or 4+",
            tls_version >> 24u, (tls_version >> 16u) & 0xffu,
            (tls_version >> 8u) & 0xffu);
        return MAELYS_OCI_ERR_UNSUPPORTED;
    }
    maelys_oci_pull_result_t *result = calloc(1u, sizeof(*result));
    if (!result) return MAELYS_OCI_ERR_MEMORY;
    pull_options_t request = {.store = store, .reference = reference, .platform = platform,
        .timeout_ms = options ? options->timeout_ms : MAELYS_OCI_PULL_TIMEOUT_MS,
        .ca_file = options ? options->ca_file : NULL,
        .token_file = options ? options->token_file : NULL,
        .docker_config = options ? options->docker_config : NULL};
    oci_error_t error = OCI_ERROR_INIT;
    if (oci_pull(&request, &result->document, &error) != 0) {
        maelys_oci_result_t status = public_error(error.kind);
        maelys_oci_set_error(out_error, "%s", oci_error_message(&error));
        oci_error_clear(&error);
        maelys_oci_pull_result_release(&result);
        return status;
    }
    oci_error_clear(&error);
    result->json = oci_document_dump(result->document);
    if (!result->json) {
        maelys_oci_pull_result_release(&result);
        maelys_oci_set_error(out_error, "cannot serialize the pull receipt");
        return MAELYS_OCI_ERR_MEMORY;
    }
    *out_result = result;
    return MAELYS_OCI_OK;
}
void maelys_oci_pull_result_release(maelys_oci_pull_result_t **result) {
    if (!result || !*result) return;
    oci_document_release((*result)->document);
    free((*result)->json);
    free(*result);
    *result = NULL;
}
#define STRING_GETTER(name, field) \
    const char *maelys_oci_pull_result_##name(const maelys_oci_pull_result_t *result) { \
        return result ? oci_document_string_value(oci_document_get(result->document, field)) : NULL; \
    }
STRING_GETTER(requested_reference, "requestedReference")
STRING_GETTER(resolved_digest, "resolvedDigest")
STRING_GETTER(platform, "platform")
STRING_GETTER(manifest_digest, "manifestDigest")
STRING_GETTER(config_digest, "configDigest")
STRING_GETTER(artifact_digest, "artifactDigest")
STRING_GETTER(artifact_path, "artifact")
#undef STRING_GETTER
#define INTEGER_GETTER(name, field) \
    uint64_t maelys_oci_pull_result_##name(const maelys_oci_pull_result_t *result) { \
        return result ? (uint64_t)oci_document_integer_value(oci_document_get(result->document, field)) : 0u; \
    }
INTEGER_GETTER(blob_count, "blobCount")
INTEGER_GETTER(blob_bytes, "blobBytes")
INTEGER_GETTER(downloaded_blobs, "downloadedBlobs")
INTEGER_GETTER(cached_blobs, "cachedBlobs")
#undef INTEGER_GETTER
int maelys_oci_pull_result_cache_hit(const maelys_oci_pull_result_t *result) {
    return result && !maelys_oci_pull_result_downloaded_blobs(result);
}
const char *maelys_oci_pull_result_receipt_json(const maelys_oci_pull_result_t *result) {
    return result ? result->json : NULL;
}
