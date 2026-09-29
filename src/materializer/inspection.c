/* SPDX-License-Identifier: MPL-2.0 */
/*
 * Descriptor closure inspection and the store path helpers shared by the
 * store operations.
 */
#include "src/materializer/internal.h"

#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int oci_inspect_manifest(
    const oci_source_t *source, const oci_descriptor_t *index_descriptor,
    oci_manifest_t *out, oci_error_t *error) {
    memset(out, 0, sizeof(*out));
    out->manifest = *index_descriptor;
    unsigned char *manifest_bytes = NULL;
    size_t manifest_size = 0u;
    if (source_read_descriptor(source, index_descriptor, OCI_JSON_MAX,
            &manifest_bytes, &manifest_size) != 0) {
        oci_error_report(error, OCI_ERROR_PROTOCOL,
            "manifest %s is absent, oversized or does not match its digest",
            index_descriptor->digest);
        return -1;
    }
    maelys_json_document_t *manifest = oci_json_parse_object(
        manifest_bytes, manifest_size, OCI_JSON_TOKENS_MAX);
    free(manifest_bytes);
    maelys_json_value_t manifest_root = maelys_json_document_root(manifest);
    char manifest_media_type[OCI_MEDIA_TYPE_SIZE] = {0};
    int result = -1;
    maelys_json_value_t config_value;
    maelys_json_value_t layers;
    size_t count = 0u;
    if (!manifest || !oci_manifest_media_type_supported(index_descriptor->media_type) ||
        !oci_json_u64_is(manifest, manifest_root, "schemaVersion", 2u) ||
        oci_json_copy_string(manifest, manifest_root, "mediaType",
            manifest_media_type, sizeof(manifest_media_type), 0) != 0 ||
        (manifest_media_type[0] && strcmp(manifest_media_type,
            index_descriptor->media_type) != 0)) {
        oci_error_report(error, OCI_ERROR_PROTOCOL,
            "manifest %s is not a supported schema 2 image manifest",
            index_descriptor->digest);
        goto done;
    }
    if (maelys_json_object_get(manifest, manifest_root, "config", &config_value) !=
            MAELYS_JSON_OK ||
        oci_descriptor_parse(manifest, config_value, 0, &out->config) != 0 ||
        !oci_config_media_type_supported(out->config.media_type)) {
        oci_error_report(error, OCI_ERROR_PROTOCOL,
            "manifest %s has an invalid config descriptor",
            index_descriptor->digest);
        goto done;
    }
    if (maelys_json_object_get(manifest, manifest_root, "layers", &layers) !=
            MAELYS_JSON_OK ||
        maelys_json_array_size(manifest, layers, &count) != MAELYS_JSON_OK ||
        count > OCI_LAYER_MAX) {
        oci_error_report(error, OCI_ERROR_PROTOCOL,
            "manifest %s must declare at most %u layers",
            index_descriptor->digest, OCI_LAYER_MAX);
        goto done;
    }
    out->layers = calloc(count ? count : 1u, sizeof(*out->layers));
    if (!out->layers) {
        oci_error_report(error, OCI_ERROR_MEMORY, "out of memory");
        goto done;
    }
    out->layer_count = count;
    for (size_t i = 0u; i < count; ++i) {
        maelys_json_value_t layer;
        if (maelys_json_array_get(manifest, layers, i, &layer) !=
                MAELYS_JSON_OK ||
            oci_descriptor_parse(manifest, layer, 0, &out->layers[i]) != 0 ||
            !oci_layer_media_type_supported(out->layers[i].media_type)) {
            oci_error_report(error, OCI_ERROR_PROTOCOL,
                "manifest %s layer %zu has an invalid or unsupported descriptor",
                index_descriptor->digest, i);
            goto done;
        }
    }
    if (!oci_manifest_staging_valid(out)) {
        oci_error_report(error, OCI_ERROR_PROTOCOL,
            "manifest %s exceeds the cumulative staging byte limit",
            index_descriptor->digest);
        goto done;
    }
    unsigned char *config_bytes = NULL;
    size_t config_size = 0u;
    if (source_read_descriptor(source, &out->config, OCI_JSON_MAX,
            &config_bytes, &config_size) != 0) {
        oci_error_report(error, OCI_ERROR_PROTOCOL,
            "config %s is absent, oversized or does not match its digest",
            out->config.digest);
        goto done;
    }
    result = oci_config_parse(config_bytes, config_size, out, error);
    free(config_bytes);
    if (result != 0)
        oci_error_report(error, OCI_ERROR_PROTOCOL,
            "config %s has an invalid platform or rootfs DiffIDs, or disagrees with the index",
            out->config.digest);

done:
    maelys_json_document_release(manifest);
    if (result != 0) oci_manifest_clear(out);
    return result;
}

