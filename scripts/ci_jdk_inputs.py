#!/usr/bin/env python3
"""Materialize selected JDK input bytes; never install, alter trust, or run tools."""
from __future__ import annotations
import argparse,hashlib,json,os,stat,sys,time,uuid
from contextlib import contextmanager
from pathlib import Path
from ci_package_lane import ROOT
from build_appimage import _OutputParent
from verify_execution_bundle import _no_links
import run_android_validation as driver

SCHEMA='caesura.ci-jdk-inputs.v1'
CA_SCHEMA='caesura.ci-jdk-cacerts-input.v1'
EXTERNAL_CACERTS=Path('/etc/ssl/certs/adoptium/cacerts')
SELECTED=('bin','lib','conf','release')
MAX_ENTRIES=200000
MAX_BYTES=2*1024**3
MAX_JSON=64*1024**2
SECONDS=120


def need(ok,message):
    if not ok:raise ValueError(message)


def _meta(info):
    return dict(device=info.st_dev,inode=info.st_ino,mode=info.st_mode,nlink=info.st_nlink,
                size=info.st_size,mtime_ns=info.st_mtime_ns)


class Budget:
    def __init__(self):self.end=time.monotonic()+SECONDS
    def check(self):need(time.monotonic()<self.end,'JDK input preparation deadline exceeded')


def _plain(path, directory=False):
    path=Path(path);need(path.is_absolute(),'Absolute input path required')
    _no_links(path)
    info=path.lstat()
    need(stat.S_ISDIR(info.st_mode) if directory else stat.S_ISREG(info.st_mode) and info.st_nlink==1,
         'Input must be an ordinary '+('directory: ' if directory else 'single-link file: ')+str(path))
    return info


class _JdkOutputParent(_OutputParent):
    """Retain the existing binding contract with read/list sharing on Windows."""

    def _windows_open(self):
        import ctypes
        from ctypes import wintypes
        class FileInformation(ctypes.Structure):
            _fields_ = [("attributes", wintypes.DWORD), ("creation", wintypes.FILETIME),
                ("access", wintypes.FILETIME), ("write", wintypes.FILETIME),
                ("volume", wintypes.DWORD), ("size_high", wintypes.DWORD),
                ("size_low", wintypes.DWORD), ("links", wintypes.DWORD),
                ("index_high", wintypes.DWORD), ("index_low", wintypes.DWORD)]
        self.kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        self.kernel.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
            ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
        self.kernel.CreateFileW.restype = wintypes.HANDLE
        self.kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        self.kernel.GetFileInformationByHandle.argtypes = [wintypes.HANDLE, ctypes.POINTER(FileInformation)]
        self.kernel.GetFileInformationByHandle.restype = wintypes.BOOL
        # FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES is required: attribute-only
        # handles do not impose the deletion-sharing constraint on rename.
        handle = self.kernel.CreateFileW(str(self.path), 0x81, 0x3, None, 3, 0x02200000, None)
        if handle == ctypes.c_void_p(-1).value:
            raise OSError(ctypes.get_last_error(), "Cannot bind the Windows output directory")
        try:
            info = FileInformation()
            if not self.kernel.GetFileInformationByHandle(handle, ctypes.byref(info)):
                raise OSError(ctypes.get_last_error(), "Cannot identify the Windows output directory")
            if not info.attributes & 0x10 or info.attributes & 0x400:
                raise ValueError("Output parent must be a plain directory")
            return handle, (info.volume, info.index_high, info.index_low)
        except BaseException:
            self.kernel.CloseHandle(handle)
            raise


@contextmanager
def _bound_parent(path):
    """Pin ordinary ancestors before any mutation, including on Windows.

    POSIX mutations use retained directory fds. Windows read/list handles
    omit SHARE_DELETE and prevent renaming each bound path component.
    """
    path=Path(path);need(path.is_absolute() and '..' not in path.parts,'Plain absolute output parent required')
    parents=[]
    try:
        for item in (*reversed(path.parents),path):
            info=_plain(item,directory=True)
            parents.append(_JdkOutputParent(item,(info.st_dev,info.st_ino)))
        for parent in parents:parent.verify()
        yield parents[-1]
        for parent in parents:parent.verify()
    finally:
        for parent in reversed(parents):parent.close()


