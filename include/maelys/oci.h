/* SPDX-License-Identifier: MPL-2.0 */
#ifndef MAELYS_OCI_H
#define MAELYS_OCI_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAELYS_OCI_ABI_VERSION 4u
#define MAELYS_OCI_DIGEST_HEX_SIZE 65u

/* Versioned standalone materialization: image contents have no injected
 * runtime helpers. Older seals require reimport from their OCI source. */
#define MAELYS_OCI_ARTIFACT_SEAL_MAGIC "MWOCI/7"
#define MAELYS_OCI_MATERIALIZER_ID "maelys-oci-materializer/9"
#define MAELYS_OCI_MATERIALIZER_SEAL_LINE \
    "materializer=" MAELYS_OCI_MATERIALIZER_ID "\n"

/* The execution lease a resolved artifact holds: one file under
 * `leases/` of the store, whose liveness the kernel answers through the
 * exclusive lock the handle keeps on it. */
#define MAELYS_OCI_LEASE_SCHEMA "maelys.warden.oci-lease/v3"
#define MAELYS_OCI_LEASE_LIVENESS "kernel-lock"

typedef enum maelys_oci_result {
    MAELYS_OCI_OK = 0,
    MAELYS_OCI_ERR_ARGUMENT = 1,
    MAELYS_OCI_ERR_MEMORY = 2,
    MAELYS_OCI_ERR_IO = 3,
    MAELYS_OCI_ERR_UNSUPPORTED = 4,
    MAELYS_OCI_ERR_PROTOCOL = 5,
    MAELYS_OCI_ERR_ACCESS = 6,
    MAELYS_OCI_ERR_NOT_FOUND = 7,
    MAELYS_OCI_ERR_STATE = 8
} maelys_oci_result_t;

/* Sequential HTTPS acquisition into the local store. Options own copied
 * absolute file paths; setters leave the previous value intact on failure.
 * NULL clears a path. Token and Docker config are mutually exclusive.
 * Handles have no shared mutable global state; a handle is used by one
 * caller at a time. The implementation never prints, invokes a helper or
 * calls exit(). Explicit platform is required for ambiguous indexes.
 *
 * timeout bounds the complete acquisition (including lock waits) and is
 * checked before and after materialization; synchronous filesystem calls
 * and the OS resolver cannot be preempted. A failure may leave verified
 * reusable blobs or a complete sealed artifact, never a partial publication.
 */
#define MAELYS_OCI_PULL_TIMEOUT_MS 300000
#define MAELYS_OCI_PULL_TIMEOUT_MAX_MS 86400000

typedef struct maelys_oci_pull_options maelys_oci_pull_options_t;
typedef struct maelys_oci_pull_result maelys_oci_pull_result_t;
maelys_oci_result_t maelys_oci_pull_options_create(maelys_oci_pull_options_t **out_options);
void maelys_oci_pull_options_release(maelys_oci_pull_options_t **options);
maelys_oci_result_t maelys_oci_pull_options_set_ca_file(maelys_oci_pull_options_t *options, const char *path);
maelys_oci_result_t maelys_oci_pull_options_set_token_file(maelys_oci_pull_options_t *options, const char *path);
maelys_oci_result_t maelys_oci_pull_options_set_docker_config(maelys_oci_pull_options_t *options, const char *path);
maelys_oci_result_t maelys_oci_pull_options_set_timeout_ms(maelys_oci_pull_options_t *options, uint64_t timeout_ms);
/* store is an absolute private path, created when absent. reference is
 * REGISTRY/REPOSITORY@sha256:HEX; platform is NULL, linux/arm64 or linux/amd64.
 * On failure *out_result is NULL. Optional out_error receives an owned
 * diagnostic, released with maelys_oci_error_free(). NULL options uses defaults. */
maelys_oci_result_t maelys_oci_pull(const maelys_oci_pull_options_t *options,
    const char *store, const char *reference, const char *platform,
    maelys_oci_pull_result_t **out_result, char **out_error);
void maelys_oci_pull_result_release(maelys_oci_pull_result_t **result);
/* Borrowed strings valid until release; NULL for a NULL result. Digests carry
 * sha256:. The JSON receipt is canonical and contains no credentials. */
