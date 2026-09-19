"""Actual local HTTP/ZIP/Git fixtures; only the final U1 verdict is substituted."""
from __future__ import annotations
import contextlib, copy, hashlib, http.client, http.server, importlib.util, io, json, os
from pathlib import Path
import subprocess, sys, tempfile, threading, unittest, uuid, zipfile
from unittest.mock import patch
from urllib.parse import urlsplit, parse_qs
from urllib.request import Request, urlopen
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'scripts'))
import test_package_bundle as package_fixture
from test_validation_evidence import EvidenceFixture
import verify_execution_bundle as execution
from verify_release_inputs import GitHubAPI
from download_release_artifact import download_artifact
try:
    import ci_release_gate as gate
except ModuleNotFoundError:
    gate=None

def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()

class GateTests(unittest.TestCase):
    def setUp(self):
        temporary=tempfile.TemporaryDirectory(prefix='caesura-gate-fixture-')
        self.addCleanup(temporary.cleanup);self.root=Path(temporary.name).resolve()
        self.repo=self.root/'repo';self.repo.mkdir();(self.repo/'.git').mkdir();(self.repo/'scripts').mkdir()
        base=self.root/'execution';base.mkdir();self.e=EvidenceFixture(base)
        (self.repo/'CMakeLists.txt').write_text('project(CaesuraAmeKAG VERSION 1.2.3 LANGUAGES C CXX)\n')
        (self.repo/'scripts/validation_profiles.json').write_bytes(self.e.profile_path.read_bytes())
        def git(*args):
            assert (self.repo/'.git').exists()
            return subprocess.check_output(['git','-c','user.name=GateFixture','-c','user.email=fixture@example.invalid',*args],cwd=self.repo,stderr=subprocess.PIPE).decode().strip()
        git('init','-q');git('add','.');git('commit','-qm','fixture');self.source=git('rev-parse','HEAD')
        self.p=package_fixture.PackageBundleTests()
        with patch.object(package_fixture,'SOURCE',self.source):self.p.setUp()
        self.addCleanup(self.p.doCleanups)
        producer=package_fixture.PRODUCER
        self.expected=dict(schema_version=1,repository=producer['repository'],repository_id=producer['repository_id'],run_id=producer['run_id'],run_attempt=2,source_sha=self.source,trigger_head_sha=self.source,source_mode='head',workflow_path='.github/workflows/ci.yml',called_workflow_path='.github/workflows/validate-engine.yml',called_workflow_sha=producer['workflow_sha'],workflow_ref=producer['workflow_ref'],workflow_sha=producer['workflow_sha'])
        self.e.run.update(source_sha=self.source,run_id=str(uuid.uuid4()),run_attempt=2,repository=producer['repository'],workflow=producer['workflow_ref'])
        self.e.output=base/'validation'/self.source/self.e.run['run_id']/'test-debug';self.e.collect()
        self.policy=dict(schema_version=1,source_sha=self.source,version='1.2.3',required_jobs={'native':'Validate / Native'},artifact_roles={'package':'native','execution':'native'},output_prefixes={'package':'native','execution':'native_debug'},source_files={name:sha(self.repo/name) for name in ('CMakeLists.txt','scripts/validation_profiles.json')},package_inputs={'package':dict(platform='windows',configuration='Release',job_key=producer['job_key'],required_files=self.p.required)},execution_inputs={'execution':dict(platform='test',configuration='Debug',profile_name='test-debug')})
        self.archives={};self.outputs={};self.artifacts={}
        for i,(role,directory) in enumerate((('package',self.p.root),('execution',self.e.output))):
            archive=self.root/(role+'.zip')
            with zipfile.ZipFile(archive,'w') as stream:
                for path in directory.rglob('*'):
                    if path.is_file():stream.write(path,path.relative_to(directory).as_posix())
            identifier=100+i;self.archives[identifier]=archive;prefix=self.policy['output_prefixes'][role]
            self.outputs.update({prefix+'_artifact_id':str(identifier),prefix+'_artifact_digest':sha(archive),prefix+'_manifest_sha256':sha(directory/('upload-manifest.json' if role=='package' else 'manifest.json'))})
            self.artifacts[identifier]=dict(id=identifier,name='untrusted-name-'+role,digest='sha256:'+sha(archive),expired=False,expires_at='2999-01-01T00:00:00Z',workflow_run=dict(id=producer['run_id'],repository_id=producer['repository_id'],head_repository_id=producer['repository_id'],head_sha=self.source))
        self.outputs.update(native_debug_receipt_sha256=sha(self.e.run_path),native_debug_run_uuid=self.e.run['run_id'])
        self.token='disposable-gate-token';self.calls=[];self.api_count=0;self.count=0;self.prior_failure=False;self.final_changed=False;self.fillers=0;self.short_body=False;self.api_hook=None
        self.expected_path=self.root/'expected.json';self.policy_path=self.root/'policy.json';self.outputs_path=self.root/'outputs.json';self.write_inputs()
        fixture=self
        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self,*args):pass
            def do_GET(self):
                fixture.calls.append((self.path,self.headers.get('Authorization')))
                parsed=urlsplit(self.path)
                if parsed.path.startswith('/blob/'):
                    fixture.assertIsNone(self.headers.get('Authorization'))
                    body=fixture.archives[int(parsed.path.split('/')[2])].read_bytes()
                    self.send_response(200);self.send_header('Content-Length',str(len(body)+(1 if fixture.short_body else 0)));self.end_headers();self.wfile.write(body);return
                if parsed.path.endswith('/zip'):
                    identifier=int(parsed.path.split('/')[-2]);self.send_response(302);self.send_header('Location',f'https://fixture.blob.core.windows.net/blob/{identifier}/archive?sig=private-signed-query');self.end_headers();return
                value,headers=fixture.api_data(parsed)
                body=json.dumps(value).encode();self.send_response(200)
                for k,v in headers.items():self.send_header(k,v)
                self.send_header('Content-Length',str(len(body)));self.end_headers();self.wfile.write(body)
        self.server=http.server.HTTPServer(('127.0.0.1',0),Handler)
        worker=threading.Thread(target=self.server.serve_forever,daemon=True);worker.start()
        self.addCleanup(lambda:(self.server.shutdown(),self.server.server_close(),worker.join(3)))

    def write_inputs(self):
        for path,value in ((self.expected_path,self.expected),(self.policy_path,self.policy),(self.outputs_path,self.outputs)):path.write_text(json.dumps(value),encoding='utf-8')

    def api_data(self,parsed):
        route=parsed.path.removeprefix('/repos/'+self.expected['repository']);e=self.expected
        run=dict(id=e['run_id'],run_attempt=e['run_attempt'],head_sha=self.source,workflow_id=9,path=e['workflow_path'],status='in_progress',conclusion=None,event='push',repository=dict(id=e['repository_id'],full_name=e['repository']),head_repository=dict(id=e['repository_id']),referenced_workflows=[dict(path=e['repository']+'/'+e['called_workflow_path']+'@main',sha=e['called_workflow_sha'])],pull_requests=[])
        if route.endswith('/jobs'):
            attempt=int(route.split('/')[-2]);jobs=[dict(id=1000+i,run_id=e['run_id'],run_attempt=attempt,name='optional'+str(i),head_sha=self.source,status='completed',conclusion='success') for i in range(self.fillers)]
            jobs.append(dict(id=40+attempt+(100 if self.final_changed and self.api_count==3 else 0),run_id=e['run_id'],run_attempt=attempt,name=self.policy['required_jobs']['native'],head_sha=self.source,status='completed',conclusion='failure' if self.prior_failure and attempt==1 else 'success'))
            page=int(parse_qs(parsed.query)['page'][0]);size=int(parse_qs(parsed.query)['per_page'][0]);headers={}
            if page*size<len(jobs):headers['Link']=f'<https://api.github.com{parsed.path}?per_page={size}&page={page+1}>; rel="next"'
            return dict(total_count=len(jobs),jobs=jobs[(page-1)*size:page*size]),headers
        if '/attempts/' in route:return dict(run,run_attempt=int(route.split('/')[-1]),status='completed',conclusion='success'),{}
        if route.startswith('/actions/artifacts/'):return self.artifacts[int(route.rsplit('/',1)[1])],{}
        if route=='/actions/workflows/9':return dict(id=9,path=e['workflow_path']),{}
        return run,{}

    def api_factory(self,token,evidence_dir):
        self.api_count+=1
        if self.api_hook:self.api_hook(self.api_count)
        def opener(request,timeout):
            parsed=urlsplit(request.full_url)
            return urlopen(Request(f'http://127.0.0.1:{self.server.server_port}'+parsed.path+'?'+parsed.query,headers=dict(request.header_items())),timeout=timeout)
        return GitHubAPI(token,evidence_dir,opener=opener)

    def downloader(self,**kwargs):
        return download_artifact(**kwargs,connection_factory=lambda host,timeout:http.client.HTTPConnection('127.0.0.1',self.server.server_port,timeout=timeout))

    def call(self,*,fake_u1=True,**changes):
        self.assertIsNotNone(gate,'ci_release_gate not implemented')
        self.count+=1;self.work=self.root/('work-'+str(self.count))
        args=dict(repo_root=self.repo,expected_path=self.expected_path,policy_path=self.policy_path,policy_sha256=sha(self.policy_path),producer_outputs_path=self.outputs_path,work_dir=self.work,token=self.token,api_factory=self.api_factory,downloader=self.downloader,release_tag='v1.2.3');args.update(changes)
        with patch.object(execution,'verify_evidence',return_value=[]) if fake_u1 else contextlib.nullcontext():return gate.run_release_gate(**args)

    def test_real_wire_and_bytes_only_mock_lowest_u1(self):
        result=self.call();self.assertEqual(result['status'],'FIXTURE_INPUTS_VERIFIED');self.assertFalse(result['release_ready']);self.assertEqual(self.api_count,3)
        self.assertEqual(sha(result['gate_receipt']),result['gate_receipt_sha256']);self.assertEqual(sha(result['aggregate_receipt']),result['aggregate_receipt_sha256'])
        aggregate=json.loads(Path(result['aggregate_receipt']).read_text());self.assertEqual(len(aggregate['executions']['execution']['locks']),4)
        self.assertEqual([f['name'] for f in result['upload_files']],[self.p.name])
        for path in self.work.rglob('*'):
            if path.is_file() and path.suffix in ('.json','.body'):self.assertNotIn(self.token,path.read_text(errors='replace'));self.assertNotIn('private-signed-query',path.read_text(errors='replace'))

    def test_strict_flat_outputs_before_any_api(self):
        for key,value in (('native_artifact_id',True),('native_artifact_id',100),('native_artifact_id','00100'),('native_artifact_digest','X'*64),('native_debug_run_uuid','fixture-001'),('native_debug_receipt_sha256','bad')):
            original=self.outputs[key];self.outputs[key]=value;self.write_inputs()
            with self.subTest(key=key,value=value),self.assertRaises(ValueError):self.call()
            self.outputs[key]=original
        self.assertFalse(self.calls)

    def test_roles_and_duplicate_ids_fail_before_api(self):
        for mutate in (lambda:self.outputs.pop('native_artifact_id'),lambda:self.outputs.update(extra='value'),lambda:self.outputs.update(native_debug_artifact_id=self.outputs['native_artifact_id']),lambda:self.policy['output_prefixes'].update(execution='native')):
            old=copy.deepcopy((self.outputs,self.policy));mutate();self.write_inputs()
            with self.assertRaises(ValueError):self.call()
            self.outputs,self.policy=old
        self.assertFalse(self.calls)

    def test_policy_source_digest_and_duplicate_json_fail(self):
        with self.assertRaises(ValueError):self.call(policy_sha256='0'*64)
        self.policy['source_sha']='f'*40;self.write_inputs()
        with self.assertRaises(ValueError):self.call()
        self.policy['source_sha']=self.source;self.write_inputs();self.expected_path.write_text('{"schema_version":1,"schema_version":1}')
        with self.assertRaises(ValueError):self.call()
        self.assertFalse(self.calls)

    def test_pagination_resolves_required_job_on_second_page(self):
        self.fillers=100;self.call();self.assertTrue(any('page=2' in path for path,_ in self.calls))

    def test_historical_failure_prevents_any_download(self):
        self.prior_failure=True
        with self.assertRaises(ValueError):self.call()
        self.assertFalse(any(path.endswith('/zip') for path,_ in self.calls))
        receipt=json.loads((self.work/'gate.json').read_text());self.assertEqual(receipt['stage'],'hosted-initial');self.assertEqual(receipt['status'],'FAIL')

    def test_second_hosted_verification_cannot_change_job_identity(self):
        self.final_changed=True
        with self.assertRaises(ValueError):self.call()
        self.assertEqual(json.loads((self.work/'gate.json').read_text())['stage'],'hosted-final')

    def test_actual_truncated_download_preserves_first_failure(self):
        self.short_body=True
        with self.assertRaises(ValueError):self.call()
        downloads=list(self.work.glob('download-*/download.json'));self.assertEqual(len(downloads),1);self.assertEqual(json.loads(downloads[0].read_text())['status'],'FAIL')

    def test_unmocked_u1_fixture_is_rejected_precisely(self):
        with self.assertRaisesRegex(ValueError,'test-fixture'):self.call(fake_u1=False)

    def test_input_and_source_changes_before_preupload_are_rejected(self):
        for target in (self.outputs_path,self.repo/'CMakeLists.txt'):
            original=target.read_bytes()
            self.api_hook=lambda count:target.write_bytes(b'changed') if count==3 else None
            with self.subTest(target=target),self.assertRaises(ValueError):self.call()
            target.write_bytes(original);self.api_count=0

    def test_work_existing_inside_repo_and_input_alias_are_refused(self):
        sentinel=self.root/'existing';sentinel.mkdir();(sentinel/'prior').write_text('keep')
        for work in (sentinel,self.repo/'forbidden',self.policy_path):
            with self.subTest(work=work),self.assertRaises((ValueError,FileExistsError)):self.call(work_dir=work)
        self.assertEqual((sentinel/'prior').read_text(),'keep')
        with self.assertRaises(ValueError):self.call(producer_outputs_path=self.policy_path)

    def test_linked_input_rejected_without_overwrite(self):
        alias=self.root/'alias.json';os.link(self.outputs_path,alias)
        with self.assertRaises(ValueError):self.call(producer_outputs_path=alias)
        self.assertEqual(alias.read_bytes(),self.outputs_path.read_bytes())

    def test_cli_fixture_returns_77_and_no_permission(self):
        self.assertIsNotNone(gate)
        output=io.StringIO()
        argv=['--repo',str(self.repo),'--expected',str(self.expected_path),'--policy',str(self.policy_path),'--policy-sha256',sha(self.policy_path),'--producer-outputs',str(self.outputs_path),'--work',str(self.root/'cli-work')]
        with patch.dict(os.environ,{'GITHUB_TOKEN':self.token}),patch.object(execution,'verify_evidence',return_value=[]),contextlib.redirect_stdout(output):
            code=gate.main(argv,api_factory=self.api_factory,downloader=self.downloader)
        self.assertEqual(code,77);self.assertEqual(json.loads(output.getvalue())['status'],'FIXTURE_INPUTS_VERIFIED');self.assertNotIn(self.token,output.getvalue())

if __name__=='__main__':unittest.main(verbosity=2)
