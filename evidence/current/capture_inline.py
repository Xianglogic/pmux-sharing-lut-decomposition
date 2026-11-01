"""Untimed captures with logical initialization fixed before synthesis.

Original RTL and frozen executables are read only. The common RTLIL input is
created by the original Yosys frontend, before any state-polarity optimization.
"""
from pathlib import Path
from collections import Counter
import argparse
import hashlib
import importlib.util
import json
import os
import subprocess

ROOT = Path(__file__).resolve().parent
PACKAGE = ROOT.parents[1]
INIT = 'setundef -init -params -zero'


def load(p):
    return json.loads(p.read_text(encoding='utf-8-sig'))


def save(p, d):
    p.write_text(json.dumps(d, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')


def sha(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()


def canonical(value):
    if isinstance(value, dict):
        return {k: ({a: canonical(b) for a, b in v.items() if a != 'src'}
                    if k == 'attributes' else canonical(v))
                for k, v in value.items() if k != 'creator'}
    if isinstance(value, list):
        return [canonical(v) for v in value]
    return value


def graph(p):
    return hashlib.sha256(json.dumps(canonical(load(p)), sort_keys=True, separators=(',', ':')).encode()).hexdigest()


def init_bits(module):
    out = {}
    for net in module['netnames'].values():
        value = net.get('attributes', {}).get('init')
        if value is None:
            continue
        for bit, state in zip(net['bits'], str(value)[::-1]):
            if isinstance(bit, int) and state in '01':
                assert bit not in out or out[bit] == state
                out[bit] = state
    return out


def mapped_resources(path, top):
    m = load(path)['modules'][top]
    t = Counter(c['type'] for c in m['cells'].values())
    assert not set(t) - {'cycloneiv_lcell_comb', 'cycloneive_lcell_comb', 'dffeas', '$not', '$print', '$scopeinfo'}
    for cell in m['cells'].values():
        if cell['type'] == '$not':
            assert int(cell['parameters']['Y_WIDTH'], 2) == 1
    lut = t['cycloneiv_lcell_comb'] + t['cycloneive_lcell_comb']
    return {'logic': lut + t['$not'], 'lut': lut, 'not_count': t['$not'],
            'ff': t['dffeas'], 'cells': len(m['cells']), 'types': dict(sorted(t.items()))}


def run(exe, script, log, commands, env):
    script.write_text('; '.join(commands) + '\n', encoding='utf-8')
    with log.open('wb') as f:
        p = subprocess.run([str(exe), '-Q', '-T', '-s', str(script)], cwd=PACKAGE,
                           stdout=f, stderr=subprocess.STDOUT, env=env,
                           creationflags=subprocess.CREATE_NO_WINDOW, timeout=180)
    if p.returncode:
        print(log.read_text(errors='replace')[-1600:], flush=True)
    assert p.returncode == 0, (exe, script, p.returncode)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--tag', required=True, choices=['pilot', 'full'])
    ap.add_argument('--only', default='all')
    args = ap.parse_args()
    destination = ROOT / ('capture_' + args.tag)
    destination.mkdir(exist_ok=False)
    manifest = load(PACKAGE / 'INPUT_MANIFEST.json')
    cases = [c for c in manifest['cases'] if args.only == 'all' or c['case_id'] == args.only]
    assert len(cases) == (9 if args.only == 'all' else 1)
    exes = {arm: PACKAGE / 'runtime' / arm / 'yosys.exe' for arm in ['baseline', 'candidate']}
    assert sha(exes['baseline']) == '4096266b5ad31531c67a1c81f88066c0a3cdf31008fe111a38c0443c2b04dec7'
    assert sha(exes['candidate']) == load(PACKAGE / 'BUILD.json')['exe_sha256']
    env = os.environ.copy()
    env['ABC'] = str(PACKAGE / 'runtime/candidate/yosys-abc.exe')
    env.pop('YOSYS_DATDIR', None)
    record = {'tag': args.tag, 'cases': {}, 'initialization_command': INIT,
              'source_contract': 'Unknown logical RTL state and x parameter/connection bits set to zero before state recoding; explicitly defined init bits retained.',
              'synthesis_timing_excluded': True, 'frozen_exe_sha256': {a: sha(p) for a, p in exes.items()},
              'not_the_archived_submission_netlists': True, 'complete': False}
    spec = importlib.util.spec_from_file_location('primitive_audit_readonly', PACKAGE / 'evidence/audit_primitives.py')
    auditor = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(auditor)
    for case in cases:
        cid, top = case['case_id'], case['top']
        folder = destination / cid
        folder.mkdir()
        assert not case.get('parameters') and not case.get('include_dirs')
        source_commands = []
        for rel in case['sources']:
            source = PACKAGE / 'inputs' / rel
            assert sha(source) == case['sha256'][rel]
            source_commands.append('read_verilog -sv "' + ('inputs/' + rel.replace('\\', '/')) + '"')
        source_commands += [f'hierarchy -check -top {top}', 'proc']
        commands = [*source_commands, f'write_json "{folder.as_posix()}/before_init.json"', INIT,
                    'check -assert', f'write_rtlil "{folder.as_posix()}/initialized.il"',
                    f'write_json "{folder.as_posix()}/initialized.json"']
        run(exes['baseline'], folder / 'initialize.ys', folder / 'initialize.log', commands, env)
        before = load(folder / 'before_init.json')['modules'][top]
        after = load(folder / 'initialized.json')['modules'][top]
        old_init, new_init = init_bits(before), init_bits(after)
        assert all(new_init[b] == v for b, v in old_init.items())
        flops = {bit for cell in after['cells'].values() if cell['type'] in ('$dff', '$adff', '$dffe', '$adffe', '$aldff', '$sdff', '$sdffe')
                 for bit in cell['connections']['Q'] if isinstance(bit, int)}
        assert flops <= set(new_init), (cid, flops - set(new_init))
        assert all(new_init[b] == '0' for b in flops - set(old_init))
        row = {'case': cid, 'top': top, 'manifest': case, 'source_sha256': case['sha256'],
               'folder': folder.relative_to(ROOT).as_posix(),
               'initialized_il_sha256': sha(folder / 'initialized.il'),
               'initialized_json_sha256': sha(folder / 'initialized.json'),
               'logical_ff_bits_initialized': len(flops), 'explicit_old_init_bits_preserved': len(old_init),
               'arms': {}}
        record['cases'][cid] = row
        initialized_read = f'read_rtlil "{folder.as_posix()}/initialized.il"'
        # Both arms execute this exact monolithic script against identical RTLIL.
        full_commands = [*source_commands, INIT, f'synth_intel -family cycloneiv -top {top}', 'check -assert',
                         f'write_rtlil "{folder.as_posix()}/full.il"', f'write_json "{folder.as_posix()}/full.json"']
        # ScriptPass end label is exclusive. This point includes all map_luts work.
        split_commands = [*source_commands, INIT, f'synth_intel -family cycloneiv -top {top} -run begin:map_cells',
                          f'write_rtlil "{folder.as_posix()}/boundary.il"', f'write_json "{folder.as_posix()}/boundary.json"',
                          f'synth_intel -family cycloneiv -top {top} -run map_cells:', 'check -assert',
                          f'write_rtlil "{folder.as_posix()}/continued.il"', f'write_json "{folder.as_posix()}/continued.json"']
        for arm, exe in exes.items():
            run(exe, folder / 'full.ys', folder / (arm + '.full.log'), full_commands, env)
            for ext in ['il', 'json']:
                (folder / ('full.' + ext)).rename(folder / (arm + '.final.' + ext))
            run(exe, folder / 'split.ys', folder / (arm + '.split.log'), split_commands, env)
            for name in ['boundary', 'continued']:
                for ext in ['il', 'json']:
                    (folder / (name + '.' + ext)).rename(folder / (arm + '.' + name + '.' + ext))
            final = folder / (arm + '.final.json')
            boundary = folder / (arm + '.boundary.json')
            # Strip only Yosys semantic cells before checking native primitive modes.
            primitive = load(boundary)
            primitive['modules'][top]['cells'] = {n: c for n, c in primitive['modules'][top]['cells'].items()
                                                  if c['type'] not in ('$lut', '$_NOT_', '$print', '$scopeinfo')}
            temp = folder / (arm + '.boundary_native_audit.json')
            save(temp, primitive)
            boundary_audit = auditor.audit(temp, top)
            final_audit = auditor.audit(final, top)
            identity = graph(final) == graph(folder / (arm + '.continued.json'))
            assert identity, ('Split continuation changed graph', cid, arm)
            row['arms'][arm] = {'resources': mapped_resources(final, top),
                                'final_il_sha256': sha(folder / (arm + '.final.il')),
                                'final_json_sha256': sha(final),
                                'boundary_il_sha256': sha(folder / (arm + '.boundary.il')),
                                'boundary_json_sha256': sha(boundary),
                                'split_final_graph_equal_monolithic': identity,
                                'boundary_primitive_audit': boundary_audit,
                                'final_primitive_audit': final_audit}
            save(ROOT / ('CAPTURE_' + args.tag.upper() + '.json'), record)
            print(cid, arm, row['arms'][arm]['resources'], 'split=IDENTICAL', flush=True)
    record['complete'] = True
    save(ROOT / ('CAPTURE_' + args.tag.upper() + '.json'), record)


if __name__ == '__main__':
    main()