def _relative_open(parent,name,kind):
    need(name not in ('','.','..') and not any(c in name for c in '/\\:'),'One output component required')
    parent.verify()
    return _native_relative(parent,name,kind)


def _native_relative(parent,name,kind):
    if parent.fd is not None:
        if kind=='directory':return os.mkdir(name,0o700,dir_fd=parent.fd)
        flags=os.O_WRONLY|os.O_NOFOLLOW|(os.O_APPEND if kind=='append' else os.O_CREAT|os.O_EXCL)
        return os.open(name,flags,0o600,dir_fd=parent.fd)
    # Every ancestor is held open with read/list access and no delete sharing.
    # Exclusive creation refuses an existing leaf; append never creates a file
    # and its descriptor/path identity is checked before any bytes are written.
    target=parent.path/name
    if kind=='directory':return os.mkdir(target,0o700)
    flags=os.O_WRONLY|os.O_BINARY|(os.O_APPEND if kind=='append' else os.O_CREAT|os.O_EXCL)
    return os.open(target,flags,0o600)


def _mkdir(path):
    path=Path(path);driver.relative(path.name)
    with _bound_parent(path.parent) as parent:
        _relative_open(parent,path.name,'directory')
        parent.verify();_plain(path,directory=True)
    return path


def _new_work(value):
    path=Path(value).absolute()
    for parent in path.parents:
        need(not (parent/'.git').exists(),'Work must be outside every checkout')
    need(not ROOT.resolve().is_relative_to(path),'Work must not contain the checkout')
    return _mkdir(path)


@contextmanager
def _create(path):
    path=Path(path)
    with _bound_parent(path.parent) as parent:
        fd=_relative_open(parent,path.name,'create')
        with os.fdopen(fd,'wb') as stream:
            opened=os.fstat(fd);current=_plain(path)
            need((opened.st_dev,opened.st_ino)==(current.st_dev,current.st_ino),'Output changed before write')
            yield stream
            stream.flush();os.fsync(fd)
            need(_meta(os.fstat(fd))==_meta(_plain(path)),'Output changed during write')


def _read_file(path,budget,destination=None):
    """Bind opened descriptor to the checked path before and after streaming."""
    budget.check();before=_meta(_plain(path));need(before['size']<=MAX_BYTES,'JDK file exceeds byte limit')
    flags=os.O_RDONLY|getattr(os,'O_BINARY',0)|getattr(os,'O_NOFOLLOW',0)
    descriptor=os.open(path,flags);digest=hashlib.sha256();size=0;output=None;copied=None;creation=None
    try:
        need(_meta(os.fstat(descriptor))==before and _meta(_plain(path))==before,'File replaced before read: '+str(path))
        if destination is not None:
            creation=_create(destination);output=creation.__enter__()
        with os.fdopen(descriptor,'rb',closefd=False) as stream:
            while True:
                budget.check();block=stream.read(1024*1024)
                if not block:break
                size+=len(block);need(size<=MAX_BYTES,'JDK file exceeds byte limit')
                digest.update(block)
                if output:output.write(block)
            need(_meta(os.fstat(descriptor))==before and _meta(_plain(path))==before,'File changed during read: '+str(path))
        if output:
            output.flush();os.fsync(output.fileno())
            mode=stat.S_IMODE(before['mode']) & 0o777
            if hasattr(os,'fchmod'):os.fchmod(output.fileno(),mode)
            # Windows fixtures retain the newly created ordinary file mode;
            # never fall back to a raceable pathname-based chmod.
            copied=dict(path=str(destination),metadata=_meta(os.fstat(output.fileno())),sha256=digest.hexdigest())
            need(_meta(_plain(destination))==copied['metadata'],'Output path changed during copy')
    finally:
        try:
            if output:creation.__exit__(*sys.exc_info())
        finally:os.close(descriptor)
    budget.check();need(size==before['size'],'Incomplete stable file read')
    record=dict(path=str(path),metadata=before,sha256=digest.hexdigest())
    return (record,copied) if destination is not None else record


