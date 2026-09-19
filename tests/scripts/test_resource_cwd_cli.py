"""Run the real Engine from native Unicode working directories, without a GPU.

The engine must find the nearest assets root, print its path in UTF-8, load its
Lua config and serve a ping. Retain process output even when startup aborts or
times out. This does not claim packaged GPU or physical audio acceptance.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import uuid


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--engine', type=Path, required=True)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    engine = args.engine.resolve(strict=True)
    output = (args.output if args.output is not None else Path(tempfile.gettempdir()) /
              ('caesura-resource-cwd-' + str(uuid.uuid4()))).resolve()
    output.mkdir(parents=True, exist_ok=False)
    print(f'Evidence directory: {output}', flush=True)
    engine_sha = sha256(engine)
    environment = dict(os.environ)
    environment.pop('CAESURA_RESOURCE_ROOT', None)
    records = []
    # The supplementary characters cannot be represented by legacy Windows
    # code pages, including Chinese ACP 936 used by some developer hosts.
    cases = [('ascii', 'game space', False),
             ('unicode', '作品 输出 🎵𠀀', False),
             ('unicode_parent', '作品 输出 🎵𠀀', True)]
    for name, component, nested in cases:
        evidence = output / name
        resource = evidence / component
        (resource / 'assets').mkdir(parents=True)
        (resource / 'scripts/kag').mkdir(parents=True)
        (resource / 'scripts/config.lua').write_text(
            "config={}\nprint('RESOURCE_CWD_CONFIG_LOADED')\n", encoding='utf-8')
        (resource / 'scripts/kag/init.lua').write_text('-- Isolated startup fixture.\n', encoding='utf-8')
        launch = resource / 'nested' / 'launch space' if nested else resource
        launch.mkdir(parents=True, exist_ok=True)
        command = [str(engine), '--headless']
        record = {'name': name, 'argv': command, 'cwd': str(launch),
                  'resource_root': str(resource), 'passed': False, 'timed_out': False}
        try:
            proc = subprocess.run(command, cwd=launch, env=environment,
                                  input=b'{"id":101,"method":"ping"}\n',
                                  capture_output=True, timeout=30)
            stdout, stderr = proc.stdout, proc.stderr
            record['exit_code'] = proc.returncode
        except subprocess.TimeoutExpired as error:
            stdout, stderr = error.stdout or b'', error.stderr or b''
            record['timed_out'] = True
            record['error'] = 'Startup exceeded 30 seconds; subprocess.run killed and reaped its owned process'
        (evidence / 'stdout.log').write_bytes(stdout)
        (evidence / 'stderr.log').write_bytes(stderr)
        record['stdout_sha256'] = sha256(evidence / 'stdout.log')
        record['stderr_sha256'] = sha256(evidence / 'stderr.log')
        replies = []
        for line in stdout.decode('utf-8', errors='replace').splitlines():
            try:
                reply = json.loads(line)
            except ValueError:
                continue
            if isinstance(reply, dict):
                replies.append(reply)
        path_line = ('[main] Working directory: ' + str(resource) + '\n').encode('utf-8')
        record['utf8_resource_path_reported'] = path_line in stderr.replace(b'\r\n', b'\n')
        record['config_loaded'] = b'RESOURCE_CWD_CONFIG_LOADED' in stdout
        record['ping_ok'] = any(item.get('id') == 101 and item.get('result') == 'ok' for item in replies)
        record['passed'] = (not record['timed_out'] and record.get('exit_code') == 0
                            and record['utf8_resource_path_reported']
                            and record['config_loaded'] and record['ping_ok'])
        records.append(record)
        print(f"{'PASS' if record['passed'] else 'FAIL'} {name}", flush=True)
    stable = sha256(engine) == engine_sha
    report = {'schema': 'caesura.resource-cwd-cli-test.v1', 'engine': str(engine),
              'engine_sha256': engine_sha, 'engine_stable': stable, 'cases': records,
              'packaged_gpu': 'NOT_RUN', 'physical_audio': 'NOT_RUN',
              'passed': stable and all(record['passed'] for record in records)}
    (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(f"{sum(record['passed'] for record in records)}/{len(records)} passed", flush=True)
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