static int read_layout_marker(const oci_source_t *source, oci_error_t *error) {
    unsigned char *layout_bytes = NULL;
    size_t layout_size = 0u;
    if (source_read(source, "oci-layout", OCI_JSON_MAX,
            &layout_bytes, &layout_size) != 0) {
        oci_error_report(error, OCI_ERROR_PROTOCOL,
            "source lacks a readable oci-layout marker");
        return -1;
    }
    maelys_json_document_t *layout = oci_json_parse_object(
        layout_bytes, layout_size, OCI_JSON_TOKENS_MAX);
    free(layout_bytes);
    maelys_json_value_t layout_root = maelys_json_document_root(layout);
    char layout_version[32];
    int valid = layout && oci_json_copy_string(layout, layout_root,
        "imageLayoutVersion", layout_version, sizeof(layout_version), 1) == 0 &&
        strcmp(layout_version, "1.0.0") == 0;
    maelys_json_document_release(layout);
    if (!valid)
        oci_error_report(error, OCI_ERROR_PROTOCOL,
            "oci-layout must declare imageLayoutVersion 1.0.0");
    return valid ? 0 : -1;
}

static int descriptor_is_unsupported_sibling(
    const oci_descriptor_t *descriptor) {
    if (!descriptor->os[0] ||
        strcmp(descriptor->os, "unknown") == 0 ||
        strcmp(descriptor->architecture, "unknown") == 0)
        return 0;
    return !oci_platform_parts_supported(
            descriptor->os, descriptor->architecture) ||
        !oci_platform_variant_supported(
            descriptor->architecture, descriptor->variant);
}

typedef struct inspection_walk {
    const oci_source_t *source;
    oci_manifest_t *items;
    size_t count;
    size_t descriptors;
    char ancestors[OCI_INDEX_DEPTH_MAX][OCI_DIGEST_SIZE];
    oci_error_t *error;
} inspection_walk_t;

/* A platform on an index descriptor constrains its entire subtree. Keep
 * that constraint when a child omits its platform; never select a config
 * of another architecture through an apparently compatible index. */
static int inherit_platform(oci_descriptor_t *child,
    const oci_descriptor_t *parent, oci_error_t *error) {
    if (!parent || !parent->os[0]) return 0;
    if (child->os[0] && (strcmp(child->os, parent->os) != 0 ||
            strcmp(child->architecture, parent->architecture) != 0)) {
        oci_error_report(error, OCI_ERROR_PROTOCOL,
            "descriptor %s disagrees with its parent index platform",
            child->digest);
        return -1;
    }
    if (!child->os[0]) {
        memcpy(child->os, parent->os, sizeof(child->os));
        memcpy(child->architecture, parent->architecture,
            sizeof(child->architecture));
        memcpy(child->variant, parent->variant, sizeof(child->variant));
    }
    return 0;
}

