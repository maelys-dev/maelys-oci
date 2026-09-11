/* SPDX-License-Identifier: MPL-2.0 */
/*
 * Import of one selected manifest: copy and verify the descriptor closure,
 * apply the layers to the logical graph,
 * materialize the deterministic ext4 and tar roots, then publish blobs,
 * closure and artifact without ever replacing an existing object.
 */
#include "src/materializer/internal.h"

#include <maelys/sys/clock.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int select_manifest(
    oci_manifest_t *items, size_t count, const char *platform,
    const char *digest, oci_manifest_t **out_selected, oci_error_t *error) {
    oci_manifest_t *selected = NULL;
    for (size_t i = 0u; i < count; ++i) {
        char candidate_platform[OCI_PLATFORM_SIZE];
        if (oci_manifest_platform(&items[i], candidate_platform) != 0) {
            oci_error_report(error, OCI_ERROR_PROTOCOL,
                "manifest %s declares an unusable platform",
                items[i].manifest.digest);
            return -1;
        }
        if (platform && strcmp(platform, candidate_platform) != 0) continue;
        if (digest && strcmp(digest, items[i].manifest.digest) != 0) continue;
        if (selected) {
            oci_error_report(error, OCI_ERROR_STATE,
                "several manifests match; select one with --platform or "
                "--digest");
            return -1;
        }
        selected = &items[i];
    }
    if (!selected || (!platform && !digest && count != 1u)) {
        oci_error_report(error, OCI_ERROR_STATE,
            "the source needs one unambiguous manifest; select it with "
            "--platform or --digest");
        return -1;
    }
    *out_selected = selected;
    return 0;
}

/* A descriptor whose bytes are absent, are not a regular file, or are not
 * the declared size and digest is the source's fault, reported as a protocol
 * failure; the copy's own host failures stay I/O failures reported by the
 * stage. The copy returns which of the two it was. */
static int copy_descriptor(
    const oci_source_t *source, const oci_descriptor_t *descriptor,
    const char *destination, oci_error_t *error) {
    source_copy_status_t status = SOURCE_COPY_OK;
    if (source_copy_descriptor(source, descriptor, destination, &status) == 0)
        return 0;
    if (status == SOURCE_COPY_CONTENT)
        oci_error_report(error, OCI_ERROR_PROTOCOL,
            "descriptor %s is absent from the source or is not its declared "
            "size and digest", descriptor->digest);
    return -1;
}

static int copy_selected_descriptors(
    const oci_source_t *source, const oci_manifest_t *selected,
    const char *directory, char ***out_layer_paths, oci_error_t *error) {
    if (!oci_manifest_staging_valid(selected) || !selected->diff_ids) {
        oci_error_report(error, OCI_ERROR_PROTOCOL,
            "manifest %s exceeds the staging bound or lacks DiffIDs",
            selected->manifest.digest);
        return -1;
    }
    char manifest_path[PATH_MAX];
    char config_path[PATH_MAX];
    if (oci_snprintf(manifest_path, sizeof(manifest_path), "%s/manifest.json",
            directory) < 0 ||
        oci_snprintf(config_path, sizeof(config_path), "%s/config.json",
            directory) < 0 ||
        copy_descriptor(source, &selected->manifest, manifest_path, error) != 0 ||
        copy_descriptor(source, &selected->config, config_path, error) != 0)
        return -1;
    char **paths = calloc(selected->layer_count ? selected->layer_count : 1u, sizeof(*paths));
    if (!paths) return -1;
    for (size_t i = 0u; i < selected->layer_count; ++i) {
        paths[i] = malloc(PATH_MAX);
        if (!paths[i] ||
            oci_snprintf(paths[i], PATH_MAX, "%s/layer-%04zu.tar", directory,
                i) < 0 ||
            copy_descriptor(source, &selected->layers[i], paths[i], error) != 0) {
            for (size_t j = 0u; j <= i; ++j) free(paths[j]);
            free(paths);
            return -1;
        }
    }
    *out_layer_paths = paths;
    return 0;
}

