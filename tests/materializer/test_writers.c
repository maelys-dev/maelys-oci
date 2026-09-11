/* SPDX-License-Identifier: MPL-2.0 */
/* Writer failure isolation and concurrent use of independent graphs. */
#include "src/materializer/internal.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { WORKERS = 8, ITERATIONS = 16 };
typedef struct worker {
    const char *layer;
    uint64_t layer_size;
    const char *media_type;
    const char *diff_id;
    char directory[PATH_MAX];
    char tar[PATH_MAX];
    char digest[OCI_DIGEST_HEX_SIZE];
} worker_t;

static void make_layer(const char *path, int filter) {
    struct archive *writer = archive_write_new();
    assert(writer);
    assert(archive_write_set_format_ustar(writer) == ARCHIVE_OK);
    assert(archive_write_add_filter(writer, filter) == ARCHIVE_OK);
    assert(archive_write_set_bytes_in_last_block(writer, 1) == ARCHIVE_OK);
    assert(archive_write_open_filename(writer, path) == ARCHIVE_OK);
    struct archive_entry *entry = archive_entry_new();
    assert(entry);
    archive_entry_set_pathname(entry, "hello");
    archive_entry_set_filetype(entry, AE_IFREG);
    archive_entry_set_perm(entry, 0644);
    archive_entry_set_size(entry, 5);
    assert(archive_write_header(writer, entry) == ARCHIVE_OK);
    assert(archive_write_data(writer, "hello", 5u) == 5);
    archive_entry_free(entry);
    assert(archive_write_close(writer) == ARCHIVE_OK);
    assert(archive_write_free(writer) == ARCHIVE_OK);
}

