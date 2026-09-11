#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0
set -euo pipefail

helper="${1:?usage: test_oci_materializer.sh MAELYS_OCI TEST_LEASE}"
lease_test="${2:?usage: test_oci_materializer.sh MAELYS_OCI TEST_LEASE}"
root="$(mktemp -d /tmp/maelys-oci-inspect.XXXXXX)"

# Every command renders its schema-described data document; --json wraps it
# in the agent-cli/v2 envelope. There is no prose output to parse.
# data_field FILE KEY: prints one member of data.
data_field() {
  python3 -c 'import json,sys; print(json.load(open(sys.argv[1], encoding="utf-8"))["data"][sys.argv[2]])' "$1" "$2"
}
# data_assert FILE SCRIPT [ARG...]: parses the envelope written by a --json
# invocation and runs the Python assertions against its data member.
data_assert() {
  python3 - "$1" "${@:3}" <<PY
import json, sys
document = json.load(open(sys.argv[1], encoding="utf-8"))
assert document["contract"] == "agent-cli/v2" and document["ok"] is True, document
data = document["data"]
$2
PY
}
trap 'rm -rf "$root"' EXIT
layout="$root/layout"
mkdir -p "$layout/blobs/sha256"
python3 - "$root/layer.tar" <<'PY'
import io, sys, tarfile

with tarfile.open(sys.argv[1], "w", format=tarfile.USTAR_FORMAT) as archive:
    for name, content, kind in (
        ("etc", b"", "directory"),
        ("etc/example", b"hello\n", "file"),
        ("README", b"upper\n", "file"),
        ("readme", b"lower\n", "file"),
        ("caf\u00e9", b"composed\n", "file"),
        ("cafe\u0301", b"decomposed\n", "file"),
        ("maelys-init", b"image-owned-init-must-not-run\n", "file"),
        ("maelys-bwrap", b"image-owned-bwrap-must-not-run\n", "file"),
        ("maelys-guest-supervisor-linux", b"image-owned-relay-must-not-run\n", "file"),
    ):
        entry = tarfile.TarInfo(name)
        entry.uid = 1234
        entry.gid = 2345
        entry.mtime = 0
        entry.mode = 0o755 if kind == "directory" else 0o644
        if kind == "directory":
            entry.type = tarfile.DIRTYPE
            archive.addfile(entry)
        else:
            entry.size = len(content)
            archive.addfile(entry, io.BytesIO(content))
    hardlink = tarfile.TarInfo("etc/example-link")
    hardlink.type = tarfile.LNKTYPE
    hardlink.linkname = "etc/example"
    hardlink.uid = 1234
    hardlink.gid = 2345
    hardlink.mtime = 0
    hardlink.mode = 0o644
    archive.addfile(hardlink)
PY

sha() { shasum -a 256 "$1" | awk '{print $1}'; }
size() {
  if [ "$(uname -s)" = Darwin ]; then stat -f '%z' "$1"; else stat -c '%s' "$1"; fi
}

layer_sha="$(sha "$root/layer.tar")"
layer_size="$(size "$root/layer.tar")"
cp "$root/layer.tar" "$layout/blobs/sha256/$layer_sha"

printf '%s\n' \
  '{' \
  '  "architecture": "arm64",' \
  '  "os": "linux",' \
  '  "config": {"Env": ["LANG=C"], "WorkingDir": "/"},' \
  "  \"rootfs\": {\"type\": \"layers\", \"diff_ids\": [\"sha256:$layer_sha\"]}" \
  '}' >"$root/config.json"
config_sha="$(sha "$root/config.json")"
config_size="$(size "$root/config.json")"
cp "$root/config.json" "$layout/blobs/sha256/$config_sha"

printf '%s\n' \
  '{' \
  '  "schemaVersion": 2,' \
  '  "mediaType": "application/vnd.oci.image.manifest.v1+json",' \
  '  "config": {' \
  '    "mediaType": "application/vnd.oci.image.config.v1+json",' \
  "    \"digest\": \"sha256:$config_sha\"," \
  "    \"size\": $config_size" \
  '  },' \
  '  "layers": [' \
  '    {' \
  '      "mediaType": "application/vnd.oci.image.layer.v1.tar",' \
  "      \"digest\": \"sha256:$layer_sha\"," \
  "      \"size\": $layer_size" \
  '    }' \
  '  ]' \
  '}' >"$root/manifest.json"
manifest_sha="$(sha "$root/manifest.json")"
manifest_size="$(size "$root/manifest.json")"
cp "$root/manifest.json" "$layout/blobs/sha256/$manifest_sha"

printf '%s\n' \
  '{"imageLayoutVersion":"1.0.0"}' >"$layout/oci-layout"