def _copy_file(source,destination,expected,budget):
    need(_meta(_plain(source))==expected['metadata'],'Source replaced before copy: '+str(source))
    result,copied=_read_file(source,budget,destination)
    need(result==expected,'Source bytes changed before copy: '+str(source))
    return copied


def _save(path,value):
    raw=(json.dumps(value,ensure_ascii=False,sort_keys=True,separators=(',',':'))+'\n').encode('utf-8')
    need(len(raw)<=MAX_JSON,'JDK receipt exceeds JSON limit')
    with _create(path) as f:f.write(raw)
    return hashlib.sha256(raw).hexdigest()


def _control(path,digest):
    # Reuse duplicate-key, plain-file and digest validation from the strict driver.
    return driver.load(path,digest)


def _root(value):
    need(Path(value).is_absolute(),'JDK root must be absolute')
    need(not str(value).startswith('\\\\'),'Device/UNC JDK roots are unsupported')
    requested=Path(value).absolute();need('..' not in requested.parts,'JDK root contains traversal')
    resolved=requested.resolve(strict=True);_plain(resolved,directory=True)
    trace=[];item=Path(requested.anchor)
    for part in requested.parts[1:]:
        item=item/part;info=item.lstat()
        row=dict(path=str(item),device=info.st_dev,inode=info.st_ino,mode=info.st_mode)
        need(not item.is_junction(),'JDK root resolution traverses junction: '+str(item))
        if stat.S_ISLNK(info.st_mode):row.update(metadata=_meta(info),target=os.readlink(item))
        trace.append(row)
    return dict(requested_root=str(requested),resolved_root=str(resolved),resolution_trace=trace)


def _external_file(path,budget,fixture_path=None):
    path=Path(path);allowed=Path(fixture_path) if fixture_path is not None else EXTERNAL_CACERTS
    need(path==allowed and path.is_absolute(),'Only the explicitly declared cacerts path is allowed')
    _plain(path)  # canonical, no links, regular, nlink1 BEFORE reading any bytes
    return _read_file(path,budget)


def select_cacerts(path,work_dir,*,_fixture_path=None):
    source=Path(path);work=Path(work_dir).absolute()
    need(work!=source and not source.is_relative_to(work) and not work.is_relative_to(source.parent),'CA output overlaps input')
    work=_new_work(work);receipt=work/'cacerts-input.json';budget=Budget()
    report=dict(schema=CA_SCHEMA,status='FAIL',role='jdk-cacerts',fixture=_fixture_path is not None,
                allowed_path=str(_fixture_path if _fixture_path is not None else EXTERNAL_CACERTS),
                vendor_signature='NOT_VERIFIED',trust_contents='SELECTED_HOST_BYTES_NOT_ENDORSED',errors=[])
    try:
        report['file']=_external_file(source,budget,_fixture_path)
        report['status']='FIXTURE_CA_INPUT' if _fixture_path is not None else 'CA_INPUT_SELECTED'
    except BaseException as error:
        report['errors'].append(type(error).__name__+': '+str(error))
        if hasattr(error,'jdk_input_failure'):report['failure']=error.jdk_input_failure
        raise
    finally:digest=_save(receipt,report)
    return dict(report,receipt_path=str(receipt),receipt_sha256=digest)


def _external(receipt,digest,budget,allow_fixture=False):
    if receipt is None:
        need(digest is None,'External CA receipt required with digest');return None
    value=_control(receipt,digest)
    need(value.get('schema')==CA_SCHEMA and value.get('role')=='jdk-cacerts' and
         value.get('status') in ('CA_INPUT_SELECTED','FIXTURE_CA_INPUT'),'Invalid external CA selection')
    need(type(value.get('fixture')) is bool,'Explicit external fixture state required')
    fixture=value['fixture']
    need(value['status']==('FIXTURE_CA_INPUT' if fixture else 'CA_INPUT_SELECTED'),'Inconsistent external fixture status')
    need(not fixture or allow_fixture,'Fixture CA cannot become real JDK input')
    need(value['allowed_path']==(value['file']['path'] if fixture else str(EXTERNAL_CACERTS)),'Wrong declared external CA path')
    observed=_external_file(value['file']['path'],budget,value['allowed_path'] if fixture else None)
    need(observed==value['file'],'External CA changed after selection')
    return dict(receipt_path=str(receipt),receipt_sha256=digest,selection=value)


