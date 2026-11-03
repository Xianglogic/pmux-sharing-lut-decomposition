"""Check maintained entry files for machine-specific absolute paths."""
import argparse
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
PATTERN = re.compile(r'(?<![\w])(?:[A-Za-z]:[/\\]+|/(?:home|Users)/|/mnt/[A-Za-z]/)')
TEXT = {'.py', '.ps1', '.cmd', '.ys', '.eqy', '.cc', '.h', '.md', '.json', '.psd1', '.toml', '.yml', '.yaml'}

def in_scope(name):
    path = Path(name)
    if path.suffix not in TEXT:
        return False
    if name.startswith(('scripts/', 'proof_templates/', 'source/overlay_files/', 'config/')):
        return True
    if path.parent.as_posix() == 'docs' and path.suffix == '.md':
        return True
    return name in {'README.md', 'REPORT.md'}

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--staged', action='store_true', help='Read only staged additions and modifications')
    args = parser.parse_args()
    command = ['git', 'diff', '--cached', '--name-only', '--diff-filter=ACM', '-z'] if args.staged else ['git', 'ls-files', '-z']
    names = subprocess.check_output(command, cwd=ROOT).decode('utf-8').split('\0')
    checked = 0
    failures = []
    for name in sorted(filter(None, names)):
        if not in_scope(name):
            continue
        if args.staged:
            data = subprocess.check_output(['git', 'show', ':' + name], cwd=ROOT)
        else:
            path = ROOT / name
            if not path.is_file():
                continue
            data = path.read_bytes()
        checked += 1
        for number, line in enumerate(data.decode('utf-8-sig').splitlines(), 1):
            if PATTERN.search(line):
                failures.append(f'{name}:{number}')
    for location in failures:
        print('Machine-specific absolute path: ' + location)
    print(f'Checked {checked} maintained files; path findings: {len(failures)}')
    raise SystemExit(bool(failures))

if __name__ == '__main__':
    main()
