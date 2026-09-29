/* SPDX-License-Identifier: MPL-2.0 */
/* Read-only image metadata. Runtime parameters are reported as declared;
 * no defaults, user lookup or runtime policy are applied to the image. */
#include "src/materializer/internal.h"

#include <stdlib.h>
#include <string.h>

typedef enum metadata_shape {
    METADATA_STRING,
    METADATA_BOOLEAN,
    METADATA_STRINGS,
    METADATA_STRING_MAP,
    METADATA_EMPTY_MAP
} metadata_shape_t;

static oci_document_t *metadata_string(const maelys_json_document_t *json,
    maelys_json_value_t value, oci_error_t *error) {
    maelys_json_view_t view;
    if (maelys_json_value_string(json, value, &view) != MAELYS_JSON_OK ||
        memchr(view.data, '\0', view.size)) {
        oci_error_report(error, OCI_ERROR_PROTOCOL,
            "image metadata must contain strings without NUL bytes");
        return NULL;
    }
    char *text = strndup(view.data, view.size);
    oci_document_t *copy = text ? oci_document_string(text) : NULL;
    free(text);
    if (!copy) oci_error_report(error, OCI_ERROR_MEMORY, "out of memory");
    return copy;
}

static oci_document_t *metadata_value(const maelys_json_document_t *json,
    maelys_json_value_t value, metadata_shape_t shape, oci_error_t *error) {
    if (shape == METADATA_STRING) return metadata_string(json, value, error);
    if (shape == METADATA_BOOLEAN) {
        int boolean;
        if (maelys_json_value_boolean(json, value, &boolean) != MAELYS_JSON_OK)
            goto invalid;
        oci_document_t *copy = oci_document_boolean(boolean);
        if (!copy) oci_error_report(error, OCI_ERROR_MEMORY, "out of memory");
        return copy;
    }
    int array = shape == METADATA_STRINGS;
    size_t count = 0u;
    if ((array ? maelys_json_array_size(json, value, &count) :
            maelys_json_object_size(json, value, &count)) != MAELYS_JSON_OK)
        goto invalid;
    oci_document_t *copy = array ? oci_document_array() : oci_document_object();
    if (!copy) {
        oci_error_report(error, OCI_ERROR_MEMORY, "out of memory");
        return NULL;
    }
    for (size_t i = 0u; i < count; ++i) {
        maelys_json_value_t child;
        maelys_json_view_t key = {0};
        maelys_json_result_t found = array ?
            maelys_json_array_get(json, value, i, &child) :
            maelys_json_object_member_at(json, value, i, &key, &child);
        if (found != MAELYS_JSON_OK || (!array && memchr(key.data, '\0', key.size))) {
            oci_document_release(copy);
            goto invalid;
        }
        oci_document_t *item;
        if (shape == METADATA_EMPTY_MAP) {
            size_t children = 0u;
            if (maelys_json_object_size(json, child, &children) != MAELYS_JSON_OK ||
                children != 0u) {
                oci_document_release(copy);
                goto invalid;
            }
            item = oci_document_object();
        } else {
            item = metadata_string(json, child, error);
        }
        char *name = array ? NULL : strndup(key.data, key.size);
        int result = array ? oci_document_append(copy, item) :
            oci_document_put(copy, name, item);
        free(name);
        if (result != 0) {
            oci_document_release(copy);
            if (!error->message)
                oci_error_report(error, OCI_ERROR_MEMORY, "out of memory");
            return NULL;
        }
    }
    return copy;
invalid:
    oci_error_report(error, OCI_ERROR_PROTOCOL,
        "image metadata has an invalid field type");
    return NULL;
}

/* The image spec treats null optional fields as absent. Unknown fields are
 * not copied or interpreted, so extensions cannot break this typed report. */
static int metadata_member(const maelys_json_document_t *json,
    maelys_json_value_t root, const char *key, oci_document_t *target,
    const char *name, metadata_shape_t shape, oci_error_t *error) {
    maelys_json_value_t value;
    maelys_json_result_t found = maelys_json_object_get(json, root, key, &value);
    if (found == MAELYS_JSON_ERR_NOT_FOUND) return 0;
    if (found != MAELYS_JSON_OK) {
        oci_error_report(error, OCI_ERROR_PROTOCOL,
            "cannot read image metadata field %s", key);
        return -1;
    }
    if (maelys_json_value_type(json, value) == MAELYS_JSON_TYPE_NULL) return 0;
    oci_document_t *copy = metadata_value(json, value, shape, error);
    if (!copy || oci_document_put(target, name, copy) != 0) {
        if (!error->message)
            oci_error_report(error, OCI_ERROR_MEMORY, "out of memory");
        oci_error_report(error, error->kind, "metadata field %s", key);
        return -1;
    }
    return 0;
}

