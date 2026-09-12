"""Real Engine/Lua/runner error boundaries, with bounded waits and owned PIDs."""
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
       Path.cwd() / 'artifacts' / 'validation' / ('runtime-errors-' + str(uuid.uuid4()))).resolve()
out.mkdir(parents=True, exist_ok=False)
report = {"binary": str(binary), "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(), "cases": []}

frame_code = """
local old=engine_update
local attempts=0
engine_update=function(dt)
  attempts=attempts+1
  if attempts<=20 then error('U18_FRAME_REPEAT:'..attempts,0) end
  engine_update=old
  print('U18_FRAME_RECOVERED:'..attempts)
end
return 'installed'
"""

second_frame_code = """
local old=engine_update
local attempts=0
engine_update=function(dt)
  attempts=attempts+1
  if attempts<=2 then error('U18_FRAME_SECOND:'..attempts,0) end
  engine_update=old
  print('U18_SECOND_RECOVERED:3')
end
return 'installed-again'
"""

def command_code(fail):
    body = "error('U18_COMMAND_FAILURE',0)" if fail else "print('U18_COMMAND_POSITIVE')"
    return """
local runner=require('kag_runner')
assert(runner.stop())
local kag=require('kag')
local flow=require('flow')
local tokenize=require('tokenizer')
kag.u18fail=function(ctx,params) BODY end
kag.u18after=function(ctx,params) print('U18_AFTER_COMMAND') end
local old=flow.load_scene
flow.load_scene=function(path)
  assert(path=='u18-command.ks')
  return {tokens=tokenize.parse('[p]\\n[u18fail]\\n[u18after]'), labels={}}
end
local ok,reason=runner.start('u18-command.ks')
flow.load_scene=old
assert(ok,reason)
runner.update(0.016)
assert(runner.get_ctx().waiting_input)
local accepted,reason=runner.on_click()
print('U18_COMMAND_RESULT:'..tostring(accepted)..':'..tostring(reason))
return 'dispatched'
""".replace("BODY", body)


def probe(name, code, recovery_marker=None, fatal=False):
    lines = []
    error_lines = []
    incoming = queue.Queue()
    diagnostics = queue.Queue()
    env = dict(os.environ)
    env.pop('CAESURA_TEST_STALL_MS', None)
    env['CAESURA_RPC_DISPATCH_TIMEOUT_MS'] = '5000'
    record = {"name": name, "checks": {}, "forced_termination": False}
    report['cases'].append(record)
    error_path = out / (name + '.stderr.log')
    with error_path.open('w', encoding='utf-8') as err:
        proc = subprocess.Popen([str(binary), '--headless'], cwd=binary.parent, env=env,
                                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                text=True, encoding='utf-8', errors='replace', bufsize=1)
        record['pid'] = proc.pid
        def consume(stream, captured, pending):
            for line in stream:
                captured.append(line)
                pending.put(line)
            pending.put(None)
        readers = [threading.Thread(target=consume, args=(proc.stdout, lines, incoming), daemon=True),
                   threading.Thread(target=consume, args=(proc.stderr, error_lines, diagnostics), daemon=True)]
        for reader in readers: reader.start()
        def next_until(predicate, timeout, pending=incoming):
            deadline=time.monotonic()+timeout
            while True:
                remaining=deadline-time.monotonic()
                if remaining<=0: raise TimeoutError('bounded stdout wait expired')
                line=pending.get(timeout=remaining)
                if line is None: raise RuntimeError('stdout closed before expected observation')
                if predicate(line): return line
        def request(payload):
            proc.stdin.write(json.dumps(payload)+'\n'); proc.stdin.flush()
            def match(line):
                try: value=json.loads(line)
                except ValueError: return False
                return isinstance(value,dict) and value.get('id')==payload['id']
            return json.loads(next_until(match,45))
        try:
            record['checks']['transport_ready']=request({'id':1810,'method':'ping'}).get('result')=='ok'
            response=request({'id':1811,'method':'eval','code':code})
            record['response']=response
            record['checks']['fixture_installed']=response.get('status')=='ok'
            if recovery_marker:
                next_until(lambda line: recovery_marker in line,5,diagnostics)
                record['checks']['transient_callback_recovers']=True
                again=request({'id':1812,'method':'eval','code':second_frame_code})
                record['checks']['second_episode_installed']=again.get('result')=='installed-again'
                next_until(lambda line: 'U18_SECOND_RECOVERED:3' in line,5,diagnostics)
                record['checks']['second_episode_recovers']=True
            if not fatal or not record['checks']['fixture_installed']: proc.stdin.close()
            proc.wait(timeout=10)
            for reader in readers: reader.join(timeout=3)
            record['checks']['natural_exit']=proc.returncode==0 and all(not reader.is_alive() for reader in readers)
        except Exception as error:
            record['error']=repr(error)
            record['checks']['protocol_completed']=False
        finally:
            if proc.poll() is None:
                record['forced_termination']=True
                proc.kill(); proc.wait(timeout=10)
            if not proc.stdin.closed: proc.stdin.close()
            for reader in readers: reader.join(timeout=3)
            proc.stdout.close(); proc.stderr.close()
            err.write(''.join(error_lines))
            record['exit_code']=proc.returncode
    (out/(name+'.stdout.log')).write_text(''.join(lines),encoding='utf-8')
    errors=error_path.read_text(encoding='utf-8', errors='replace')
    if name=='frame-error':
        count=errors.count('engine_update (recoverable): U18_FRAME_REPEAT')
        record['error_log_count']=count
        record['checks']['frame_errors_reported']=count>=1
        record['checks']['repeated_frame_errors_bounded']=count<=3
        record['checks']['new_episode_reports_again']=errors.count('engine_update (recoverable): U18_FRAME_SECOND')==2
        record['checks']['recovery_summaries']='recovered after 20 consecutive errors' in errors and 'recovered after 2 consecutive errors' in errors
    else:
        after=errors.count('U18_AFTER_COMMAND')
        record['after_command_count']=after
        record['checks']['later_command_stopped' if fatal else 'healthy_later_command_runs_once']=after==(0 if fatal else 1)
        if fatal: record['checks']['real_error_ui_chain']='Engine Runtime Error' in errors
        if fatal: record['checks']['failed_click_reported']='U18_COMMAND_RESULT:false:command-error' in errors
    record['passed']=all(record['checks'].values()) and not record['forced_termination']
    print(json.dumps(record,ensure_ascii=False))

try:
    probe('frame-error',frame_code,recovery_marker='U18_FRAME_RECOVERED:21')
    probe('command-positive',command_code(False))
    probe('command-fatal',command_code(True),fatal=True)
finally:
    report['binary_unchanged']=hashlib.sha256(binary.read_bytes()).hexdigest()==report['binary_sha256']
    report['passed']=len(report['cases'])==3 and all(c.get('passed',False) for c in report['cases']) and report['binary_unchanged']
    (out/'report.json').write_text(json.dumps(report,indent=2,ensure_ascii=False)+'\n',encoding='utf-8')
print('Runtime error evidence: ' + str(out / 'report.json'))
if report['passed']: print('ALL HEADLESS ERROR TESTS PASSED')
sys.exit(0 if report['passed'] else 1)
