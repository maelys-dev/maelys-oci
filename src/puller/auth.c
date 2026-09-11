/* SPDX-License-Identifier: MPL-2.0 */
#include "src/puller/internal.h"

#include <maelys/sys/file.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

void secret_wipe(void *bytes, size_t size) {
    volatile unsigned char *cursor = bytes;
    while (size--) *cursor++ = 0u;
}

unsigned char *read_private_file(
    const char *path, size_t maximum, size_t *out_size) {
    *out_size = 0u;
    if (!path || path[0] != '/' || maximum == SIZE_MAX ||
        oci_store_validate_ancestors(path) != 0) return NULL;
    maelys_sys_file_expectations_t expectations = {
        .check_owner = 1, .owner = geteuid(),
        .forbidden_mode_bits = S_IRWXG | S_IRWXO,
        .require_single_link = 1, .bound_size = 1, .maximum_size = maximum
    };
    int descriptor = -1;
    maelys_sys_file_identity_t before, after, at_path;
    maelys_sys_result_t opened = maelys_sys_file_open_trusted(
        path, &expectations, &before, NULL, &descriptor);
    if (opened != MAELYS_SYS_OK) {
        errno = opened == MAELYS_SYS_ERR_NOT_FOUND ? ENOENT : EACCES;
        return NULL;
    }
    unsigned char *bytes = calloc(maximum + 1u, 1u);
    size_t size = 0u;
    int valid = bytes && maelys_sys_file_read_bounded(descriptor, bytes,
        maximum, &size) == MAELYS_SYS_OK && size &&
        maelys_sys_file_verify(descriptor, &expectations, &after, NULL) ==
            MAELYS_SYS_OK &&
        maelys_sys_file_identity_same(&before, &after) &&
        before.size == after.size && size == before.size &&
        maelys_sys_file_path_identity(path, &at_path) == MAELYS_SYS_OK &&
        maelys_sys_file_identity_same(&before, &at_path);
    (void)maelys_sys_fd_close(&descriptor);
    if (!valid) {
        if (bytes) secret_wipe(bytes, maximum + 1u);
        free(bytes);
        errno = EACCES;
        return NULL;
    }
    *out_size = size;
    return bytes;
}

char *read_secret_file(const char *path, size_t maximum) {
    size_t allocated = 0u;
    unsigned char *bytes = read_private_file(path, maximum, &allocated);
    if (!bytes) return NULL;
    size_t size = allocated;
    while (size && (bytes[size - 1u] == '\n' || bytes[size - 1u] == '\r'))
        --size;
    if (!size || memchr(bytes, '\0', size)) {
        secret_wipe(bytes, allocated + 1u);
        free(bytes);
        return NULL;
    }
    for (size_t index = 0u; index < size; ++index) {
        unsigned char byte = (unsigned char)bytes[index];
        if (byte < 0x21u || byte == 0x7fu) {
            secret_wipe(bytes, allocated + 1u);
            free(bytes);
            return NULL;
        }
    }
    if (size < allocated) memset(bytes + size, 0, allocated - size);
    bytes[size] = '\0';
    return (char *)bytes;
}

char *authorization_value(const char *scheme, const char *credential) {
    size_t size;
    char *value;
    if (!credential || !*credential || strlen(credential) > PULL_SECRET_MAX)
        return NULL;
    for (const unsigned char *cursor = (const unsigned char *)credential;
         *cursor; ++cursor)
        if (*cursor < 0x21u || *cursor > 0x7eu) return NULL;
    if (!scheme || !credential ||
        strlen(scheme) > SIZE_MAX - strlen(credential) - 2u) return NULL;
    size = strlen(scheme) + strlen(credential) + 2u;
    value = malloc(size);
    if (value) (void)oci_snprintf(value, size, "%s %s", scheme, credential);
    return value;
}

void secret_free(char **value) {
    if (!value || !*value) return;
    secret_wipe(*value, strlen(*value));
    free(*value);
    *value = NULL;
}

static int base64_digit(unsigned char byte) {
    if (byte >= 'A' && byte <= 'Z') return (int)(byte - 'A');
    if (byte >= 'a' && byte <= 'z') return (int)(byte - 'a') + 26;
    if (byte >= '0' && byte <= '9') return (int)(byte - '0') + 52;
    if (byte == '+') return 62;
    if (byte == '/') return 63;
    return -1;
}

/* Decode and validate the bounded user:password bytes, including canonical
 * padding bits. The wire representation is retained only after this check. */
