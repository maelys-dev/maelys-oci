/* SPDX-License-Identifier: MPL-2.0 */
/*
 * The public face of the store operations: what the terminal, and any
 * other consumer of <maelys/oci.h>, runs. Each operation opens the source
 * or resolves the store, calls the private operation and hands back an
 * opaque document; diagnostics cross the boundary as a result code and an
 * owned message.
 */
#include "src/materializer/internal.h"

#include <stdlib.h>
#include <string.h>

struct maelys_oci_document {
    oci_document_t *root;
};

struct maelys_oci_import_options {
    char *store;
    char *platform;
    char *digest;
    int apply;
};

static maelys_oci_result_t public_result(oci_error_kind_t kind) {
    switch (kind) {
    case OCI_ERROR_ARGUMENT: return MAELYS_OCI_ERR_ARGUMENT;
    case OCI_ERROR_MEMORY: return MAELYS_OCI_ERR_MEMORY;
    case OCI_ERROR_NOT_FOUND: return MAELYS_OCI_ERR_NOT_FOUND;
    case OCI_ERROR_ACCESS: return MAELYS_OCI_ERR_ACCESS;
    case OCI_ERROR_PROTOCOL: return MAELYS_OCI_ERR_PROTOCOL;
    case OCI_ERROR_STATE: return MAELYS_OCI_ERR_STATE;
    case OCI_ERROR_UNSUPPORTED: return MAELYS_OCI_ERR_UNSUPPORTED;
    default: return MAELYS_OCI_ERR_IO;
    }
}

/* Moves a private diagnostic across the boundary and clears it. */
static maelys_oci_result_t failed(oci_error_t *error, char **out_error) {
    maelys_oci_result_t result =
        public_result(error->kind == OCI_ERROR_NONE ? OCI_ERROR_IO : error->kind);
    maelys_oci_set_error(out_error, "%s",
        error->message ? error->message : "the operation failed");
    oci_error_clear(error);
    return result;
}

static maelys_oci_result_t wrap(
    oci_document_t *root, maelys_oci_document_t **out_document) {
    maelys_oci_document_t *document = calloc(1u, sizeof(*document));
    if (!document) {
        oci_document_release(root);
        return MAELYS_OCI_ERR_MEMORY;
    }
    document->root = root;
    *out_document = document;
    return MAELYS_OCI_OK;
}

/* Every operation starts here: no stale outputs, a NULL store means the
 * default one, and a store that cannot be located is a state error. */
static maelys_oci_result_t begin(
    const char **store, char *buffer, size_t capacity,
    maelys_oci_document_t **out_document, char **out_error) {
    if (out_error) *out_error = NULL;
    if (!out_document) return MAELYS_OCI_ERR_ARGUMENT;
    *out_document = NULL;
    if (store && !*store) {
        if (maelys_oci_store_default_path(buffer, capacity) != 0) {
            maelys_oci_set_error(out_error,
                "no private OCI store location is configured: pass a store or "
                "set HOME, XDG_DATA_HOME or " MAELYS_OCI_ENV_STORE);
            return MAELYS_OCI_ERR_STATE;
        }
        *store = buffer;
    }
    return MAELYS_OCI_OK;
}

/* ---- documents -------------------------------------------------------------------------- */

char *maelys_oci_document_text(const maelys_oci_document_t *document) {
    return document && document->root ? oci_document_dump(document->root) : NULL;
}

static oci_document_t *array_member(
    const maelys_oci_document_t *document, const char *member) {
    if (!document || !document->root || !member) return NULL;
    oci_document_t *value = oci_document_get(document->root, member);
    return value && oci_document_is_array(value) ? value : NULL;
}

size_t maelys_oci_document_count(
    const maelys_oci_document_t *document, const char *member) {
    oci_document_t *array = array_member(document, member);
    return array ? oci_document_array_size(array) : 0u;
}

char *maelys_oci_document_item_text(
    const maelys_oci_document_t *document, const char *member, size_t index) {
    oci_document_t *array = array_member(document, member);
    if (!array || index >= oci_document_array_size(array)) return NULL;
    return oci_document_dump(oci_document_at(array, index));
}

void maelys_oci_document_release(maelys_oci_document_t **document) {
    if (!document || !*document) return;
    oci_document_release((*document)->root);
    free(*document);
    *document = NULL;
}

void maelys_oci_text_free(char *text) {
    free(text);
}