def _target(alias,raw,root):
    """Inspect every path component before consuming ..; never follow a chain."""
    target=Path(raw)
    if target.is_absolute():
        need(target.is_relative_to(root),'JDK file alias escapes root: '+str(alias))
        current=root;parts=target.relative_to(root).parts
    else:current=alias.parent;parts=target.parts
    for part in parts:
        _plain(current,directory=True)
        if part=='.':continue
        if part=='..':
            need(current!=root,'JDK alias escapes root: '+str(alias));current=current.parent
        else:
            need(part and not part.endswith(('.', ' ')) and ':' not in part,'Bad alias path component')
            current=current/part
    need(current.is_relative_to(root),'JDK alias escapes root')
    _plain(current)
    return current


def _snapshot(source,external,budget):
    root=Path(source['resolved_root']);_plain(root,directory=True)
    entries={};directories={};folded=set();total=0
    stack=[root/name for name in reversed(SELECTED)]
    while stack:
        budget.check();path=stack.pop();relative=path.relative_to(root).as_posix()
        driver.relative(relative);need(relative.casefold() not in folded,'Aliased JDK input path');folded.add(relative.casefold())
        need(len(folded)<=MAX_ENTRIES,'JDK entry limit exceeded')
        _plain(path.parent,directory=True);info=path.lstat();need(not path.is_junction(),'Directory junction is not a JDK file alias: '+relative)
        if stat.S_ISDIR(info.st_mode):
            _plain(path,directory=True);directories[relative]=_meta(info)
            stack.extend(sorted(path.iterdir(),reverse=True));continue
        try:
            if stat.S_ISLNK(info.st_mode):
                raw=os.readlink(path);link_meta=_meta(info)
                external_text=external['selection']['file']['path'] if external else None
                fixture_spelling=bool(external and external['selection']['fixture'] and os.name=='nt' and raw=='\\\\?\\'+external_text)
                if external and relative=='lib/security/cacerts' and (raw==external_text or fixture_spelling):
                    target=Path(external_text);observed=_external_file(target,budget,external['selection']['allowed_path'] if external['selection']['fixture'] else None)
                    need(observed==external['selection']['file'],'External cacerts bytes changed')
                    kind='external-cacerts'
                else:
                    target=_target(path,raw,root);observed=_read_file(target,budget);kind='internal-file-alias'
                need(_meta(path.lstat())==link_meta and os.readlink(path)==raw,'JDK link changed while selecting: '+relative)
                entry=dict(kind=kind,link_metadata=link_meta,link_target=raw,file=observed)
            else:
                observed=_read_file(path,budget);entry=dict(kind='file',file=observed)
        except Exception as error:
            detail=dict(path=str(path),logical_path=relative,metadata=_meta(info),error_type=type(error).__name__,error=str(error))
            if stat.S_ISLNK(info.st_mode):
                try:detail['link_target']=os.readlink(path)
                except OSError as inspection:detail['link_read_error']=str(inspection)
            error.jdk_input_failure=detail
            raise
        total+=entry['file']['metadata']['size'];need(total<=MAX_BYTES,'Selected JDK bytes exceed limit')
        entries[relative]=entry
    return dict(entries=entries,directories=directories,total_bytes=total)


def _mirror(root,budget):
    root=_no_links(root);entries={};dirs={};stack=[root];total=0
    while stack:
        budget.check();path=stack.pop();rel=path.relative_to(root).as_posix()
        need(len(entries)+len(dirs)<=MAX_ENTRIES+1,'Mirror entry limit exceeded')
        info=path.lstat();_no_links(path)
        if stat.S_ISDIR(info.st_mode):
            dirs[rel]=_meta(info);stack.extend(sorted(path.iterdir(),reverse=True))
        else:
            selected=_read_file(path,budget);entries[rel]=selected;total+=selected['metadata']['size'];need(total<=MAX_BYTES,'Mirror byte limit exceeded')
    return dict(entries=entries,directories=dirs,total_bytes=total)


