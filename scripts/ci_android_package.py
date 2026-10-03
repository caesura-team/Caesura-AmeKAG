#!/usr/bin/env python3
"""Hosted Android package gate using existing JNI and shared TEST-signing checks.

This lane resolves Gradle dependencies online. It does not claim the full
run_android_validation offline build provenance or device execution.
"""
from __future__ import annotations
import argparse, hashlib, json, os, re, shutil, sys
from pathlib import Path
import run_android_validation as driver
import android_package_contract as package
import ci_jdk_inputs as jdk_inputs
from ci_package_lane import _new_work, _outputs
from verify_execution_bundle import _no_links

SCHEMA = 'caesura.ci-android-package.v1'
BUNDLETOOL_URL = 'https://github.com/google/bundletool/releases/download/1.17.1/bundletool-all-1.17.1.jar'
# Official release asset 181329951, downloaded 2026-09-30 over HTTPS. The old
# GitHub asset has no publisher digest; this pins those observed original bytes.
BUNDLETOOL_SHA256 = '45881ead13388872d82c4255b195488b7fc33f2cac5a9a977b0afc5e92367592'
need, lock, save = driver.need, driver.lock, driver.save


def verify_bundletool(path):
    selected = lock(Path(path).resolve(strict=True))
    need(selected['sha256'] == BUNDLETOOL_SHA256, 'Unpinned bundletool bytes')
    return selected


def _tool_failure(error, component, root=None, paths=()):
    """Add diagnostics to the original rejection; never replace its predicate."""
    if hasattr(error, 'tool_selection_failure'):
        return
    details=dict(component=component, requested_root=str(root) if root is not None else None,
                 selected_paths=list(paths), error_type=type(error).__name__, error=str(error))
    try:
        trace=error.__traceback__
        while trace:
            frame=trace.tb_frame
            if frame.f_code is driver._no_links.__code__:
                # Read only this known validator's path locals, never arbitrary
                # frame/environment contents. Metadata is observed after rejection.
                value=frame.f_locals.get('value')
                refused=frame.f_locals.get('item')
                path=Path(value);chain=[]
                for item in (path,*path.parents):
                    row=dict(path=str(item))
                    try:
                        info=item.lstat()
                        row.update(mode=info.st_mode,nlink=info.st_nlink,
                                   is_symlink=item.is_symlink(),is_junction=item.is_junction())
                        if row['is_symlink']:
                            row['link_target']=os.readlink(item)
                            try:row['resolved_target']=str(item.resolve(strict=True))
                            except OSError as resolution:row['resolution_error']=str(resolution)
                    except OSError as inspection:row['inspection_error']=str(inspection)
                    chain.append(row)
                details['path_check']=dict(requested_path=str(value),rejected_ancestor=str(refused),
                    ancestors=chain,metadata_timing='observed after the unchanged validator rejected the path')
                break
            trace=trace.tb_next
    except Exception as diagnostic:
        details['diagnostic_error']=type(diagnostic).__name__+': '+str(diagnostic)
    error.tool_selection_failure=details


def _component(root, paths, name='component'):
    try:
        root = Path(root).resolve(strict=True)
        return dict(root=str(root), paths=paths, **driver._tree(root, paths))
    except Exception as error:
        _tool_failure(error,name,root,paths)
        raise


def _tool_lock(path, component):
    try:return lock(path)
    except Exception as error:
        _tool_failure(error,component,path)
        raise