static int publish_selected_blobs(
    const char *store, const oci_manifest_t *selected,
    const char *descriptor_directory, char *const *layer_paths) {
    char manifest[PATH_MAX];
    char config[PATH_MAX];
    if (oci_snprintf(manifest, sizeof(manifest), "%s/manifest.json",
            descriptor_directory) < 0 ||
        oci_snprintf(config, sizeof(config), "%s/config.json",
            descriptor_directory) < 0 ||
        oci_store_blob_publish(store, &selected->manifest, manifest) != 0 ||
        oci_store_blob_publish(store, &selected->config, config) != 0)
        return -1;
    for (size_t i = 0u; i < selected->layer_count; ++i)
        if (oci_store_blob_publish(
                store, &selected->layers[i], layer_paths[i]) != 0)
            return -1;
    return 0;
}

static oci_document_t *descriptor_json(const oci_descriptor_t *descriptor) {
    return OCI_DOCUMENT_OBJECT(
            {"digest", oci_document_string(descriptor->digest)},
            {"mediaType", oci_document_string(descriptor->media_type)},
            {"size", oci_document_integer((int64_t)descriptor->size)});
}

static int write_closure_json(
    const char *path, const oci_manifest_t *selected) {
    char platform[OCI_PLATFORM_SIZE];
    if (oci_manifest_platform(selected, platform) != 0) return -1;
    oci_document_t *layers = oci_document_array();
    oci_document_t *root = oci_document_object();
    oci_document_t *manifest = descriptor_json(&selected->manifest);
    oci_document_t *config = descriptor_json(&selected->config);
    int result = layers && root && manifest && config ? 0 : -1;
    for (size_t i = 0u; result == 0 && i < selected->layer_count; ++i) {
        oci_document_t *layer = descriptor_json(&selected->layers[i]);
        if (!layer || oci_document_append(layers, layer) != 0) {
            result = -1;
        }
    }
    if (result == 0)
        result = oci_document_set(root, "config", config) == 0 &&
            oci_document_set(root, "layers", layers) == 0 &&
            oci_document_set(root, "manifest", manifest) == 0 &&
            oci_document_put(root, "manifestDigest",
                oci_document_string(selected->manifest.digest)) == 0 &&
            oci_document_put(root, "platform", oci_document_string(platform)) == 0 &&
            oci_document_put(root, "schema",
                oci_document_string(OCI_SCHEMA_CLOSURE)) == 0
            ? write_json_file(path, root) : -1;
    oci_document_release(config);
    oci_document_release(layers);
    oci_document_release(manifest);
    oci_document_release(root);
    return result;
}

static int publish_source_closure(
    const char *store, const oci_manifest_t *selected,
    const char *platform_name, const char *import_root) {
    char sources[PATH_MAX];
    char manifest_directory[PATH_MAX];
    if (ensure_private_child(store, "sources", sources) != 0 ||
        ensure_private_child(sources,
            selected->manifest.digest + OCI_DIGEST_PREFIX_SIZE,
            manifest_directory) != 0) return -1;
    char final_path[PATH_MAX];
    char staging[PATH_MAX];
    if (oci_snprintf(final_path, sizeof(final_path), "%s/%s",
            manifest_directory, platform_name) < 0 ||
        oci_snprintf(staging, sizeof(staging), "%s/source.XXXXXX",
            import_root) < 0 || !mkdtemp(staging) || chmod(staging, 0700) != 0)
        return -1;
    char closure[PATH_MAX];
    if (oci_snprintf(closure, sizeof(closure), "%s/closure.json", staging) < 0 ||
        write_closure_json(closure, selected) != 0 ||
        fsync_directory(staging) != 0) {
        (void)remove_filesystem_tree(staging);
        return -1;
    }
    int publication = maelys_oci_store_publish_directory_noreplace(
        staging, final_path);
    if (publication == 0) return fsync_directory(manifest_directory);
    if (publication != 1) {
        (void)remove_filesystem_tree(staging);
        return -1;
    }
    char existing[PATH_MAX];
    if (oci_snprintf(existing, sizeof(existing), "%s/closure.json",
            final_path) < 0 ||
        !private_directory(final_path) || !files_equal(closure, existing)) {
        (void)remove_filesystem_tree(staging);
        return -1;
    }
    return remove_filesystem_tree(staging);
}

