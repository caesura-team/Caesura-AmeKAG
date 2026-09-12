"""Real owner RPC cancellation and late outcomes over stdio and HTTP.

Component barriers prove the state race; these processes verify production
transport envelopes, a single mutation, correlated diagnostics and natural exit.
"""
import hashlib
import json
import os
from pathlib import Path
import queue
import re
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request
import uuid

binary = Path(sys.argv[1]).resolve(strict=True)
out = (Path(sys.argv[2]) if len(sys.argv) > 2 else
       Path.cwd() / 'artifacts' / 'validation' / ('rpc-timeouts-' + str(uuid.uuid4()))).resolve()
out.mkdir(parents=True, exist_ok=False)
report = {'binary': str(binary), 'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(), 'cases': []}


def probe(transport, outcome):
    name = transport + '-' + outcome
    record = {'name': name, 'checks': {}, 'forced_termination': False}
    report['cases'].append(record)
    env = dict(os.environ)
    env.pop('CAESURA_RESOURCE_ROOT', None)
    env.pop('CAESURA_TEST_STALL_MS', None)
    if outcome == 'queued':
        env['CAESURA_RPC_DISPATCH_TIMEOUT_MS'] = '100'
        env['CAESURA_TEST_STALL_MS'] = '5000'
    else:
        # Normal control requests retain the production 5s deadline. A release
        # barrier, not a guessed 100ms owner scheduling window, forces the eval
        # to remain Running until its real caller timeout has been observed.
        env.pop('CAESURA_RPC_DISPATCH_TIMEOUT_MS', None)
    record['dispatch_timeout_ms'] = 100 if outcome == 'queued' else 5000
    token = 'u18-test-' + str(uuid.uuid4())
    if transport == 'http':
        with socket.socket() as reservation:
            reservation.bind(('127.0.0.1', 0))
            port = reservation.getsockname()[1]
        env['CAESURA_EDITOR_PORT'] = str(port)
        env['CAESURA_EDITOR_TOKEN'] = token
    command = [str(binary), '--editor' if transport == 'http' else '--headless']
    if outcome == 'queued': command += ['--frames', '1']
    proc = subprocess.Popen(command, cwd=binary.parent, env=env, stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                            encoding='utf-8', errors='replace', bufsize=1)
    record['pid'] = proc.pid
    lines, errors = [], []
    changed = threading.Condition()
    responses = queue.Queue()
    stdout_closed = threading.Event()

    def consume(stream, capture):
        try:
            for line in stream:
                with changed:
                    capture.append(line)
                    changed.notify_all()
                if capture is lines:
                    try: value = json.loads(line)
                    except ValueError: continue
                    if isinstance(value, dict): responses.put(value)
        finally:
            if capture is lines: stdout_closed.set()
            with changed: changed.notify_all()

    readers = [threading.Thread(target=consume, args=(proc.stdout, lines), daemon=True),
               threading.Thread(target=consume, args=(proc.stderr, errors), daemon=True)]
    for reader in readers: reader.start()

    def wait_observation(predicate, timeout=10):
        deadline = time.monotonic() + timeout
        with changed:
            while not predicate():
                remaining = deadline - time.monotonic()
                if remaining <= 0: raise TimeoutError('observation deadline')
                changed.wait(min(remaining, .1))

    request_id = 1830
    eval_worker = None
    eval_results = queue.Queue()
    release_dir = release_file = None

    def request(method, code=None):
        nonlocal request_id
        request_id += 1
        if transport == 'http':
            data = code.encode() if code is not None else (b'' if method == 'stop' else None)
            req = urllib.request.Request('http://127.0.0.1:%d/api/%s' % (port, method), data=data,
                                         headers={'Authorization': 'Bearer ' + token, 'Content-Type': 'text/plain'})
            try:
                with urllib.request.urlopen(req, timeout=12) as response:
                    return response.status, json.loads(response.read())
            except urllib.error.HTTPError as error:
                with error: return error.code, json.loads(error.read())
        payload = {'id': request_id, 'method': method}
        if code is not None: payload['code'] = code
        proc.stdin.write(json.dumps(payload) + '\n')
        proc.stdin.flush()
        deadline = time.monotonic() + 45
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0: raise TimeoutError('response id deadline')
            try: response = responses.get(timeout=min(.1, remaining))
            except queue.Empty:
                if not stdout_closed.is_set(): continue
                # Process exit can precede the reader's final response. EOF is
                # published after every parsed line is queued; drain that tail
                # before treating the protocol as closed.
                try: response = responses.get_nowait()
                except queue.Empty: raise RuntimeError('protocol closed before response')
            if response.get('id') == request_id: return None, response

    try:
        if transport == 'http':
            # Readiness belongs to this PID; a different listener cannot satisfy
            # the test using the fresh per-process authentication token.
            wait_observation(lambda: any('Started on http://127.0.0.1:%d' % port in line for line in lines), 45)
        _, ping = request('ping')
        record['checks']['own_transport_ready'] = ping.get('result') == 'ok' if transport == 'stdio' else ping.get('status') == 'ok'
        marker = 'U18_QUEUED_MUTATION'
        if outcome == 'queued':
            code = "print('" + marker + "'); return 'unexpected'"
            status, response = request('eval', code)
        else:
            test_root = (binary.parent / 'tests').resolve()
            if test_root.parent != binary.parent:
                raise AssertionError('test root is outside the selected binary directory')
            test_root.mkdir(exist_ok=True)
            release_dir = Path(tempfile.mkdtemp(prefix='u18-rpc-', dir=test_root)).resolve(strict=True)
            if release_dir.parent != test_root:
                raise AssertionError('release barrier is outside its owned test directory')
            release_file = release_dir / 'release.flag'
            relative_release = 'tests/' + release_dir.name + '/release.flag'
            tail = "error('U18_LATE_FAILURE',0)" if outcome == 'late-failure' else "return 'late-success'"
            # Real sandbox-allowed file reads provide the gate. Fixed-size C
            # string work bounds Lua instruction cost; both wall time and loop
            # count bound a missing release. No production file or hook is modified.
            code = ("local block=string.rep('Work',262144); local released=false; local deadline=os.time()+30; "
                    "for attempt=1,50000 do local f=io.open('" + relative_release + "','rb'); "
                    "if f then f:close(); released=true; break end; "
                    "if os.time()>deadline then break end; local converted=string.lower(block) end; "
                    "assert(released,'U18_RELEASE_NOT_OBSERVED'); "
                    "local k=require('kag'); k._u18_count=(k._u18_count or 0)+1; " + tail)

            def submit_eval():
                try: eval_results.put((request('eval', code), None))
                except Exception as error: eval_results.put((None, error))
                with changed: changed.notify_all()

            eval_worker = threading.Thread(target=submit_eval, daemon=True)
            eval_worker.start()
            accepted_pattern = re.compile(r'\[RpcRequest\] id=(\d+) op=eval phase=accepted ')
            wait_observation(lambda: any(accepted_pattern.search(line) for line in errors) or not eval_results.empty(), 7)
            accepted = next((accepted_pattern.search(line) for line in errors if accepted_pattern.search(line)), None)
            if not accepted: raise AssertionError('intended Running eval was not accepted')
            running_id = accepted.group(1)
            running_prefix = '[RpcRequest] id=' + running_id + ' '
            wait_observation(lambda: any(running_prefix in line and 'phase=started' in line for line in errors)
                             or not eval_results.empty(), 7)
            started = any(running_prefix in line and 'phase=started' in line for line in errors)
            record['checks']['running_precondition_observed'] = started
            evaluation, worker_error = eval_results.get(timeout=8)
            if worker_error is not None: raise worker_error
            status, response = evaluation
            record['checks']['barrier_closed_at_timeout'] = (not release_file.exists()
                and not any(running_prefix in line and 'phase=completed' in line for line in errors))
            record['response'] = response
            if not started:
                raise AssertionError('intended Running eval did not start before its dispatch deadline')
            timeout_id = re.search(r'request_id=(\d+)', response.get('message', response.get('error', '')))
            record['checks']['running_timeout_confirmed_before_release'] = (
                response.get('code') == 'result_unknown' and (transport != 'http' or status == 503)
                and timeout_id is not None and timeout_id.group(1) == running_id)
            if not record['checks']['barrier_closed_at_timeout'] or not record['checks']['running_timeout_confirmed_before_release']:
                raise AssertionError('release requires the same Running request to time out behind a closed gate')
            release_file.write_text('release\n', encoding='ascii')
        record['response'] = response
        expected_code = 'request_cancelled' if outcome == 'queued' else 'result_unknown'
        record['checks']['timeout_envelope'] = response.get('code') == expected_code and (transport != 'http' or status == 503)
        match = re.search(r'request_id=(\d+)', response.get('message', response.get('error', '')))
        if not match: raise AssertionError('timeout response lacks its internal request ID')
        internal_id = match.group(1)
        record['internal_request_id'] = internal_id
        prefix = '[RpcRequest] id=' + internal_id + ' '
        if outcome == 'queued':
            proc.wait(timeout=15)
        else:
            wait_observation(lambda: any(prefix in line and 'phase=completed' in line for line in errors))
            _, fresh = request('eval', "return require('kag')._u18_count")
            record['fresh_response'] = fresh
            record['checks']['mutation_once_and_fresh_request_works'] = fresh.get('status') == 'ok' and fresh.get('result') == '1'
            stop_status, stopped = request('stop')
            record['stop_status'] = stop_status
            record['stop_response'] = stopped
            record['checks']['stop_acknowledged'] = (stopped.get('result') == 'ok' if transport == 'stdio' else stopped.get('status') == 'ok')
            proc.wait(timeout=10)
        record['checks']['natural_exit'] = proc.returncode == 0
    except Exception as error:
        record['error'] = repr(error)
        record['checks']['protocol_completed'] = False
    finally:
        cleanup_errors = []
        if release_file is not None:
            # Release this test's operation on failure without retrying a
            # mutation or disguising cleanup as a successful protocol result.
            try:
                if not release_file.exists(): release_file.write_text('cleanup\n', encoding='ascii')
            except OSError as error: cleanup_errors.append('release: ' + type(error).__name__)
        try:
            if not proc.stdin.closed: proc.stdin.close()
        except OSError as error: cleanup_errors.append('stdin: ' + type(error).__name__)
        if proc.poll() is None:
            # Cleanup may finish a failed case naturally; it never erases the
            # failed observation. Termination remains an explicit failure.
            try: proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                record['forced_termination'] = True
                proc.kill()
                proc.wait(timeout=10)
        if eval_worker is not None:
            eval_worker.join(timeout=3)
            record['checks']['eval_submitter_joined'] = not eval_worker.is_alive()
        for reader in readers: reader.join(timeout=3)
        record['checks']['readers_joined'] = all(not reader.is_alive() for reader in readers)
        proc.stdout.close(); proc.stderr.close()
        record['exit_code'] = proc.returncode
        record['stop_events'] = [line for line in errors if '[RpcRequest]' in line and 'op=stop ' in line]
        (out / (name + '.stdout.log')).write_text(''.join(lines), encoding='utf-8')
        (out / (name + '.stderr.log')).write_text(''.join(errors), encoding='utf-8')
        if release_dir is not None:
            try:
                if release_file is not None: release_file.unlink(missing_ok=True)
                release_dir.rmdir()  # Only the exclusively created, now-empty directory.
            except OSError as error: cleanup_errors.append('remove: ' + type(error).__name__)
        record['checks']['barrier_cleanup_succeeded'] = not cleanup_errors
        if cleanup_errors: record['cleanup_errors'] = cleanup_errors
    if 'internal_request_id' in record:
        prefix = '[RpcRequest] id=' + record['internal_request_id'] + ' '
        events = [line for line in errors if prefix in line]
        record['events'] = events
        record['checks']['one_timeout'] = sum('phase=timed_out' in line for line in events) == 1
        if outcome == 'queued':
            record['checks']['never_started'] = not any('phase=started' in line for line in events)
            record['checks']['one_cancel_terminal'] = sum('phase=cancelled' in line for line in events) == 1
            record['checks']['cancelled_mutation_absent'] = marker not in ''.join(lines + errors)
        else:
            expected = 'code=eval_error' if outcome == 'late-failure' else 'code=none'
            record['checks']['started_once'] = sum('phase=started' in line for line in events) == 1
            record['checks']['one_late_terminal'] = sum('phase=completed' in line and expected in line for line in events) == 1
        record['checks']['diagnostics_exclude_payload_and_token'] = all(token not in line and 'U18_' not in line and 'string.rep' not in line for line in events)
    record['passed'] = all(record['checks'].values()) and not record['forced_termination']
    print(json.dumps(record))


try:
    for transport in ('stdio', 'http'):
        for outcome in ('queued', 'late-success', 'late-failure'):
            probe(transport, outcome)
finally:
    report['binary_unchanged'] = hashlib.sha256(binary.read_bytes()).hexdigest() == report['binary_sha256']
    report['passed'] = len(report['cases']) == 6 and all(case.get('passed') for case in report['cases']) and report['binary_unchanged']
    (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
if report['passed']: print('ALL RPC TIMEOUT TESTS PASSED')
sys.exit(0 if report['passed'] else 1)