static maelys_json_document_t *read_metadata(const oci_source_t *source,
    const oci_descriptor_t *descriptor, oci_error_t *error) {
    unsigned char *bytes = NULL;
    size_t size = 0u;
    if (source_read_descriptor(source, descriptor, OCI_JSON_MAX,
            &bytes, &size) != 0) {
        oci_error_report(error, OCI_ERROR_PROTOCOL,
            "metadata %s is absent, oversized or does not match its digest",
            descriptor->digest);
        return NULL;
    }
    maelys_json_document_t *json = oci_json_parse_object(bytes, size,
        OCI_JSON_TOKENS_MAX);
    free(bytes);
    if (!json) oci_error_report(error, OCI_ERROR_PROTOCOL,
        "metadata %s is not a valid JSON object", descriptor->digest);
    return json;
}

static int configuration(const maelys_json_document_t *json,
    maelys_json_value_t root, oci_document_t *target, oci_error_t *error) {
    maelys_json_value_t value;
    maelys_json_result_t found = maelys_json_object_get(json, root, "config", &value);
    if (found == MAELYS_JSON_ERR_NOT_FOUND || (found == MAELYS_JSON_OK &&
            maelys_json_value_type(json, value) == MAELYS_JSON_TYPE_NULL)) return 0;
    if (found != MAELYS_JSON_OK ||
        maelys_json_value_type(json, value) != MAELYS_JSON_TYPE_OBJECT) {
        oci_error_report(error, OCI_ERROR_PROTOCOL,
            "image config must be an object or null");
        return -1;
    }
    static const struct {
        const char *key;
        metadata_shape_t shape;
    } fields[] = {
        {"User", METADATA_STRING}, {"Env", METADATA_STRINGS},
        {"Entrypoint", METADATA_STRINGS}, {"Cmd", METADATA_STRINGS},
        {"WorkingDir", METADATA_STRING}, {"Labels", METADATA_STRING_MAP},
        {"ExposedPorts", METADATA_EMPTY_MAP}, {"Volumes", METADATA_EMPTY_MAP},
        {"StopSignal", METADATA_STRING}, {"ArgsEscaped", METADATA_BOOLEAN}
    };
    for (size_t i = 0u; i < sizeof(fields) / sizeof(fields[0]); ++i)
        if (metadata_member(json, value, fields[i].key, target, fields[i].key,
                fields[i].shape, error) != 0) return -1;
    return 0;
}

static int history(const maelys_json_document_t *json, maelys_json_value_t root,
    const oci_manifest_t *manifest, oci_document_t *target, oci_error_t *error) {
    maelys_json_value_t entries;
    maelys_json_result_t found = maelys_json_object_get(json, root, "history", &entries);
    if (found == MAELYS_JSON_ERR_NOT_FOUND || (found == MAELYS_JSON_OK &&
            maelys_json_value_type(json, entries) == MAELYS_JSON_TYPE_NULL)) return 0;
    size_t count = 0u;
    if (found != MAELYS_JSON_OK ||
        maelys_json_array_size(json, entries, &count) != MAELYS_JSON_OK) {
        oci_error_report(error, OCI_ERROR_PROTOCOL, "image history must be an array");
        return -1;
    }
    size_t layer = 0u;
    for (size_t i = 0u; i < count; ++i) {
        maelys_json_value_t entry, empty_value;
        int empty = 0;
        if (maelys_json_array_get(json, entries, i, &entry) != MAELYS_JSON_OK ||
            maelys_json_value_type(json, entry) != MAELYS_JSON_TYPE_OBJECT) {
            oci_error_report(error, OCI_ERROR_PROTOCOL,
                "image history entry %zu must be an object", i);
            return -1;
        }
        found = maelys_json_object_get(json, entry, "empty_layer", &empty_value);
        if (found != MAELYS_JSON_ERR_NOT_FOUND && (found != MAELYS_JSON_OK ||
                (maelys_json_value_type(json, empty_value) != MAELYS_JSON_TYPE_NULL &&
                 maelys_json_value_boolean(json, empty_value, &empty) != MAELYS_JSON_OK))) {
            oci_error_report(error, OCI_ERROR_PROTOCOL,
                "history empty_layer must be a boolean or null");
            return -1;
        }
        if (!empty && layer >= manifest->layer_count) {
            oci_error_report(error, OCI_ERROR_PROTOCOL,
                "image history references more layers than the manifest");
            return -1;
        }
        oci_document_t *item = OCI_DOCUMENT_OBJECT(
            {"emptyLayer", oci_document_boolean(empty)});
        if (!item) return -1;
        int result = metadata_member(json, entry, "created", item, "created",
            METADATA_STRING, error) ||
            metadata_member(json, entry, "created_by", item, "createdBy",
                METADATA_STRING, error) ||
            metadata_member(json, entry, "author", item, "author",
                METADATA_STRING, error) ||
            metadata_member(json, entry, "comment", item, "comment",
                METADATA_STRING, error);
        if (!result && !empty) {
            result = oci_document_put(item, "layerIndex",
                oci_document_integer((int64_t)layer)) ||
                oci_document_put(item, "layerDigest",
                    oci_document_string(manifest->layers[layer].digest)) ||
                oci_document_put(item, "diffId",
                    oci_document_string(manifest->diff_ids[layer]));
            ++layer;
        }
        if (result) {
            oci_document_release(item);
            return -1;
        }
        if (oci_document_append(target, item) != 0) return -1;
    }
    return 0;
}

