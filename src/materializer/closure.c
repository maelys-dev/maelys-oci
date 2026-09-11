/* SPDX-License-Identifier: MPL-2.0 */
/* GC roots come from the verified manifest, never from a cached list alone. */
#include "src/materializer/internal.h"

#include <string.h>

static int string_is(oci_document_t *object, const char *name, const char *expected) {
    const char *actual = oci_document_string_value(oci_document_get(object, name));
    return actual && strcmp(actual, expected) == 0;
}

static int closure_descriptor(oci_document_t *object, oci_descriptor_t *out) {
    const char *digest = oci_document_string_value(oci_document_get(object, "digest"));
    const char *media = oci_document_string_value(oci_document_get(object, "mediaType"));
    oci_document_t *size = oci_document_get(object, "size");
    memset(out, 0, sizeof(*out));
    if (!oci_document_is_object(object) || oci_document_object_size(object) != 3u ||
        !oci_digest_valid(digest) || !media || !media[0] ||
        strlen(media) >= sizeof(out->media_type) || !oci_document_is_integer(size) ||
        oci_document_integer_value(size) < 0 ||
        (uint64_t)oci_document_integer_value(size) > OCI_DESCRIPTOR_MAX) return -1;
    memcpy(out->digest, digest, OCI_DIGEST_SIZE);
    memcpy(out->media_type, media, strlen(media) + 1u);
    out->size = (uint64_t)oci_document_integer_value(size);
    return 0;
}

static int descriptor_matches(oci_document_t *object, const oci_descriptor_t *expected) {
    oci_descriptor_t actual;
    return closure_descriptor(object, &actual) == 0 &&
        strcmp(actual.digest, expected->digest) == 0 &&
        strcmp(actual.media_type, expected->media_type) == 0 &&
        actual.size == expected->size;
}

static int blob_matches(const char *store, const oci_descriptor_t *expected) {
    char path[PATH_MAX], digest[OCI_DIGEST_SIZE];
    uint64_t size = 0u;
    return oci_snprintf(path, sizeof(path), "%s/blobs/sha256/%s", store,
            expected->digest + OCI_DIGEST_PREFIX_SIZE) > 0 &&
        safe_regular_digest(path, 0400, digest, &size) == 0 &&
        strcmp(digest, expected->digest) == 0 && size == expected->size;
}

static int artifact_matches(const char *store, const oci_manifest_t *image,
    const char *platform) {
    char directory[OCI_PLATFORM_SIZE], artifact[PATH_MAX], path[PATH_MAX];
    if (oci_platform_to_directory(platform, directory) != 0 ||
        oci_snprintf(artifact, sizeof(artifact), "%s/objects/%s/%s", store,
            image->manifest.digest + OCI_DIGEST_PREFIX_SIZE, directory) < 0 ||
        oci_snprintf(path, sizeof(path), "%s/artifact.json", artifact) < 0)
        return 0;
    /* A closure can survive a crash before its derived artifact is published.
     * scan_artifacts independently checks the shape of any published object. */
    if (!private_directory(artifact)) return 1;
    oci_document_t *metadata = load_canonical_json_file(path, OCI_JSON_MAX, 1);
    oci_document_t *layers = oci_document_get(metadata, "layerDigests");
    int valid = metadata && string_is(metadata, "schema", OCI_SCHEMA_ARTIFACT) &&
        string_is(metadata, "manifestDigest", image->manifest.digest) &&
        string_is(metadata, "configDigest", image->config.digest) &&
        string_is(metadata, "platform", platform) &&
        oci_document_is_array(layers) &&
        oci_document_array_size(layers) == image->layer_count;
    for (size_t i = 0u; valid && i < image->layer_count; ++i) {
        const char *digest = oci_document_string_value(oci_document_at(layers, i));
        valid = digest && strcmp(digest, image->layers[i].digest) == 0;
    }
    oci_document_release(metadata);
    return valid;
}

int verify_source_closure(const char *store, oci_document_t *closure,
    digest_set_t *reachable, store_report_t *report) {
    oci_descriptor_t descriptor;
    oci_manifest_t image = {0};
    oci_source_t source = {.directory_fd = -1};
    char platform[OCI_PLATFORM_SIZE];
    const char *manifest = oci_document_string_value(
        oci_document_get(closure, "manifestDigest"));
    oci_document_t *layers = oci_document_get(closure, "layers");
    int valid = manifest &&
        closure_descriptor(oci_document_get(closure, "manifest"), &descriptor) == 0 &&
        strcmp(descriptor.digest, manifest) == 0 &&
        oci_manifest_media_type_supported(descriptor.media_type) &&
        descriptor.size <= OCI_JSON_MAX && blob_matches(store, &descriptor) &&
        source_open(store, &source, NULL) == 0 &&
        oci_inspect_manifest(&source, &descriptor, &image, NULL) == 0 &&
        oci_manifest_platform(&image, platform) == 0 &&
        string_is(closure, "platform", platform) &&
        descriptor_matches(oci_document_get(closure, "config"), &image.config) &&
        blob_matches(store, &image.config) && oci_document_is_array(layers) &&
        oci_document_array_size(layers) == image.layer_count;
    for (size_t i = 0u; valid && i < image.layer_count; ++i)
        valid = descriptor_matches(oci_document_at(layers, i), &image.layers[i]) &&
            blob_matches(store, &image.layers[i]);
    if (valid) valid = artifact_matches(store, &image, platform);
    int result = 0;
    if (!valid) {
        result = report_message(report->errors,
            "source closure %s disagrees with its verified manifest, config, layers or artifact",
            manifest ? manifest : "(invalid)");
    } else {
        result = digest_set_add(reachable, image.manifest.digest);
        if (result == 0) result = digest_set_add(reachable, image.config.digest);
        for (size_t i = 0u; result == 0 && i < image.layer_count; ++i)
            result = digest_set_add(reachable, image.layers[i].digest);
    }
    source_close(&source);
    oci_manifest_clear(&image);
    return result;
}