static int artifact_directories_equal(
    const char *staging, const char *existing) {
    static const char *const names[] = {
        "root.ext4", "rootfs.tar", "artifact.json", "artifact.seal"
    };
    if (!private_directory(existing)) return 0;
    for (size_t i = 0u; i < sizeof(names) / sizeof(names[0]); ++i) {
        char left[PATH_MAX];
        char right[PATH_MAX];
        if (oci_snprintf(left, sizeof(left), "%s/%s", staging, names[i]) < 0 ||
            oci_snprintf(right, sizeof(right), "%s/%s", existing, names[i]) < 0 ||
            !files_equal(left, right)) return 0;
    }
    return 1;
}

/* Everything the artifact metadata and seal record about one import. */
typedef struct import_facts {
    char root_digest[OCI_DIGEST_HEX_SIZE];
    uint64_t root_size;
    char rootfs_tar_digest[OCI_DIGEST_HEX_SIZE];
    uint64_t rootfs_tar_size;
} import_facts_t;

static int prefixed_digest(char out[OCI_DIGEST_SIZE], const char *hex) {
    return oci_snprintf(out, OCI_DIGEST_SIZE, OCI_DIGEST_PREFIX "%s", hex) < 0
        ? -1 : 0;
}

static int write_metadata(
    const char *path, const oci_manifest_t *selected,
    const logical_graph_t *graph, const import_facts_t *facts,
    oci_error_t *error) {
    char platform[OCI_PLATFORM_SIZE];
    char root_identifier[OCI_DIGEST_SIZE];
    char rootfs_tar_identifier[OCI_DIGEST_SIZE];
    if (oci_manifest_platform(selected, platform) != 0 ||
        prefixed_digest(root_identifier, facts->root_digest) != 0 ||
        prefixed_digest(rootfs_tar_identifier, facts->rootfs_tar_digest) != 0) {
        oci_error_report(error, OCI_ERROR_IO,
            "cannot format artifact metadata identifiers");
        return -1;
    }
    oci_document_t *layers = oci_document_array();
    oci_document_t *root = oci_document_object();
    oci_document_t *materializer = OCI_DOCUMENT_OBJECT(
            {"name", oci_document_string("maelys-oci-materializer")},
            {"version", oci_document_string(MAELYS_OCI_BUILD_VERSION)},
            {"libext2fs", oci_document_string(MAELYS_EXT2FS_VERSION)});
    oci_document_t *ceilings = OCI_DOCUMENT_OBJECT(
            {"uncompressedBytes", oci_document_integer((int64_t)OCI_MAX_UNCOMPRESSED)},
            {"ext4Bytes", oci_document_integer((int64_t)OCI_MAX_IMAGE_SIZE)},
            {"entries", oci_document_integer((int)OCI_MAX_ENTRIES)},
            {"inodes", oci_document_integer((int)OCI_MAX_INODES)});
    oci_document_t *transformations = OCI_DOCUMENT_OBJECT(
            {"clearedSetuidSetgid", oci_document_integer((int64_t)graph->cleared_privilege_bits)});
    int built = layers && root && materializer && ceilings && transformations;
    for (size_t i = 0u; built && i < selected->layer_count; ++i)
        built = oci_document_append(
            layers, oci_document_string(selected->layers[i].digest)) == 0;
    if (built)
        built = oci_document_put(root, "schema",
                oci_document_string(OCI_SCHEMA_ARTIFACT)) == 0 &&
            oci_document_put(root, "manifestDigest",
                oci_document_string(selected->manifest.digest)) == 0 &&
            oci_document_put(root, "configDigest",
                oci_document_string(selected->config.digest)) == 0 &&
            oci_document_put(root, "platform", oci_document_string(platform)) == 0 &&
            oci_document_set(root, "layerDigests", layers) == 0 &&
            oci_document_put(root, "rootDigest",
                oci_document_string(root_identifier)) == 0 &&
            oci_document_put(root, "rootfsTarDigest",
                oci_document_string(rootfs_tar_identifier)) == 0 &&
            oci_document_put(root, "rootfsTarFormat",
                oci_document_string("pax-restricted")) == 0 &&
            oci_document_put(root, "rootBytes",
                oci_document_integer((int64_t)facts->root_size)) == 0 &&
            oci_document_put(root, "rootfsTarBytes",
                oci_document_integer((int64_t)facts->rootfs_tar_size)) == 0 &&
            oci_document_put(root, "logicalEntries",
                oci_document_integer((int64_t)graph->count)) == 0 &&
            oci_document_set(root, "materializer", materializer) == 0 &&
            oci_document_set(root, "ceilings", ceilings) == 0 &&
            oci_document_set(root, "transformations", transformations) == 0;
    int result = -1;
    if (!built) {
        oci_error_report(error, OCI_ERROR_MEMORY,
            "cannot construct canonical artifact metadata JSON");
    } else if (write_json_file(path, root) != 0) {
        oci_error_report(error, OCI_ERROR_IO,
            "cannot durably serialize artifact metadata JSON");
    } else {
        result = 0;
    }
    oci_document_release(layers);
    oci_document_release(materializer);
    oci_document_release(ceilings);
    oci_document_release(transformations);
    oci_document_release(root);
    return result;
}

