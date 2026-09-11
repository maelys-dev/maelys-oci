/* SPDX-License-Identifier: MPL-2.0 */
/* External consumer of the store operations: this translation unit uses only
 * the installed header, as the terminal does. argv[1] is an absolute scratch
 * directory that exists and is empty. */
#include <maelys/oci.h>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void absent_store_lists_nothing(const char *scratch) {
    char store[4096];
    assert(snprintf(store, sizeof(store), "%s/absent-store", scratch) > 0);
    maelys_oci_document_t *document = NULL;
    char *error = NULL;
    assert(maelys_oci_store_list(store, &document, &error) == MAELYS_OCI_OK);
    assert(document && !error);
    assert(maelys_oci_document_count(document, "artifacts") == 0u);
    assert(maelys_oci_document_count(document, "nothing") == 0u);
    assert(maelys_oci_document_item_text(document, "artifacts", 0u) == NULL);
    char *text = maelys_oci_document_text(document);
    assert(text && strstr(text, "\"artifacts\"") && strstr(text, store));
    maelys_oci_text_free(text);
    maelys_oci_document_release(&document);
    maelys_oci_document_release(&document);
    assert(!document);
    /* The same store cannot be verified: it does not exist. */
    int valid = 1;
    assert(maelys_oci_store_verify(store, &document, &valid, &error) ==
        MAELYS_OCI_ERR_STATE);
    assert(!document && !valid && error && error[0]);
    maelys_oci_error_free(error);
    error = NULL;
    assert(maelys_oci_store_gc(store, 0, 0u, &document, &valid, &error) ==
        MAELYS_OCI_ERR_STATE);
    assert(!document && error);
    maelys_oci_error_free(error);
}

int main(int argc, char **argv) {
    assert(argc == 2 && argv[1][0] == '/');
    assert(maelys_oci_platform_valid("linux/arm64"));
    assert(!maelys_oci_platform_valid("Linux/arm64"));
    assert(!maelys_oci_platform_valid("linux"));
    assert(!maelys_oci_platform_valid(NULL));

    maelys_oci_import_options_t *options = NULL;
    assert(maelys_oci_import_options_create(NULL) == MAELYS_OCI_ERR_ARGUMENT);
    assert(maelys_oci_import_options_create(&options) == MAELYS_OCI_OK);
    assert(maelys_oci_import_options_set_store(options, "relative") == MAELYS_OCI_ERR_ARGUMENT);
    assert(maelys_oci_import_options_set_store(options, argv[1]) == MAELYS_OCI_OK);
    assert(maelys_oci_import_options_set_store(options, NULL) == MAELYS_OCI_OK);
    assert(maelys_oci_import_options_set_platform(options, "windows") == MAELYS_OCI_ERR_ARGUMENT);
    assert(maelys_oci_import_options_set_platform(options, "linux/amd64") == MAELYS_OCI_OK);
    assert(maelys_oci_import_options_set_digest(options, "sha256:short") == MAELYS_OCI_ERR_ARGUMENT);
    assert(maelys_oci_import_options_set_apply(options, 1) == MAELYS_OCI_OK);

    maelys_oci_document_t *document = NULL;
    char *error = NULL;
    assert(maelys_oci_inspect(NULL, &document, &error) == MAELYS_OCI_ERR_ARGUMENT);
    assert(maelys_oci_inspect("/nonexistent/layout", NULL, &error) == MAELYS_OCI_ERR_ARGUMENT);
    assert(maelys_oci_inspect("/nonexistent/layout", &document, &error) != MAELYS_OCI_OK);
    assert(!document && error && error[0]);
    maelys_oci_error_free(error);
    error = NULL;
    assert(maelys_oci_import(options, "/nonexistent/layout", &document, &error) != MAELYS_OCI_OK);
    assert(!document && error);
    maelys_oci_error_free(error);
    error = NULL;
    maelys_oci_import_options_release(&options);
    maelys_oci_import_options_release(&options);
    assert(!options);

    absent_store_lists_nothing(argv[1]);

    assert(maelys_oci_unpack_portable_root("/nonexistent/rootfs.tar", argv[1],
        &document, &error) != MAELYS_OCI_OK);
    assert(!document);
    maelys_oci_error_free(error);
    maelys_oci_text_free(NULL);
    maelys_oci_document_release(NULL);
    return 0;
}
