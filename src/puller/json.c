/* SPDX-License-Identifier: MPL-2.0 */
#include "src/puller/internal.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* Registry descriptors must announce a positive size: an empty blob cannot
 * carry a manifest, a config or a layer. */
static int descriptor_parse(
    const maelys_json_document_t *document, maelys_json_value_t value,
    int platform_required, oci_descriptor_t *out) {
    return oci_descriptor_parse(document, value, platform_required, out) == 0 &&
        out->size > 0u ? 0 : -1;
}

static int schema_version_two(
    const maelys_json_document_t *document, maelys_json_value_t root) {
    return oci_json_u64_is(document, root, "schemaVersion", 2u);
}

int document_media_type_matches(
    const unsigned char *bytes, size_t size, const char *expected) {
    maelys_json_document_t *document = oci_json_parse_object(
        bytes, size, OCI_JSON_TOKENS_MAX);
    char media_type[OCI_MEDIA_TYPE_SIZE];
    int valid = document && oci_json_copy_string(document,
        maelys_json_document_root(document), "mediaType", media_type,
        sizeof(media_type), 0) == 0 &&
        (!media_type[0] || oci_media_type_equal(expected, media_type));
    maelys_json_document_release(document);
    return valid;
}

int index_parse(const unsigned char *bytes, size_t size,
    oci_descriptor_t **out_items, size_t *out_count) {
    *out_items = NULL;
    *out_count = 0u;
    maelys_json_document_t *document = oci_json_parse_object(
        bytes, size, OCI_JSON_TOKENS_MAX);
    maelys_json_value_t root = maelys_json_document_root(document);
    maelys_json_value_t manifests;
    size_t count = 0u;
    oci_descriptor_t *items = NULL;
    int result = -1;
    if (!document || !schema_version_two(document, root) ||
        maelys_json_object_get(document, root, "manifests", &manifests) !=
            MAELYS_JSON_OK ||
        maelys_json_array_size(document, manifests, &count) != MAELYS_JSON_OK ||
        !count || count > PULL_MANIFEST_MAX) goto done;
    items = calloc(count, sizeof(*items));
    if (!items) goto done;
    for (size_t i = 0u; i < count; ++i) {
        maelys_json_value_t value;
        if (maelys_json_array_get(document, manifests, i, &value) !=
                MAELYS_JSON_OK ||
            descriptor_parse(document, value, 0, &items[i]) != 0 ||
            items[i].size > OCI_JSON_MAX ||
            (!oci_manifest_media_type_supported(items[i].media_type) &&
             !oci_index_media_type_supported(items[i].media_type))) goto done;
    }
    *out_items = items;
    *out_count = count;
    items = NULL;
    result = 0;
done:
    free(items);
    maelys_json_document_release(document);
    return result;
}

static int manifest_fail(
    maelys_json_document_t *document, oci_manifest_t *manifest) {
    maelys_json_document_release(document);
    oci_manifest_clear(manifest);
    return -1;
}

int manifest_parse(
    const unsigned char *bytes, size_t size, oci_manifest_t *out) {
    memset(out, 0, sizeof(*out));
    maelys_json_document_t *document = oci_json_parse_object(
        bytes, size, OCI_JSON_TOKENS_MAX);
    maelys_json_value_t root = maelys_json_document_root(document);
    maelys_json_value_t config;
    maelys_json_value_t layers;
    size_t count;
    if (!document || !schema_version_two(document, root) ||
        maelys_json_object_get(document, root, "config", &config) !=
            MAELYS_JSON_OK ||
        descriptor_parse(document, config, 0, &out->config) != 0 ||
        out->config.size > PULL_CONFIG_MAX ||
        !oci_config_media_type_supported(out->config.media_type) ||
        maelys_json_object_get(document, root, "layers", &layers) !=
            MAELYS_JSON_OK ||
        maelys_json_array_size(document, layers, &count) != MAELYS_JSON_OK ||
        count > PULL_LAYER_MAX)
        return manifest_fail(document, out);
    out->layer_count = count;
    out->layers = calloc(count ? count : 1u, sizeof(*out->layers));
    if (!out->layers || (uint64_t)size > PULL_CLOSURE_MAX - out->config.size)
        return manifest_fail(document, out);
    uint64_t closure_size = out->config.size + (uint64_t)size;
    for (size_t index = 0u; index < count; ++index) {
        maelys_json_value_t value;
        if (maelys_json_array_get(document, layers, index, &value) !=
                MAELYS_JSON_OK ||
            descriptor_parse(document, value, 0, &out->layers[index]) != 0 ||
            !oci_layer_media_type_supported(out->layers[index].media_type))
            return manifest_fail(document, out);
        if (strcmp(out->layers[index].digest, out->config.digest) == 0)
            return manifest_fail(document, out);
        int duplicate = 0;
        for (size_t previous = 0u; previous < index; ++previous) {
            if (strcmp(out->layers[previous].digest,
                    out->layers[index].digest) != 0)
                continue;
            if (out->layers[previous].size != out->layers[index].size ||
                strcmp(out->layers[previous].media_type,
                    out->layers[index].media_type) != 0)
                return manifest_fail(document, out);
            duplicate = 1;
            break;
        }
        if (!duplicate) {
            if (closure_size > PULL_CLOSURE_MAX - out->layers[index].size)
                return manifest_fail(document, out);
            closure_size += out->layers[index].size;
        }
    }
    out->manifest.size = size;
    if (!oci_manifest_staging_valid(out)) return manifest_fail(document, out);
    maelys_json_document_release(document);
    return 0;
}