def _select_precompile_tools(jdk, sdk, ndk, gradle, bundletool, *, fixture):
    # Used by both the early diagnostic and the final package lane. SDL is not
    # fabricated here: the final lane separately requires its real built slice.
    components = dict(jdk=_component(jdk, ['bin', 'lib', 'conf', 'release'], 'jdk'),
        sdk=_component(sdk, ['build-tools/34.0.0', 'platforms/android-35'], 'sdk'),
        ndk=_component(ndk, ['source.properties'], 'ndk'), gradle=_component(gradle, ['lib'], 'gradle'))
    need(re.search(r'^JAVA_VERSION="17(?:\.|\")', (Path(components['jdk']['root'])/'release').read_text(), re.M), 'JDK17 required')
    for role, rel, pattern in [('ndk','source.properties',r'Pkg.Revision\s*=\s*27\.3\.13750724'),
            ('sdk','build-tools/34.0.0/source.properties',r'Pkg.Revision\s*=\s*34\.0\.0')]:
        need(re.search(pattern, (Path(components[role]['root'])/rel).read_text()), 'Wrong selected '+role)
    suffix = '.exe' if os.name == 'nt' and not fixture else ''
    jdk = Path(components['jdk']['root']); bt = Path(components['sdk']['root'])/'build-tools/34.0.0'
    tools = {name:_tool_lock(jdk/'bin'/(name+suffix),'jdk') for name in ('java','keytool','jarsigner')}
    tools.update({name:_tool_lock(bt/(name+suffix),'sdk') for name in ('aapt2','zipalign')})
    tools['apksigner_jar'] = _tool_lock(bt/'lib/apksigner.jar','sdk')
    try:tools['bundletool_jar'] = lock(bundletool) if fixture else verify_bundletool(bundletool)
    except Exception as error:
        _tool_failure(error,'bundletool',bundletool)
        raise
    git = shutil.which('git'); need(git, 'Git required')
    tools['git'] = _tool_lock(Path(git).resolve(strict=True),'git')
    return dict(components=components, tools=tools)


def _select_tools(jdk, sdk, ndk, gradle, sdl, bundletool, *, fixture):
    result=_select_precompile_tools(jdk,sdk,ndk,gradle,bundletool,fixture=fixture)
    result['components']['sdl']=_component(sdl,['lib/libSDL3.so'],'sdl')
    return result


def _bind_jdk_inputs(root, receipt, digest, *, fixture):
    need(receipt is not None and digest is not None,'Prepared JDK source receipt and SHA required')
    verified=jdk_inputs.verify_jdk_inputs(receipt,digest,allow_fixture=fixture)
    need(str(_no_links(root))==verified['mirror_root'],'Selected JDK root differs from prepared ordinary mirror')
    binding=dict(receipt_path=str(receipt),receipt_sha256=digest,mirror_root=verified['mirror_root'])
    original=driver.load(receipt,digest)
    if original.get('external'):
        binding['external_receipt']=dict(path=original['external']['receipt_path'],sha256=original['external']['receipt_sha256'])
    return binding


def preflight_toolchain(*,jdk_root,sdk_root,ndk_root,gradle_root,bundletool_jar,jdk_input_receipt=None,jdk_input_sha256=None,fixture=False):
    binding=_bind_jdk_inputs(jdk_root,jdk_input_receipt,jdk_input_sha256,fixture=fixture)
    selected=_select_precompile_tools(jdk_root,sdk_root,ndk_root,gradle_root,bundletool_jar,fixture=fixture)
    return dict(status='FIXTURE_TOOLCHAIN_PREFLIGHT' if fixture else 'CI_TOOLCHAIN_PREFLIGHT_VERIFIED',
        release_ready=False,commands=[],sdl='REQUIRES_POST_BUILD_VALIDATION',
        scope='Selected precompile tool files only; no build, package, signature or device acceptance',
        components={name:dict(root=item['root'],paths=item['paths'],files=len(item['files']),
                    directories=len(item['directories'])) for name,item in selected['components'].items()},
        tools=selected['tools'],jdk_inputs=binding)