static int inspect_index(inspection_walk_t *walk,
    const unsigned char *bytes, size_t size,
    const oci_descriptor_t *parent, size_t depth) {
    maelys_json_document_t *index = oci_json_parse_object(
        bytes, size, OCI_JSON_TOKENS_MAX);
    maelys_json_value_t root = maelys_json_document_root(index);
    maelys_json_value_t manifests;
    char media_type[OCI_MEDIA_TYPE_SIZE] = {0};
    size_t count = 0u;
    int result = -1;
    if (!index || !oci_json_u64_is(index, root, "schemaVersion", 2u) ||
        oci_json_copy_string(index, root, "mediaType", media_type,
            sizeof(media_type), 0) != 0 ||
        (media_type[0] && (!oci_index_media_type_supported(media_type) ||
            (parent && strcmp(media_type, parent->media_type) != 0))) ||
        maelys_json_object_get(index, root, "manifests", &manifests) !=
            MAELYS_JSON_OK ||
        maelys_json_array_size(index, manifests, &count) != MAELYS_JSON_OK ||
        count == 0u || count > OCI_MANIFEST_MAX - walk->descriptors) {
        oci_error_report(walk->error, OCI_ERROR_PROTOCOL,
            "OCI index must be schema 2 with supported media type and 1 to %u "
            "descriptors in the complete traversal", OCI_MANIFEST_MAX);
        goto done;
    }
    walk->descriptors += count;
    result = 0;
    for (size_t i = 0u; result == 0 && i < count; ++i) {
        oci_descriptor_t descriptor;
        maelys_json_value_t value;
        if (maelys_json_array_get(index, manifests, i, &value) !=
                MAELYS_JSON_OK ||
            oci_descriptor_parse(index, value, 0, &descriptor) != 0) {
            oci_error_report(walk->error, OCI_ERROR_PROTOCOL,
                "OCI index entry %zu is not a valid descriptor", i);
            result = -1;
            break;
        }
        if (oci_descriptor_is_buildx_attestation(&descriptor) ||
            descriptor_is_unsupported_sibling(&descriptor)) continue;
        if (inherit_platform(&descriptor, parent, walk->error) != 0) {
            result = -1;
            break;
        }
        if (oci_index_media_type_supported(descriptor.media_type)) {
            if (depth == OCI_INDEX_DEPTH_MAX) {
                oci_error_report(walk->error, OCI_ERROR_PROTOCOL,
                    "OCI index depth exceeds %u nested indexes",
                    OCI_INDEX_DEPTH_MAX);
                result = -1;
                break;
            }
            for (size_t j = 0u; j < depth; ++j) {
                if (strcmp(walk->ancestors[j], descriptor.digest) == 0) {
                    oci_error_report(walk->error, OCI_ERROR_PROTOCOL,
                        "cyclic OCI index graph at %s", descriptor.digest);
                    result = -1;
                    break;
                }
            }
            if (result != 0) break;
            unsigned char *child_bytes = NULL;
            size_t child_size = 0u;
            if (source_read_descriptor(walk->source, &descriptor, OCI_JSON_MAX,
                    &child_bytes, &child_size) != 0) {
                oci_error_report(walk->error, OCI_ERROR_PROTOCOL,
                    "index %s is absent, oversized or does not match its digest",
                    descriptor.digest);
                result = -1;
                break;
            }
            memcpy(walk->ancestors[depth], descriptor.digest, OCI_DIGEST_SIZE);
            result = inspect_index(walk, child_bytes, child_size,
                &descriptor, depth + 1u);
            free(child_bytes);
        } else {
            oci_manifest_t item;
            oci_error_t sibling = OCI_ERROR_INIT;
            int inspected = oci_inspect_manifest(walk->source, &descriptor,
                &item, &sibling);
            if (inspected == OCI_CONFIG_OK) {
                size_t duplicate = 0u;
                while (duplicate < walk->count &&
                    strcmp(walk->items[duplicate].manifest.digest,
                        item.manifest.digest) != 0) ++duplicate;
                if (duplicate == walk->count)
                    walk->items[walk->count++] = item;
                else
                    oci_manifest_clear(&item);
            } else if (inspected != OCI_CONFIG_UNSUPPORTED) {
                oci_error_report(walk->error, sibling.kind, "%s",
                    sibling.message ? sibling.message : "invalid image manifest");
                result = -1;
            }
            oci_error_clear(&sibling);
        }
    }
done:
    maelys_json_document_release(index);
    return result;
}

