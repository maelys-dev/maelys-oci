/* SPDX-License-Identifier: MPL-2.0 */
/*
 * Layer application (tar parsing with pax and GNU long names, whiteouts),
 * the deterministic portable tar writer and the Linux-only unpacker.
 */
#include "src/materializer/internal.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct pax_override {
    char *path;
    char *linkpath;
    int uid_set;
    int gid_set;
    int mtime_set;
    int size_set;
    uint64_t uid;
    uint64_t gid;
    int64_t mtime;
    uint64_t size;
} pax_override_t;

static void pax_clear(pax_override_t *pax) {
    free(pax->path);
    free(pax->linkpath);
    memset(pax, 0, sizeof(*pax));
}

int parse_decimal_u64(const char *value, size_t size, uint64_t *out) {
    if (!size) return -1;
    uint64_t result = 0u;
    for (size_t i = 0u; i < size; ++i) {
        if (value[i] < '0' || value[i] > '9' ||
            result > (UINT64_MAX - (uint64_t)(value[i] - '0')) / 10u)
            return -1;
        result = result * 10u + (uint64_t)(value[i] - '0');
    }
    *out = result;
    return 0;
}

static int pax_set_string(char **slot, const char *value, size_t size) {
    if (!size || memchr(value, '\0', size)) return -1;
    char *copy = malloc(size + 1u);
    if (!copy) return -1;
    memcpy(copy, value, size);
    copy[size] = '\0';
    free(*slot);
    *slot = copy;
    return 0;
}

static int parse_pax(const unsigned char *bytes, size_t size, pax_override_t *pax) {
    size_t offset = 0u;
    while (offset < size) {
        size_t cursor = offset;
        while (cursor < size && bytes[cursor] >= '0' && bytes[cursor] <= '9')
            ++cursor;
        if (cursor == offset || cursor >= size || bytes[cursor] != ' ') return -1;
        uint64_t record_size = 0u;
        if (parse_decimal_u64((const char *)bytes + offset, cursor - offset,
                              &record_size) != 0 ||
            record_size < cursor - offset + 3u || record_size > size - offset)
            return -1;
        size_t end = offset + (size_t)record_size;
        if (bytes[end - 1u] != '\n') return -1;
        const unsigned char *key = bytes + cursor + 1u;
        const unsigned char *equals = memchr(key, '=', end - 1u - (cursor + 1u));
        if (!equals || equals == key) return -1;
        size_t key_size = (size_t)(equals - key);
        const char *value = (const char *)equals + 1u;
        size_t value_size = end - 1u - (size_t)((const unsigned char *)value - bytes);
        uint64_t number = 0u;
        if (key_size == 4u && memcmp(key, "path", 4u) == 0) {
            if (pax_set_string(&pax->path, value, value_size) != 0) return -1;
        } else if (key_size == 8u && memcmp(key, "linkpath", 8u) == 0) {
            if (pax_set_string(&pax->linkpath, value, value_size) != 0) return -1;
        } else if (key_size == 3u && memcmp(key, "uid", 3u) == 0) {
            if (parse_decimal_u64(value, value_size, &number) != 0) return -1;
            pax->uid = number;
            pax->uid_set = 1;
        } else if (key_size == 3u && memcmp(key, "gid", 3u) == 0) {
            if (parse_decimal_u64(value, value_size, &number) != 0) return -1;
            pax->gid = number;
            pax->gid_set = 1;
        } else if (key_size == 4u && memcmp(key, "size", 4u) == 0) {
            if (parse_decimal_u64(value, value_size, &number) != 0) return -1;
            pax->size = number;
            pax->size_set = 1;
        } else if (key_size == 5u && memcmp(key, "mtime", 5u) == 0) {
            size_t integer = 0u;
            while (integer < value_size && value[integer] != '.') ++integer;
            if (parse_decimal_u64(value, integer, &number) != 0 || number > INT64_MAX)
                return -1;
            pax->mtime = (int64_t)number;
            pax->mtime_set = 1;
        } else if (key_size == 10u && memcmp(key, "hdrcharset", 10u) == 0) {
            if (!((value_size == 6u && memcmp(value, "BINARY", 6u) == 0) ||
                  (value_size == 5u && memcmp(value, "UTF-8", 5u) == 0)))
                return -1;
        } else {
            /* Unknown metadata may change filesystem semantics (ACLs are the
             * canonical example). Preserve it fully or reject the layer. */
            return -1;
        }
        offset = end;
    }
    return offset == size ? 0 : -1;
}

