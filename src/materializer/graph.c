/* SPDX-License-Identifier: MPL-2.0 */
/*
 * The logical Linux graph: a path-indexed set of directories, regular files
 * and symlinks with hardlink identity.
 */
#include "src/materializer/internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>

void graph_entry_clear(graph_entry_t *entry) {
    if (!entry) return;
    free(entry->path);
    free(entry->content_path);
    free(entry->symlink_target);
    memset(entry, 0, sizeof(*entry));
}

void graph_clear(logical_graph_t *graph) {
    if (!graph) return;
    for (size_t i = 0u; i < graph->count; ++i)
        graph_entry_clear(&graph->entries[i]);
    free(graph->entries);
    free(graph->index);
    memset(graph, 0, sizeof(*graph));
}

static uint64_t graph_path_hash(const char *path) {
    uint64_t hash = UINT64_C(1469598103934665603);
    for (const unsigned char *cursor = (const unsigned char *)path;
         *cursor; ++cursor) {
        hash ^= *cursor;
        hash *= UINT64_C(1099511628211);
    }
    return hash ? hash : UINT64_C(1);
}

static int graph_index_rebuild(logical_graph_t *graph, size_t capacity) {
    if (capacity < 512u) capacity = 512u;
    size_t rounded = 1u;
    while (rounded < capacity) {
        if (rounded > SIZE_MAX / 2u) return -1;
        rounded *= 2u;
    }
    graph_index_slot_t *slots = calloc(rounded, sizeof(*slots));
    if (!slots) return -1;
    for (size_t i = 0u; i < graph->count; ++i) {
        if (!graph->entries[i].path) continue;
        uint64_t hash = graph_path_hash(graph->entries[i].path);
        size_t slot = (size_t)hash & (rounded - 1u);
        while (slots[slot].state == 1u) slot = (slot + 1u) & (rounded - 1u);
        slots[slot].state = 1u;
        slots[slot].hash = hash;
        slots[slot].entry_index = i;
    }
    free(graph->index);
    graph->index = slots;
    graph->index_capacity = rounded;
    graph->index_count = graph->active_count;
    graph->index_tombstones = 0u;
    return 0;
}

int graph_find(const logical_graph_t *graph, const char *path) {
    if (!graph->index_capacity || !path) return -1;
    uint64_t hash = graph_path_hash(path);
    size_t slot = (size_t)hash & (graph->index_capacity - 1u);
    for (size_t visited = 0u; visited < graph->index_capacity; ++visited) {
        const graph_index_slot_t *candidate = &graph->index[slot];
        if (candidate->state == 0u) return -1;
        if (candidate->state == 1u && candidate->hash == hash &&
            candidate->entry_index < graph->count &&
            graph->entries[candidate->entry_index].path &&
            strcmp(graph->entries[candidate->entry_index].path, path) == 0)
            return (int)candidate->entry_index;
        slot = (slot + 1u) & (graph->index_capacity - 1u);
    }
    return -1;
}

static int graph_index_insert(
    logical_graph_t *graph, const char *path, size_t entry_index) {
    if (!graph->index_capacity ||
        (graph->index_count + graph->index_tombstones + 1u) * 10u >=
            graph->index_capacity * 7u) {
        size_t requested = graph->index_capacity
            ? graph->index_capacity * 2u : 512u;
        if (graph_index_rebuild(graph, requested) != 0) return -1;
    }
    uint64_t hash = graph_path_hash(path);
    size_t slot = (size_t)hash & (graph->index_capacity - 1u);
    size_t tombstone = SIZE_MAX;
    for (;;) {
        graph_index_slot_t *candidate = &graph->index[slot];
        if (candidate->state == 0u) {
            if (tombstone != SIZE_MAX) candidate = &graph->index[tombstone];
            candidate->state = 1u;
            candidate->hash = hash;
            candidate->entry_index = entry_index;
            ++graph->index_count;
            if (tombstone != SIZE_MAX) --graph->index_tombstones;
            return 0;
        }
        if (candidate->state == 2u && tombstone == SIZE_MAX) tombstone = slot;
        slot = (slot + 1u) & (graph->index_capacity - 1u);
    }
}

