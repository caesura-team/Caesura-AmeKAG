"""Compare measured U13 traces, reusing the platform parity comparison rules."""
import argparse
import json
from pathlib import Path
from compare_platform_parity import compare_route, sanitize_check_obj

CASES = {'language', 'macros', 'calls', 'choices', 'timing', 'restore'}
LANES = {'native': {'source', 'ast', 'cache'}, 'web': {'source', 'bundle'}}


def compare_traces(documents):
    errors, reference, seen_runtimes = [], {}, set()
    for document in documents:
        runtime = document.get('runtime')
        if document.get('version') != 1 or runtime not in LANES or runtime in seen_runtimes:
            errors.append('unsupported or duplicate runtime document')
            continue
        seen_runtimes.add(runtime)
        runs = document.get('runs')
        if not isinstance(runs, list) or not runs:
            errors.append(f'{runtime}: missing measured runs')
            continue
        identities, present = set(), set()
        for run in runs:
            case, lane, dt = run.get('case'), run.get('lane'), run.get('dt')
            identity = (case, lane, dt)
            if case not in CASES or lane not in LANES[runtime] or identity in identities:
                errors.append(f'{runtime}: invalid/duplicate run {identity}')
                continue
            identities.add(identity)
            present.add((case, lane))
            trace = run.get('trace')
            if not isinstance(trace, list) or len(trace) < 2 or trace[-1].get('kind') != 'end':
                errors.append(f'{identity}: missing events or normal ending')
                continue
            value = {'trace': trace, 'replay': run.get('replay')}
            errors.extend(sanitize_check_obj(value))
            if case == 'restore' and not isinstance(value['replay'], list):
                errors.append(f'{identity}: missing save replay')
            reference.setdefault(case, value)
            errors.extend(compare_route(value, reference[case], f'{runtime}/{case}/{lane}/{dt}'))
        if present != {(case, lane) for case in CASES for lane in LANES[runtime]}:
            errors.append(f'{runtime}: incomplete corpus/lane coverage')
        if runtime == 'native' and identities != {(c, l, dt) for c in CASES for l in LANES[runtime]
                                                  for dt in (0.007, 0.031)}:
            errors.append('native: both required dt runs must be present')
    if seen_runtimes != set(LANES):
        errors.append('both native and web measurements are required')
    return errors


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('traces', nargs='+', type=Path)
    args = parser.parse_args()
    try:
        errors = compare_traces([json.loads(p.read_text(encoding='utf-8')) for p in args.traces])
    except (ValueError, OSError, TypeError, AttributeError) as error:
        errors = [str(error)]
    for error in errors:
        print('FAIL: ' + error)
    if not errors:
        print('PASS: native source/AST/cache and Web source/bundle events and save replay agree')
    return 1 if errors else 0


if __name__ == '__main__':
    raise SystemExit(main())
