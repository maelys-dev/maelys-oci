/* SPDX-License-Identifier: MPL-2.0 */
/*
 * Read access to an OCI layout: a directory opened without following links
 * or a tar archive validated into a private one-pass cache. Every read is
 * bounded and every blob is verified against its descriptor digest and size.
 */
#include "src/materializer/internal.h"

#include <errno.h>
#include <fcntl.h>
#include <maelys/sys/clock.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SOURCE_ARCHIVE_BLOCK (1024u * 1024u)
#define SOURCE_COPY_BUFFER (64u * 1024u)

typedef struct source_archive_member {
    char *relative;
    uint64_t offset;
    uint64_t size;
} source_archive_member_t;

typedef struct source_archive_cache {
    int descriptor;
    source_archive_member_t *members;
    size_t count;
    size_t capacity;
    uint64_t bytes;
} source_archive_cache_t;

static int archive_member_is_plain_file(struct archive_entry *entry);

static void source_archive_cache_clear(source_archive_cache_t *cache) {
    if (!cache) return;
    for (size_t i = 0u; i < cache->count; ++i) {
        free(cache->members[i].relative);
    }
    if (cache->descriptor >= 0)
        (void)maelys_sys_fd_close(&cache->descriptor);
    free(cache->members);
    free(cache);
}

static const source_archive_member_t *source_archive_member_find(
    const source_archive_cache_t *cache, const char *relative) {
    if (!cache || !relative) return NULL;
    size_t left = 0u;
    size_t right = cache->count;
    while (left < right) {
        size_t middle = left + (right - left) / 2u;
        int order = strcmp(cache->members[middle].relative, relative);
        if (order == 0) return &cache->members[middle];
        if (order < 0) left = middle + 1u;
        else right = middle;
    }
    return NULL;
}

static int compare_source_archive_members(const void *left, const void *right) {
    const source_archive_member_t *a = left;
    const source_archive_member_t *b = right;
    return strcmp(a->relative, b->relative);
}

static int source_archive_cache_finalize(source_archive_cache_t *cache) {
    if (cache->count > 1u)
        qsort(cache->members, cache->count, sizeof(*cache->members),
            compare_source_archive_members);
    for (size_t i = 1u; i < cache->count; ++i) {
        if (strcmp(cache->members[i - 1u].relative,
                cache->members[i].relative) == 0) {
            errno = EEXIST;
            return -1;
        }
    }
    return 0;
}

static int source_archive_relevant_name(
    const char *raw, const char **out_relative) {
    if (!raw) return 0;
    while (strncmp(raw, "./", 2u) == 0) raw += 2u;
    if (strcmp(raw, "oci-layout") == 0 || strcmp(raw, "index.json") == 0) {
        *out_relative = raw;
        return 1;
    }
    static const char prefix[] = "blobs/sha256/";
    if (strncmp(raw, prefix, sizeof(prefix) - 1u) == 0 &&
        oci_digest_hex_valid(raw + sizeof(prefix) - 1u)) {
        *out_relative = raw;
        return 1;
    }
    return 0;
}

static int source_archive_checkpoint(uint64_t deadline) {
    int expired = 0;
    if (maelys_sys_deadline_expired(deadline, &expired) != MAELYS_SYS_OK) {
        errno = EIO;
        return -1;
    }
    if (expired) {
        errno = ETIMEDOUT;
        return -1;
    }
    return 0;
}

static int source_archive_cache_reserve(source_archive_cache_t *cache) {
    if (cache->count >= OCI_SOURCE_ARCHIVE_MAX_FILES) {
        errno = EFBIG;
        return -1;
    }
    if (cache->count < cache->capacity) return 0;
    size_t capacity = cache->capacity ? cache->capacity * 2u : 64u;
    if (capacity > OCI_SOURCE_ARCHIVE_MAX_FILES)
        capacity = OCI_SOURCE_ARCHIVE_MAX_FILES;
    source_archive_member_t *grown = realloc(
        cache->members, capacity * sizeof(*cache->members));
    if (!grown) return -1;
    memset(grown + cache->capacity, 0,
        (capacity - cache->capacity) * sizeof(*grown));
    cache->members = grown;
    cache->capacity = capacity;
    return 0;
}