int oci_inspect(
    const oci_source_t *source, oci_manifest_t **out_items,
    size_t *out_count, oci_error_t *error) {
    *out_items = NULL;
    *out_count = 0u;
    if (read_layout_marker(source, error) != 0) return -1;
    unsigned char *bytes = NULL;
    size_t size = 0u;
    if (source_read(source, "index.json", OCI_JSON_MAX, &bytes, &size) != 0) {
        oci_error_report(error, OCI_ERROR_PROTOCOL,
            "source lacks a readable index.json");
        return -1;
    }
    inspection_walk_t walk = {.source = source, .error = error};
    walk.items = calloc(OCI_MANIFEST_MAX, sizeof(*walk.items));
    int result = -1;
    if (!walk.items)
        oci_error_report(error, OCI_ERROR_MEMORY, "out of memory");
    else
        result = inspect_index(&walk, bytes, size, NULL, 0u);
    free(bytes);
    if (result == 0 && walk.count == 0u) {
        oci_error_report(error, OCI_ERROR_PROTOCOL,
            "index.json declares no runnable manifest");
        result = -1;
    }
    if (result != 0) {
        for (size_t i = 0u; i < walk.count; ++i)
            oci_manifest_clear(&walk.items[i]);
        free(walk.items);
        return -1;
    }
    *out_items = walk.items;
    *out_count = walk.count;
    return 0;
}

oci_document_t *oci_inspection_document(
    const char *source_path, const oci_manifest_t *items, size_t count) {
    oci_document_t *root = oci_document_object();
    oci_document_t *array = oci_document_array();
    if (!root || !array ||
        oci_document_put(root, "schema", oci_document_string(OCI_SCHEMA_INSPECTION)) != 0 ||
        oci_document_put(root, "source", oci_document_string(source_path)) != 0 ||
        oci_document_set(root, "manifests", array) != 0) {
        oci_document_release(root);
        oci_document_release(array);
        return NULL;
    }
    oci_document_release(array); /* root owns the retained array. */
    for (size_t i = 0u; i < count; ++i) {
        char platform[OCI_PLATFORM_SIZE];
        uint64_t layer_bytes = 0u;
        for (size_t j = 0u; j < items[i].layer_count; ++j) {
            if (UINT64_MAX - layer_bytes < items[i].layers[j].size) {
                oci_document_release(root);
                return NULL;
            }
            layer_bytes += items[i].layers[j].size;
        }
        oci_document_t *item = oci_manifest_platform(&items[i], platform) == 0 ? OCI_DOCUMENT_OBJECT(
            {"digest", oci_document_string(items[i].manifest.digest)},
            {"platform", oci_document_string(platform)},
            {"configDigest", oci_document_string(items[i].config.digest)},
            {"compressedLayerBytes", oci_document_integer((int64_t)layer_bytes)},
            {"layerCount", oci_document_integer((int64_t)items[i].layer_count)}) : NULL;
        if (!item || oci_document_append(array, item) != 0) {
            oci_document_release(root);
            return NULL;
        }
    }
    return root;
}

/* ---- store path helpers ------------------------------------------------------- */

static int remove_tree_entry(
    const char *path, const struct stat *status, int type, struct FTW *walk) {
    (void)status;
    (void)type;
    (void)walk;
    return remove(path);
}

/* Same, but the root of the walk stays for the caller's rmdir_same. */
static int remove_tree_content(
    const char *path, const struct stat *status, int type, struct FTW *walk) {
    (void)status;
    (void)type;
    return walk->level == 0 ? 0 : remove(path);
}

int remove_filesystem_tree(const char *path) {
    return path && path[0]
        ? nftw(path, remove_tree_entry, 32, FTW_DEPTH | FTW_PHYS) : -1;
}

int remove_filesystem_tree_contents(const char *path) {
    return path && path[0]
        ? nftw(path, remove_tree_content, 32, FTW_DEPTH | FTW_PHYS) : -1;
}

int ensure_private_child(
    const char *parent, const char *name, char output[PATH_MAX]) {
    return maelys_oci_store_ensure_private_directory(
        parent, name, output, PATH_MAX);
}

int fsync_directory(const char *path) {
    return maelys_oci_store_fsync_directory(path);
}

int store_path_open(
    const char *store_argument, char **out_canonical, int allow_missing) {
    *out_canonical = NULL;
    if (!store_argument || store_argument[0] != '/') return -1;
    struct stat status;
    if (lstat(store_argument, &status) != 0)
        return allow_missing && errno == ENOENT ? 1 : -1;
    return maelys_oci_store_open_root(store_argument, 0, out_canonical);
}

