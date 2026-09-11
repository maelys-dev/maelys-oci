/* SPDX-License-Identifier: MPL-2.0 */
#include "src/puller/internal.h"

#include <maelys/sys/clock.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void pull_report(
    pull_http_t *http, oci_error_kind_t kind, const char *format, ...) {
    if (!http || !http->error) return;
    va_list arguments;
    va_start(arguments, format);
    char message[PULL_MESSAGE_MAX];
    (void)vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    oci_error_report(http->error, kind, "%s", message);
}

void headers_clear(pull_headers_t *headers) {
    if (!headers) return;
    free(headers->content_type);
    free(headers->www_authenticate);
    free(headers->content_digest);
    struct pull_body *body = headers->body;
    memset(headers, 0, sizeof(*headers));
    headers->body = body;
}

static int slice_equal(maelys_http_slice_t slice, const char *text) {
    size_t length = text ? strlen(text) : 0u;
    if (!slice.data || slice.length != length) return 0;
    for (size_t index = 0u; index < length; ++index) {
        unsigned char left = (unsigned char)slice.data[index];
        unsigned char right = (unsigned char)text[index];
        if (left >= 'A' && left <= 'Z') left = (unsigned char)(left + 32u);
        if (right >= 'A' && right <= 'Z') right = (unsigned char)(right + 32u);
        if (left != right) return 0;
    }
    return 1;
}

static int copy_unique_header(
    char **destination, maelys_http_slice_t value) {
    if (*destination || !value.data || memchr(value.data, '\0', value.length))
        return -1;
    *destination = strndup(value.data, value.length);
    return *destination ? 0 : -1;
}

maelys_http_headers_step_t capture_headers(
    void *opaque, const maelys_http_exchange_t *exchange) {
    pull_headers_t *headers = opaque;
    headers->status = maelys_http_exchange_status(exchange);
    if (headers->body) {
        headers->body->discard = headers->status != 200u;
        headers->body->headers_received = 1;
        if (maelys_sys_monotonic_ms(&headers->body->last_progress) != MAELYS_SYS_OK)
            return MAELYS_HTTP_HEADERS_FAILED;
    }
    for (size_t index = 0u;
         index < maelys_http_exchange_header_count(exchange); ++index) {
        maelys_http_header_view_t header =
            maelys_http_exchange_header(exchange, index);
        int result = 0;
        if (slice_equal(header.name, "content-type")) {
            result = copy_unique_header(&headers->content_type, header.value);
        } else if (slice_equal(header.name, "www-authenticate")) {
            result = copy_unique_header(
                &headers->www_authenticate, header.value);
        } else if (slice_equal(header.name, "docker-content-digest")) {
            result = copy_unique_header(&headers->content_digest, header.value);
        } else if (slice_equal(header.name, "content-encoding") &&
                   !slice_equal(header.value, "identity")) {
            result = -1;
        }
        if (result != 0) {
            headers->invalid = 1;
            return MAELYS_HTTP_HEADERS_FAILED;
        }
    }
    return MAELYS_HTTP_HEADERS_ACCEPT;
}

int body_reset(pull_body_t *body) {
    body->size = 0u;
    body->received = 0u;
    body->discard = 0;
    if (body->fd >= 0) {
        if (lseek(body->fd, 0, SEEK_SET) < 0 ||
            ftruncate(body->fd, 0) != 0) return -1;
    }
    if (body->hash_active) maelys_oci_sha256_init(&body->hash);
    return 0;
}

void body_clear(pull_body_t *body) {
    if (!body) return;
    if (body->fd >= 0) (void)maelys_sys_fd_close(&body->fd);
    free(body->bytes);
    memset(body, 0, sizeof(*body));
    body->fd = -1;
}

