"""Actual native resource, entered-worker and initialized-stall controls.

Requires an externally selected, matching ordinary Release diagnostic. Expected
rejections are retained as rejections; this lane never grants soak acceptance.
"""
import argparse
import copy
from dataclasses import asdict
import json
import os
from pathlib import Path

from engine_soak_contract import CACHES, GROWTH_BUDGETS, check_quiet
from engine_soak_trace import check_trace, _get, _set
from native_package_runtime import observe_loaded_modules
from run_engine_soak import prepare_runtime, require, run_observed_command, sha, source_identity, verify_images, write_json
from run_validation import _validate_environment
from verify_render_contracts import safe_file


def review_fault(result, events, fault, owner, observed_seconds, role):
    """Check admission and real failure witnesses with the unchanged predicates."""
    require(fault.get('role')==role and fault.get('cycle')==20, 'Fault was not admitted after warmup')
    require(result.get('pid')==owner['pid'] and result.get('process_created')==owner['created'], 'Wrong fault result owner')
    require(result.get('shutdown_host')==dict(initialized=False,running=False), 'Fault owner did not shut down')
    require(result.get('process_seconds',0)>0 and result['process_seconds']<=observed_seconds, 'Fault duration exceeds owner')
    require(result.get('warm_cycles')==20, 'Warmup contract changed')
    require(all(a['seconds']<=b['seconds'] and a['owner_frame']<=b['owner_frame'] for a,b in zip(events,events[1:])), 'Fault trace moved backwards')
    warm=[event for event in events if event['event']=='quiet' and event['cycle']<=20]
    require([event['cycle'] for event in warm]==list(range(1,21)), 'Missing original warm checkpoints')
    baseline=copy.deepcopy(warm[-1]['detail'])
    for path in (*CACHES,*GROWTH_BUDGETS):_set(baseline,path,max(_get(event['detail'],path) for event in warm))
    require(all(not check_quiet(event['detail'],baseline) for event in warm[-3:]), 'Warm baseline was already rejected')
    require(fault['seconds']>=warm[-1]['seconds'] and fault['owner_frame']>=warm[-1]['owner_frame'], 'Fault predates warm baseline')
    require(all(result['fault'].get(k)==v for k,v in fault.items()), 'Fault admission record changed')
    if role=='fault-resources':
        require(result.get('status')=='PROBE_COMPLETED' and result.get('completed_cycles')==22, 'Resource workload did not complete')
        require(type(fault.get('texture')) is int and fault['texture']>0 and fault.get('texture_valid') is True
                and type(fault.get('target')) is int and fault['target']>0, 'Resources were never admitted')
        require(not check_quiet(fault['before'],baseline), 'Resource baseline was already ineligible')
        require(result['fault'].get('retained_texture_valid') is True and result['fault'].get('resource_release_requested') is True, 'Retained resource lifetime missing')
        measured=[event for event in events if event['event']=='quiet' and event['cycle']>20]
        require([event['cycle'] for event in measured]==[21,22], 'Missing measured resource boundaries')
        rejections=[]
        for event in measured:
            sample=event['detail'];errors=check_quiet(sample,baseline)
            require(sample['render']['resources']['textures']==baseline['render']['resources']['textures']+2
                    and sample['render']['resources']['frameBuffers']==baseline['render']['resources']['frameBuffers']+1, 'Actual retained texture/RTT delta differs')
            require(any('sample.render.resources.textures:' in e for e in errors)
                    and any('sample.render.resources.frameBuffers:' in e for e in errors), 'Unchanged quiet predicate failed to reject resources')
            require(all(e.startswith(('sample.render.resources.textures:','sample.render.resources.frameBuffers:','sample.memory.textureBytes:')) for e in errors), 'Resource control encountered another failure')
            rejections.append(errors)
        trace_errors=check_trace(result,events,owner,observed_seconds,'diagnostic')
        require(len(trace_errors)==1 and trace_errors[0].startswith('Measured quiet boundary:')
                and 'sample.render.resources.textures:' in trace_errors[0], 'Ordinary trace did not reject the actual retained resources')
        return dict(baseline=baseline,quiet_rejections=rejections,trace_rejection=trace_errors)
    require(role=='fault-worker', 'Unknown fault witness role')
    require(result.get('status')=='FAIL' and result.get('completed_cycles')==20 and result.get('last_phase')==9, 'Worker did not reach its quiet failure boundary')
    require(result.get('error')=='Quiet boundary did not settle within fixed budget', 'Worker failed for another reason')
    require(type(fault.get('job_id')) is int and fault['job_id']>0 and fault.get('worker_entered') is True
            and fault['after']['jobs']['workerPending']==1, 'Worker was not admitted and entered')
    require(result['fault'].get('worker_release_requested') is True, 'Blocked worker was not released for shutdown')
    require(events[-1]['event']=='settling' and events[-1]['cycle']==20, 'Missing actual worker failure observation')
    last=events[-1];rollback=next(event for event in reversed(events) if event['event']=='rollback')
    require(last['owner_frame']-rollback['owner_frame']>120 or last['seconds']-rollback['seconds']>=4, 'Worker was not held through the original settle budget')
    errors=check_quiet(last['detail'],baseline)
    require(errors==['sample.jobs.workerPending: outstanding ownership 1'], 'Worker control did not isolate the actual pending debt')
    return dict(baseline=baseline,quiet_rejections=[errors],failure_observation=last,
                trace_scope='Partial failing trace; never rewritten as PROBE_COMPLETED')