static int write_seal(
    const char *path, const oci_manifest_t *selected,
    const import_facts_t *facts) {
    maelys_oci_seal_t seal;
    memset(&seal, 0, sizeof(seal));
    memcpy(seal.manifest, selected->manifest.digest, OCI_DIGEST_SIZE);
    memcpy(seal.config, selected->config.digest, OCI_DIGEST_SIZE);
    seal.root_size = facts->root_size;
    seal.rootfs_tar_size = facts->rootfs_tar_size;
    char text[MAELYS_OCI_SEAL_MAX_BYTES];
    int size;
    if (prefixed_digest(seal.root, facts->root_digest) != 0 ||
        prefixed_digest(seal.rootfs_tar, facts->rootfs_tar_digest) != 0 ||
        oci_manifest_platform(selected, seal.platform) != 0 ||
        (size = maelys_oci_seal_format(&seal, text, sizeof(text))) < 0)
        return -1;
    return oci_write_file_exclusive(path, text, (size_t)size, 0400);
}

static int hash_staged(
    const char *path, char out_digest[OCI_DIGEST_HEX_SIZE], oci_error_t *error) {
    char *hash_error = NULL;
    if (maelys_oci_sha256_file(path, out_digest, &hash_error) != MAELYS_OCI_OK) {
        oci_error_report(error, OCI_ERROR_IO, "%s",
            hash_error ? hash_error : "cannot hash the staged member");
        free(hash_error);
        return -1;
    }
    free(hash_error);
    return 0;
}

static oci_document_t *import_document(
    const oci_import_request_t *request, const oci_manifest_t *selected,
    const char *platform, const char *store, const char *artifact,
    int existed, int changed) {
    char reference[96];
    if (oci_snprintf(reference, sizeof(reference), "oci@%s",
            selected->manifest.digest) < 0) return NULL;
    return OCI_DOCUMENT_OBJECT(
            {"mode", oci_document_string(request->apply ? "apply" : "plan")},
            {"changed", oci_document_boolean(changed)},
            {"reference", oci_document_string(reference)},
            {"manifestDigest", oci_document_string(selected->manifest.digest)},
            {"configDigest", oci_document_string(selected->config.digest)},
            {"platform", oci_document_string(platform)},
            {"store", oci_document_string(store)},
            {"artifact", oci_document_string(artifact)},
            {"precondition", OCI_DOCUMENT_OBJECT(
            {"artifactExists", oci_document_boolean(existed)})});
}

static int import_checkpoint(uint64_t deadline, oci_error_t *error) {
    int expired = 0;
    if (deadline && (maelys_sys_deadline_expired(deadline, &expired) != MAELYS_SYS_OK || expired)) {
        oci_error_report(error, OCI_ERROR_IO, "global pull deadline exceeded during materialization");
        return -1;
    }
    return 0;
}