static int graph_apply_entry(
    logical_graph_t *graph, const char *raw_path, char type,
    uint32_t mode, uint32_t uid, uint32_t gid, int64_t mtime,
    const char *raw_link, int tar_fd, uint64_t data_offset,
    uint64_t data_size, const char *content_directory, int whiteouts_only) {
    char *path = NULL;
    if (normalize_layer_path(raw_path, &path) != 0) return -1;
    if (!path[0]) {
        free(path);
        return type == '5' ? 0 : -1;
    }
    const char *base = path_basename(path);
    /* Only the last segment may name a whiteout marker, which both passes
     * consume. An interior one is never consumed: it would be materialized
     * as an ordinary directory whose name a consumer of the published
     * rootfs.tar reads back as a deletion, so the layer is refused. */
    if (path_has_whiteout_segment(path, (size_t)(base - path))) {
        free(path);
        return -1;
    }
    int whiteout = strncmp(base, ".wh.", 4u) == 0;
    if (whiteout && ((type != '0' && type != '\0') || data_size != 0u ||
                     (raw_link && raw_link[0]))) {
        free(path);
        return -1;
    }
    if (whiteout != whiteouts_only) {
        free(path);
        return 0;
    }
    if (strcmp(base, ".wh..wh..opq") == 0) {
        char *parent = path_parent_copy(path);
        if (!parent) {
            free(path);
            return -1;
        }
        graph_remove_children(graph, parent);
        free(parent);
        free(path);
        return 0;
    }
    if (strncmp(base, ".wh.", 4u) == 0) {
        char *parent = path_parent_copy(path);
        const char *target_name = base + 4u;
        if (!parent || !oci_relative_path_valid(target_name) ||
            strncmp(target_name, ".wh.", 4u) == 0) {
            free(parent);
            free(path);
            return -1;
        }
        size_t needed = strlen(parent) + strlen(target_name) + 2u;
        char *target = malloc(needed);
        if (!target) {
            free(parent);
            free(path);
            return -1;
        }
        (void)oci_snprintf(target, needed, "%s%s%s", parent,
                       parent[0] ? "/" : "", target_name);
        graph_remove_tree(graph, target);
        free(target);
        free(parent);
        free(path);
        return 0;
    }
    if (graph_ensure_parent(graph, path) != 0) {
        free(path);
        return -1;
    }
    mode &= 07777u;
    if (mode & 06000u) {
        mode &= ~06000u;
        ++graph->cleared_privilege_bits;
    }
    graph_entry_t entry = {
        .path = path, .mode = mode, .uid = uid, .gid = gid, .mtime = mtime
    };
    int result = 0;
    if (type == '5') {
        entry.type = GRAPH_DIRECTORY;
        result = graph_allocate_inode(graph, &entry.logical_inode) == 0
            ? graph_put(graph, &entry) : -1;
    } else if (type == '0' || type == '\0') {
        entry.type = GRAPH_REGULAR;
        entry.size = data_size;
        if (graph_allocate_inode(graph, &entry.logical_inode) != 0 ||
            write_fd_content(tar_fd, data_offset, data_size,
                content_directory, &entry.content_path) != 0)
            result = -1;
        else
            result = graph_put(graph, &entry);
    } else if (type == '2') {
        if (!raw_link || strlen(raw_link) > 4096u) result = -1;
        else {
            entry.type = GRAPH_SYMLINK;
            if (graph_allocate_inode(graph, &entry.logical_inode) != 0)
                result = -1;
            else {
                entry.symlink_target = strdup(raw_link);
                result = entry.symlink_target ? graph_put(graph, &entry) : -1;
            }
        }
    } else if (type == '1') {
        char *target = NULL;
        if (!raw_link || normalize_layer_path(raw_link, &target) != 0) result = -1;
        int target_index = result == 0 ? graph_find(graph, target) : -1;
        if (target_index < 0 || graph->entries[target_index].type != GRAPH_REGULAR)
            result = -1;
        else {
            graph_entry_t *source = &graph->entries[target_index];
            entry.type = GRAPH_REGULAR;
            entry.logical_inode = source->logical_inode;
            entry.size = source->size;
            entry.content_path = strdup(source->content_path);
            result = entry.content_path ? graph_put(graph, &entry) : -1;
        }
        free(target);
    } else {
        result = -1;
    }
    if (result != 0) graph_entry_clear(&entry);
    return result;
}

