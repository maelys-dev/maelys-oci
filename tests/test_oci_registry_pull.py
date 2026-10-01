#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Physical HTTPS registry gate for the bounded OCI pull command.

Failures are agent-cli/v2 envelopes: exit 1 with the diagnostic on stderr."""

from __future__ import annotations

import base64
import hashlib
import http.server
import io
import json
import os
import pathlib
import ssl
import subprocess
import sys
import tarfile
import tempfile
import threading
import urllib.parse


INDEX_MEDIA = "application/vnd.oci.image.index.v1+json"
MANIFEST_MEDIA = "application/vnd.oci.image.manifest.v1+json"
CONFIG_MEDIA = "application/vnd.oci.image.config.v1+json"
LAYER_MEDIA = "application/vnd.oci.image.layer.v1.tar"
TOKEN = "fixture-pull-token"
BASIC = "Basic " + base64.b64encode(b"fixture:secret").decode("ascii")


def canonical(document: object) -> bytes:
    return json.dumps(document, sort_keys=True, separators=(",", ":")).encode()


def digest(data: bytes) -> str:
    return "sha256:" + hashlib.sha256(data).hexdigest()


def descriptor(data: bytes, media_type: str) -> dict[str, object]:
    return {"digest": digest(data), "mediaType": media_type, "size": len(data)}


def make_layer() -> bytes:
    output = io.BytesIO()
    with tarfile.open(fileobj=output, mode="w", format=tarfile.USTAR_FORMAT) as archive:
        directory = tarfile.TarInfo("etc")
        directory.type = tarfile.DIRTYPE
        directory.mode = 0o755
        directory.mtime = 0
        archive.addfile(directory)
        content = b"pulled through maelys-http\n"
        member = tarfile.TarInfo("etc/registry-proof")
        member.mode = 0o644
        member.mtime = 0
        member.size = len(content)
        archive.addfile(member, io.BytesIO(content))
    return output.getvalue()


def make_fixture() -> tuple[dict[str, bytes], str, str, str, str]:
    layer = make_layer()
    layer_descriptor = descriptor(layer, LAYER_MEDIA)
    assets: dict[str, bytes] = {layer_descriptor["digest"]: layer}  # type: ignore[index]
    manifests: list[dict[str, object]] = []
    arm_manifest_digest = ""
    for architecture in ("arm64", "amd64"):
        config = canonical({
            "architecture": architecture,
            "config": {"Env": ["LANG=C"], "WorkingDir": "/"},
            "os": "linux",
            "rootfs": {"diff_ids": [digest(layer)], "type": "layers"},
        })
        config_descriptor = descriptor(config, CONFIG_MEDIA)
        assets[config_descriptor["digest"]] = config  # type: ignore[index]
        manifest = canonical({
            "config": config_descriptor,
            "layers": [layer_descriptor],
            "mediaType": MANIFEST_MEDIA,
            "schemaVersion": 2,
        })
        manifest_descriptor = descriptor(manifest, MANIFEST_MEDIA)
        manifest_descriptor["platform"] = {
            "architecture": architecture,
            "os": "linux",
        }
        assets[manifest_descriptor["digest"]] = manifest  # type: ignore[index]
        manifests.append(manifest_descriptor)
        if architecture == "arm64":
            arm_manifest_digest = str(manifest_descriptor["digest"])
    index = canonical({
        "manifests": manifests,
        "mediaType": INDEX_MEDIA,
        "schemaVersion": 2,
    })
    index_digest = digest(index)
    assets[index_digest] = index
    mismatched_descriptor = dict(manifests[0])
    mismatched_descriptor["platform"] = {
        "architecture": "amd64",
        "os": "linux",
    }
    mismatched_index = canonical({
        "manifests": [mismatched_descriptor],
        "mediaType": INDEX_MEDIA,
        "schemaVersion": 2,
    })
    mismatched_index_digest = digest(mismatched_index)
    assets[mismatched_index_digest] = mismatched_index
    oversized = canonical({
        "config": descriptor(canonical({"architecture": "arm64", "os": "linux"}),
                             CONFIG_MEDIA),
        "layers": [
            {
                "digest": digest(f"oversized-{index}".encode()),
                "mediaType": LAYER_MEDIA,
                "size": 16 * 1024 * 1024 * 1024,
            }
            for index in range(4)
        ],
        "mediaType": MANIFEST_MEDIA,
        "schemaVersion": 2,
    })
    oversized_digest = digest(oversized)
    assets[oversized_digest] = oversized
    # Tags a resolution reads: the index itself, a body of an unknown media
    # type, and a manifest that does not parse. A tag names no content, so the
    # key is the tag and the digest is whatever its bytes produce.
    assets["1.0"] = index
    # A valid index other than the one a lying registry claims.
    assets["other"] = mismatched_index
    assets["unknown-type"] = canonical({"greeting": "not a manifest"})
    assets["broken"] = canonical({
        "schemaVersion": 2,
        "mediaType": "application/vnd.oci.image.manifest.v1+json",
        "config": {"mediaType": "application/vnd.oci.image.config.v1+json"},
    })
    return (assets, index_digest, arm_manifest_digest, oversized_digest,
            mismatched_index_digest)


class Registry(http.server.ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, assets: dict[str, bytes], index_digest: str):
        super().__init__(("127.0.0.1", 0), Handler)
        self.assets = assets
        self.index_digest = index_digest
        self.cdn_authority: str | None = None
        self.requests: list[tuple[str, str | None, int]] = []
        self._connection_lock = threading.Lock()
        self._connection_ids: dict[int, int] = {}
        self._next_connection_id = 1

    def get_request(self):  # type: ignore[no-untyped-def]
        connection, address = super().get_request()
        with self._connection_lock:
            self._connection_ids[id(connection)] = self._next_connection_id
            self._next_connection_id += 1
        return connection, address

    def connection_id(self, connection: object) -> int:
        with self._connection_lock:
            return self._connection_ids[id(connection)]


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server: Registry
    expire_on_next_request = False

    def log_message(self, _format: str, *args: object) -> None:
        del args

    def reply(self, status: int, data: bytes = b"", *, media: str | None = None,
              digest_header: str | None = None, chunked: bool = False,
              extra: dict[str, str] | None = None) -> None:
        self.send_response(status)
        if media:
            self.send_header("Content-Type", media)
        if digest_header:
            self.send_header("Docker-Content-Digest", digest_header)
        if extra:
            for name, value in extra.items():
                self.send_header(name, value)
        if chunked:
            self.send_header("Transfer-Encoding", "chunked")
        else:
            self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        if chunked:
            for offset in range(0, len(data), 97):
                part = data[offset:offset + 97]
                self.wfile.write(f"{len(part):x}\r\n".encode() + part + b"\r\n")
            self.wfile.write(b"0\r\nRegistry-Proof: complete\r\n\r\n")
        elif data:
            self.wfile.write(data)

    def do_GET(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler API
        parsed = urllib.parse.urlsplit(self.path)
        authorization = self.headers.get("Authorization")
        self.server.requests.append((
            parsed.path, authorization, self.server.connection_id(self.connection)
        ))
        # Close after the client's idle probe and request write, making the
        # keep-alive race deterministic instead of depending on TCP timing.
        if self.expire_on_next_request or parsed.path.startswith("/v2/unavailable/tool/"):
            self.close_connection = True
            return
        if parsed.path == "/token":
            query = urllib.parse.parse_qs(parsed.query, strict_parsing=True)
            if authorization != BASIC or query != {
                    "scope": ["repository:example/tool:pull"],
                    "service": ["fixture"],
            }:
                self.reply(401)
                return
            self.reply(200, canonical({"token": TOKEN}), media="application/json")
            return
        # A host that is not a registry API endpoint: it answers a manifest
        # request with a redirect to another host, which serves a web page,
        # nothing at all, or (a front for a real registry) the manifest.
        redirected = parsed.path.split("/")[2] if parsed.path.startswith("/v2/") else ""
        if redirected in ("moved", "moved-gone", "mirror"):
            if self.server.cdn_authority:
                self.reply(302, extra={
                    "Location": f"https://{self.server.cdn_authority}{parsed.path}"
                })
                return
            if redirected == "moved":
                self.reply(200, b"<!doctype html><title>Not a registry</title>",
                           media="text/html; charset=utf-8")
                return
            if redirected == "moved-gone":
                self.reply(404, b"<html>gone</html>", media="text/html")
                return
        if parsed.path.startswith("/v2/html/tool/") and authorization == f"Bearer {TOKEN}":
            self.reply(200, b"<html>maintenance</html>", media="text/html")
            return
        if (parsed.path.startswith("/v2/foreign-realm/tool/") and
                authorization != f"Bearer {TOKEN}"):
            self.reply(401, extra={
                "WWW-Authenticate": (
                    f'Bearer realm="https://{self.server.cdn_authority}/token",'
                    f'service="fixture",scope="repository:{"/".join(parsed.path.split("/")[2:-2])}:pull"'
                )
            })
            return
        if (parsed.path.startswith("/v2/") and redirected != "mirror" and
                authorization != f"Bearer {TOKEN}"):
            port = self.server.server_address[1]
            self.reply(401, extra={
                "WWW-Authenticate": (
                    f'Bearer realm="https://localhost:{port}/token",'
                    f'service="fixture",scope="repository:{"/".join(parsed.path.split("/")[2:-2])}:pull"'
                )
            })
            return
        components = parsed.path.split("/")
        if len(components) >= 6 and components[1] == "v2":
            repository = "/".join(components[2:-2])
            operation = components[-2]
            requested = urllib.parse.unquote(components[-1])
            data = self.server.assets.get(requested)
            if data is None:
                self.reply(404)
                return
            if operation == "manifests":
                media = str(json.loads(data).get("mediaType", ""))
                if requested == self.server.index_digest and repository == "race/tool":
                    self.expire_on_next_request = True
                if requested == self.server.index_digest and repository == "close/tool":
                    self.reply(200, data, media=media + "; charset=utf-8",
                               digest_header=requested,
                               extra={"Connection": "close"})
                    self.close_connection = True
                    return
                if requested == self.server.index_digest and repository == "silent/tool":
                    self.reply(200, data, media=media + "; charset=utf-8",
                               digest_header=requested)
                    self.close_connection = True
                    return
                if not requested.startswith("sha256:"):
                    if repository == "lying/tool":
                        # A registry may claim any digest; the resolution
                        # believes the bytes it received, not this header.
                        self.reply(200, data, media=media or "application/json",
                                   digest_header=self.server.index_digest)
                        return
                    if repository == "claiming/tool":
                        self.reply(200, data, media=media,
                                   digest_header=digest(data))
                        return
                    self.reply(200, data,
                               media=media or "application/json")
                    return
                self.reply(200, data, media=media + "; charset=utf-8",
                           digest_header=requested)
                return
            if operation == "blobs":
                if repository == "truncated/tool" and len(data) > 1024:
                    self.send_response(200)
                    self.send_header("Content-Length", str(len(data)))
                    self.end_headers()
                    self.wfile.write(data[:len(data) // 2])
                    self.close_connection = True
                    return
                if repository == "downgrade/tool" and len(data) > 1024:
                    self.reply(307, extra={"Location": "http://127.0.0.1/rejected"})
                    return
                if repository == "tampered/tool" and len(data) > 1024:
                    data = data[:-1] + bytes((data[-1] ^ 1,))
                if len(data) > 1024 and repository == "example/tool":
                    self.reply(307, extra={
                        "Location": f"https://{self.server.cdn_authority}/cdn/{requested}"
                    })
                    return
                self.reply(200, data, media="application/octet-stream",
                           digest_header=requested)
                return
        if len(components) == 3 and components[1] == "cdn":
            requested = urllib.parse.unquote(components[2])
            data = self.server.assets.get(requested)
            if data is not None:
                self.reply(200, data, media="application/octet-stream",
                           digest_header=requested, chunked=True)
                return
        self.reply(404)


def run(command: list[str], *, env: dict[str, str], expected: int = 0) -> subprocess.CompletedProcess[str]:
    command = [*command, "--json", "--compact", "--non-interactive"]
    completed = subprocess.run(command, env=env, text=True, capture_output=True,
                               timeout=90, check=False)
    if completed.returncode != expected:
        raise AssertionError(
            f"command returned {completed.returncode}, expected {expected}: {command}\n"
            f"stdout={completed.stdout}\nstderr={completed.stderr}"
        )
    return completed


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: test_oci_registry_pull.py PULLER MATERIALIZER", file=sys.stderr)
        return 64
    puller = os.path.abspath(sys.argv[1])
    materializer = os.path.abspath(sys.argv[2])
    (assets, index_digest, arm_manifest_digest, oversized_digest,
     mismatched_index_digest) = make_fixture()
    with tempfile.TemporaryDirectory(prefix="maelys-oci-pull-") as directory:
        root = pathlib.Path(directory)
        fixture = pathlib.Path(__file__).with_name("fixtures") / "oci-registry"
        ca_cert = fixture / "ca-cert.pem"
        key = fixture / "server-key.pem"
        cert = fixture / "server-cert.pem"
        cdn = Registry(assets, index_digest)
        server = Registry(assets, index_digest)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cert, key)
        cdn.socket = context.wrap_socket(cdn.socket, server_side=True)
        server.socket = context.wrap_socket(server.socket, server_side=True)
        server.cdn_authority = f"localhost:{cdn.server_address[1]}"
        cdn_thread = threading.Thread(target=cdn.serve_forever, daemon=True)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        cdn_thread.start()
        thread.start()
        try:
            port = server.server_address[1]
            authority = f"localhost:{port}"
            docker_config = root / "docker-config.json"
            docker_config.write_text(json.dumps({
                "auths": {authority: {
                    "auth": base64.b64encode(b"fixture:secret").decode("ascii")
                }}
            }), encoding="utf-8")
            docker_config.chmod(0o600)
            env = os.environ.copy()
            store = root / "store"
            store.mkdir(mode=0o700)
            # resolve: a tag becomes the digest of the bytes received.
            resolution = json.loads(run([
                puller, "resolve", f"{authority}/example/tool:1.0",
                "--ca-file", str(ca_cert), "--docker-config", str(docker_config),
            ], env=env).stdout)["data"]
            assert resolution["digest"] == index_digest, resolution
            assert resolution["tag"] == "1.0", resolution
            offered = {entry["platform"] for entry in resolution["platforms"]}
            assert offered == {"linux/amd64", "linux/arm64"}, resolution
            assert all(entry["supported"] for entry in resolution["platforms"]), resolution
            assert not any("variant" in entry for entry in resolution["platforms"]), \
                "a descriptor without a variant must still be reported"

            # The token endpoint only issues tokens for example/tool. A
            # refused exchange names the endpoint, the scope and the status,
            # is an access failure that no retry can change, and reveals
            # neither the credentials sent nor a token.
            for command, target, extra in (
                ("resolve", f"{authority}/lying/tool:broken", []),
                ("stat-remote", f"{authority}/lying/tool@{index_digest}",
                 ["--platform", "linux/arm64"]),
                ("pull", f"{authority}/lying/tool@{index_digest}",
                 ["--platform", "linux/arm64",
                  "--store", str(root / "refused")]),
            ):
                before = len(server.requests)
                refused = run([
                    puller, command, target, *extra, "--ca-file", str(ca_cert),
                    "--docker-config", str(docker_config),
                ], env=env, expected=1)
                assert refused.stdout == "", (command, refused.stdout)
                error = json.loads(refused.stderr)["error"]
                assert error["code"] == "ACCESS_DENIED", (command, error)
                assert error["message"] == (
                    f"token endpoint {authority} refused the configured "
                    "credentials for scope repository:lying/tool:pull with "
                    "HTTP 401"), (command, error)
                assert "--docker-config" in error["hint"], (command, error)
                assert "refused again" in error["hint"], (command, error)
                assert "Retry" not in error["hint"], (command, error)
                assert "verify" not in error["hint"], (command, error)
                for secret in (TOKEN, "fixture:secret", BASIC,
                               BASIC.split()[-1]):
                    assert secret not in refused.stderr, (command, secret)
                # One challenge, one refused exchange, and no second attempt.
                assert [path for path, _a, _c in server.requests[before:]] \
                    [-1] == "/token", (command, server.requests[before:])
            assert not (root / "refused").exists() or \
                not any((root / "refused").rglob("root.ext4"))

            # A digest claimed by the registry is never believed. The bearer
            # token is given directly: the fixture's token endpoint serves
            # example/tool only, and a refused token exchange would fail
            # these resolutions before any header is read.
            resolve_token = root / "resolve-token"
            resolve_token.write_text(TOKEN + "\n", encoding="ascii")
            resolve_token.chmod(0o600)
            lied = run([puller, "resolve", f"{authority}/lying/tool:broken",
                        "--ca-file", str(ca_cert), "--token-file", str(resolve_token)],
                       env=env, expected=1)
            error = json.loads(lied.stderr)["error"]
            assert error["code"] == "PROTOCOL_FAILED", error
            assert "digest the received bytes do not produce" in error["message"], error
            # A tag served with a lying header for bytes that are a valid
            # index fails on the header alone.
            lied = run([puller, "resolve", f"{authority}/lying/tool:other",
                        "--ca-file", str(ca_cert), "--token-file", str(resolve_token)],
                       env=env, expected=1)
            error = json.loads(lied.stderr)["error"]
            assert error["code"] == "PROTOCOL_FAILED", error
            assert "digest the received bytes do not produce" in error["message"], error
            # The header is optional and adds nothing: the resolution above
            # came without one, and one that agrees reports the same digest.
            claimed = json.loads(run([
                puller, "resolve", f"{authority}/claiming/tool:1.0",
                "--ca-file", str(ca_cert), "--token-file", str(resolve_token),
            ], env=env).stdout)["data"]
            assert claimed["digest"] == resolution["digest"] == index_digest, claimed
            # Neither an index nor an image manifest.
            run([puller, "resolve", f"{authority}/example/tool:unknown-type",
                 "--ca-file", str(ca_cert), "--docker-config", str(docker_config)],
                env=env, expected=1)
            # An image manifest that does not parse.
            run([puller, "resolve", f"{authority}/example/tool:broken",
                 "--ca-file", str(ca_cert), "--docker-config", str(docker_config)],
                env=env, expected=1)

            # The resolutions above opened their own connections; the pull
            # scenario counts its own, so it starts from a clean record.
            server.requests.clear()

            reference = f"{authority}/example/tool@{index_digest}"
            result = run([
                puller, "pull", reference, "--platform", "linux/arm64",
                "--store", str(store), "--ca-file", str(ca_cert),
                "--docker-config", str(docker_config),
            ], env=env)
            expected_reference = f"{authority}/example/tool@{arm_manifest_digest}"
            pulled = json.loads(result.stdout)["data"]
            assert pulled["reference"] == expected_reference
            assert pulled["platform"] == "linux/arm64" and pulled["changed"] is True
            assert pulled["manifestDigest"] == arm_manifest_digest
            assert TOKEN not in result.stdout + result.stderr
            assert "fixture:secret" not in result.stdout + result.stderr
            assert any(path.startswith("/cdn/") and authorization is None
                       for path, authorization, _connection in cdn.requests)
            registry_connection_ids = {
                connection for path, _authorization, connection in server.requests
                if path == "/token" or path.startswith("/v2/example/tool/")
            }
            assert len(registry_connection_ids) == 1, server.requests
            artifact = pathlib.Path(pulled["artifact"])
            assert (artifact / "root.ext4").is_file()
            run([materializer, "verify", "--store", str(store)], env=env)

            foreign_requests = len(cdn.requests)
            foreign_realm = run([
                puller, "pull",
                f"{authority}/foreign-realm/tool@{index_digest}",
                "--platform", "linux/arm64",
                "--store", str(root / "foreign-realm"),
                "--ca-file", str(ca_cert),
                "--docker-config", str(docker_config),
            ], env=env, expected=1)
            assert "cross-authority Bearer realm" in foreign_realm.stderr, foreign_realm.stderr
            assert len(cdn.requests) == foreign_requests, cdn.requests

            token_file = root / "token"
            token_file.write_text(TOKEN + "\n", encoding="ascii")
            token_file.chmod(0o600)
            second = run([
                puller, "pull", reference, "--platform", "linux/arm64",
                "--store", str(store), "--ca-file", str(ca_cert),
                "--token-file", str(token_file),
            ], env=env)
            second_pulled = json.loads(second.stdout)["data"]
            assert second_pulled["artifact"] == pulled["artifact"]
            assert second_pulled["changed"] is False

            for repository in ("close/tool", "silent/tool", "race/tool"):
                before = len(server.requests)
                redial_store = root / repository.split("/")[0]
                redial_store.mkdir(mode=0o700)
                redial = run([
                    puller, "pull", f"{authority}/{repository}@{index_digest}",
                    "--platform", "linux/arm64",
                    "--store", str(redial_store),
                    "--ca-file", str(ca_cert), "--token-file", str(token_file),
                ], env=env)
                assert json.loads(redial.stdout)["data"]["reference"] == (
                    f"{authority}/{repository}@{arm_manifest_digest}"
                )
                connection_ids = [
                    connection
                    for path, _authorization, connection in server.requests[before:]
                    if path.startswith(f"/v2/{repository}/")
                ]
                assert len(connection_ids) >= 2, server.requests[before:]
                if repository == "race/tool":
                    assert len(connection_ids) == 5, server.requests[before:]
                    assert connection_ids[0] == connection_ids[1], server.requests[before:]
                    assert connection_ids[1] != connection_ids[2], server.requests[before:]
                else:
                    assert connection_ids[0] != connection_ids[1], server.requests[before:]

            before = len(server.requests)
            unavailable = run([
                puller, "pull", f"{authority}/unavailable/tool@{index_digest}",
                "--platform", "linux/arm64", "--store", str(root / "unavailable"),
                "--ca-file", str(ca_cert), "--token-file", str(token_file),
            ], env=env, expected=1)
            assert json.loads(unavailable.stderr)["error"]["code"] == "IO_FAILED"
            unavailable_connections = [
                connection for path, _authorization, connection in server.requests[before:]
                if path.startswith("/v2/unavailable/tool/")
            ]
            assert len(unavailable_connections) == 2, server.requests[before:]
            assert len(set(unavailable_connections)) == 2, server.requests[before:]

            before = len(server.requests)
            truncated = run([
                puller, "pull", f"{authority}/truncated/tool@{index_digest}",
                "--platform", "linux/arm64", "--store", str(root / "truncated"),
                "--ca-file", str(ca_cert), "--token-file", str(token_file),
            ], env=env, expected=1)
            assert "digest/size verification" in truncated.stderr
            truncated_paths = [path for path, _authorization, _connection
                               in server.requests[before:]]
            assert len(truncated_paths) == 4, server.requests[before:]
            assert len(set(truncated_paths)) == 4, server.requests[before:]

            token_file.chmod(0o644)
            exposed = run([
                puller, "pull", reference, "--platform", "linux/arm64",
                "--store", str(store), "--ca-file", str(ca_cert),
                "--token-file", str(token_file),
            ], env=env, expected=1)
            assert "private regular file" in exposed.stderr
            token_file.chmod(0o600)

            token_link = root / "token-link"
            token_link.symlink_to(token_file)
            linked = run([
                puller, "pull", reference, "--platform", "linux/arm64",
                "--store", str(store), "--ca-file", str(ca_cert),
                "--token-file", str(token_link),
            ], env=env, expected=1)
            assert "private regular file" in linked.stderr

            ambiguous = run([
                puller, "pull", reference, "--store", str(root / "ambiguous"),
                "--ca-file", str(ca_cert), "--token-file", str(token_file),
            ], env=env, expected=1)
            assert "unambiguous --platform" in ambiguous.stderr

            mismatched = run([
                puller, "pull",
                f"{authority}/mismatched/tool@{mismatched_index_digest}",
                "--store", str(root / "mismatched"),
                "--ca-file", str(ca_cert), "--token-file", str(token_file),
            ], env=env, expected=1)
            assert "disagrees with its index platform" in mismatched.stderr, \
                mismatched.stderr

            for repository, message in (
                ("tampered/tool", "digest/size verification"),
                ("downgrade/tool", "digest/size verification"),
            ):
                failed = run([
                    puller, "pull", f"{authority}/{repository}@{index_digest}",
                    "--platform", "linux/arm64", "--store", str(root / repository.split('/')[0]),
                    "--ca-file", str(ca_cert), "--token-file", str(token_file),
                ], env=env, expected=1)
                assert message in failed.stderr

            # A manifest request redirected to another host that serves a web
            # page (docker.io answers this way): the diagnostic names the
            # host and the Content-Type received, the code stays stable, the
            # hint says what to change, and the bearer token sent to the
            # registry never follows the redirect.
            cdn_authority = server.cdn_authority
            for command, target, extra in (
                ("resolve", f"{authority}/moved/tool:1.0", []),
                ("stat-remote", f"{authority}/moved/tool@{index_digest}",
                 ["--platform", "linux/arm64"]),
                ("pull", f"{authority}/moved/tool@{index_digest}",
                 ["--platform", "linux/arm64", "--store", str(root / "moved")]),
            ):
                before = len(server.requests)
                foreign = len(cdn.requests)
                moved = run([
                    puller, command, target, *extra,
                    "--ca-file", str(ca_cert), "--token-file", str(token_file),
                ], env=env, expected=1)
                assert moved.stdout == "", (command, moved.stdout)
                error = json.loads(moved.stderr)["error"]
                assert error["code"] == "PROTOCOL_FAILED", (command, error)
                assert (f"registry answered text/html from {cdn_authority} "
                        "after a redirect") in error["message"], (command, error)
                assert f"{authority} does not serve the registry API" in \
                    error["message"], (command, error)
                assert "disagrees" not in error["message"], (command, error)
                assert "does not match its descriptor" not in error["message"], \
                    (command, error)
                assert "registry API host" in error["hint"], (command, error)
                assert "Retry" not in error["hint"], (command, error)
                assert TOKEN not in moved.stderr, command
                sent = server.requests[before:]
                assert sent and all(authorization == f"Bearer {TOKEN}"
                                    for _path, authorization, _c in sent), sent
                followed = cdn.requests[foreign:]
                assert [path.split("/")[2] for path, _a, _c in followed] == \
                    ["moved"], (command, followed)
                assert all(authorization is None
                           for _path, authorization, _c in followed), followed
            assert not (root / "moved").exists() or \
                not any((root / "moved").rglob("root.ext4"))

            # The other host answers no page at all: the status is named.
            gone = run([
                puller, "resolve", f"{authority}/moved-gone/tool:1.0",
                "--ca-file", str(ca_cert), "--token-file", str(token_file),
            ], env=env, expected=1)
            error = json.loads(gone.stderr)["error"]
            assert error["code"] == "PROTOCOL_FAILED", error
            assert f"registry answered HTTP 404 from {cdn_authority} after a redirect" \
                in error["message"], error

            # The same page served by the requested host itself: no redirect
            # is claimed, the Content-Type is still named.
            page = run([
                puller, "resolve", f"{authority}/html/tool:1.0",
                "--ca-file", str(ca_cert), "--token-file", str(token_file),
            ], env=env, expected=1)
            error = json.loads(page.stderr)["error"]
            assert error["code"] == "PROTOCOL_FAILED", error
            assert f"registry {authority} answered text/html to a manifest request" \
                in error["message"], error
            assert "redirect" not in error["message"], error

            # A cross-host redirect to a host that does serve the manifest
            # (registry.k8s.io fronts its backends this way) is still
            # followed, without the credentials of the first host.
            foreign = len(cdn.requests)
            mirrored = json.loads(run([
                puller, "resolve", f"{authority}/mirror/tool:1.0",
                "--ca-file", str(ca_cert), "--token-file", str(token_file),
            ], env=env).stdout)["data"]
            assert mirrored["digest"] == index_digest, mirrored
            assert mirrored["registry"] == authority, mirrored
            assert [authorization for _p, authorization, _c
                    in cdn.requests[foreign:]] == [None], cdn.requests[foreign:]

            helper_config = root / "helper-config.json"
            helper_config.write_text(json.dumps({"credsStore": "desktop"}),
                                     encoding="utf-8")
            helper_config.chmod(0o600)
            helper = run([
                puller, "pull", reference, "--platform", "linux/arm64",
                "--store", str(root / "helper"), "--ca-file", str(ca_cert),
                "--docker-config", str(helper_config),
            ], env=env, expected=1)
            assert "credential helper is intentionally unsupported" in helper.stderr

            oversized = run([
                puller, "pull", f"{authority}/oversized/tool@{oversized_digest}",
                "--platform", "linux/arm64", "--store", str(root / "oversized"),
                "--ca-file", str(ca_cert), "--token-file", str(token_file),
            ], env=env, expected=1)
            assert "structurally invalid" in oversized.stderr
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=5)
            cdn.shutdown()
            cdn.server_close()
            cdn_thread.join(timeout=5)
    print("PASS bounded authenticated OCI registry pull")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
