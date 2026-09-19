#!/usr/bin/env python3
"""Read-only release-input dry run. Success never authorizes publication.

Expected context and producer outputs must come from the controlled caller,
not from downloaded artifacts. API/download injections always mark fixtures.
"""
from __future__ import annotations
import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import uuid

import aggregate_release_inputs as aggregate
from ci_package_lane import _new_work
from download_release_artifact import download_artifact
from package_verification import _sha256_file
from verify_execution_bundle import _no_links
from verify_release_inputs import GitHubAPI, _jobs, _validate_inputs

SCHEMA='caesura.release-gate.v1'
MAX_INPUT=1024*1024

class GateError(ValueError):
    """First failure, with its new attempt's original evidence retained."""


def _need(condition,message):
    if not condition:raise GateError(message)


def _digest(value):
    return isinstance(value,str) and re.fullmatch('[0-9a-f]{64}',value) is not None


def _safe(value,token):
    text=str(value)
    if isinstance(token,str) and token:text=text.replace(token,'<redacted>')
    return re.sub(r'https?://[^\s\"\'<>]+','<redacted-url>',text)[:8192]


def _read(path):
    path=_no_links(path)
    info=path.stat()
    _need(stat.S_ISREG(info.st_mode) and info.st_nlink==1,'Input must be an unlinked regular file')
    with path.open('rb') as stream:raw=stream.read(MAX_INPUT+1)
    _need(len(raw)<=MAX_INPUT,'Input exceeds size limit')
    digest=hashlib.sha256(raw).hexdigest()
    _need(_sha256_file(path)==digest,'Input changed while reading')
    def pairs(items):
        result={}
        for key,value in items:
            _need(key not in result,'Duplicate input JSON key');result[key]=value
        return result
    def invalid(_):raise GateError('Non-integer JSON number')
    try:value=json.loads(raw.decode('utf-8-sig'),object_pairs_hook=pairs,parse_constant=invalid,parse_float=invalid)
    except (UnicodeError,json.JSONDecodeError,RecursionError) as error:raise GateError('Malformed input JSON') from error
    _need(isinstance(value,dict),'Input JSON must be an object')
    return path,raw,digest,value


def _outputs(expected,policy,outputs):
    _need(policy.get('source_sha')==expected.get('source_sha'),'Policy source differs from expected source')
    roles=policy.get('artifact_roles');prefixes=policy.get('output_prefixes')
    executions=policy.get('execution_inputs');packages=policy.get('package_inputs');pages=policy.get('pages_inputs',{})
    _need(isinstance(roles,dict) and roles and isinstance(prefixes,dict) and set(prefixes)==set(roles),'Policy output roles differ')
    _need(isinstance(executions,dict) and executions and isinstance(packages,dict) and packages and isinstance(pages,dict),'Policy input roles must be mappings')
    groups=(set(executions),set(packages),set(pages))
    _need(not any(groups[i]&groups[j] for i in range(3) for j in range(i+1,3)) and set.union(*groups)==set(roles),'Policy input roles must form an exact partition')
    for role,spec in pages.items():
        _need(isinstance(spec,dict) and set(spec)=={'package_role','file'} and spec.get('file')=='artifact.tar','Pages must select the final artifact.tar')
        package_role=spec.get('package_role')
        package=packages.get(package_role) if isinstance(package_role,str) else None
        _need(isinstance(package,dict) and package.get('platform')=='web'
              and isinstance(package.get('required_files'),dict)
              and package['required_files'].get('artifact.tar')=='file','Pages needs an independently accepted Web tar')
        _need(roles[role]==roles[package_role],'Pages and accepted package must share the same producer job role')
    _need(all(isinstance(p,str) and re.fullmatch('[a-z][a-z0-9_]{0,99}',p) for p in prefixes.values()) and len(set(prefixes.values()))==len(prefixes),'Output prefixes must be distinct')
    producers={'schema_version':1,'artifacts':{}};claims={};keys=set()
    for role,prefix in prefixes.items():
        names=['artifact_id','artifact_digest','manifest_sha256']+(['receipt_sha256','run_uuid'] if role in executions else [])
        keys.update(prefix+'_'+name for name in names)
        values={name:outputs.get(prefix+'_'+name) for name in names}
        identifier=values['artifact_id']
        _need(isinstance(identifier,str) and re.fullmatch('[1-9][0-9]{0,19}',identifier),'Artifact ID must be a canonical positive decimal string')
        digest=values['artifact_digest']
        _need(isinstance(digest,str) and re.fullmatch('(?:sha256:)?[0-9a-f]{64}',digest),'Invalid artifact digest output')
        _need(_digest(values['manifest_sha256']),'Invalid manifest digest output')
        producers['artifacts'][role]=dict(artifact_id=int(identifier),artifact_digest='sha256:'+digest.removeprefix('sha256:'),manifest_sha256=values['manifest_sha256'],job_id=1,run_attempt=expected.get('run_attempt'))
        if role in executions:
            _need(_digest(values['receipt_sha256']),'Invalid execution receipt digest output')
            identifier=values['run_uuid']
            _need(isinstance(identifier,str) and re.fullmatch('[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}',identifier),'Invalid canonical execution UUIDv4')
            _need(str(uuid.UUID(identifier))==identifier,'Noncanonical execution UUID')
            claims[role]=dict(receipt_sha256=values['receipt_sha256'],run_id=identifier)
    _need(set(outputs)==keys,'Missing or extra producer output keys')
    for role,spec in pages.items():
        _need(producers['artifacts'][role]['manifest_sha256']==producers['artifacts'][spec['package_role']]['manifest_sha256'],'Pages manifest output differs from the accepted package producer')
    _validate_inputs(expected,policy,producers)
    return producers,claims