/* Decompresses one layer blob into a private tar and verifies its DiffID.
 * What the content gets wrong (encoding, expansion, digest) is reported as
 * a protocol failure; what the host gets wrong as an I/O failure. */
static int decompress_layer(
    const char *layer_path, const char *content_directory,
    uint64_t compressed_size, const char *media_type, const char *diff_id,
    char output[PATH_MAX], uint64_t *out_size, oci_error_t *error) {
    if (!oci_layer_media_type_supported(media_type) || !oci_digest_valid(diff_id)) {
        oci_error_report(error, OCI_ERROR_PROTOCOL,
            "layer %s has an unsupported media type or DiffID", diff_id);
        return -1;
    }
    int filter = strstr(media_type, "gzip") ? ARCHIVE_FILTER_GZIP :
        strstr(media_type, "zstd") ? ARCHIVE_FILTER_ZSTD : ARCHIVE_FILTER_NONE;
    struct archive *archive = archive_read_new();
    if (!archive) {
        oci_error_report(error, OCI_ERROR_MEMORY, "out of memory");
        return -1;
    }
    if (oci_archive_support_filters(archive) != 0 ||
        archive_read_support_format_raw(archive) != ARCHIVE_OK ||
        archive_read_open_filename(archive, layer_path, 1024u * 1024u) != ARCHIVE_OK) {
        archive_read_free(archive);
        oci_error_report(error, OCI_ERROR_IO,
            "cannot open the staged layer for DiffID %s", diff_id);
        return -1;
    }
    struct archive_entry *entry = NULL;
    if (archive_read_next_header(archive, &entry) != ARCHIVE_OK ||
        archive_filter_code(archive, 0) != filter ||
        archive_filter_count(archive) != (filter == ARCHIVE_FILTER_NONE ? 1 : 2) ||
        archive_filter_code(archive, -1) != ARCHIVE_FILTER_NONE) {
        archive_read_close(archive);
        archive_read_free(archive);
        oci_error_report(error, OCI_ERROR_PROTOCOL,
            "layer %s is not encoded as its media type %s declares",
            diff_id, media_type);
        return -1;
    }
    char id[33];
    int fd = oci_store_random_id(id) == 0 &&
        oci_snprintf(output, PATH_MAX, "%s/.tar.%s", content_directory, id) > 0
        ? open(output, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600)
        : -1;
    if (fd < 0) {
        archive_read_close(archive);
        archive_read_free(archive);
        oci_error_report(error, OCI_ERROR_IO,
            "cannot create the private tar of layer %s", diff_id);
        return -1;
    }
    uint64_t expansion_limit = compressed_size >
        (OCI_MAX_UNCOMPRESSED - UINT64_C(67108864)) / OCI_MAX_EXPANSION_RATIO
        ? OCI_MAX_UNCOMPRESSED
        : compressed_size * OCI_MAX_EXPANSION_RATIO + UINT64_C(67108864);
    uint64_t total = 0u;
    maelys_oci_sha256_context_t hash;
    maelys_oci_sha256_init(&hash);
    int result = 0;
    unsigned char buffer[64u * 1024u];
    while (result == 0) {
        la_ssize_t amount = archive_read_data(archive, buffer, sizeof(buffer));
        if (amount < 0) {
            oci_error_report(error, OCI_ERROR_PROTOCOL,
                "layer %s is a corrupt %s stream", diff_id, media_type);
            result = -1;
        } else if (amount == 0) {
            break;
        } else if ((uint64_t)amount > expansion_limit - total) {
            oci_error_report(error, OCI_ERROR_PROTOCOL,
                "layer %s expands beyond the accepted ratio", diff_id);
            result = -1;
        } else if (oci_write_all(fd, buffer, (size_t)amount) != 0) {
            oci_error_report(error, OCI_ERROR_IO,
                "cannot write the private tar of layer %s", diff_id);
            result = -1;
        } else {
            maelys_oci_sha256_update(&hash, buffer, (size_t)amount);
            total += (uint64_t)amount;
        }
    }
    char digest[OCI_DIGEST_HEX_SIZE];
    maelys_oci_sha256_finish(&hash, digest);
    if (result == 0 && strcmp(digest, diff_id + OCI_DIGEST_PREFIX_SIZE) != 0) {
        oci_error_report(error, OCI_ERROR_PROTOCOL,
            "layer content hashes to sha256:%s, not its DiffID %s",
            digest, diff_id);
        result = -1;
    }
    if (result == 0 && fsync(fd) != 0) {
        oci_error_report(error, OCI_ERROR_IO,
            "cannot make the private tar of layer %s durable", diff_id);
        result = -1;
    }
    if (maelys_sys_fd_close(&fd) != MAELYS_SYS_OK) result = -1;
    if (archive_read_close(archive) != ARCHIVE_OK) result = -1;
    if (archive_read_free(archive) != ARCHIVE_OK) result = -1;
    if (result != 0) (void)unlink(output);
    *out_size = total;
    return result;
}