int private_directory(const char *path) {
    return maelys_oci_store_private_directory(path);
}

int digest_from_directory_name(const char *name, char output[OCI_DIGEST_SIZE]) {
    if (!oci_digest_hex_valid(name)) return -1;
    memcpy(output, OCI_DIGEST_PREFIX, OCI_DIGEST_PREFIX_SIZE);
    memcpy(output + OCI_DIGEST_PREFIX_SIZE, name, 64u);
    output[OCI_DIGEST_SIZE - 1u] = '\0';
    return 0;
}

/* Reads artifact.json as the private, read-only, singly linked file the
 * import published, bounded by OCI_JSON_MAX; NULL when it is anything else. */
static oci_document_t *read_artifact_document(const char *path) {
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    struct stat status;
    if (fd < 0 || fstat(fd, &status) != 0 ||
        !S_ISREG(status.st_mode) || status.st_uid != geteuid() ||
        (status.st_mode & 0777) != 0400 || status.st_nlink != 1) {
        if (fd >= 0) (void)maelys_sys_fd_close(&fd);
        return NULL;
    }
    unsigned char *bytes = NULL;
    size_t size = 0u;
    int read_result = oci_read_regular_bounded(fd, OCI_JSON_MAX, &bytes, &size);
    (void)maelys_sys_fd_close(&fd);
    if (read_result != 0) return NULL;
    oci_document_t *metadata = load_json(bytes, size);
    free(bytes);
    return metadata;
}

int artifact_schema_is_former(const char *schema) {
    return schema && (strcmp(schema, OCI_SCHEMA_ARTIFACT_FORMER_V5) == 0 ||
        strcmp(schema, OCI_SCHEMA_ARTIFACT_FORMER_V6) == 0);
}

oci_document_t *load_former_artifact_schema(
    const char *path, const char **out_schema) {
    *out_schema = NULL;
    oci_document_t *metadata = read_artifact_document(path);
    if (!metadata) return NULL;
    const char *schema = oci_document_string_value(oci_document_get(metadata, "schema"));
    if (!artifact_schema_is_former(schema)) {
        oci_document_release(metadata);
        return NULL;
    }
    *out_schema = schema;
    return metadata;
}

oci_document_t *load_artifact_metadata(
    const char *path, const char *expected_digest,
    const char *expected_platform) {
    oci_document_t *metadata = read_artifact_document(path);
    if (!metadata) return NULL;
    const char *schema = oci_document_string_value(oci_document_get(metadata, "schema"));
    const char *digest = oci_document_string_value(oci_document_get(metadata, "manifestDigest"));
    const char *platform = oci_document_string_value(oci_document_get(metadata, "platform"));
    const char *root_digest = oci_document_string_value(oci_document_get(metadata, "rootDigest"));
    const char *rootfs_tar_digest =
        oci_document_string_value(oci_document_get(metadata, "rootfsTarDigest"));
    const char *rootfs_tar_format =
        oci_document_string_value(oci_document_get(metadata, "rootfsTarFormat"));
    oci_document_t *root_bytes = oci_document_get(metadata, "rootBytes");
    oci_document_t *rootfs_tar_bytes = oci_document_get(metadata, "rootfsTarBytes");
    char platform_directory[OCI_PLATFORM_SIZE];
    if (!schema || strcmp(schema, OCI_SCHEMA_ARTIFACT) != 0 ||
        !oci_digest_valid(digest) || !oci_digest_valid(root_digest) ||
        !oci_digest_valid(rootfs_tar_digest) || !rootfs_tar_format ||
        strcmp(rootfs_tar_format, "pax-restricted") != 0 ||
        !platform || oci_platform_to_directory(platform, platform_directory) != 0 ||
        !oci_document_is_integer(root_bytes) || oci_document_integer_value(root_bytes) < 0 ||
        !oci_document_is_integer(rootfs_tar_bytes) ||
        oci_document_integer_value(rootfs_tar_bytes) <= 0 ||
        (expected_digest && strcmp(digest, expected_digest) != 0) ||
        (expected_platform && strcmp(platform, expected_platform) != 0)) {
        oci_document_release(metadata);
        return NULL;
    }
    return metadata;
}
