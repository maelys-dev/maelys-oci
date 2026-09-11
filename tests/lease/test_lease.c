/* SPDX-License-Identifier: MPL-2.0 */
/*
 * Library-level proof of the lease protocol, driven by the shell suite:
 * resolve an imported artifact, print the lease path, hold the lease until
 * a line arrives on stdin, revalidate, release, and prove the lease is gone.
 * Usage: test_lease STORE IMAGE_DIGEST PLATFORM
 */
#include <maelys/oci.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fail(const char *what, char *error) {
    fprintf(stderr, "test_lease: %s%s%s\n", what, error ? ": " : "",
        error ? error : "");
    maelys_oci_error_free(error);
    return 1;
}

static int argument_contract(void) {
    maelys_oci_artifact_t *artifact = (maelys_oci_artifact_t *)1;
    char *error = NULL;
    if (maelys_oci_artifact_resolve("sha256:nope", "linux/arm64", &artifact,
            &error) != MAELYS_OCI_ERR_ARGUMENT || artifact || error)
        return fail("an invalid digest must be an argument error", error);
    if (maelys_oci_artifact_resolve(
            "sha256:0000000000000000000000000000000000000000000000000000000000000000",
            "windows/amd64", &artifact, &error) != MAELYS_OCI_ERR_ARGUMENT ||
        artifact || error)
        return fail("an unsupported platform must be an argument error", error);
    if (maelys_oci_artifact_resolve("sha256:nope", "linux/arm64", NULL,
            NULL) != MAELYS_OCI_ERR_ARGUMENT)
        return fail("a NULL handle must be an argument error", NULL);
    if (maelys_oci_artifact_revalidate(NULL, &error) != MAELYS_OCI_ERR_ARGUMENT)
        return fail("revalidating NULL must be an argument error", error);
    if (maelys_oci_artifact_root_path(NULL) ||
        maelys_oci_artifact_lease_path(NULL) ||
        maelys_oci_artifact_root_identity(NULL, NULL) != -1)
        return fail("accessors of NULL must be NULL or -1", NULL);
    maelys_oci_artifact_t *none = NULL;
    maelys_oci_artifact_release(&none);
    maelys_oci_artifact_release(NULL);
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: test_lease STORE IMAGE_DIGEST PLATFORM\n");
        return 2;
    }
    if (setenv("MAELYS_OCI_STORE", argv[1], 1) != 0) return 2;
    if (argument_contract() != 0) return 1;
    char *error = NULL;
    maelys_oci_artifact_t *artifact = NULL;
    maelys_oci_result_t result =
        maelys_oci_artifact_resolve(argv[2], argv[3], &artifact, &error);
    if (result != MAELYS_OCI_OK || !artifact) return fail("resolve", error);
    const char *lease = maelys_oci_artifact_lease_path(artifact);
    maelys_oci_file_identity_t root;
    struct stat root_status;
    if (!lease || strcmp(maelys_oci_artifact_image_digest(artifact), argv[2]) != 0 ||
        strcmp(maelys_oci_artifact_platform(artifact), argv[3]) != 0 ||
        strlen(maelys_oci_artifact_root_digest(artifact)) != 64u ||
        strlen(maelys_oci_artifact_rootfs_tar_digest(artifact)) != 64u ||
        strncmp(maelys_oci_artifact_config_digest(artifact), "sha256:", 7u) != 0 ||
        maelys_oci_artifact_root_identity(artifact, &root) != 0 ||
        stat(maelys_oci_artifact_root_path(artifact), &root_status) != 0 ||
        root_status.st_dev != root.device || root_status.st_ino != root.inode ||
        root_status.st_mode != root.mode ||
        (uint64_t)root_status.st_size != root.size ||
        stat(maelys_oci_artifact_rootfs_tar_path(artifact), &root_status) != 0)
        return fail("accessors disagree with the resolved artifact", NULL);
    /* Two resolutions are two leases. */
    maelys_oci_artifact_t *second = NULL;
    if (maelys_oci_artifact_resolve(argv[2], argv[3], &second, &error) !=
            MAELYS_OCI_OK ||
        strcmp(maelys_oci_artifact_lease_path(second), lease) == 0)
        return fail("second resolution", error);
    maelys_oci_artifact_release(&second);
    struct stat status;
    if (second || lstat(lease, &status) != 0)
        return fail("releasing one lease must not touch the other", NULL);
    printf("%s\n", lease);
    fflush(stdout);
    char line[64];
    if (!fgets(line, sizeof(line), stdin)) return fail("no release order", NULL);
    if (maelys_oci_artifact_revalidate(artifact, &error) != MAELYS_OCI_OK)
        return fail("revalidate while held", error);
    /* The lease is retired by identity: a file that took its place stays. */
    char *held = strdup(lease);
    if (!held) return 2;
    maelys_oci_artifact_release(&artifact);
    int gone = artifact == NULL && lstat(held, &status) != 0 && errno == ENOENT;
    free(held);
    if (!gone) return fail("lease must be gone after release", NULL);
    puts("released");
    return 0;
}
