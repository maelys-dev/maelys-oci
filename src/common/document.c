/* SPDX-License-Identifier: MPL-2.0 */
#include "src/common/document.h"
#include <maelys/json.h>
#include <stdlib.h>
#include <string.h>

#define DOCUMENT_BYTES (8u * 1024u * 1024u)
#define DOCUMENT_NODES 32768u
#define DOCUMENT_DEPTH 64u

typedef struct member {
    char *key;
    oci_document_t *value;
} member_t;
struct oci_document {
    maelys_json_type_t type;
    size_t references;
    char *string;
    int64_t integer;
    member_t *members;
    size_t count;
    size_t capacity;
};

static oci_document_t *create(maelys_json_type_t type) {
    oci_document_t *value = calloc(1u, sizeof(*value));
    if (value) { value->type = type; value->references = 1u; }
    return value;
}
oci_document_t *oci_document_object(void) { return create(MAELYS_JSON_TYPE_OBJECT); }
oci_document_t *oci_document_array(void) { return create(MAELYS_JSON_TYPE_ARRAY); }
oci_document_t *oci_document_string(const char *text) {
    if (!text || strnlen(text, DOCUMENT_BYTES + 1u) > DOCUMENT_BYTES) return NULL;
    oci_document_t *value = create(MAELYS_JSON_TYPE_STRING);
    if (value) {
        value->string = strdup(text);
        if (!value->string) { free(value); return NULL; }
    }
    return value;
}
oci_document_t *oci_document_integer(int64_t integer) {
    oci_document_t *value = create(MAELYS_JSON_TYPE_NUMBER);
    if (value) value->integer = integer;
    return value;
}
oci_document_t *oci_document_boolean(int boolean) {
    oci_document_t *value = create(MAELYS_JSON_TYPE_BOOLEAN);
    if (value) value->integer = boolean != 0;
    return value;
}
void oci_document_release(oci_document_t *value) {
    if (!value || --value->references) return;
    for (size_t i = 0u; i < value->count; ++i) {
        free(value->members[i].key);
        oci_document_release(value->members[i].value);
    }
    free(value->members);
    free(value->string);
    free(value);
}
static int reserve(oci_document_t *value) {
    if (value->count == DOCUMENT_NODES) return -1;
    if (value->count < value->capacity) return 0;
    size_t capacity = value->capacity ? value->capacity * 2u : 8u;
    member_t *members = realloc(value->members, capacity * sizeof(*members));
    if (!members) return -1;
    value->members = members;
    value->capacity = capacity;
    return 0;
}
int oci_document_is_object(const oci_document_t *v) { return v && v->type == MAELYS_JSON_TYPE_OBJECT; }
int oci_document_is_array(const oci_document_t *v) { return v && v->type == MAELYS_JSON_TYPE_ARRAY; }
int oci_document_is_integer(const oci_document_t *v) { return v && v->type == MAELYS_JSON_TYPE_NUMBER; }
size_t oci_document_object_size(const oci_document_t *v) { return oci_document_is_object(v) ? v->count : 0u; }
size_t oci_document_array_size(const oci_document_t *v) { return oci_document_is_array(v) ? v->count : 0u; }
const char *oci_document_string_value(const oci_document_t *v) {
    return v && v->type == MAELYS_JSON_TYPE_STRING ? v->string : NULL;
}
int64_t oci_document_integer_value(const oci_document_t *v) {
    return oci_document_is_integer(v) ? v->integer : 0;
}
oci_document_t *oci_document_get(const oci_document_t *object, const char *key) {
    if (!oci_document_is_object(object) || !key) return NULL;
    for (size_t i = 0u; i < object->count; ++i)
        if (strcmp(object->members[i].key, key) == 0) return object->members[i].value;
    return NULL;
}
oci_document_t *oci_document_at(const oci_document_t *array, size_t index) {
    return oci_document_is_array(array) && index < array->count ? array->members[index].value : NULL;
}
int oci_document_put(oci_document_t *object, const char *key, oci_document_t *value) {
    if (!oci_document_is_object(object) || !key || !value || value == object ||
        strnlen(key, DOCUMENT_BYTES + 1u) > DOCUMENT_BYTES) {
        oci_document_release(value); return -1;
    }
    for (size_t i = 0u; i < object->count; ++i) {
        if (strcmp(object->members[i].key, key) == 0) {
            oci_document_release(object->members[i].value);
            object->members[i].value = value;
            return 0;
        }
    }
    char *owned_key = strdup(key);
    if (!owned_key || reserve(object) != 0) {
        free(owned_key); oci_document_release(value); return -1;
    }
    object->members[object->count++] = (member_t){owned_key, value};
    return 0;
}
int oci_document_set(oci_document_t *object, const char *key, oci_document_t *value) {
    if (!value || value->references == SIZE_MAX || value == object) return -1;
    ++value->references;
    return oci_document_put(object, key, value);
}
int oci_document_append(oci_document_t *array, oci_document_t *value) {
    if (!oci_document_is_array(array) || !value || array == value || reserve(array) != 0) {
        oci_document_release(value); return -1;
    }
    array->members[array->count++] = (member_t){NULL, value};
    return 0;
}
int oci_document_delete(oci_document_t *object, const char *key) {
    if (!oci_document_is_object(object) || !key) return -1;
    for (size_t i = 0u; i < object->count; ++i) {
        if (strcmp(object->members[i].key, key) != 0) continue;
        free(object->members[i].key);
        oci_document_release(object->members[i].value);
        memmove(&object->members[i], &object->members[i + 1u],
            (object->count - i - 1u) * sizeof(*object->members));
        --object->count;
        return 0;
    }
    return -1;
}
oci_document_t *oci_document_members(const oci_document_member_t *members, size_t count) {
    oci_document_t *object = oci_document_object();
    int failed = !object;
    for (size_t i = 0u; i < count; ++i) {
        if (failed) oci_document_release(members[i].value);
        else if (oci_document_put(object, members[i].name, members[i].value) != 0) failed = 1;
    }
    if (failed) { oci_document_release(object); return NULL; }
    return object;
}

