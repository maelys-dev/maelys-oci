/* SPDX-License-Identifier: MPL-2.0 */
#ifndef MAELYS_OCI_MATERIALIZER_INTERNAL_H
#define MAELYS_OCI_MATERIALIZER_INTERNAL_H

/*
 * Private materializer seam: source access, the logical Linux graph, the
 * deterministic ext4 and tar writers and the store operations. Every
 * operation returns a document or a status and reports failures through
 * oci_error_t; nothing here prints or parses argv. The terminal in cli/
 * is the only presentation layer.
 */

#include "src/common/internal.h"
#include "src/store/core.h"
#include "src/store/seal.h"

#include <archive.h>
#include <archive_entry.h>
#include <ext2fs/ext2fs.h>
#include "src/common/document.h"

#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>

#define OCI_MANIFEST_MAX 1024u
#define OCI_LAYER_MAX 256u
#define OCI_MAX_ENTRIES 200000u
#define OCI_MAX_INODES 200000u
#define OCI_MAX_UNCOMPRESSED UINT64_C(8589934592)
#define OCI_MAX_IMAGE_SIZE UINT64_C(17179869184)
#define OCI_MAX_EXPANSION_RATIO 100u
#define OCI_SOURCE_ARCHIVE_MAX_MEMBERS 32768u
#define OCI_SOURCE_ARCHIVE_MAX_FILES 16384u
#define OCI_SOURCE_ARCHIVE_MAX_BYTES OCI_MAX_IMAGE_SIZE
#define OCI_SOURCE_ARCHIVE_TIMEOUT_MS UINT64_C(1800000)
#define OCI_BLOCK_SIZE 4096u
#define OCI_MIN_IMAGE_SIZE UINT64_C(67108864)
#define OCI_LEASE_MAX_BYTES MAELYS_OCI_LEASE_MAX_BYTES
/* Plain literal: also the catalog default of --grace-seconds. */
#define OCI_GC_DEFAULT_GRACE_SECONDS MAELYS_OCI_GC_DEFAULT_GRACE_SECONDS

/* Document schema identifiers published by the store operations. */
#define OCI_SCHEMA_INSPECTION "maelys.warden.oci-inspection/v1"
#define OCI_SCHEMA_STORE "maelys.warden.oci-store/v1"
#define OCI_SCHEMA_VERIFICATION "maelys.warden.oci-store-verification/v1"
#define OCI_SCHEMA_ARTIFACT "maelys.oci-artifact/v7"
/* Identities this materializer produced before and refuses to reuse. There
 * is no migration: a store still holding one is named as such by `list`
 * and `verify`, every command refuses it, and the store is recreated. */
#define OCI_SCHEMA_ARTIFACT_FORMER_V5 "maelys.warden.oci-artifact/v5"
#define OCI_SCHEMA_ARTIFACT_FORMER_V6 "maelys.oci-artifact/v6"
#define OCI_SCHEMA_CLOSURE "maelys.warden.oci-closure/v1"
#define OCI_SCHEMA_MIGRATION "maelys.warden.oci-migration/v1"
#define OCI_SCHEMA_LEASE MAELYS_OCI_LEASE_SCHEMA

#ifndef MAELYS_EXT2FS_VERSION
#define MAELYS_EXT2FS_VERSION "unknown"
#endif

/* ---- source access ------------------------------------------------------------ */

typedef enum source_kind {
    SOURCE_DIRECTORY = 1,
    SOURCE_ARCHIVE = 2
} source_kind_t;

struct source_archive_cache;

typedef struct oci_source {
    source_kind_t kind;
    char *path;
    int directory_fd;
    struct source_archive_cache *archive_cache;
} oci_source_t;

int source_open(const char *path, oci_source_t *out, oci_error_t *error);
int oci_archive_support_filters(struct archive *archive);
void source_close(oci_source_t *source);
int source_read(
    const oci_source_t *source, const char *relative, size_t maximum,
    unsigned char **out_bytes, size_t *out_size);
int source_read_descriptor(
    const oci_source_t *source, const oci_descriptor_t *descriptor,
    size_t maximum, unsigned char **out_bytes, size_t *out_size);
/* Why a descriptor copy failed. The distinction is what the caller reports,
 * so it is returned instead of being inferred from errno: an open refused by
 * O_NOFOLLOW and a host out of descriptors share too many errno values for
 * that inference to stay honest. */
typedef enum source_copy_status {
    SOURCE_COPY_OK = 0,
    SOURCE_COPY_CONTENT = 1,   /* the source's bytes: absent, not a regular
                                * file, not the declared size or digest */
    SOURCE_COPY_HOST = 2       /* this host's own open, read, write or fsync */
} source_copy_status_t;

int source_copy_descriptor(
    const oci_source_t *source, const oci_descriptor_t *descriptor,
    const char *destination, source_copy_status_t *out_status);
oci_document_t *load_json(const unsigned char *bytes, size_t size);

/* ---- logical Linux graph -------------------------------------------------------- */

