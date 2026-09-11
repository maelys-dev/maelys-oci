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

int oci_inspect(
    const oci_source_t *source, oci_manifest_t **out_items,
    size_t *out_count, oci_error_t *error) {
    *out_items = NULL;
    *out_count = 0u;
    if (read_layout_marker(source, error) != 0) return -1;
    unsigned char *index_bytes = NULL;
    size_t index_size = 0u;
    if (source_read(source, "index.json", OCI_JSON_MAX,
            &index_bytes, &index_size) != 0) {
        oci_error_report(error, OCI_ERROR_PROTOCOL,
            "source lacks a readable index.json");
        return -1;
    }
    maelys_json_document_t *index = oci_json_parse_object(
        index_bytes, index_size, OCI_JSON_TOKENS_MAX);
    free(index_bytes);
    maelys_json_value_t index_root = maelys_json_document_root(index);
    maelys_json_value_t manifests;
    size_t count;
    if (!index || !oci_json_u64_is(index, index_root, "schemaVersion", 2u) ||
        maelys_json_object_get(index, index_root, "manifests", &manifests) !=
            MAELYS_JSON_OK ||
        maelys_json_array_size(index, manifests, &count) != MAELYS_JSON_OK ||
        count == 0u || count > OCI_MANIFEST_MAX) {
        maelys_json_document_release(index);
        oci_error_report(error, OCI_ERROR_PROTOCOL,
            "index.json must be a schema 2 index with 1 to %u manifests",
            OCI_MANIFEST_MAX);
        return -1;
    }
    oci_manifest_t *items = calloc(count, sizeof(*items));
    if (!items) {
        maelys_json_document_release(index);
        oci_error_report(error, OCI_ERROR_MEMORY, "out of memory");
        return -1;
    }
    size_t runnable_count = 0u;
    int result = 0;
    for (size_t i = 0u; result == 0 && i < count; ++i) {
        oci_descriptor_t descriptor;
        maelys_json_value_t descriptor_value;
        if (maelys_json_array_get(index, manifests, i, &descriptor_value) !=
                MAELYS_JSON_OK ||
            oci_descriptor_parse(index, descriptor_value, 0, &descriptor) != 0) {
            oci_error_report(error, OCI_ERROR_PROTOCOL,
                "index.json manifest %zu is not a valid descriptor", i);
            result = -1;
        } else if (oci_descriptor_is_buildx_attestation(&descriptor)) {
            continue;
        } else {
            /* An OCI index may mix Linux images with platforms this product
             * does not materialize. They are valid non-runnable siblings, not
             * a reason to reject an explicitly selected Linux image. */
            if (descriptor_is_unsupported_sibling(&descriptor))
                continue;
            /* The sibling reports into its own error: an unsupported one is
             * skipped without discarding what the caller already accumulated. */
            oci_error_t sibling = OCI_ERROR_INIT;
            int inspected = oci_inspect_manifest(source, &descriptor,
                &items[runnable_count], &sibling);
            if (inspected != OCI_CONFIG_UNSUPPORTED) {
                if (inspected != OCI_CONFIG_OK) {
                    if (sibling.message)
                        oci_error_report(error, sibling.kind, "%s",
                            sibling.message);
                    result = -1;
                } else {
                    ++runnable_count;
                }
            }
            oci_error_clear(&sibling);
        }
    }
    maelys_json_document_release(index);
    if (result == 0 && runnable_count == 0u) {
        oci_error_report(error, OCI_ERROR_PROTOCOL,
            "index.json declares no runnable manifest");
        result = -1;
    }
    if (result != 0) {
        for (size_t j = 0u; j < runnable_count; ++j) oci_manifest_clear(&items[j]);
        free(items);
        return -1;
    }
    *out_items = items;
    *out_count = runnable_count;
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
