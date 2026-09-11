/* SPDX-License-Identifier: MPL-2.0 */
/* The same image config contract applies to local imports and registry pulls. */
#include "src/common/internal.h"

#include <stdlib.h>
#include <string.h>

int oci_config_parse(const unsigned char *bytes, size_t size,
    oci_manifest_t *manifest, oci_error_t *error) {
    maelys_json_document_t *document = oci_json_parse_object(
        bytes, size, OCI_JSON_TOKENS_MAX);
    maelys_json_value_t root = maelys_json_document_root(document);
    maelys_json_value_t rootfs, diff_ids;
    char os[OCI_PLATFORM_PART_SIZE], architecture[OCI_PLATFORM_PART_SIZE];
    char variant[OCI_PLATFORM_PART_SIZE], type[16];
    char (*ids)[OCI_DIGEST_SIZE] = NULL;
    size_t count = 0u;
    int result = OCI_CONFIG_INVALID;
    oci_error_kind_t kind = OCI_ERROR_PROTOCOL;
    const char *detail = "image config has an invalid platform declaration";
    if (!document ||
        oci_json_copy_string(document, root, "os", os, sizeof(os), 1) != 0 ||
        oci_json_copy_string(document, root, "architecture", architecture,
            sizeof(architecture), 1) != 0 ||
        oci_json_copy_string(document, root, "variant", variant,
            sizeof(variant), 0) != 0)
        goto done;
    detail = "selected manifest config disagrees with its index platform";
    if ((manifest->manifest.os[0] && strcmp(manifest->manifest.os, os) != 0) ||
        (manifest->manifest.architecture[0] &&
         strcmp(manifest->manifest.architecture, architecture) != 0)) goto done;
    if (!oci_platform_parts_supported(os, architecture) ||
        !oci_platform_variant_supported(architecture, variant) ||
        !oci_platform_variant_supported(architecture, manifest->manifest.variant)) {
        result = OCI_CONFIG_UNSUPPORTED;
        detail = "image config declares a platform this build cannot materialize";
        goto done;
    }
    detail = "config rootfs must declare type layers and one SHA-256 DiffID per layer";
    if (maelys_json_object_get(document, root, "rootfs", &rootfs) != MAELYS_JSON_OK ||
        oci_json_copy_string(document, rootfs, "type", type, sizeof(type), 1) != 0 ||
        strcmp(type, "layers") != 0 ||
        maelys_json_object_get(document, rootfs, "diff_ids", &diff_ids) != MAELYS_JSON_OK ||
        maelys_json_array_size(document, diff_ids, &count) != MAELYS_JSON_OK ||
        count != manifest->layer_count) goto done;
    ids = calloc(count ? count : 1u, sizeof(*ids));
    if (!ids) {
        kind = OCI_ERROR_MEMORY;
        detail = "cannot allocate image DiffIDs";
        goto done;
    }
    for (size_t i = 0u; i < count; ++i) {
        maelys_json_value_t value;
        maelys_json_view_t view;
        if (maelys_json_array_get(document, diff_ids, i, &value) != MAELYS_JSON_OK ||
            maelys_json_value_string(document, value, &view) != MAELYS_JSON_OK ||
            view.size != OCI_DIGEST_SIZE - 1u || memchr(view.data, '\0', view.size))
            goto done;
        memcpy(ids[i], view.data, view.size);
        if (!oci_digest_valid(ids[i])) goto done;
    }
    free(manifest->diff_ids);
    manifest->diff_ids = ids;
    ids = NULL;
    memcpy(manifest->os, os, strlen(os) + 1u);
    memcpy(manifest->architecture, architecture, strlen(architecture) + 1u);
    result = OCI_CONFIG_OK;
done:
    if (result != OCI_CONFIG_OK) oci_error_report(error, kind, "%s", detail);
    free(ids);
    maelys_json_document_release(document);
    return result;
}
