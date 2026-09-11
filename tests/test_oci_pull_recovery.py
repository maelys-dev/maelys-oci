#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Offline adversarial HTTPS, concurrency and crash recovery integration gate."""
from __future__ import annotations
import base64
import copy
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
import time
import urllib.parse
from test_oci_registry_pull import (
    Registry, Handler, INDEX_MEDIA, MANIFEST_MEDIA, CONFIG_MEDIA, LAYER_MEDIA,
    canonical, digest, descriptor, make_fixture, run,
)


def layer(name: str, content: bytes) -> bytes:
    output = io.BytesIO()
    with tarfile.open(fileobj=output, mode='w', format=tarfile.USTAR_FORMAT) as archive:
        item = tarfile.TarInfo(name)
        item.size = len(content)
        item.mode = 0o644
        archive.addfile(item, io.BytesIO(content))
    return output.getvalue()


class RecoveryRegistry(Registry):
    def handle_error(self, request, client_address):
        if isinstance(sys.exception(), (ConnectionResetError, BrokenPipeError, ssl.SSLError)):
            return  # Deliberate disconnect/cancellation scenarios.
        super().handle_error(request, client_address)


class Faults(Handler):
    def do_GET(self):  # noqa: N802
        parts = self.path.split('?')[0].split('/')
        mode = parts[2] if len(parts) > 2 else ''
        self.server.requests.append((self.path, self.headers.get('Authorization'),
                                     self.server.connection_id(self.connection)))
        assert self.headers.get('Range') is None, 'open pull must restart partial blobs from zero'
        if parts == ['', 'token']:
            query = urllib.parse.parse_qs(urllib.parse.urlsplit(self.path).query)
            selected = query.get('mode', [''])[0]
            assert self.headers.get('Authorization') is None
            assert query.get('scope') == [f'repository:{selected}/tool:pull']
            assert query.get('service') == ['fixture']
            token_body = canonical({'token': 'anonymous-bearer'})
            media = 'application/json'
            if selected == 'bearer-empty':
                token_body = b'{}'
            elif selected == 'bearer-disagree':
                token_body = canonical({'token': 'secret-one', 'access_token': 'secret-two'})
            elif selected == 'bearer-big':
                token_body = b'x' * (65536 + 1)
            elif selected == 'bearer-media':
                media = 'text/plain'
            self.reply(200, token_body, media=media)
            return
        if len(parts) < 6:
            self.reply(404)
            return
        if mode.startswith('bearer') and self.headers.get('Authorization') != 'Bearer anonymous-bearer':
            self.reply(401, extra={'WWW-Authenticate':
                f'Bearer realm="https://localhost:{self.server.server_port}/token?mode={mode}",'
                f'service="fixture",scope="repository:{mode}/tool:pull"'})
            return
        requested = parts[-1]
        body = self.server.assets.get(requested)
        if body is None:
            self.reply(404)
            return
        if mode.startswith('challenge'):
            challenges = {
                'challenge-duplicate': 'Bearer realm="https://localhost/token",realm="https://localhost/token",service="fixture"',
                'challenge-http': 'Bearer realm="http://localhost/token",service="fixture"',
                'challenge-scope': 'Bearer realm="https://localhost/token",service="fixture",scope="repository:other/tool:pull,push"',
                'challenge-query': 'Bearer realm="https://localhost/token?scope=repository:other:pull",service="fixture"',
                'challenge-escaped': 'Bearer realm="https://localhost/token",service="fi\\"xture"',
            }
            self.reply(401, extra={'WWW-Authenticate': challenges[mode]})
            return
        if mode == 'redirect-loop':
            self.reply(307, extra={'Location': f'https://localhost:{self.server.server_port}{self.path}'})
            return
        if mode == 'headers':
            self.reply(200, body, extra={f'X-Limit-{i}': 'value' for i in range(70)})
            return
        if mode == 'media':
            self.reply(200, body, media='application/unknown')
            return
        if mode == 'digest-header':
            self.reply(200, body, media=MANIFEST_MEDIA, digest_header='sha256:' + '0' * 64)
            return
        if mode == 'tamper-manifest':
            self.reply(200, body + b' ', media=MANIFEST_MEDIA)
            return
        if mode == 'encoding':
            self.reply(200, body, media=MANIFEST_MEDIA, extra={'Content-Encoding': 'gzip'})
            return
        is_layer = parts[-2] == 'blobs' and len(body) > 1024
        if is_layer and mode in ('short', 'extra'):
            changed = body[:-1] if mode == 'short' else body + b'x'
            self.reply(200, changed, media='application/octet-stream')
            return
        if is_layer and mode == 'disconnect':
            self.send_response(200)
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body[:1000])
            self.wfile.flush()
            self.close_connection = True
            return
        if mode == 'delay':
            time.sleep(0.18)
        if mode == 'crash' and requested == self.server.pause_digest:
            self.send_response(200)
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body[:1024])
            self.wfile.flush()
            self.server.paused.set()
            self.server.resume.wait(15)
            try:
                self.wfile.write(body[1024:])
            except (BrokenPipeError, ConnectionResetError, ssl.SSLError):
                pass
            return
        media = json.loads(body).get('mediaType', MANIFEST_MEDIA) if parts[-2] == 'manifests' else 'application/octet-stream'
        try:
            self.reply(200, body, media=media, digest_header=requested)
        except (BrokenPipeError, ConnectionResetError, ssl.SSLError):
            pass