printf '%s\n' \
  '{' \
  '  "schemaVersion": 2,' \
  '  "manifests": [' \
  '    {' \
  '      "mediaType": "application/vnd.oci.image.manifest.v1+json",' \
  "      \"digest\": \"sha256:$manifest_sha\"," \
  "      \"size\": $manifest_size," \
  '      "platform": {"os":"linux","architecture":"arm64"}' \
  '    },' \
  '    {' \
  '      "mediaType": "application/vnd.oci.image.manifest.v1+json",' \
  '      "digest": "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",' \
  '      "size": 1,' \
  '      "platform": {"os":"unknown","architecture":"unknown"},' \
  '      "annotations": {' \
  '        "vnd.docker.reference.type": "attestation-manifest",' \
  "        \"vnd.docker.reference.digest\": \"sha256:$manifest_sha\"" \
  '      }' \
  '    }' \
  '  ]' \
  '}' >"$layout/index.json"

"$helper" inspect "$layout" --json >"$root/directory.json"
data_assert "$root/directory.json" '
assert data["schema"] == "maelys.warden.oci-inspection/v1"
assert [m["platform"] for m in data["manifests"]] == ["linux/arm64"]
assert data["manifests"][0]["digest"] == "sha256:" + sys.argv[2]
assert data["manifests"][0]["layerCount"] == 1' "$manifest_sha"

cp -R "$layout" "$root/malformed-attestation"
sed -i.bak 's/attestation-manifest/not-an-attestation/' \
  "$root/malformed-attestation/index.json"
rm "$root/malformed-attestation/index.json.bak"
if "$helper" inspect "$root/malformed-attestation" >/dev/null 2>&1; then
  echo "unrecognized non-runnable OCI descriptor was ignored" >&2
  exit 1
fi

COPYFILE_DISABLE=1 tar -cf "$root/layout.oci.tar" -C "$layout" .
"$helper" inspect "$root/layout.oci.tar" --json >"$root/archive.json"
data_assert "$root/archive.json" 'assert data["manifests"][0]["platform"] == "linux/arm64"'

cp -R "$layout" "$root/tampered"
printf x >>"$root/tampered/blobs/sha256/$config_sha"
if "$helper" inspect "$root/tampered" >/dev/null 2>&1; then
  echo "tampered OCI descriptor was accepted" >&2
  exit 1
fi

cp -R "$layout" "$root/symlinked"
rm "$root/symlinked/index.json"
ln -s ../layout/index.json "$root/symlinked/index.json"
if "$helper" inspect "$root/symlinked" >/dev/null 2>&1; then
  echo "symlinked OCI metadata was accepted" >&2
  exit 1
fi

mkdir -m 0700 "$root/store-a" "$root/store-b"
# A plan never writes: the store stays untouched and the document says so.
"$helper" import "$layout" --store "$root/store-a" --platform linux/arm64 --json \
  >"$root/import-plan.json"
data_assert "$root/import-plan.json" '
assert data["mode"] == "plan" and data["changed"] is False
assert data["precondition"]["artifactExists"] is False
assert data["reference"] == "oci@sha256:" + sys.argv[2]
assert data["artifact"].endswith("/objects/" + sys.argv[2] + "/linux-arm64")' "$manifest_sha"
test ! -e "$root/store-a/objects"
test ! -e "$root/store-a/store.version"
if "$helper" import "$layout" --store "$root/store-a" --dry-run >/dev/null 2>"$root/dry-run.err"; then
  echo "legacy --dry-run was accepted" >&2
  exit 1
fi
grep -Fq 'Add --apply only after reviewing' "$root/dry-run.err"
"$helper" import "$layout" --store "$root/store-a" --platform linux/arm64 --apply --json \
  >"$root/import-a.json"
"$helper" import "$root/layout.oci.tar" --store "$root/store-b" --digest "sha256:$manifest_sha" --apply --json \
  >"$root/import-b.json"
data_assert "$root/import-a.json" '
assert data["mode"] == "apply" and data["changed"] is True
assert data["reference"] == "oci@sha256:" + sys.argv[2] and data["platform"] == "linux/arm64"' "$manifest_sha"
artifact_a="$(data_field "$root/import-a.json" artifact)"
artifact_b="$(data_field "$root/import-b.json" artifact)"
test -f "$artifact_a/root.ext4"
test -f "$artifact_a/rootfs.tar"
test -f "$artifact_a/artifact.json"
test -f "$artifact_a/artifact.seal"
test ! -e "$artifact_a/content"
test ! -e "$artifact_a/descriptors"
test "$(find "$artifact_a" -mindepth 1 -maxdepth 1 | wc -l | tr -d ' ')" = 4
cmp "$artifact_a/root.ext4" "$artifact_b/root.ext4"
cmp "$artifact_a/rootfs.tar" "$artifact_b/rootfs.tar"
mkdir -m 0700 "$root/unpacked-rootfs"
if [ "$(uname -s)" = Linux ]; then
  "$helper" unpack-rootfs "$artifact_a/rootfs.tar" "$root/unpacked-rootfs"
  cmp "$root/unpacked-rootfs/etc/example" "$root/unpacked-rootfs/etc/example-link"
  test "$(cat "$root/unpacked-rootfs/README")" = upper
  test "$(cat "$root/unpacked-rootfs/readme")" = lower
  test "$(cat "$root/unpacked-rootfs/café")" = composed
  test "$(cat "$root/unpacked-rootfs/café")" = decomposed
  first_inode="$(stat -c '%i' "$root/unpacked-rootfs/etc/example")"
  link_inode="$(stat -c '%i' "$root/unpacked-rootfs/etc/example-link")"
  test "$first_inode" = "$link_inode"