typedef enum graph_entry_type {
    GRAPH_DIRECTORY = 1,
    GRAPH_REGULAR = 2,
    GRAPH_SYMLINK = 3
} graph_entry_type_t;

typedef struct graph_entry {
    char *path;
    graph_entry_type_t type;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    int64_t mtime;
    char *content_path;
    char *symlink_target;
    uint64_t logical_inode;
    uint64_t size;
} graph_entry_t;

typedef struct graph_index_slot {
    uint64_t hash;
    size_t entry_index;
    unsigned char state;
} graph_index_slot_t;

typedef struct logical_graph {
    graph_entry_t *entries;
    size_t count;
    size_t active_count;
    size_t capacity;
    graph_index_slot_t *index;
    size_t index_capacity;
    size_t index_count;
    size_t index_tombstones;
    uint64_t next_inode;
    uint64_t content_bytes;
    uint64_t cleared_privilege_bits;
} logical_graph_t;

void graph_entry_clear(graph_entry_t *entry);
void graph_clear(logical_graph_t *graph);
int graph_find(const logical_graph_t *graph, const char *path);
int graph_compact(logical_graph_t *graph);
void graph_remove_tree(logical_graph_t *graph, const char *path);
void graph_remove_children(logical_graph_t *graph, const char *path);
int normalize_layer_path(const char *raw, char **out_path);
int graph_put(logical_graph_t *graph, graph_entry_t *entry);
int graph_allocate_inode(logical_graph_t *graph, uint64_t *out_inode);
int graph_ensure_parent(logical_graph_t *graph, const char *path);
/* 1 when any segment of the first size bytes of path is a whiteout marker
 * name (a ".wh." prefix). */
int path_has_whiteout_segment(const char *path, size_t size);
int read_exact_at(int fd, void *buffer, size_t size, uint64_t offset);
int write_fd_content(
    int input, uint64_t input_offset, uint64_t content_size,
    const char *content_directory, char **out_path);
int parse_octal(
    const unsigned char *field, size_t field_size, uint64_t *out);
int tar_checksum_valid(const unsigned char header[512]);
const char *path_basename(const char *path);
char *path_parent_copy(const char *path);
char *tar_field_string(const unsigned char *field, size_t size);
char *tar_header_path(const unsigned char header[512]);
int parse_decimal_u64(const char *value, size_t size, uint64_t *out);
int graph_apply_layer(
    logical_graph_t *graph, const char *layer_path,
    const char *content_directory, uint64_t compressed_size,
    const char *media_type, const char *diff_id, oci_error_t *error);
int compare_graph_entries(const void *left, const void *right);
/* Writers consume a graph compacted after the final layer application. */
int graph_write_tar(
    const logical_graph_t *graph, const char *destination,
    uint64_t *out_archive_size, oci_error_t *error);
ext2_ino_t ext_parent_inode(
    const logical_graph_t *graph, const ext2_ino_t *inode_map,
    const char *path);
int graph_write_ext4(
    const logical_graph_t *graph, const char *destination,
    uint64_t *out_image_size, oci_error_t *error);

/* ---- store helpers ----------------------------------------------------------------- */

typedef struct digest_set {
    char (*items)[OCI_DIGEST_HEX_SIZE];
    size_t count;
    size_t capacity;
} digest_set_t;

typedef struct store_report {
    oci_document_t *errors;
    oci_document_t *warnings;
    uint64_t blobs;
    uint64_t blob_bytes;
    uint64_t closures;
    uint64_t artifacts;
    uint64_t leases;
} store_report_t;

/* A validated lease document. Liveness is not in the document: the kernel
 * answers it through the exclusive lock its holder keeps on the file. */
typedef struct lease_view {
    const char *manifest;
    const char *platform;
    const char *root_digest;
    const char *rootfs_tar_digest;
    uint64_t created;
    uint64_t expires;
    int acquisition;
    oci_document_t *blobs;
} lease_view_t;

int remove_filesystem_tree(const char *path);
/* Empties a directory, leaving the directory itself for rmdir_same. */
int remove_filesystem_tree_contents(const char *path);
int ensure_private_child(
    const char *parent, const char *name, char output[PATH_MAX]);
int fsync_directory(const char *path);
int store_path_open(
    const char *store_argument, char **out_canonical, int allow_missing);
int private_directory(const char *path);
int digest_from_directory_name(const char *name, char output[OCI_DIGEST_SIZE]);
oci_document_t *load_artifact_metadata(
    const char *path, const char *expected_manifest,
    const char *expected_platform);
/* 1 when schema names an artifact identity this materializer no longer
 * produces (v5 or v6), 0 otherwise. */
int artifact_schema_is_former(const char *schema);
/* The former schema named by the artifact.json at path when it is a
 * private read-only file carrying one, for the diagnostic that refuses the
 * store; NULL otherwise. Nothing else in the document is interpreted. The
 * string is borrowed from the document, which the caller releases. */
