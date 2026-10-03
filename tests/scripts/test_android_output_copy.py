"""Execute the maintained desktop POST_BUILD copy block with real CMake.

LANGUAGES NONE and a custom target isolate copy behavior from the compiler.
Only TARGET_FILE_DIR is replaced with the fixture output directory because
custom targets have no binary file. This is not an Android native build.
"""
from pathlib import Path
import hashlib
import json
import os
import shutil
import subprocess
import tempfile
import unittest
import uuid

ROOT = Path(__file__).resolve().parents[2]


class AndroidOutputCopyTests(unittest.TestCase):
    def run_copy(self, *, android, projects):
        cmake = shutil.which('cmake')
        self.assertIsNotNone(cmake, 'Actual CMake is required')
        generator = []
        if os.name == 'nt':
            ninja = shutil.which('ninja')
            if not ninja:
                visual_studio = Path(os.environ.get('ProgramFiles', 'C:/Program Files')) / 'Microsoft Visual Studio'
                candidates = (visual_studio / year / edition / 'Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe'
                              for year in ('2022', '18') for edition in ('Community', 'Professional', 'Enterprise', 'BuildTools'))
                ninja = next((str(path) for path in candidates if path.is_file()), None)
            self.assertIsNotNone(ninja, 'Actual Ninja on PATH or in Visual Studio is required')
            generator = ['-G', 'Ninja', '-DCMAKE_MAKE_PROGRAM=' + ninja]
        preserved = os.environ.get('CAESURA_OUTPUT_COPY_EVIDENCE')
        if preserved:
            attempt = Path(preserved) / (self._testMethodName[:20] + '-' + uuid.uuid4().hex[:12])
            attempt.mkdir(parents=True)
        else:
            temporary = tempfile.TemporaryDirectory(prefix='caesura output copy ')
            self.addCleanup(temporary.cleanup)
            attempt = Path(temporary.name)
        source = attempt / 'source'
        source.mkdir()
        expected = {}
        for name in ('scripts', 'demo', 'assets', 'web/dist', *(['projects'] if projects else [])):
            relative = name + ('/index.html' if name == 'web/dist' else '/content.txt')
            p = source / relative
            p.parent.mkdir(parents=True, exist_ok=True)
            expected[relative] = ('retained fixture ' + relative).encode()
            p.write_bytes(expected[relative])
        maintained = (ROOT / 'CMakeLists.txt').read_text(encoding='utf-8')
        block = maintained.split('# Copy scripts to each config output directory', 1)[1].split('# -- Steam (optional)', 1)[0]
        block = block.replace('$<TARGET_FILE_DIR:${PROJECT_NAME}>', '${CMAKE_BINARY_DIR}/output')
        (source / 'CMakeLists.txt').write_text(
            'cmake_minimum_required(VERSION 3.21)\nproject(CaesuraOutputFixture LANGUAGES NONE)\n'
            'set(ANDROID ' + ('TRUE' if android else 'FALSE') + ')\n'
            'add_custom_target(${PROJECT_NAME} ALL)\n' + block, encoding='utf-8')
        report = dict(source_sha256=hashlib.sha256(maintained.encode()).hexdigest(), commands=[])
        for name, argv in [('configure', [cmake, '-S', str(source), '-B', str(attempt / 'build'), *generator]),
                           ('build', [cmake, '--build', str(attempt / 'build'), '--config', 'Debug'])]:
            result = subprocess.run(argv, cwd=attempt, capture_output=True, timeout=90)
            for stream in ('stdout', 'stderr'):
                (attempt / (name + '.' + stream)).write_bytes(getattr(result, stream))
            report['commands'].append(dict(name=name, argv=argv, exit_code=result.returncode,
                stdout_sha256=hashlib.sha256(result.stdout).hexdigest(), stderr_sha256=hashlib.sha256(result.stderr).hexdigest()))
            (attempt / 'commands.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
            self.assertEqual(result.returncode, 0, (result.stdout + result.stderr).decode(errors='replace'))
        output = attempt / 'build/output'
        actual = {p.relative_to(output).as_posix(): p.read_bytes() for p in output.rglob('*') if p.is_file()}
        self.assertEqual(actual, {} if android else expected)
        if not projects:
            self.assertFalse((source / 'projects').exists(), 'Android must not create a desktop project directory')

    def test_android_build_does_not_copy_present_desktop_payloads(self):
        self.run_copy(android=True, projects=True)

    def test_android_build_does_not_create_or_copy_desktop_project_directory(self):
        self.run_copy(android=True, projects=False)

    def test_desktop_build_keeps_complete_runtime_output(self):
        self.run_copy(android=False, projects=True)


if __name__ == '__main__':
    unittest.main(verbosity=2)