const char *maelys_oci_pull_result_requested_reference(const maelys_oci_pull_result_t *result);
const char *maelys_oci_pull_result_resolved_digest(const maelys_oci_pull_result_t *result);
const char *maelys_oci_pull_result_platform(const maelys_oci_pull_result_t *result);
const char *maelys_oci_pull_result_manifest_digest(const maelys_oci_pull_result_t *result);
const char *maelys_oci_pull_result_config_digest(const maelys_oci_pull_result_t *result);
const char *maelys_oci_pull_result_artifact_digest(const maelys_oci_pull_result_t *result);
const char *maelys_oci_pull_result_artifact_path(const maelys_oci_pull_result_t *result);
const char *maelys_oci_pull_result_receipt_json(const maelys_oci_pull_result_t *result);
uint64_t maelys_oci_pull_result_blob_count(const maelys_oci_pull_result_t *result);
uint64_t maelys_oci_pull_result_blob_bytes(const maelys_oci_pull_result_t *result);
uint64_t maelys_oci_pull_result_downloaded_blobs(const maelys_oci_pull_result_t *result);
uint64_t maelys_oci_pull_result_cached_blobs(const maelys_oci_pull_result_t *result);
int maelys_oci_pull_result_cache_hit(const maelys_oci_pull_result_t *result);

/*
 * A resolved artifact: the sealed members of one image manifest for one
 * platform, held by an execution lease. The handle is opaque; it owns its
 * paths and the kernel lock of its lease. One handle is one lease: two
 * resolutions of the same artifact hold two leases.
 */
typedef struct maelys_oci_artifact maelys_oci_artifact_t;

/* Identity of a sealed member as it was at resolution: POSIX scalars only. */
typedef struct maelys_oci_file_identity {
    dev_t device;
    ino_t inode;
    mode_t mode;
    uint64_t size;
} maelys_oci_file_identity_t;

/*
 * Resolves image_digest ("sha256:HEX") for platform ("linux/arm64" or
 * "linux/amd64") in the configured store (MAELYS_OCI_STORE, else
 * the XDG data directory): under the ordered store locks, checks and
 * hashes every sealed member, publishes the lease `leases/ID.json`
 * (MAELYS_OCI_LEASE_SCHEMA) and takes an exclusive flock(2) on it that the
 * handle holds until release. The lock lives with its close-on-exec
 * descriptor: a caller that forks without exec closes inherited
 * descriptors, or the lease outlives it. Local file systems only, as the
 * lock contract of maelys-system says.
 *
 * On success *out_artifact is owned by the caller and ends with
 * maelys_oci_artifact_release. On failure *out_artifact is NULL and
 * *out_error, when given, receives a message for maelys_oci_error_free.
 */
maelys_oci_result_t maelys_oci_artifact_resolve(
    const char *image_digest,
    const char *platform,
    maelys_oci_artifact_t **out_artifact,
    char **out_error);

/*
 * Hashes the members again and requires the identity and digest seen at
 * resolution; then requires the lease to be the locked file, still at its
 * path, still private, singly linked and read-only, and the lock to be
 * held. ERR_UNSUPPORTED names the member or the lease that changed.
 */
maelys_oci_result_t maelys_oci_artifact_revalidate(
    const maelys_oci_artifact_t *artifact,
    char **out_error);

/*
 * Retires the lease (unlinked by identity while the lock is held, then the
 * directory is synced), releases the lock, frees the handle and sets
 * *artifact to NULL. NULL and an already-NULL handle are no-ops.
 */
void maelys_oci_artifact_release(maelys_oci_artifact_t **artifact);

/* Borrowed strings, valid until release; NULL for a NULL handle. Digests of
 * members are 64 hexadecimal digits without prefix; image and config
 * digests carry their "sha256:" prefix. */
const char *maelys_oci_artifact_image_digest(const maelys_oci_artifact_t *artifact);
const char *maelys_oci_artifact_platform(const maelys_oci_artifact_t *artifact);
const char *maelys_oci_artifact_root_path(const maelys_oci_artifact_t *artifact);
const char *maelys_oci_artifact_rootfs_tar_path(
    const maelys_oci_artifact_t *artifact);
const char *maelys_oci_artifact_root_digest(const maelys_oci_artifact_t *artifact);
const char *maelys_oci_artifact_rootfs_tar_digest(
    const maelys_oci_artifact_t *artifact);
const char *maelys_oci_artifact_config_digest(
    const maelys_oci_artifact_t *artifact);
const char *maelys_oci_artifact_lease_path(const maelys_oci_artifact_t *artifact);

/* Identity of root.ext4 at resolution, for a consumer that binds it into
 * its own contract. Returns -1 for a NULL argument. */
int maelys_oci_artifact_root_identity(
    const maelys_oci_artifact_t *artifact,
    maelys_oci_file_identity_t *out_identity);

void maelys_oci_error_free(char *error);

/* ---- store operations ----------------------------------------------------------------
 *
 * The operations the terminal runs, published so that the terminal is a
 * consumer of this header like any other program. Each returns a result
 * code, an optional owned diagnostic (*out_error, for maelys_oci_error_free)
 * and, on success, an owned document that the caller releases. Nothing here
 * prints, reads argv or calls exit().
 */

