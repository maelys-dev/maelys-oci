/* SPDX-License-Identifier: MPL-2.0 */
#include <maelys/oci.h>
#include <type_traits>
static_assert(MAELYS_OCI_ABI_VERSION == 7, "remote image metadata");
static_assert(MAELYS_OCI_ABI_COMPATIBLE_SINCE == 4, "public store operations");
static_assert(MAELYS_OCI_ABI_COMPATIBLE_SINCE <= MAELYS_OCI_ABI_VERSION,
    "the floor is a revision that exists");
/* The check the header gives a consumer, for the oldest revision served and
 * for the newest: both must pass against this header. */
#define SERVED(N) \
    (MAELYS_OCI_ABI_COMPATIBLE_SINCE <= (N) && (N) <= MAELYS_OCI_ABI_VERSION)
#if !SERVED(4u) || !SERVED(7u)
#error "a consumer of revision 4 or 7 must be served"
#endif
#if SERVED(3u) || SERVED(8u)
#error "a consumer of a broken or of a future revision must be refused"
#endif
static_assert(std::is_pointer<maelys_oci_pull_options_t *>::value, "opaque options");
static_assert(std::is_pointer<maelys_oci_pull_result_t *>::value, "opaque receipt");
static_assert(std::is_pointer<maelys_oci_document_t *>::value, "opaque document");
static_assert(std::is_pointer<maelys_oci_import_options_t *>::value, "opaque import options");
static_assert(std::is_pointer<maelys_oci_artifact_t *>::value, "opaque artifact");
