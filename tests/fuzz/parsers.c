/* SPDX-License-Identifier: MPL-2.0 */
/* libFuzzer entry, plus a deterministic mutation smoke gate for make check. */
#include "src/puller/internal.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const unsigned char *bytes, size_t size);
int LLVMFuzzerTestOneInput(const unsigned char *bytes, size_t size) {
    if (size > 65536u) return 0;
    char *text = malloc(size + 1u);
    if (!text) return 0;
    memcpy(text, bytes, size); text[size] = '\0';
    pull_reference_t reference;
    if (reference_parse(text, &reference) == 0) reference_clear(&reference);
    pull_challenge_t challenge;
    if (challenge_parse(text, &challenge) == 0) challenge_clear(&challenge);
    (void)basic_credential_valid(text);
    oci_manifest_t manifest;
    if (manifest_parse(bytes, size, &manifest) == 0) oci_manifest_clear(&manifest);
    oci_manifest_t configured = {0};
    (void)oci_config_parse(bytes, size, &configured, NULL);
    oci_manifest_clear(&configured);
    configured.layer_count = 1u;
    (void)oci_config_parse(bytes, size, &configured, NULL);
    oci_manifest_clear(&configured);
    oci_descriptor_t *items = NULL;
    size_t count = 0u;
    if (index_parse(bytes, size, &items, &count) == 0) free(items);
    maelys_json_document_t *parsed = oci_json_parse_object(bytes, size, OCI_JSON_TOKENS_MAX);
    if (parsed) {
        oci_descriptor_t descriptor;
        (void)oci_descriptor_parse(parsed, maelys_json_document_root(parsed), 0, &descriptor);
    }
    maelys_json_document_release(parsed);
    oci_document_t *document = oci_document_parse(bytes, size);
    if (document) {
        char *canonical = oci_document_dump(document);
        assert(canonical);
        oci_document_t *roundtrip = oci_document_parse((unsigned char *)canonical, strlen(canonical));
        assert(roundtrip);
        char *again = oci_document_dump(roundtrip);
        assert(again && strcmp(canonical, again) == 0);
        free(again); free(canonical);
        oci_document_release(roundtrip); oci_document_release(document);
    }
    free(text);
    return 0;
}
#ifdef OCI_FUZZ_SMOKE
int main(void) {
    static const char *const seeds[] = {
        "localhost:443/a.b__c--d/x@sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
        "[::1]:443/a@sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
        "Bearer realm=\"https://localhost/token\",service=\"fixture\",scope=\"repository:a:pull\"",
        "{\"size\":42,\"digest\":\"sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\",\"mediaType\":\"application/vnd.oci.image.config.v1+json\"}",
        "{\"schemaVersion\":2,\"config\":{\"size\":42,\"digest\":\"sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\",\"mediaType\":\"application/vnd.oci.image.config.v1+json\"},\"layers\":[]}",
        "{\"schemaVersion\":2,\"manifests\":[{\"size\":42,\"digest\":\"sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\",\"mediaType\":\"application/vnd.oci.image.manifest.v1+json\"}]}",
        "{\"b\":true,\"a\":[-9223372036854775808,9223372036854775807,\"caf\\u00e9\",null]}",
        "{\"x\":1,\"x\":2}", "{\"x\":\"\\u0000\"}", "Zml4dHVyZTpzZWNyZXQ="
    };
    for (size_t i = 0u; i < sizeof(seeds) / sizeof(seeds[0]); ++i) {
        size_t size = strlen(seeds[i]);
        unsigned char bytes[1024];
        assert(size < sizeof(bytes));
        memcpy(bytes, seeds[i], size);
        (void)LLVMFuzzerTestOneInput(bytes, size);
        for (size_t cut = 0u; cut < size; ++cut) {
            (void)LLVMFuzzerTestOneInput(bytes, cut);
            unsigned char original = bytes[cut];
            for (unsigned value = 0u; value < 256u; value += 17u) {
                bytes[cut] = (unsigned char)value;
                (void)LLVMFuzzerTestOneInput(bytes, size);
            }
            bytes[cut] = original;
        }
    }
    assert(basic_credential_valid("Zml4dHVyZTpzZWNyZXQ="));
    assert(!basic_credential_valid("Zml4dHVyZTpzZWNyZXR=")); /* noncanonical padding */
    return 0;
}
#endif
