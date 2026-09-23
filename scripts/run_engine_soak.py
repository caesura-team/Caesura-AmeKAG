"""Owned native soak diagnostics; complete soak acceptance is not yet enabled.

Requires an already configured Windows foundation build. Builds the actual
Release probe, preserves one attempt and uses a fresh external runtime copy.
Diagnostics never report short/long, cold recovery or release acceptance.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
from dataclasses import asdict
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import shutil
import struct
import sys
import time
import wave

from engine_soak_trace import check_trace, check_epochs
from native_package_runtime import observe_loaded_modules
from package_runtime import ProcessIdentity, process_identity, run_runtime_command
from run_validation import _source_identity, _validate_environment
from verify_render_contracts import decode_png, safe_file


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def write_json(path, value):
    Path(path).write_text(json.dumps(value, indent=2) + '\n', encoding='utf-8')


def run_observed_command(argv, cwd, env, evidence, ready, progress, *,
                         timeout, startup_seconds, progress_seconds, inspect=None):
    """Supervise a real owned child; observation errors never become success.

    The optional inspector runs only against the OS-verified retained child's
    identity. Stop requests are consumed by its launcher, never a searched PID.
    Deadlines here detect missing progress; the owned runner also bounds the
    entire tree and writes its original timeout receipt before raising.
    """
    for value in (timeout, startup_seconds, progress_seconds):
        require(type(value) in (int, float) and math.isfinite(value) and value > 0,
                'Deadlines must be positive finite numbers')
    evidence, ready, progress = map(Path, (evidence, ready, progress))
    evidence.mkdir()  # Never reuse a partial/successful prior attempt.
    control = evidence / 'process'
    stop = control / 'stop.json'
    started = time.monotonic()
    result = dict(status='FAIL', inspection=None, ready_observed=False)
    error = None
    with (evidence/'stdout').open('xb') as stdout, (evidence/'stderr').open('xb') as stderr:
        with ThreadPoolExecutor(max_workers=1) as pool:
            future = pool.submit(run_runtime_command, argv, cwd, env, control,
                                 stdout, stderr, timeout, stop_request=stop)
            try:
                last_progress, last_size = started, None
                while not future.done():
                    now = time.monotonic()
                    if not result['ready_observed']:
                        identity_file = control/'process.json'
                        if identity_file.is_file() and ready.is_file():
                            # The native readiness writer closes its report
                            # after writing. A partial JSON publication is not
                            # readiness; it remains subject to the startup limit.
                            try:
                                ready_value = json.loads(ready.read_text(encoding='utf-8'))
                            except json.JSONDecodeError:
                                ready_value = None
                            if isinstance(ready_value, dict):
                                identity = ProcessIdentity(**json.loads(identity_file.read_text(encoding='utf-8')))
                                require(process_identity(identity.pid) == identity,
                                        'Owned identity changed before readiness')
                                result['observed_process'] = asdict(identity)
                                result['inspection'] = inspect(identity) if inspect else None
                                result['ready_observed'] = True
                                last_progress = time.monotonic()
                        if not result['ready_observed'] and now-started >= startup_seconds:
                            raise ValueError('Startup deadline expired before readiness')
                    if result['ready_observed']:
                        size = progress.stat().st_size if progress.is_file() else None
                        require(last_size is None or size is not None and size >= last_size,
                                'Progress stream was removed or truncated')
                        if size is not None and size != last_size:
                            last_progress, last_size = time.monotonic(), size
                        if time.monotonic()-last_progress >= progress_seconds:
                            raise ValueError('Progress deadline expired after readiness')
                    time.sleep(.02)
            except BaseException as problem:
                error = f'{type(problem).__name__}: {problem}'
            finally:
                if error and not future.done() and control.is_dir():
                    with stop.open('x', encoding='utf-8') as stream:
                        stream.write('stop owned child\n')
                try:
                    result['receipt'] = future.result()
                except BaseException as problem:
                    if error is None:
                        error = f'{type(problem).__name__}: {problem}'
                    receipt_file = control/'run.json'
                    if receipt_file.is_file():
                        result['receipt'] = json.loads(receipt_file.read_text(encoding='utf-8'))
    result['owner_observed_seconds'] = time.monotonic()-started
    receipt = result.get('receipt', {})
    good = (error is None and result['ready_observed']
            and receipt.get('process') == result.get('observed_process')
            and receipt.get('actual_exit_code') == 0 and receipt.get('status') == 'EXITED'
            and receipt.get('owned_tree_cleanup') == 'COMPLETE'
            and receipt.get('timed_out') is False and receipt.get('forced_kill') is False)
    result['status'] = 'OBSERVED' if good else 'FAIL'
    if not good:
        result['error'] = error or 'Missing readiness/owner, failed exit or incomplete cleanup'
    result['stdout_sha256'] = sha(evidence/'stdout')
    result['stderr_sha256'] = sha(evidence/'stderr')
    write_json(evidence/'observation.json', result)
    return result


def verify_images(output, events):
    """Decode actual output bytes after check_trace accepted their event grammar."""
    output = Path(output)
    captures = [event for event in events if event['event'] == 'capture_consumed']
    require(captures and len(captures) % 3 == 0, 'Incomplete screenshot triplets')
    rows, used = [], set()
    for offset in range(0, len(captures), 3):
        group = captures[offset:offset+3]
        cycle = offset//3
        require([event['detail']['page'] for event in group] == ['a', 'b', 'restored'],
                'Wrong screenshot sequence')
        rgba = {}
        for event in group:
            detail = event['detail']; page = detail['page']
            require(event['cycle'] == cycle and detail['file'] == f'cycle-{cycle+1}-{page}.png',
                    'Wrong screenshot cycle/output path')
            path = safe_file(output, detail['file'], used)
            encoded = path.read_bytes()
            require(type(detail['png_bytes']) is int and len(encoded) == detail['png_bytes'],
                    'PNG byte count differs from consumed output')
            width, height, pixels = decode_png(encoded)
            require((width, height) == (640, 360), 'Wrong PNG geometry')
            expected = (130, 35, 50) if page == 'b' else (20, 50, 90)
            pixel = (10*width+10)*4
            require(all(abs(pixels[pixel+i]-expected[i]) <= 2 for i in range(3)),
                    'Actual screenshot background differs')
            rgba[page] = pixels
            rows.append(dict(file=path.name, sha256=hashlib.sha256(encoded).hexdigest(),
                             rgba_sha256=hashlib.sha256(pixels).hexdigest()))
        require(rgba['a'] == rgba['restored'], 'Restored pixels differ from saved page')
        require(rgba['a'] != rgba['b'], 'Changed page is indistinguishable from saved page')
    return rows


def prepare_runtime(repo, runtime):
    runtime.mkdir()
    shutil.copytree(repo/'scripts', runtime/'scripts',
                    ignore=shutil.ignore_patterns('__pycache__', '*.pyc', '*.pyo'))
    shutil.copytree(repo/'tests/projects/engine_soak', runtime/'tests/projects/engine_soak')
    (runtime/'assets/fonts').mkdir(parents=True)
    shutil.copy2(repo/'assets/fonts/NotoSansCJKsc-Regular.otf', runtime/'assets/fonts/NotoSansCJKsc-Regular.otf')
    assets = runtime/'assets/soak'; assets.mkdir()
    (runtime/'assets/script').mkdir()
    for name, color in [('a', (20, 50, 90)), ('b', (130, 35, 50))]:
        data = bytes(color[::-1])*64*36
        (assets/(name+'.bmp')).write_bytes(b'BM'+struct.pack('<IHHI',54+len(data),0,0,54)
            +struct.pack('<IiiHHIIiiII',40,64,36,1,24,0,len(data),2835,2835,0,0)+data)
    for name, duration, hz in [('long-a',1.0,220), ('long-b',1.0,330), ('short',.08,440)]:
        with wave.open(str(assets/(name+'.wav')), 'wb') as audio:
            audio.setnchannels(1); audio.setsampwidth(2); audio.setframerate(48000)
            audio.writeframes(b''.join(struct.pack('<h',int(1800*math.sin(2*math.pi*hz*i/48000)))
                                       for i in range(int(duration*48000))))
    return {str(path.relative_to(runtime)):sha(path) for path in runtime.rglob('*') if path.is_file()}


def source_identity(repo):
    require((repo/'.git').exists(), 'Repository .git is required for source identity')
    return _source_identity(repo)


def run_diagnostic(repo, build, output, *, contexts=False):
    repo, build = Path(repo).resolve(strict=True), Path(build).resolve(strict=True)
    output = Path(output).absolute()
    require(os.name == 'nt', 'This native D3D11/Device lane requires Windows')
    require(not output.resolve().is_relative_to(repo), 'Runtime evidence must be outside the source checkout')
    output.mkdir()
    report = dict(status='FAIL', mode='context-diagnostic' if contexts else 'diagnostic', accepted_soak=False,
                  physical_audibility='NOT_MEASURED', context_restart='NOT_RUN',
                  cold_restart='NOT_RUN', negative_controls='NOT_RUN', commands=[])
    before = None
    build_keys = {'PATH','SYSTEMROOT','SYSTEMDRIVE','WINDIR','COMSPEC','TEMP','TMP','PROGRAMFILES',
                  'PROGRAMFILES(X86)','PROGRAMW6432','NUMBER_OF_PROCESSORS',
                  'PROCESSOR_ARCHITECTURE','VCINSTALLDIR','VCTOOLSINSTALLDIR',
                  'WINDOWSSDKDIR','INCLUDE','LIB','LIBPATH','VSINSTALLDIR',
                  'PROGRAMDATA','ALLUSERSPROFILE','USERPROFILE','APPDATA',
                  'LOCALAPPDATA','HOMEDRIVE','HOMEPATH'}
    env = {key:value for key,value in os.environ.items() if key.upper() in build_keys}
    env.update(PYTHONDONTWRITEBYTECODE='1', PYTHONIOENCODING='utf-8')
    try:
        before = source_identity(repo); report['source_before'] = before
        profiles = json.loads((repo/'scripts/validation_profiles.json').read_text(encoding='utf-8'))
        cache = _validate_environment(profiles['profiles']['windows-release'], repo, build, 'Release')
        for option in ('CAESURA_ENABLE_FFMPEG', 'CAESURA_LIVE2D', 'CAESURA_HAS_STEAM'):
            require(cache.get(option) == 'OFF', 'Foundation cache option differs: '+option)
        report['configuration'] = dict(configuration='Release', cache_sha256=sha(build/'CMakeCache.txt'),
            generator=cache['CMAKE_GENERATOR'], host=dict(platform.uname()._asdict()))
        cmake = Path(shutil.which('cmake')).resolve(strict=True)
        with (output/'build.stdout').open('xb') as so, (output/'build.stderr').open('xb') as se:
            receipt = run_runtime_command([str(cmake),'--build',str(build),'--config','Release',
                '--target','CaesuraEngineSoakProbe','--parallel','4'],repo,env,output/'build-process',so,se,1800)
        report['commands'].append(dict(name='build',receipt=receipt,
            stdout_sha256=sha(output/'build.stdout'),stderr_sha256=sha(output/'build.stderr')))
        require(receipt['actual_exit_code'] == 0 and receipt['owned_tree_cleanup'] == 'COMPLETE', 'Release build failed')
        runtime, probe_output = output/'runtime', output/'probe'
        report['input_locks'] = prepare_runtime(repo, runtime); probe_output.mkdir()
        executable = build/'tests/Release/CaesuraEngineSoakProbe.exe'
        binary_locks = {str(path.resolve()):sha(path) for path in [executable, *executable.parent.glob('*.dll')]}
        sdl = (executable.parent/'SDL3.dll').resolve(strict=True)
        report['binary_locks'] = binary_locks
        home, temp = output/'home', output/'temp'
        home.mkdir(); temp.mkdir()
        system = Path(os.environ['SystemRoot']).resolve(strict=True)
        probe_env = dict(SystemRoot=str(system), SystemDrive=system.drive, WINDIR=str(system),
            ComSpec=str(system/'System32/cmd.exe'),
            PATH=os.pathsep.join((str(executable.parent), str(system/'System32'))),
            USERPROFILE=str(home), HOME=str(home), ProgramData=str(home), ALLUSERSPROFILE=str(home),
            APPDATA=str(home/'AppData/Roaming'), LOCALAPPDATA=str(home/'AppData/Local'),
            TMP=str(temp), TEMP=str(temp))

        def inspect(identity):
            require(Path(identity.executable) == executable.resolve(), 'Wrong probe executable owner')
            initialized = json.loads((probe_output/'initialized.json').read_text(encoding='utf-8'))
            require(initialized.get('pid') == identity.pid, 'Readiness belongs to another PID')
            modules = observe_loaded_modules(identity)
            paths = {Path(path) for path in modules['paths']}
            require(executable.resolve() in paths and sdl in paths, 'Actual probe/SDL module missing')
            require(not any(path.name.casefold() == 'sdl3.dll' and path != sdl for path in paths),
                    'Foreign SDL module observed')
            d3d = [path for path in paths if path.name.casefold() == 'd3d11.dll']
            require(len(d3d) == 1, 'Actual D3D11 module missing or ambiguous')
            require(d3d[0] == (system/'System32/d3d11.dll').resolve(strict=True),
                    'D3D11 was not loaded from the declared system directory')
            modules['required_bytes'] = {str(path):sha(path) for path in (executable.resolve(), sdl, d3d[0])}
            write_json(output/'loaded-modules.json', modules)
            return modules

        argv = [str(executable),str(runtime),str(probe_output),'22','0'] + (['2'] if contexts else [])
        progress_file = probe_output/('progress.jsonl' if contexts else 'events.jsonl')
        observed = run_observed_command(argv,
            runtime,probe_env,output/'probe-process',probe_output/'initialized.json',progress_file,
            timeout=120,startup_seconds=30,progress_seconds=10,inspect=inspect)
        report['probe_observation'] = observed
        require(observed['status'] == 'OBSERVED', observed.get('error', 'Probe observation failed'))
        result_path = safe_file(probe_output, 'contexts.json' if contexts else 'result.json')
        event_path = safe_file(probe_output, progress_file.name, limit=16*1024**2)
        result = json.loads(result_path.read_text(encoding='utf-8'))
        if contexts:
            selected = result.get('epochs')
            require(isinstance(selected,list) and len(selected)==2, 'Diagnostic needs exactly two contexts')
            epochs, epoch_files = [], []
            for index, item in enumerate(selected):
                name = f'epoch-{index}'
                require(isinstance(item,dict) and item.get('directory')==name
                        and item.get('backend_registry_retired') is True, 'Context directory or retired registry differs')
                raw = safe_file(probe_output, name+'/result.json')
                trace = safe_file(probe_output, name+'/events.jsonl', limit=16*1024**2)
                epoch_result = json.loads(raw.read_text(encoding='utf-8'))
                require(item.get('result')==epoch_result, 'Continuous summary substituted its epoch result')
                events = [json.loads(line) for line in trace.read_text(encoding='utf-8').splitlines()]
                epochs.append(dict(item,events=events))
                epoch_files.append(dict(directory=name,result_sha256=sha(raw),events_sha256=sha(trace)))
            errors = check_epochs(result,epochs,observed['receipt']['process'],observed['owner_observed_seconds'],'diagnostic')
            require(not errors, '; '.join(errors))
            progress = [json.loads(line) for line in event_path.read_text(encoding='utf-8').splitlines()]
            expected = []
            for index, epoch in enumerate(epochs):
                expected.extend((index,event['event'],event['cycle']) for event in epoch['events'])
                expected.append((index,'context_destroyed',None))
            require(len(progress)==len(expected), 'Continuous progress omits or duplicates events')
            previous = 0
            for position, (event, (index,name,cycle)) in enumerate(zip(progress,expected)):
                require(type(event.get('epoch')) is int and event['epoch']==index
                        and event.get('event')==name and event.get('cycle')==cycle, 'Continuous progress order differs')
                clock = event.get('process_seconds')
                require(type(clock) in (int,float) and math.isfinite(clock)
                        and previous<=clock<=observed['owner_observed_seconds']
                        and clock-previous<(30 if position==0 else 10), 'Continuous progress clock differs')
                previous = clock
            report['pngs'] = []
            for index, epoch in enumerate(epochs):
                report['pngs'].extend(dict(row,file=f'epoch-{index}/'+row['file'])
                                     for row in verify_images(probe_output/f'epoch-{index}',epoch['events']))
            report.update(epoch_files=epoch_files,context_restart='OBSERVED')
        else:
            events = [json.loads(line) for line in event_path.read_text(encoding='utf-8').splitlines()]
            errors = check_trace(result,events,observed['receipt']['process'],observed['owner_observed_seconds'],'diagnostic')
            require(not errors, '; '.join(errors))
            report['pngs'] = verify_images(probe_output, events)
        report.update(result=result,result_sha256=sha(result_path),events_sha256=sha(event_path))
        require(all(sha(runtime/name) == digest for name,digest in report['input_locks'].items()), 'Runtime input bytes changed')
        require(all(sha(path) == digest for path,digest in binary_locks.items()), 'Probe binary/DLL bytes changed')
        require(all(sha(path) == digest for path,digest in observed['inspection']['required_bytes'].items()), 'Observed module bytes changed')
        report['source_after'] = source_identity(repo)
        require(before == report['source_after'], 'Source changed during diagnostic')
        report['source_stable'] = True
        report['status'] = 'DIAGNOSTIC_REVIEWED'
    except Exception as error:
        report['error'] = f'{type(error).__name__}: {error}'
    finally:
        try:
            report['source_after'] = source_identity(repo)
            report['source_stable'] = before is not None and before == report['source_after']
            if not report['source_stable']:
                report['status'] = 'FAIL'
        except Exception as error:
            report.update(status='FAIL', source_stable=False, source_error=f'{type(error).__name__}: {error}')
        build_receipt = output/'build-process/run.json'
        if build_receipt.is_file():
            report['build_receipt_sha256'] = sha(build_receipt)
        write_json(output/'run.json', report)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo-root', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--run-dir', type=Path, required=True, help='New directory under an existing parent, outside checkout')
    parser.add_argument('--mode', choices=('diagnostic','context-diagnostic'), default='diagnostic')
    args = parser.parse_args()
    report = run_diagnostic(args.repo_root,args.build_dir,args.run_dir,contexts=args.mode=='context-diagnostic')
    print(json.dumps({key:report[key] for key in ('status','accepted_soak','error') if key in report}))
    return 0 if report['status'] == 'DIAGNOSTIC_REVIEWED' else 1


if __name__ == '__main__':
    raise SystemExit(main())
