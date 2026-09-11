/* SPDX-License-Identifier: MPL-2.0 */
#include <maelys/oci.h>
#include <type_traits>
static_assert(MAELYS_OCI_ABI_VERSION == 4, "public store operations");
static_assert(std::is_pointer<maelys_oci_pull_options_t *>::value, "opaque options");
static_assert(std::is_pointer<maelys_oci_pull_result_t *>::value, "opaque receipt");
static_assert(std::is_pointer<maelys_oci_document_t *>::value, "opaque document");
static_assert(std::is_pointer<maelys_oci_import_options_t *>::value, "opaque import options");
static_assert(std::is_pointer<maelys_oci_artifact_t *>::value, "opaque artifact");