static int materialize(
    const oci_source_t *source, const oci_manifest_t *selected,
    const char *store,
    const char *platform_name, const char *final_path,
    int *out_changed, uint64_t deadline, oci_error_t *error) {
    char objects[PATH_MAX];
    char manifest_directory[PATH_MAX];
    if (ensure_private_child(store, "objects", objects) != 0 ||
        ensure_private_child(objects,
            selected->manifest.digest + OCI_DIGEST_PREFIX_SIZE,
            manifest_directory) != 0 ||
        fsync_directory(store) != 0 || fsync_directory(objects) != 0) {
        oci_error_report(error, OCI_ERROR_IO,
            "cannot durably create the OCI object namespace");
        return -1;
    }
    char import_root[PATH_MAX];
    char staging[PATH_MAX];
    if (oci_snprintf(import_root, sizeof(import_root), "%s/tmp/import",
            store) < 0 ||
        oci_snprintf(staging, sizeof(staging), "%s/artifact.XXXXXX",
            import_root) < 0 ||
        !mkdtemp(staging) || chmod(staging, 0700) != 0) {
        oci_error_report(error, OCI_ERROR_IO,
            "cannot create the private import staging directory");
        return -1;
    }
    char content[PATH_MAX];
    char descriptors[PATH_MAX];
    char root_path[PATH_MAX];
    char rootfs_tar_path[PATH_MAX];
    char metadata[PATH_MAX];
    char seal[PATH_MAX];
    const char *stage = "create private staging directories";
    int result = ensure_private_child(staging, "content", content) == 0 &&
        ensure_private_child(staging, "descriptors", descriptors) == 0 &&
        oci_snprintf(root_path, sizeof(root_path), "%s/root.ext4", staging) >= 0 &&
        oci_snprintf(rootfs_tar_path, sizeof(rootfs_tar_path),
            "%s/rootfs.tar", staging) >= 0 &&
        oci_snprintf(metadata, sizeof(metadata), "%s/artifact.json",
            staging) >= 0 &&
        oci_snprintf(seal, sizeof(seal), "%s/artifact.seal", staging) >= 0
        ? 0 : -1;
    char **layer_paths = NULL;
    logical_graph_t graph = {0};
    import_facts_t facts;
    memset(&facts, 0, sizeof(facts));
    if (result == 0 && (result = import_checkpoint(deadline, error)) == 0) {
        stage = "copy and verify the selected OCI descriptor closure";
        result = copy_selected_descriptors(
            source, selected, descriptors, &layer_paths, error);
    }
    if (result == 0 && (result = import_checkpoint(deadline, error)) == 0) {
        stage = "apply OCI layers to the logical Linux graph";
        for (size_t i = 0u; result == 0 && i < selected->layer_count; ++i)
            result = import_checkpoint(deadline, error) == 0
                ? graph_apply_layer(&graph, layer_paths[i], content,
                    selected->layers[i].size, selected->layers[i].media_type,
                    selected->diff_ids[i], error)
                : -1;
    }
    if (result == 0 && (result = import_checkpoint(deadline, error)) == 0) {
        stage = "publish the verified OCI source blobs without replacement";
        result = publish_selected_blobs(
            store, selected, descriptors, layer_paths);
    }
    if (result == 0 && (result = import_checkpoint(deadline, error)) == 0) {
        stage = "publish the canonical OCI source closure before its artifact";
        result = publish_source_closure(
            store, selected, platform_name, import_root);
    }
    if (result == 0 && (result = import_checkpoint(deadline, error)) == 0) {
        stage = "compact and reindex the logical Linux graph";
        result = graph_compact(&graph);
    }
    if (result == 0 && (result = import_checkpoint(deadline, error)) == 0) {
        stage = "materialize the deterministic ext4 root";
        result = graph_write_ext4(&graph, root_path, &facts.root_size, error);
    }
    if (result == 0 && (result = import_checkpoint(deadline, error)) == 0) {
        stage = "materialize the deterministic portable OCI root archive";
        result = graph_write_tar(&graph, rootfs_tar_path,
            &facts.rootfs_tar_size, error);
    }
    if (result == 0 && (result = import_checkpoint(deadline, error)) == 0) {
        stage = "hash the sealed roots";
        result = hash_staged(root_path, facts.root_digest, error) == 0 &&
            hash_staged(rootfs_tar_path, facts.rootfs_tar_digest, error) == 0
            ? 0 : -1;
    }
    if (result == 0 && (result = import_checkpoint(deadline, error)) == 0) {
        stage = "write immutable artifact metadata";
        result = write_metadata(metadata, selected, &graph, &facts, error);
    }
    if (result == 0 && (result = import_checkpoint(deadline, error)) == 0) {
        stage = "write the machine-verifiable artifact seal";
        result = write_seal(seal, selected, &facts);
    }
    if (result == 0 && (result = import_checkpoint(deadline, error)) == 0) {
        stage = "discard verified OCI import intermediates";
        result = remove_filesystem_tree(content) == 0 &&
            remove_filesystem_tree(descriptors) == 0 ? 0 : -1;
    }
    if (result == 0 && (result = import_checkpoint(deadline, error)) == 0) {
        stage = "synchronize the sealed artifact before publication";
        result = fsync_directory(staging);
    }
    if (result == 0 && (result = import_checkpoint(deadline, error)) == 0) {
        stage = "publish the immutable artifact without replacement";
        int publication = maelys_oci_store_publish_directory_noreplace(
            staging, final_path);
        *out_changed = publication == 0;
        if (publication == 1 && artifact_directories_equal(staging, final_path))
            result = remove_filesystem_tree(staging);
        else if (publication != 0)
            result = -1;
    }
    if (result == 0 && (result = import_checkpoint(deadline, error)) == 0) {
        stage = "synchronize the published OCI object namespace";
        result = fsync_directory(manifest_directory);
    }
    if (result != 0) {
        oci_error_report(error, OCI_ERROR_IO,
            "import failed closed while trying to %s; no partial artifact "
            "was published", stage);
        struct stat unfinished;
        if (lstat(staging, &unfinished) == 0 && S_ISDIR(unfinished.st_mode))
            (void)remove_filesystem_tree(staging);
    }
    if (layer_paths)
        for (size_t i = 0u; i < selected->layer_count; ++i) free(layer_paths[i]);
    free(layer_paths);
    graph_clear(&graph);
    return result;
}

