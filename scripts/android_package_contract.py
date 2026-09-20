#!/usr/bin/env python3
"""Verify final Android archive bytes and test signatures; never prove device execution.

Expected context/tool digests are caller-controlled assertions, not build provenance.
Only the seven selected launcher/JAR files are locked here. The calling build driver
must also lock JDK/SDK runtime dependencies and the compiler/source inventories.
Injected runners are fixtures regardless of their returned status.
"""
from __future__ import annotations
import argparse,copy,hashlib,json,math,os,re,stat,struct,subprocess,zipfile
from pathlib import Path
import xml.etree.ElementTree as ET
from ci_package_lane import _new_work
from package_runtime import run_runtime_command
from package_verification import _name,_sha256_file,prepare_package,verify_stable
from verify_execution_bundle import _no_links

SCHEMA='caesura.android-package-contract.v2'
TOOL_NAMES={'java','keytool','jarsigner','aapt2','zipalign','apksigner_jar','bundletool_jar'}
MAX_JSON=1024*1024
MAX_OUTPUT=4*1024*1024
MAX_ARCHIVE=2*1024**3
MAX_EXPANDED=4*1024**3
MAX_ENTRIES=100_000
CHAIN='This jar contains entries whose certificate chain is invalid. Reason: PKIX path building failed: sun.security.provider.certpath.SunCertPathBuilderException: unable to find valid certification path to requested target'
SELF='This jar contains entries whose signer certificate is self-signed.'
JAVA_FLAGS=['-J-Duser.language=en','-J-Duser.country=US','-J-Dfile.encoding=UTF-8']

class AndroidPackageError(ValueError):pass
def _need(value,message):
    if not value:raise AndroidPackageError(message)
def _digest(value):return type(value) is str and re.fullmatch('[0-9a-f]{64}',value) is not None
def _canonical(value):return json.dumps(value,sort_keys=True,separators=(',',':'),ensure_ascii=False).encode()
def _save(path,value):
    with Path(path).open('xb') as stream:stream.write(_canonical(value)+b'\n')
def _file(path,digest):
    _need(Path(path).is_absolute(),'A canonical absolute file path is required')
    path=_no_links(path);info=path.stat()
    _need(stat.S_ISREG(info.st_mode) and info.st_nlink==1,'Input must be a regular file without hardlinks')
    _need(_digest(digest) and _sha256_file(path)==digest,'External file digest mismatch')
    return path
def _json(path,digest):
    path=_file(path,digest)
    _need(path.stat().st_size<=MAX_JSON,'JSON exceeds limit')
    raw=path.read_bytes()
    def pairs(items):
        value={}
        for k,v in items:
            _need(k not in value,'Duplicate JSON key');value[k]=v
        return value
    def invalid(value):raise AndroidPackageError('Non-integer JSON number')
    try:value=json.loads(raw.decode('utf-8-sig'),object_pairs_hook=pairs,parse_float=invalid,parse_constant=invalid)
    except (UnicodeError,json.JSONDecodeError,RecursionError) as error:raise AndroidPackageError('Malformed JSON') from error
    _need(hashlib.sha256(raw).hexdigest()==digest,'JSON changed while reading')
    return value
def _expected(value):
    keys={'source_sha','package_name','version_name','version_code','abi','min_sdk','target_sdk','certificate_sha256','required_apk_entries','required_aab_entries'}
    _need(type(value) is dict and set(value)==keys,'Expected context must have the exact contract fields')
    _need(type(value['source_sha']) is str and re.fullmatch('[0-9a-f]{40}',value['source_sha']),'Invalid declared source SHA')
    _need(type(value['package_name']) is str and re.fullmatch(r'[A-Za-z]\w*(?:\.[A-Za-z]\w*)+',value['package_name']),'Invalid expected package name')
    _need(type(value['version_name']) is str and re.fullmatch(r'[0-9][0-9A-Za-z.+-]{0,99}',value['version_name']),'Invalid expected version name')
    for key in ('version_code','min_sdk','target_sdk'):
        _need(type(value[key]) is int and 0<value[key]<2**31,'Invalid expected '+key)
    _need(value['min_sdk']<=value['target_sdk'] and value['abi']=='arm64-v8a','Unsupported ABI or SDK range')
    _need(_digest(value['certificate_sha256']),'Expected test certificate digest required')
    libs={'lib/arm64-v8a/libCaesuraAmeKAG.so','lib/arm64-v8a/libSDL3.so'}
    core={'apk':{'AndroidManifest.xml','classes.dex'}|libs,
          'aab':{'base/manifest/AndroidManifest.xml','BundleConfig.pb','base/dex/classes.dex'}|{'base/'+name for name in libs}}
    for kind in ('apk','aab'):
        required=value['required_'+kind+'_entries']
        _need(type(required) is dict and core[kind]<=set(required),'Missing core required '+kind+' entries')
        for name,digest in required.items():
            _need(type(name) is str and _name(name)==name and _digest(digest),'Invalid required entry lock')
    apk=value['required_apk_entries'];aab=value['required_aab_entries']
    for name,digest in apk.items():
        if name.startswith(('lib/','assets/')):
            _need(aab.get('base/'+name)==digest,'APK/AAB required content differs: '+name)
    for name in aab:
        if name.startswith(('base/lib/','base/assets/')):_need(name[5:] in apk,'AAB content has no corresponding APK requirement')
    return copy.deepcopy(value)