maelys_http_sink_step_t capture_body(
    void *opaque, const unsigned char *bytes, size_t length) {
    pull_body_t *body = opaque;
    if (body->http) {
        if (length > PULL_CLOSURE_MAX ||
            body->http->network_bytes > PULL_CLOSURE_MAX - length ||
            maelys_sys_monotonic_ms(&body->last_progress) != MAELYS_SYS_OK)
            return MAELYS_HTTP_SINK_FAILED;
        body->http->network_bytes += length;
    }
    if (body->discard) {
        if (length > PULL_TOKEN_JSON_MAX ||
            body->received > PULL_TOKEN_JSON_MAX - length)
            return MAELYS_HTTP_SINK_FAILED;
        body->received += length;
        return MAELYS_HTTP_SINK_ACCEPT;
    }
    if (body->received > UINT64_MAX - length ||
        body->received + length > body->maximum) {
        return MAELYS_HTTP_SINK_FAILED;
    }
    if (body->expected_size &&
        body->received + length > body->expected_size) {
        return MAELYS_HTTP_SINK_FAILED;
    }
    if (body->fd >= 0) {
        size_t offset = 0u;
        while (offset < length) {
            ssize_t amount = write(body->fd, bytes + offset, length - offset);
            if (amount > 0) offset += (size_t)amount;
            else if (amount < 0 && errno == EINTR) continue;
            else return MAELYS_HTTP_SINK_FAILED;
        }
    } else {
        if (body->size > SIZE_MAX - length) return MAELYS_HTTP_SINK_FAILED;
        size_t needed = body->size + length;
        if (needed > body->capacity) {
            size_t capacity = body->capacity ? body->capacity : 4096u;
            while (capacity < needed) {
                if (capacity > SIZE_MAX / 2u) return MAELYS_HTTP_SINK_FAILED;
                capacity *= 2u;
            }
            unsigned char *grown = realloc(body->bytes, capacity);
            if (!grown) return MAELYS_HTTP_SINK_FAILED;
            body->bytes = grown;
            body->capacity = capacity;
        }
        if (length) memcpy(body->bytes + body->size, bytes, length);
        body->size += length;
    }
    if (body->hash_active) maelys_oci_sha256_update(&body->hash, bytes, length);
    body->received += length;
    return MAELYS_HTTP_SINK_ACCEPT;
}

maelys_http_redirect_decision_t safe_redirect(
    void *opaque, unsigned status, maelys_http_slice_t old_authority,
    maelys_http_slice_t scheme, maelys_http_slice_t authority,
    maelys_http_slice_t target, size_t index) {
    pull_http_t *http = opaque;
    (void)status;
    (void)target;
    (void)index;
    if (!http || !slice_equal(scheme, "https") ||
        http->requests >= PULL_REQUEST_MAX || authority.length > 253u)
        return MAELYS_HTTP_REDIRECT_DENY;
    char name[254];
    memcpy(name, authority.data, authority.length);
    name[authority.length] = '\0';
    if (!authority_valid(name)) return MAELYS_HTTP_REDIRECT_DENY;
    if (!slice_equal(old_authority, name)) http->cross_authority = 1;
    ++http->requests;
    return MAELYS_HTTP_REDIRECT_FOLLOW;
}

int manifest_header_media_type(const char *value) {
    return oci_media_type_equal(
               value, "application/vnd.oci.image.manifest.v1+json") ||
        oci_media_type_equal(
               value, "application/vnd.docker.distribution.manifest.v2+json");
}

int index_header_media_type(const char *value) {
    return oci_media_type_equal(value,
               "application/vnd.oci.image.index.v1+json") ||
        oci_media_type_equal(value,
               "application/vnd.docker.distribution.manifest.list.v2+json");
}

const char *discover_ca_file(const char *explicit_path) {
    static const char *const candidates[] = {
        "/etc/ssl/cert.pem",
        "/etc/ssl/certs/ca-certificates.crt",
        "/opt/homebrew/etc/ca-certificates/cert.pem",
        "/usr/local/etc/ca-certificates/cert.pem"
    };
    const char *environment = getenv("SSL_CERT_FILE");
    if (explicit_path) return explicit_path;
    if (environment && environment[0] == '/') return environment;
    for (size_t index = 0u;
         index < sizeof(candidates) / sizeof(candidates[0]); ++index) {
        if (access(candidates[index], R_OK) == 0) return candidates[index];
    }
    return NULL;
}