def _stable(report):
    binding=report['jdk_inputs']
    verified=jdk_inputs.verify_jdk_inputs(binding['receipt_path'],binding['receipt_sha256'],allow_fixture=report.get('fixture') is True)
    need(verified['mirror_root']==report['toolchain']['components']['jdk']['root'],'JDK provenance mirror differs')
    package._file(binding['evidence_copy']['path'],binding['receipt_sha256'])
    if binding.get('external_receipt'):
        package._file(binding['external_evidence_copy']['path'],binding['external_receipt']['sha256'])
    for selected in [report['native']['library'], *report['outputs'].values(), *report['toolchain']['tools'].values()]:
        package._file(selected['path'], selected['sha256'])
    for component in report['toolchain']['components'].values():
        need(driver._tree(Path(component['root']), component['paths']) == {k:component[k] for k in ('files','directories')}, 'Selected runtime dependency changed')
    need(driver._source(Path(report['repo']), report['toolchain']['tools']['git']['path'],
                        driver._env(report['toolchain'], Path(report['work']))) == report['source'], 'Source changed')
    need(driver._stage_snapshot(Path(report['staging']['path'])) == report['staging']['snapshot'], 'Staging changed')
    for item in report['unsigned'].values():
        for key in ('file','producer_file'):package._file(item[key]['path'],item[key]['sha256'])
        driver.verify_stable(item['prepared'])
    for command in report['commands']:
        for selected in [command['stdout'],command['stderr'],*command['process_files']]:
            package._file(selected['path'],selected['sha256'])
    package.verify_android_package_stable(report['package'])
    need(driver._signature_closure(report) == report['signature_transform'], 'Signature closure changed')
    need(report['private_cleanup']=='COMPLETE' and not (Path(report['work'])/'private-signing').exists(), 'Private signing cleanup incomplete')


def verify_ci_android_package_stable(result):
    need(result.get('schema')==SCHEMA and result.get('status') in ('CI_ANDROID_PACKAGE_VERIFIED','FIXTURE_ONLY')
         and result.get('release_ready') is False, 'Completed CI Android package result required')
    saved=driver.load(result['receipt_path'],result['receipt_sha256'])
    need(saved=={k:v for k,v in result.items() if k!='receipt_sha256'}, 'CI receipt differs')
    _stable(result)
    work=Path(result['work']);proof=work/'proof'
    expected=[]
    for path in _proof_sources(result):
        record=lock(path);rel=path.relative_to(work)
        package._file(proof/rel,record['sha256'])
        expected.append(dict(file=rel.as_posix(),sha256=record['sha256']))
    actual=json.loads((proof/'inventory.json').read_text(encoding='utf-8'))
    need(actual==dict(files=expected,scope='Original small evidence; final APK/AAB bytes are separate artifacts'),'Proof inventory differs')
    need(set(driver._tree(proof,['.'])['files'])=={x['file'] for x in expected}|{'inventory.json'},'Proof has unexpected files')
    return dict(status='CI_ANDROID_PACKAGE_STABLE',release_ready=False)


def _proof_sources(report):
    work=Path(report['work'])
    selected=[Path(report['receipt_path'])]
    if report.get('jdk_inputs',{}).get('evidence_copy'):
        selected.append(Path(report['jdk_inputs']['evidence_copy']['path']))
        if report['jdk_inputs'].get('external_evidence_copy'):
            selected.append(Path(report['jdk_inputs']['external_evidence_copy']['path']))
    for command in report.get('commands',[]):
        selected.extend(Path(command[k]['path']) for k in ('stdout','stderr'))
        selected.extend(Path(v['path']) for v in command['process_files'])
    check=report.get('package')
    if check:
        selected.extend([Path(check['receipt_path']),Path(check['control']['path'])])
        for command in check['commands']:
            selected.extend(Path(command[k]['path']) for k in ('stdout','stderr'))
            selected.extend(Path(v['path']) for v in command['process_files'])
    elif (work/'verify/android-package.json').is_file():
        selected.append(work/'verify/android-package.json')
        # A failed verifier still owns its tool diagnostics.
        selected.extend((work/'verify').glob('*.stdout'))
        selected.extend((work/'verify').glob('*.stderr'))
        selected.extend((work/'verify').glob('*-process/*.json'))
    certificate=work/'outputs/test-certificate.der'
    if certificate.is_file():selected.append(certificate)
    return sorted(set(selected))


