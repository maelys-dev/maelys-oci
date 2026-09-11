/* SPDX-License-Identifier: MPL-2.0 */
/* Build and runtime gate for CVE-2025-27810. */
#include "src/puller/tls_version.h"

#include <stdio.h>

int main(void) {
    unsigned int runtime = oci_mbedtls_runtime_version();
    if (!oci_mbedtls_version_secure(runtime)) {
        fprintf(stderr,
            "Mbed TLS runtime %u.%u.%u is vulnerable to CVE-2025-27810; "
            "require 2.28.10+, 3.6.3+ or 4+\n",
            runtime >> 24u, (runtime >> 16u) & 0xffu,
            (runtime >> 8u) & 0xffu);
        return 1;
    }
    return 0;
}
