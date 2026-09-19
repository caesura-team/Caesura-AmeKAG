"""Real archive/owned-process fixtures; synthetic Android tools never prove signing."""
from pathlib import Path
import copy,hashlib,io,json,os,stat,struct,subprocess,sys,tempfile,unittest,zipfile
from unittest.mock import patch
from contextlib import redirect_stdout
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'scripts'))
try:
 import android_package_contract as contract
except ModuleNotFoundError:
 contract=None
from package_runtime import run_runtime_command

def digest(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def hashed(data):return hashlib.sha256(data).hexdigest()
CHAIN='This jar contains entries whose certificate chain is invalid. Reason: PKIX path building failed: sun.security.provider.certpath.SunCertPathBuilderException: unable to find valid certification path to requested target'
SELF='This jar contains entries whose signer certificate is self-signed.'
JAR='jar verified, with signer errors.\n\nError:\n'+CHAIN+'\n'+SELF+'\n\nWarning:\nThis jar contains signatures that do not include a timestamp.\n'

def elf(machine=183):
 ident=b'\x7fELF'+bytes([2,1,1])+bytes(9)
 head=struct.pack('<16sHHIQQQIHHHHHH',ident,3,machine,1,0,64,0,0,64,56,1,64,0,0)
 return head+struct.pack('<IIQQQQQQ',1,5,0,0,0,120,120,4096)

class AndroidPackageTests(unittest.TestCase):
 def setUp(self):
  temp=tempfile.TemporaryDirectory(prefix='u24-package-fixture-');self.addCleanup(temp.cleanup)
  self.root=Path(temp.name).resolve();self.count=0
  self.apk=self.root/'final.apk';self.aab=self.root/'final.aab'
  self.apk_entries={'AndroidManifest.xml':b'fixture binary XML','classes.dex':b'fixture dex',
   'lib/arm64-v8a/libCaesuraAmeKAG.so':elf(),'lib/arm64-v8a/libSDL3.so':elf(),
   'assets/game/scripts/kag/init.lua':b'return true','assets/game/demo/first_vn/story.ks':b'fixture story'}
  self.aab_entries={'base/manifest/AndroidManifest.xml':b'fixture protobuf XML','BundleConfig.pb':b'fixture protobuf',
   'base/dex/classes.dex':b'fixture dex',**{'base/'+k:v for k,v in self.apk_entries.items() if k.startswith(('lib/','assets/'))}}
  self.expected=dict(source_sha='a'*40,package_name='com.caesura.app',version_name='1.0.1',version_code=1,
   abi='arm64-v8a',min_sdk=24,target_sdk=35,certificate_sha256='b'*64,
   required_apk_entries={k:hashed(v) for k,v in self.apk_entries.items()},required_aab_entries={k:hashed(v) for k,v in self.aab_entries.items()})
  self.tools={}
  for name in ('java','keytool','jarsigner','aapt2','zipalign','apksigner_jar'):
   path=self.root/(name+'.bin');path.write_bytes(('synthetic locked '+name).encode());self.tools[name]=dict(path=str(path),sha256=digest(path))
  self.output=dict(aapt2="package: name='com.caesura.app' versionCode='1' versionName='1.0.1'\nsdkVersion:'24'\ntargetSdkVersion:'35'\nnative-code: 'arm64-v8a'\n",
   zipalign='Verification successful\n',apksigner='Verifies\nNumber of signers: 1\nSigner #1 certificate SHA-256 digest: '+'b'*64+'\n',
   jarsigner=JAR,keytool='Signer #1:\nCertificate #1:\nSHA256: '+':'.join(['BB']*32)+'\n')
  self.codes={'jarsigner':4};self.hook=None;self.calls=[]
  self.child=self.root/'synthetic_tool.py';self.child.write_text('import sys\nprint(sys.argv[1],end="")\nraise SystemExit(int(sys.argv[2]))\n')
  self.pack()
 def pack(self):
  for path,entries in ((self.apk,self.apk_entries),(self.aab,self.aab_entries)):
   with zipfile.ZipFile(path,'w') as z:
    for name,data in entries.items():z.writestr(name,data)
 def runner(self,argv,**kw):
  role=next(k for k,v in self.tools.items() if v['path']==argv[0])
  if role=='java':role='apksigner'
  self.calls.append((role,argv,kw['env']))
  if self.hook:self.hook(role)
  return run_runtime_command([str(Path(sys.executable).resolve()),str(self.child),self.output[role],str(self.codes.get(role,0))],**kw)
 def call(self,**extra):
  self.assertIsNotNone(contract,'Android package contract missing')
  self.count+=1;self.work=self.root/('work-'+str(self.count))
  args=dict(apk_path=self.apk,apk_sha256=digest(self.apk),aab_path=self.aab,aab_sha256=digest(self.aab),
   expected=self.expected,tools=self.tools,work_dir=self.work,runner=self.runner);args.update(extra)
  return contract.verify_android_package(**args)
 def test_fixture_real_archives_and_processes_never_claim_runtime(self):
  with patch.dict(os.environ,{'JAVA_TOOL_OPTIONS':'fixture-secret-must-not-pass','GITHUB_TOKEN':'fixture-secret-must-not-pass','PYTHONPATH':'fixture-secret-must-not-pass'}):
   result=self.call()
  self.assertEqual(result['status'],'FIXTURE_PACKAGE_VERIFIED');self.assertFalse(result['release_ready'])
  self.assertEqual(result['source_provenance'],'NOT_VERIFIED');self.assertEqual(result['runtime'],'NOT_RUN')
  self.assertEqual(result['scopes']['aab_manifest_semantics'],'NOT_VERIFIED')
  self.assertEqual(len(self.calls),5);self.assertEqual(contract.verify_android_package_stable(result)['status'],'ANDROID_PACKAGE_STABLE')
  self.assertEqual(digest(result['receipt_path']),result['receipt_sha256'])
  for _,_,env in self.calls:
   self.assertNotIn('JAVA_TOOL_OPTIONS',env);self.assertNotIn('GITHUB_TOKEN',env);self.assertNotIn('PYTHONPATH',env)
   self.assertEqual(env['JAVA_HOME'],str(Path(self.tools['java']['path']).parent.parent))
  for path in self.work.rglob('*.json'):self.assertNotIn('fixture-secret-must-not-pass',path.read_text(encoding='utf-8'))
 def test_each_missing_required_entry_is_refused(self):
  for entries in (self.apk_entries,self.aab_entries):
   for name in tuple(entries):
    value=entries.pop(name);self.pack()
    with self.subTest(name=name),self.assertRaises((ValueError,RuntimeError)):self.call()
    entries[name]=value
  self.assertFalse(self.calls)
 def test_wrong_entry_bytes_and_external_archive_digest_refused(self):
  with self.assertRaises(ValueError):self.call(apk_sha256='0'*64)
  self.apk_entries['assets/game/demo/first_vn/story.ks']=b'changed';self.pack()
  with self.assertRaises((ValueError,RuntimeError)):self.call()
  self.assertFalse(self.calls)
 def test_wrong_elf_and_unexpected_native_abi_refused(self):
  name='lib/arm64-v8a/libCaesuraAmeKAG.so';self.apk_entries[name]=elf(62)
  self.aab_entries['base/'+name]=elf(62)
  self.expected['required_apk_entries'][name]=hashed(elf(62));self.expected['required_aab_entries']['base/'+name]=hashed(elf(62));self.pack()
  with self.assertRaisesRegex(ValueError,'ELF'):self.call()
  self.apk_entries[name]=elf();self.expected['required_apk_entries'][name]=hashed(elf())
  self.aab_entries['base/'+name]=elf();self.expected['required_aab_entries']['base/'+name]=hashed(elf())
  self.apk_entries['lib/x86_64/unexpected.so']=elf(62);self.pack()
  with self.assertRaisesRegex(ValueError,'native'):self.call()
 def test_aab_native_libraries_are_base_only(self):
  original=dict(self.aab_entries)
  for name,data in (('base/lib/x86_64/extra.so',elf(62)),
                    ('optional_feature/lib/x86_64/extra.so',elf(62)),
                    ('optional_feature/lib/arm64-v8a/extra.so',elf()),
                    ('optional_feature/lib/',b'')):
   with self.subTest(name=name):
    self.aab_entries=dict(original);self.aab_entries[name]=data;self.pack();self.calls=[]
    with self.assertRaisesRegex(ValueError,'native'):self.call()
    self.assertFalse(self.calls,'Unsupported native trees must fail before signature tools')
 def test_aab_base_only_rule_preserves_non_native_entries(self):
  for extra in (False,True):
   with self.subTest(extra=extra):
    if extra:
     self.aab_entries['optional_feature/assets/info.txt']=b'non-native metadata'
     self.aab_entries['optional_feature/assets/lib/note.txt']=b'not a module native directory'
    self.pack();result=self.call()
    self.assertEqual(result['status'],'FIXTURE_PACKAGE_VERIFIED')
    self.assertEqual(contract.verify_android_package_stable(result)['status'],'ANDROID_PACKAGE_STABLE')
 def test_zip_duplicates_escape_case_alias_and_links_are_refused(self):
  for name,mode in (('../escape',stat.S_IFREG),('AndroidManifest.xml',stat.S_IFREG),('androidmanifest.xml',stat.S_IFREG),('link',stat.S_IFLNK),('pipe',stat.S_IFIFO)):
   self.pack()
   with zipfile.ZipFile(self.apk,'a') as z:
    entry=zipfile.ZipInfo(name);entry.create_system=3;entry.external_attr=mode<<16;z.writestr(entry,b'target')
   with self.subTest(name=name),self.assertRaises((ValueError,RuntimeError)):self.call()
 def test_real_tool_output_manifest_and_cert_mismatch_refused(self):
  for role,text in (('aapt2',self.output['aapt2'].replace("'1.0.1'","'0.0.0'")),('aapt2',self.output['aapt2'].replace("'24'","'23'")),('apksigner',self.output['apksigner'].replace('b'*64,'c'*64)),('keytool',self.output['keytool'].replace('BB','CC'))):
   original=self.output[role];self.output[role]=text
   with self.subTest(role=role),self.assertRaises(ValueError):self.call()
   self.output[role]=original
 def test_jarsigner_exit4_does_not_allow_expired_unsigned_or_unknown_error(self):
  for text,code in ((JAR.replace(CHAIN,'This jar contains entries whose signer certificate has expired.\n'+CHAIN),4),(JAR.replace(CHAIN,'This jar contains entries whose signer certificate is not yet valid.\n'+CHAIN),4),(JAR.replace(SELF,'Unknown signer error.'),4),('jar is unsigned.',0),('jar verified.',4),(JAR,20)):
   self.output['jarsigner']=text;self.codes['jarsigner']=code
   with self.subTest(text=text,code=code),self.assertRaises(ValueError):self.call()
 def test_multiple_signers_and_ambiguous_manifests_are_refused(self):
  cases=[('aapt2',self.output['aapt2']+self.output['aapt2']),
         ('apksigner',self.output['apksigner'].replace('Number of signers: 1','Number of signers: 2')),
         ('keytool',self.output['keytool']+'Signer #2:\nCertificate #1:\nSHA256: '+':'.join(['CC']*32)+'\n')]
  for role,text in cases:
   original=self.output[role];self.output[role]=text
   with self.subTest(role=role),self.assertRaises(ValueError):self.call()
   self.output[role]=original
 def test_java_home_is_explicitly_isolated_from_default_user_keystore(self):
  self.call()
  for role,argv,_ in self.calls:
   if role in ('apksigner','jarsigner','keytool'):
    prefix='-Duser.home=' if role=='apksigner' else '-J-Duser.home='
    with self.subTest(role=role):self.assertIn(prefix+str(self.work),argv)
 def test_required_context_cannot_omit_core_entries_or_use_bad_types(self):
  for mutate in (lambda e:e['required_apk_entries'].pop('AndroidManifest.xml'),lambda e:e.update(version_code=True),lambda e:e.update(abi='x86'),lambda e:e.update(certificate_sha256='bad')):
   expected=copy.deepcopy(self.expected);mutate(expected)
   with self.assertRaises(ValueError):self.call(expected=expected)
  self.assertFalse(self.calls)
 def test_tool_external_locks_links_and_nonzero_exits(self):
  tools=copy.deepcopy(self.tools);tools['aapt2']['sha256']='0'*64
  with self.assertRaises(ValueError):self.call(tools=tools)
  alias=self.root/'alias';os.link(self.apk,alias)
  with self.assertRaises(ValueError):self.call(apk_path=alias)
  alias.unlink();self.codes['zipalign']=1
  with self.assertRaises(ValueError):self.call()
 def test_new_work_and_failure_receipt_are_preserved(self):
  self.codes['aapt2']=1
  with self.assertRaises(ValueError):self.call()
  path=self.work/'android-package.json';before=path.read_bytes();self.assertEqual(json.loads(before)['status'],'FAIL')
  with self.assertRaises((ValueError,FileExistsError)):self.call(work_dir=self.work)
  self.assertEqual(path.read_bytes(),before)
 def test_stability_reopens_archives_tools_controls_receipt_and_prepared_files(self):
  result=self.call()
  targets=[self.apk,Path(self.tools['java']['path']),Path(result['control']['path']),Path(result['receipt_path']),Path(result['prepared']['apk']['package_path'])/'classes.dex']
  for path in targets:
   original=path.read_bytes();path.write_bytes(b'changed')
   with self.subTest(path=path),self.assertRaises((ValueError,RuntimeError)):contract.verify_android_package_stable(result)
   path.write_bytes(original)
  changed=copy.deepcopy(result);changed['expected']['version_name']='changed'
  with self.assertRaises(ValueError):contract.verify_android_package_stable(changed)
 def test_changes_during_commands_fail_final_stability(self):
  self.hook=lambda role:self.apk.write_bytes(b'changed') if role=='keytool' else None
  with self.assertRaises((ValueError,RuntimeError)):self.call()
  self.assertEqual(json.loads((self.work/'android-package.json').read_text())['status'],'FAIL')
 def request(self):
  value=dict(apk_path=str(self.apk),apk_sha256=digest(self.apk),aab_path=str(self.aab),aab_sha256=digest(self.aab),expected=self.expected,tools=self.tools)
  path=self.root/'request.json';path.write_text(json.dumps(value),encoding='utf-8');return path
 def test_original_request_and_process_receipts_are_locked(self):
  request=self.request();result=self.call(request_path=request,request_sha256=digest(request))
  for path in (request,Path(result['commands'][0]['process']['control_dir'])/'run.json'):
   before=path.read_bytes();path.write_bytes(before+b' ')
   with self.subTest(path=path),self.assertRaises(ValueError):contract.verify_android_package_stable(result)
   path.write_bytes(before)
 def test_cli_injected_runner_returns_77_and_locks_raw_request(self):
  request=self.request();capture=io.StringIO();work=self.root/'cli-work'
  with redirect_stdout(capture):
   code=contract.main(['--request',str(request),'--request-sha256',digest(request),'--work',str(work)],runner=self.runner)
  result=json.loads(capture.getvalue());self.assertEqual(code,77);self.assertEqual(result['status'],'FIXTURE_PACKAGE_VERIFIED')
  self.assertFalse(result['release_ready']);contract.verify_android_package_stable(result)
  request.write_bytes(request.read_bytes()+b' ')
  with self.assertRaises(ValueError):contract.verify_android_package_stable(result)
 def test_real_cli_duplicate_json_keeps_first_failure_receipt(self):
  request=self.root/'invalid.json';request.write_bytes(b'{"expected":{},"expected":{}}');work=self.root/'bad-cli'
  args=[sys.executable,str(Path(contract.__file__)), '--request',str(request),'--request-sha256',digest(request),'--work',str(work)]
  result=subprocess.run(args,capture_output=True,text=True,timeout=10)
  self.assertEqual(result.returncode,1);self.assertEqual(json.loads(result.stdout)['status'],'FAIL')
  receipt=work/'android-package.json';self.assertTrue(receipt.is_file());before=receipt.read_bytes()
  self.assertEqual(json.loads(before)['stage'],'request')
  retry=subprocess.run(args,capture_output=True,text=True,timeout=10);self.assertEqual(retry.returncode,1);self.assertEqual(receipt.read_bytes(),before)
 def test_cli_owned_timeout_preserves_failure_and_cleanup(self):
  self.child.write_text('import time\ntime.sleep(10)\n');request=self.request();capture=io.StringIO();work=self.root/'timeout-cli'
  def timeout_runner(argv,**kw):
   kw['timeout']=0.3;return self.runner(argv,**kw)
  with redirect_stdout(capture):
   code=contract.main(['--request',str(request),'--request-sha256',digest(request),'--work',str(work)],runner=timeout_runner)
  self.assertEqual(code,1);self.assertEqual(json.loads(capture.getvalue())['status'],'FAIL')
  result=json.loads((work/'android-package.json').read_text());self.assertEqual(result['stage'],'aapt2')
  run=json.loads((work/'aapt2-process'/'run.json').read_text());self.assertEqual(run['status'],'TIMED_OUT');self.assertEqual(run['owned_tree_cleanup'],'COMPLETE')

if __name__=='__main__':unittest.main(verbosity=2)
