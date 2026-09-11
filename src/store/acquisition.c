/* SPDX-License-Identifier: MPL-2.0 */
/* A public, versioned acquisition lease binds the expected object digests
 * before their publication. Liveness uses the same kernel lock as execution
 * leases; neither credential state nor partial transfer offsets are stored. */
#include "src/store/core.h"
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int render(const oci_manifest_t *image, uint64_t duration_ms,
    char **out_bytes, size_t *out_size) {
    time_t now = time(NULL);
    uint64_t seconds = duration_ms / 1000u + 1u;
    if (now < 0 || seconds > (uint64_t)INT64_MAX ||
        (uint64_t)now > (uint64_t)INT64_MAX - seconds) return -1;
    char platform[OCI_PLATFORM_SIZE];
    if (oci_manifest_platform(image, platform) != 0) return -1;
    maelys_json_limits_t limits = {.maximum_bytes = MAELYS_OCI_LEASE_MAX_BYTES,
        .maximum_depth = 4u, .maximum_tokens = 1024u};
    maelys_json_writer_t *writer = NULL;
    int failed = maelys_json_writer_create(MAELYS_JSON_PROFILE_RFC8259, &limits,
        MAELYS_JSON_WRITER_FINAL_NEWLINE, &writer) != MAELYS_JSON_OK ||
        maelys_json_writer_object_begin(writer) != MAELYS_JSON_OK ||
        maelys_json_writer_key_cstr(writer, "schema") != MAELYS_JSON_OK ||
        maelys_json_writer_string_cstr(writer, OCI_ACQUISITION_LEASE_SCHEMA) != MAELYS_JSON_OK ||
        maelys_json_writer_key_cstr(writer, "liveness") != MAELYS_JSON_OK ||
        maelys_json_writer_string_cstr(writer, MAELYS_OCI_LEASE_LIVENESS) != MAELYS_JSON_OK ||
        maelys_json_writer_key_cstr(writer, "manifestDigest") != MAELYS_JSON_OK ||
        maelys_json_writer_string_cstr(writer, image->manifest.digest) != MAELYS_JSON_OK ||
        maelys_json_writer_key_cstr(writer, "platform") != MAELYS_JSON_OK ||
        maelys_json_writer_string_cstr(writer, platform) != MAELYS_JSON_OK ||
        maelys_json_writer_key_cstr(writer, "createdUnixSeconds") != MAELYS_JSON_OK ||
        maelys_json_writer_u64(writer, (uint64_t)now) != MAELYS_JSON_OK ||
        maelys_json_writer_key_cstr(writer, "expiresUnixSeconds") != MAELYS_JSON_OK ||
        maelys_json_writer_u64(writer, (uint64_t)now + seconds) != MAELYS_JSON_OK ||
        maelys_json_writer_key_cstr(writer, "blobs") != MAELYS_JSON_OK ||
        maelys_json_writer_array_begin(writer) != MAELYS_JSON_OK ||
        maelys_json_writer_string_cstr(writer, image->manifest.digest) != MAELYS_JSON_OK ||
        maelys_json_writer_string_cstr(writer, image->config.digest) != MAELYS_JSON_OK;
    for (size_t i = 0u; !failed && i < image->layer_count; ++i) {
        int duplicate = 0;
        for (size_t j = 0u; j < i; ++j)
            if (strcmp(image->layers[i].digest, image->layers[j].digest) == 0) duplicate = 1;
        if (!duplicate) failed = maelys_json_writer_string_cstr(writer,
            image->layers[i].digest) != MAELYS_JSON_OK;
    }
    if (!failed) failed = maelys_json_writer_array_end(writer) != MAELYS_JSON_OK ||
        maelys_json_writer_object_end(writer) != MAELYS_JSON_OK ||
        maelys_json_writer_finish(writer, out_bytes, out_size) != MAELYS_JSON_OK;
    maelys_json_writer_release(writer);
    return failed ? -1 : 0;
}

int oci_acquisition_lease_retire(oci_acquisition_lease_t *lease) {
    if (!lease) return 0;
    int result = 0;
    if (lease->lock && lease->path) {
        result = maelys_sys_file_unlink_same(lease->path, &lease->identity) ==
            MAELYS_SYS_OK ? 0 : -1;
        char *slash = strrchr(lease->path, '/');
        if (slash) {
            *slash = '\0';
            if (maelys_oci_store_fsync_directory(lease->path) != 0) result = -1;
        }
    }
    (void)maelys_sys_file_lock_release(&lease->lock);
    free(lease->path);
    memset(lease, 0, sizeof(*lease));
    return result;
}

int oci_acquisition_lease_create(const char *store, const oci_manifest_t *image,
    uint64_t duration_ms, oci_acquisition_lease_t *out) {
    memset(out, 0, sizeof(*out));
    char id[33], leases[PATH_MAX], path[PATH_MAX], temporary[PATH_MAX];
    char *bytes = NULL;
    size_t size = 0u;
    if (oci_store_random_id(id) != 0 ||
        maelys_oci_store_ensure_private_directory(store, "leases", leases, sizeof(leases)) != 0 ||
        oci_snprintf(path, sizeof(path), "%s/%s.json", leases, id) < 0 ||
        render(image, duration_ms, &bytes, &size) != 0) return -1;
    int fd = oci_store_temporary_file(store, temporary);
    if (fd < 0) { free(bytes); return -1; }
    int result = oci_write_all(fd, bytes, size) == 0 && fchmod(fd, 0400) == 0 &&
        maelys_sys_file_sync(fd) == MAELYS_SYS_OK ? 0 : -1;
    free(bytes);
    if (maelys_sys_fd_close(&fd) != MAELYS_SYS_OK) result = -1;
    if (!result) result = maelys_oci_store_lease_probe(temporary, &out->lock);
    maelys_sys_file_expectations_t expectations;
    maelys_oci_store_lease_expectations(&expectations);
    if (!result && maelys_sys_file_verify(maelys_sys_file_lock_fd(out->lock),
            &expectations, &out->identity, NULL) != MAELYS_SYS_OK) result = -1;
    out->path = strdup(path);
    if (!out->path) result = -1;
    if (!result) result = maelys_oci_store_publish_file_noreplace(temporary, path);
    if (!result) result = maelys_oci_store_fsync_directory(leases);
    if (result) {
        (void)unlink(temporary);
        (void)oci_acquisition_lease_retire(out);
    }
    return result ? -1 : 0;
}