def run_faults(repo, build, baseline_root, output, *, baseline_sha256):
    repo,build,baseline_root=(Path(p).resolve(strict=True) for p in (repo,build,baseline_root))
    output=Path(output).absolute()
    require(os.name=='nt','Native fault lane requires Windows')
    require(not output.resolve().is_relative_to(repo),'Fault evidence must be outside checkout');output.mkdir()
    report=dict(status='FAIL',accepted_soak=False,physical_audibility='NOT_MEASURED',stages=[])
    before=None
    try:
        before=source_identity(repo);report['source_before']=before
        baseline_file=safe_file(baseline_root,'run.json');baseline=json.loads(baseline_file.read_text(encoding='utf-8'))
        require(sha(baseline_file)==baseline_sha256,'Baseline differs from caller digest')
        report.update(baseline_root=str(baseline_root),baseline_run_sha256=baseline_sha256)
        require(baseline.get('status')=='DIAGNOSTIC_REVIEWED' and baseline.get('source_stable') is True
                and baseline.get('source_before')==baseline.get('source_after')==before,'Missing accepted matching diagnostic')
        profile=json.loads((repo/'scripts/validation_profiles.json').read_text(encoding='utf-8'))['profiles']['windows-release']
        cache=_validate_environment(profile,repo,build,'Release')
        for key in ('CAESURA_ENABLE_FFMPEG','CAESURA_LIVE2D','CAESURA_HAS_STEAM'):require(cache.get(key)=='OFF','Wrong foundation option: '+key)
        require(sha(build/'CMakeCache.txt')==baseline['configuration']['cache_sha256'],'Configuration changed')
        executable=(build/'tests/Release/CaesuraEngineSoakProbe.exe').resolve(strict=True);sdl=(executable.parent/'SDL3.dll').resolve(strict=True)
        binaries=baseline['binary_locks'];report['binary_locks']=binaries
        require(str(executable) in binaries and str(sdl) in binaries and all(sha(p)==v for p,v in binaries.items()),'Binary/DLL changed')
        system=Path(os.environ['SystemRoot']).resolve(strict=True);identities=set()
        for role in ('fault-resources','fault-worker','fault-stall'):
            d=output/role;d.mkdir();runtime,probe=d/'runtime',d/'probe';probe.mkdir()
            locks=prepare_runtime(repo,runtime);home,temp=d/'home',d/'temp';home.mkdir();temp.mkdir()
            stage=dict(role=role,directory=str(d),input_locks=locks,status='FAIL');report['stages'].append(stage)
            env=dict(SystemRoot=str(system),SystemDrive=system.drive,WINDIR=str(system),ComSpec=str(system/'System32/cmd.exe'),
                PATH=os.pathsep.join((str(executable.parent),str(system/'System32'))),USERPROFILE=str(home),HOME=str(home),
                ProgramData=str(home),ALLUSERSPROFILE=str(home),APPDATA=str(home/'AppData/Roaming'),LOCALAPPDATA=str(home/'AppData/Local'),TMP=str(temp),TEMP=str(temp))
            def inspect(identity):
                require(Path(identity.executable)==executable,'Wrong native owner')
                ready=json.loads((probe/'initialized.json').read_text(encoding='utf-8'));require(ready.get('pid')==identity.pid,'Wrong readiness owner')
                modules=observe_loaded_modules(identity);paths={Path(p) for p in modules['paths']}
                require(executable in paths and sdl in paths and not any(p.name.casefold()=='sdl3.dll' and p!=sdl for p in paths),'Actual probe/SDL mismatch')
                d3d=[p for p in paths if p.name.casefold()=='d3d11.dll']
                require(d3d==[(system/'System32/d3d11.dll').resolve(strict=True)],'Actual D3D11 mismatch')
                modules['required_bytes']={str(p):sha(p) for p in (executable,sdl,d3d[0])};write_json(d/'loaded-modules.json',modules)
                write_json(probe/'owner-ready.tmp',asdict(identity));(probe/'owner-ready.tmp').replace(probe/'owner-ready.json');return modules
            observed=run_observed_command([str(executable),str(runtime),str(probe),role],runtime,env,d/'process',probe/'initialized.json',probe/'events.jsonl',
                timeout=60,startup_seconds=30,progress_seconds=1 if role=='fault-stall' else 10,inspect=inspect)
            stage['observation']=observed;q=observed['receipt'];owner=q['process'];identity=(owner['pid'],owner['created'])
            require(observed['ready_observed'] is True and observed['observed_process']==owner and observed['inspection']['process']==owner,'Missing observed fault owner')
            require(identity not in identities,'Fault role reused owner');identities.add(identity)
            require(q['owned_tree_cleanup']=='COMPLETE' and q['timed_out'] is False and q['forced_kill'] is False,'Fault owner cleanup failed')
            events_file=safe_file(probe,'events.jsonl',limit=16*1024**2);events=[json.loads(line) for line in events_file.read_text(encoding='utf-8').splitlines()]
            fault_file=safe_file(probe,'fault.json');fault=json.loads(fault_file.read_text(encoding='utf-8'))
            stage.update(fault=fault,fault_sha256=sha(fault_file),events_sha256=sha(events_file))
            if role=='fault-stall':
                require(observed['status']=='FAIL' and observed.get('error')=='ValueError: Progress deadline expired after readiness','Stall did not hit the owned progress deadline')
                require(q['status']=='STOPPED' and q['stop_requested'] is True and q['actual_exit_code']!=0,'Stall was mistaken for a completed workload')
                require([x['event'] for x in events]==['initialized','fault_stall'] and fault['role']==role and fault['cycle']==0,'Stall did not preserve its initialized checkpoint')
                require(fault['after']['host']['running'] is True and fault['after']['render']['backendName']=='Direct3D 11'
                        and fault['after']['audio']['outputMode']=='Device','Stall was injected before the real owner loop')
                require(not (probe/'result.json').exists(),'Stall unexpectedly completed')
                stage['rejection']='OWNED_PROGRESS_DEADLINE'
            else:
                expected=0 if role=='fault-resources' else 1
                require(q['status']=='EXITED' and q['actual_exit_code']==expected and q['stop_requested'] is False,'Fault exited for an unexpected reason')
                require(observed['status']==('OBSERVED' if expected==0 else 'FAIL'),'Fault observation was rewritten as success')
                file=safe_file(probe,'result.json');result=json.loads(file.read_text(encoding='utf-8'))
                stage.update(result=result,result_sha256=sha(file),review=review_fault(result,events,fault,owner,observed['owner_observed_seconds'],role),pngs=verify_images(probe,events))
            require(all(sha(runtime/n)==v for n,v in locks.items()),'Fault runtime input changed')
            require(all(sha(p)==v for p,v in observed['inspection']['required_bytes'].items()),'Observed module changed')
            stage['status']='EXPECTED_REJECTION_REVIEWED';write_json(output/'run.json',report)
        require(sha(baseline_file)==baseline_sha256 and all(sha(p)==v for p,v in binaries.items()),'Baseline/binary changed')
        report['status']='NATIVE_FAULT_CONTROLS_REVIEWED'
    except Exception as error:report['error']=f'{type(error).__name__}: {error}'
    finally:
        try:
            report['source_after']=source_identity(repo);report['source_stable']=before is not None and before==report['source_after']
            if not report['source_stable']:report['status']='FAIL'
        except Exception as error:report.update(status='FAIL',source_stable=False,source_error=str(error))
        write_json(output/'run.json',report)
    return report


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo-root',type=Path,default=Path(__file__).resolve().parents[1])
    parser.add_argument('--build-dir',type=Path,required=True);parser.add_argument('--baseline-run',type=Path,required=True)
    parser.add_argument('--baseline-sha256',required=True);parser.add_argument('--run-dir',type=Path,required=True)
    args=parser.parse_args();report=run_faults(args.repo_root,args.build_dir,args.baseline_run,args.run_dir,baseline_sha256=args.baseline_sha256)
    print(json.dumps({k:report[k] for k in ('status','accepted_soak','error') if k in report}))
    return 0 if report['status']=='NATIVE_FAULT_CONTROLS_REVIEWED' else 1


if __name__=='__main__':raise SystemExit(main())