int maelys_oci_platform_valid(const char *platform) {
    char directory[OCI_PLATFORM_SIZE];
    return platform && oci_platform_to_directory(platform, directory) == 0;
}

/* ---- inspect and import ----------------------------------------------------------------- */

maelys_oci_result_t maelys_oci_inspect(
    const char *source_path, maelys_oci_document_t **out_document,
    char **out_error) {
    maelys_oci_result_t started =
        begin(NULL, NULL, 0u, out_document, out_error);
    if (started != MAELYS_OCI_OK) return started;
    if (!source_path) return MAELYS_OCI_ERR_ARGUMENT;
    oci_error_t error = OCI_ERROR_INIT;
    oci_source_t source;
    if (source_open(source_path, &source, &error) != 0)
        return failed(&error, out_error);
    oci_manifest_t *items = NULL;
    size_t count = 0u;
    oci_document_t *document = NULL;
    if (oci_inspect(&source, &items, &count, &error) == 0)
        document = oci_inspection_document(source.path, items, count);
    for (size_t i = 0u; i < count; ++i) oci_manifest_clear(&items[i]);
    free(items);
    source_close(&source);
    if (error.message) {
        oci_document_release(document);
        return failed(&error, out_error);
    }
    if (!document) {
        maelys_oci_set_error(out_error, "cannot build the inspection document");
        return MAELYS_OCI_ERR_MEMORY;
    }
    return wrap(document, out_document);
}

maelys_oci_result_t maelys_oci_import_options_create(
    maelys_oci_import_options_t **out_options) {
    if (!out_options) return MAELYS_OCI_ERR_ARGUMENT;
    *out_options = calloc(1u, sizeof(**out_options));
    return *out_options ? MAELYS_OCI_OK : MAELYS_OCI_ERR_MEMORY;
}

void maelys_oci_import_options_release(maelys_oci_import_options_t **options) {
    if (!options || !*options) return;
    free((*options)->store);
    free((*options)->platform);
    free((*options)->digest);
    free(*options);
    *options = NULL;
}

/* Copies value into *destination once accepted; NULL clears. */
static maelys_oci_result_t set_string(
    char **destination, const char *value, int accepted) {
    if (value && !accepted) return MAELYS_OCI_ERR_ARGUMENT;
    char *copy = value ? strdup(value) : NULL;
    if (value && !copy) return MAELYS_OCI_ERR_MEMORY;
    free(*destination);
    *destination = copy;
    return MAELYS_OCI_OK;
}

maelys_oci_result_t maelys_oci_import_options_set_store(
    maelys_oci_import_options_t *options, const char *store) {
    if (!options) return MAELYS_OCI_ERR_ARGUMENT;
    return set_string(&options->store, store,
        store && store[0] == '/' && strnlen(store, PATH_MAX) < PATH_MAX);
}

maelys_oci_result_t maelys_oci_import_options_set_platform(
    maelys_oci_import_options_t *options, const char *platform) {
    if (!options) return MAELYS_OCI_ERR_ARGUMENT;
    return set_string(&options->platform, platform,
        maelys_oci_platform_valid(platform));
}

maelys_oci_result_t maelys_oci_import_options_set_digest(
    maelys_oci_import_options_t *options, const char *digest) {
    if (!options) return MAELYS_OCI_ERR_ARGUMENT;
    return set_string(&options->digest, digest, oci_digest_valid(digest));
}

maelys_oci_result_t maelys_oci_import_options_set_apply(
    maelys_oci_import_options_t *options, int apply) {
    if (!options) return MAELYS_OCI_ERR_ARGUMENT;
    options->apply = apply != 0;
    return MAELYS_OCI_OK;
}

maelys_oci_result_t maelys_oci_import(
    const maelys_oci_import_options_t *options, const char *source_path,
    maelys_oci_document_t **out_document, char **out_error) {
    char default_store[PATH_MAX];
    const char *store = options ? options->store : NULL;
    maelys_oci_result_t started = begin(&store, default_store,
        sizeof(default_store), out_document, out_error);
    if (started != MAELYS_OCI_OK) return started;
    if (!source_path) return MAELYS_OCI_ERR_ARGUMENT;
    oci_error_t error = OCI_ERROR_INIT;
    oci_source_t source;
    if (source_open(source_path, &source, &error) != 0)
        return failed(&error, out_error);
    oci_manifest_t *items = NULL;
    size_t count = 0u;
    oci_document_t *document = NULL;
    if (oci_inspect(&source, &items, &count, &error) == 0) {
        oci_import_request_t request = {
            .store = store,
            .platform = options ? options->platform : NULL,
            .digest = options ? options->digest : NULL,
            .apply = options ? options->apply : 0
        };
        (void)oci_import(&source, items, count, &request, &document, &error);
    }
    for (size_t i = 0u; i < count; ++i) oci_manifest_clear(&items[i]);
    free(items);
    source_close(&source);
    if (!document) return failed(&error, out_error);
    oci_error_clear(&error);
    return wrap(document, out_document);
}

