/* SPDX-License-Identifier: MPL-2.0 */
/*
 * OCI vocabulary shared by the materializer and the registry puller: digest
 * and platform syntax, media types and the JSON descriptor model.
 */
#include "src/common/internal.h"

#include <stdlib.h>
#include <string.h>

void oci_manifest_clear(oci_manifest_t *manifest) {
    if (!manifest) return;
    free(manifest->layers);
    free(manifest->diff_ids);
    memset(manifest, 0, sizeof(*manifest));
}

int oci_manifest_platform(
    const oci_manifest_t *manifest, char out[OCI_PLATFORM_SIZE]) {
    return oci_snprintf(out, OCI_PLATFORM_SIZE, "%s/%s", manifest->os,
        manifest->architecture) < 0 ? -1 : 0;
}

int oci_manifest_staging_valid(const oci_manifest_t *manifest) {
    uint64_t total = manifest->manifest.size;
    if (total > OCI_STAGING_MAX || manifest->config.size > OCI_STAGING_MAX - total)
        return 0;
    total += manifest->config.size;
    for (size_t i = 0u; i < manifest->layer_count; ++i) {
        if (manifest->layers[i].size > OCI_STAGING_MAX - total) return 0;
        total += manifest->layers[i].size;
    }
    return 1;
}

int oci_digest_valid(const char *digest) {
    return digest &&
        strncmp(digest, OCI_DIGEST_PREFIX, OCI_DIGEST_PREFIX_SIZE) == 0 &&
        oci_hex_valid(digest + OCI_DIGEST_PREFIX_SIZE, 64u);
}

int oci_digest_hex_valid(const char *hex) {
    return oci_hex_valid(hex, 64u);
}

int oci_reference_digest(const char *reference, char out[OCI_DIGEST_SIZE]) {
    if (!reference || !reference[0]) return -1;
    const char *digest = strrchr(reference, '@');
    digest = digest ? digest + 1u : reference;
    if (!oci_digest_valid(digest)) return -1;
    memcpy(out, digest, OCI_DIGEST_SIZE);
    return 0;
}

int oci_relative_path_valid(const char *path) {
    if (!path || !path[0] || path[0] == '/' || strlen(path) > 4096u)
        return 0;
    const char *cursor = path;
    while (*cursor) {
        const char *slash = strchr(cursor, '/');
        size_t size = slash ? (size_t)(slash - cursor) : strlen(cursor);
        if (size == 0u || size > 255u ||
            (size == 1u && cursor[0] == '.') ||
            (size == 2u && cursor[0] == '.' && cursor[1] == '.')) return 0;
        cursor += size;
        if (*cursor == '/') ++cursor;
    }
    return 1;
}

int oci_directory_entry_name_valid(const char *name) {
    if (!name || !name[0] || strlen(name) > 255u ||
        oci_directory_entry_is_dot(name)) return 0;
    for (const unsigned char *cursor = (const unsigned char *)name;
         *cursor; ++cursor) {
        if (!((*cursor >= 'a' && *cursor <= 'z') ||
              (*cursor >= '0' && *cursor <= '9') || *cursor == '-')) return 0;
    }
    return 1;
}

int oci_platform_to_directory(
    const char *platform, char out[OCI_PLATFORM_SIZE]) {
    if (!platform || !platform[0] || strlen(platform) >= OCI_PLATFORM_SIZE)
        return -1;
    const char *slash = strchr(platform, '/');
    if (!slash || slash == platform || !slash[1] || strchr(slash + 1u, '/'))
        return -1;
    size_t prefix = (size_t)(slash - platform);
    char os[OCI_PLATFORM_PART_SIZE];
    char architecture[OCI_PLATFORM_PART_SIZE];
    if (prefix >= sizeof(os) || strlen(slash + 1u) >= sizeof(architecture))
        return -1;
    memcpy(os, platform, prefix);
    os[prefix] = '\0';
    memcpy(architecture, slash + 1u, strlen(slash + 1u) + 1u);
    if (!oci_directory_entry_name_valid(os) ||
        !oci_directory_entry_name_valid(architecture)) return -1;
    return oci_snprintf(out, OCI_PLATFORM_SIZE, "%s-%s", os, architecture) < 0
        ? -1 : 0;
}