else
  if "$helper" unpack-rootfs "$artifact_a/rootfs.tar" "$root/unpacked-rootfs" \
      >"$root/unpack-non-linux.out" 2>"$root/unpack-non-linux.err"; then
    echo "portable root extraction must fail closed outside Linux" >&2
    exit 1
  fi
  grep -Fq 'may only be unpacked on Linux' "$root/unpack-non-linux.err"
  python3 - "$artifact_a/rootfs.tar" <<'PY'
import sys, tarfile

with tarfile.open(sys.argv[1], "r:") as archive:
    assert archive.extractfile("README").read() == b"upper\n"
    assert archive.extractfile("readme").read() == b"lower\n"
    assert archive.extractfile("café").read() == b"composed\n"
    assert archive.extractfile("café").read() == b"decomposed\n"
    assert archive.getmember("etc/example-link").islnk()
    assert archive.getmember("etc/example-link").linkname == "etc/example"
PY
fi
grep -Fq '"schema":"maelys.oci-artifact/v7"' "$artifact_a/artifact.json"
grep -Fq '"rootfsTarFormat":"pax-restricted"' "$artifact_a/artifact.json"
grep -Fq "\"manifestDigest\":\"sha256:$manifest_sha\"" "$artifact_a/artifact.json"
grep -Fq "manifest=sha256:$manifest_sha" "$artifact_a/artifact.seal"
python3 - "$artifact_a/artifact.json" "$artifact_a/artifact.seal" <<'PYTEST'
import json, pathlib, sys
metadata = json.loads(pathlib.Path(sys.argv[1]).read_text())
assert metadata['transformations'] == {'clearedSetuidSetgid': 0}
assert not any('trusted' in key.lower() for key in metadata)
assert pathlib.Path(sys.argv[2]).read_text().startswith('MWOCI/7\n')
PYTEST

test "$(cat "$root/store-a/store.version")" = 2
test "$(cat "$root/store-a/migration.json")" = \
  '{"legacyDerived":[],"schema":"maelys.warden.oci-migration/v1"}'
test -f "$root/store-a/sources/$manifest_sha/linux-arm64/closure.json"
test "$(find "$root/store-a/blobs/sha256" -type f | wc -l | tr -d ' ')" = 3
for immutable in \
    "$root/store-a/store.version" \
    "$root/store-a/migration.json" \
    "$root/store-a/sources/$manifest_sha/linux-arm64/closure.json" \
    "$root/store-a/blobs/sha256/$manifest_sha" \
    "$root/store-a/blobs/sha256/$config_sha" \
    "$root/store-a/blobs/sha256/$layer_sha" \
    "$artifact_a/root.ext4" \
    "$artifact_a/rootfs.tar" \
    "$artifact_a/artifact.json" \
    "$artifact_a/artifact.seal"; do
  if [ "$(uname -s)" = Darwin ]; then
    test "$(stat -f '%Lp' "$immutable")" = 400
  else
    test "$(stat -c '%a' "$immutable")" = 400
  fi
done
"$helper" verify --store "$root/store-a" --json >"$root/verify.json"
data_assert "$root/verify.json" '
assert data["valid"] is True and data["errors"] == [] and data["warnings"] == []
assert data["counts"] == {
    "artifacts": 1, "blobBytes": data["counts"]["blobBytes"],
    "blobs": 3, "closures": 1, "leases": 0,
}'
"$helper" verify --store "$root/store-a" \
  | python3 -c 'import json,sys; assert json.load(sys.stdin)["valid"] is True'
"$helper" verify --store "$root/store-a" --json --compact >"$root/verify-compact.json"
test "$(wc -l <"$root/verify-compact.json" | tr -d ' ')" = 1

# Reconstruct every durable inter-namespace crash state. Blobs-only is a valid
# incomplete import, closure-without-artifact is recoverable, and a v2 artifact
# without either its closure or a migration witness is an integrity error.
cp -R "$root/store-b" "$root/store-boundary-blobs"
rm -rf "$root/store-boundary-blobs/sources/$manifest_sha/linux-arm64"
rm -rf "$root/store-boundary-blobs/objects/$manifest_sha/linux-arm64"
"$helper" verify --store "$root/store-boundary-blobs" --json \
  >"$root/boundary-blobs.json"