static void graph_index_remove(logical_graph_t *graph, const char *path) {
    if (!graph->index_capacity || !path) return;
    uint64_t hash = graph_path_hash(path);
    size_t slot = (size_t)hash & (graph->index_capacity - 1u);
    for (size_t visited = 0u; visited < graph->index_capacity; ++visited) {
        graph_index_slot_t *candidate = &graph->index[slot];
        if (candidate->state == 0u) return;
        if (candidate->state == 1u && candidate->hash == hash &&
            candidate->entry_index < graph->count &&
            graph->entries[candidate->entry_index].path &&
            strcmp(graph->entries[candidate->entry_index].path, path) == 0) {
            candidate->state = 2u;
            --graph->index_count;
            ++graph->index_tombstones;
            return;
        }
        slot = (slot + 1u) & (graph->index_capacity - 1u);
    }
}

static int compare_inode_entries(const void *left, const void *right) {
    const graph_entry_t *a = *(const graph_entry_t *const *)left;
    const graph_entry_t *b = *(const graph_entry_t *const *)right;
    if (a->logical_inode < b->logical_inode) return -1;
    if (a->logical_inode > b->logical_inode) return 1;
    return strcmp(a->path, b->path);
}

int graph_compact(logical_graph_t *graph) {
    size_t target = 0u;
    for (size_t source = 0u; source < graph->count; ++source) {
        if (!graph->entries[source].path) continue;
        if (target != source) {
            graph->entries[target] = graph->entries[source];
            memset(&graph->entries[source], 0, sizeof(graph->entries[source]));
        }
        ++target;
    }
    graph->count = target;
    graph->active_count = target;
    graph_entry_t **by_inode = calloc(target ? target : 1u, sizeof(*by_inode));
    if (!by_inode) return -1;
    for (size_t i = 0u; i < target; ++i) {
        if (graph->entries[i].logical_inode == 0u) {
            free(by_inode);
            return -1;
        }
        by_inode[i] = &graph->entries[i];
    }
    qsort(by_inode, target, sizeof(*by_inode), compare_inode_entries);
    uint64_t previous = 0u;
    uint64_t dense = 0u;
    for (size_t i = 0u; i < target; ++i) {
        uint64_t original = by_inode[i]->logical_inode;
        if (i == 0u || original != previous) {
            previous = original;
            ++dense;
        }
        by_inode[i]->logical_inode = dense;
    }
    free(by_inode);
    graph->next_inode = dense;
    return graph_index_rebuild(graph, target * 2u);
}

int graph_allocate_inode(logical_graph_t *graph, uint64_t *out_inode) {
    if (!graph || !out_inode || graph->next_inode == UINT64_MAX) return -1;
    *out_inode = ++graph->next_inode;
    return 0;
}

static int path_is_descendant(const char *path, const char *parent) {
    size_t parent_size = strlen(parent);
    if (parent_size == 0u) return path[0] != '\0';
    return strncmp(path, parent, parent_size) == 0 &&
        path[parent_size] == '/';
}

static void graph_remove_index(logical_graph_t *graph, size_t index) {
    if (index >= graph->count || !graph->entries[index].path) return;
    if (graph->entries[index].type == GRAPH_REGULAR &&
        graph->content_bytes >= graph->entries[index].size)
        graph->content_bytes -= graph->entries[index].size;
    graph_index_remove(graph, graph->entries[index].path);
    graph_entry_clear(&graph->entries[index]);
    --graph->active_count;
}

void graph_remove_tree(logical_graph_t *graph, const char *path) {
    size_t index = 0u;
    while (index < graph->count) {
        if (graph->entries[index].path &&
            (strcmp(graph->entries[index].path, path) == 0 ||
             path_is_descendant(graph->entries[index].path, path)))
            graph_remove_index(graph, index);
        ++index;
    }
}

void graph_remove_children(logical_graph_t *graph, const char *path) {
    size_t index = 0u;
    while (index < graph->count) {
        if (graph->entries[index].path &&
            path_is_descendant(graph->entries[index].path, path))
            graph_remove_index(graph, index);
        ++index;
    }
}

int normalize_layer_path(const char *raw, char **out_path) {
    *out_path = NULL;
    if (!raw || !raw[0] || raw[0] == '/' || strlen(raw) > 4096u)
        return -1;
    while (strncmp(raw, "./", 2u) == 0) raw += 2u;
    if (!raw[0]) {
        *out_path = strdup("");
        return *out_path ? 0 : -1;
    }
    size_t raw_size = strlen(raw);
    while (raw_size && raw[raw_size - 1u] == '/') --raw_size;
    if (!raw_size) {
        *out_path = strdup("");
        return *out_path ? 0 : -1;
    }
    char *path = malloc(raw_size + 1u);
    if (!path) return -1;
    memcpy(path, raw, raw_size);
    path[raw_size] = '\0';
    if (!oci_relative_path_valid(path)) {
        free(path);
        return -1;
    }
    *out_path = path;
    return 0;
}

