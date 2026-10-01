/* SPDX-License-Identifier: MPL-2.0 */
/* Remote metadata uses pull's verified traversal with no store attached. */
#include "src/puller/internal.h"
#include "src/common/metadata.h"
#include <string.h>

static oci_document_t *remote_document(const pull_options_t *options,
    const pull_reference_t *reference, const pull_resolved_t *resolved,
    oci_error_t *error) {
    const oci_manifest_t *image = &resolved->image;
    oci_document_t *item = oci_manifest_summary(image);
    oci_document_t *items = oci_document_array();
    oci_document_t *root = NULL;
    if (!item || !items || oci_manifest_metadata(image,
            resolved->manifest_bytes, resolved->manifest_size,
            resolved->config_bytes, resolved->config_size, item, error) != 0)
        goto done;
    int appended = oci_document_append(items, item);
    item = NULL; /* append consumes even on failure. */
    if (appended != 0) goto done;
    root = OCI_DOCUMENT_OBJECT(
        {"schema", oci_document_string("maelys.oci-remote-stat/v1")},
        {"reference", oci_document_string(options->reference)},
        {"registry", oci_document_string(reference->authority)},
        {"repository", oci_document_string(reference->repository)},
        {"verification", OCI_DOCUMENT_OBJECT(
            {"scope", oci_document_string("metadata-only")},
            {"referenceDigest", oci_document_string(reference->digest)},
            {"manifestDigest", oci_document_string(image->manifest.digest)},
            {"configDigest", oci_document_string(image->config.digest)},
            {"layersVerified", oci_document_boolean(0)},
            {"diffIdsVerified", oci_document_boolean(0)},
            {"materialized", oci_document_boolean(0)})});
    if (root && (oci_document_set(root, "manifests", items) != 0 ||
            !oci_metadata_report_valid(root, error))) {
        oci_document_release(root);
        root = NULL;
    }
done:
    oci_document_release(item);
    oci_document_release(items);
    if (!root && !error->message)
        oci_error_report(error, OCI_ERROR_MEMORY, "cannot build remote image metadata");
    return root;
}

int oci_stat_remote(const pull_options_t *options,
    oci_document_t **out_document, oci_error_t *error) {
    *out_document = NULL;
    if (options->store || options->expected_root ||
        (options->token_file && options->docker_config) ||
        (options->platform && strcmp(options->platform, "linux/arm64") != 0 &&
         strcmp(options->platform, "linux/amd64") != 0)) {
        oci_error_report(error, OCI_ERROR_ARGUMENT, "invalid remote inspection options");
        return -1;
    }
    pull_reference_t reference;
    if (reference_parse(options->reference, &reference) != 0) {
        oci_error_report(error, OCI_ERROR_ARGUMENT,
            "REFERENCE must be REGISTRY/REPOSITORY@sha256:HEX");
        return -1;
    }
    pull_http_t http = {0};
    pull_resolved_t resolved = {0};
    int result = http_initialize(&http, options, &reference, error);
    if (!result) result = pull_resolve(&http, &reference, options->platform, &resolved);
    if (!result) {
        *out_document = remote_document(options, &reference, &resolved, error);
        if (!*out_document) result = -1;
    }
    resolved_clear(&resolved);
    http_clear(&http);
    reference_clear(&reference);
    return result;
}