def _stable(value,budget,allow_fixture):
    need(not value['fixture'] or allow_fixture,'Fixture JDK input cannot be accepted')
    need(_root(value['source']['requested_root'])==value['source'],'JDK root resolution changed')
    external=value.get('external')
    if external:
        need(_external(external['receipt_path'],external['receipt_sha256'],budget,allow_fixture)==external,'External CA binding changed')
    need(_snapshot(value['source'],external,budget)==value['source_snapshot'],'Original JDK files or links changed')
    need(value['mirror_snapshot']['entries']==value['copied_files'],'Mirror copy descriptor locks differ')
    need(_mirror(Path(value['mirror_root']),budget)==value['mirror_snapshot'],'JDK mirror changed')


def prepare_jdk_inputs(jdk_root,work_dir,*,external_receipt=None,external_receipt_sha256=None,_allow_fixture=False):
    source=_root(jdk_root);root=Path(source['resolved_root']);work=Path(work_dir).absolute()
    need(not work.is_relative_to(root) and not root.is_relative_to(work),'JDK output overlaps original root')
    if external_receipt:
        need(not Path(external_receipt).is_relative_to(work),'JDK output overlaps external receipt')
        selection=_control(external_receipt,external_receipt_sha256)
        ca=Path(selection['file']['path'])
        need(ca.is_absolute() and not ca.is_relative_to(work) and not work.is_relative_to(ca.parent),'JDK output overlaps external CA')
    work=_new_work(work);receipt=work/'jdk-inputs.json';budget=Budget()
    report=dict(schema=SCHEMA,status='FAIL',source=source,fixture=False,errors=[],
                mirror_root=str(work/'jdk'),limits=dict(entries=MAX_ENTRIES,bytes=MAX_BYTES,seconds=SECONDS),
                scope='Selected ordinary JDK inputs only; no Java/build/package/signature/device execution')
    _save(work/'intent.json',dict(source=source,external_receipt=str(external_receipt) if external_receipt else None,
                                external_receipt_sha256=external_receipt_sha256,limits=report['limits']))
    try:
        external=_external(external_receipt,external_receipt_sha256,budget,_allow_fixture)
        report['external']=external;report['fixture']=bool(external and external['selection']['fixture'])
        if external:
            ca=Path(external['selection']['file']['path']);need(not ca.is_relative_to(work) and not work.is_relative_to(ca.parent),'JDK output overlaps external CA')
        report['source_snapshot']=_snapshot(source,external,budget)
        mirror=Path(report['mirror_root']);_mkdir(mirror)
        directory_identity=lambda path:{k:v for k,v in _meta(_plain(path,directory=True)).items() if k in ('device','inode','mode')}
        report['created_directories']={'.':directory_identity(mirror)}
        for name in sorted(report['source_snapshot']['directories'],key=lambda n:(len(Path(n).parts),n)):
            budget.check();_mkdir(mirror/name)
            report['created_directories'][name]=directory_identity(mirror/name)
        copied={}
        for name,item in sorted(report['source_snapshot']['entries'].items()):
            budget.check();target=mirror/name
            copied[name]=_copy_file(Path(item['file']['path']),target,item['file'],budget)
        report['copied_files']=copied
        report['mirror_snapshot']=_mirror(mirror,budget)
        observed_dirs={name:{k:row[k] for k in ('device','inode','mode')} for name,row in report['mirror_snapshot']['directories'].items()}
        need(observed_dirs==report['created_directories'],'Mirror directories replaced during copy')
        need(report['mirror_snapshot']['entries']==copied,'Mirror replaced between copy and seal')
        _stable(report,budget,_allow_fixture)
        report['status']='FIXTURE_JDK_INPUTS' if report['fixture'] else 'JDK_INPUTS_PREPARED'
    except BaseException as error:
        report['errors'].append(type(error).__name__+': '+str(error))
        if hasattr(error,'jdk_input_failure'):report['failure']=error.jdk_input_failure
        raise
    finally:digest=_save(receipt,report)
    return dict(report,receipt_path=str(receipt),receipt_sha256=digest)


