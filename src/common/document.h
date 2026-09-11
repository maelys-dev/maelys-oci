/* SPDX-License-Identifier: MPL-2.0 */
#ifndef MAELYS_OCI_DOCUMENT_H
#define MAELYS_OCI_DOCUMENT_H
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* Owned operation documents. JSON syntax, UTF-8, duplicate-key rejection and
 * serialization belong exclusively to maelys-json. This tree only supports
 * construction and borrowed traversal of the integer-valued store contracts. */
typedef struct oci_document oci_document_t;
typedef struct oci_document_member {
    const char *name;
    oci_document_t *value;
} oci_document_member_t;

oci_document_t *oci_document_object(void);
oci_document_t *oci_document_array(void);
oci_document_t *oci_document_string(const char *value);
oci_document_t *oci_document_integer(int64_t value);
oci_document_t *oci_document_boolean(int value);
void oci_document_release(oci_document_t *document);
/* put, append and members consume all passed values, including on failure.
 * set shares its value with a retained reference. Getters borrow their result. */
int oci_document_put(oci_document_t *object, const char *key, oci_document_t *value);
int oci_document_set(oci_document_t *object, const char *key, oci_document_t *value);
int oci_document_append(oci_document_t *array, oci_document_t *value);
int oci_document_delete(oci_document_t *object, const char *key);
oci_document_t *oci_document_members(const oci_document_member_t *members, size_t count);
#define OCI_DOCUMENT_OBJECT(...) oci_document_members( \
    (const oci_document_member_t[]){__VA_ARGS__}, \
    sizeof((const oci_document_member_t[]){__VA_ARGS__}) / sizeof(oci_document_member_t))
oci_document_t *oci_document_get(const oci_document_t *object, const char *key);
oci_document_t *oci_document_at(const oci_document_t *array, size_t index);
const char *oci_document_string_value(const oci_document_t *value);
int64_t oci_document_integer_value(const oci_document_t *value);
size_t oci_document_object_size(const oci_document_t *value);
size_t oci_document_array_size(const oci_document_t *value);
int oci_document_is_object(const oci_document_t *value);
int oci_document_is_array(const oci_document_t *value);
int oci_document_is_integer(const oci_document_t *value);
oci_document_t *oci_document_parse(const unsigned char *bytes, size_t size);
char *oci_document_dump(const oci_document_t *document);
int oci_document_dump_file(const oci_document_t *document, FILE *stream);
#endif