data_assert "$root/boundary-blobs.json" '
assert data["errors"] == [] and data["warnings"] == []
assert data["counts"]["blobs"] == 3
assert data["counts"]["closures"] == 0
assert data["counts"]["artifacts"] == 0'

cp -R "$root/store-b" "$root/store-boundary-closure"
rm -rf "$root/store-boundary-closure/objects/$manifest_sha/linux-arm64"
"$helper" verify --store "$root/store-boundary-closure" --json \
  >"$root/boundary-closure.json"
data_assert "$root/boundary-closure.json" '
assert data["errors"] == []
assert len(data["warnings"]) == 1
assert "has no derived artifact" in data["warnings"][0]
assert data["counts"]["closures"] == 1
assert data["counts"]["artifacts"] == 0'

cp -R "$root/store-b" "$root/store-boundary-artifact"
rm -rf "$root/store-boundary-artifact/sources/$manifest_sha/linux-arm64"
# Integrity errors are a completed validation report: exit 2 with the
# findings in data, never an error envelope.
set +e
"$helper" verify --store "$root/store-boundary-artifact" --json \
  >"$root/boundary-artifact.json" 2>"$root/boundary-artifact.err"
verify_status=$?
set -e
test "$verify_status" = 2
test ! -s "$root/boundary-artifact.err"
data_assert "$root/boundary-artifact.json" '
assert data["valid"] is False
assert any("lacks a source closure and migration witness" in e for e in data["errors"])'

cp -R "$root/store-b" "$root/store-migration-tamper"
chmod 0600 "$root/store-migration-tamper/migration.json"
python3 - "$root/store-migration-tamper/migration.json" <<'PY'
import json, pathlib, sys
path = pathlib.Path(sys.argv[1])
document = json.loads(path.read_text(encoding="utf-8"))
document["unexpected"] = True
path.write_text(json.dumps(document, sort_keys=True, separators=(",", ":")) + "\n",
                encoding="utf-8")
PY
chmod 0400 "$root/store-migration-tamper/migration.json"
if "$helper" import "$layout" --store "$root/store-migration-tamper" \
    --platform linux/arm64 --apply >/dev/null 2>&1; then
  echo "OCI import mutated a store with a malformed migration witness" >&2
  exit 1
fi

# Reimporting the exact source is idempotent and never replaces the published
# closure or artifact.
"$helper" import "$layout" --store "$root/store-a" --platform linux/arm64 \
  --apply --json >"$root/repeat.json"
data_assert "$root/repeat.json" '
assert data["mode"] == "apply" and data["changed"] is False
assert data["precondition"]["artifactExists"] is True
assert data["artifact"] == sys.argv[2]' "$artifact_a"
# Without --format the data document itself is printed, indented.
"$helper" import "$layout" --store "$root/store-a" --platform linux/arm64 --apply \
  | python3 -c 'import json,sys; d=json.load(sys.stdin); assert d["changed"] is False and d["artifact"] == sys.argv[1]' "$artifact_a"
"$helper" verify --store "$root/store-a" >/dev/null

# A pre-0.42 derived-only store is inventoried once before any v2 mutation.
mkdir -m 0700 "$root/store-legacy"
mkdir -m 0700 "$root/store-legacy/objects"
mkdir -m 0700 "$root/store-legacy/objects/$manifest_sha"
cp -R "$artifact_a" \
  "$root/store-legacy/objects/$manifest_sha/linux-arm64"
chmod 0600 \
  "$root/store-legacy/objects/$manifest_sha/linux-arm64/artifact.json" \
  "$root/store-legacy/objects/$manifest_sha/linux-arm64/artifact.seal"
"$helper" import "$layout" --store "$root/store-legacy" \
  --platform linux/arm64 --apply >/dev/null
for hardened in \
    "$root/store-legacy/objects/$manifest_sha/linux-arm64/artifact.json" \
    "$root/store-legacy/objects/$manifest_sha/linux-arm64/artifact.seal"; do
  if [ "$(uname -s)" = Darwin ]; then
    test "$(stat -f '%Lp' "$hardened")" = 400
  else
    test "$(stat -c '%a' "$hardened")" = 400
  fi
done
"$helper" remove "legacy@sha256:$manifest_sha" --platform linux/arm64 \
  --store "$root/store-legacy" --apply >/dev/null
python3 - "$root/store-legacy/migration.json" <<'PY'
import json, sys
document = json.load(open(sys.argv[1], encoding="utf-8"))
assert document["schema"] == "maelys.warden.oci-migration/v1"
assert len(document["legacyDerived"]) == 1
entry = document["legacyDerived"][0]
assert entry["platform"] == "linux/arm64"
assert entry["rootDigest"].startswith("sha256:")
assert entry["sealDigest"].startswith("sha256:")
PY
"$helper" verify --store "$root/store-legacy" >/dev/null