int basic_credential_valid(const char *value) {
    size_t size = value ? strlen(value) : 0u;
    if (!size || size % 4u || size > PULL_SECRET_MAX) return 0;
    unsigned char decoded[PULL_SECRET_MAX];
    size_t used = 0u;
    int valid = 1;
    for (size_t i = 0u; valid && i < size; i += 4u) {
        int a = base64_digit((unsigned char)value[i]);
        int b = base64_digit((unsigned char)value[i + 1u]);
        int c = value[i + 2u] == '=' ? 0 :
            base64_digit((unsigned char)value[i + 2u]);
        int d = value[i + 3u] == '=' ? 0 :
            base64_digit((unsigned char)value[i + 3u]);
        int pad = value[i + 2u] == '=' ? 2 : value[i + 3u] == '=' ? 1 : 0;
        if (a < 0 || b < 0 || c < 0 || d < 0 ||
            (pad && i + 4u != size) ||
            (pad == 2 && (value[i + 3u] != '=' || (b & 15))) ||
            (pad == 1 && (c & 3))) { valid = 0; break; }
        decoded[used++] = (unsigned char)((unsigned)a << 2u | (unsigned)b >> 4u);
        if (pad < 2) decoded[used++] =
            (unsigned char)((unsigned)b << 4u | (unsigned)c >> 2u);
        if (!pad) decoded[used++] = (unsigned char)((unsigned)c << 6u | (unsigned)d);
    }
    const unsigned char *colon = memchr(decoded, ':', used);
    valid = valid && colon && colon != decoded;
    for (size_t i = 0u; valid && i < used; ++i)
        valid = decoded[i] >= 0x20u && decoded[i] != 0x7fu;
    secret_wipe(decoded, sizeof(decoded));
    return valid;
}

static char *default_docker_config(void) {
    const char *directory = getenv("DOCKER_CONFIG");
    const char *home = getenv("HOME");
    const char *suffix = directory ? "/config.json" : "/.docker/config.json";
    const char *base = directory ? directory : home;
    if (!base || base[0] != '/') return NULL;
    if (strlen(base) > SIZE_MAX - strlen(suffix) - 1u) return NULL;
    size_t size = strlen(base) + strlen(suffix) + 1u;
    char *path = malloc(size);
    if (path) (void)oci_snprintf(path, size, "%s%s", base, suffix);
    return path;
}

static char *copy_secret_view(maelys_json_view_t view) {
    if (!view.size || view.size > 16384u || view.size == SIZE_MAX ||
        memchr(view.data, '\0', view.size))
        return NULL;
    char *copy = malloc(view.size + 1u);
    if (!copy) return NULL;
    memcpy(copy, view.data, view.size);
    copy[view.size] = '\0';
    return copy;
}