def _tools(value):
    _need(type(value) is dict and set(value)==TOOL_NAMES,'Seven exact tool locks required')
    result={}
    for key,lock in value.items():
        _need(type(lock) is dict and set(lock)=={'path','sha256'},'Invalid tool lock')
        path=_file(lock['path'],lock['sha256'])
        _need(path.suffix.lower() not in ('.bat','.cmd'),'Shell wrappers cannot be verification tools')
        result[key]=dict(path=str(path),sha256=lock['sha256'])
    return result
def _elf(path):
    size=path.stat().st_size
    with path.open('rb') as stream:
        head=stream.read(64)
        _need(len(head)==64 and head[:7]==b'\x7fELF\x02\x01\x01','Invalid ELF64 little-endian header')
        fields=struct.unpack('<16sHHIQQQIHHHHHH',head)
        _,kind,machine,version,_,phoff,_,_,ehsize,phsize,phcount,*_=fields
        _need(kind==3 and machine==183 and version==1 and ehsize==64 and phsize==56 and 0<phcount<=4096
              and phoff>=64 and phoff+phsize*phcount<=size,'Invalid AArch64 shared ELF header')
        stream.seek(phoff);loads=0
        for _ in range(phcount):
            entry=struct.unpack('<IIQQQQQQ',stream.read(56));typ,_,offset,_,_,filesz,memsz,_=entry
            _need(offset+filesz<=size and filesz<=memsz,'ELF segment exceeds file')
            if typ==1:loads+=1
        _need(loads>0,'ELF has no loadable segment')
def _archive(path,digest,kind,required,work):
    _need(0<path.stat().st_size<=MAX_ARCHIVE,'Android archive exceeds size limit')
    with zipfile.ZipFile(path) as z:
        entries=z.infolist();_need(len(entries)<=MAX_ENTRIES and sum(e.file_size for e in entries)<=MAX_EXPANDED,'Android ZIP exceeds limits')
        for entry in entries:
            mode=stat.S_IFMT(entry.external_attr>>16)
            _need(mode in (0,stat.S_IFREG,stat.S_IFDIR),'Android ZIP rejects links and special entries')
            _need(not entry.flag_bits&1,'Encrypted Android ZIP is unsupported')
    prepared=prepare_package(path,work,expected_sha256=digest)
    root=Path(prepared['package_path'])
    for name,wanted in required.items():
        item=root/name;_need(item.is_file(),'Missing required '+kind+' entry: '+name)
        _need(_sha256_file(item)==wanted,'Required entry digest mismatch: '+name)
    prefix='lib/' if kind=='apk' else 'base/lib/'
    if kind=='aab':
        # This contract supports only the base native module, including when
        # an unsupported module's lib tree is empty or uses the expected ABI.
        for module in root.iterdir():
            _need(module.name=='base' or not (module/'lib').exists(),
                  'Unsupported AAB native module: '+module.name)
    native=[p for p in root.rglob('*') if p.is_file() and p.relative_to(root).as_posix().startswith(prefix)]
    for item in native:
        name=item.relative_to(root).as_posix()
        _need(name in required and name.startswith(prefix+'arm64-v8a/') and name.endswith('.so'),'Unexpected native library/ABI: '+name)
        _elf(item)
    return prepared
def _manifest(text,expected):
    packages=re.findall(r"^package: name='([^']+)' versionCode='([^']+)' versionName='([^']*)'[^\r\n]*$",text,re.M)
    _need(packages==[(expected['package_name'],str(expected['version_code']),expected['version_name'])],'Actual APK manifest identity differs')
    for field,key in (('sdkVersion','min_sdk'),('targetSdkVersion','target_sdk')):
        _need(re.findall(r"^"+field+r":'([^']+)'\s*$",text,re.M)==[str(expected[key])],'Actual APK '+field+' differs')
    rows=re.findall(r'^native-code:([^\r\n]*)$',text,re.M)
    _need(len(rows)==1 and re.findall(r"'([^']+)'",rows[0])==[expected['abi']],'Actual APK manifest ABI differs')