static int graph_reserve(logical_graph_t *graph) {
    if (graph->active_count >= OCI_MAX_ENTRIES) return -1;
    if (graph->count < graph->capacity) return 0;
    /* Compaction renumbers hardlink identities, so it is performed only at
     * explicit quiescent points where no pending entry carries an old inode. */
    if (graph->count >= OCI_MAX_ENTRIES) return -1;
    size_t capacity = graph->capacity ? graph->capacity * 2u : 256u;
    if (capacity > OCI_MAX_ENTRIES) capacity = OCI_MAX_ENTRIES;
    graph_entry_t *grown = realloc(
        graph->entries, capacity * sizeof(*graph->entries));
    if (!grown) return -1;
    graph->entries = grown;
    graph->capacity = capacity;
    return 0;
}

int graph_put(logical_graph_t *graph, graph_entry_t *entry) {
    int existing = graph_find(graph, entry->path);
    if (existing >= 0) {
        if (graph->entries[existing].type == GRAPH_DIRECTORY &&
            entry->type != GRAPH_DIRECTORY)
            graph_remove_tree(graph, entry->path);
        else
            graph_remove_index(graph, (size_t)existing);
    }
    if (graph_reserve(graph) != 0) return -1;
    if (entry->type == GRAPH_REGULAR) {
        if (UINT64_MAX - graph->content_bytes < entry->size ||
            graph->content_bytes + entry->size > OCI_MAX_UNCOMPRESSED)
            return -1;
        graph->content_bytes += entry->size;
    }
    size_t inserted = graph->count;
    graph->entries[inserted] = *entry;
    if (graph_index_insert(graph, entry->path, inserted) != 0) {
        memset(&graph->entries[inserted], 0, sizeof(graph->entries[inserted]));
        if (entry->type == GRAPH_REGULAR) graph->content_bytes -= entry->size;
        return -1;
    }
    ++graph->count;
    ++graph->active_count;
    memset(entry, 0, sizeof(*entry));
    return 0;
}

int path_has_whiteout_segment(const char *path, size_t size) {
    size_t start = 0u;
    for (size_t i = 0u; i <= size; ++i) {
        if (i == size || path[i] == '/') {
            if (i - start >= 4u && memcmp(path + start, ".wh.", 4u) == 0)
                return 1;
            start = i + 1u;
        }
    }
    return 0;
}

static int graph_ensure_directory(
    logical_graph_t *graph, const char *path, uint32_t mode,
    uint32_t uid, uint32_t gid, int64_t mtime) {
    if (!path[0]) return 0;
    /* A marker name is consumed by the layer application and never
     * materialized; creating one implicitly, as the parent of an entry,
     * would publish it in the root, where a consumer of rootfs.tar reads it
     * back as a deletion. */
    if (path_has_whiteout_segment(path, strlen(path))) return -1;
    int existing = graph_find(graph, path);
    if (existing >= 0)
        return graph->entries[existing].type == GRAPH_DIRECTORY ? 0 : -1;
    const char *slash = strrchr(path, '/');
    if (slash) {
        size_t parent_size = (size_t)(slash - path);
        char *parent = malloc(parent_size + 1u);
        if (!parent) return -1;
        memcpy(parent, path, parent_size);
        parent[parent_size] = '\0';
        int result = graph_ensure_directory(graph, parent, 0755u, 0u, 0u, 0);
        free(parent);
        if (result != 0) return -1;
    }
    uint64_t inode = 0u;
    if (graph_allocate_inode(graph, &inode) != 0) return -1;
    graph_entry_t entry = {
        .path = strdup(path),
        .type = GRAPH_DIRECTORY,
        .mode = mode,
        .uid = uid,
        .gid = gid,
        .mtime = mtime,
        .logical_inode = inode
    };
    if (!entry.path || graph_put(graph, &entry) != 0) {
        graph_entry_clear(&entry);
        return -1;
    }
    return 0;
}

int graph_ensure_parent(logical_graph_t *graph, const char *path) {
    const char *slash = strrchr(path, '/');
    if (!slash) return 0;
    size_t size = (size_t)(slash - path);
    char *parent = malloc(size + 1u);
    if (!parent) return -1;
    memcpy(parent, path, size);
    parent[size] = '\0';
    int result = graph_ensure_directory(graph, parent, 0755u, 0u, 0u, 0);
    free(parent);
    return result;
}

const char *path_basename(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1u : path;
}

char *path_parent_copy(const char *path) {
    const char *slash = strrchr(path, '/');
    if (!slash) return strdup("");
    size_t size = (size_t)(slash - path);
    char *parent = malloc(size + 1u);
    if (!parent) return NULL;
    memcpy(parent, path, size);
    parent[size] = '\0';
    return parent;
}