int http_initialize(
    pull_http_t *http, const pull_options_t *options,
    const pull_reference_t *reference, oci_error_t *error) {
    memset(http, 0, sizeof(*http));
    http->timeout_ms = options->timeout_ms;
    http->error = error;
    if (!http->timeout_ms || maelys_sys_deadline_after(http->timeout_ms,
            &http->deadline) != MAELYS_SYS_OK) {
        pull_report(http, OCI_ERROR_ARGUMENT, "pull timeout must be finite and positive");
        return -1;
    }
    if (options->token_file) {
        char *token = read_secret_file(options->token_file, PULL_SECRET_MAX);
        if (!token) {
            pull_report(http, OCI_ERROR_ACCESS,
                "--token-file must be a private regular file owned by the "
                "current user");
            return -1;
        }
        http->bearer_authorization = authorization_value("Bearer", token);
        secret_free(&token);
        if (!http->bearer_authorization) {
            pull_report(http, OCI_ERROR_MEMORY, "out of memory");
            return -1;
        }
    } else if (load_docker_authorization(
            http, options->docker_config, reference->authority) != 0) {
        pull_report(http, OCI_ERROR_ACCESS,
            "Docker config must be a private regular JSON file owned by the "
            "current user");
        return -1;
    }
    const char *ca_file = discover_ca_file(options->ca_file);
    if (!ca_file || ca_file[0] != '/') {
        pull_report(http, OCI_ERROR_NOT_FOUND,
            "no absolute CA trust-anchor bundle is available");
        return -1;
    }
    maelys_http_tls_client_files_t files = {.ca_file = ca_file};
    char *tls_error = NULL;
    maelys_http_result_t result = maelys_http_tls_mbedtls_client_create(
        &files, &http->tls, &tls_error);
    if (result == MAELYS_HTTP_OK) result = maelys_http_posix_transport_create(
        http->tls, &http->transport, &tls_error);
    maelys_http_client_limits_t limits;
    maelys_http_client_limits_default(&limits);
    limits.parser.max_body_bytes = OCI_DESCRIPTOR_MAX;
    limits.parser.max_start_line_bytes = 8192u;
    limits.parser.max_header_line_bytes = PULL_HEADERS_MAX;
    limits.parser.max_header_bytes = PULL_HEADERS_MAX;
    limits.parser.max_header_count = PULL_HEADER_COUNT_MAX;
    limits.parser.max_chunk_line_bytes = 1024u;
    limits.parser.max_trailer_bytes = 8192u;
    limits.parser.max_trailer_count = 16u;
    limits.max_informational_responses = 4u;
    limits.max_wait_slice_ms = 100u;
    limits.max_redirects = PULL_REDIRECT_MAX;
    limits.max_connection_reuses = PULL_CONNECTION_REUSE_MAX;
    limits.idle_connection_ttl_ms = PULL_CONNECTION_IDLE_TTL_MS;
    if (result == MAELYS_HTTP_OK) result = maelys_http_client_create(
        http->transport, &limits, &http->client);
    if (result == MAELYS_HTTP_OK) result =
        maelys_http_client_set_connection_reuse(http->client, 1);
    if (result != MAELYS_HTTP_OK) {
        pull_report(http, OCI_ERROR_IO, "HTTPS initialization failed: %s",
            tls_error ? tls_error : maelys_http_result_string(result));
        maelys_http_client_release(http->client);
        maelys_http_transport_release(http->transport);
        maelys_http_tls_provider_release(http->tls);
        http->client = NULL;
        http->transport = NULL;
        http->tls = NULL;
        secret_free(&http->basic_authorization);
        secret_free(&http->bearer_authorization);
    }
    free(tls_error);
    return result == MAELYS_HTTP_OK ? 0 : -1;
}
void http_clear(pull_http_t *http) {
    if (!http) return;
    maelys_http_client_release(http->client);
    maelys_http_transport_release(http->transport);
    maelys_http_tls_provider_release(http->tls);
    secret_free(&http->basic_authorization);
    secret_free(&http->bearer_authorization);
    memset(http, 0, sizeof(*http));
}