static int source_archive_cache_member(
    source_archive_cache_t *cache, struct archive *archive,
    const char *relative, uint64_t declared_size, uint64_t deadline) {
    if (source_archive_cache_reserve(cache) != 0)
        return -1;
    uint64_t cached_offset = cache->bytes;
    uint64_t copied = 0u;
    int result = 0;
    unsigned char buffer[SOURCE_COPY_BUFFER];
    while (result == 0 && copied < declared_size) {
        if (source_archive_checkpoint(deadline) != 0) {
            result = -1;
            break;
        }
        size_t wanted = declared_size - copied < sizeof(buffer)
            ? (size_t)(declared_size - copied) : sizeof(buffer);
        la_ssize_t amount = archive_read_data(archive, buffer, wanted);
        if (amount <= 0 || (uint64_t)amount > declared_size - copied ||
            oci_write_all(cache->descriptor, buffer, (size_t)amount) != 0) {
            if (amount <= 0) errno = EINVAL;
            result = -1;
            break;
        }
        copied += (uint64_t)amount;
    }
    if (result == 0 && archive_read_data(archive, buffer, 1u) != 0) {
        errno = EINVAL;
        result = -1;
    }
    char *relative_copy = result == 0 ? strdup(relative) : NULL;
    if (result != 0 || !relative_copy) {
        free(relative_copy);
        return -1;
    }
    cache->members[cache->count] = (source_archive_member_t){
        .relative = relative_copy,
        .offset = cached_offset,
        .size = declared_size
    };
    ++cache->count;
    cache->bytes += declared_size;
    return 0;
}