int write_fd_content(
    int input, uint64_t input_offset, uint64_t content_size,
    const char *content_directory, char **out_path) {
    char temporary[PATH_MAX], id[33];
    if (oci_store_random_id(id) != 0 ||
        oci_snprintf(temporary, sizeof(temporary),
            "%s/.content.%s", content_directory, id) < 0) return -1;
    int output = open(temporary,
        O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (output < 0) return -1;
    maelys_oci_sha256_context_t hash;
    maelys_oci_sha256_init(&hash);
    uint64_t total = 0u;
    int result = 0;
    unsigned char buffer[64u * 1024u];
    for (;;) {
        size_t wanted = content_size - total < sizeof(buffer)
            ? (size_t)(content_size - total) : sizeof(buffer);
        if (wanted == 0u) break;
        ssize_t amount = pread(input, buffer, wanted,
                               (off_t)(input_offset + total));
        if (amount < 0 && errno == EINTR) continue;
        if (amount <= 0 || oci_write_all(output, buffer, (size_t)amount) != 0) {
            result = -1;
            break;
        }
        total += (uint64_t)amount;
        maelys_oci_sha256_update(&hash, buffer, (size_t)amount);
    }
    char digest[OCI_DIGEST_HEX_SIZE];
    maelys_oci_sha256_finish(&hash, digest);
    if (result == 0 && fsync(output) != 0) result = -1;
    if (maelys_sys_fd_close(&output) != MAELYS_SYS_OK) result = -1;
    char final[PATH_MAX];
    if (oci_snprintf(final, sizeof(final), "%s/%s", content_directory,
            digest) < 0) result = -1;
    if (result == 0 && link(temporary, final) != 0 && errno != EEXIST)
        result = -1;
    if (result == 0 && unlink(temporary) != 0) result = -1;
    if (result != 0) {
        (void)unlink(temporary);
        return -1;
    }
    *out_path = strdup(final);
    return *out_path ? 0 : -1;
}

int parse_octal(
    const unsigned char *field, size_t field_size, uint64_t *out) {
    size_t offset = 0u;
    while (offset < field_size && (field[offset] == ' ' || field[offset] == '\0'))
        ++offset;
    if (offset == field_size) {
        *out = 0u;
        return 0;
    }
    if (field[offset] & 0x80u) return -1;
    uint64_t value = 0u;
    int digits = 0;
    for (; offset < field_size; ++offset) {
        unsigned char byte = field[offset];
        if (byte == '\0' || byte == ' ') break;
        if (byte < '0' || byte > '7' || value > (UINT64_MAX >> 3u)) return -1;
        value = (value << 3u) | (uint64_t)(byte - '0');
        digits = 1;
    }
    if (!digits) return -1;
    *out = value;
    return 0;
}

int tar_checksum_valid(const unsigned char header[512]) {
    uint64_t expected = 0u;
    if (parse_octal(header + 148u, 8u, &expected) != 0) return 0;
    uint64_t actual = 0u;
    for (size_t i = 0u; i < 512u; ++i)
        actual += i >= 148u && i < 156u ? (unsigned char)' ' : header[i];
    return actual == expected;
}

char *tar_field_string(const unsigned char *field, size_t size) {
    size_t length = 0u;
    while (length < size && field[length] != '\0') ++length;
    if (!length) return strdup("");
    char *text = malloc(length + 1u);
    if (!text) return NULL;
    memcpy(text, field, length);
    text[length] = '\0';
    return text;
}

char *tar_header_path(const unsigned char header[512]) {
    char *name = tar_field_string(header, 100u);
    char *prefix = tar_field_string(header + 345u, 155u);
    if (!name || !prefix) {
        free(name);
        free(prefix);
        return NULL;
    }
    if (!prefix[0]) {
        free(prefix);
        return name;
    }
    size_t size = strlen(prefix) + strlen(name) + 2u;
    char *path = malloc(size);
    if (path) (void)oci_snprintf(path, size, "%s/%s", prefix, name);
    free(prefix);
    free(name);
    return path;
}

int read_exact_at(int fd, void *buffer, size_t size, uint64_t offset) {
    size_t done = 0u;
    while (done < size) {
        ssize_t amount = pread(fd, (unsigned char *)buffer + done, size - done,
                               (off_t)(offset + done));
        if (amount < 0 && errno == EINTR) continue;
        if (amount <= 0) return -1;
        done += (size_t)amount;
    }
    return 0;
}
