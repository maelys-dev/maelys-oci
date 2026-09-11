/* SPDX-License-Identifier: MPL-2.0 */
#ifndef MAELYS_OCI_COMMON_INTERNAL_H
#define MAELYS_OCI_COMMON_INTERNAL_H

/*
 * Private vocabulary shared by every layer of libmaelys-oci: diagnostics,
 * bounded formatting, durable I/O primitives and the OCI descriptor model.
 * Nothing here is installed; the public contract is include/maelys/oci.h.
 */

#include <maelys/oci.h>
#include <maelys/sys.h>

#include <maelys/json.h>

#include <limits.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifndef MAELYS_OCI_BUILD_VERSION
#define MAELYS_OCI_BUILD_VERSION "development"
#endif

#if defined(__GNUC__) || defined(__clang__)
#define OCI_PRINTF(format_index, first_argument) \
    __attribute__((format(printf, format_index, first_argument)))
#else
#define OCI_PRINTF(format_index, first_argument)
#endif

/* Store location honoured by the terminal and public artifact API. */

/* Fixed-size text fields of the OCI model. */
#define OCI_DIGEST_PREFIX "sha256:"
#define OCI_DIGEST_PREFIX_SIZE 7u
#define OCI_DIGEST_SIZE 72u        /* "sha256:" + 64 hex digits + NUL */
#define OCI_DIGEST_HEX_SIZE 65u    /* 64 hex digits + NUL */
#define OCI_PLATFORM_SIZE 72u      /* "OS/ARCH" or "OS-ARCH" + NUL */
#define OCI_PLATFORM_PART_SIZE 32u
#define OCI_MEDIA_TYPE_SIZE 160u
#define OCI_JSON_MAX (8u * 1024u * 1024u)
#define OCI_JSON_TOKENS_MAX 32768u
#define OCI_DESCRIPTOR_MAX UINT64_C(17179869184)
#define OCI_STAGING_MAX UINT64_C(17179869184)

/* ---- diagnostics ----------------------------------------------------------- */

/* Failure classes reported by internal operations. The terminal maps them to
 * the stable agent-cli error codes; the library never prints. */
typedef enum oci_error_kind {
    OCI_ERROR_NONE = 0,
    OCI_ERROR_ARGUMENT,     /* an input value is malformed */
    OCI_ERROR_NOT_FOUND,    /* a required path or resource is absent */
    OCI_ERROR_ACCESS,       /* a file is untrusted or has unsafe modes */
    OCI_ERROR_PROTOCOL,     /* OCI content or a registry answer is invalid */
    OCI_ERROR_STATE,        /* the store does not allow the operation */
    OCI_ERROR_IO,           /* a system read or write failed */
    OCI_ERROR_UNSUPPORTED,  /* the host or build cannot perform the action */
    OCI_ERROR_MEMORY
} oci_error_kind_t;

typedef struct oci_error {
    oci_error_kind_t kind;
    char *message; /* owned; NULL while no failure was reported */
} oci_error_t;

#define OCI_ERROR_INIT {OCI_ERROR_NONE, NULL}

/* Records a diagnostic. The first report fixes the kind and starts the
 * message; later reports append their text so the root cause stays first and
 * the enclosing stages follow. A NULL error discards the report. */
void oci_error_report(
    oci_error_t *error, oci_error_kind_t kind, const char *format, ...)
    OCI_PRINTF(3, 4);
void oci_error_clear(oci_error_t *error);
const char *oci_error_message(const oci_error_t *error);

/* Public-API flavour used by include/maelys/oci.h functions: first message
 * wins, the caller releases it with maelys_oci_error_free(). */
void maelys_oci_set_error(char **out_error, const char *format, ...)
    OCI_PRINTF(2, 3);

/* ---- bounded formatting and I/O -------------------------------------------- */

/* snprintf that reports truncation as a failure: returns the length written
 * or -1 when the result does not fit. Every path is built through it. */
int oci_snprintf(char *buffer, size_t capacity, const char *format, ...)
    OCI_PRINTF(3, 4);

int oci_hex_valid(const char *text, size_t digit_count);
int oci_directory_entry_is_dot(const char *name);

int oci_write_all(int descriptor, const void *bytes, size_t size);
/* Reads the whole regular file behind an open descriptor when its size is
 * within maximum. The buffer is owned by the caller. */
int oci_read_regular_bounded(
    int descriptor, size_t maximum, unsigned char **out_bytes,
    size_t *out_size);
/* Creates path exclusively with mode 0600, writes, fsyncs, seals it with
 * final_mode and closes it; the file is unlinked on any failure. */
int oci_write_file_exclusive(
    const char *path, const void *bytes, size_t size, mode_t final_mode);
/* Hashes and copies through EOF, refusing overflow before writing a block. */
int oci_copy_fd_hashed(int input, int output, uint64_t maximum,
    uint64_t *out_size, char out_digest[OCI_DIGEST_HEX_SIZE]);

/* ---- SHA-256 ---------------------------------------------------------------- */

typedef struct maelys_oci_sha256_context {
    uint32_t state[8];
    uint64_t bit_count;
    unsigned char block[64];
    size_t block_length;
} maelys_oci_sha256_context_t;