int load_docker_authorization(
    pull_http_t *http, const char *configured_path, const char *authority) {
    const char *docker_directory = getenv("DOCKER_CONFIG");
    if (!configured_path && docker_directory && docker_directory[0] != '/')
        return -1;
    char *owned_path = configured_path ? NULL : default_docker_config();
    const char *path = configured_path ? configured_path : owned_path;
    if (!path || path[0] != '/') {
        free(owned_path);
        return configured_path ? -1 : 0;
    }
    size_t config_size = 0u;
    errno = 0;
    unsigned char *config = read_private_file(
        path, 1024u * 1024u, &config_size);
    if (!config) {
        int absent = errno == ENOENT && !configured_path;
        free(owned_path);
        return absent ? 0 : -1;
    }
    maelys_json_document_t *document = oci_json_parse_object(
        config, config_size, OCI_JSON_TOKENS_MAX);
    secret_wipe(config, config_size + 1u);
    free(config);
    free(owned_path);
    if (!document) return -1;
    maelys_json_value_t root = maelys_json_document_root(document);
    maelys_json_value_t helpers;
    maelys_json_result_t helpers_result = maelys_json_object_get(
        document, root, "credHelpers", &helpers);
    size_t helper_count = 0u;
    if (helpers_result == MAELYS_JSON_OK &&
        maelys_json_object_size(document, helpers, &helper_count) !=
            MAELYS_JSON_OK) {
        maelys_json_document_release(document);
        return -1;
    }
    if (helpers_result != MAELYS_JSON_OK &&
        helpers_result != MAELYS_JSON_ERR_NOT_FOUND) {
        maelys_json_document_release(document);
        return -1;
    }
    maelys_json_view_t store = {0};
    int store_result = oci_json_optional_string(
        document, root, "credsStore", &store);
    if (store_result < 0) {
        maelys_json_document_release(document);
        return -1;
    }
    http->helper_credentials_present = helper_count != 0u || store_result == 1;
    maelys_json_value_t auths;
    maelys_json_result_t auths_result = maelys_json_object_get(
        document, root, "auths", &auths);
    if (auths_result == MAELYS_JSON_OK &&
        maelys_json_value_type(document, auths) != MAELYS_JSON_TYPE_OBJECT) {
        maelys_json_document_release(document);
        return -1;
    }
    if (auths_result != MAELYS_JSON_OK &&
        auths_result != MAELYS_JSON_ERR_NOT_FOUND) {
        maelys_json_document_release(document);
        return -1;
    }
    maelys_json_value_t entry = 0u;
    char https_key[320], v1_key[336];
    if (oci_snprintf(https_key, sizeof(https_key), "https://%s", authority) < 0 ||
        oci_snprintf(v1_key, sizeof(v1_key), "https://%s/v1/", authority) < 0) {
        maelys_json_document_release(document); return -1;
    }
    const char *keys[] = {authority, https_key, v1_key, NULL};
    if (strcasecmp(authority, "registry-1.docker.io") == 0 ||
        strcasecmp(authority, "docker.io") == 0)
        keys[3] = "https://index.docker.io/v1/";
    size_t matches = 0u;
    for (size_t i = 0u; auths_result == MAELYS_JSON_OK && i < 4u; ++i) {
        if (!keys[i]) continue;
        maelys_json_value_t candidate;
        maelys_json_result_t found = maelys_json_object_get(document, auths, keys[i], &candidate);
        if (found == MAELYS_JSON_OK) { ++matches; entry = candidate; }
        else if (found != MAELYS_JSON_ERR_NOT_FOUND) { matches = 2u; break; }
    }
    if (matches > 1u) {
        maelys_json_document_release(document); return -1;
    }
    int entry_present = matches == 1u;
    if (entry_present) {
        if (maelys_json_value_type(document, entry) != MAELYS_JSON_TYPE_OBJECT) {
            maelys_json_document_release(document);
            return -1;
        }
        maelys_json_view_t identity_view = {0};
        maelys_json_view_t auth_view = {0};
        int identity_result = oci_json_optional_string(
            document, entry, "identitytoken", &identity_view);
        int auth_result = oci_json_optional_string(
            document, entry, "auth", &auth_view);
        char *identity = identity_result == 1 ?
            copy_secret_view(identity_view) : NULL;
        char *auth = auth_result == 1 ? copy_secret_view(auth_view) : NULL;
        if (identity_result < 0 || auth_result < 0 ||
            (identity_result == 1 && !identity) ||
            (auth_result == 1 && !auth)) {
            secret_free(&identity);
            secret_free(&auth);
            maelys_json_document_release(document);
            return -1;
        }
        if (identity) {
            http->bearer_authorization = authorization_value("Bearer", identity);
        } else if (auth && basic_credential_valid(auth)) {
            http->basic_authorization = authorization_value("Basic", auth);
        } else if (auth) {
            secret_free(&auth);
            maelys_json_document_release(document);
            return -1;
        }
        secret_free(&identity);
        secret_free(&auth);
    }
    maelys_json_document_release(document);
    return http->basic_authorization || http->bearer_authorization ||
        !entry_present ? 0 : -1;
}

