/* SPDX-License-Identifier: MPL-2.0 */
/* One sequential local transaction: resolve, publish verified blobs, then
 * call the ordinary materializer while holding the ordered store locks. */
#include "src/puller/internal.h"
#include "src/materializer/internal.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

static int fetch_layers(pull_http_t *http, const pull_reference_t *reference,
    const oci_manifest_t *selected) {
    for (size_t i = 0u; i < selected->layer_count; ++i) {
        int duplicate = 0;
        for (size_t j = 0u; j < i; ++j)
            if (strcmp(selected->layers[j].digest, selected->layers[i].digest) == 0)
                duplicate = 1;
        if (duplicate) continue;
        if (fetch_file_blob(http, reference, &selected->layers[i]) != 0) {
            pull_report(http, OCI_ERROR_PROTOCOL,
                "layer %zu failed digest/size verification", i);
            return -1;
        }
    }
    return 0;
}

static int pull_receipt(const pull_options_t *options, const pull_reference_t *reference,
    const oci_manifest_t *selected, const pull_http_t *http, oci_document_t *document) {
    char resolved_reference[PULL_TARGET_MAX], metadata_path[PATH_MAX];
    const char *artifact = oci_document_string_value(oci_document_get(document, "artifact"));
    if (!artifact || oci_snprintf(metadata_path, sizeof(metadata_path),
            "%s/artifact.json", artifact) < 0 ||
        oci_snprintf(resolved_reference, sizeof(resolved_reference), "%s/%s@%s",
            reference->authority, reference->repository, selected->manifest.digest) < 0)
        return -1;
    oci_document_t *metadata = load_artifact_metadata(metadata_path, selected->manifest.digest, NULL);
    const char *root = metadata ? oci_document_string_value(oci_document_get(metadata, "rootDigest")) : NULL;
    uint64_t bytes = selected->manifest.size + selected->config.size;
    uint64_t count = 2u;
    for (size_t i = 0u; i < selected->layer_count; ++i) {
        int duplicate = 0;
        for (size_t j = 0u; j < i; ++j)
            if (strcmp(selected->layers[i].digest, selected->layers[j].digest) == 0)
                duplicate = 1;
        if (!duplicate) { ++count; bytes += selected->layers[i].size; }
    }
    int result = root &&
        oci_document_put(document, "reference", oci_document_string(resolved_reference)) == 0 &&
        oci_document_put(document, "requestedReference", oci_document_string(options->reference)) == 0 &&
        oci_document_put(document, "requestedDigest", oci_document_string(reference->digest)) == 0 &&
        oci_document_put(document, "resolvedDigest", oci_document_string(selected->manifest.digest)) == 0 &&
        oci_document_put(document, "artifactDigest", oci_document_string(root)) == 0 &&
        oci_document_put(document, "registry", oci_document_string(reference->authority)) == 0 &&
        oci_document_put(document, "repository", oci_document_string(reference->repository)) == 0 &&
        oci_document_put(document, "blobCount", oci_document_integer((int64_t)count)) == 0 &&
        oci_document_put(document, "blobBytes", oci_document_integer((int64_t)bytes)) == 0 &&
        oci_document_put(document, "downloadedBlobs", oci_document_integer((int64_t)http->downloaded_blobs)) == 0 &&
        oci_document_put(document, "cachedBlobs", oci_document_integer((int64_t)http->cached_blobs)) == 0 &&
        oci_document_put(document, "cacheHit", oci_document_boolean(!http->downloaded_blobs)) == 0 &&
        oci_document_delete(document, "mode") == 0 && oci_document_delete(document, "precondition") == 0 ? 0 : -1;
    oci_document_release(metadata);
    return result;
}

int oci_pull(const pull_options_t *options, oci_document_t **out_document, oci_error_t *error) {
    if (!out_document) return -1;
    *out_document = NULL;
    if (!options || !options->store || options->store[0] != '/' ||
        (options->platform && strcmp(options->platform, "linux/arm64") != 0 &&
         strcmp(options->platform, "linux/amd64") != 0) ||
        (options->token_file && options->docker_config)) {
        oci_error_report(error, OCI_ERROR_ARGUMENT, "invalid pull options, platform or absolute store path");
        return -1;
    }
    pull_reference_t reference;
    if (reference_parse(options->reference, &reference) != 0) {
        oci_error_report(error, OCI_ERROR_ARGUMENT, "REFERENCE must be REGISTRY/REPOSITORY@sha256:HEX");
        return -1;
    }
    pull_http_t http = {0};
    char *store = NULL;
    pull_resolved_t resolved = {0};
    maelys_oci_store_lock_t store_lock = MAELYS_OCI_STORE_LOCK_INIT;
    maelys_oci_store_lock_t manifest_lock = MAELYS_OCI_STORE_LOCK_INIT;
    oci_source_t source = {.directory_fd = -1};
    oci_acquisition_lease_t lease = {0};
    int result = http_initialize(&http, options, &reference, error);
    if (result) goto done;
    if (maelys_oci_store_open_root(options->store, 1, &store) != 0 ||
        store_prepare_v2_until(store, http.deadline) != 0 ||
        maelys_oci_store_lock_until(store, "store.lock", 0, http.deadline, &store_lock) != 0) {
        pull_report(&http, OCI_ERROR_STATE, "cannot prepare the private OCI store or acquire its bounded lock");
        result = -1; goto done;
    }
    http.store = store;
    if ((result = pull_resolve(&http, &reference, options->platform, &resolved)) != 0) goto done;
    char lock_name[80];
    if (oci_snprintf(lock_name, sizeof(lock_name), "%s.lock",
            resolved.image.manifest.digest + OCI_DIGEST_PREFIX_SIZE) < 0 ||
        maelys_oci_store_lock_until(store, lock_name, 1, http.deadline, &manifest_lock) != 0) {
        pull_report(&http, OCI_ERROR_IO, "cannot acquire the bounded manifest lock");
        result = -1; goto done;
    }
    if (oci_acquisition_lease_create(store, &resolved.image, http.timeout_ms, &lease) != 0) {
        pull_report(&http, OCI_ERROR_IO, "cannot durably hold an acquisition lease");
        result = -1; goto done;
    }
    if (store_memory_blob(&http, &resolved.image.manifest, resolved.manifest_bytes,
            resolved.manifest_size) != 0 ||
        store_memory_blob(&http, &resolved.image.config, resolved.config_bytes,
            resolved.config_size) != 0 || fetch_layers(&http, &reference, &resolved.image) != 0) {
        pull_report(&http, OCI_ERROR_STATE, "cannot publish the verified descriptor closure");
        result = -1; goto done;
    }
    if ((result = source_open(store, &source, error)) != 0) goto done;
    result = oci_import_locked(&source, &resolved.image, store, &store_lock,
        &manifest_lock, http.deadline, out_document, error);
    if (!result && pull_receipt(options, &reference, &resolved.image, &http, *out_document) != 0) {
        pull_report(&http, OCI_ERROR_MEMORY, "cannot build the pull receipt");
        result = -1;
    }
done:
    source_close(&source);
    if (oci_acquisition_lease_retire(&lease) != 0) {
        pull_report(&http, OCI_ERROR_IO, "cannot retire the acquisition lease by identity");
        result = -1;
    }
    if (result) { oci_document_release(*out_document); *out_document = NULL; }
    resolved_clear(&resolved);
    maelys_oci_store_unlock_manifest(&store_lock, &manifest_lock);
    http_clear(&http);
    reference_clear(&reference);
    free(store);
    return result;
}