static int apply_tar_pass(logical_graph_t *graph, int fd, uint64_t tar_size,
    const char *content_directory, int whiteouts_only) {
    uint64_t offset = 0u;
    unsigned int zero_headers = 0u;
    int result = 0;
    pax_override_t pax = {0};
    char *long_path = NULL;
    char *long_link = NULL;
    while (result == 0 && offset + 512u <= tar_size) {
        unsigned char header[512];
        if (read_exact_at(fd, header, sizeof(header), offset) != 0) {
            result = -1;
            break;
        }
        offset += 512u;
        int all_zero = 1;
        for (size_t i = 0u; i < sizeof(header); ++i)
            if (header[i] != 0u) { all_zero = 0; break; }
        if (all_zero) {
            if (++zero_headers == 2u) break;
            continue;
        }
        if (zero_headers || !tar_checksum_valid(header)) {
            result = -1;
            break;
        }
        uint64_t size = 0u, uid = 0u, gid = 0u, mode = 0u, mtime = 0u;
        if (parse_octal(header + 124u, 12u, &size) != 0 ||
            parse_octal(header + 108u, 8u, &uid) != 0 ||
            parse_octal(header + 116u, 8u, &gid) != 0 ||
            parse_octal(header + 100u, 8u, &mode) != 0 ||
            parse_octal(header + 136u, 12u, &mtime) != 0 ||
            size > tar_size - offset) {
            result = -1;
            break;
        }
        char type = (char)header[156u];
        uint64_t padded = (size + 511u) & ~UINT64_C(511);
        if (padded > tar_size - offset) {
            result = -1;
            break;
        }
        if (type == 'x' || type == 'L' || type == 'K') {
            if (size > OCI_JSON_MAX) { result = -1; break; }
            unsigned char *special = malloc((size_t)size + 1u);
            if (!special || read_exact_at(fd, special, (size_t)size, offset) != 0) {
                free(special);
                result = -1;
                break;
            }
            special[size] = '\0';
            if (type == 'x') result = parse_pax(special, (size_t)size, &pax);
            else {
                while (size && (special[size - 1u] == '\0' || special[size - 1u] == '\n'))
                    special[--size] = '\0';
                result = pax_set_string(type == 'L' ? &long_path : &long_link,
                                        (const char *)special, (size_t)size);
            }
            free(special);
            offset += padded;
            continue;
        }
        if (type == 'g' || type == 'S') { result = -1; break; }
        char *header_path = tar_header_path(header);
        char *header_link = tar_field_string(header + 157u, 100u);
        const char *path = pax.path ? pax.path : long_path ? long_path : header_path;
        const char *link = pax.linkpath ? pax.linkpath : long_link ? long_link : header_link;
        if (!path || uid > UINT32_MAX || gid > UINT32_MAX || mtime > INT64_MAX ||
            (pax.uid_set && pax.uid > UINT32_MAX) ||
            (pax.gid_set && pax.gid > UINT32_MAX)) result = -1;
        uint64_t effective_size = pax.size_set ? pax.size : size;
        if (effective_size != size) result = -1;
        if (result == 0)
            result = graph_apply_entry(graph, path, type,
                (uint32_t)mode,
                (uint32_t)(pax.uid_set ? pax.uid : uid),
                (uint32_t)(pax.gid_set ? pax.gid : gid),
                pax.mtime_set ? pax.mtime : (int64_t)mtime,
                link, fd, offset, size, content_directory, whiteouts_only);
        free(header_path);
        free(header_link);
        free(long_path);
        free(long_link);
        long_path = NULL;
        long_link = NULL;
        pax_clear(&pax);
        offset += padded;
    }
    if (zero_headers != 2u || offset > tar_size || long_path || long_link ||
        pax.path || pax.linkpath) result = -1;
    pax_clear(&pax);
    free(long_path);
    free(long_link);
    return result;
}

