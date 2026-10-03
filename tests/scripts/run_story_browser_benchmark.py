"""Bound the real-browser benchmark with the maintained process-tree owner."""
from pathlib import Path
import json
import os
import signal
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
from validation_process import run_owned_command


def interrupted(_signum, _frame):
    # POSIX SIGTERM must unwind through the existing process-group cleanup.
    # Windows owns a non-inheritable KILL_ON_JOB_CLOSE handle even if this
    # Python parent is terminated before it can execute a signal handler.
    raise KeyboardInterrupt('browser benchmark owner interrupted')


def main():
    if len(sys.argv) != 2 or not Path(sys.argv[1]).is_absolute():
        raise ValueError('An explicit absolute Node executable is required')
    signal.signal(signal.SIGTERM, interrupted)
    output = Path(tempfile.mkdtemp(prefix='caesura-story-owner-'))
    receipt = dict(actualExit=None, cleanupComplete=False, timedOut=False,
                   python=sys.executable, output=str(output))
    try:
        with (output / 'stdout.json').open('xb') as stdout, (output / 'stderr.log').open('xb') as stderr:
            code = run_owned_command(
                [sys.argv[1], str(ROOT / 'web/test-support/run-story-browser-benchmark.mjs')],
                cwd=ROOT, stdout=stdout, stderr=stderr, timeout=80, env=dict(os.environ))
        # This call returns only after the owned Windows job / POSIX group is
        # retired. A terminal notification by itself cannot authorize success.
        receipt.update(actualExit=code, cleanupComplete=True)
        if code != 0:
            raise RuntimeError('Browser helper failed: ' + (output / 'stderr.log').read_text(encoding='utf-8'))
        raw = (output / 'stdout.json').read_bytes()
        if len(raw) > 16 * 1024 * 1024:
            raise ValueError('Browser report exceeded its bound')
        report = json.loads(raw)
        report['owner'] = receipt
        (output / 'report.json').write_text(json.dumps(report, ensure_ascii=False) + '\n', encoding='utf-8')
        print(json.dumps(report, ensure_ascii=False))
        return 0
    except subprocess.TimeoutExpired:
        receipt['timedOut'] = True
        raise
    finally:
        (output / 'owner.json').write_text(json.dumps(receipt) + '\n', encoding='utf-8')


if __name__ == '__main__':
    raise SystemExit(main())