void maelys_oci_sha256_init(maelys_oci_sha256_context_t *context);
void maelys_oci_sha256_update(
    maelys_oci_sha256_context_t *context,
    const unsigned char *bytes,
    size_t length);
void maelys_oci_sha256_finish(
    maelys_oci_sha256_context_t *context,
    char out_hex[MAELYS_OCI_DIGEST_HEX_SIZE]);
void maelys_oci_sha256_hex(
    const void *data,
    size_t length,
    char out_hex[MAELYS_OCI_DIGEST_HEX_SIZE]);
maelys_oci_result_t maelys_oci_sha256_file(
    const char *path,
    char out_hex[MAELYS_OCI_DIGEST_HEX_SIZE],
    char **out_error);
void oci_bytes_to_hex(const unsigned char *bytes, size_t size, char *out_hex);

/* ---- OCI descriptor model ----------------------------------------------------- */

typedef struct oci_descriptor {
    char digest[OCI_DIGEST_SIZE];
    char media_type[OCI_MEDIA_TYPE_SIZE];
    uint64_t size;
    char os[OCI_PLATFORM_PART_SIZE];
    char architecture[OCI_PLATFORM_PART_SIZE];
    char variant[OCI_PLATFORM_PART_SIZE];
    char reference_type[64];
    char reference_digest[OCI_DIGEST_SIZE];
} oci_descriptor_t;

/* One runnable image manifest: its own descriptor, its config, its layers
 * and the platform recorded by the config document. */
typedef struct oci_manifest {
    oci_descriptor_t manifest;
    oci_descriptor_t config;
    oci_descriptor_t *layers;
    size_t layer_count;
    char (*diff_ids)[OCI_DIGEST_SIZE];
    char os[OCI_PLATFORM_PART_SIZE];
    char architecture[OCI_PLATFORM_PART_SIZE];
} oci_manifest_t;

/* Config parsing distinguishes malformed content from a well-formed platform
 * that this build cannot materialize. Registry pulls and closure verification
 * reject both; layout inspection may omit unsupported siblings. */
typedef enum oci_config_result {
    OCI_CONFIG_INVALID = -1,
    OCI_CONFIG_OK = 0,
    OCI_CONFIG_UNSUPPORTED = 1
} oci_config_result_t;

void oci_manifest_clear(oci_manifest_t *manifest);
/* Validates the config platform and ordered rootfs DiffIDs for this manifest.
 * On success the manifest owns diff_ids; no layer may be applied without it. */
int oci_config_parse(const unsigned char *bytes, size_t size,
    oci_manifest_t *manifest, oci_error_t *error);
/* Includes repeated layer occurrences, since each is staged for application. */
int oci_manifest_staging_valid(const oci_manifest_t *manifest);
/* Formats "OS/ARCH" from the config platform of a manifest. */
int oci_manifest_platform(const oci_manifest_t *manifest, char out[OCI_PLATFORM_SIZE]);

int oci_digest_valid(const char *digest);
int oci_digest_hex_valid(const char *hex);
/* Extracts the digest of "NAME@sha256:HEX" or a bare "sha256:HEX". */
int oci_reference_digest(const char *reference, char out[OCI_DIGEST_SIZE]);
int oci_relative_path_valid(const char *path);

/* Platform syntax "OS/ARCH" with lowercase components; the directory form
 * replaces the slash with a dash. */
int oci_platform_to_directory(const char *platform, char out[OCI_PLATFORM_SIZE]);
int oci_platform_variant_supported(const char *architecture, const char *variant);
int oci_platform_parts_supported(const char *os, const char *architecture);
int oci_directory_entry_name_valid(const char *name);

int oci_manifest_media_type_supported(const char *media_type);
int oci_index_media_type_supported(const char *media_type);
int oci_config_media_type_supported(const char *media_type);
int oci_layer_media_type_supported(const char *media_type);
/* Compares a Content-Type header value, ignoring parameters, to a type. */
int oci_media_type_equal(const char *header_value, const char *expected);

/* Bounded RFC 8259 parsing of one JSON object; NULL when the document is
 * not an object or exceeds the limits. */
maelys_json_document_t *oci_json_parse_object(
    const unsigned char *bytes, size_t size, size_t maximum_tokens);
/* Copies a string member; an absent optional member yields "". */
int oci_json_copy_string(
    const maelys_json_document_t *document, maelys_json_value_t object,
    const char *name, char *output, size_t capacity, int required);
/* Returns 1 with the view of a present string member, 0 when absent and -1
 * when present but not a string. */
int oci_json_optional_string(
    const maelys_json_document_t *document, maelys_json_value_t object,
    const char *name, maelys_json_view_t *out_view);
int oci_json_u64_is(
    const maelys_json_document_t *document, maelys_json_value_t object,
    const char *name, uint64_t expected);
/* Parses one content descriptor with its optional platform and the Docker
 * reference annotations used by buildx attestations. */
int oci_descriptor_parse(
    const maelys_json_document_t *document, maelys_json_value_t value,
    int platform_required, oci_descriptor_t *out);
int oci_descriptor_is_buildx_attestation(const oci_descriptor_t *descriptor);

#endif
