from pathlib import Path
import argparse
import hashlib
import json

ROOT = Path(__file__).resolve().parent
ap = argparse.ArgumentParser()
ap.add_argument('--tag', required=True, choices=['pilot', 'full'])
args = ap.parse_args()
capture = json.loads((ROOT / ('CAPTURE_' + args.tag.upper() + '.json')).read_text(encoding='utf-8'))
assert capture['complete']
rows = []
for case in capture['cases'].values():
    for original in sorted((ROOT / case['folder']).glob('*.il')):
        if original.name.endswith('.formal.il'):
            continue
        raw = original.read_bytes()
        lf = raw.replace(b'\r\n', b'\n')
        destination = original.with_name(original.stem + '.formal.il')
        assert not destination.exists()
        destination.write_bytes(lf)
        rows.append({'native_file': original.relative_to(ROOT).as_posix(),
                     'formal_file': destination.relative_to(ROOT).as_posix(),
                     'native_sha256': hashlib.sha256(raw).hexdigest(),
                     'formal_sha256': hashlib.sha256(lf).hexdigest(),
                     'crlf_pairs_replaced': raw.count(b'\r\n'),
                     'only_crlf_to_lf': True})
output = ROOT / ('NORMALIZED_INPUTS_' + args.tag.upper() + '.json')
assert not output.exists()
output.write_text(json.dumps({'files': rows, 'no_original_file_overwritten': True}, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
print('Prepared', len(rows), 'LF-only RTLIL copies; native originals preserved.', flush=True)