/* A published artifact of a former schema is never replaced and never
 * migrated, so the import is refused before it stages anything: without this
 * it would do the whole work and fail at publication, naming neither the
 * former schema nor the way out. */
static int refuse_former_artifact(
    const char *artifact_path, const char *digest_name,
    const char *platform_directory, oci_error_t *error) {
    char metadata_path[PATH_MAX];
    if (oci_snprintf(metadata_path, sizeof(metadata_path), "%s/artifact.json",
            artifact_path) < 0) {
        oci_error_report(error, OCI_ERROR_ARGUMENT,
            "store path is too long for the artifact namespace");
        return -1;
    }
    const char *former = NULL;
    oci_document_t *document = load_former_artifact_schema(
        metadata_path, &former);
    int refused = former != NULL;
    if (refused)
        oci_error_report(error, OCI_ERROR_STATE,
            "artifact %s/%s carries the former schema %s, which this release "
            "does not migrate: recreate the store and reimport its sources",
            digest_name, platform_directory, former);
    oci_document_release(document);
    return refused ? -1 : 0;
}

int oci_import_locked(const oci_source_t *source, oci_manifest_t *selected,
    const char *store, const maelys_oci_store_lock_t *store_lock,
    const maelys_oci_store_lock_t *manifest_lock, uint64_t deadline, oci_document_t **out_document,
    oci_error_t *error) {
    *out_document = NULL;
    if (!store_lock || !store_lock->handle || !manifest_lock || !manifest_lock->handle)
        return -1;
    char platform[OCI_PLATFORM_SIZE], directory[OCI_PLATFORM_SIZE], path[PATH_MAX];
    if (oci_manifest_platform(selected, platform) != 0 ||
        oci_platform_to_directory(platform, directory) != 0 ||
        oci_snprintf(path, sizeof(path), "%s/objects/%s/%s", store,
            selected->manifest.digest + OCI_DIGEST_PREFIX_SIZE, directory) < 0)
        return -1;
    struct stat status;
    int existed = lstat(path, &status) == 0;
    if (existed && S_ISDIR(status.st_mode) &&
        refuse_former_artifact(path,
            selected->manifest.digest + OCI_DIGEST_PREFIX_SIZE, directory,
            error) != 0)
        return -1;
    int changed = 0;
    if (materialize(source, selected, store, directory, path, &changed, deadline, error) != 0)
        return -1;
    oci_import_request_t request = {.apply = 1};
    *out_document = import_document(&request, selected, platform, store, path, existed, changed);
    if (!*out_document) oci_error_report(error, OCI_ERROR_MEMORY, "cannot build import receipt");
    return *out_document ? 0 : -1;
}

