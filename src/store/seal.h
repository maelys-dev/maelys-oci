/* SPDX-License-Identifier: MPL-2.0 */
#ifndef MAELYS_OCI_STORE_SEAL_H
#define MAELYS_OCI_STORE_SEAL_H

/*
 * The artifact seal is the nine-line text file that binds a materialized
 * artifact to its manifest, config, platform, root
 * digests. This is the only reader and the only writer of that format.
 */

#include "src/common/internal.h"

#include <stddef.h>
#include <stdint.h>

#define MAELYS_OCI_SEAL_MAX_BYTES 1024u

typedef struct maelys_oci_seal {
    char manifest[OCI_DIGEST_SIZE];
    char config[OCI_DIGEST_SIZE];
    char platform[OCI_PLATFORM_SIZE];
    char root[OCI_DIGEST_SIZE];
    uint64_t root_size;
    char rootfs_tar[OCI_DIGEST_SIZE];
    uint64_t rootfs_tar_size;
} maelys_oci_seal_t;

/* Renders the seal text; returns the length or -1 when it does not fit. */
int maelys_oci_seal_format(
    const maelys_oci_seal_t *seal, char *buffer, size_t capacity);

/* Reads and strictly parses a seal file opened without following links.
 * Returns 0 on success and -1 for any deviation from the format. */
int maelys_oci_seal_parse(const char *path, maelys_oci_seal_t *out);

#endif