/* The host's own failures during a tar pass leave one of these in errno;
 * anything else a pass refuses is the layer's content. */
static int errno_names_host_failure(int saved) {
    return saved == ENOSPC || saved == EDQUOT || saved == EIO ||
        saved == EROFS || saved == EMFILE || saved == ENFILE || saved == ENOMEM;
}

int graph_apply_layer(
    logical_graph_t *graph, const char *layer_path,
    const char *content_directory, uint64_t compressed_size,
    const char *media_type, const char *diff_id, oci_error_t *error) {
    char tar_path[PATH_MAX];
    uint64_t tar_size = 0u;
    if (decompress_layer(layer_path, content_directory, compressed_size,
            media_type, diff_id, tar_path, &tar_size, error) != 0) return -1;
    int fd = open(tar_path, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        oci_error_report(error, OCI_ERROR_IO,
            "cannot reopen the private tar of layer %s", diff_id);
        (void)unlink(tar_path);
        return -1;
    }
    /* OCI whiteouts only remove inherited entries. Both passes use the same
     * verified private tar, so PAX/GNU names and hardlink ordering agree. */
    errno = 0;
    int result =
        apply_tar_pass(graph, fd, tar_size, content_directory, 1) == 0 &&
        apply_tar_pass(graph, fd, tar_size, content_directory, 0) == 0 ? 0 : -1;
    int saved = errno;
    if (result != 0)
        oci_error_report(error,
            errno_names_host_failure(saved) ? OCI_ERROR_IO : OCI_ERROR_PROTOCOL,
            errno_names_host_failure(saved)
                ? "cannot apply layer %s to the logical graph: %s"
                : "layer %s is not a valid OCI layer for this materializer: %s",
            diff_id, errno_names_host_failure(saved) ? strerror(saved)
                : "an entry, whiteout marker or link is refused");
    (void)maelys_sys_fd_close(&fd);
    (void)unlink(tar_path);
    return result;
}

static size_t path_depth(const char *path) {
    size_t depth = path[0] ? 1u : 0u;
    for (const char *cursor = path; *cursor; ++cursor)
        if (*cursor == '/') ++depth;
    return depth;
}

int compare_graph_entries(const void *left, const void *right) {
    const graph_entry_t *const *a = left;
    const graph_entry_t *const *b = right;
    size_t a_depth = path_depth((*a)->path);
    size_t b_depth = path_depth((*b)->path);
    if (a_depth < b_depth) return -1;
    if (a_depth > b_depth) return 1;
    return strcmp((*a)->path, (*b)->path);
}

