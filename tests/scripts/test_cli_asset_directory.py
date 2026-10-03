"""Run the maintained CLI asset controls with fresh files and owned cleanup.

CTest supplies this build's interpreter, explicit --test Lua entry, and
source scripts. All generated files stay in a tempfile-owned child of the CWD.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import sys
import tempfile

sys.dont_write_bytecode = True
MIN_CHECKS = 26
CHILD_TIMEOUT = 30
MAX_LOG_BYTES = 2 * 1024 * 1024

def digest(path: Path) -> str:
    value = hashlib.sha256()
    with path.open('rb') as stream:
        for part in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(part)
    return value.hexdigest()

def read_log(path: Path) -> str:
    if not path.exists():
        return ''
    if path.is_symlink() or not path.is_file() or path.stat().st_size > MAX_LOG_BYTES:
        raise RuntimeError('Invalid or oversized CLI control log')
    return path.read_text(encoding='utf-8', errors='replace')

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--lua', required=True, type=Path)
    parser.add_argument('--scripts', required=True, type=Path)
    parser.add_argument('--test', type=Path, default=Path(__file__).with_suffix('.lua'))
    args = parser.parse_args()
    lua = args.lua.resolve(strict=True)
    scripts = args.scripts.resolve(strict=True)
    test = args.test.resolve(strict=True)
    if not lua.is_file() or not scripts.is_dir():
        raise ValueError('Actual Lua executable and source scripts directory are required')
    inputs = [lua, test, scripts/'fileutil.lua', scripts/'sandbox.lua',
              scripts/'package_runtime.py', scripts/'validation_process.py']
    before = {str(path): digest(path) for path in inputs}
    sys.path.insert(0, str(scripts))
    from package_runtime import run_runtime_command

    parent = Path.cwd().resolve(strict=True)
    work = Path(tempfile.mkdtemp(prefix='cli-assets-', dir=parent)).resolve(strict=True)
    print('CLI_ASSET_DIRECTORY_ATTEMPT=' + str(work), flush=True)
    success = False
    result = None
    error = None
    try:
        fixture = work/'fixtures'
        fixture.mkdir()
        (fixture/'nested').mkdir()
        (fixture/'directory.wav').mkdir()
        for name in ('alpha.ogg', 'zeta.wav', '音楽.wav'):
            (fixture/name).write_bytes(b'enumeration fixture\n')
        (fixture/'nested'/'hidden.wav').write_bytes(b'not an immediate leaf\n')
        for name in ('tmp', 'home', 'local', 'roaming'):
            (work/name).mkdir()
        env = dict(os.environ)
        env.update(TEMP=str(work/'tmp'), TMP=str(work/'tmp'), TMPDIR=str(work/'tmp'),
                   HOME=str(work/'home'), USERPROFILE=str(work/'home'),
                   LOCALAPPDATA=str(work/'local'), APPDATA=str(work/'roaming'),
                   PYTHONDONTWRITEBYTECODE='1')
        # Ignore ambient LUA_INIT/LUA_PATH configuration without changing the
        # maintained test. Its explicit source path and builtin preload remain.
        argv = [str(lua), '-E', str(test), 'fixtures', str(scripts)]
        with (work/'stdout.log').open('xb') as stdout, (work/'stderr.log').open('xb') as stderr:
            result = run_runtime_command(argv, work, env, work/'owned', stdout, stderr, CHILD_TIMEOUT)
        text = read_log(work/'stdout.log')
        diagnostics = read_log(work/'stderr.log')
        print(text, end='' if text.endswith('\n') else '\n')
        if diagnostics:
            print(diagnostics, file=sys.stderr, end='' if diagnostics.endswith('\n') else '\n')
        if not (result.get('status') == 'EXITED'
                and type(result.get('actual_exit_code')) is int and result['actual_exit_code'] == 0
                and type(result.get('launcher_exit_code')) is int and result['launcher_exit_code'] == 0
                and result.get('owned_tree_cleanup') == 'COMPLETE'
                and all(result.get(key) is False for key in ('timed_out', 'forced_kill', 'stop_requested'))):
            raise RuntimeError('CLI controls did not exit naturally with complete owned cleanup')
        markers = re.findall(r'^CLI_ASSET_DIRECTORY_CONTROLS_PASS checks=(\d+)\s*$', text, flags=re.MULTILINE)
        if len(markers) != 1 or int(markers[0]) < MIN_CHECKS:
            raise RuntimeError('Missing unique CLI control completion or reduced assertion count')
        if before != {str(path): digest(path) for path in inputs}:
            raise RuntimeError('CLI control inputs changed during execution')
        report = {'status': 'PASS', 'checks': int(markers[0]), 'argv': argv,
                  'source_sha256': before, 'owned': result}
        success = True
    except Exception as cause:
        error = str(cause)
        # Failure receipts/partial logs stay in this unique attempt directory;
        # no uncertain live process or another test's directory is cleaned.
        print('CLI_ASSET_DIRECTORY_CTEST_FAIL: ' + error, file=sys.stderr, flush=True)
    finally:
        if success:
            resolved = work.resolve(strict=True)
            if resolved.parent != parent or work.is_symlink() or not work.name.startswith('cli-assets-'):
                raise RuntimeError('Refusing unexpected cleanup target')
            shutil.rmtree(work)
        else:
            print('CLI_ASSET_DIRECTORY_RETAINED=' + str(work), file=sys.stderr, flush=True)
    if error is not None:
        return 1
    print('CLI_ASSET_DIRECTORY_CTEST_RESULT:' + json.dumps(report, sort_keys=True), flush=True)
    print('CLI_ASSET_DIRECTORY_CTEST_PASS', flush=True)
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