chmod 0600 "$root/store-a/blobs/sha256/$config_sha"
printf tamper >>"$root/store-a/blobs/sha256/$config_sha"
chmod 0400 "$root/store-a/blobs/sha256/$config_sha"
if "$helper" verify --store "$root/store-a" >/dev/null 2>&1; then
  echo "OCI store verification accepted a tampered CAS blob" >&2
  exit 1
fi
chmod 0600 "$root/store-a/blobs/sha256/$config_sha"
cp "$layout/blobs/sha256/$config_sha" \
  "$root/store-a/blobs/sha256/$config_sha"
chmod 0400 "$root/store-a/blobs/sha256/$config_sha"

closure_a="$root/store-a/sources/$manifest_sha/linux-arm64/closure.json"
closure_b="$root/store-b/sources/$manifest_sha/linux-arm64/closure.json"
chmod 0444 "$closure_a"
if "$helper" verify --store "$root/store-a" >/dev/null 2>&1; then
  echo "OCI store verification accepted a non-canonical immutable mode" >&2
  exit 1
fi
chmod 0400 "$closure_a"

ln "$root/store-a/blobs/sha256/$config_sha" "$root/config-hardlink-alias"
if "$helper" verify --store "$root/store-a" >/dev/null 2>&1; then
  echo "OCI store verification accepted a multiply-linked CAS blob" >&2
  exit 1
fi
rm -f "$root/config-hardlink-alias"

chmod 0600 "$closure_a"
printf ' ' >>"$closure_a"
chmod 0400 "$closure_a"
if "$helper" verify --store "$root/store-a" >/dev/null 2>&1; then
  echo "OCI store verification accepted a non-canonical closure" >&2
  exit 1
fi
chmod 0600 "$closure_a"
cp "$closure_b" "$closure_a"
chmod 0400 "$closure_a"
printf unexpected >"$(dirname "$closure_a")/extra"
if "$helper" verify --store "$root/store-a" >/dev/null 2>&1; then
  echo "OCI store verification accepted an unknown closure member" >&2
  exit 1
fi
rm "$(dirname "$closure_a")/extra"

chmod 0600 "$artifact_a/artifact.seal"
printf tamper >>"$artifact_a/artifact.seal"
chmod 0400 "$artifact_a/artifact.seal"
if "$helper" verify --store "$root/store-a" >/dev/null 2>&1; then
  echo "OCI store verification accepted a tampered artifact seal" >&2
  exit 1
fi
chmod 0600 "$artifact_a/artifact.seal"
cp "$artifact_b/artifact.seal" "$artifact_a/artifact.seal"
chmod 0400 "$artifact_a/artifact.seal"

chmod 0600 "$artifact_a/root.ext4"
printf tamper >>"$artifact_a/root.ext4"
chmod 0400 "$artifact_a/root.ext4"
if "$helper" verify --store "$root/store-a" >/dev/null 2>&1; then
  echo "OCI store verification accepted a tampered derived root" >&2
  exit 1
fi
chmod 0600 "$artifact_a/root.ext4"
cp "$artifact_b/root.ext4" "$artifact_a/root.ext4"
chmod 0400 "$artifact_a/root.ext4"
"$helper" verify --store "$root/store-a" >/dev/null

mkdir -m 0700 "$root/store-concurrent"
"$helper" import "$layout" --store "$root/store-concurrent" \
  --platform linux/arm64 --apply --json --compact >"$root/concurrent-a.json" &
import_pid_a=$!
"$helper" import "$root/layout.oci.tar" --store "$root/store-concurrent" \
  --digest "sha256:$manifest_sha" --apply --json --compact >"$root/concurrent-b.json" &
import_pid_b=$!
wait "$import_pid_a"
wait "$import_pid_b"
test "$(data_field "$root/concurrent-a.json" artifact)" = \
  "$(data_field "$root/concurrent-b.json" artifact)"
changed_pair="$(data_field "$root/concurrent-a.json" changed)$(data_field "$root/concurrent-b.json" changed)"
test "$changed_pair" = TrueFalse || test "$changed_pair" = FalseTrue
"$helper" verify --store "$root/store-concurrent" --json >"$root/concurrent.json"
data_assert "$root/concurrent.json" '
assert data["errors"] == [] and data["warnings"] == []
assert data["counts"] == {"artifacts": 1, "blobBytes": data["counts"]["blobBytes"], "blobs": 3, "closures": 1, "leases": 0}'

require_e2fsprogs_tool() {
  tool="$1"
  override="$2"
  purpose="$3"
  candidate="$override"
  if [ -z "$candidate" ]; then
    candidate="$(command -v "$tool" 2>/dev/null || true)"
  fi
  if [ -z "$candidate" ] &&
     [ -x "/opt/homebrew/opt/e2fsprogs/sbin/$tool" ]; then
    candidate="/opt/homebrew/opt/e2fsprogs/sbin/$tool"
  fi
  if [ ! -x "$candidate" ]; then
    echo "$tool is required: $purpose did not run" >&2
    return 1
  fi
  printf '%s\n' "$candidate"
}