/* Conversion starts only after maelys-json has validated the complete input. */
static oci_document_t *copy_value(const maelys_json_document_t *document,
    maelys_json_value_t value, size_t depth) {
    if (depth > DOCUMENT_DEPTH) return NULL;
    maelys_json_type_t type = maelys_json_value_type(document, value);
    if (type == MAELYS_JSON_TYPE_STRING) {
        maelys_json_view_t view;
        if (maelys_json_value_string(document, value, &view) != MAELYS_JSON_OK) return NULL;
        char *text = strndup(view.data, view.size);
        oci_document_t *copy = text ? oci_document_string(text) : NULL;
        free(text);
        return copy;
    }
    if (type == MAELYS_JSON_TYPE_NUMBER) {
        int64_t integer;
        return maelys_json_value_i64(document, value, &integer) == MAELYS_JSON_OK ?
            oci_document_integer(integer) : NULL;
    }
    if (type == MAELYS_JSON_TYPE_BOOLEAN) {
        int boolean;
        return maelys_json_value_boolean(document, value, &boolean) == MAELYS_JSON_OK ?
            oci_document_boolean(boolean) : NULL;
    }
    if (type == MAELYS_JSON_TYPE_NULL) return create(type);
    if (type != MAELYS_JSON_TYPE_OBJECT && type != MAELYS_JSON_TYPE_ARRAY) return NULL;
    size_t count;
    maelys_json_result_t counted = type == MAELYS_JSON_TYPE_OBJECT ?
        maelys_json_object_size(document, value, &count) : maelys_json_array_size(document, value, &count);
    if (counted != MAELYS_JSON_OK || count > DOCUMENT_NODES) return NULL;
    oci_document_t *copy = create(type);
    if (!copy) return NULL;
    for (size_t i = 0u; i < count; ++i) {
        maelys_json_value_t child;
        maelys_json_view_t key = {0};
        maelys_json_result_t found = type == MAELYS_JSON_TYPE_OBJECT ?
            maelys_json_object_member_at(document, value, i, &key, &child) :
            maelys_json_array_get(document, value, i, &child);
        oci_document_t *item = found == MAELYS_JSON_OK ? copy_value(document, child, depth + 1u) : NULL;
        char *name = type == MAELYS_JSON_TYPE_OBJECT && found == MAELYS_JSON_OK ?
            strndup(key.data, key.size) : NULL;
        int result = type == MAELYS_JSON_TYPE_OBJECT ? oci_document_put(copy, name, item) :
            oci_document_append(copy, item);
        free(name);
        if (result != 0) { oci_document_release(copy); return NULL; }
    }
    return copy;
}
oci_document_t *oci_document_parse(const unsigned char *bytes, size_t size) {
    maelys_json_limits_t limits = {.maximum_bytes = DOCUMENT_BYTES,
        .maximum_depth = DOCUMENT_DEPTH, .maximum_tokens = DOCUMENT_NODES};
    maelys_json_document_t *parsed = NULL;
    maelys_json_error_t error;
    if (maelys_json_document_parse(bytes, size, MAELYS_JSON_PROFILE_RFC8259,
            &limits, &parsed, &error) != MAELYS_JSON_OK) return NULL;
    oci_document_t *document = copy_value(parsed, maelys_json_document_root(parsed), 0u);
    maelys_json_document_release(parsed);
    return document;
}
static int write_value(maelys_json_writer_t *writer, const oci_document_t *value, size_t depth) {
    if (!value || depth > DOCUMENT_DEPTH) return -1;
    switch (value->type) {
    case MAELYS_JSON_TYPE_STRING:
        return maelys_json_writer_string_cstr(writer, value->string) == MAELYS_JSON_OK ? 0 : -1;
    case MAELYS_JSON_TYPE_NUMBER:
        return maelys_json_writer_i64(writer, value->integer) == MAELYS_JSON_OK ? 0 : -1;
    case MAELYS_JSON_TYPE_BOOLEAN:
        return maelys_json_writer_boolean(writer, value->integer != 0) == MAELYS_JSON_OK ? 0 : -1;
    case MAELYS_JSON_TYPE_NULL:
        return maelys_json_writer_null(writer) == MAELYS_JSON_OK ? 0 : -1;
    case MAELYS_JSON_TYPE_OBJECT:
    case MAELYS_JSON_TYPE_ARRAY: {
        int object = value->type == MAELYS_JSON_TYPE_OBJECT;
        maelys_json_result_t started = object ? maelys_json_writer_object_begin(writer) :
            maelys_json_writer_array_begin(writer);
        if (started != MAELYS_JSON_OK) return -1;
        for (size_t i = 0u; i < value->count; ++i) {
            if ((object && maelys_json_writer_key_cstr(writer, value->members[i].key) != MAELYS_JSON_OK) ||
                write_value(writer, value->members[i].value, depth + 1u) != 0) return -1;
        }
        return (object ? maelys_json_writer_object_end(writer) : maelys_json_writer_array_end(writer)) ==
            MAELYS_JSON_OK ? 0 : -1;
    }
    default: return -1;
    }
}
char *oci_document_dump(const oci_document_t *document) {
    maelys_json_limits_t limits = {.maximum_bytes = DOCUMENT_BYTES,
        .maximum_depth = DOCUMENT_DEPTH, .maximum_tokens = DOCUMENT_NODES};
    maelys_json_writer_t *writer = NULL;
    char *bytes = NULL;
    size_t size;
    if (maelys_json_writer_create(MAELYS_JSON_PROFILE_RFC8259, &limits, 0u, &writer) == MAELYS_JSON_OK &&
        write_value(writer, document, 0u) == 0 &&
        maelys_json_writer_finish(writer, &bytes, &size) != MAELYS_JSON_OK) {
        free(bytes); bytes = NULL;
    }
    maelys_json_writer_release(writer);
    return bytes;
}
int oci_document_dump_file(const oci_document_t *document, FILE *stream) {
    char *bytes = oci_document_dump(document);
    if (!bytes) return -1;
    size_t size = strlen(bytes);
    int result = fwrite(bytes, 1u, size, stream) == size ? 0 : -1;
    free(bytes);
    return result;
}