def main():
    binary = str(pathlib.Path(sys.argv[1]).resolve())
    assets, index, arm, _, _ = make_fixture()
    with tempfile.TemporaryDirectory(prefix='maelys-oci-recovery-') as temporary:
        root = pathlib.Path(temporary).resolve()
        server = RecoveryRegistry(assets, index)
        server.RequestHandlerClass = Faults
        server.paused, server.resume = threading.Event(), threading.Event()
        server.pause_digest = ''
        fixture = pathlib.Path(__file__).parent / 'fixtures' / 'oci-registry'
        ca = str((fixture / 'ca-cert.pem').resolve())
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(fixture / 'server-cert.pem', fixture / 'server-key.pem')
        server.socket = context.wrap_socket(server.socket, server_side=True)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        env = os.environ.copy()
        env['HOME'] = str(root)
        env.pop('DOCKER_CONFIG', None)
        authority = f'localhost:{server.server_port}'

        def command(mode='anonymous', wanted=arm, store=None, platform=None, extra=()):
            cmd = [binary, 'pull', f'{authority}/{mode}/tool@{wanted}',
                   '--store', str(store or root / mode), '--ca-file', ca]
            if platform:
                cmd += ['--platform', platform]
            return cmd + list(extra)

        def pull(mode='anonymous', wanted=arm, store=None, platform=None, extra=(), expected=0):
            return run(command(mode, wanted, store, platform, extra), env=env, expected=expected)

        def add(document):
            body = canonical(document)
            identifier = digest(body)
            assets[identifier] = body
            return identifier

        try:
            run([binary, 'describe', '--summary'], env=env)
            for action in ('pull', 'verify', 'gc', 'remove'):
                run([binary, 'describe', action], env=env)
            if len(sys.argv) == 3:
                external = subprocess.run([sys.argv[2], str(root / 'public-api'),
                    f'{authority}/api/tool@{arm}', ca], env=env, text=True, capture_output=True, timeout=90)
                assert external.returncode == 0, external.stderr
                assert json.loads(external.stdout)['manifestDigest'] == arm
            first = json.loads(pull().stdout)['data']
            assert first['blobCount'] == 3 and first['downloadedBlobs'] == 3
            assert not first['cacheHit'] and first['cachedBlobs'] == 0
            before = len(server.requests)
            second = json.loads(pull().stdout)['data']
            assert second['cacheHit'] and second['downloadedBlobs'] == 0 and second['cachedBlobs'] == 3
            assert len(server.requests) == before, 'direct digest hit must use verified local metadata too'
            assert second['artifactDigest'] == first['artifactDigest'] and not second['changed']
            run([binary, 'verify', '--store', str(root / 'anonymous')], env=env)
            # Both acquisition and local import enforce the image config contract.
            config_changes = [
                lambda c: c.pop('rootfs'),
                lambda c: c['rootfs'].update(type='foreign'),
                lambda c: c['rootfs'].update(diff_ids=[]),
                lambda c: c['rootfs'].update(diff_ids=['sha256:short']),
                lambda c: c['rootfs'].update(diff_ids=[digest(b'other tar')]),
                lambda c: c.update(os='windows'),
                lambda c: c.update(architecture='riscv64'),
                lambda c: c.update(variant='v9'),
            ]
            for i, change in enumerate(config_changes):
                manifest = json.loads(assets[arm])
                config = json.loads(assets[manifest['config']['digest']])
                change(config)
                config_id = add(config)
                manifest['config'] = descriptor(assets[config_id], CONFIG_MEDIA)
                selected = add(manifest)
                rejected_store = root / f'config-contract-{i}'
                pull('anonymous', selected, store=rejected_store, expected=1)
                assert not list(rejected_store.glob('objects/*/*/artifact.seal'))
                assert not list(rejected_store.glob('sources/*/*/closure.json'))
                assert not list(rejected_store.glob('tmp/import/*'))
            assert json.loads(pull('bearer').stdout)['data']['manifestDigest'] == arm
            for mode in ('bearer-empty', 'bearer-disagree', 'bearer-big', 'bearer-media'):
                failed = pull(mode, expected=1)
                assert 'secret-one' not in failed.stdout + failed.stderr
                assert 'secret-two' not in failed.stdout + failed.stderr
            multi = json.loads(pull('amd64', index, platform='linux/amd64').stdout)['data']
            assert multi['platform'] == 'linux/amd64' and multi['manifestDigest'] != arm
            nested = add({'schemaVersion': 2, 'mediaType': INDEX_MEDIA,
                          'manifests': [descriptor(assets[index], INDEX_MEDIA)]})
            assert json.loads(pull('nested', nested, platform='linux/arm64').stdout)['data']['manifestDigest'] == arm
            amd_descriptor = json.loads(assets[index])['manifests'][1]
            inner = add({'schemaVersion': 2, 'mediaType': INDEX_MEDIA, 'manifests': [amd_descriptor]})
            wrong_parent = descriptor(assets[inner], INDEX_MEDIA)
            wrong_parent['platform'] = {'os': 'linux', 'architecture': 'arm64'}
            outer = add({'schemaVersion': 2, 'mediaType': INDEX_MEDIA, 'manifests': [wrong_parent]})
            pull('nested-platform-mismatch', outer, expected=1)
            # All rejected responses must leave the artifact namespace empty.
            for mode in ('challenge-duplicate', 'challenge-http', 'challenge-scope', 'challenge-query',
                         'challenge-escaped', 'redirect-loop', 'headers', 'media', 'digest-header',
                         'tamper-manifest', 'encoding', 'short', 'extra', 'disconnect'):
                failure = pull(mode, expected=1)
                assert json.loads(failure.stderr)['error']['code'] in ('PROTOCOL_FAILED', 'ACCESS_DENIED', 'IO_FAILED'), (mode, failure.stderr)
                assert not list((root / mode / 'objects').rglob('artifact.seal'))
            for mode, wanted, platform in [('ambiguous', index, None), ('missing', arm, 'linux/amd64')]:
                pull(mode, wanted, platform=platform, expected=1)
            variant = copy.deepcopy(json.loads(assets[index]))
            for item in variant['manifests']:
                item['platform']['variant'] = 'v9'
            pull('variant', add(variant), platform='linux/arm64', expected=1)
            deep = nested
            for _ in range(9):
                deep = add({'schemaVersion': 2, 'mediaType': INDEX_MEDIA,
                            'manifests': [descriptor(assets[deep], INDEX_MEDIA)]})
            pull('depth', deep, platform='linux/arm64', expected=1)
            malformed = copy.deepcopy(json.loads(assets[arm]))
            malformed['layers'][0]['size'] += 1
            pull('descriptor-size', add(malformed), expected=1)
            started = time.monotonic()
            pull('delay', extra=('--timeout-ms', '300'), expected=1)
            assert time.monotonic() - started < 2, 'deadline must cover several exchanges cumulatively'
            token = root / 'token'
            token.write_text('private-token')
            token.chmod(0o600)
            config = root / 'config.json'
            for body, mode in [(b'{}', 0o644), (b'{', 0o600), (b'x' * (1024 * 1024 + 1), 0o600),
                               (canonical({'auths': {authority: {'auth': '!!!!'}}}), 0o600),
                               (canonical({'auths': {authority: {'auth': base64.b64encode(b'without-colon').decode()}}}), 0o600)]:
                config.write_bytes(body)
                config.chmod(mode)
                pull('config', extra=('--docker-config', str(config)), expected=1)
            link = root / 'linked-parent'
            link.symlink_to(root, target_is_directory=True)
            pull('ancestor', store=link / 'new-store', expected=1)
            pull('credential-ancestor', extra=('--token-file', str(link / 'token')), expected=1)
            # Existing immutable paths are never overwritten, even to repair them.
            blob = root / 'anonymous' / 'blobs' / 'sha256' / arm[7:]
            original = blob.read_bytes()
            blob.chmod(0o600)
            blob.write_bytes(b'corruption')
            blob.chmod(0o400)
            pull(store=root / 'anonymous', expected=1)
            assert blob.read_bytes() == b'corruption'
            blob.chmod(0o600)
            blob.write_bytes(original)
            blob.chmod(0o400)
            blob.unlink()
            os.mkfifo(blob, mode=0o400)
            started = time.monotonic()
            pull(store=root / 'anonymous', expected=1)
            assert time.monotonic() - started < 2, 'an untrusted FIFO must never block a CAS reader'
            blob.unlink()
            blob.write_bytes(original)
            blob.chmod(0o400)
            # The same manifest lock serializes complete operations, not parallel layers.
            concurrent_store = root / 'concurrent'
            cmd = command('concurrent', store=concurrent_store) + ['--format', 'json', '--non-interactive']
            processes = [subprocess.Popen(cmd, env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE) for _ in range(2)]
            results = []
            for process in processes:
                stdout, stderr = process.communicate(timeout=90)
                assert process.returncode == 0, stderr
                results.append(json.loads(stdout)['data'])
            assert sorted(r['downloadedBlobs'] for r in results) == [0, 3]
            assert len({r['artifactDigest'] for r in results}) == 1
            # Crash after the first blob and during the second: complete CAS survives;
            # the incomplete body is private, pinned acquisition roots protect the CAS.
            complete = layer('complete', b'first verified object\n')
            partial = layer('partial', b'x' * 100_000)
            for body in (complete, partial):
                assets[digest(body)] = body
            manifest = copy.deepcopy(json.loads(assets[arm]))
            manifest['layers'] = [descriptor(complete, LAYER_MEDIA), descriptor(partial, LAYER_MEDIA)]
            config = json.loads(assets[manifest['config']['digest']])
            config['rootfs']['diff_ids'] = [digest(complete), digest(partial)]
            config_bytes = canonical(config)
            assets[digest(config_bytes)] = config_bytes
            manifest['config'] = descriptor(config_bytes, CONFIG_MEDIA)
            crash_digest = add(manifest)
            server.pause_digest = digest(partial)
            crash_store = root / 'crash'
            cmd = command('crash', crash_digest, crash_store) + ['--format', 'json', '--non-interactive']
            process = subprocess.Popen(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            assert server.paused.wait(10), 'partial layer not reached'
            assert (crash_store / 'blobs' / 'sha256' / digest(complete)[7:]).is_file()
            assert not (crash_store / 'blobs' / 'sha256' / digest(partial)[7:]).exists()
            leases = list((crash_store / 'leases').glob('*.json'))
            assert len(leases) == 1 and json.loads(leases[0].read_text())['schema'] == 'maelys.oci-acquisition-lease/v1'
            gc_cmd = [binary, 'gc', '--store', str(crash_store), '--grace-seconds', '0', '--apply', '--format', 'json', '--non-interactive']
            collector = subprocess.Popen(gc_cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            remove_cmd = [binary, 'remove', crash_digest, '--platform', 'linux/arm64',
                '--store', str(crash_store), '--apply', '--format', 'json', '--non-interactive']
            remover = subprocess.Popen(remove_cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            time.sleep(0.15)
            assert remover.poll() is None, 'remove must wait behind the active store lock'
            assert collector.poll() is None, 'GC must wait behind the active store lock'
            process.kill()
            process.communicate(timeout=5)
            server.resume.set()
            remove_out, remove_error = remover.communicate(timeout=15)
            assert remover.returncode == 1, (remove_out, remove_error)
            assert json.loads(remove_error)['error']['code'] == 'NOT_FOUND', remove_error
            stdout, stderr = collector.communicate(timeout=15)
            assert collector.returncode == 0, (collector.returncode, stdout.decode(), stderr.decode())
            assert (crash_store / 'blobs' / 'sha256' / digest(complete)[7:]).exists()
            before = len(server.requests)
            resumed = json.loads(pull('recovery', crash_digest, crash_store).stdout)['data']
            assert resumed['downloadedBlobs'] == 1 and resumed['cachedBlobs'] == 3
            requests = [path for path, _, _ in server.requests[before:]]
            assert len(requests) == 1 and requests[0].endswith(digest(partial)), requests
            run([binary, 'verify', '--store', str(crash_store)], env=env)
            # A dead, expired lease can be retired. Its shape remains public/canonical.
            stale = json.loads(leases[0].read_text())
            stale['createdUnixSeconds'], stale['expiresUnixSeconds'] = 1, 2
            leases[0].chmod(0o600)
            leases[0].write_bytes(canonical(stale) + b'\n')
            leases[0].chmod(0o400)
            run([binary, 'gc', '--store', str(crash_store), '--grace-seconds', '0', '--apply'], env=env)
            assert not list((crash_store / 'leases').iterdir())
            run([binary, 'verify', '--store', str(crash_store)], env=env)
            print('PASS anonymous, platforms, nested indexes, adversarial HTTPS, global deadline, CAS reuse, concurrent pull and crash/GC recovery')
        finally:
            server.resume.set()
            server.shutdown()
            server.server_close()
            thread.join(timeout=5)


if __name__ == '__main__':
    main()