static void *materialize(void *opaque) {
    worker_t *worker = opaque;
    logical_graph_t graph = {0};
    for (size_t i = 0u; i < ITERATIONS; ++i) {
        oci_error_t error = OCI_ERROR_INIT;
        if (graph_apply_layer(&graph, worker->layer, worker->directory,
                              worker->layer_size, worker->media_type,
                              worker->diff_id, &error) != 0) {
            fprintf(stderr, "layer %s iteration %zu in %s failed: %s\n",
                    worker->layer, i, worker->directory, oci_error_message(&error));
            abort();
        }
    }
    int index = graph_find(&graph, "hello");
    assert(index >= 0);
    const graph_entry_t *entry = &graph.entries[index];
    assert(entry->size == 5u);
    int fd = open(entry->content_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    char bytes[5];
    assert(fd >= 0 && read(fd, bytes, sizeof(bytes)) == 5);
    assert(memcmp(bytes, "hello", sizeof(bytes)) == 0);
    assert(close(fd) == 0);
    assert(graph_compact(&graph) == 0);
    assert(graph.next_inode == 1u);
    assert(graph.entries[0].logical_inode == 1u);
    uint64_t size = 0u;
    oci_error_t error = OCI_ERROR_INIT;
    assert(graph_write_tar(&graph, worker->tar, &size, &error) == 0);
    struct stat status;
    assert(maelys_oci_store_hash_immutable(worker->tar, 0400,
                                         worker->digest, &status) == 0);
    assert(size == (uint64_t)status.st_size);
    oci_error_clear(&error);
    graph_clear(&graph);
    return NULL;
}

static void existing_destination(const char *path, const char *target) {
    const char sentinel[] = "preexisting file must survive";
    assert(oci_write_file_exclusive(target, sentinel, sizeof(sentinel), 0400) == 0);
    if (strcmp(path, target) != 0) assert(symlink(target, path) == 0);
    struct stat before, after;
    assert(lstat(path, &before) == 0);
    logical_graph_t graph = {0};
    oci_error_t error = OCI_ERROR_INIT;
    uint64_t size = 0u;
    assert(graph_write_ext4(&graph, path, &size, &error) != 0);
    oci_error_clear(&error);
    assert(graph_write_tar(&graph, path, &size, &error) != 0);
    oci_error_clear(&error);
    assert(lstat(path, &after) == 0);
    assert(before.st_dev == after.st_dev && before.st_ino == after.st_ino &&
           before.st_mode == after.st_mode && before.st_size == after.st_size);
    int fd = open(target, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    char bytes[sizeof(sentinel)];
    assert(fd >= 0 && read(fd, bytes, sizeof(bytes)) == (ssize_t)sizeof(bytes));
    assert(memcmp(bytes, sentinel, sizeof(bytes)) == 0);
    assert(close(fd) == 0);
}

static void bounded_copy(const char *base) {
    char input_path[PATH_MAX], output_path[PATH_MAX];
    assert(oci_snprintf(input_path, sizeof(input_path), "%s/input", base) > 0);
    assert(oci_snprintf(output_path, sizeof(output_path), "%s/output", base) > 0);
    unsigned char bytes[65538] = {0};
    assert(oci_write_file_exclusive(input_path, bytes, sizeof(bytes), 0400) == 0);
    const uint64_t limits[] = {0u, 1u, 65535u, 65536u, 65537u, sizeof(bytes)};
    for (size_t i = 0u; i < sizeof(limits) / sizeof(limits[0]); ++i) {
        int input = open(input_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        int output = open(output_path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        assert(input >= 0 && output >= 0);
        uint64_t size = 0u;
        char digest[OCI_DIGEST_HEX_SIZE], expected[OCI_DIGEST_HEX_SIZE];
        int result = oci_copy_fd_hashed(input, output, limits[i], &size, digest);
        struct stat status;
        assert(fstat(output, &status) == 0 && (uint64_t)status.st_size <= limits[i]);
        if (limits[i] < sizeof(bytes)) assert(result != 0 && errno == EFBIG);
        else {
            maelys_oci_sha256_hex(bytes, sizeof(bytes), expected);
            assert(result == 0 && size == sizeof(bytes) && strcmp(digest, expected) == 0);
        }
        assert(close(input) == 0 && close(output) == 0);
        assert(unlink(output_path) == 0);
    }
}

static void staging_limits(void) {
    oci_descriptor_t layers[2] = {{.size = OCI_STAGING_MAX - 3u}, {.size = 1u}};
    oci_manifest_t image = {
        .manifest = {.size = 1u}, .config = {.size = 1u},
        .layers = layers, .layer_count = 2u
    };
    assert(oci_manifest_staging_valid(&image));
    layers[1].size = 2u;
    assert(!oci_manifest_staging_valid(&image));
    image.manifest.size = UINT64_MAX;
    assert(!oci_manifest_staging_valid(&image));
    image.manifest.size = 1u;
    image.config.size = UINT64_MAX;
    assert(!oci_manifest_staging_valid(&image));
}

int main(int argc, char **argv) {
    assert(argc == 2);
    staging_limits();
    char base[PATH_MAX], layers[3][PATH_MAX], path[PATH_MAX], target[PATH_MAX];
    assert(oci_snprintf(base, sizeof(base), "%s/writers.XXXXXX", argv[1]) >= 0);
    assert(mkdtemp(base));
    bounded_copy(base);
    assert(oci_snprintf(path, sizeof(path), "%s/existing", base) >= 0);
    existing_destination(path, path);
    assert(oci_snprintf(path, sizeof(path), "%s/link", base) >= 0);
    assert(oci_snprintf(target, sizeof(target), "%s/target", base) >= 0);
    existing_destination(path, target);
    struct stat status[3];
    const int filters[] = {ARCHIVE_FILTER_NONE, ARCHIVE_FILTER_GZIP, ARCHIVE_FILTER_ZSTD};
    const char *const media[] = {
        "application/vnd.oci.image.layer.v1.tar",
        "application/vnd.oci.image.layer.v1.tar+gzip",
        "application/vnd.oci.image.layer.v1.tar+zstd"
    };
    for (size_t i = 0u; i < 3u; ++i) {
        assert(oci_snprintf(layers[i], PATH_MAX, "%s/layer-%zu", base, i) >= 0);
        make_layer(layers[i], filters[i]);
        assert(stat(layers[i], &status[i]) == 0);
    }
    char hex[OCI_DIGEST_HEX_SIZE], diff_id[OCI_DIGEST_SIZE];
    assert(maelys_oci_sha256_file(layers[0], hex, NULL) == MAELYS_OCI_OK);
    assert(oci_snprintf(diff_id, sizeof(diff_id), "sha256:%s", hex) > 0);
    pthread_t threads[WORKERS];
    worker_t workers[WORKERS] = {0};
    for (size_t codec = 0u; codec < 3u; ++codec) {
        for (size_t i = 0u; i < WORKERS; ++i) {
            workers[i].layer = layers[codec];
            workers[i].media_type = media[codec];
            workers[i].diff_id = diff_id;
            workers[i].layer_size = (uint64_t)status[codec].st_size;
            assert(oci_snprintf(workers[i].directory, PATH_MAX, "%s/content-%zu-%zu", base, codec, i) >= 0);
            assert(oci_snprintf(workers[i].tar, PATH_MAX, "%s/root-%zu-%zu.tar", base, codec, i) >= 0);
            assert(mkdir(workers[i].directory, 0700) == 0);
            assert(pthread_create(&threads[i], NULL, materialize, &workers[i]) == 0);
        }
        for (size_t i = 0u; i < WORKERS; ++i) assert(pthread_join(threads[i], NULL) == 0);
        for (size_t i = 1u; i < WORKERS; ++i)
            assert(strcmp(workers[0].digest, workers[i].digest) == 0);
    }
    assert(remove_filesystem_tree(base) == 0);
    return 0;
}