static int archive_write_regular_content(
    struct archive *writer, const graph_entry_t *entry) {
    int descriptor = open(
        entry->content_path, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    struct stat status;
    if (descriptor < 0 || fstat(descriptor, &status) != 0 ||
        !S_ISREG(status.st_mode) || (uint64_t)status.st_size != entry->size) {
        if (descriptor >= 0) (void)maelys_sys_fd_close(&descriptor);
        return -1;
    }
    unsigned char buffer[64u * 1024u];
    uint64_t written = 0u;
    int result = 0;
    while (written < entry->size) {
        size_t wanted = entry->size - written < sizeof(buffer)
            ? (size_t)(entry->size - written) : sizeof(buffer);
        ssize_t amount = read(descriptor, buffer, wanted);
        if (amount < 0 && errno == EINTR) continue;
        if (amount <= 0) {
            result = -1;
            break;
        }
        la_ssize_t archived = archive_write_data(
            writer, buffer, (size_t)amount);
        if (archived != amount) {
            result = -1;
            break;
        }
        written += (uint64_t)amount;
    }
    if (maelys_sys_fd_close(&descriptor) != MAELYS_SYS_OK) result = -1;
    return result;
}

/*
 * Emit a host-filesystem-independent OCI root representation. The archive is
 * built directly from the canonical logical Linux graph: it is never staged
 * through APFS or another host filesystem, so byte-distinct Linux names and
 * hardlink identity survive on every supported host.
 */
int graph_write_tar(
    const logical_graph_t *graph, const char *destination,
    uint64_t *out_archive_size, oci_error_t *error) {
    char detail[256] = {0};
    int descriptor = open(destination,
        O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (descriptor < 0) return -1;
    struct archive *writer = archive_write_new();
    graph_entry_t **ordered = calloc(
        graph->count ? graph->count : 1u, sizeof(*ordered));
    char **first_path = calloc(
        (size_t)graph->next_inode + 1u, sizeof(*first_path));
    int result = writer && ordered && first_path ? 0 : -1;
    if (result == 0 && archive_write_set_format_pax_restricted(writer) != ARCHIVE_OK)
        result = -1;
    /* Linux pathnames are byte strings.  Never let the host locale normalize
     * or reject byte-distinct names while producing the portable root. */
    if (result == 0 &&
        archive_write_set_options(writer, "hdrcharset=BINARY") != ARCHIVE_OK)
        result = -1;
    if (result == 0 && archive_write_open_fd(writer, descriptor) != ARCHIVE_OK)
        result = -1;
    if (result == 0) {
        for (size_t i = 0u; i < graph->count; ++i)
            ordered[i] = &graph->entries[i];
        qsort(ordered, graph->count, sizeof(*ordered), compare_graph_entries);
    }
    for (size_t i = 0u; result == 0 && i < graph->count; ++i) {
        const graph_entry_t *source = ordered[i];
        /* The logical graph carries an explicit root inode.  A tar archive
         * represents that root implicitly; an empty pathname is invalid and
         * emitting "." would add a host-tool-dependent archive member. */
        if (!source->path[0]) continue;
        struct archive_entry *entry = archive_entry_new();
        if (!entry) {
            result = -1;
            break;
        }
        archive_entry_set_pathname(entry, source->path);
        archive_entry_set_perm(entry, (mode_t)(source->mode & 07777u));
        archive_entry_set_uid(entry, (la_int64_t)source->uid);
        archive_entry_set_gid(entry, (la_int64_t)source->gid);
        archive_entry_set_mtime(entry, (time_t)source->mtime, 0L);
        archive_entry_set_atime(entry, 0, 0L);
        archive_entry_set_ctime(entry, 0, 0L);
        archive_entry_set_birthtime(entry, 0, 0L);
        if (source->type == GRAPH_DIRECTORY) {
            archive_entry_set_filetype(entry, AE_IFDIR);
            archive_entry_set_size(entry, 0);
        } else if (source->type == GRAPH_SYMLINK) {
            archive_entry_set_filetype(entry, AE_IFLNK);
            archive_entry_set_symlink(entry, source->symlink_target);
            archive_entry_set_size(entry, 0);
        } else if (first_path[source->logical_inode]) {
            archive_entry_set_filetype(entry, AE_IFREG);
            archive_entry_set_hardlink(
                entry, first_path[source->logical_inode]);
            archive_entry_set_size(entry, 0);
        } else {
            archive_entry_set_filetype(entry, AE_IFREG);
            archive_entry_set_size(entry, (la_int64_t)source->size);
            first_path[source->logical_inode] = source->path;
        }
        int header_result = archive_write_header(writer, entry);
        int has_content = source->type == GRAPH_REGULAR &&
            !archive_entry_hardlink(entry);
        if (header_result != ARCHIVE_OK ||
            (has_content && archive_write_regular_content(writer, source) != 0) ||
            archive_write_finish_entry(writer) != ARCHIVE_OK)
            result = -1;
        archive_entry_free(entry);
    }
    if (result != 0 && writer && archive_error_string(writer))
        (void)oci_snprintf(detail, sizeof(detail), "%s",
            archive_error_string(writer));
    if (writer && archive_write_close(writer) != ARCHIVE_OK) {
        if (!detail[0] && archive_error_string(writer))
            (void)oci_snprintf(detail, sizeof(detail), "%s",
                archive_error_string(writer));
        result = -1;
    }
    if (writer && archive_write_free(writer) != ARCHIVE_OK) result = -1;
    free(first_path);
    free(ordered);
    struct stat status;
    if (result == 0 && (fstat(descriptor, &status) != 0 ||
        status.st_size <= 0 || (uint64_t)status.st_size > OCI_MAX_IMAGE_SIZE ||
        fchmod(descriptor, 0400) != 0 || fsync(descriptor) != 0)) result = -1;
    if (maelys_sys_fd_close(&descriptor) != MAELYS_SYS_OK) result = -1;
    if (result != 0) {
        (void)unlink(destination);
        oci_error_report(error, OCI_ERROR_IO,
            "cannot materialize the deterministic OCI root archive%s%s",
            detail[0] ? ": " : "", detail);
        return -1;
    }
    *out_archive_size = (uint64_t)status.st_size;
    return 0;
}

#if defined(__linux__)
static int directory_fd_empty(int descriptor) {
    int duplicate = dup(descriptor);
    if (duplicate < 0) return 0;
    DIR *directory = fdopendir(duplicate);
    if (!directory) {
        (void)maelys_sys_fd_close(&duplicate);
        return 0;
    }
    int empty = 1;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, ".") != 0 &&
            strcmp(entry->d_name, "..") != 0) {
            empty = 0;
            break;
        }
    }
    if (closedir(directory) != 0) empty = 0;
    return empty;
}
#endif

