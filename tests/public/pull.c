/* SPDX-License-Identifier: MPL-2.0 */
/* External consumer: this translation unit uses only the installed header. */
#include <maelys/oci.h>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    maelys_oci_pull_options_t *options = NULL;
    assert(maelys_oci_pull_options_create(NULL) == MAELYS_OCI_ERR_ARGUMENT);
    assert(maelys_oci_pull_options_create(&options) == MAELYS_OCI_OK);
    assert(maelys_oci_pull_options_set_timeout_ms(options, 0u) == MAELYS_OCI_ERR_ARGUMENT);
    assert(maelys_oci_pull_options_set_timeout_ms(options, UINT64_MAX) == MAELYS_OCI_ERR_ARGUMENT);
    assert(maelys_oci_pull_options_set_ca_file(options, "relative") == MAELYS_OCI_ERR_ARGUMENT);
    assert(maelys_oci_pull_options_set_token_file(options, "/private/token") == MAELYS_OCI_OK);
    assert(maelys_oci_pull_options_set_docker_config(options, "/private/config") == MAELYS_OCI_ERR_ARGUMENT);
    assert(maelys_oci_pull_options_set_token_file(options, NULL) == MAELYS_OCI_OK);
    maelys_oci_pull_result_t *result = NULL;
    char *error = NULL;
    assert(maelys_oci_pull(options, "relative", "invalid", NULL, &result, &error) == MAELYS_OCI_ERR_ARGUMENT);
    assert(!result && error && error[0]);
    maelys_oci_error_free(error);
    error = NULL;
    if (argc == 4) {
        size_t size = strlen(argv[3]) + 1u;
        char *ca = malloc(size);
        assert(ca);
        memcpy(ca, argv[3], size);
        assert(maelys_oci_pull_options_set_ca_file(options, ca) == MAELYS_OCI_OK);
        memset(ca, 'X', size - 1u); /* setter must own its copy */
        free(ca);
        assert(maelys_oci_pull(options, argv[1], argv[2], NULL, &result, &error) == MAELYS_OCI_OK);
        assert(!error && result);
        assert(strcmp(maelys_oci_pull_result_requested_reference(result), argv[2]) == 0);
        assert(strcmp(maelys_oci_pull_result_resolved_digest(result), maelys_oci_pull_result_manifest_digest(result)) == 0);
        assert(strncmp(maelys_oci_pull_result_artifact_digest(result), "sha256:", 7u) == 0);
        assert(strncmp(maelys_oci_pull_result_config_digest(result), "sha256:", 7u) == 0);
        assert(maelys_oci_pull_result_blob_count(result) == maelys_oci_pull_result_cached_blobs(result) + maelys_oci_pull_result_downloaded_blobs(result));
        assert(maelys_oci_pull_result_blob_bytes(result) > 0u);
        assert(maelys_oci_pull_result_artifact_path(result)[0] == '/');
        puts(maelys_oci_pull_result_receipt_json(result));
    } else assert(argc == 1);
    maelys_oci_pull_result_release(&result);
    maelys_oci_pull_result_release(&result);
    maelys_oci_pull_options_release(&options);
    maelys_oci_pull_options_release(&options);
    assert(!options && !result);
    return 0;
}
