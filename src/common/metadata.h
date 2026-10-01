/* SPDX-License-Identifier: MPL-2.0 */
#ifndef MAELYS_OCI_COMMON_METADATA_H
#define MAELYS_OCI_COMMON_METADATA_H
#include "src/common/internal.h"
#include "src/common/document.h"

oci_document_t *oci_manifest_summary(const oci_manifest_t *manifest);
/* Format already verified manifest/config bytes. Layer digests and DiffIDs
 * remain declarations; no I/O or runtime defaults belong in this formatter. */
int oci_manifest_metadata(const oci_manifest_t *manifest,
    const unsigned char *image_bytes, size_t image_size,
    const unsigned char *config_bytes, size_t config_size,
    oci_document_t *target, oci_error_t *error);
int oci_metadata_report_valid(const oci_document_t *root, oci_error_t *error);
#endif