int oci_platform_parts_supported(const char *os, const char *architecture) {
    return os && architecture && strcmp(os, "linux") == 0 &&
        (strcmp(architecture, "arm64") == 0 ||
         strcmp(architecture, "amd64") == 0);
}

int oci_manifest_media_type_supported(const char *media_type) {
    return media_type &&
        (strcmp(media_type, "application/vnd.oci.image.manifest.v1+json") == 0 ||
         strcmp(media_type,
                "application/vnd.docker.distribution.manifest.v2+json") == 0);
}

int oci_index_media_type_supported(const char *media_type) {
    return media_type &&
        (strcmp(media_type, "application/vnd.oci.image.index.v1+json") == 0 ||
         strcmp(media_type,
                "application/vnd.docker.distribution.manifest.list.v2+json") == 0);
}

int oci_config_media_type_supported(const char *media_type) {
    return media_type &&
        (strcmp(media_type, "application/vnd.oci.image.config.v1+json") == 0 ||
         strcmp(media_type,
                "application/vnd.docker.container.image.v1+json") == 0);
}

int oci_layer_media_type_supported(const char *media_type) {
    static const char *const supported[] = {
        "application/vnd.oci.image.layer.v1.tar",
        "application/vnd.oci.image.layer.v1.tar+gzip",
        "application/vnd.oci.image.layer.v1.tar+zstd",
        "application/vnd.oci.image.layer.nondistributable.v1.tar",
        "application/vnd.oci.image.layer.nondistributable.v1.tar+gzip",
        "application/vnd.oci.image.layer.nondistributable.v1.tar+zstd",
        "application/vnd.docker.image.rootfs.diff.tar",
        "application/vnd.docker.image.rootfs.diff.tar.gzip",
        "application/vnd.docker.image.rootfs.foreign.diff.tar.gzip"
    };
    if (!media_type) return 0;
    for (size_t i = 0u; i < sizeof(supported) / sizeof(supported[0]); ++i)
        if (strcmp(media_type, supported[i]) == 0) return 1;
    return 0;
}

int oci_media_type_equal(const char *header_value, const char *expected) {
    if (!header_value || !expected) return 0;
    size_t length = strcspn(header_value, ";");
    while (length && (header_value[length - 1u] == ' ' ||
                      header_value[length - 1u] == '\t')) --length;
    return strlen(expected) == length &&
        memcmp(header_value, expected, length) == 0;
}

maelys_json_document_t *oci_json_parse_object(
    const unsigned char *bytes, size_t size, size_t maximum_tokens) {
    maelys_json_limits_t limits = {
        .maximum_bytes = OCI_JSON_MAX,
        .maximum_depth = 64u,
        .maximum_tokens = maximum_tokens
    };
    maelys_json_document_t *document = NULL;
    maelys_json_error_t error;
    if (size > OCI_JSON_MAX ||
        maelys_json_document_parse(bytes, size, MAELYS_JSON_PROFILE_RFC8259,
            &limits, &document, &error) != MAELYS_JSON_OK ||
        maelys_json_value_type(document, maelys_json_document_root(document)) !=
            MAELYS_JSON_TYPE_OBJECT) {
        maelys_json_document_release(document);
        return NULL;
    }
    return document;
}

int oci_json_copy_string(
    const maelys_json_document_t *document, maelys_json_value_t object,
    const char *name, char *output, size_t capacity, int required) {
    maelys_json_value_t value;
    maelys_json_result_t found = maelys_json_object_get(
        document, object, name, &value);
    if (found == MAELYS_JSON_ERR_NOT_FOUND && !required) {
        output[0] = '\0';
        return 0;
    }
    maelys_json_view_t view;
    if (found != MAELYS_JSON_OK ||
        maelys_json_value_string(document, value, &view) != MAELYS_JSON_OK ||
        !view.size || view.size >= capacity || memchr(view.data, '\0', view.size))
        return -1;
    memcpy(output, view.data, view.size);
    output[view.size] = '\0';
    return 0;
}

