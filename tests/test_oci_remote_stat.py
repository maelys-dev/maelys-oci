#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Remote metadata: physical HTTPS requests, schema, local parity and no store I/O."""
import base64
import copy
import json
import os
import pathlib
import ssl
import sys
import tempfile
import threading
import time

from schema_assert import assert_schema
from test_oci_inspection import image, layout
from test_oci_registry_pull import (
    Handler, INDEX_MEDIA, MANIFEST_MEDIA, CONFIG_MEDIA, TOKEN,
    canonical, descriptor, digest, make_fixture, run,
)
from test_oci_pull_recovery import Faults, RecoveryRegistry


class MetadataHandler(Faults):
    def do_GET(self):
        path = self.path.split('?')[0]
        # Exercise the existing Basic -> Bearer fixture and foreign CDN too.
        if path.startswith(('/v2/example/', '/cdn/')) or path == '/token' and '?' in self.path and 'mode=' not in self.path:
            Handler.do_GET(self)
            return
        parts = path.split('/')
        if len(parts) == 6 and parts[-2] == 'manifests' and not parts[-1].startswith('sha256:'):
            self.server.requests.append((path, self.headers.get('Authorization'),
                                         self.server.connection_id(self.connection)))
            body = self.server.assets[parts[-1]]
            self.reply(200, body, media=json.loads(body)['mediaType'], digest_header=digest(body))
            return
        if len(parts) == 6 and parts[2] in ('tamper-config', 'config-header') and parts[-2] == 'blobs':
            self.server.requests.append((path, self.headers.get('Authorization'),
                                         self.server.connection_id(self.connection)))
            body = self.server.assets[parts[-1]]
            if parts[2] == 'tamper-config':
                body += b' '
            self.reply(200, body, media='application/octet-stream',
                       digest_header=digest(b'lie') if parts[2] == 'config-header' else parts[-1])
            return
        super().do_GET()


def snapshot(root):
    result = {}
    for path in [root, *sorted(root.rglob('*'))]:
        st = path.lstat()
        result[str(path.relative_to(root))] = (st.st_mode, st.st_ino, st.st_size,
            st.st_mtime_ns, st.st_ctime_ns, digest(path.read_bytes()) if path.is_file() else None)
    return result


