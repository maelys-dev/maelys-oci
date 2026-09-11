/* SPDX-License-Identifier: MPL-2.0 */
#include "src/common/internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void maelys_oci_error_free(char *error) {
    free(error);
}

static char *format_message(const char *format, va_list arguments)
    OCI_PRINTF(1, 0);

static char *format_message(const char *format, va_list arguments) {
    va_list copy;
    va_copy(copy, arguments);
    int length = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (length < 0) return NULL;
    char *message = malloc((size_t)length + 1u);
    if (message) (void)vsnprintf(message, (size_t)length + 1u, format, arguments);
    return message;
}

void maelys_oci_set_error(char **out_error, const char *format, ...) {
    if (!out_error || *out_error) return;
    va_list arguments;
    va_start(arguments, format);
    *out_error = format_message(format, arguments);
    va_end(arguments);
}

void oci_error_report(
    oci_error_t *error, oci_error_kind_t kind, const char *format, ...) {
    if (!error) return;
    va_list arguments;
    va_start(arguments, format);
    char *message = format_message(format, arguments);
    va_end(arguments);
    if (!message) {
        if (!error->message) error->kind = OCI_ERROR_MEMORY;
        return;
    }
    if (!error->message) {
        error->kind = kind == OCI_ERROR_NONE ? OCI_ERROR_IO : kind;
        error->message = message;
        return;
    }
    size_t existing = strlen(error->message);
    size_t appended = strlen(message);
    char *joined = realloc(error->message, existing + appended + 3u);
    if (joined) {
        memcpy(joined + existing, "; ", 2u);
        memcpy(joined + existing + 2u, message, appended + 1u);
        error->message = joined;
    }
    free(message);
}

void oci_error_clear(oci_error_t *error) {
    if (!error) return;
    free(error->message);
    error->message = NULL;
    error->kind = OCI_ERROR_NONE;
}

const char *oci_error_message(const oci_error_t *error) {
    return error && error->message ? error->message : "";
}