int oci_import(
    const oci_source_t *source, oci_manifest_t *items, size_t count,
    const oci_import_request_t *request, oci_document_t **out_document,
    oci_error_t *error) {
    *out_document = NULL;
    oci_manifest_t *selected = NULL;
    if (!request->store || request->store[0] != '/') {
        oci_error_report(error, OCI_ERROR_ARGUMENT,
            "--store must be an absolute path");
        return -1;
    }
    if (select_manifest(items, count, request->platform, request->digest,
            &selected, error) != 0)
        return -1;
    char platform[OCI_PLATFORM_SIZE];
    char platform_name[OCI_PLATFORM_SIZE];
    if (oci_manifest_platform(selected, platform) != 0 ||
        oci_platform_to_directory(platform, platform_name) != 0) {
        oci_error_report(error, OCI_ERROR_PROTOCOL,
            "manifest %s declares an unusable platform",
            selected->manifest.digest);
        return -1;
    }
    char *store = NULL;
    if (request->apply) {
        if (maelys_oci_store_open_root(request->store, 1, &store) != 0) {
            oci_error_report(error, OCI_ERROR_STATE,
                "store %s must be an owned directory with mode 0700",
                request->store);
            return -1;
        }
        if (store_prepare_v2(store) != 0) {
            free(store);
            oci_error_report(error, OCI_ERROR_STATE,
                "cannot initialize or migrate the OCI store v2 contract in %s",
                request->store);
            return -1;
        }
    } else {
        store = realpath(request->store, NULL);
        if (!store) store = strdup(request->store);
        if (!store) {
            oci_error_report(error, OCI_ERROR_MEMORY, "out of memory");
            return -1;
        }
    }
    char final_path[PATH_MAX];
    if (oci_snprintf(final_path, sizeof(final_path), "%s/objects/%s/%s", store,
            selected->manifest.digest + OCI_DIGEST_PREFIX_SIZE,
            platform_name) < 0) {
        free(store);
        oci_error_report(error, OCI_ERROR_ARGUMENT,
            "store path is too long for the artifact namespace");
        return -1;
    }
    struct stat status;
    int existed = lstat(final_path, &status) == 0 && S_ISDIR(status.st_mode);
    int changed = 0;
    int result = 0;
    if (request->apply) {
        maelys_oci_store_lock_t store_lock = MAELYS_OCI_STORE_LOCK_INIT;
        maelys_oci_store_lock_t manifest_lock = MAELYS_OCI_STORE_LOCK_INIT;
        if (maelys_oci_store_lock_manifest(store,
                selected->manifest.digest + OCI_DIGEST_PREFIX_SIZE,
                &store_lock, &manifest_lock) != 0) {
            free(store);
            oci_error_report(error, OCI_ERROR_IO,
                "cannot acquire the ordered OCI import locks");
            return -1;
        }
        result = oci_import_locked(source, selected, store, &store_lock,
            &manifest_lock, 0u, out_document, error);
        maelys_oci_store_unlock_manifest(&store_lock, &manifest_lock);
    }
    if (result == 0 && !request->apply) {
        *out_document = import_document(request, selected, platform, store,
            final_path, existed, changed);
        if (!*out_document) {
            oci_error_report(error, OCI_ERROR_MEMORY, "out of memory");
            result = -1;
        }
    }
    free(store);
    return result;
}