int http_get_once(
    pull_http_t *http, const char *authority, const char *target,
    const char *accept, const char *authorization,
    pull_headers_t *headers, pull_body_t *body) {
    headers->body = body;
    body->http = http;
    body->headers_received = 0;
    if (maelys_sys_monotonic_ms(&body->last_progress) != MAELYS_SYS_OK)
        return -1;
    maelys_http_request_t *request = NULL;
    maelys_http_exchange_t *exchange = NULL;
    maelys_http_result_t result = maelys_http_request_config_create(
        "GET", "https", authority, target, &request);
    if (result == MAELYS_HTTP_OK && accept) {
        result = maelys_http_request_add_header(request, "Accept", accept);
    }
    if (result == MAELYS_HTTP_OK && authorization) {
        result = maelys_http_request_add_header(
            request, "Authorization", authorization);
    }
    if (result == MAELYS_HTTP_OK) result =
        maelys_http_request_set_redirect_policy(request, safe_redirect, http);
    if (result == MAELYS_HTTP_OK) result =
        maelys_http_request_set_response_headers(
            request, capture_headers, headers);
    if (result == MAELYS_HTTP_OK) result =
        maelys_http_request_set_response_sink(request, capture_body, body);
    uint64_t deadline = http->deadline;
    if (body->maximum == PULL_TOKEN_JSON_MAX &&
        body->last_progress <= UINT64_MAX - PULL_PHASE_TIMEOUT_MS &&
        deadline > body->last_progress + PULL_PHASE_TIMEOUT_MS)
        deadline = body->last_progress + PULL_PHASE_TIMEOUT_MS;
    for (unsigned attempt = 0u; result == MAELYS_HTTP_OK && attempt < 2u; ++attempt) {
        if (http->requests >= PULL_REQUEST_MAX) {
            pull_report(http, OCI_ERROR_PROTOCOL, "pull request limit exceeded");
            result = MAELYS_HTTP_ERR_LIMIT;
            break;
        }
        ++http->requests;
        http->cross_authority = 0;
        result = maelys_http_exchange_create(
            http->client, request, deadline, &exchange);
        while (result == MAELYS_HTTP_OK || result == MAELYS_HTTP_AGAIN) {
            uint64_t now;
            if (maelys_sys_monotonic_ms(&now) != MAELYS_SYS_OK ||
                now >= deadline || now - body->last_progress >= PULL_PHASE_TIMEOUT_MS) {
                maelys_http_exchange_cancel(exchange);
                pull_report(http, OCI_ERROR_IO,
                    "pull deadline or connection/TLS/read deadline exceeded");
                result = MAELYS_HTTP_ERR_IO;
                break;
            }
            result = maelys_http_exchange_advance(exchange);
            if (result != MAELYS_HTTP_AGAIN) break;
        }
        maelys_http_exchange_release(exchange);
        exchange = NULL;
        /* A peer may close after the idle probe. GET is safe to repeat once
         * before response headers are accepted; failed exchanges destroy their
         * connection. Preserve every budget and never resume a partial body. */
        uint64_t now;
        if (attempt != 0u || result != MAELYS_HTTP_ERR_IO ||
            body->headers_received || headers->invalid ||
            maelys_sys_monotonic_ms(&now) != MAELYS_SYS_OK ||
            now >= deadline || now - body->last_progress >= PULL_PHASE_TIMEOUT_MS)
            break;
        result = MAELYS_HTTP_OK;
    }
    if (result != MAELYS_HTTP_COMPLETE) {
        pull_report(http, (result == MAELYS_HTTP_ERR_LIMIT || result == MAELYS_HTTP_ERR_SYNTAX ||
            result == MAELYS_HTTP_ERR_FRAMING) ? OCI_ERROR_PROTOCOL : OCI_ERROR_IO, "HTTPS exchange failed: %s",
            maelys_http_result_string(result));
    }
    maelys_http_request_release(request);
    headers->body = NULL;
    return result == MAELYS_HTTP_COMPLETE && !headers->invalid ? 0 : -1;
}
