"""Exercise the actual simulation-clock CLI and reject invalid input before init.

The isolated headless fixture checks selection and argument handling, not GPU or
PCM output. Actual owner-loop timing has C++ coverage; the unchanged packaged
demo separately exercises real GPU rendering and SoLoud software mixing.
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


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--engine', type=Path, required=True)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    engine = args.engine.resolve(strict=True)
    output = (args.output or Path(tempfile.gettempdir()) / ('caesura-fixed-step-' + str(uuid.uuid4()))).resolve()
    output.mkdir(parents=True, exist_ok=False)
    print(f'Evidence directory: {output}', flush=True)
    before = sha(engine)
    cases = [('realtime_default', [], 0, None)]
    for value in (1, 16, 250):
        cases.append((f'fixed_{value}', ['--fixed-step-ms', str(value)], value, None))
    cases.append(('same_repeated', ['--fixed-step-ms', '16', '--fixed-step-ms', '16'], 16, None))
    for name, options in [('missing', ['--fixed-step-ms']), ('empty', ['--fixed-step-ms', '']),
                          ('next_flag', ['--fixed-step-ms', '--frames', '1'])]:
        cases.append((name, options, None, '--fixed-step-ms requires a value'))
    for index, value in enumerate(('0', '251', '-1', 'nan', 'inf', '1.5', '1e2', '0x10',
                                   '+16', '16x', ' 16', '16 ', '4294967296', '999999999999999999999999')):
        cases.append((f'invalid_{index}', ['--fixed-step-ms', value], None, 'Invalid --fixed-step-ms value'))
    cases.append(('conflicting', ['--fixed-step-ms', '16', '--fixed-step-ms', '17'], None,
                  'Conflicting --fixed-step-ms options'))
    environment = os.environ.copy()
    environment.pop('CAESURA_RESOURCE_ROOT', None)
    records = []
    for name, options, step, error in cases:
        directory = output / name
        launch = directory / 'launch space'
        resource = directory / 'resource space'
        launch.mkdir(parents=True)
        (resource / 'assets').mkdir(parents=True)
        (resource / 'scripts/kag').mkdir(parents=True)
        (resource / 'scripts/config.lua').write_text("config = {}\nprint('U29_CLOCK_CONFIG_LOADED')\n", encoding='utf-8')
        (resource / 'scripts/kag/init.lua').write_text('-- Isolated startup fixture.\n', encoding='utf-8')
        argv = [str(engine), '--headless', '--resource-root', str(resource), *options]
        record = dict(name=name, argv=argv, cwd=str(launch), passed=False)
        try:
            completed = subprocess.run(argv, cwd=launch, env=environment,
                input=b'{"id":101,"method":"ping"}\n', stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
            stdout, stderr = completed.stdout, completed.stderr
            record['exit_code'] = completed.returncode
            text = (stdout + stderr).decode('utf-8', errors='replace')
            clocks = [line for line in text.splitlines() if line.startswith('[Engine] Simulation clock:')]
            if error is not None:
                record['passed'] = completed.returncode != 0 and error in text and not clocks and 'U29_CLOCK_CONFIG_LOADED' not in text
            else:
                responses = []
                for line in stdout.decode('utf-8', errors='replace').splitlines():
                    try: responses.append(json.loads(line))
                    except ValueError: pass
                ping = any(isinstance(row, dict) and row.get('id') == 101 and row.get('result') == 'ok' for row in responses)
                expected = '[Engine] Simulation clock: realtime' if step == 0 else f'[Engine] Simulation clock: fixed; step_ms={step}'
                record['passed'] = completed.returncode == 0 and ping and clocks == [expected] and 'U29_CLOCK_CONFIG_LOADED' in text
        except subprocess.TimeoutExpired as caught:
            stdout, stderr = caught.stdout or b'', caught.stderr or b''
            record['error'] = 'Owned subprocess exceeded 30 seconds and was killed by subprocess.run'
        (directory / 'stdout.log').write_bytes(stdout)
        (directory / 'stderr.log').write_bytes(stderr)
        record.update(stdout_sha256=sha(directory / 'stdout.log'), stderr_sha256=sha(directory / 'stderr.log'))
        records.append(record)
        print(f"{'PASS' if record['passed'] else 'FAIL'} {name}", flush=True)
    stable = sha(engine) == before
    report = dict(schema='caesura.fixed-step-cli-test.v1', engine=str(engine), engine_sha256=before,
                  engine_stable=stable, cases=records, passed=stable and all(row['passed'] for row in records),
                  scope='Actual CLI selection and rejection; owner-loop timing and GPU/audio effects verified separately.')
    (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(f"{sum(row['passed'] for row in records)}/{len(records)} passed", flush=True)
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