static int stat_manifest(const oci_source_t *source, const oci_manifest_t *manifest,
    oci_document_t *target, oci_error_t *error) {
    maelys_json_document_t *image = read_metadata(source, &manifest->manifest, error);
    maelys_json_document_t *config = image ?
        read_metadata(source, &manifest->config, error) : NULL;
    maelys_json_value_t image_root = maelys_json_document_root(image);
    maelys_json_value_t config_root = maelys_json_document_root(config);
    oci_document_t *parameters = oci_document_object();
    oci_document_t *layers = oci_document_array();
    oci_document_t *entries = oci_document_array();
    oci_document_t *annotations = oci_document_object();
    int result = -1;
    if (!image || !config || !parameters || !layers || !entries || !annotations)
        goto done;
    if (configuration(config, config_root, parameters, error) != 0 ||
        history(config, config_root, manifest, entries, error) != 0 ||
        metadata_member(image, image_root, "annotations", target, "annotations",
            METADATA_STRING_MAP, error) != 0 ||
        metadata_member(config, config_root, "created", target, "created",
            METADATA_STRING, error) != 0 ||
        metadata_member(config, config_root, "author", target, "author",
            METADATA_STRING, error) != 0 ||
        metadata_member(config, config_root, "variant", target, "variant",
            METADATA_STRING, error) != 0) goto done;
    if (!oci_document_get(target, "annotations") &&
        oci_document_set(target, "annotations", annotations) != 0) goto done;
    for (size_t i = 0u; i < manifest->layer_count; ++i) {
        oci_document_t *item = OCI_DOCUMENT_OBJECT(
            {"digest", oci_document_string(manifest->layers[i].digest)},
            {"mediaType", oci_document_string(manifest->layers[i].media_type)},
            {"size", oci_document_integer((int64_t)manifest->layers[i].size)},
            {"diffId", oci_document_string(manifest->diff_ids[i])});
        if (!item || oci_document_append(layers, item) != 0) goto done;
    }
    result = oci_document_put(target, "mediaType",
            oci_document_string(manifest->manifest.media_type)) ||
        oci_document_put(target, "manifestBytes",
            oci_document_integer((int64_t)manifest->manifest.size)) ||
        oci_document_put(target, "configBytes",
            oci_document_integer((int64_t)manifest->config.size)) ||
        oci_document_set(target, "config", parameters) ||
        oci_document_set(target, "layers", layers) ||
        oci_document_set(target, "history", entries);
done:
    oci_document_release(parameters);
    oci_document_release(layers);
    oci_document_release(entries);
    oci_document_release(annotations);
    maelys_json_document_release(image);
    maelys_json_document_release(config);
    if (result != 0 && !error->message)
        oci_error_report(error, OCI_ERROR_MEMORY, "cannot build image metadata");
    return result ? -1 : 0;
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
    /* History adds derived layer fields. Check the resulting byte and token
     * ceilings too, so a successful opaque document can always be rendered. */
    char *text = oci_document_dump(root);
    if (!text) {
        oci_error_report(error, OCI_ERROR_PROTOCOL,
            "image metadata report exceeds the JSON byte or token limit");
        oci_document_release(root);
        return NULL;
    }
    free(text);
    return root;
}
