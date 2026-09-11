/* SPDX-License-Identifier: MPL-2.0 */
#ifndef MAELYS_OCI_STORE_CORE_H
#define MAELYS_OCI_STORE_CORE_H

/*
 * Immutable content-addressed store mechanics shared by the library and the
 * materializer: private directories, ordered locks, no-replace publication
 * and hashing of sealed members.
 */

#include "src/common/internal.h"

#include <maelys/sys/file.h>

#include <stddef.h>
#include <sys/stat.h>
#include <sys/types.h>

/* An identity-checked lock held by maelys-system; NULL when not held. */
typedef struct maelys_oci_store_lock {
    maelys_sys_file_lock_t *handle;
} maelys_oci_store_lock_t;

#define MAELYS_OCI_STORE_LOCK_INIT {NULL}

/* Resolves the private store: MAELYS_OCI_STORE, else XDG_DATA_HOME
 * or HOME. Returns -1 when no absolute location is configured. */
int maelys_oci_store_default_path(char *output, size_t capacity);
int maelys_oci_store_open_root(
    const char *argument, int create, char **out_canonical);
int maelys_oci_store_ensure_private_directory(
    const char *parent, const char *name, char *output, size_t capacity);
int maelys_oci_store_private_directory(const char *path);
int maelys_oci_store_fsync_directory(const char *path);

/* A lease document never exceeds this size. */
#define MAELYS_OCI_LEASE_MAX_BYTES 65535u

/* What every lease file must be: a regular file of the caller, mode exactly
 * 0400, singly linked, within MAELYS_OCI_LEASE_MAX_BYTES. */
void maelys_oci_store_lease_expectations(
    maelys_sys_file_expectations_t *out_expectations);

/* Asks the kernel whether a lease has a live holder: tries its exclusive
 * lock without waiting or creating. Returns 1 when a holder exists
 * (ERR_BUSY); 0 when nobody holds it, with *out_lock then held by the
 * caller until maelys_sys_file_lock_release; -1 when the file cannot be
 * locked at all (absent, not what a lease must be, or refused). */
int maelys_oci_store_lease_probe(
    const char *path, maelys_sys_file_lock_t **out_lock);

int maelys_oci_store_lock_acquire(
    const char *store, const char *name, int exclusive,
    maelys_oci_store_lock_t *out_lock);
int maelys_oci_store_lock_until(
    const char *store, const char *name, int exclusive, uint64_t deadline_ms,
    maelys_oci_store_lock_t *out_lock);
int oci_store_validate_ancestors(const char *path);
int oci_store_random_id(char out[33]);
int oci_store_temporary_file(const char *store, char path[PATH_MAX]);
int oci_store_blob_path(const char *store, const char *digest, char path[PATH_MAX]);
/* 1 is a verified hit, 0 absent, -1 unsafe or corrupt. */
int oci_store_blob_check(const char *store, const oci_descriptor_t *descriptor);
int oci_store_blob_read(const char *store, const char *digest, size_t maximum,
    unsigned char **out_bytes, size_t *out_size);
/* Validates the staged bytes and publishes without replacement; an existing
 * destination must have the same digest, size, mode and exact bytes. */
int oci_store_blob_publish(const char *store,
    const oci_descriptor_t *descriptor, const char *staging);
int oci_store_files_equal(const char *left, const char *right);

#define OCI_ACQUISITION_LEASE_SCHEMA "maelys.oci-acquisition-lease/v1"
typedef struct oci_acquisition_lease {
    char *path;
    maelys_sys_file_lock_t *lock;
    maelys_sys_file_identity_t identity;
} oci_acquisition_lease_t;
int oci_acquisition_lease_create(const char *store, const oci_manifest_t *image,
    uint64_t duration_ms, oci_acquisition_lease_t *out);
int oci_acquisition_lease_retire(oci_acquisition_lease_t *lease);
void maelys_oci_store_lock_release(maelys_oci_store_lock_t *lock);

/* Acquires the shared store lock then the exclusive per-manifest lock, in
 * that fixed order. Both are released by maelys_oci_store_unlock_manifest. */
int maelys_oci_store_lock_manifest(
    const char *store, const char *manifest_hex,
    maelys_oci_store_lock_t *out_store_lock,
    maelys_oci_store_lock_t *out_manifest_lock);
void maelys_oci_store_unlock_manifest(
    maelys_oci_store_lock_t *store_lock,
    maelys_oci_store_lock_t *manifest_lock);

/* Return 0 when published, 1 when the destination already exists, -1 on
 * failure. Neither operation ever replaces an existing destination. */
int maelys_oci_store_publish_file_noreplace(
    const char *staging, const char *destination);
int maelys_oci_store_publish_directory_noreplace(
    const char *staging, const char *destination);

/* Hashes a sealed member: a regular, singly-linked file owned by the caller
 * with exactly expected_mode, whose identity is unchanged after hashing.
 * out_status receives the pre-hash metadata when non-NULL. */
int maelys_oci_store_hash_immutable(
    const char *path, mode_t expected_mode,
    char out_hex[MAELYS_OCI_DIGEST_HEX_SIZE], struct stat *out_status);

#endif