static int realm_query_valid(const char *query) {
    if (!query) return 1;
    ++query;
    if (!*query) return 0;
    while (*query) {
        const char *end = strchr(query, '&');
        if (!end) end = query + strlen(query);
        const char *equal = memchr(query, '=', (size_t)(end - query));
        if (!equal || equal == query) return 0;
        size_t key_size = (size_t)(equal - query);
        for (const char *p = query; p < equal; ++p)
            if (!( (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                   (*p >= '0' && *p <= '9') || *p == '_' || *p == '-')) return 0;
        static const char *const reserved[] = {"scope", "service", "account", "client_id",
            "access_token", "token", "refresh_token", "grant_type"};
        for (size_t i = 0u; i < sizeof(reserved) / sizeof(reserved[0]); ++i)
            if (strlen(reserved[i]) == key_size && strncasecmp(query, reserved[i], key_size) == 0)
                return 0;
        if (!*end) return 1;
        query = end + 1u;
        if (!*query) return 0;
    }
    return 1;
}

static int parse_https_url(
    const char *url, char **out_authority, char **out_target) {
    *out_authority = NULL;
    *out_target = NULL;
    static const char prefix[] = "https://";
    if (!url || strncmp(url, prefix, sizeof(prefix) - 1u) != 0 ||
        strchr(url, '#') || strchr(url, '@') ||
        strlen(url) > PULL_TARGET_MAX) return -1;
    const char *authority = url + sizeof(prefix) - 1u;
    const char *path = strchr(authority, '/');
    const char *query = strchr(authority, '?');
    if (!path || (query && query < path)) path = query;
    size_t authority_size = path ? (size_t)(path - authority) : strlen(authority);
    if (!authority_size) return -1;
    *out_authority = strndup(authority, authority_size);
    if (!*out_authority || !authority_valid(*out_authority)) goto invalid;
    if (!path) *out_target = strdup("/");
    else if (*path == '/') *out_target = strdup(path);
    else {
        size_t length = strlen(path);
        *out_target = malloc(length + 2u);
        if (*out_target) {
            (*out_target)[0] = '/';
            memcpy(*out_target + 1u, path, length + 1u);
        }
    }
    if (!*out_target || !realm_query_valid(strchr(*out_target, '?'))) goto invalid;
    return 0;
invalid:
    free(*out_authority);
    free(*out_target);
    *out_authority = NULL;
    *out_target = NULL;
    return -1;
}

void challenge_clear(pull_challenge_t *challenge) {
    free(challenge->realm);
    free(challenge->service);
    free(challenge->scope);
    memset(challenge, 0, sizeof(*challenge));
}

int challenge_parse(const char *text, pull_challenge_t *out) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    if (!text || strnlen(text, PULL_SECRET_MAX + 1u) > PULL_SECRET_MAX ||
        strncasecmp(text, "Bearer", 6u) != 0 ||
        (text[6] != ' ' && text[6] != '\t')) return -1;
    const char *cursor = text + 6u;
    char names[16][64];
    size_t count = 0u;
    while (*cursor) {
        while (*cursor == ' ' || *cursor == '\t') ++cursor;
        if (count == 16u) goto invalid;
        size_t size = 0u;
        while ((*cursor >= 'A' && *cursor <= 'Z') ||
               (*cursor >= 'a' && *cursor <= 'z') ||
               (*cursor >= '0' && *cursor <= '9') ||
               *cursor == '_' || *cursor == '-') {
            if (size == sizeof(names[0]) - 1u) goto invalid;
            names[count][size++] = *cursor++;
        }
        if (!size) goto invalid;
        names[count][size] = '\0';
        for (size_t i = 0u; i < count; ++i)
            if (strcasecmp(names[i], names[count]) == 0) goto invalid;
        while (*cursor == ' ' || *cursor == '\t') ++cursor;
        if (*cursor != '=') goto invalid;
        ++cursor;
        while (*cursor == ' ' || *cursor == '\t') ++cursor;
        if (*cursor != '"') goto invalid;
        ++cursor;
        char *value = malloc(strlen(cursor) + 1u);
        if (!value) goto invalid;
        size_t used = 0u;
        while (*cursor && *cursor != '"') {
            if (*cursor == '\\') ++cursor;
            unsigned char byte = (unsigned char)*cursor;
            if (byte < 0x20u || byte > 0x7eu) { free(value); goto invalid; }
            value[used++] = *cursor++;
        }
        if (*cursor != '"' || !used) { free(value); goto invalid; }
        value[used] = '\0';
        ++cursor;
        if (strcasecmp(names[count], "realm") == 0) out->realm = value;
        else if (strcasecmp(names[count], "service") == 0) out->service = value;
        else if (strcasecmp(names[count], "scope") == 0) out->scope = value;
        else free(value);
        ++count;
        while (*cursor == ' ' || *cursor == '\t') ++cursor;
        if (!*cursor) break;
        if (*cursor++ != ',' || !*cursor) goto invalid;
    }
    if (!out->realm || !out->service || strlen(out->service) > 253u)
        goto invalid;
    for (const unsigned char *p = (const unsigned char *)out->service; *p; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || strchr(".-_:", *p))) goto invalid;
    return 0;
invalid:
    challenge_clear(out);
    return -1;
}

static char *percent_encode(const char *value) {
    static const char hex[] = "0123456789ABCDEF";
    size_t length = value ? strlen(value) : 0u;
    if (length > (SIZE_MAX - 1u) / 3u) return NULL;
    char *encoded = malloc(length * 3u + 1u);
    if (!encoded) return NULL;
    size_t output = 0u;
    for (size_t index = 0u; index < length; ++index) {
        unsigned char byte = (unsigned char)value[index];
        if ((byte >= 'A' && byte <= 'Z') ||
            (byte >= 'a' && byte <= 'z') ||
            (byte >= '0' && byte <= '9') || byte == '-' || byte == '.' ||
            byte == '_' || byte == '~') {
            encoded[output++] = (char)byte;
        } else {
            encoded[output++] = '%';
            encoded[output++] = hex[byte >> 4u];
            encoded[output++] = hex[byte & 0x0fu];
        }
    }
    encoded[output] = '\0';
    return encoded;
}