def main():
    binary = str(pathlib.Path(sys.argv[1]).resolve())
    assets, multi, arm, _, mismatch = make_fixture()
    fixture = pathlib.Path(__file__).parent / 'fixtures' / 'oci-registry'
    ca = str((fixture / 'ca-cert.pem').resolve())
    servers, threads = [], []
    with tempfile.TemporaryDirectory(prefix='maelys-oci-remote-stat-') as temporary:
        root = pathlib.Path(temporary).resolve()
        env = dict(os.environ, HOME=str(root), XDG_DATA_HOME=str(root / 'xdg'),
                   MAELYS_OCI_STORE=str(root / 'absent-store'))
        env.pop('DOCKER_CONFIG', None)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(fixture / 'server-cert.pem', fixture / 'server-key.pem')
        for _ in range(2):
            server = RecoveryRegistry(assets, multi)
            server.RequestHandlerClass = MetadataHandler
            server.socket = context.wrap_socket(server.socket, server_side=True)
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            servers.append(server)
            threads.append(thread)
            thread.start()
        server, cdn = servers
        authority = f'localhost:{server.server_port}'
        server.cdn_authority = f'localhost:{cdn.server_port}'

        def call(command, *arguments, code=0):
            result = run([binary, command, *map(str, arguments)], env=env, expected=code)
            assert not (result.stderr if code == 0 else result.stdout), result
            envelope = json.loads(result.stdout if code == 0 else result.stderr)
            assert envelope['ok'] == (code == 0)
            return envelope['data'] if code == 0 else envelope['error']

        def add(document):
            body = canonical(document)
            assets[digest(body)] = body
            return digest(body)

        def index(children):
            return add({'schemaVersion': 2, 'mediaType': INDEX_MEDIA, 'manifests': children})

        def describe(identifier):
            return call('describe', identifier)['commands'][0]

        def metadata(wanted=arm, mode='anonymous', platform=None, extra=(), code=0):
            start = [len(s.requests) for s in servers]
            args = [f'{authority}/{mode}/tool@{wanted}', '--ca-file', ca]
            if platform:
                args += ['--platform', platform]
            result = call('stat-remote', *args, *extra, code=code)
            requests = [entry for s, n in zip(servers, start) for entry in s.requests[n:]]
            assert not any(path.rsplit('/', 1)[-1] in layer_digests for path, _, _ in requests), requests
            assert not (root / 'absent-store').exists()
            assert not (root / 'xdg').exists()
            if code == 0:
                assert_schema(result, contract['outputSchema'])
                assert result['reference'] == args[0]
                assert result['registry'] == authority and result['repository'] == f'{mode}/tool'
                assert len(result['manifests']) == 1
                selected = result['manifests'][0]
                assert result['verification'] == {
                    'scope': 'metadata-only', 'referenceDigest': wanted,
                    'manifestDigest': selected['digest'], 'configDigest': selected['configDigest'],
                    'layersVerified': False, 'diffIdsVerified': False, 'materialized': False}
            return result, requests

        try:
            assert 'stat-remote' in {c['id'] for c in call('describe', '--summary')['commands']}
            contract = describe('stat-remote')
            local_contract = describe('stat')
            for name in ('resolve', 'pull'):
                describe(name)
            assert contract['effect'] == 'read'
            assert contract['outputSchema']['properties']['manifests']['items'] == local_contract['outputSchema']['properties']['manifests']['items']
            opts = {o['long']: o for o in contract['input']['options']}
            assert set(opts) == {'--platform', '--ca-file', '--token-file', '--docker-config', '--timeout-ms'}
            assert opts['--platform']['argument']['choices'] == ['linux/arm64', 'linux/amd64']
            assert 'default' not in opts['--platform']
            assert opts['--timeout-ms']['default'] == '300000'
            assert opts['--token-file']['conflictsWith'] == ['--docker-config']
            # A real populated store, including seals and leases, is left intact.
            existing = root / 'existing-store'
            call('pull', f'{authority}/anonymous/tool@{arm}', '--store', existing, '--ca-file', ca)
            store_before = snapshot(existing)
            layer_digests = {layer['digest'] for blob in list(assets.values())
                             if blob.startswith(b'{') for layer in json.loads(blob).get('layers', [])}
            # Rich local and remote results must be identical, field for field.
            local = root / 'layout'
            rich, _, layers = image(local, changes={'variant': 'v8', 'author': 'fixture ' + 'x' * 1024})
            layout(local, [rich])
            for path in (local / 'blobs/sha256').iterdir():
                assets['sha256:' + path.name] = path.read_bytes()
            layer_digests.update(layer['digest'] for layer in layers)
            local_report = call('stat', local)
            assert_schema(local_report, local_contract['outputSchema'])
            # No layer can be downloaded, even if a future regression tries.
            for identifier in layer_digests:
                assets.pop(identifier, None)
            report, requests = metadata(rich['digest'])
            assert report['manifests'] == local_report['manifests']
            assert len(requests) == 2 and '/blobs/' in requests[1][0], requests
            # Prove validation is active for nested and verification fields.
            for mutate in (lambda r: r['manifests'][0]['config'].update(Unexpected=True),
                           lambda r: r['verification'].update(layersVerified=True),
                           lambda r: r['manifests'][0]['layers'][0].update(diffId='sha256:bad')):
                invalid = copy.deepcopy(report)
                mutate(invalid)
                try:
                    assert_schema(invalid, contract['outputSchema'])
                except AssertionError:
                    pass
                else:
                    raise AssertionError('schema accepted corrupted report')
            env['MAELYS_OCI_STORE'] = str(existing)
            # One direct manifest and one selected leaf behind an index.
            report, requests = metadata()
            assert report['manifests'][0]['digest'] == arm and len(requests) == 2
            report, requests = metadata(multi, platform='linux/amd64')
            assert report['manifests'][0]['platform'] == 'linux/amd64' and len(requests) == 3
            assert metadata(multi, code=1)[0]['code'] == 'PRECONDITION_FAILED'
            assert metadata(platform='linux/amd64', code=1)[0]['code'] == 'PRECONDITION_FAILED'
            assert metadata(mismatch, code=1)[0]['code'] == 'PROTOCOL_FAILED'
            # Diamond and repeated direct references name only one distinct image.
            leaf = descriptor(assets[arm], MANIFEST_MEDIA)
            repeated = index([leaf, leaf])
            nested = index([descriptor(assets[repeated], INDEX_MEDIA), leaf])
            report, requests = metadata(nested)
            assert report['manifests'][0]['digest'] == arm and len(requests) == 8
            # Repetition must never bypass descriptor/config validation.
            for field, value in [('size', leaf['size'] + 1), ('mediaType', INDEX_MEDIA),
                                 ('platform', {'os': 'linux', 'architecture': 'amd64'})]:
                bad = dict(leaf, **{field: value})
                assert metadata(index([leaf, bad]), code=1)[0]['code'] == 'PROTOCOL_FAILED'
            changed = json.loads(assets[arm])
            changed['annotations'] = {'different': 'same platform'}
            other = add(changed)
            ambiguous = index([leaf, descriptor(assets[other], MANIFEST_MEDIA)])
            assert metadata(ambiguous, platform='linux/arm64', code=1)[0]['code'] == 'PRECONDITION_FAILED'
            # resolve deliberately reads just the top level, including for tags
            # that name nested indexes without a platform on their descriptor.
            assets['nested'] = assets[nested]
            before = len(server.requests)
            resolution = call('resolve', f'{authority}/anonymous/tool:nested', '--ca-file', ca)
            assert resolution['digest'] == nested and resolution['platforms'] == []
            assert len(server.requests) - before == 1
            assert 'nested indexes are not traversed' in describe('resolve')['outputSchema']['properties']['platforms']['description']
            # Eight indexes may precede the manifest, but a ninth is refused.
            deep = arm
            for _ in range(8):
                deep = index([descriptor(assets[deep], MANIFEST_MEDIA if deep == arm else INDEX_MEDIA)])
            assert metadata(deep)[0]['manifests'][0]['digest'] == arm
            assert metadata(index([descriptor(assets[deep], INDEX_MEDIA)]), code=1)[0]['code'] == 'PROTOCOL_FAILED'
            assert metadata(index([leaf] * 1025), code=1)[0]['code'] == 'PROTOCOL_FAILED'
            skipped = dict(leaf, platform={'os': 'windows', 'architecture': 'amd64'})
            wide = index([leaf] + [skipped] * 511)
            assert metadata(index([descriptor(assets[wide], INDEX_MEDIA)] * 2), code=1)[0]['code'] == 'PROTOCOL_FAILED'
            # Authentication adds requests; the deadline covers their sum.
            report, requests = metadata(mode='bearer')
            assert len(requests) == 4 and any(p.startswith('/token?') for p, _, _ in requests)
            credentials = root / 'config.json'
            credentials.write_bytes(canonical({'auths': {authority: {'auth': base64.b64encode(b'fixture:secret').decode()}}}))
            credentials.chmod(0o600)
            report, requests = metadata(rich['digest'], mode='example', extra=('--docker-config', str(credentials)))
            # The rich config also takes a cross-authority HTTPS redirect.
            assert len(requests) == 5, requests
            redirected = [entry for entry in requests if entry[0].startswith('/cdn/')]
            assert len(redirected) == 1 and redirected[0][1] is None, redirected
            token = root / 'token'
            token.write_text(TOKEN)
            token.chmod(0o600)
            report, requests = metadata(mode='example', extra=('--token-file', str(token)))
            assert len(requests) == 2 and all(auth == f'Bearer {TOKEN}' for _, auth, _ in requests)
            for mode in ('digest-header', 'tamper-manifest', 'tamper-config', 'config-header', 'media', 'redirect-loop'):
                assert metadata(mode=mode, code=1)[0]['code'] in ('PROTOCOL_FAILED', 'IO_FAILED')
            for config_change in (lambda c: c.update(architecture='riscv64'),
                                  lambda c: c['rootfs'].update(diff_ids=[]),
                                  lambda c: c['config'].update(Env='not an array')):
                changed = json.loads(assets[arm])
                cfg = json.loads(assets[changed['config']['digest']])
                config_change(cfg)
                cfg_id = add(cfg)
                changed['config'] = descriptor(assets[cfg_id], CONFIG_MEDIA)
                assert metadata(add(changed), code=1)[0]['code'] == 'PROTOCOL_FAILED'
            wrong_size = json.loads(assets[arm])
            wrong_size['config']['size'] += 1
            assert metadata(add(wrong_size), code=1)[0]['code'] == 'PROTOCOL_FAILED'
            # A different but syntactically valid DiffID remains a declaration.
            changed = json.loads(assets[arm])
            cfg = json.loads(assets[changed['config']['digest']])
            cfg['rootfs']['diff_ids'] = [digest(b'not the layer bytes')]
            cfg_id = add(cfg)
            changed['config'] = descriptor(assets[cfg_id], CONFIG_MEDIA)
            assert metadata(add(changed))[0]['manifests'][0]['layers'][0]['diffId'] == cfg['rootfs']['diff_ids'][0]
            # Each input is below 8 MiB, but their aggregate exceeds the report cap.
            cfg['config']['Env'] = ['x' * (8 * 1024 * 1024 - 512)]
            cfg_id = add(cfg)
            changed['config'] = descriptor(assets[cfg_id], CONFIG_MEDIA)
            huge = add(changed)
            assert len(assets[cfg_id]) < 8 * 1024 * 1024 < len(assets[cfg_id]) + len(assets[huge])
            assert metadata(huge, code=1)[0]['code'] == 'PROTOCOL_FAILED'
            started = time.monotonic()
            assert metadata(mode='delay', extra=('--timeout-ms', '300'), code=1)[0]['code'] == 'IO_FAILED'
            assert time.monotonic() - started < 2
            # CLI refusals must precede any network request.
            before = len(server.requests)
            immutable = f'{authority}/anonymous/tool@{arm}'
            for extra in (['--store', str(existing)], ['--apply'], ['--expect-root', arm],
                          ['--platform', 'linux/riscv64'], ['--timeout-ms', '0'],
                          ['--token-file', str(token), '--docker-config', str(credentials)],
                          ['--platform', 'linux/arm64', '--platform', 'linux/arm64']):
                assert call('stat-remote', immutable, *extra, code=1)['code'] == 'VALIDATION_FAILED'
            for reference in (arm, f'{authority}/anonymous/tool:latest', 'invalid'):
                assert call('stat-remote', reference, code=1)['code'] == 'VALIDATION_FAILED'
            assert call('stat-remote', code=1)['code'] == 'VALIDATION_FAILED'
            assert len(server.requests) == before
            assert snapshot(existing) == store_before
            assert not (root / 'absent-store').exists() and not (root / 'xdg').exists()
            print('PASS remote metadata schema/local parity, distinct selection, nested bounds, auth/redirect request counts, zero layers, unchanged stores and deadline')
        finally:
            for server in servers:
                server.shutdown()
                server.server_close()
            for thread in threads:
                thread.join(timeout=5)


if __name__ == '__main__':
    main()