/* ---- store ------------------------------------------------------------------------------- */

maelys_oci_result_t maelys_oci_store_list(
    const char *store, maelys_oci_document_t **out_document, char **out_error) {
    char default_store[PATH_MAX];
    maelys_oci_result_t started = begin(&store, default_store,
        sizeof(default_store), out_document, out_error);
    if (started != MAELYS_OCI_OK) return started;
    oci_error_t error = OCI_ERROR_INIT;
    oci_document_t *document = NULL;
    if (oci_store_list(store, &document, &error) != 0)
        return failed(&error, out_error);
    return wrap(document, out_document);
}

maelys_oci_result_t maelys_oci_store_verify(
    const char *store, maelys_oci_document_t **out_document, int *out_valid,
    char **out_error) {
    char default_store[PATH_MAX];
    maelys_oci_result_t started = begin(&store, default_store,
        sizeof(default_store), out_document, out_error);
    if (started != MAELYS_OCI_OK) return started;
    if (!out_valid) return MAELYS_OCI_ERR_ARGUMENT;
    *out_valid = 0;
    oci_error_t error = OCI_ERROR_INIT;
    oci_document_t *document = NULL;
    if (oci_store_verify(store, &document, out_valid, &error) != 0)
        return failed(&error, out_error);
    return wrap(document, out_document);
}

maelys_oci_result_t maelys_oci_store_gc(
    const char *store, int apply, uint64_t grace_seconds,
    maelys_oci_document_t **out_document, int *out_valid, char **out_error) {
    char default_store[PATH_MAX];
    maelys_oci_result_t started = begin(&store, default_store,
        sizeof(default_store), out_document, out_error);
    if (started != MAELYS_OCI_OK) return started;
    if (!out_valid) return MAELYS_OCI_ERR_ARGUMENT;
    *out_valid = 0;
    oci_error_t error = OCI_ERROR_INIT;
    oci_document_t *document = NULL;
    if (oci_store_gc(store, apply != 0, grace_seconds, &document, out_valid,
            &error) != 0)
        return failed(&error, out_error);
    return wrap(document, out_document);
}

maelys_oci_result_t maelys_oci_store_remove(
    const char *store, const char *reference, const char *platform, int apply,
    maelys_oci_document_t **out_document, char **out_error) {
    char default_store[PATH_MAX];
    maelys_oci_result_t started = begin(&store, default_store,
        sizeof(default_store), out_document, out_error);
    if (started != MAELYS_OCI_OK) return started;
    if (!reference) return MAELYS_OCI_ERR_ARGUMENT;
    oci_error_t error = OCI_ERROR_INIT;
    oci_document_t *document = NULL;
    if (oci_store_remove(store, reference, platform, apply != 0, &document,
            &error) != 0)
        return failed(&error, out_error);
    return wrap(document, out_document);
}

maelys_oci_result_t maelys_oci_unpack_portable_root(
    const char *archive, const char *destination,
    maelys_oci_document_t **out_document, char **out_error) {
    maelys_oci_result_t started =
        begin(NULL, NULL, 0u, out_document, out_error);
    if (started != MAELYS_OCI_OK) return started;
    if (!archive || !destination) return MAELYS_OCI_ERR_ARGUMENT;
    oci_error_t error = OCI_ERROR_INIT;
    if (oci_unpack_portable_root(archive, destination, &error) != 0)
        return failed(&error, out_error);
    oci_document_t *document = OCI_DOCUMENT_OBJECT(
        {"archive", oci_document_string(archive)},
        {"destination", oci_document_string(destination)});
    if (!document) {
        maelys_oci_set_error(out_error, "cannot build the unpack document");
        return MAELYS_OCI_ERR_MEMORY;
    }
    return wrap(document, out_document);
}