static int source_archive_discard_member(
    struct archive *archive, uint64_t declared_size, uint64_t deadline) {
    uint64_t discarded = 0u;
    unsigned char buffer[SOURCE_COPY_BUFFER];
    while (discarded < declared_size) {
        if (source_archive_checkpoint(deadline) != 0) return -1;
        size_t wanted = declared_size - discarded < sizeof(buffer)
            ? (size_t)(declared_size - discarded) : sizeof(buffer);
        la_ssize_t amount = archive_read_data(archive, buffer, wanted);
        if (amount <= 0 || (uint64_t)amount > declared_size - discarded) {
            errno = EINVAL;
            return -1;
        }
        discarded += (uint64_t)amount;
    }
    if (archive_read_data(archive, buffer, 1u) != 0) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

static int source_archive_cache_create(
    const char *path, source_archive_cache_t **out_cache);

static int open_relative_regular(int root_fd, const char *path) {
    if (!oci_relative_path_valid(path)) {
        errno = EINVAL;
        return -1;
    }
    char *copy = strdup(path);
    if (!copy) return -1;
    int directory = dup(root_fd);
    if (directory < 0) {
        free(copy);
        return -1;
    }
    char *cursor = copy;
    for (;;) {
        char *slash = strchr(cursor, '/');
        if (!slash) break;
        *slash = '\0';
        int next = openat(directory, cursor,
            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        (void)maelys_sys_fd_close(&directory);
        if (next < 0) {
            free(copy);
            return -1;
        }
        directory = next;
        cursor = slash + 1u;
    }
    int result = openat(directory, cursor, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    (void)maelys_sys_fd_close(&directory);
    free(copy);
    if (result < 0) return -1;
    struct stat status;
    if (fstat(result, &status) != 0 || !S_ISREG(status.st_mode)) {
        (void)maelys_sys_fd_close(&result);
        errno = EINVAL;
        return -1;
    }
    return result;
}

int source_open(const char *path, oci_source_t *out, oci_error_t *error) {
    memset(out, 0, sizeof(*out));
    out->directory_fd = -1;
    char *canonical = path ? realpath(path, NULL) : NULL;
    struct stat status;
    if (!canonical || lstat(canonical, &status) != 0) {
        free(canonical);
        oci_error_report(error, OCI_ERROR_NOT_FOUND,
            "source %s cannot be resolved: %s", path ? path : "(null)",
            strerror(errno));
        return -1;
    }
    if (!S_ISDIR(status.st_mode) && !S_ISREG(status.st_mode)) {
        free(canonical);
        oci_error_report(error, OCI_ERROR_ARGUMENT,
            "source %s is neither an OCI layout directory nor an archive",
            path);
        return -1;
    }
    out->path = canonical;
    if (S_ISDIR(status.st_mode)) {
        out->kind = SOURCE_DIRECTORY;
        out->directory_fd = open(canonical,
            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (out->directory_fd < 0) {
            oci_error_report(error, OCI_ERROR_IO,
                "cannot open source directory %s: %s", canonical,
                strerror(errno));
            source_close(out);
            return -1;
        }
    } else {
        out->kind = SOURCE_ARCHIVE;
        if (source_archive_cache_create(canonical, &out->archive_cache) != 0) {
            int saved = errno;
            oci_error_kind_t kind = saved == ENOMEM
                ? OCI_ERROR_MEMORY
                : (saved == EINVAL || saved == EFBIG || saved == EEXIST)
                    ? OCI_ERROR_PROTOCOL : OCI_ERROR_IO;
            const char *reason = saved == EFBIG
                ? "exceeds its security budgets"
                : saved == EEXIST ? "contains a duplicate OCI member"
                : saved == EINVAL ? "is malformed"
                : saved == ETIMEDOUT ? "exceeded its scan deadline"
                : "cannot be cached safely";
            oci_error_report(error, kind, "source archive %s %s: %s",
                canonical, reason, strerror(saved));
            source_close(out);
            errno = saved;
            return -1;
        }
    }
    return 0;
}

void source_close(oci_source_t *source) {
    if (!source) return;
    if (source->directory_fd >= 0) (void)maelys_sys_fd_close(&source->directory_fd);
    source_archive_cache_clear(source->archive_cache);
    free(source->path);
    memset(source, 0, sizeof(*source));
    source->directory_fd = -1;
}

int oci_archive_support_filters(struct archive *archive) {
    /* ARCHIVE_WARN means that libarchive registered an external-program
     * fallback. Refuse it before opening any input. Never enable filter_all:
     * it also registers formats that always execute a helper from PATH. */
    return archive_read_support_filter_none(archive) == ARCHIVE_OK &&
        archive_read_support_filter_gzip(archive) == ARCHIVE_OK &&
        archive_read_support_filter_zstd(archive) == ARCHIVE_OK ? 0 : -1;
}

/*
 * Validate and cache every OCI-relevant regular member in one bounded pass.
 * Compressed archives cannot be safely sought by tar offset; this private flat
 * cache avoids re-decompressing attacker-controlled prefixes for every blob.
 */
static int source_archive_cache_create(
    const char *path, source_archive_cache_t **out_cache) {
    *out_cache = NULL;
    int input = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    struct stat status;
    if (input < 0) return -1;
    if (fstat(input, &status) != 0) {
        int saved = errno;
        (void)maelys_sys_fd_close(&input);
        errno = saved;
        return -1;
    }
    if (!S_ISREG(status.st_mode) || status.st_size < 0) {
        (void)maelys_sys_fd_close(&input);
        errno = EINVAL;
        return -1;
    }
    if ((uint64_t)status.st_size > OCI_SOURCE_ARCHIVE_MAX_BYTES) {
        (void)maelys_sys_fd_close(&input);
        errno = EFBIG;
        return -1;
    }
    source_archive_cache_t *cache = calloc(1u, sizeof(*cache));
    if (!cache) {
        int saved = errno;
        (void)maelys_sys_fd_close(&input);
        errno = saved;
        return -1;
    }
    cache->descriptor = -1;
    char temporary[] = "/tmp/maelys-oci-source.XXXXXX";
    cache->descriptor = mkstemp(temporary);
    if (cache->descriptor < 0 ||
        fcntl(cache->descriptor, F_SETFD, FD_CLOEXEC) != 0 ||
        fchmod(cache->descriptor, 0600) != 0 || unlink(temporary) != 0) {
        int saved = errno;
        if (cache->descriptor >= 0) (void)unlink(temporary);
        source_archive_cache_clear(cache);
        (void)maelys_sys_fd_close(&input);
        errno = saved;
        return -1;
    }
    struct archive *archive = archive_read_new();
    uint64_t deadline = 0u;
    int result = archive &&
        maelys_sys_deadline_after(OCI_SOURCE_ARCHIVE_TIMEOUT_MS, &deadline) ==
            MAELYS_SYS_OK ? 0 : -1;
    if (result == 0 && (oci_archive_support_filters(archive) != 0 ||
            archive_read_support_format_tar(archive) != ARCHIVE_OK ||
            archive_read_set_format_option(
                archive, "tar", "hdrcharset", "UTF-8") != ARCHIVE_OK ||
            archive_read_open_fd(archive, input, SOURCE_ARCHIVE_BLOCK) != ARCHIVE_OK)) {
        errno = EINVAL;
        result = -1;
    }
    size_t member_count = 0u;
    uint64_t logical_bytes = 0u;
    while (result == 0) {
        if (source_archive_checkpoint(deadline) != 0) {
            result = -1;
            break;
        }
        struct archive_entry *entry = NULL;
        int next = archive_read_next_header(archive, &entry);
        if (next == ARCHIVE_EOF) break;
        if (next != ARCHIVE_OK || !entry ||
            ++member_count > OCI_SOURCE_ARCHIVE_MAX_MEMBERS) {
            errno = next == ARCHIVE_OK ? EFBIG : EINVAL;
            result = -1;
            break;
        }
        la_int64_t signed_size = archive_entry_size(entry);
        if (signed_size < 0 || (uint64_t)signed_size >
                OCI_SOURCE_ARCHIVE_MAX_BYTES - logical_bytes) {
            errno = EFBIG;
            result = -1;
            break;
        }
        uint64_t size = (uint64_t)signed_size;
        logical_bytes += size;
        const char *relative = NULL;
        if (source_archive_relevant_name(
                archive_entry_pathname(entry), &relative)) {
            if (!archive_member_is_plain_file(entry)) {
                errno = EINVAL;
                result = -1;
            } else if (source_archive_cache_member(
                    cache, archive, relative, size, deadline) != 0) {
                result = -1;
            }
        } else if (source_archive_discard_member(
                archive, size, deadline) != 0)
            result = -1;
    }
    if (archive && archive_read_close(archive) != ARCHIVE_OK) result = -1;
    if (archive && archive_read_free(archive) != ARCHIVE_OK) result = -1;
    if (maelys_sys_fd_close(&input) != MAELYS_SYS_OK) result = -1;
    if (result == 0 && source_archive_cache_finalize(cache) != 0) result = -1;
    if (result != 0) {
        int saved = errno ? errno : EINVAL;
        source_archive_cache_clear(cache);
        errno = saved;
        return -1;
    }
    *out_cache = cache;
    return 0;
}

static int archive_member_is_plain_file(struct archive_entry *entry) {
    return archive_entry_filetype(entry) == AE_IFREG &&
        !archive_entry_hardlink(entry) && !archive_entry_symlink(entry) &&
        archive_entry_size(entry) >= 0;
}

static const source_archive_member_t *cached_archive_member(
    const oci_source_t *source, const char *relative) {
    const source_archive_member_t *member = source_archive_member_find(
        source->archive_cache, relative);
    if (!member) {
        errno = ENOENT;
        return NULL;
    }
    return member;
}

static int source_read_archive(
    const oci_source_t *source, const char *relative, size_t maximum,
    unsigned char **out_bytes, size_t *out_size) {
    const source_archive_member_t *member = cached_archive_member(source, relative);
    if (!member) return -1;
    if (member->size > maximum || member->size > SIZE_MAX) {
        errno = EFBIG;
        return -1;
    }
    size_t size = (size_t)member->size;
    unsigned char *bytes = malloc(size ? size : 1u);
    if (!bytes || read_exact_at(source->archive_cache->descriptor,
            bytes, size, member->offset) != 0) {
        free(bytes);
        free(*out_bytes);
        *out_bytes = NULL;
        *out_size = 0u;
        return -1;
    }
    *out_bytes = bytes;
    *out_size = size;
    return 0;
}

int source_read(
    const oci_source_t *source, const char *relative, size_t maximum,
    unsigned char **out_bytes, size_t *out_size) {
    *out_bytes = NULL;
    *out_size = 0u;
    if (!oci_relative_path_valid(relative)) return -1;
    if (source->kind == SOURCE_ARCHIVE)
        return source_read_archive(
            source, relative, maximum, out_bytes, out_size);
    int fd = open_relative_regular(source->directory_fd, relative);
    if (fd < 0) return -1;
    int result = oci_read_regular_bounded(fd, maximum, out_bytes, out_size);
    (void)maelys_sys_fd_close(&fd);
    return result;
}

static int blob_relative_path(
    const oci_descriptor_t *descriptor, char relative[96]) {
    return oci_digest_valid(descriptor->digest) &&
        oci_snprintf(relative, 96u, "blobs/sha256/%s",
            descriptor->digest + OCI_DIGEST_PREFIX_SIZE) > 0 ? 0 : -1;
}

int source_read_descriptor(
    const oci_source_t *source, const oci_descriptor_t *descriptor,
    size_t maximum, unsigned char **out_bytes, size_t *out_size) {
    char relative[96];
    *out_bytes = NULL;
    *out_size = 0u;
    if (descriptor->size > maximum || blob_relative_path(descriptor, relative) != 0 ||
        source_read(source, relative, (size_t)descriptor->size,
            out_bytes, out_size) != 0) return -1;
    char digest[OCI_DIGEST_HEX_SIZE];
    maelys_oci_sha256_hex(*out_bytes, *out_size, digest);
    if (*out_size != descriptor->size ||
        strcmp(digest, descriptor->digest + OCI_DIGEST_PREFIX_SIZE) != 0) {
        free(*out_bytes);
        *out_bytes = NULL;
        *out_size = 0u;
        return -1;
    }
    return 0;
}

static int stream_copy_cached(
    const source_archive_cache_t *cache,
    const source_archive_member_t *member, int output, uint64_t maximum,
    uint64_t *out_size, char out_digest[OCI_DIGEST_HEX_SIZE]) {
    if (member->size > maximum) {
        errno = EFBIG;
        return -1;
    }
    maelys_oci_sha256_context_t hash;
    maelys_oci_sha256_init(&hash);
    uint64_t total = 0u;
    unsigned char buffer[SOURCE_COPY_BUFFER];
    while (total < member->size) {
        size_t amount = member->size - total < sizeof(buffer)
            ? (size_t)(member->size - total) : sizeof(buffer);
        if (read_exact_at(cache->descriptor, buffer, amount,
                member->offset + total) != 0 ||
            oci_write_all(output, buffer, amount) != 0)
            return -1;
        maelys_oci_sha256_update(&hash, buffer, amount);
        total += (uint64_t)amount;
    }
    maelys_oci_sha256_finish(&hash, out_digest);
    *out_size = total;
    return 0;
}

/* Classifies a failed open of a blob under the source root. O_NOFOLLOW turns
 * a symbolic link into ELOOP, a missing or non-directory prefix into ENOENT
 * or ENOTDIR and a directory into EISDIR: the layout's fault. Only the
 * exhaustion of this host's own resources is the host's. */
static source_copy_status_t open_failure_status(int reason) {
    switch (reason) {
    case ENOMEM:
    case EMFILE:
    case ENFILE:
    case EIO:
    case EROFS:
    case ENOSPC:
#ifdef EDQUOT
    case EDQUOT:
#endif
        return SOURCE_COPY_HOST;
    default:
        return SOURCE_COPY_CONTENT;
    }
}

int source_copy_descriptor(
    const oci_source_t *source, const oci_descriptor_t *descriptor,
    const char *destination, source_copy_status_t *out_status) {
    char relative[96];
    *out_status = SOURCE_COPY_CONTENT;
    if (blob_relative_path(descriptor, relative) != 0) return -1;
    int output = open(destination,
        O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (output < 0) {
        *out_status = SOURCE_COPY_HOST; /* the staging file is ours */
        return -1;
    }
    uint64_t size = 0u;
    char digest[OCI_DIGEST_HEX_SIZE] = {0};
    int result = -1;
    source_copy_status_t status = SOURCE_COPY_HOST;
    if (source->kind == SOURCE_DIRECTORY) {
        int input = open_relative_regular(source->directory_fd, relative);
        if (input < 0) {
            status = open_failure_status(errno);
        } else {
            struct stat blob;
            if (fstat(input, &blob) != 0) {
                status = SOURCE_COPY_HOST;
            } else if (blob.st_size < 0 ||
                (uint64_t)blob.st_size != descriptor->size) {
                status = SOURCE_COPY_CONTENT; /* not its declared size */
            } else if ((result = oci_copy_fd_hashed(
                    input, output, descriptor->size, &size, digest)) != 0) {
                /* A blob that grew between the fstat and the read is the
                 * source's; a failed read or write is this host's. */
                status = errno == EFBIG ? SOURCE_COPY_CONTENT : SOURCE_COPY_HOST;
            }
            (void)maelys_sys_fd_close(&input);
        }
    } else {
        const source_archive_member_t *member =
            cached_archive_member(source, relative);
        if (!member) {
            status = SOURCE_COPY_CONTENT; /* absent from the cached archive */
        } else if ((result = stream_copy_cached(source->archive_cache, member,
                output, descriptor->size, &size, digest)) != 0) {
            status = errno == EFBIG ? SOURCE_COPY_CONTENT : SOURCE_COPY_HOST;
        }
    }
    if (result == 0 && (size != descriptor->size ||
        strcmp(digest, descriptor->digest + OCI_DIGEST_PREFIX_SIZE) != 0)) {
        status = SOURCE_COPY_CONTENT; /* the bytes are not the descriptor's */
        result = -1;
    }
    if (result == 0 && fsync(output) != 0) {
        status = SOURCE_COPY_HOST;
        result = -1;
    }
    if (maelys_sys_fd_close(&output) != MAELYS_SYS_OK) {
        if (result == 0) status = SOURCE_COPY_HOST;
        result = -1;
    }
    if (result != 0) {
        int saved = errno;
        (void)unlink(destination);
        errno = saved;
    }
    *out_status = result == 0 ? SOURCE_COPY_OK : status;
    return result;
}

oci_document_t *load_json(const unsigned char *bytes, size_t size) {
    oci_document_t *root = oci_document_parse(bytes, size);
    if (!root || !oci_document_is_object(root)) {
        oci_document_release(root);
        return NULL;
    }
    return root;
}