int oci_json_optional_string(
    const maelys_json_document_t *document, maelys_json_value_t object,
    const char *name, maelys_json_view_t *out_view) {
    maelys_json_value_t value;
    maelys_json_result_t found = maelys_json_object_get(
        document, object, name, &value);
    if (found == MAELYS_JSON_ERR_NOT_FOUND) return 0;
    if (found != MAELYS_JSON_OK ||
        maelys_json_value_string(document, value, out_view) != MAELYS_JSON_OK ||
        !out_view->size || memchr(out_view->data, '\0', out_view->size))
        return -1;
    return 1;
}

int oci_json_u64_is(
    const maelys_json_document_t *document, maelys_json_value_t object,
    const char *name, uint64_t expected) {
    maelys_json_value_t value;
    uint64_t observed;
    return maelys_json_object_get(document, object, name, &value) ==
            MAELYS_JSON_OK &&
        maelys_json_value_u64(document, value, &observed) == MAELYS_JSON_OK &&
        observed == expected;
}

int oci_descriptor_parse(
    const maelys_json_document_t *document, maelys_json_value_t value,
    int platform_required, oci_descriptor_t *out) {
    memset(out, 0, sizeof(*out));
    if (maelys_json_value_type(document, value) != MAELYS_JSON_TYPE_OBJECT ||
        oci_json_copy_string(document, value, "digest", out->digest,
            sizeof(out->digest), 1) != 0 ||
        oci_json_copy_string(document, value, "mediaType", out->media_type,
            sizeof(out->media_type), 1) != 0 || !oci_digest_valid(out->digest))
        return -1;
    maelys_json_value_t member;
    uint64_t descriptor_size;
    if (maelys_json_object_get(document, value, "size", &member) !=
            MAELYS_JSON_OK ||
        maelys_json_value_u64(document, member, &descriptor_size) !=
            MAELYS_JSON_OK || descriptor_size > OCI_DESCRIPTOR_MAX)
        return -1;
    out->size = descriptor_size;
    maelys_json_result_t platform_result = maelys_json_object_get(
        document, value, "platform", &member);
    if (platform_required && platform_result != MAELYS_JSON_OK) return -1;
    if (platform_result == MAELYS_JSON_OK) {
        if (maelys_json_value_type(document, member) != MAELYS_JSON_TYPE_OBJECT ||
            oci_json_copy_string(document, member, "os", out->os,
                sizeof(out->os), 1) != 0 ||
            oci_json_copy_string(document, member, "architecture",
                out->architecture, sizeof(out->architecture), 1) != 0 ||
            oci_json_copy_string(document, member, "variant",
                out->variant, sizeof(out->variant), 0) != 0)
            return -1;
    } else if (platform_result != MAELYS_JSON_ERR_NOT_FOUND) {
        return -1;
    }
    maelys_json_result_t annotations_result = maelys_json_object_get(
        document, value, "annotations", &member);
    if (annotations_result == MAELYS_JSON_OK) {
        if (maelys_json_value_type(document, member) != MAELYS_JSON_TYPE_OBJECT ||
            oci_json_copy_string(document, member,
                "vnd.docker.reference.type", out->reference_type,
                sizeof(out->reference_type), 0) != 0 ||
            oci_json_copy_string(document, member,
                "vnd.docker.reference.digest", out->reference_digest,
                sizeof(out->reference_digest), 0) != 0)
            return -1;
    } else if (annotations_result != MAELYS_JSON_ERR_NOT_FOUND) {
        return -1;
    }
    return 0;
}

int oci_descriptor_is_buildx_attestation(const oci_descriptor_t *descriptor) {
    return descriptor &&
        oci_manifest_media_type_supported(descriptor->media_type) &&
        strcmp(descriptor->os, "unknown") == 0 &&
        strcmp(descriptor->architecture, "unknown") == 0 &&
        strcmp(descriptor->reference_type, "attestation-manifest") == 0 &&
        oci_digest_valid(descriptor->reference_digest);
}

/* Bare arm64 denotes ARMv8; bare amd64 denotes the baseline ISA. A richer
 * variant must never silently select an image requiring another ISA. */
int oci_platform_variant_supported(const char *architecture, const char *variant) {
    return !variant[0] ||
        (strcmp(architecture, "arm64") == 0 && strcmp(variant, "v8") == 0) ||
        (strcmp(architecture, "amd64") == 0 && strcmp(variant, "v1") == 0);
}
