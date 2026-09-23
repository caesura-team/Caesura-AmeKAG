"""Owned cold-checkpoint controls against a matching accepted native diagnostic.

The prerequisite diagnostic builds the Release probe and proves the ordinary
workload. This lane keeps its exact source/binary/configuration, then runs three
fresh processes. It does not grant short/long soak or release acceptance.
"""
import argparse
from dataclasses import asdict
import hashlib
import json
import os
from pathlib import Path

from engine_soak_trace import check_cold_trace
from native_package_runtime import observe_loaded_modules
from run_engine_soak import prepare_runtime, require, run_observed_command, sha, source_identity, write_json
from run_validation import _validate_environment
from verify_render_contracts import decode_png, safe_file


def run_cold(repo, build, baseline_root, output, *, baseline_sha256):
    repo,build,baseline_root=(Path(p).resolve(strict=True) for p in (repo,build,baseline_root))
    output=Path(output).absolute()
    require(os.name=='nt','Cold native lane requires Windows')
    require(not output.resolve().is_relative_to(repo),'Cold evidence must be outside checkout')
    output.mkdir()
    report=dict(status='FAIL',accepted_soak=False,cold_restart='NOT_RUN',corrupt_control='NOT_RUN',
                physical_audibility='NOT_MEASURED',baseline_root=str(baseline_root),stages=[])
    before=None
    try:
        before=source_identity(repo);report['source_before']=before
        baseline_file=safe_file(baseline_root,'run.json');baseline=json.loads(baseline_file.read_text(encoding='utf-8'))
        baseline_digest=sha(baseline_file);report['baseline_run_sha256']=baseline_digest
        require(baseline_digest==baseline_sha256,'Baseline differs from the controlled caller digest')
        require(baseline.get('status')=='DIAGNOSTIC_REVIEWED' and baseline.get('source_stable') is True
                and baseline.get('source_before')==baseline.get('source_after')==before,'Baseline is not an accepted matching diagnostic')
        profile=json.loads((repo/'scripts/validation_profiles.json').read_text(encoding='utf-8'))['profiles']['windows-release']
        cache=_validate_environment(profile,repo,build,'Release')
        for key in ('CAESURA_ENABLE_FFMPEG','CAESURA_LIVE2D','CAESURA_HAS_STEAM'):
            require(cache.get(key)=='OFF','Wrong foundation option: '+key)
        require(sha(build/'CMakeCache.txt')==baseline['configuration']['cache_sha256'],'Configuration differs from baseline')
        executable=(build/'tests/Release/CaesuraEngineSoakProbe.exe').resolve(strict=True)
        sdl=(executable.parent/'SDL3.dll').resolve(strict=True)
        binary_locks=baseline['binary_locks'];report['binary_locks']=binary_locks
        require(str(executable) in binary_locks and str(sdl) in binary_locks,'Baseline omitted the selected binary/SDL')
        require(all(sha(path)==digest for path,digest in binary_locks.items()),'Binary/DLL changed since baseline')
        system=Path(os.environ['SystemRoot']).resolve(strict=True)
        images={};identities=set();checkpoint=None;checkpoint_digest=None
        for role in ('cold-producer','cold-consumer','cold-corrupt'):
            directory=output/role;directory.mkdir()
            runtime,probe=directory/'runtime',directory/'probe';probe.mkdir()
            locks=prepare_runtime(repo,runtime)
            stage=dict(role=role,directory=str(directory),input_locks=locks,status='FAIL')
            report['stages'].append(stage)
            if role!='cold-producer':
                require(checkpoint is not None and sha(checkpoint)==checkpoint_digest,'Producer checkpoint changed')
                (runtime/'saves').mkdir()
                data=checkpoint.read_bytes()
                if role=='cold-corrupt':data=data[:-1]+bytes([data[-1]^1])
                save=runtime/'saves/save_39.json';save.write_bytes(data)
                locks['saves/save_39.json']=sha(save)
                stage['checkpoint_input_sha256']=sha(save)
            home,temp=directory/'home',directory/'temp';home.mkdir();temp.mkdir()
            env=dict(SystemRoot=str(system),SystemDrive=system.drive,WINDIR=str(system),
                ComSpec=str(system/'System32/cmd.exe'),PATH=os.pathsep.join((str(executable.parent),str(system/'System32'))),
                USERPROFILE=str(home),HOME=str(home),ProgramData=str(home),ALLUSERSPROFILE=str(home),
                APPDATA=str(home/'AppData/Roaming'),LOCALAPPDATA=str(home/'AppData/Local'),TMP=str(temp),TEMP=str(temp))
            def inspect(identity):
                require(Path(identity.executable)==executable,'Wrong cold executable owner')
                ready=json.loads((probe/'initialized.json').read_text(encoding='utf-8'))
                require(ready.get('pid')==identity.pid,'Cold readiness belongs to another process')
                modules=observe_loaded_modules(identity);paths={Path(path) for path in modules['paths']}
                require(executable in paths and sdl in paths,'Actual cold probe/SDL module missing')
                require(not any(p.name.casefold()=='sdl3.dll' and p!=sdl for p in paths),'Foreign SDL module observed')
                d3d=[p for p in paths if p.name.casefold()=='d3d11.dll']
                require(d3d==[(system/'System32/d3d11.dll').resolve(strict=True)],'Cold D3D11 is missing, ambiguous or foreign')
                modules['required_bytes']={str(p):sha(p) for p in (executable,sdl,d3d[0])}
                write_json(directory/'loaded-modules.json',modules)
                write_json(probe/'owner-ready.tmp',asdict(identity))
                (probe/'owner-ready.tmp').replace(probe/'owner-ready.json')
                return modules
            observed=run_observed_command([str(executable),str(runtime),str(probe),role],runtime,env,
                directory/'process',probe/'initialized.json',probe/'events.jsonl',
                timeout=60,startup_seconds=30,progress_seconds=10,inspect=inspect)
            stage['observation']=observed
            require(observed['status']=='OBSERVED',observed.get('error','Cold process observation failed'))
            owner=observed['receipt']['process'];identity=(owner['pid'],owner['created'])
            require(identity not in identities,'Cold roles reused one process identity');identities.add(identity)
            result_file=safe_file(probe,'result.json');events_file=safe_file(probe,'events.jsonl',limit=1024**2)
            result=json.loads(result_file.read_text(encoding='utf-8'))
            events=[json.loads(line) for line in events_file.read_text(encoding='utf-8').splitlines()]
            errors=check_cold_trace(result,events,owner,observed['owner_observed_seconds'],role)
            require(not errors,'; '.join(errors))
            stage.update(result=result,result_sha256=sha(result_file),events_sha256=sha(events_file),pngs=[])
            used=set()
            for event in events:
                if event['event']!='capture_consumed':continue
                detail=event['detail'];file=safe_file(probe,detail['file'],used);encoded=file.read_bytes()
                require(len(encoded)==detail['png_bytes'],'Cold PNG byte count differs')
                width,height,pixels=decode_png(encoded)
                require((width,height)==(640,360),'Cold screenshot geometry differs')
                color=(20,50,90) if detail['page'] in ('a','restored') else (130,35,50);offset=(10*width+10)*4
                require(all(abs(pixels[offset+i]-color[i])<=2 for i in range(3)),'Cold actual background pixel differs')
                images[(role,detail['page'])]=pixels
                stage['pngs'].append(dict(file=detail['file'],sha256=sha(file),rgba_sha256=hashlib.sha256(pixels).hexdigest()))
            save=safe_file(runtime,'saves/save_39.json',used)
            if role=='cold-producer':
                checkpoint=safe_file(probe,'checkpoint.caes',used);data=checkpoint.read_bytes()
                require(data.startswith(b'CAES') and len(data)>32 and b'soak-encrypted-checkpoint' not in data,'Producer did not retain encrypted CAES')
                checkpoint_digest=sha(checkpoint)
                require(sha(save)==checkpoint_digest,'Producer retained bytes differ from its save')
                saved=next(x['detail'] for x in events if x['event']=='cold_saved')
                require(saved['bytes']==len(data),'Reported checkpoint byte count differs')
                report['checkpoint']=dict(path=str(checkpoint),sha256=checkpoint_digest,bytes=len(data))
            else:
                require(sha(save)==stage['checkpoint_input_sha256'],'Consumer rewrote its checkpoint')
                require((sha(save)==checkpoint_digest)==(role=='cold-consumer'),'Cold input substitution differs')
                if role=='cold-corrupt':
                    require(isinstance(result.get('cold_error'),str) and result['cold_error'],'Missing actual load rejection reason')
            require(all(sha(runtime/name)==digest for name,digest in locks.items()),'Cold runtime input changed')
            require(all(sha(path)==digest for path,digest in observed['inspection']['required_bytes'].items()),'Observed cold module changed')
            stage['status']='ROLE_REVIEWED'
            write_json(output/'run.json',report)
        require(images[('cold-producer','a')]==images[('cold-consumer','restored')],'Cold restored full pixels differ from producer')
        require(images[('cold-consumer','before')]!=images[('cold-consumer','restored')],'Cold load did not change the page')
        require(images[('cold-corrupt','before')]==images[('cold-corrupt','after')],'Rejected load changed full pixels')
        require(sha(checkpoint)==checkpoint_digest and sha(baseline_file)==baseline_digest,'Producer/baseline evidence changed')
        require(all(sha(path)==digest for path,digest in binary_locks.items()),'Binary/DLL changed during cold controls')
        report.update(status='COLD_CONTROLS_REVIEWED',cold_restart='OBSERVED',corrupt_control='REJECTED_WITH_OLD_PAGE')
    except Exception as error:
        report['error']=f'{type(error).__name__}: {error}'
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
    parser.add_argument('--build-dir',type=Path,required=True)
    parser.add_argument('--baseline-run',type=Path,required=True)
    parser.add_argument('--baseline-sha256',required=True,help='Original accepted diagnostic digest selected by the caller')
    parser.add_argument('--run-dir',type=Path,required=True)
    args=parser.parse_args();report=run_cold(args.repo_root,args.build_dir,args.baseline_run,args.run_dir,baseline_sha256=args.baseline_sha256)
    print(json.dumps({k:report[k] for k in ('status','accepted_soak','error') if k in report}))
    return 0 if report['status']=='COLD_CONTROLS_REVIEWED' else 1


if __name__=='__main__':raise SystemExit(main())