def _aab_manifest(text,expected):
    """Observe only base package/version/SDK identity, never delivery or device support."""
    _need(type(text) is str and 0<len(text.encode('utf-8'))<=MAX_JSON,'AAB manifest XML exceeds limit')
    _need('<!DOCTYPE' not in text and '<!ENTITY' not in text,'AAB manifest XML declarations are unsupported')
    try:root=ET.fromstring(text)
    except ET.ParseError as error:raise AndroidPackageError('Malformed AAB manifest XML') from error
    _need(root.tag=='manifest','Actual AAB manifest root differs')
    android='{http://schemas.android.com/apk/res/android}'
    sdk=root.findall('uses-sdk')
    _need(len(sdk)==1,'Actual AAB requires one base uses-sdk element')
    def number(element,name):
        value=element.get(android+name)
        _need(type(value) is str and re.fullmatch('[0-9]{1,10}',value) is not None,'Actual AAB '+name+' is not a numeric value')
        result=int(value)
        _need(0<result<2**31,'Actual AAB '+name+' exceeds supported range')
        return result
    _need(root.get(android+'versionCodeMajor') in (None,'0'),'Actual AAB major version code is unsupported')
    observed=dict(package_name=root.get('package'),version_name=root.get(android+'versionName'),
                  version_code=number(root,'versionCode'),min_sdk=number(sdk[0],'minSdkVersion'),
                  target_sdk=number(sdk[0],'targetSdkVersion'))
    _need(observed=={key:expected[key] for key in observed},'Actual AAB base manifest identity/version/SDK differs')
    return observed

def _jar_verdict(code,text):
    lines=[line.strip() for line in text.splitlines()]
    _need(code in (0,4),'AAB jarsigner verification failed')
    if code==0:
        _need(lines.count('jar verified.')==1 and 'Error:' not in lines,'AAB is not verified')
        return 'VERIFIED'
    _need(lines.count('jar verified, with signer errors.')==1 and lines.count('Error:')==1,'Unknown jarsigner signer-error result')
    start=lines.index('Error:')+1;errors=[]
    for line in lines[start:]:
        if not line or line=='Warning:':break
        errors.append(line)
    _need(errors==[CHAIN,SELF],'AAB errors are not solely the observed self-signed trust chain')
    return 'VERIFIED_SELF_SIGNED_TEST_CERTIFICATE'
def _environment(tools,work):
    # The runtime helper persists this mapping. Never forward credentials or
    # Java/Gradle option injection variables from the parent environment.
    env={key:os.environ[key] for key in ('SystemRoot','WINDIR','COMSPEC','SYSTEMDRIVE') if key in os.environ}
    env.update(JAVA_HOME=str(Path(tools['java']['path']).parent.parent),LANG='C',LC_ALL='C',
               TEMP=str(work),TMP=str(work),TMPDIR=str(work),HOME=str(work),USERPROFILE=str(work))
    env['PATH']=os.pathsep.join(sorted({str(Path(v['path']).parent) for v in tools.values()}))
    return env
def _check_files(report):
    control=report['control'];value=_json(control['path'],control['sha256'])
    _need(value==report['inputs'],'Controlled input snapshot differs')
    _need(report['expected']==value['expected'] and report['tools']==value['tools'],'Expected/tool control inputs changed')
    request=report.get('request')
    if request:_need(_json(request['path'],request['sha256'])==value,'Original request changed')
    for kind in ('apk','aab'):
        _file(value[kind+'_path'],value[kind+'_sha256']);verify_stable(report['prepared'][kind])
    _tools(report['tools'])
    for command in report['commands']:
        for stream in ('stdout','stderr'):_file(command[stream]['path'],command[stream]['sha256'])
        for lock in command['process_files']:_file(lock['path'],lock['sha256'])
def verify_android_package_stable(result):
    _need(type(result) is dict and result.get('schema')==SCHEMA and result.get('status') in ('ANDROID_PACKAGE_VERIFIED','FIXTURE_PACKAGE_VERIFIED') and result.get('release_ready') is False,'Verified Android package result required')
    saved=_json(result['receipt_path'],result['receipt_sha256'])
    _need(saved=={k:v for k,v in result.items() if k!='receipt_sha256'},'Android receipt/control selection changed')
    _check_files(result)
    return dict(status='ANDROID_PACKAGE_STABLE',release_ready=False)

