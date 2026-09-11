/* SPDX-License-Identifier: MPL-2.0 */
/* Reject Mbed TLS releases affected by CVE-2025-27810. */
#include "src/puller/tls_version.h"

#include <mbedtls/version.h>

#if MBEDTLS_VERSION_NUMBER < 0x021c0a00
#error "Mbed TLS 2.28.10 or newer is required"
#elif MBEDTLS_VERSION_NUMBER >= 0x03000000 && \
      MBEDTLS_VERSION_NUMBER < 0x03060300
#error "Mbed TLS 3.6.3 or newer is required"
#endif

unsigned int oci_mbedtls_runtime_version(void) {
    return mbedtls_version_get_number();
}

int oci_mbedtls_version_secure(unsigned int version) {
    unsigned int major = version >> 24u;
    if (major == 2u) return version >= 0x021c0a00u;
    if (major == 3u) return version >= 0x03060300u;
    return major >= 4u;
}