int acquire_bearer_token(
    pull_http_t *http, const char *challenge, const pull_reference_t *reference) {
    pull_challenge_t parsed;
    if (challenge_parse(challenge, &parsed) != 0) {
        pull_report(http, OCI_ERROR_PROTOCOL, "malformed Bearer challenge");
        return -1;
    }
    char expected_scope[PULL_TARGET_MAX];
    if (oci_snprintf(expected_scope, sizeof(expected_scope), "repository:%s:pull",
            reference->repository) < 0 ||
        (parsed.scope && strcmp(parsed.scope, expected_scope) != 0)) {
        challenge_clear(&parsed);
        pull_report(http, OCI_ERROR_ACCESS, "Bearer scope differs from the requested repository:pull scope");
        return -1;
    }
    char *realm = parsed.realm;
    char *service = parsed.service;
    free(parsed.scope);
    char *authority = NULL;
    char *target = NULL;
    if (parse_https_url(realm, &authority, &target) != 0) {
        pull_report(http, OCI_ERROR_PROTOCOL, "Bearer realm must be a safe HTTPS URL with unambiguous scope and service");
        free(realm); free(service);
        return -1;
    }
    if (http->basic_authorization &&
        strcasecmp(authority, reference->authority) != 0) {
        pull_report(http, OCI_ERROR_ACCESS,
            "refusing to delegate registry credentials to a cross-authority "
            "Bearer realm");
        free(realm); free(service); free(authority); free(target);
        return -1;
    }
    char *encoded_scope = percent_encode(expected_scope);
    char *encoded_service = percent_encode(service);
    if (!encoded_scope || (service && !encoded_service)) {
        free(realm); free(service); free(authority); free(target);
        free(encoded_scope); free(encoded_service);
        return -1;
    }
    size_t query_size = strlen(target) + strlen(encoded_scope) +
        (encoded_service ? strlen(encoded_service) : 0u) + 32u;
    char *query = malloc(query_size);
    if (query) (void)oci_snprintf(query, query_size, "%s%cscope=%s%s%s",
        target, strchr(target, '?') ? '&' : '?', encoded_scope,
        encoded_service ? "&service=" : "",
        encoded_service ? encoded_service : "");
    free(realm); free(service); free(target);
    free(encoded_scope); free(encoded_service);
    if (!query) { free(authority); return -1; }
    pull_headers_t headers = {0};
    pull_body_t body = {.maximum = PULL_TOKEN_JSON_MAX, .fd = -1};
    int result = http_get_once(
        http, authority, query, "application/json",
        http->basic_authorization, &headers, &body);
    free(authority); free(query);
    if (result != 0 || headers.status != 200u ||
        !headers.content_type ||
        !oci_media_type_equal(headers.content_type, "application/json")) {
        headers_clear(&headers); body_clear(&body); return -1;
    }
    maelys_json_document_t *document = oci_json_parse_object(
        body.bytes, body.size, 1024u);
    maelys_json_value_t root = maelys_json_document_root(document);
    maelys_json_view_t token = {0};
    int token_result = document ? oci_json_optional_string(
        document, root, "token", &token) : -1;
    maelys_json_view_t access_token = {0};
    int access_result = document ? oci_json_optional_string(
        document, root, "access_token", &access_token) : -1;
    if (token_result == 0) { token_result = access_result; token = access_token; }
    else if (access_result < 0 || (access_result == 1 &&
        (access_token.size != token.size ||
         memcmp(access_token.data, token.data, token.size) != 0))) token_result = -1;
    char *token_copy = NULL;
    if (token_result == 1 && token.size <= PULL_SECRET_MAX) {
        token_copy = malloc(token.size + 1u);
        if (token_copy) {
            memcpy(token_copy, token.data, token.size);
            token_copy[token.size] = '\0';
        }
    }
    char *authorization = token_copy ?
        authorization_value("Bearer", token_copy) : NULL;
    if (token_copy) {
        secret_wipe(token_copy, token.size);
        free(token_copy);
    }
    maelys_json_document_release(document);
    headers_clear(&headers);
    if (body.bytes) secret_wipe(body.bytes, body.size);
    body_clear(&body);
    if (!authorization) return -1;
    secret_free(&http->bearer_authorization);
    http->bearer_authorization = authorization;
    return 0;
}