def _pages_artifacts(expected,policy,hosted,combined):
    """Describe authenticated IDs and accepted tar bytes without publishing."""
    specs=policy.get('pages_inputs',{});results=combined['pages']
    _need(set(results)==set(specs) and combined['pages_specs']==specs,'Pages selections differ from controlled policy')
    plans={}
    for role,spec in specs.items():
        result=results[role];entry=hosted['artifacts'][role]
        _need(result.get('status')=='PAGES_ARTIFACT_VERIFIED' and result.get('release_ready') is False
              and result.get('tar_name')==spec['file'] and _digest(result.get('tar_sha256'))
              and result.get('manifest_sha256')==entry['manifest_sha256']
              and result.get('source_sha')==expected['source_sha'] and result.get('version')==policy['version'],
              'Pages result differs from authenticated inputs')
        plans[role]=dict(artifact_id=entry['artifact_id'],artifact_digest=entry['artifact_digest'],
            manifest_sha256=entry['manifest_sha256'],tar_sha256=result['tar_sha256'],
            repository=expected['repository'],run_id=expected['run_id'],run_attempt=expected['run_attempt'],
            job_id=entry['job_id'],source_sha=expected['source_sha'],version=policy['version'],
            package_role=spec['package_role'],file=spec['file'],deployment='NOT_RUN',release_ready=False)
    return plans


def _inputs_stable(locks):
    for lock in locks:
        for key in ('path','snapshot'):
            path=_no_links(lock[key])
            _need(path.is_file() and path.stat().st_nlink==1 and _sha256_file(path)==lock['sha256'],'Controlled input changed after selection')


