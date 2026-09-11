/* SPDX-License-Identifier: MPL-2.0 */
#ifndef MAELYS_OCI_PULLER_INTERNAL_H
#define MAELYS_OCI_PULLER_INTERNAL_H

/*
 * Bounded registry acquisition over maelys-http: reference parsing, HTTPS
 * session with Basic or Bearer credentials, digest-verified downloads into the
 * local CAS, then the ordinary locked import path of the materializer.
 */

#include "src/common/internal.h"
#include "src/store/core.h"

#include <maelys/http_client.h>
#include <maelys/http_tls_modules.h>
#include <maelys/http_transports.h>

#include "src/common/document.h"

#include <stdint.h>
#include <sys/types.h>

#define PULL_TOKEN_JSON_MAX (64u * 1024u)
#define PULL_CONFIG_MAX (8u * 1024u * 1024u)
#define PULL_CLOSURE_MAX UINT64_C(68719476736)
#define PULL_LAYER_MAX 256u
#define PULL_MANIFEST_MAX 1024u
#define PULL_INDEX_DEPTH_MAX 8u
#define PULL_REDIRECT_MAX 5u
#define PULL_REQUEST_MAX 4096u
#define PULL_HEADERS_MAX (32u * 1024u)
#define PULL_HEADER_COUNT_MAX 64u
#define PULL_PHASE_TIMEOUT_MS UINT64_C(30000)
/* Plain literal: also the catalog default of --timeout-ms. */
#define PULL_TIMEOUT_MS MAELYS_OCI_PULL_TIMEOUT_MS
#define PULL_CONNECTION_REUSE_MAX 64u
#define PULL_CONNECTION_IDLE_TTL_MS UINT64_C(30000)
#define PULL_SECRET_MAX 16384u
#define PULL_TARGET_MAX 1400u
#define PULL_MESSAGE_MAX 4096u

typedef struct pull_reference {
    char *authority;
    char *repository;
    char digest[OCI_DIGEST_SIZE];
} pull_reference_t;

typedef struct pull_challenge {
    char *realm;
    char *service;
    char *scope;
} pull_challenge_t;

void secret_wipe(void *bytes, size_t size);
int basic_credential_valid(const char *value);
int challenge_parse(const char *text, pull_challenge_t *out);
void challenge_clear(pull_challenge_t *challenge);

typedef struct pull_headers {
    unsigned status;
    char *content_type;
    char *www_authenticate;
    char *content_digest;
    int invalid;
    struct pull_body *body;
} pull_headers_t;

typedef struct pull_body {
    unsigned char *bytes;
    size_t size;
    size_t capacity;
    size_t maximum;
    int fd;
    uint64_t expected_size;
    uint64_t received;
    maelys_oci_sha256_context_t hash;
    int hash_active;
    int discard;
    struct pull_http *http;
    uint64_t last_progress;
    int headers_received;
} pull_body_t;

typedef struct pull_http {
    maelys_http_tls_provider_t *tls;
    maelys_http_transport_t *transport;
    maelys_http_client_t *client;
    uint64_t timeout_ms;
    uint64_t deadline;
    uint64_t network_bytes;
    size_t requests;
    int cross_authority;
    const char *store;
    uint64_t downloaded_blobs;
    uint64_t cached_blobs;
    char *basic_authorization;
    char *bearer_authorization;
    int helper_credentials_present;
    oci_error_t *error; /* borrowed sink for diagnostics */
} pull_http_t;

typedef struct pull_options {
    const char *reference;          /* REGISTRY/REPOSITORY@sha256:HEX */
    const char *platform;           /* optional linux/arm64 or linux/amd64 */
    const char *store;              /* absolute private store root */
    const char *ca_file;            /* optional absolute CA bundle */
    const char *token_file;         /* optional private bearer token file */
    const char *docker_config;      /* optional private Docker config */
    uint64_t timeout_ms;
} pull_options_t;

typedef struct pull_resolved {
    oci_manifest_t image;
    unsigned char *manifest_bytes;
    size_t manifest_size;
    unsigned char *config_bytes;
    size_t config_size;
} pull_resolved_t;

void resolved_clear(pull_resolved_t *resolved);
int pull_resolve(pull_http_t *http, const pull_reference_t *reference,
    const char *platform, pull_resolved_t *out);

/* Records a diagnostic on the session's error sink. */
void pull_report(
    pull_http_t *http, oci_error_kind_t kind, const char *format, ...)
    OCI_PRINTF(3, 4);

int authority_valid(const char *authority);
void reference_clear(pull_reference_t *reference);
int reference_parse(const char *text, pull_reference_t *out);
void headers_clear(pull_headers_t *headers);
int body_reset(pull_body_t *body);
void body_clear(pull_body_t *body);
maelys_http_headers_step_t capture_headers(
    void *opaque, const maelys_http_exchange_t *exchange);
maelys_http_sink_step_t capture_body(
    void *opaque, const unsigned char *bytes, size_t length);
maelys_http_redirect_decision_t safe_redirect(
    void *opaque,
    unsigned status,
    maelys_http_slice_t old_authority,
    maelys_http_slice_t scheme,
    maelys_http_slice_t authority,
    maelys_http_slice_t target,
    size_t index);
int manifest_header_media_type(const char *value);
int index_header_media_type(const char *value);
unsigned char *read_private_file(
    const char *path, size_t maximum, size_t *out_size);
char *read_secret_file(const char *path, size_t maximum);
char *authorization_value(const char *scheme, const char *credential);
void secret_free(char **value);
int load_docker_authorization(
    pull_http_t *http,
    const char *configured_path,
    const char *authority);
const char *discover_ca_file(const char *explicit_path);
int http_initialize(
    pull_http_t *http,
    const pull_options_t *options,
    const pull_reference_t *reference,
    oci_error_t *error);
void http_clear(pull_http_t *http);
int http_get_once(
    pull_http_t *http, const char *authority, const char *target,
    const char *accept, const char *authorization,
    pull_headers_t *headers, pull_body_t *body);
int acquire_bearer_token(
    pull_http_t *http, const char *challenge, const pull_reference_t *reference);
int registry_get(
    pull_http_t *http,
    const pull_reference_t *reference,
    const char *target,
    const char *accept,
    pull_headers_t *headers,
    pull_body_t *body);
int fetch_memory(
    pull_http_t *http,
    const pull_reference_t *reference,
    const char *target,
    const char *accept,
    const char *expected_digest,
    size_t maximum,
    pull_headers_t *out_headers,
    unsigned char **out_bytes,
    size_t *out_size);
int index_parse(const unsigned char *bytes, size_t size,
    oci_descriptor_t **out_items, size_t *out_count);
int document_media_type_matches(
    const unsigned char *bytes, size_t size, const char *expected);
int manifest_parse(
    const unsigned char *bytes,
    size_t size,
    oci_manifest_t *out);

int store_memory_blob(pull_http_t *http,
    const oci_descriptor_t *descriptor,
    const unsigned char *bytes,
    size_t size);
int fetch_file_blob(
    pull_http_t *http,
    const pull_reference_t *reference,
    const oci_descriptor_t *descriptor);

int fetch_manifest_document(
    pull_http_t *http,
    const pull_reference_t *reference,
    const char *digest,
    unsigned char **out_bytes,
    size_t *out_size,
    pull_headers_t *out_headers);

/* Acquires the referenced image into the store and materializes it. The
 * document reports reference, registry, repository, manifestDigest,
 * configDigest, platform, store, artifact and changed. */
int oci_pull(
    const pull_options_t *options, oci_document_t **out_document, oci_error_t *error);

#endif
