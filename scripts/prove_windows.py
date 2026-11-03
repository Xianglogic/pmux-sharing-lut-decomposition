"""Reprove the archived nine-case netlists with Windows OSS CAD Suite."""
import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import ctypes
import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time

from EqyRunner import coverage
import WindowsJob

ROOT = Path(__file__).resolve().parents[1]

def load(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))

def save(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def main():
    if os.name != 'nt':
        raise SystemExit('Use this entry on Windows. Linux/WSL uses Reprove.py.')
    ctypes.windll.kernel32.SetErrorMode(0x8003)
    parser = argparse.ArgumentParser()
    parser.add_argument('--oss-root', required=True, type=Path)
    parser.add_argument('--make-bin', required=True, type=Path)
    parser.add_argument('--shell-bin', required=True, type=Path)
    parser.add_argument('--work-root', type=Path, default=Path(tempfile.gettempdir()))
    parser.add_argument('--case', default='all')
    parser.add_argument('--jobs', type=int, default=2, choices=range(1, 5))
    args = parser.parse_args()
    suite = args.oss_root.resolve()
    make_bin = args.make_bin.resolve()
    shell_bin = args.shell_bin.resolve()
    required = [suite / 'lib/python3.exe', suite / 'bin/eqy-script.py', suite / 'bin/yosys.exe',
                suite / 'bin/sby.exe', shell_bin / 'bash.exe', shell_bin / 'sh.exe']
    for path in required:
        if not path.is_file():
            raise SystemExit(f'Missing dependency: {path}')
    make = next((path for path in [make_bin / 'make.exe', make_bin / 'mingw32-make.exe'] if path.is_file()), None)
    if make is None:
        raise SystemExit('The Make directory must contain GNU make.exe or mingw32-make.exe.')
    args.work_root.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix='pmux_eqy_win_', dir=args.work_root)).resolve()
    if not work.as_posix().isascii() or any(char.isspace() for char in work.as_posix()):
        raise SystemExit('Use --work-root with an ASCII directory without spaces; EQY generates shell commands there.')
    destination = ROOT / 'runs' / ('eqy_windows_' + datetime.datetime.now().strftime('%Y%m%d_%H%M%S_%f'))
    destination.mkdir(parents=True, exist_ok=False)
    env = os.environ.copy()
    shim = work / 'toolbin'
    shim.mkdir()
    if make.name.lower() != 'make.exe':
        shutil.copy2(make, shim / 'make.exe')
    env['PATH'] = ';'.join(map(str, [suite / 'bin', suite / 'lib', shim, make_bin, shell_bin])) + ';' + env.get('PATH', '')
    env['YOSYSHQ_ROOT'] = str(suite)
    env['PYTHON_EXECUTABLE'] = str(suite / 'lib/python3.exe')
    env['PYTHONIOENCODING'] = 'utf-8'
    env['SHELL'] = env['MAKESHELL'] = (shell_bin / 'sh.exe').as_posix()
    for name in ['PYTHONPATH', 'PYTHONHOME', 'YOSYS', 'ABC', 'SBY', 'YOSYS_DATDIR']:
        env.pop(name, None)
    versions = {}
    for name, executable, arguments in [('eqy', suite / 'bin/eqy.exe', ['--version']),
                                         ('yosys', suite / 'bin/yosys.exe', ['-V']),
                                         ('sby', suite / 'bin/sby.exe', ['--version']),
                                         ('make', make, ['--version']),
                                         ('bash', shell_bin / 'bash.exe', ['--version'])]:
        result = subprocess.run([str(executable)] + arguments, env=env, capture_output=True, text=True,
                                encoding='utf-8', errors='replace', timeout=20)
        versions[name] = {'exit': result.returncode, 'version': result.stdout.strip(), 'sha256': sha(executable)}
        if result.returncode:
            raise SystemExit(f'{name} failed to start: {result.stderr}')
    versions['eqy_script_sha256'] = sha(suite / 'bin/eqy-script.py')
    save(destination / 'TOOLS.json', versions)
    manifest = load(ROOT / 'MANIFEST_SHA256.json')['files']
    index = load(ROOT / 'PROOF_TEMPLATE_INDEX.json')['jobs']
    capture = load(ROOT / 'evidence/current/CAPTURE_FULL.json')
    selected = [item for item in index if item['kind'] in {'final_pair', 'rtl_to_boundary', 'boundary_to_final'}
                and (args.case == 'all' or item['case'] == args.case)]
    assert len(selected) == (45 if args.case == 'all' else 5), 'Unknown case or incomplete proof index'
    configs = work / 'configs'
    configs.mkdir()
    tasks = []
    for number, item in enumerate(selected):
        relative = 'proof_templates/' + item['key'] + '.eqy'
        original = ROOT / relative
        assert sha(original) == manifest[relative], f'Changed proof configuration: {relative}'
        config = configs / f'p{number:02}.eqy'
        config.write_bytes(original.read_bytes())
        inputs = {}
        for name in re.findall(r'^read_(?:rtlil|json|verilog)\b[^\n]*?"([^"]+)"', original.read_text(encoding='utf-8-sig'), re.M):
            source = (ROOT / name).resolve()
            assert source.is_relative_to(ROOT) and source.is_file(), name
            digest = sha(source)
            assert digest == manifest[name], f'Changed proof input: {name}'
            target = work / name
            target.parent.mkdir(parents=True, exist_ok=True)
            if not target.exists():
                shutil.copy2(source, target)
            inputs[name] = digest
        ports_file = ROOT / 'evidence/current' / capture['cases'][item['case']]['folder'] / 'initialized.json'
        ports = load(ports_file)['modules'][capture['cases'][item['case']]['top']]['ports']
        tasks.append(dict(item, config=config, number=number, ports=ports, config_sha256=sha(config), input_sha256=inputs))

    def invoke(name, config, ports=None, expected='PASS'):
        job = work / name
        log = work / (name + '.log')
        arguments = [str(suite / 'lib/python3.exe'), str(ROOT / 'scripts/eqy_windows_lf.py'),
                     str(suite / 'bin/eqy-script.py'), '-j', '1', '-d', job.as_posix(), config.as_posix()]
        start = time.monotonic()
        with log.open('wb') as output:
            try:
                result = WindowsJob.run(arguments, timeout=420, cwd=work, env=env, stdout=output, stderr=subprocess.STDOUT)
                code = result.returncode
            except Exception as exc:
                code = type(exc).__name__ + ': ' + str(exc)
        status = 'PASS' if code == 0 and (job / 'PASS').is_file() else 'FAIL' if code == 2 and (job / 'FAIL').is_file() else 'ERROR'
        checked = coverage(job, ports) if ports is not None and status == 'PASS' else None
        valid = status == expected and (checked is None or (checked['all_outputs_covered'] and checked['proof_dependencies_closed']))
        return {'job': name, 'status': status, 'expected': expected, 'accepted': valid, 'exit': code,
                'seconds': time.monotonic() - start, 'coverage': checked, 'log': name + '.log'}

    control_gold = work / 'control_gold.v'
    control_gold.write_text('module control(input [3:0] a,b,input s,output [3:0] y); assign y=s?a:b; endmodule\n', encoding='ascii')
    controls = []
    for suffix, expression, expected in [('good', '(a & {4{s}})|(b & {4{!s}})', 'PASS'), ('bad', "(s?a:b)^4'b0001", 'FAIL')]:
        gate = work / f'control_{suffix}.v'
        gate.write_text(f'module control(input [3:0] a,b,input s,output [3:0] y); assign y={expression}; endmodule\n', encoding='ascii')
        config = configs / f'control_{suffix}.eqy'
        config.write_text(f'[gold]\nread_verilog "{control_gold.name}"\nprep -top control\n\n[gate]\nread_verilog "{gate.name}"\nprep -top control\n\n[match *]\nnodefault\ngold-match a\ngold-match b\ngold-match s\ngold-match y\n\n[strategy pdr]\nuse sby\nengine abc pdr\ntimeout 60\n', encoding='utf-8')
        controls.append(invoke('control_' + suffix, config, expected=expected))
    save(destination / 'CONTROLS.json', controls)
    record = {'platform': 'native Windows; no WSL', 'case': args.case, 'expected_jobs': len(tasks), 'complete': False,
              'controls_passed': all(item['accepted'] for item in controls), 'work_directory': str(work),
              'archived_netlists_reproved': True, 'synthesis_rerun': False, 'jobs': []}
    save(destination / 'RESULTS.json', record)
    if record['controls_passed']:
        with ThreadPoolExecutor(max_workers=args.jobs) as pool:
            pending = {pool.submit(invoke, f'p{task["number"]:02}', task['config'], task['ports']): task for task in tasks}
            for future in as_completed(pending):
                task = pending[future]
                row = future.result()
                row.update({name: task[name] for name in ['key', 'case', 'kind', 'config_sha256', 'input_sha256']})
                record['jobs'].append(row)
                save(destination / 'RESULTS.json', record)
                print(f'{task["key"]}: {row["status"]} ({len(record["jobs"])}/{len(tasks)})', flush=True)
    record['complete'] = len(record['jobs']) == len(tasks)
    record['all_checks_pass'] = record['controls_passed'] and record['complete'] and all(item['accepted'] for item in record['jobs'])
    save(destination / 'RESULTS.json', record)
    shutil.copytree(work, destination / 'work')
    print('Results: ' + str(destination), flush=True)
    print('All checks passed: ' + str(record['all_checks_pass']), flush=True)
    # Keep the isolated temporary directory available for failed-run diagnosis.
    if not record['all_checks_pass']:
        raise SystemExit(1)

if __name__ == '__main__':
    main()
