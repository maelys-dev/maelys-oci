/* SPDX-License-Identifier: MPL-2.0 */
/*
 * Deterministic ext4 image writer over libext2fs: fixed UUID and times,
 * inode metadata copied from the logical graph, hardlinks preserved.
 */
#include "src/materializer/internal.h"

#include <com_err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void inode_set_metadata(
    struct ext2_inode *inode, const graph_entry_t *entry, uint16_t kind) {
    inode->i_mode = kind | (uint16_t)(entry->mode & 07777u);
    inode->i_uid = (uint16_t)(entry->uid & 0xffffu);
    inode->i_gid = (uint16_t)(entry->gid & 0xffffu);
    ext2fs_set_i_uid_high(*inode, (uint16_t)(entry->uid >> 16u));
    ext2fs_set_i_gid_high(*inode, (uint16_t)(entry->gid >> 16u));
    int64_t seconds = entry->mtime;
    if (seconds < INT32_MIN) seconds = INT32_MIN;
    if (seconds > INT32_MAX) seconds = INT32_MAX;
    inode->i_atime = (__u32)(int32_t)seconds;
    inode->i_mtime = (__u32)(int32_t)seconds;
    inode->i_ctime = 0u;
}

static errcode_t ext_make_directory(
    ext2_filsys filesystem, ext2_ino_t parent, ext2_ino_t requested,
    const char *name, ext2_ino_t *out_inode) {
    errcode_t error = ext2fs_mkdir(filesystem, parent, requested, name);
    if (error || !out_inode) return error;
    if (requested) {
        *out_inode = requested;
        return 0;
    }
    return ext2fs_lookup(filesystem, parent, name, (int)strlen(name), NULL,
                         out_inode);
}

static errcode_t ext_zero_inode_times(
    ext2_filsys filesystem, ext2_ino_t inode_number) {
    struct ext2_inode inode;
    errcode_t error = ext2fs_read_inode(filesystem, inode_number, &inode);
    if (error) return error;
    inode.i_atime = 0u;
    inode.i_ctime = 0u;
    inode.i_mtime = 0u;
    return ext2fs_write_inode(filesystem, inode_number, &inode);
}

static int ext_write_regular(
    ext2_filsys filesystem, ext2_ino_t inode_number, const char *path) {
    int input = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    if (input < 0) return -1;
    ext2_file_t file = NULL;
    errcode_t error = ext2fs_file_open(
        filesystem, inode_number, EXT2_FILE_WRITE, &file);
    unsigned char buffer[64u * 1024u];
    while (!error) {
        ssize_t amount = read(input, buffer, sizeof(buffer));
        if (amount < 0 && errno == EINTR) continue;
        if (amount < 0) {
            error = EIO;
            break;
        }
        if (amount == 0) break;
        unsigned int offset = 0u;
        while (offset < (unsigned int)amount) {
            unsigned int written = 0u;
            error = ext2fs_file_write(
                file, buffer + offset, (unsigned int)amount - offset, &written);
            if (error || written == 0u) {
                if (!error) error = EIO;
                break;
            }
            offset += written;
        }
    }
    if (file) {
        errcode_t close_error = ext2fs_file_close(file);
        if (!error) error = close_error;
    }
    if (maelys_sys_fd_close(&input) != MAELYS_SYS_OK && !error) error = EIO;
    return error ? -1 : 0;
}

static int ext_update_metadata(
    ext2_filsys filesystem, ext2_ino_t inode_number,
    const graph_entry_t *entry, uint16_t kind) {
    struct ext2_inode inode;
    if (ext2fs_read_inode(filesystem, inode_number, &inode) != 0) return -1;
    inode_set_metadata(&inode, entry, kind);
    return ext2fs_write_inode(filesystem, inode_number, &inode) == 0 ? 0 : -1;
}