oci_document_t *load_former_artifact_schema(
    const char *path, const char **out_schema);
oci_document_t *load_canonical_json_file(
    const char *path, size_t maximum, int require_read_only);
int safe_regular_digest(
    const char *path, mode_t expected_permissions,
    char output[OCI_DIGEST_SIZE], uint64_t *out_size);
int write_json_file(const char *path, oci_document_t *document);
int files_equal(const char *left, const char *right);
int exact_file(const char *path, const char *bytes, size_t size);
int migration_document_valid(oci_document_t *migration);
int store_prepare_v2(const char *store);
int store_prepare_v2_until(const char *store, uint64_t deadline);

void digest_set_clear(digest_set_t *set);
int digest_set_add(digest_set_t *set, const char *digest);
int scan_acquisition_roots(const char *store, int collecting, uint64_t grace_seconds,
    digest_set_t *reachable);
int report_message(oci_document_t *array, const char *format, ...) OCI_PRINTF(2, 3);
int scan_source_closures(
    const char *store, digest_set_t *reachable, store_report_t *report);
int verify_source_closure(const char *store, oci_document_t *closure,
    digest_set_t *reachable, store_report_t *report);
int scan_blobs(
    const char *store, const digest_set_t *reachable, int report_unreachable,
    store_report_t *report, oci_document_t *candidates);
int scan_artifacts(
    const char *store, oci_document_t *migration, store_report_t *report);
int scan_leases(const char *store, store_report_t *report);
int lease_name_valid(const char *name);
int lease_document_valid(oci_document_t *lease, lease_view_t *out);
int collect_stale_leases(
    const char *store, uint64_t grace_seconds, store_report_t *report,
    oci_document_t *candidates);
int scan_temporary_state(
    const char *store, int collect, uint64_t grace_seconds, int warn,
    store_report_t *report, oci_document_t *candidates);
int report_initialize(store_report_t *report);
void report_clear(store_report_t *report);
oci_document_t *store_report_json(const char *store, const store_report_t *report);
int inspect_store_locked(
    const char *store, int collect_candidates, uint64_t grace_seconds, store_report_t *report,
    digest_set_t *reachable, oci_document_t *candidates);
int store_target_has_lease(
    const char *store, const char *manifest, const char *platform,
    int *out_has_lease);

/* ---- operations consumed by the terminal ------------------------------------------- */

/* Reads oci-layout and index.json and resolves every runnable manifest.
 * Buildx attestation descriptors are skipped; anything else invalid fails. */
int oci_inspect(
    const oci_source_t *source, oci_manifest_t **out_items,
    size_t *out_count, oci_error_t *error);
int oci_inspect_manifest(const oci_source_t *source,
    const oci_descriptor_t *descriptor, oci_manifest_t *out, oci_error_t *error);
oci_document_t *oci_inspection_document(
    const char *source_path, const oci_manifest_t *items, size_t count);

typedef struct oci_import_request {
    const char *store;              /* absolute private store root */
    const char *platform;           /* optional "OS/ARCH" selector */
    const char *digest;             /* optional "sha256:HEX" selector */
    int apply;                      /* 0 plans, 1 materializes */
} oci_import_request_t;

/* Plans or applies the import of one selected manifest. The document
 * reports mode, changed, reference, manifestDigest, configDigest,
 * platform, store and artifact. */
int oci_import(
    const oci_source_t *source, oci_manifest_t *items, size_t count,
    const oci_import_request_t *request, oci_document_t **out_document,
    oci_error_t *error);
/* Internal transaction entry: both ordered locks must already be held by
 * the caller. Shared by the standalone import command and registry pull. */
int oci_import_locked(const oci_source_t *source, oci_manifest_t *selected,
    const char *store, const maelys_oci_store_lock_t *store_lock,
    const maelys_oci_store_lock_t *manifest_lock, uint64_t deadline, oci_document_t **out_document,
    oci_error_t *error);

/* Lists published artifacts: {schema, store, artifacts: [...]}. A missing
 * store yields an empty list. */
int oci_store_list(
    const char *store_argument, oci_document_t **out_document, oci_error_t *error);

/* Verifies the whole store under the shared lock. out_valid is 1 when the
 * report carries no error; the document is always produced on success. */
int oci_store_verify(
    const char *store_argument, oci_document_t **out_document, int *out_valid,
    oci_error_t *error);

/* Plans (apply == 0) or performs garbage collection under the exclusive
 * lock. out_valid mirrors the integrity of the store. */
int oci_store_gc(
    const char *store_argument, int apply, uint64_t grace_seconds,
    oci_document_t **out_document, int *out_valid, oci_error_t *error);

/* Plans or removes one artifact and its source closure. */
int oci_store_remove(
    const char *store_argument, const char *reference,
    const char *platform_argument, int apply, oci_document_t **out_document,
    oci_error_t *error);

/* Extracts a Warden-generated portable root archive on Linux only. */
int oci_unpack_portable_root(
    const char *archive_path, const char *destination, oci_error_t *error);

#endif
