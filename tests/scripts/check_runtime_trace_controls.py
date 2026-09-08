"""Negative controls derived from the just-executed corpus, not canned traces."""
import copy
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'scripts'))
from compare_runtime_contracts import compare_traces


def main(paths):
    measured = [json.loads(Path(p).read_text(encoding='utf-8')) for p in paths]
    assert not compare_traces(measured), 'positive measured baseline must pass first'
    controls = ('variable', 'dialogue order', 'caller frame', 'ending', 'missing lane', 'save replay')
    for name in controls:
        candidate = copy.deepcopy(measured)
        web = next(document for document in candidate if document['runtime'] == 'web')
        calls = next(run for run in web['runs'] if run['case'] == 'calls')
        if name == 'variable':
            calls['trace'][-1]['variables']['total'] += 1
        elif name == 'dialogue order':
            calls['trace'][0], calls['trace'][1] = calls['trace'][1], calls['trace'][0]
        elif name == 'caller frame':
            calls['trace'][0]['callers'] = {}
        elif name == 'ending':
            calls['trace'].pop()
        elif name == 'missing lane':
            web['runs'] = [run for run in web['runs'] if run['lane'] != 'bundle']
        else:
            next(run for run in web['runs'] if run['case'] == 'restore')['replay'] = None
        assert compare_traces(candidate), 'negative control accepted: ' + name
        print('PASS: rejected changed ' + name)
    assert not compare_traces(measured), 'negative controls must not alter original measurements'
    print('PASS: 6 negative controls and unchanged positive measurements')


if __name__ == '__main__':
    main(sys.argv[1:])