def _proof(report):
    """Copy original small receipts/logs, never keys or expanded archive payloads."""
    work=Path(report['work']); proof=work/'proof'; proof.mkdir()
    inventory=[]
    for path in _proof_sources(report):
        need(path.is_relative_to(work) and 'private-signing' not in path.parts, 'Unsafe proof selection')
        record=lock(path);rel=path.relative_to(work);target=proof/rel
        driver._copy(path,target,record['sha256']);inventory.append(dict(file=rel.as_posix(),sha256=record['sha256']))
    save(proof/'inventory.json',dict(files=inventory,scope='Original small evidence; final APK/AAB bytes are separate artifacts'))


def run_ci_android_package(*,repo,source_sha,native_library,sdl_root,jdk_root,sdk_root,ndk_root,
                           gradle_root,bundletool_jar,work_dir,jdk_input_receipt=None,jdk_input_sha256=None,runner=None):
    work=_new_work(work_dir); receipt=work/'ci-android-package.json'
    report=dict(schema=SCHEMA,status='FAIL',release_ready=False,device='NOT_RUN',runtime='NOT_RUN',install='NOT_RUN',
        work=str(work),repo=str(Path(repo).resolve(strict=True)),receipt_path=str(receipt),commands=[],errors=[],
        private_cleanup='NOT_CREATED',dependency_scope='HOSTED_ONLINE_GRADLE_NOT_OFFLINE_DRIVER',
        native_provenance='EXISTING_JNI_BYTES_NOT_REBUILT_BY_THIS_CONTROLLER',
        producer_provenance='CALLER_MUST_AUTHENTICATE_JOB_SOURCE_RUN_ATTEMPT',
        limits=['Selected JDK/SDK/Gradle files inventoried, not a hermetic host',
                'NDK source.properties checked; this controller does not attest the prior compiler process',
                'AAB semantics cover base identity/version/SDK only; signatures use an ephemeral TEST key'])
    try:
        for name in ('home','tmp','commands','gradle-home','unsigned','aligned','outputs'):(work/name).mkdir()
        need(re.fullmatch('[0-9a-f]{40}',source_sha or ''), 'Full source SHA required')
        report['stage']='jdk-provenance'
        report['fixture']=runner is not None
        binding=_bind_jdk_inputs(jdk_root,jdk_input_receipt,jdk_input_sha256,fixture=runner is not None)
        copied=work/'jdk-source-receipt.json'
        driver._copy(binding['receipt_path'],copied,binding['receipt_sha256'])
        binding['evidence_copy']=lock(copied)
        if binding.get('external_receipt'):
            external_copy=work/'jdk-cacerts-source-receipt.json';external=binding['external_receipt']
            driver._copy(external['path'],external_copy,external['sha256'])
            binding['external_evidence_copy']=lock(external_copy)
        report['jdk_inputs']=binding
        report['stage']='tool-selection'
        tc=_select_tools(jdk_root,sdk_root,ndk_root,gradle_root,sdl_root,bundletool_jar,fixture=runner is not None)
        report['toolchain']=tc;env=driver._env(tc,work)
        source=driver._source(Path(report['repo']),tc['tools']['git']['path'],env)
        need(source['source_sha']==source_sha,'Source HEAD differs')
        report['source']=source
        versions=re.findall(r'project\s*\(\s*CaesuraAmeKAG\s+VERSION\s+(\d+\.\d+\.\d+)',
                            (Path(report['repo'])/'CMakeLists.txt').read_text(encoding='utf-8'),re.I)
        need(len(versions)==1,'Exactly one controlled engine version required')
        value=dict(repo=report['repo'],source_sha=source_sha,version_name=versions[0],version_code=1,
            package_name='com.caesura.app',abi='arm64-v8a',min_sdk=24,target_sdk=35,
            game_relative_path='tests/projects/first_vn',jobs=3,timeouts=dict(gradle=900,sign=120,verify=120))
        report['inputs']=value
        library=lock(native_library);package._elf(Path(library['path']));report['native']=dict(library=library)
        report['staging']=driver._stage(value,source,report['native'],tc,dict(files={}),work)
        command=driver.Commands(report,work,env,runner)
        text=command.run('java-version',[tc['tools']['java']['path'],'-version'],60)
        need(re.search(r'\bversion "17(?:\.|\")',text),'Actual Java is not JDK17')
        report['unsigned']=driver._gradle(value,tc,report['staging'],work,command,offline=False,include_debug=True)
        report['outputs']=driver._sign(value,tc,report['unsigned'],command,work,report)
        expected={k:value[k] for k in ('source_sha','package_name','version_name','version_code','abi','min_sdk','target_sdk')}
        expected.update(certificate_sha256=report['outputs']['certificate']['sha256'],
            required_apk_entries=report['unsigned']['apk']['entries'],required_aab_entries=report['unsigned']['aab']['entries'])
        report['package']=package.verify_android_package(apk_path=report['outputs']['apk']['path'],
            apk_sha256=report['outputs']['apk']['sha256'],aab_path=report['outputs']['aab']['path'],
            aab_sha256=report['outputs']['aab']['sha256'],expected=expected,
            tools={k:tc['tools'][k] for k in package.TOOL_NAMES},work_dir=work/'verify',runner=runner,timeout=120)
        report['signature_transform']=driver._signature_closure(report)
        driver._cleanup_private(work,report);_stable(report)
        report['status']='FIXTURE_ONLY' if runner is not None else 'CI_ANDROID_PACKAGE_VERIFIED'
    except Exception as error:
        report['errors'].append(str(error))
        if hasattr(error,'tool_selection_failure'):report['tool_selection_failure']=error.tool_selection_failure
        raise
    finally:
        try:
            if (work/'private-signing').exists() and report['private_cleanup']!='FAILED':driver._cleanup_private(work,report)
        except Exception as error:
            report.update(status='FAIL',private_cleanup='FAILED');report['errors'].append('Private cleanup: '+str(error))
        save(receipt,report);_proof(report)
    need(report['status']!='FAIL','Private cleanup failed')
    return dict(report,receipt_sha256=package._sha256_file(receipt))


