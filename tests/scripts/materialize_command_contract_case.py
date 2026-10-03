"""Materialize one reviewed contract scene into an existing PRIVATE runtime.

No engine launch, asset download, source repository modification, or overwrite.
The caller supplies the allowed private root and exact reviewed input digests.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re


def digest(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    for name in ('manifest', 'manifest-sha256', 'driver', 'driver-sha256', 'case', 'runtime', 'allowed-root'):
        parser.add_argument('--' + name, required=True)
    args = parser.parse_args()
    allowed = Path(args.allowed_root).resolve(strict=True)
    runtime = Path(args.runtime).resolve(strict=True)
    if runtime == allowed or not runtime.is_relative_to(allowed):
        raise ValueError('Runtime must be a private child of the explicit allowed root')
    current = runtime
    while current != allowed:
        if (current / '.git').exists():
            raise ValueError('Refusing to materialize into a source checkout')
        current = current.parent
    manifest_bytes = Path(args.manifest).read_bytes()
    driver_bytes = Path(args.driver).read_bytes()
    if digest(manifest_bytes) != args.manifest_sha256 or digest(driver_bytes) != args.driver_sha256:
        raise ValueError('Reviewed manifest/driver digest mismatch')
    manifest = json.loads(manifest_bytes)
    matches = [c for c in manifest['cases'] if c['id'] == args.case]
    if len(matches) != 1:
        raise ValueError('Exactly one authored case is required')
    case = matches[0]
    if not re.fullmatch(r'[a-z0-9_-]+', case['id']) or case['status'] != 'READY_FOR_NATIVE_EXECUTION':
        raise ValueError('Case is not ready')
    if case['scene_path'] != 'assets/script/contracts/' + case['id'] + '.ks':
        raise ValueError('Unexpected scene path')
    known_assets = {asset['path'] for asset in manifest['assets']}
    if not set(case['required_assets']).issubset(known_assets):
        raise ValueError('Required asset has no source byte lock')
    for asset in manifest['assets']:
        if asset['path'] not in case['required_assets']:
            continue
        path = runtime / asset['path']
        if not path.resolve(strict=True).is_relative_to(runtime) or path.is_symlink():
            raise ValueError('Asset escapes runtime or is a link')
        content = path.read_bytes()
        if len(content) != asset['bytes'] or digest(content) != asset['sha256']:
            raise ValueError('Actual asset differs: ' + asset['path'])
    entry_relative = 'scripts/contract-entry-' + case['id'] + '.lua'
    driver_relative = 'tests/scripts/command_contract_host_driver.lua'
    selected = json.dumps({'manifest_sha256': args.manifest_sha256, 'case': case}, ensure_ascii=False)
    if ']==]' in selected:
        raise ValueError('Unsupported control delimiter in selected request')
    entry = ('package.path = "tests/scripts/?.lua;" .. package.path\n'
             'local json = require("capability_json")\n'
             'local request = assert(json.decode([==[' + selected + ']==]))\n'
             'require("command_contract_host_driver").start(request)\n').encode('utf-8')
    outputs = {case['scene_path']: case['source'].encode('utf-8'), entry_relative: entry}
    auxiliary = case.get('auxiliary_scenes', [])
    if not isinstance(auxiliary, list) or len(auxiliary) > 8:
        raise ValueError('Auxiliary scene count invalid')
    for scene in auxiliary:
        if not isinstance(scene, dict) or set(scene) != {'path', 'source'}:
            raise ValueError('Auxiliary scene fields invalid')
        relative, source = scene['path'], scene['source']
        prefix = 'assets/script/contracts/' + case['id'] + '-'
        if not isinstance(relative, str) or not relative.startswith(prefix) or not re.fullmatch(r'[a-z0-9_-]+\.ks', relative[len(prefix):]):
            raise ValueError('Auxiliary scene path outside case namespace')
        if not isinstance(source, str) or len(source.encode('utf-8')) > 65536 or relative in outputs:
            raise ValueError('Auxiliary scene content or duplicate invalid')
        outputs[relative] = source.encode('utf-8')
    installed_driver = runtime / driver_relative
    if installed_driver.exists():
        if installed_driver.is_symlink() or installed_driver.read_bytes() != driver_bytes:
            raise ValueError('Existing driver differs')
    else:
        outputs[driver_relative] = driver_bytes
    for relative in outputs:
        target = runtime / relative
        if target.exists() or target.is_symlink():
            raise ValueError('Refusing existing output: ' + relative)
        for parent in target.parents:
            if parent == runtime:
                break
            if parent.is_symlink():
                raise ValueError('Linked output parent')
    receipt_path = runtime / ('contract-materialization-' + case['id'] + '.json')
    if receipt_path.exists():
        raise ValueError('Existing materialization receipt')
    files = []
    for relative, data in outputs.items():
        path = runtime / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open('xb') as stream:
            stream.write(data)
        files.append({'path': relative, 'bytes': len(data), 'sha256': digest(data)})
    receipt = {'schema': 1, 'status': 'MATERIALIZED_NOT_EXECUTED', 'case_id': case['id'],
               'manifest_sha256': args.manifest_sha256, 'driver_sha256': args.driver_sha256,
               'entry_script': entry_relative, 'runtime': str(runtime), 'files': files,
               'engine_launched': False, 'source_identity_and_process_deadline_owned_by_parent': True}
    with receipt_path.open('x', encoding='utf-8') as stream:
        json.dump(receipt, stream, ensure_ascii=False, indent=2)
        stream.write('\n')
    print(json.dumps(receipt, ensure_ascii=False))


if __name__ == '__main__':
    main()