def verify_jdk_inputs(receipt_path,receipt_sha256,*,allow_fixture=False):
    value=_control(receipt_path,receipt_sha256)
    need(value.get('schema')==SCHEMA and value.get('status') in ('JDK_INPUTS_PREPARED','FIXTURE_JDK_INPUTS'),'Completed JDK preparation required')
    need(type(value.get('fixture')) is bool and value['status']==('FIXTURE_JDK_INPUTS' if value['fixture'] else 'JDK_INPUTS_PREPARED'),'Inconsistent JDK fixture status')
    _stable(value,Budget(),allow_fixture)
    return dict(status='JDK_INPUTS_STABLE',mirror_root=value['mirror_root'],fixture=value['fixture'])


def _github_output(path,work,source,protected=()):
    if path is None:return None
    path=Path(path).absolute();_plain(path.parent,directory=True)
    source=Path(source).resolve(strict=True)
    need(not path.is_relative_to(Path(work).absolute()) and not path.is_relative_to(source),'GitHub output overlaps selected inputs/work')
    need(all(path!=Path(p).absolute() for p in protected),'GitHub output aliases an external input')
    if path.exists():_plain(path)
    return path


def _outputs(path,values):
    if path is None:return
    path=Path(path)
    raw=''
    for key,value in values.items():
        delimiter='caesura_'+uuid.uuid4().hex
        raw+=f'{key}<<{delimiter}\n{value}\n{delimiter}\n'
    data=raw.encode('utf-8')
    with _bound_parent(path.parent) as parent:
        if not os.path.lexists(path):
            with _create(path) as stream:stream.write(data)
            return
        before=_meta(_plain(path))
        fd=_relative_open(parent,path.name,'append')
        with os.fdopen(fd,'ab') as stream:
            need(_meta(os.fstat(fd))==before and _meta(_plain(path))==before,'GitHub output replaced before append')
            parent.verify();stream.write(data);stream.flush();os.fsync(fd)
            need(_meta(os.fstat(fd))==_meta(_plain(path)),'GitHub output changed during append')


def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__);sub=parser.add_subparsers(dest='mode',required=True)
    ca=sub.add_parser('select-cacerts');ca.add_argument('--path',required=True);ca.add_argument('--work',required=True);ca.add_argument('--github-output')
    prep=sub.add_parser('prepare');prep.add_argument('--jdk-root',required=True);prep.add_argument('--external-receipt',required=True);prep.add_argument('--external-receipt-sha256',required=True);prep.add_argument('--work',required=True);prep.add_argument('--github-output')
    verify=sub.add_parser('verify');verify.add_argument('--receipt',required=True);verify.add_argument('--sha256',required=True)
    args=parser.parse_args(argv)
    try:
        if args.mode=='verify':print(json.dumps(verify_jdk_inputs(args.receipt,args.sha256)));return 0
        source=args.path if args.mode=='select-cacerts' else args.jdk_root
        output=_github_output(args.github_output,args.work,source,
            [EXTERNAL_CACERTS]+([args.external_receipt] if args.mode=='prepare' else []))
        if args.mode=='select-cacerts':result=select_cacerts(args.path,args.work)
        else:result=prepare_jdk_inputs(args.jdk_root,args.work,external_receipt=args.external_receipt,external_receipt_sha256=args.external_receipt_sha256)
        values=dict(receipt=result['receipt_path'],receipt_sha256=result['receipt_sha256'])
        if 'mirror_root' in result:values['jdk_root']=result['mirror_root']
        _outputs(output,values);print(json.dumps(dict(status=result['status'],**values)));return 0
    except Exception as error:
        failure=dict(status='FAIL',error_type=type(error).__name__,error=str(error))
        if hasattr(error,'jdk_input_failure'):failure['input_failure']=error.jdk_input_failure
        print(json.dumps(failure));return 1

if __name__=='__main__':
    sys.dont_write_bytecode=True
    raise SystemExit(main())