def run_release_gate(*,repo_root,expected_path,policy_path,policy_sha256,
                     producer_outputs_path,work_dir,token,release_tag=None,
                     api_factory=None,downloader=None):
    """Join actual fixed-ID downloads and reauthenticate immediately before recheck.

    Factory signature is (token, evidence_dir); downloader accepts the same
    keyword arguments as download_artifact. Either injection forces fixture
    status even if it returns a result labelled github. Work is new and outside
    every checkout. No names/globs select archives or publication inputs.
    """
    work=_new_work(work_dir)
    receipt=work/'gate.json'
    fixture=api_factory is not None or downloader is not None
    report=dict(schema=SCHEMA,status='FAIL',release_ready=False,stage='inputs',gate_receipt=str(receipt),transport='fixture' if fixture else 'github',input_locks=[],downloads={},errors=[])
    try:
        _need(isinstance(token,str) and token and not any(c in token for c in '\r\n\0'),'A nonempty read token is required')
        repo=_no_links(repo_root)
        _need((repo/'.git').exists() and not work.is_relative_to(repo) and not repo.is_relative_to(work),'Work must be outside the source checkout')
        items=[_read(path) for path in (expected_path,policy_path,producer_outputs_path)]
        _need(len({str(item[0]).casefold() for item in items})==3,'Controlled input paths must be distinct')
        _need(_digest(policy_sha256) and items[1][2]==policy_sha256,'External policy digest mismatch')
        expected,policy,outputs=(item[3] for item in items)
        producers,claims=_outputs(expected,policy,outputs)
        before=aggregate._source_identity(repo)
        _need(before.get('source_sha')==expected['source_sha'] and before.get('dirty') is False,'Wrong or dirty source checkout')
        report['source_before']=before
        for name,(path,raw,digest,_) in zip(('expected','policy','producer-outputs'),items):
            _need(token.encode() not in raw,'Controlled input contains authentication data')
            snapshot=work/(name+'.json')
            with snapshot.open('xb') as stream:stream.write(raw)
            report['input_locks'].append(dict(path=str(path),snapshot=str(snapshot),sha256=digest))
        factory=api_factory or GitHubAPI
        def api(name):
            instance=factory(token,work/name)
            _need(getattr(instance,'transport',None) in ('github','fixture'),'Unknown API transport')
            return instance
        report['stage']='resolve-jobs'
        resolver=api('api-resolve')
        resolution=dict(kind='caesura.release-job-selection.v1',status='FAIL',jobs={},errors=[])
        try:
            jobs=_jobs(resolver,'/repos/'+expected['repository'],expected,expected['run_attempt'])
            for role,name in policy['required_jobs'].items():
                matches=[job for job in jobs if job.get('name')==name]
                _need(len(matches)==1,'Required job needs one exact name: '+role)
                resolution['jobs'][role]=matches[0]['id']
            for role,entry in producers['artifacts'].items():entry['job_id']=resolution['jobs'][policy['artifact_roles'][role]]
            _validate_inputs(expected,policy,producers);resolution['status']='RESOLVED'
        except Exception as error:
            resolution['errors'].append(_safe(error,token));raise
        finally:resolver.finish(resolution)
        report['producers']=producers
        from verify_release_inputs import verify_hosted_inputs
        report['stage']='hosted-initial'
        hosted=verify_hosted_inputs(expected,policy,producers,api('api-initial'))
        fixture=fixture or resolver.transport=='fixture' or hosted['transport']=='fixture'
        initial=copy.deepcopy(hosted)
        archives={}
        for role,entry in sorted(producers['artifacts'].items()):
            report['stage']='download:'+role
            directory=work/('download-'+role)
            report['downloads'][role]=dict(receipt=str(directory/'download.json'),status='FAIL')
            result=(downloader or download_artifact)(repository=expected['repository'],artifact_id=entry['artifact_id'],expected_sha256=entry['artifact_digest'][7:],token=token,work_dir=directory)
            _need(result.get('status')=='ARTIFACT_DOWNLOADED' and result.get('artifact_id')==entry['artifact_id'] and result.get('transport') in ('github','fixture'),'Download did not verify the selected artifact')
            archive=_no_links(result['archive']['path'])
            _need(archive==directory/'artifact.zip' and _sha256_file(archive)==entry['artifact_digest'][7:],'Downloaded archive path/bytes differ')
            archives[role]=archive;fixture=fixture or result['transport']=='fixture'
            report['downloads'][role].update(status=result['status'],archive=str(archive),sha256=entry['artifact_digest'][7:])
        report['transport']='fixture' if fixture else 'github'
        hosted=copy.deepcopy(hosted);hosted['transport']=report['transport']
        report['stage']='aggregate'
        combined=aggregate.verify_downloaded_inputs(hosted=hosted,policy_path=items[1][0],policy_sha256=policy_sha256,archives=archives,execution_claims=claims,repo_root=repo,work_dir=work/'aggregate',release_tag=release_tag)
        aggregate_path=Path(combined['receipt_path']);aggregate_sha=_sha256_file(aggregate_path)
        report.update(aggregate_receipt=str(aggregate_path),aggregate_receipt_sha256=aggregate_sha)
        report['stage']='hosted-final'
        latest=verify_hosted_inputs(expected,policy,producers,api('api-final'))
        for key in ('expected','policy','jobs','artifacts','source_association','attempts'):
            _need(latest[key]==initial[key],'Hosted selection changed before preupload: '+key)
        fixture=fixture or latest['transport']=='fixture'
        report['transport']='fixture' if fixture else 'github'
        report['stage']='preupload'
        _inputs_stable(report['input_locks'])
        _need(_sha256_file(aggregate_path)==aggregate_sha,'Aggregate receipt changed before recheck')
        if fixture:
            aggregate._stable(combined)
            upload_files=combined['upload_files']
        else:upload_files=aggregate.verify_preupload(aggregate_path,aggregate_sha)['upload_files']
        _inputs_stable(report['input_locks'])
        _need(_sha256_file(aggregate_path)==aggregate_sha,'Aggregate receipt changed during recheck')
        pages_artifacts=_pages_artifacts(expected,policy,latest,combined)
        report.update(status='FIXTURE_INPUTS_VERIFIED' if fixture else 'DRY_RUN_INPUTS_VERIFIED',upload_files=upload_files,pages_artifacts=pages_artifacts,stage='complete')
    except Exception as error:
        report['errors'].append(_safe(error,token))
        raise GateError(report['errors'][-1]) from None
    finally:
        with receipt.open('x',encoding='utf-8',newline='\n') as stream:json.dump(report,stream,ensure_ascii=False,indent=2);stream.write('\n')
    return dict(report,gate_receipt_sha256=_sha256_file(receipt))


def main(argv=None,*,api_factory=None,downloader=None):
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('repo','expected','policy','producer-outputs','work'):parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--policy-sha256',required=True);parser.add_argument('--release-tag')
    args=parser.parse_args(argv);token=os.environ.get('GITHUB_TOKEN','')
    try:
        result=run_release_gate(repo_root=args.repo,expected_path=args.expected,policy_path=args.policy,policy_sha256=args.policy_sha256,producer_outputs_path=args.producer_outputs,work_dir=args.work,token=token,release_tag=args.release_tag,api_factory=api_factory,downloader=downloader)
        print(json.dumps(result,ensure_ascii=False))
        return 77 if result['status']=='FIXTURE_INPUTS_VERIFIED' else 0
    except (ValueError,RuntimeError,OSError,KeyError,TypeError) as error:
        print(json.dumps(dict(status='FAIL',release_ready=False,error=_safe(error,token)),ensure_ascii=False));return 1

if __name__=='__main__':raise SystemExit(main())
