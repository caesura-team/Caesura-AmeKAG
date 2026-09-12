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
    env['CAESURA_RPC_DISPATCH_TIMEOUT_MS'] = '100'
    env.pop('CAESURA_TEST_STALL_MS', None)
    if outcome == 'queued': env['CAESURA_TEST_STALL_MS'] = '5000'
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

    def consume(stream, capture):
        for line in stream:
            with changed:
                capture.append(line)
                changed.notify_all()
            if capture is lines:
                try: value = json.loads(line)
                except ValueError: continue
                if isinstance(value, dict): responses.put(value)

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
            response = responses.get(timeout=max(.001, deadline - time.monotonic()))
            if response.get('id') == request_id: return None, response
            if time.monotonic() >= deadline: raise TimeoutError('response id deadline')

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
        else:
            # C string work keeps the loop below the existing Lua instruction
            # quota; the time bound is intentional work, not a scheduling race.
            tail = "error('U18_LATE_FAILURE',0)" if outcome == 'late-failure' else "return 'late-success'"
            code = "local until_time=os.clock()+0.6; while os.clock()<until_time do local s=string.rep('work',8192) end; local k=require('kag'); k._u18_count=(k._u18_count or 0)+1; " + tail
        status, response = request('eval', code)
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
        if not proc.stdin.closed: proc.stdin.close()
        if proc.poll() is None:
            # Cleanup may finish a failed case naturally; it never erases the
            # failed observation. Termination remains an explicit failure.
            try: proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                record['forced_termination'] = True
                proc.kill()
                proc.wait(timeout=10)
        for reader in readers: reader.join(timeout=3)
        record['checks']['readers_joined'] = all(not reader.is_alive() for reader in readers)
        proc.stdout.close(); proc.stderr.close()
        record['exit_code'] = proc.returncode
        record['stop_events'] = [line for line in errors if '[RpcRequest]' in line and 'op=stop ' in line]
        (out / (name + '.stdout.log')).write_text(''.join(lines), encoding='utf-8')
        (out / (name + '.stderr.log')).write_text(''.join(errors), encoding='utf-8')
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