e2fsck="$(require_e2fsprogs_tool e2fsck "${E2FSCK:-}" \
  'OCI ext4 structural validation')"
"$e2fsck" -fn "$artifact_a/root.ext4"
"$e2fsck" -fn "$artifact_b/root.ext4"

debugfs="$(require_e2fsprogs_tool debugfs "${DEBUGFS:-}" \
  'OCI ext4 semantic assertions')"
"$debugfs" -R 'cat /etc/example' "$artifact_a/root.ext4" 2>/dev/null | grep -Fq hello
"$debugfs" -R 'cat /README' "$artifact_a/root.ext4" 2>/dev/null | grep -Fq upper
"$debugfs" -R 'cat /readme' "$artifact_a/root.ext4" 2>/dev/null | grep -Fq lower
"$debugfs" -R 'cat /café' "$artifact_a/root.ext4" 2>/dev/null | grep -Fq composed
"$debugfs" -R 'cat /café' "$artifact_a/root.ext4" 2>/dev/null | grep -Fq decomposed
"$debugfs" -R 'stat /etc/example' "$artifact_a/root.ext4" 2>/dev/null \
  | grep -Eq 'User:[[:space:]]+1234[[:space:]]+Group:[[:space:]]+2345'
"$debugfs" -R 'cat /maelys-init' "$artifact_a/root.ext4" 2>/dev/null | grep -Fq image
"$debugfs" -R 'cat /maelys-bwrap' "$artifact_a/root.ext4" 2>/dev/null | grep -Fq image
"$debugfs" -R 'stat /workspace' "$artifact_a/root.ext4" 2>&1 | grep -Fq 'File not found'

# Store lifecycle: list, leases, gc plan/apply, remove plan/apply.
"$helper" inspect "$layout" | grep -Fq 'linux/arm64'
mkdir -m 0700 "$root/store-cli"
"$helper" import "$layout" --store "$root/store-cli" --platform linux/arm64 --apply --json \
  >"$root/import-cli.json"
test "$(data_field "$root/import-cli.json" changed)" = True
"$helper" list --store "$root/store-cli" --json >"$root/store-list.json"
data_assert "$root/store-list.json" '
assert data["count"] == 1
artifact = data["records"][0]
assert artifact["reference"] == "oci@" + sys.argv[2]
assert artifact["digest"] == sys.argv[2]
assert artifact["platform"] == "linux/arm64"
assert artifact["metadata"]["schema"] == "maelys.oci-artifact/v7"
assert artifact["metadata"]["rootDigest"].startswith("sha256:")' "sha256:$manifest_sha"
test "$("$helper" list --store "$root/store-cli" --format jsonl | wc -l | tr -d ' ')" = 1
"$helper" list --store "$root/store-absent" --json >"$root/store-absent.json"
data_assert "$root/store-absent.json" 'assert data == {"count": 0, "records": []}'
"$helper" verify --store "$root/store-cli" --json >"$root/store-verify.json"
data_assert "$root/store-verify.json" '
assert data["errors"] == [] and data["warnings"] == []
assert data["counts"]["blobs"] == 3
assert data["counts"]["closures"] == 1
assert data["counts"]["artifacts"] == 1'
root_digest="$(python3 - "$root/store-list.json" <<'PY'
import json, sys
print(json.load(open(sys.argv[1], encoding="utf-8"))["data"]["records"][0]["metadata"]["rootDigest"])
PY
)"
rootfs_tar_digest="$(python3 - "$root/store-list.json" <<'PY'
import json, sys
print(json.load(open(sys.argv[1], encoding="utf-8"))["data"]["records"][0]["metadata"]["rootfsTarDigest"])
PY
)"
# ---- execution leases (maelys.warden.oci-lease/v3) --------------------------------
# A lease is a file whose liveness the kernel answers: its holder keeps an
# exclusive flock on it. lease_document EXPIRES prints one document.
lease_document() {
  printf '{"createdUnixSeconds":0,"expiresUnixSeconds":%s,"liveness":"kernel-lock","manifestDigest":"sha256:%s","platform":"linux/arm64","rootDigest":"%s","rootfsTarDigest":"%s","schema":"maelys.warden.oci-lease/v3"}\n' \
    "$1" "$manifest_sha" "$root_digest" "$rootfs_tar_digest"
}
# wait_for FILE: waits up to five seconds for FILE to appear.
wait_for() {
  for _ in $(seq 1 100); do
    test -e "$1" && return 0
    sleep 0.05
  done
  echo "timed out waiting for $1" >&2
  return 1
}
leases="$root/store-cli/leases"