/*
 * Extract only a Warden-generated portable root archive.  This operation is
 * deliberately hosted by the separate materializer process: fchdir is safe
 * here, while the multi-threaded Executor process never changes its cwd or
 * links libarchive into its runtime TCB.
 */
int oci_unpack_portable_root(
    const char *archive_path, const char *destination, oci_error_t *error) {
#if !defined(__linux__)
    /*
     * The portable archive deliberately preserves Linux byte-string names.
     * Extracting it through a case-folding or Unicode-normalizing host
     * filesystem can silently alias distinct entries (README/readme is a
     * permanent regression fixture). Native crun is Linux-only, so refuse the
     * extraction command everywhere else instead of weakening the artifact.
     */
    (void)archive_path;
    (void)destination;
    oci_error_report(error, OCI_ERROR_UNSUPPORTED,
        "portable OCI roots may only be unpacked on Linux");
    return -1;
#else
    if (!archive_path || archive_path[0] != '/' || !destination ||
        destination[0] != '/') {
        oci_error_report(error, OCI_ERROR_ARGUMENT,
            "ARCHIVE and DESTINATION must be absolute paths");
        return -1;
    }
    int destination_fd = open(destination,
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    struct stat destination_status;
    if (destination_fd < 0 || fstat(destination_fd, &destination_status) != 0 ||
        !S_ISDIR(destination_status.st_mode) ||
        destination_status.st_uid != geteuid() ||
        (destination_status.st_mode & 0777) != 0700 ||
        !directory_fd_empty(destination_fd)) {
        if (destination_fd >= 0) (void)maelys_sys_fd_close(&destination_fd);
        oci_error_report(error, OCI_ERROR_STATE,
            "destination %s must be an empty private directory owned by the "
            "caller", destination);
        return -1;
    }
    int archive_fd = open(archive_path, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    struct stat archive_status;
    if (archive_fd < 0 || fstat(archive_fd, &archive_status) != 0 ||
        !S_ISREG(archive_status.st_mode) || archive_status.st_size <= 0 ||
        archive_status.st_uid != geteuid() ||
        (archive_status.st_mode & 0777) != 0400 ||
        archive_status.st_nlink != 1) {
        if (archive_fd >= 0) (void)maelys_sys_fd_close(&archive_fd);
        (void)maelys_sys_fd_close(&destination_fd);
        oci_error_report(error, OCI_ERROR_ACCESS,
            "archive %s must be a sealed read-only portable root", archive_path);
        return -1;
    }
    int original_cwd = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    struct archive *reader = archive_read_new();
    const char *stage = "initialize portable root extraction";
    int result = original_cwd >= 0 && reader ? 0 : -1;
    if (result == 0) {
        stage = "enable the tar reader";
        if (archive_read_support_format_tar(reader) != ARCHIVE_OK) result = -1;
    }
    if (result == 0) {
        stage = "restrict the portable root to an uncompressed tar";
        if (archive_read_support_filter_none(reader) != ARCHIVE_OK) result = -1;
    }
    if (result == 0) {
        stage = "open the sealed portable root archive";
        if (archive_read_open_fd(reader, archive_fd, 65536u) != ARCHIVE_OK) result = -1;
    }
    if (result == 0) {
        stage = "enter the private empty rootfs directory";
        if (fchdir(destination_fd) != 0) result = -1;
    }
    struct archive_entry *entry = NULL;
    int flags = ARCHIVE_EXTRACT_PERM | ARCHIVE_EXTRACT_TIME |
        ARCHIVE_EXTRACT_SECURE_NODOTDOT |
        ARCHIVE_EXTRACT_SECURE_NOABSOLUTEPATHS |
        ARCHIVE_EXTRACT_SECURE_SYMLINKS;
    if (geteuid() == 0) flags |= ARCHIVE_EXTRACT_OWNER;
    while (result == 0) {
        stage = "validate and extract a portable root member";
        int next = archive_read_next_header(reader, &entry);
        if (next == ARCHIVE_EOF) break;
        int header_available = next == ARCHIVE_OK || next == ARCHIVE_WARN;
        const char *path = header_available
            ? archive_entry_pathname(entry) : NULL;
        const char *hardlink = header_available
            ? archive_entry_hardlink(entry) : NULL;
        mode_t type = header_available ? archive_entry_filetype(entry) : 0;
        int member_valid = header_available && oci_relative_path_valid(path) &&
            (!hardlink || oci_relative_path_valid(hardlink)) &&
            (type == AE_IFREG || type == AE_IFDIR || type == AE_IFLNK ||
             (hardlink && type == 0));
        if (!member_valid) {
            oci_error_report(error, OCI_ERROR_PROTOCOL,
                "unsafe portable root member path=%s hardlink=%s type=%o status=%d",
                path ? path : "(null)", hardlink ? hardlink : "(none)",
                (unsigned int)type, next);
            result = -1;
            break;
        }
        if (archive_read_extract(reader, entry, flags) != ARCHIVE_OK) {
            result = -1;
            break;
        }
    }
    if (result != 0) {
        oci_error_report(error, OCI_ERROR_IO, "cannot %s%s%s", stage,
            reader && archive_error_string(reader) ? ": " : "",
            reader && archive_error_string(reader) ? archive_error_string(reader) : "");
    }
    if (original_cwd >= 0 && fchdir(original_cwd) != 0) {
        oci_error_report(error, OCI_ERROR_IO, "cannot restore the materializer working directory");
        result = -1;
    }
    if (reader && archive_read_close(reader) != ARCHIVE_OK) {
        oci_error_report(error, OCI_ERROR_IO, "cannot close the portable root archive");
        result = -1;
    }
    if (reader && archive_read_free(reader) != ARCHIVE_OK) {
        oci_error_report(error, OCI_ERROR_IO, "cannot release the portable root reader");
        result = -1;
    }
    if (original_cwd >= 0) (void)maelys_sys_fd_close(&original_cwd);
    (void)maelys_sys_fd_close(&archive_fd);
    (void)maelys_sys_fd_close(&destination_fd);
    return result;
#endif
}

ext2_ino_t ext_parent_inode(
    const logical_graph_t *graph, const ext2_ino_t *inode_map,
    const char *path) {
    char *parent = path_parent_copy(path);
    if (!parent) return 0;
    ext2_ino_t result = EXT2_ROOT_INO;
    if (parent[0]) {
        int index = graph_find(graph, parent);
        result = index >= 0
            ? inode_map[graph->entries[index].logical_inode] : 0;
    }
    free(parent);
    return result;
}