int graph_write_ext4(
    const logical_graph_t *graph, const char *destination,
    uint64_t *out_image_size, oci_error_t *error_out) {
    if (graph->count > OCI_MAX_INODES) {
        oci_error_report(error_out, OCI_ERROR_UNSUPPORTED,
            "logical graph exceeds the inode ceiling");
        return -1;
    }
    uint64_t data_blocks = (graph->content_bytes + OCI_BLOCK_SIZE - 1u) /
        OCI_BLOCK_SIZE;
    uint64_t image_size = (data_blocks + (uint64_t)graph->count * 2u + 16384u) *
        OCI_BLOCK_SIZE;
    if (image_size < OCI_MIN_IMAGE_SIZE) image_size = OCI_MIN_IMAGE_SIZE;
    if (image_size > OCI_MAX_IMAGE_SIZE) {
        oci_error_report(error_out, OCI_ERROR_UNSUPPORTED,
            "materialized ext4 image exceeds its size ceiling");
        return -1;
    }

    int image = open(destination,
        O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (image < 0 || ftruncate(image, (off_t)image_size) != 0) {
        oci_error_report(error_out, OCI_ERROR_IO,
            "cannot allocate the zero-initialized ext4 image: %s",
            strerror(errno));
        if (image >= 0) {
            (void)maelys_sys_fd_close(&image);
            (void)unlink(destination);
        }
        return -1;
    }
    if (maelys_sys_fd_close(&image) != MAELYS_SYS_OK) {
        oci_error_report(error_out, OCI_ERROR_IO,
            "cannot close the allocated ext4 image");
        (void)unlink(destination);
        return -1;
    }

    struct ext2_super_block parameters;
    memset(&parameters, 0, sizeof(parameters));
    ext2fs_blocks_count_set(&parameters, image_size / OCI_BLOCK_SIZE);
    parameters.s_inodes_count = (uint32_t)(graph->count + 64u);
    parameters.s_log_block_size = 2u;
    parameters.s_rev_level = EXT2_DYNAMIC_REV;
    parameters.s_inode_size = 256u;
    parameters.s_feature_incompat = EXT2_FEATURE_INCOMPAT_FILETYPE |
        EXT3_FEATURE_INCOMPAT_EXTENTS;
    parameters.s_feature_ro_compat = EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER;

    ext2_filsys filesystem = NULL;
    errcode_t error = ext2fs_initialize(
        destination, EXT2_FLAG_EXCLUSIVE, &parameters,
        unix_io_manager, &filesystem);
    if (!error) {
        /* A nonzero fixed value prevents older libext2fs releases from
         * consulting wall time. Every published inode is normalized below. */
        filesystem->now = 1;
        memset(filesystem->super->s_uuid, 0, sizeof(filesystem->super->s_uuid));
        memset(filesystem->super->s_hash_seed, 0,
               sizeof(filesystem->super->s_hash_seed));
        filesystem->super->s_mkfs_time = 0u;
        filesystem->super->s_mkfs_time_hi = 0u;
        filesystem->super->s_lastcheck = 0u;
        filesystem->super->s_lastcheck_hi = 0u;
        filesystem->super->s_wtime = 0u;
        filesystem->super->s_wtime_hi = 0u;
        error = ext2fs_allocate_tables(filesystem);
    }
    if (!error) {
        ext2_ino_t first_unreserved = EXT2_FIRST_INODE(filesystem->super);
        for (ext2_ino_t inode = 1u; inode < first_unreserved; ++inode) {
            /* ext_make_directory accounts for the root inode itself below. */
            if (inode != EXT2_ROOT_INO)
                ext2fs_inode_alloc_stats2(filesystem, inode, +1, 0);
        }
    }
    if (!error) {
        ext2_ino_t root_inode = 0u;
        error = ext_make_directory(filesystem, EXT2_ROOT_INO, EXT2_ROOT_INO,
                                   NULL, &root_inode);
        if (!error) error = ext_zero_inode_times(filesystem, root_inode);
    }

    graph_entry_t **ordered = NULL;
    ext2_ino_t *inode_map = NULL;
    if (!error) {
        ordered = calloc(graph->count ? graph->count : 1u, sizeof(*ordered));
        inode_map = calloc((size_t)graph->next_inode + 1u, sizeof(*inode_map));
        if (!ordered || !inode_map) error = ENOMEM;
    }
    if (!error) {
        for (size_t i = 0u; i < graph->count; ++i) ordered[i] = &graph->entries[i];
        qsort(ordered, graph->count, sizeof(*ordered), compare_graph_entries);
    }
    for (size_t i = 0u; !error && i < graph->count; ++i) {
        graph_entry_t *entry = ordered[i];
        ext2_ino_t parent = ext_parent_inode(graph, inode_map, entry->path);
        const char *name = path_basename(entry->path);
        if (!parent || !name[0]) {
            error = EINVAL;
            break;
        }
        ext2_ino_t inode_number = inode_map[entry->logical_inode];
        if (entry->type == GRAPH_DIRECTORY) {
            error = ext_make_directory(
                filesystem, parent, 0, name, &inode_number);
            if (!error && ext_update_metadata(filesystem, inode_number, entry,
                                               LINUX_S_IFDIR) != 0)
                error = EIO;
        } else if (entry->type == GRAPH_SYMLINK) {
            error = ext2fs_symlink(
                filesystem, parent, 0, name, entry->symlink_target);
            if (!error)
                error = ext2fs_lookup(filesystem, parent, name,
                                      (int)strlen(name), NULL, &inode_number);
            if (!error && ext_update_metadata(filesystem, inode_number, entry,
                                               LINUX_S_IFLNK) != 0)
                error = EIO;
        } else if (!inode_number) {
            error = ext2fs_new_inode(filesystem, parent,
                LINUX_S_IFREG | (entry->mode & 07777u), NULL, &inode_number);
            if (!error)
                error = ext2fs_link(filesystem, parent, name, inode_number,
                                    EXT2_FT_REG_FILE);
            if (!error) {
                ext2fs_inode_alloc_stats2(filesystem, inode_number, +1, 0);
                struct ext2_inode inode;
                memset(&inode, 0, sizeof(inode));
                inode_set_metadata(&inode, entry, LINUX_S_IFREG);
                inode.i_links_count = 1u;
                error = ext2fs_write_new_inode(filesystem, inode_number, &inode);
            }
            if (!error && ext_write_regular(
                    filesystem, inode_number, entry->content_path) != 0)
                error = EIO;
        } else {
            error = ext2fs_link(filesystem, parent, name, inode_number,
                                EXT2_FT_REG_FILE);
            if (!error) {
                struct ext2_inode inode;
                error = ext2fs_read_inode(filesystem, inode_number, &inode);
                if (!error) {
                    ++inode.i_links_count;
                    error = ext2fs_write_inode(filesystem, inode_number, &inode);
                }
            }
        }
        if (!error) inode_map[entry->logical_inode] = inode_number;
    }
    free(inode_map);
    free(ordered);
    if (filesystem) {
        errcode_t close_error = ext2fs_close_free(&filesystem);
        if (!error) error = close_error;
    }
    if (error) {
        oci_error_report(error_out, OCI_ERROR_IO, "libext2fs: %s",
            error_message(error));
        (void)unlink(destination);
        return -1;
    }
    int sealed = open(destination, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    int durable = sealed >= 0 && fchmod(sealed, 0400) == 0 &&
        fsync(sealed) == 0;
    if (sealed >= 0 && maelys_sys_fd_close(&sealed) != MAELYS_SYS_OK) durable = 0;
    if (!durable) {
        oci_error_report(error_out, OCI_ERROR_IO,
            "cannot durably seal the ext4 image read-only");
        (void)unlink(destination);
        return -1;
    }
    *out_image_size = image_size;
    return 0;
}