# An orphan lease that has not expired: verify warns, gc retains, remove refuses.
orphan="$leases/00000000000000000000000000000000.json"
lease_document 9223372036854775807 >"$orphan"
chmod 0400 "$orphan"
"$helper" verify --store "$root/store-cli" --json >"$root/orphan-verify.json"
data_assert "$root/orphan-verify.json" '
assert data["errors"] == [] and data["counts"]["leases"] == 1
assert any("no live holder" in warning for warning in data["warnings"])'
"$helper" gc --grace-seconds 0 --store "$root/store-cli" --json >"$root/orphan-gc.json"
data_assert "$root/orphan-gc.json" '
assert data["valid"] is True and data["candidates"] == []
assert any("retained until its expiry" in warning for warning in data["warnings"])'
set +e
"$helper" remove "local/import@sha256:$manifest_sha" --platform linux/arm64 \
  --store "$root/store-cli" --apply --json --compact >/dev/null 2>"$root/leased-remove.err"
remove_status=$?
set -e
test "$remove_status" = 1
grep -Fq '"code":"PRECONDITION_FAILED"' "$root/leased-remove.err"
test -e "$root/store-cli/objects/$manifest_sha/linux-arm64"
rm -f "$orphan"

# An expired lease held by a live process: the kernel says busy, so gc
# retains it and verify sees a holder; once the holder dies, gc retires it.
held="$leases/11111111111111111111111111111111.json"
lease_document 0 >"$held"
chmod 0400 "$held"
python3 - "$held" "$root/holder.ready" <<'PY' &
import fcntl, os, signal, sys
descriptor = os.open(sys.argv[1], os.O_RDONLY)
fcntl.flock(descriptor, fcntl.LOCK_EX)
open(sys.argv[2], "w").close()
signal.pause()
PY
holder=$!
wait_for "$root/holder.ready"
"$helper" verify --store "$root/store-cli" --json >"$root/held-verify.json"
data_assert "$root/held-verify.json" '
assert data["errors"] == [] and data["warnings"] == [] and data["counts"]["leases"] == 1'
"$helper" gc --grace-seconds 0 --store "$root/store-cli" --json >"$root/held-gc.json"
data_assert "$root/held-gc.json" '
assert data["valid"] is True and data["candidates"] == [] and data["warnings"] == []'
kill "$holder"
wait "$holder" 2>/dev/null || true
"$helper" gc --grace-seconds 0 --store "$root/store-cli" --json \
  >"$root/stale-lease-dry.json"
data_assert "$root/stale-lease-dry.json" '
assert data["mode"] == "plan" and data["changed"] is False and data["valid"] is True
assert [candidate["kind"] for candidate in data["candidates"]] == ["lease"]
assert data["graceSeconds"] == 0 and data["retired"] == 0'
test -e "$held"
"$helper" gc --grace-seconds 0 --store "$root/store-cli" --apply --json >"$root/stale-lease.json"
data_assert "$root/stale-lease.json" 'assert data["mode"] == "apply" and data["changed"] is True and data["retired"] == 1'
test ! -e "$held"

# A lease from an older schema is malformed, not honoured: verify reports it.
legacy="$leases/22222222222222222222222222222222.json"
printf '{"createdUnixSeconds":0,"expiresUnixSeconds":0,"manifestDigest":"sha256:%s","ownerPid":%s,"ownerStart":"legacy","platform":"linux/arm64","rootDigest":"%s","rootfsTarDigest":"%s","schema":"maelys.warden.oci-lease/v2"}\n' \
  "$manifest_sha" "$$" "$root_digest" "$rootfs_tar_digest" >"$legacy"
chmod 0400 "$legacy"
set +e
"$helper" verify --store "$root/store-cli" --json >"$root/legacy-verify.json"
legacy_status=$?
set -e
test "$legacy_status" = 2
python3 - "$root/legacy-verify.json" <<'PY'
import json, sys
data = json.load(open(sys.argv[1], encoding="utf-8"))["data"]
assert data["valid"] is False
assert any("malformed" in error for error in data["errors"]), data
PY
rm -f "$legacy"

# The library protocol: resolve publishes a v3 lease held by a kernel lock,
# gc sees a holder, release retires the lease by identity.
mkfifo "$root/lease.in"
MAELYS_OCI_STORE="$root/store-cli" "$lease_test" "$root/store-cli" \
  "sha256:$manifest_sha" linux/arm64 <"$root/lease.in" >"$root/lease.out" &
lease_pid=$!
exec 3>"$root/lease.in"
for _ in $(seq 1 100); do
  test -s "$root/lease.out" && break
  sleep 0.05
