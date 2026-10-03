"""Validate real generated MSVC projects; optionally link the actual Cubism Core.

--inspect-only reads a retained pre-fix SDK build and exposes its actual CRT
mismatch. No engine or probe executable is run by this regression.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import sys
import tempfile
import xml.etree.ElementTree as ET
from pathlib import Path

NS = {'m': 'http://schemas.microsoft.com/developer/msbuild/2003'}
VARIANTS = {
    'MultiThreadedDLL': 'MD',
    'MultiThreadedDebugDLL': 'MDd',
    'MultiThreaded': 'MT',
    'MultiThreadedDebug': 'MTd',
}


def generated_properties(project: Path, configuration: str) -> tuple[str, list[str]]:
    root = ET.parse(project).getroot()
    groups = [group for group in root.findall('m:ItemDefinitionGroup', NS)
              if f'{configuration}|x64' in group.get('Condition', '')]
    if len(groups) != 1:
        raise AssertionError(f'{project}: missing unique {configuration}|x64 group')
    runtime = groups[0].findtext('m:ClCompile/m:RuntimeLibrary', namespaces=NS)
    libraries = groups[0].findtext('m:Link/m:AdditionalDependencies', '', NS).split(';')
    return runtime, libraries


def inspect_build(build: Path, configuration: str, app: str, framework: str) -> dict:
    runtime, libraries = generated_properties(build / f'{app}.vcxproj', configuration)
    framework_runtime, _ = generated_properties(build / f'{framework}.vcxproj', configuration)
    if runtime != framework_runtime:
        raise AssertionError(f'application CRT {runtime} differs from Framework {framework_runtime}')
    if runtime not in VARIANTS:
        raise AssertionError(f'Unspecified or unsupported generated CRT: {runtime}')
    selected = [Path(name).name for name in libraries
                if Path(name).name.lower().startswith('live2dcubismcore_')]
    expected = f'Live2DCubismCore_{VARIANTS[runtime]}.lib'
    if not selected or set(name.lower() for name in selected) != {expected.lower()}:
        raise AssertionError(f'{app}/{configuration}: generated {runtime} requires {expected}; got {selected}')
    return {'configuration': configuration, 'runtime': runtime, 'core': expected}


def run_logged(argv: list[str], directory: Path, name: str) -> str:
    sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'scripts'))
    from package_runtime import run_runtime_command
    with (directory / f'{name}.stdout.log').open('xb') as stdout, \
            (directory / f'{name}.stderr.log').open('xb') as stderr:
        result = run_runtime_command(argv, directory, dict(os.environ),
                                     directory / f'{name}-owned', stdout, stderr, 90)
    text = (directory / f'{name}.stdout.log').read_text(encoding='utf-8', errors='replace')
    text += (directory / f'{name}.stderr.log').read_text(encoding='utf-8', errors='replace')
    (directory / f'{name}.command.json').write_text(
        json.dumps({'argv': argv, 'owned': result}, indent=2) + '\n', encoding='utf-8')
    if not (result['status'] == 'EXITED' and type(result['actual_exit_code']) is int
            and result['actual_exit_code'] == 0 and type(result['launcher_exit_code']) is int
            and result['launcher_exit_code'] == 0 and result['owned_tree_cleanup'] == 'COMPLETE'
            and all(result[key] is False for key in ('timed_out', 'forced_kill', 'stop_requested'))):
        raise AssertionError(f'{name}: owned CMake did not complete cleanly; retained logs in {directory}')
    return text


def matrix(args: argparse.Namespace) -> list[dict]:
    args.work_root.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix='cubism-crt-', dir=args.work_root))
    cases = [
        ('default', None, ('MultiThreadedDebugDLL', 'MultiThreadedDLL')),
        *[(name, name, (name, name)) for name in VARIANTS],
        ('static-per-config', 'MultiThreaded$<$<CONFIG:Debug>:Debug>',
         ('MultiThreadedDebug', 'MultiThreaded')),
        ('dll-per-config', 'MultiThreaded$<$<CONFIG:Debug>:Debug>DLL',
         ('MultiThreadedDebugDLL', 'MultiThreadedDLL')),
    ]
    results = []
    for index, (name, value, expected_runtimes) in enumerate(cases):
        work = root / str(index)
        source, build = work / 'source', work / 'build'
        source.mkdir(parents=True)
        (source / 'framework.cpp').write_text(
            '#include <Live2DCubismCore.h>\nunsigned core_version() { return csmGetVersion(); }\n',
            encoding='utf-8')
        (source / 'main.cpp').write_text(
            'extern unsigned core_version();\nint main() { return core_version() == 0; }\n', encoding='utf-8')
        (source / 'CMakeLists.txt').write_text(
            'cmake_minimum_required(VERSION 3.25)\n'
            'project(CubismCrtRegression LANGUAGES CXX)\n'
            'include("${CORE_HELPER}")\n'
            'add_library(Framework STATIC framework.cpp)\n'
            'caesura_add_windows_cubism_core(Core "${CORE_DIR}" Framework)\n'
            'target_link_libraries(Framework PUBLIC Core)\n'
            'add_executable(Consumer main.cpp)\n'
            'target_link_libraries(Consumer PRIVATE Framework)\n', encoding='utf-8')
        command = [str(args.cmake), '-S', str(source), '-B', str(build),
                   '-G', args.generator, '-A', 'x64',
                   f'-DCORE_HELPER={args.helper.resolve().as_posix()}',
                   f'-DCORE_DIR={args.core_dir.resolve().as_posix()}']
        if args.generator_instance:
            command.append(f'-DCMAKE_GENERATOR_INSTANCE={args.generator_instance}')
        if value is not None:
            command.append(f'-DCMAKE_MSVC_RUNTIME_LIBRARY={value}')
        run_logged(command, work, 'configure')
        for configuration, expected in zip(('Debug', 'Release'), expected_runtimes):
            observed = inspect_build(build, configuration, 'Consumer', 'Framework')
            if observed['runtime'] != expected:
                raise AssertionError(f'{name}: configured compiler CRT changed: {observed}')
            if args.check_links:
                output = run_logged([str(args.cmake), '--build', str(build), '--config', configuration,
                                     '--target', 'Consumer', '--parallel', '1'], work, f'link-{configuration}')
                if re.search(r'LNK4098|NODEFAULTLIB', output, re.IGNORECASE):
                    raise AssertionError(f'{name}/{configuration}: CRT warning or suppression in actual link')
            results.append({'case': name, **observed, 'actual_link_checked': args.check_links})
    (root / 'result.json').write_text(json.dumps(results, indent=2) + '\n', encoding='utf-8')
    print(f'Cubism CRT generated evidence: {root}')
    return results


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--existing-build', type=Path)
    parser.add_argument('--configuration', default='Debug')
    parser.add_argument('--inspect-only', action='store_true')
    parser.add_argument('--cmake', type=Path)
    parser.add_argument('--helper', type=Path)
    parser.add_argument('--core-dir', type=Path)
    parser.add_argument('--work-root', type=Path)
    parser.add_argument('--generator-instance')
    parser.add_argument('--generator', default='Visual Studio 17 2022')
    parser.add_argument('--check-links', action='store_true')
    args = parser.parse_args()
    if args.existing_build:
        print(json.dumps(inspect_build(args.existing_build, args.configuration,
                                       'CaesuraAmeKAG', 'CubismFramework')))
    if args.inspect_only:
        if not args.existing_build:
            parser.error('--inspect-only requires --existing-build')
        return
    if not all((args.cmake, args.helper, args.core_dir, args.work_root)):
        parser.error('matrix needs --cmake, --helper, --core-dir and --work-root')
    results = matrix(args)
    if len(results) != 14:
        raise AssertionError('Complete default/four explicit/two per-config matrix required')
    print('ALL CUBISM CRT CONFIGURATION TESTS PASSED')


if __name__ == '__main__':
    main()