def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__);sub=parser.add_subparsers(dest='mode',required=True)
    preflight=sub.add_parser('preflight')
    for name in ('jdk-root','sdk-root','ndk-root','gradle-root','bundletool-jar','jdk-input-receipt','jdk-input-sha256'):
        preflight.add_argument('--'+name,required=True)
    run=sub.add_parser('run')
    for name in ('repo','source-sha','native-library','sdl-root','jdk-root','sdk-root','ndk-root','gradle-root','bundletool-jar','work','jdk-input-receipt','jdk-input-sha256'):
        run.add_argument('--'+name,required=True)
    run.add_argument('--github-output')
    verify=sub.add_parser('verify');verify.add_argument('--receipt',required=True);verify.add_argument('--sha256',required=True)
    args=parser.parse_args(argv)
    try:
        if args.mode=='preflight':
            values=vars(args).copy();values.pop('mode')
            print(json.dumps(preflight_toolchain(**values)));return 0
        if args.mode=='verify':
            result=driver.load(args.receipt,args.sha256);result['receipt_sha256']=args.sha256
            need(result.get('status')=='CI_ANDROID_PACKAGE_VERIFIED','Real CI package evidence required')
            print(json.dumps(verify_ci_android_package_stable(result)));return 0
        values=vars(args).copy();values.pop('mode');output=values.pop('github_output');values['work_dir']=values.pop('work')
        result=run_ci_android_package(**values)
        verify_ci_android_package_stable(result)
        _outputs(Path(output) if output else None,dict(receipt=result['receipt_path'],receipt_sha256=result['receipt_sha256'],
            upload_files='\n'.join(result['outputs'][kind]['path'] for kind in ('apk','aab')),proof=str(Path(result['work'])/'proof')))
        print(json.dumps({k:result[k] for k in ('status','receipt_path','receipt_sha256','release_ready')}));return 0
    except Exception as error:
        failure=dict(status='FAIL',release_ready=False,error=str(error),error_type=type(error).__name__)
        if hasattr(error,'tool_selection_failure'):failure['tool_selection_failure']=error.tool_selection_failure
        print(json.dumps(failure));return 1

if __name__=='__main__':
    sys.dont_write_bytecode=True
    raise SystemExit(main())