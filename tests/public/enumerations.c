/* SPDX-License-Identifier: MPL-2.0 */
/* The frozen consumer of every public enumeration. Public enumerations are
 * open: a consumer must hold a value it does not know. Compiled under
 * -Werror=switch, this file stops compiling when an enumerator is added, so
 * the addition is seen here before it is seen by a consumer: it is then a
 * new revision of the interface, named to consumers in the changelog, and
 * its case joins this switch in the same change. */
#include <maelys/oci.h>

const char *result_name(maelys_oci_result_t result);

const char *result_name(maelys_oci_result_t result) {
    switch (result) {
    case MAELYS_OCI_OK: return "ok";
    case MAELYS_OCI_ERR_ARGUMENT: return "argument";
    case MAELYS_OCI_ERR_MEMORY: return "memory";
    case MAELYS_OCI_ERR_IO: return "io";
    case MAELYS_OCI_ERR_UNSUPPORTED: return "unsupported";
    case MAELYS_OCI_ERR_PROTOCOL: return "protocol";
    case MAELYS_OCI_ERR_ACCESS: return "access";
    case MAELYS_OCI_ERR_NOT_FOUND: return "not-found";
    case MAELYS_OCI_ERR_STATE: return "state";
    }
    return "unknown";
}
