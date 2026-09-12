"""Real stdio backpressure with a paused consumer, finite frames, and owned cleanup."""
import hashlib
import json
import os
from pathlib import Path
import queue
import subprocess
import sys
import threading
import time
import uuid

binary = Path(sys.argv[1]).resolve(strict=True)
out = (Path(sys.argv[2]) if len(sys.argv) > 2 else
       Path.cwd() / 'artifacts' / 'validation' / ('stdio-output-' + str(uuid.uuid4()))).resolve()
out.mkdir(parents=True, exist_ok=False)
report = {'binary': str(binary), 'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(), 'cases': []}


def probe(blocked):
    disconnected = blocked == 'closed'
    record = {'name': 'disconnected-running' if disconnected else ('paused-consumer' if blocked else 'healthy-large-response'),
              'checks': {}, 'forced_termination': False}
    report['cases'].append(record)
    env = dict(os.environ)
    env['CAESURA_TEST_STALL_MS'] = '3000'
    env['CAESURA_RPC_DISPATCH_TIMEOUT_MS'] = '10000'
    proc = subprocess.Popen([str(binary), '--headless', '--frames', '1'], cwd=binary.parent,
                            env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, bufsize=0)
    record['pid'] = proc.pid
    ready, resume, accepted, started, consumer_closed = [threading.Event() for _ in range(5)]
    if not blocked: resume.set()
    output, errors = bytearray(), bytearray()
    responses = queue.Queue()

    def read_stdout():
        pending = bytearray()
        while True:
            if consumer_closed.is_set(): break
            chunk = proc.stdout.read(4096)
            if not chunk: break
            output.extend(chunk)
            pending.extend(chunk)
            while b'\n' in pending:
                line, _, remaining = pending.partition(b'\n')
                pending[:] = remaining
                try: value = json.loads(line)
                except (ValueError, UnicodeError): continue
                if isinstance(value, dict):
                    responses.put(value)
                    if value.get('id') == 1820:
                        ready.set()
                        # No pending read remains after this acknowledgement.
                        resume.wait()

    def read_stderr():
        while True:
            line = proc.stderr.readline()
            if not line: break
            errors.extend(line)
            if b'op=eval phase=accepted' in line: accepted.set()
            if b'op=eval phase=started' in line: started.set()

    readers = [threading.Thread(target=read_stdout, daemon=True), threading.Thread(target=read_stderr, daemon=True)]
    for reader in readers: reader.start()
    try:
        proc.stdin.write(b'{"id":1820,"method":"ping"}\n')
        if not ready.wait(45): raise TimeoutError('startup ping did not return')
        record['checks']['real_transport_ready'] = True
        code = "return string.rep('X',1048576)"
        if disconnected:
            code = "local until_time=os.clock()+0.6; while os.clock()<until_time do local s=string.rep('work',8192) end; print('U18_DISCONNECT_MUTATION'); " + code
        proc.stdin.write(json.dumps({'id':1821,'method':'eval','code':code}).encode()+b'\n')
        # Receipt comes from the extracted production queue, not a timing guess.
        record['checks']['owner_request_accepted'] = accepted.wait(3)
        if disconnected:
            record['checks']['disconnect_after_owner_started'] = started.wait(8)
            # The stdout reader is parked after the ping, with no active read.
            # Close only our consumer after the real owner entered execution.
            consumer_closed.set()
            proc.stdout.close()
        start = time.monotonic()
        try:
            proc.wait(timeout=8)
            record['checks']['exits_without_consumer_release'] = proc.returncode == 0
        except subprocess.TimeoutExpired:
            record['checks']['exits_without_consumer_release'] = False
        record['wait_seconds'] = time.monotonic()-start
    except Exception as error:
        record['error'] = repr(error)
        record['checks']['protocol_completed'] = False
    finally:
        # Relieve the pipe after the measured deadline to permit natural cleanup.
        # A later exit is useful cleanup evidence and cannot turn the test green.
        resume.set()
        proc.stdin.close()
        try: proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            record['forced_termination'] = True
            proc.kill()
            proc.wait(timeout=10)
        for reader in readers: reader.join(timeout=3)
        record['checks']['readers_joined'] = all(not r.is_alive() for r in readers)
        proc.stdout.close()
        proc.stderr.close()
        record['exit_code'] = proc.returncode
    (out/(record['name']+'.stdout.log')).write_bytes(output)
    (out/(record['name']+'.stderr.log')).write_bytes(errors)
    replies = []
    while not responses.empty(): replies.append(responses.get())
    response = next((r for r in replies if r.get('id') == 1821), {})
    record['received_result_bytes'] = len(response.get('result', ''))
    if not blocked:
        record['checks']['complete_large_reply'] = response.get('status') == 'ok' and response.get('result') == 'X'*1048576
    if disconnected:
        record['checks']['mutation_runs_once_after_disconnect'] = errors.count(b'U18_DISCONNECT_MUTATION') == 1
        record['checks']['same_request_has_one_late_completion'] = errors.count(b'[RpcRequest] id=1 op=eval phase=completed') == 1
        record['checks']['broken_output_detected'] = b'[RpcOutput] output_unavailable' in errors
    record['passed'] = all(record['checks'].values()) and not record['forced_termination']
    print(json.dumps(record))


try:
    probe(False)
    probe(True)
    probe('closed')
finally:
    report['binary_unchanged'] = hashlib.sha256(binary.read_bytes()).hexdigest() == report['binary_sha256']
    report['passed'] = len(report['cases']) == 3 and all(c.get('passed') for c in report['cases']) and report['binary_unchanged']
    (out/'report.json').write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')
if report['passed']: print('ALL STDIO BACKPRESSURE TESTS PASSED')
sys.exit(0 if report['passed'] else 1)