def verify_android_package(*,apk_path,apk_sha256,aab_path,aab_sha256,expected,tools,work_dir,
                           fixture=False,runner=None,request_path=None,request_sha256=None,timeout=60):
    """Expected/producer assertions require separate build-driver authentication.

    Library input snapshots are locked internally; CLI additionally pins original
    request bytes by an externally supplied digest. Retain the returned receipt
    digest outside the work directory when reusing a saved result.
    """
    work=_new_work(work_dir);receipt=work/'android-package.json'
    report=dict(schema=SCHEMA,status='FAIL',stage='inputs',release_ready=False,runtime='NOT_RUN',
                source_provenance='NOT_VERIFIED',producer_provenance='NOT_VERIFIED',
                scopes=dict(archive_structure='NOT_VERIFIED',apk_manifest_semantics='NOT_VERIFIED',
                            aab_manifest_semantics='NOT_VERIFIED',signatures='NOT_VERIFIED',device='NOT_RUN'),
                tool_dependency_inventory='CALLER_MUST_VERIFY_JDK_SDK_RUNTIME_DEPENDENCIES',
                receipt_path=str(receipt),prepared={},commands=[],errors=[])
    try:
        _need(type(fixture) is bool and type(timeout) in (int,float) and math.isfinite(timeout) and 0<timeout<=300,'Invalid execution options')
        expected=_expected(expected);tools=_tools(tools)
        apk=_file(apk_path,apk_sha256);aab=_file(aab_path,aab_sha256)
        _need(apk!=aab and apk.suffix.lower()=='.apk' and aab.suffix.lower()=='.aab','Two distinct final APK/AAB inputs required')
        inputs=dict(apk_path=str(apk),apk_sha256=apk_sha256,aab_path=str(aab),aab_sha256=aab_sha256,expected=expected,tools=tools)
        _need(not any(Path(v['path']).is_relative_to(work) for v in tools.values()) and not apk.is_relative_to(work) and not aab.is_relative_to(work),'Inputs/tools must be outside verification work')
        report.update(inputs=inputs,expected=expected,tools=tools,transport='fixture' if fixture or runner else 'local-tools')
        if request_path is not None:
            _need(_json(request_path,request_sha256)==inputs,'Request bytes do not match supplied input selection')
            report['request']=dict(path=str(_no_links(request_path)),sha256=request_sha256)
        else:_need(request_sha256 is None,'Request path required with digest')
        control=work/'inputs.json';_save(control,inputs);report['control']=dict(path=str(control),sha256=_sha256_file(control))
        report['stage']='archives'
        for kind,path,digest in (('apk',apk,apk_sha256),('aab',aab,aab_sha256)):
            report['prepared'][kind]=_archive(path,digest,kind,expected['required_'+kind+'_entries'],work/kind)
        report['scopes'].update(archive_structure='VERIFIED',aab_manifest_semantics='NOT_VERIFIED')
        env=_environment(tools,work);execute=runner or run_runtime_command
        def command(name,argv,allowed=(0,)):
            report['stage']=name;out=work/(name+'.stdout');err=work/(name+'.stderr')
            process_dir=work/(name+'-process')
            item=dict(name=name,argv=argv,process_files=[]);report['commands'].append(item)
            with out.open('xb') as stdout,err.open('xb') as stderr:
                try:
                    result=execute(argv,cwd=work,env=env,control_dir=process_dir,stdout=stdout,stderr=stderr,timeout=timeout)
                    item['process']=result
                finally:
                    stdout.flush();stderr.flush()
                    item.update(stdout=dict(path=str(out),sha256=_sha256_file(out)),stderr=dict(path=str(err),sha256=_sha256_file(err)))
                    if process_dir.is_dir():
                        for path in sorted(process_dir.iterdir()):
                            _no_links(path)
                            _need(path.is_file() and path.stat().st_nlink==1,'Invalid owned process evidence file')
                            digest=_sha256_file(path);_file(path,digest)
                            item['process_files'].append(dict(path=str(path),sha256=digest))
            _need(result.get('owned_tree_cleanup')=='COMPLETE' and result.get('status')=='EXITED' and type(result.get('actual_exit_code')) is int and result['actual_exit_code'] in allowed,'Tool failed: '+name)
            _need(out.stat().st_size+err.stat().st_size<=MAX_OUTPUT,'Tool output exceeds limit')
            return result['actual_exit_code'],out.read_text(encoding='utf-8',errors='strict')+'\n'+err.read_text(encoding='utf-8',errors='strict')
        tool=lambda name:tools[name]['path']
        _,text=command('aapt2',[tool('aapt2'),'dump','badging',str(apk)]);_manifest(text,expected)
        report['scopes']['apk_manifest_semantics']='VERIFIED'
        bundletool=[tool('java'),'-Duser.language=en','-Duser.country=US','-Dfile.encoding=UTF-8',
                    '-Duser.home='+str(work),'-jar',tool('bundletool_jar')]
        _,text=command('bundletool_version',[*bundletool,'version'])
        _need(text.strip()=='1.17.1','Actual bundletool version differs')
        _,text=command('aab_manifest',[*bundletool,'dump','manifest','--bundle='+str(aab),'--module=base'])
        report['aab_manifest']=_aab_manifest(text,expected)
        report['scopes']['aab_manifest_semantics']='BASE_IDENTITY_VERSION_SDK_VERIFIED'
        command('zipalign',[tool('zipalign'),'-c','-v','4',str(apk)])
        _,text=command('apksigner',[tool('java'),'-Duser.language=en','-Duser.country=US','-Dfile.encoding=UTF-8','-Duser.home='+str(work),'-jar',tool('apksigner_jar'),'verify','--verbose','--print-certs',str(apk)])
        certs=re.findall(r'^Signer #1 certificate SHA-256 digest: ([0-9a-fA-F]{64})\s*$',text,re.M)
        _need(re.findall(r'^Number of signers: (\d+)\s*$',text,re.M)==['1'] and [c.lower() for c in certs]==[expected['certificate_sha256']] and re.findall(r'^Verifies\s*$',text,re.M),'Actual APK certificate differs or signature not verified')
        java_flags=[*JAVA_FLAGS,'-J-Duser.home='+str(work)]
        code,text=command('jarsigner',[tool('jarsigner'),*java_flags,'-verify','-strict','-verbose','-certs',str(aab)],(0,4))
        report['aab_signature']=_jar_verdict(code,text)
        _,text=command('keytool',[tool('keytool'),*java_flags,'-printcert','-jarfile',str(aab)])
        signers=re.findall(r'^Signer #(\d+):\s*$',text,re.M)
        certs=re.findall(r'^\s*SHA256: ([0-9A-Fa-f:]+)\s*$',text,re.M)
        _need(signers==['1'] and len(certs)==1 and certs[0].replace(':','').lower()==expected['certificate_sha256'],'Actual AAB certificate differs')
        report.update(stage='stability',apk_signature='VERIFIED_TEST_CERTIFICATE',certificate_sha256=expected['certificate_sha256'],
                      aab_manifest_semantics='BASE_IDENTITY_VERSION_SDK_VERIFIED')
        report['scopes']['signatures']='VERIFIED_TEST_CERTIFICATE'
        _check_files(report)
        report.update(status='FIXTURE_PACKAGE_VERIFIED' if fixture or runner else 'ANDROID_PACKAGE_VERIFIED',stage='complete')
    except Exception as error:
        report['status']='FAIL';report['errors'].append(str(error));raise
    finally:_save(receipt,report)
    return dict(report,receipt_sha256=_sha256_file(receipt))