done
library_lease="$(head -n 1 "$root/lease.out")"
test -f "$library_lease"
python3 - "$library_lease" "$manifest_sha" <<'PY'
import json, os, stat, sys
document = json.load(open(sys.argv[1], encoding="utf-8"))
assert document["schema"] == "maelys.warden.oci-lease/v3", document
assert document["liveness"] == "kernel-lock"
assert document["manifestDigest"] == "sha256:" + sys.argv[2]
assert "ownerPid" not in document and "ownerStart" not in document
assert stat.S_IMODE(os.lstat(sys.argv[1]).st_mode) == 0o400
PY
"$helper" verify --store "$root/store-cli" --json >"$root/library-verify.json"
data_assert "$root/library-verify.json" '
assert data["errors"] == [] and data["warnings"] == [] and data["counts"]["leases"] == 1'
"$helper" gc --grace-seconds 0 --store "$root/store-cli" --json >"$root/library-gc.json"
data_assert "$root/library-gc.json" 'assert data["candidates"] == [] and data["warnings"] == []'
set +e
"$helper" remove "local/import@sha256:$manifest_sha" --platform linux/arm64 \
  --store "$root/store-cli" --apply --json --compact >/dev/null 2>"$root/library-remove.err"
remove_status=$?
set -e
test "$remove_status" = 1
grep -Fq '"code":"PRECONDITION_FAILED"' "$root/library-remove.err"
echo release >&3
exec 3>&-
wait "$lease_pid"
test "$(tail -n 1 "$root/lease.out")" = released
test ! -e "$library_lease"
"$helper" verify --store "$root/store-cli" --json >"$root/released-verify.json"
data_assert "$root/released-verify.json" 'assert data["errors"] == [] and data["counts"]["leases"] == 0'
mkdir -m 0700 "$root/store-cli/tmp/import/abandoned-test"
printf partial >"$root/store-cli/tmp/import/abandoned-test/state"
"$helper" verify --store "$root/store-cli" --json >"$root/temporary-warning.json"
data_assert "$root/temporary-warning.json" '
assert data["errors"] == []
assert any("awaits recovery" in warning for warning in data["warnings"])'
"$helper" gc --grace-seconds 0 --store "$root/store-cli" --json \
  >"$root/temporary-dry.json"
data_assert "$root/temporary-dry.json" 'assert [candidate["kind"] for candidate in data["candidates"]] == ["temporary"]'
"$helper" gc --grace-seconds 0 --store "$root/store-cli" --apply >/dev/null
test ! -e "$root/store-cli/tmp/import/abandoned-test"
mkdir -m 0700 "$root/store-symlink" "$root/store-symlink/objects"
ln -s "$root/store-cli/objects/$manifest_sha" \
  "$root/store-symlink/objects/$manifest_sha"
if "$helper" list --store "$root/store-symlink" >/dev/null 2>&1; then
  echo "OCI store listing followed a symlinked digest directory" >&2
  exit 1
fi
if "$helper" remove "local/import@sha256:$manifest_sha" \
    --platform linux/arm64 --store "$root/store-symlink" --apply >/dev/null 2>&1; then
  echo "OCI store removal followed a symlinked digest directory" >&2
  exit 1
fi
"$helper" remove "local/import@sha256:$manifest_sha" --platform linux/arm64 \
  --store "$root/store-cli" --json >"$root/remove-plan.json"
data_assert "$root/remove-plan.json" '
assert data["mode"] == "plan" and data["changed"] is False
assert data["reference"] == "oci@" + sys.argv[2] and data["platform"] == "linux/arm64"' "sha256:$manifest_sha"
test -e "$root/store-cli/objects/$manifest_sha/linux-arm64"
"$helper" remove "local/import@sha256:$manifest_sha" \
  --platform linux/arm64 --store "$root/store-cli" --apply --json >"$root/remove.json"
data_assert "$root/remove.json" 'assert data["mode"] == "apply" and data["changed"] is True' 
test ! -e "$root/store-cli/objects/$manifest_sha/linux-arm64"
test ! -e "$root/store-cli/sources/$manifest_sha/linux-arm64"
"$helper" gc --store "$root/store-cli" --json >"$root/gc-dry.json"
data_assert "$root/gc-dry.json" '
assert data["mode"] == "plan"
assert len(data["candidates"]) == 3
assert data["selectedBytes"] > 0'
test "$(find "$root/store-cli/blobs/sha256" -type f | wc -l | tr -d ' ')" = 3
"$helper" gc --store "$root/store-cli" --apply --json >"$root/gc.json"
data_assert "$root/gc.json" 'assert data["mode"] == "apply" and data["retired"] == 3'
test "$(find "$root/store-cli/blobs/sha256" -type f | wc -l | tr -d ' ')" = 0
"$helper" verify --store "$root/store-cli" >/dev/null
"$helper" list --store "$root/store-cli" --json >"$root/store-empty.json"
data_assert "$root/store-empty.json" 'assert data["count"] == 0'

echo "PASS OCI descriptor closure, deterministic ext4 import and Linux byte-exact names"
