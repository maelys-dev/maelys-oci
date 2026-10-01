/* SPDX-License-Identifier: MPL-2.0 */
/* Read-only local metadata; the formatter is shared with remote inspection. */
#include "src/materializer/internal.h"
#include "src/common/metadata.h"
#include <stdlib.h>

static int stat_manifest(const oci_source_t *source, const oci_manifest_t *manifest,
    oci_document_t *target, oci_error_t *error) {
    unsigned char *image = NULL, *config = NULL;
    size_t image_size = 0u, config_size = 0u;
    int result = -1;
    if (source_read_descriptor(source, &manifest->manifest, OCI_JSON_MAX,
            &image, &image_size) != 0 ||
        source_read_descriptor(source, &manifest->config, OCI_JSON_MAX,
            &config, &config_size) != 0) {
        oci_error_report(error, OCI_ERROR_PROTOCOL,
            "image metadata is absent, oversized or does not match its descriptor");
    } else result = oci_manifest_metadata(manifest, image, image_size,
        config, config_size, target, error);
    free(image);
    free(config);
    return result;
}

oci_document_t *oci_stat_document(const oci_source_t *source,
    const oci_manifest_t *items, size_t count, oci_error_t *error) {
    oci_document_t *root = oci_inspection_document(source->path, items, count);
    if (!root || oci_document_put(root, "schema",
            oci_document_string(OCI_SCHEMA_STAT)) != 0) {
        oci_document_release(root);
        oci_error_report(error, OCI_ERROR_MEMORY, "cannot build image metadata");
        return NULL;
    }
    oci_document_t *array = oci_document_get(root, "manifests");
    uint64_t metadata_bytes = 0u;
    for (size_t i = 0u; i < count; ++i) {
        /* Bound the aggregate before copying metadata into a retained report,
         * rather than allowing one full JSON ceiling for every image. */
        if (items[i].manifest.size > OCI_JSON_MAX - metadata_bytes ||
            items[i].config.size > OCI_JSON_MAX - metadata_bytes -
                items[i].manifest.size) {
            oci_error_report(error, OCI_ERROR_PROTOCOL,
                "image metadata report exceeds the aggregate %u-byte JSON limit",
                OCI_JSON_MAX);
            oci_document_release(root);
            return NULL;
        }
        metadata_bytes += items[i].manifest.size + items[i].config.size;
        if (stat_manifest(source, &items[i], oci_document_at(array, i), error) != 0) {
            oci_document_release(root);
            return NULL;
        }
    }
    if (!oci_metadata_report_valid(root, error)) {
        oci_document_release(root);
        return NULL;
    }
    return root;
}