def main(argv=None,*,runner=None):
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--request',type=Path,required=True);parser.add_argument('--request-sha256',required=True)
    parser.add_argument('--work',type=Path,required=True)
    args=parser.parse_args(argv)
    stage='request'
    try:
        value=_json(args.request,args.request_sha256)
        _need(type(value) is dict and set(value)=={'apk_path','apk_sha256','aab_path','aab_sha256','expected','tools'},'Invalid request fields')
        stage='verification'
        result=verify_android_package(**value,work_dir=args.work,request_path=args.request,request_sha256=args.request_sha256,runner=runner)
        print(json.dumps(result));return 77 if result['status']=='FIXTURE_PACKAGE_VERIFIED' else 0
    except (ValueError,RuntimeError,OSError,KeyError,TypeError,subprocess.SubprocessError,zipfile.BadZipFile) as error:
        failure=dict(schema=SCHEMA,status='FAIL',stage=stage,release_ready=False,error=str(error))
        # Parsing failures precede the library's durable attempt. Create a fresh
        # failure receipt when possible, but never overwrite any prior attempt.
        if not args.work.exists():
            try:
                work=_new_work(args.work);receipt=work/'android-package.json'
                failure.update(receipt_path=str(receipt),request_path=str(args.request),request_sha256=args.request_sha256)
                _save(receipt,failure);failure['receipt_sha256']=_sha256_file(receipt)
            except (ValueError,OSError):pass
        print(json.dumps(failure));return 1

if __name__=='__main__':raise SystemExit(main())
