"""Replay frozen V53 proofs with undefined state initialized to zero (QA C149).

This does not modify the submission archive or synthesis executables. The OSS
installation is supplied explicitly, so all prover tools use one environment.
"""
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor, as_completed
import argparse
import hashlib
import json
import os
import re
import signal
import subprocess
import time

ROOT = Path(__file__).resolve().parent
PACKAGE_ROOT = Path(__file__).resolve().parents[1]


def load(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))


def save(path, data):
    path.write_text(json.dumps(data, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def coverage(job, ports):
    covered, cut_inputs = {}, {}
    for path in (job / 'partitions').glob('*.json'):
        part = load(path)['partition']
        for name, bits in part['outbits'].items():
            covered.setdefault(name, set()).update(bits)
        for name, bits in part['inbits'].items():
            cut_inputs.setdefault(name, set()).update(bits)
    missing = {}
    for name, port in ports.items():
        if port['direction'] == 'output':
            offset = port.get('offset', 0)
            absent = set(range(offset, offset + len(port['bits']))) - covered.get(name, set())
            if absent:
                missing[name] = sorted(absent)
    inputs = {name for name, p in ports.items() if p['direction'] == 'input'}
    unclosed = {name: sorted(bits - covered.get(name, set())) for name, bits in cut_inputs.items()
                if name not in inputs and bits - covered.get(name, set())}
    extra = {name: sorted(bits) for name, bits in cut_inputs.items() if name not in inputs}
    return {'all_outputs_covered': not missing, 'proof_dependencies_closed': not unclosed,
            'missing_output_bits': missing, 'unclosed_cutpoint_bits': unclosed,
            'non_primary_partition_inputs': extra}


def one(task, oss, env):
    folder = ROOT / 'proofs_zero_init' / task['key']
    folder.mkdir(parents=True, exist_ok=False)
    config = folder / 'pair.eqy'
    config.write_text(task['config'], encoding='utf-8')
    job = folder / 'run'
    command = [str(oss / 'bin/eqy'), '--yosys', str(oss / 'bin/yosys'), '-j', '1',
               '-d', str(job), str(config)]
    start = time.monotonic()
    row = {k: v for k, v in task.items() if k not in ('config', 'ports')}
    row.update(config=str(config.relative_to(ROOT)), config_sha256=sha(config), command=command,
               status='NOT_RUN', passed=False, timeout=False)
    try:
        with (folder / 'console.log').open('wb') as console:
            process = subprocess.Popen(command, cwd=PACKAGE_ROOT, env=env, stdout=console,
                                       stderr=subprocess.STDOUT, start_new_session=True)
            try:
                process.wait(timeout=360)
            except subprocess.TimeoutExpired:
                row['timeout'] = True
                os.killpg(process.pid, signal.SIGTERM)
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait()
            row['exit'] = process.returncode
        marker = next((s for s in ['PASS', 'FAIL', 'UNKNOWN'] if (job / s).is_file()), 'ERROR')
        text = (job / 'logfile.txt').read_text(errors='replace') if (job / 'logfile.txt').exists() else ''
        checked = coverage(job, task['ports'])
        row.update(status='UNPROVEN_TIMEOUT' if row['timeout'] else marker,
                   coverage=checked, ignored_boxed_modules='Ignoring boxed module' in text,
                   seconds=time.monotonic() - start)
        row['passed'] = (row['status'] == 'PASS' and row['exit'] == 0
                         and checked['all_outputs_covered'] and checked['proof_dependencies_closed']
                         and not checked['non_primary_partition_inputs'] and not row['ignored_boxed_modules'])
        if row['status'] == 'PASS' and not row['passed']:
            row['status'] = 'COVERAGE_REVIEW'
    except Exception as exc:
        row.update(status='ERROR', error=repr(exc), seconds=time.monotonic() - start)
    save(folder / 'result.json', row)
    return row


if __name__ == "__main__":
    raise SystemExit("Use scripts/Reprove.py as the portable entry point.")