/* The environment variable naming the private store, and the default
 * grace of gc in seconds. */
#define MAELYS_OCI_ENV_STORE "MAELYS_OCI_STORE"
#define MAELYS_OCI_GC_DEFAULT_GRACE_SECONDS 86400

/* The default store: $MAELYS_OCI_STORE, else $XDG_DATA_HOME/maelys-oci,
 * else $HOME/.local/share/maelys-oci. Returns -1 when none of them yields an
 * absolute location; the store may not exist yet. */
int maelys_oci_store_default_path(char *output, size_t capacity);

/* 1 when platform is OS/ARCH with lowercase components, 0 otherwise. */
int maelys_oci_platform_valid(const char *platform);

/*
 * A document is the schema-described result of one operation, as the
 * command's JSON Schema in docs/cli-contract.json states it. It is opaque:
 * its canonical JSON text is the only representation, and a member that
 * is an array can be read one item at a time, so that a caller emitting
 * records does not parse the text again.
 */
typedef struct maelys_oci_document maelys_oci_document_t;
/* Canonical JSON of the document, owned by the caller (maelys_oci_text_free);
 * NULL on allocation failure or for a NULL document. */
char *maelys_oci_document_text(const maelys_oci_document_t *document);
/* Number of items of the array member named, 0 when absent or not an array. */
size_t maelys_oci_document_count(
    const maelys_oci_document_t *document, const char *member);
/* Canonical JSON of item index of that array member, owned by the caller;
 * NULL when out of range. */
char *maelys_oci_document_item_text(
    const maelys_oci_document_t *document, const char *member, size_t index);
void maelys_oci_document_release(maelys_oci_document_t **document);
void maelys_oci_text_free(char *text);

/* Reads oci-layout and index.json of a layout directory or tar archive and
 * lists every runnable manifest. */
maelys_oci_result_t maelys_oci_inspect(
    const char *source, maelys_oci_document_t **out_document, char **out_error);

/* Import: plan by default, materialize with apply. Options own copied
 * strings; NULL clears one. The store is an absolute private directory,
 * created on apply when absent; NULL selects the default store. platform
 * ("OS/ARCH") and digest ("sha256:HEX") select one manifest of a source
 * that has several. */
typedef struct maelys_oci_import_options maelys_oci_import_options_t;
maelys_oci_result_t maelys_oci_import_options_create(
    maelys_oci_import_options_t **out_options);
void maelys_oci_import_options_release(maelys_oci_import_options_t **options);
maelys_oci_result_t maelys_oci_import_options_set_store(
    maelys_oci_import_options_t *options, const char *store);
maelys_oci_result_t maelys_oci_import_options_set_platform(
    maelys_oci_import_options_t *options, const char *platform);
maelys_oci_result_t maelys_oci_import_options_set_digest(
    maelys_oci_import_options_t *options, const char *digest);
maelys_oci_result_t maelys_oci_import_options_set_apply(
    maelys_oci_import_options_t *options, int apply);
maelys_oci_result_t maelys_oci_import(
    const maelys_oci_import_options_t *options, const char *source,
    maelys_oci_document_t **out_document, char **out_error);

/* The artifacts published in store: {schema, store, artifacts: [...]}; an
 * absent store lists nothing. store is absolute; NULL selects the default. */
maelys_oci_result_t maelys_oci_store_list(
    const char *store, maelys_oci_document_t **out_document, char **out_error);

/* Verifies the whole store under its shared lock. *out_valid is 1 when the
 * report carries no error; the document is produced whenever the store
 * could be inspected. */
maelys_oci_result_t maelys_oci_store_verify(
    const char *store, maelys_oci_document_t **out_document, int *out_valid,
    char **out_error);

/* Plans (apply 0) or performs (apply 1) garbage collection under the
 * exclusive lock; *out_valid mirrors the integrity of the store. */
maelys_oci_result_t maelys_oci_store_gc(
    const char *store, int apply, uint64_t grace_seconds,
    maelys_oci_document_t **out_document, int *out_valid, char **out_error);

/* Plans or removes one artifact and its source closure. reference ends
 * with @sha256:HEX or is a bare sha256:HEX; platform is required when the
 * manifest has several. */
maelys_oci_result_t maelys_oci_store_remove(
    const char *store, const char *reference, const char *platform, int apply,
    maelys_oci_document_t **out_document, char **out_error);

/* Extracts a sealed portable root archive into an empty private directory.
 * Linux only: ERR_UNSUPPORTED elsewhere, the byte-string names of the
 * archive would alias on a folding file system. */
maelys_oci_result_t maelys_oci_unpack_portable_root(
    const char *archive, const char *destination,
    maelys_oci_document_t **out_document, char **out_error);

#ifdef __cplusplus
}
#endif

#endif
