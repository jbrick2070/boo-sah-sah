"""Refresh/check SHA-256 for every registered source asset, without normalizing its bytes."""
import argparse
import hashlib
import json
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--check', action='store_true')
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
path = root / 'assets/manifest.json'
manifest = json.loads(path.read_text(encoding='utf-8'))
changed = []
for asset in manifest['assets']:
    digest = hashlib.sha256((root / asset['path']).read_bytes()).hexdigest()
    if asset.get('sha256') != digest:
        changed.append(asset['id'])
        asset['sha256'] = digest
if args.check:
    if changed:
        raise SystemExit('Missing/stale SHA-256: ' + ', '.join(changed))
else:
    path.write_text(json.dumps(manifest, indent=2, ensure_ascii=False)+'\n', encoding='utf-8', newline='\n')
print(f"{'Checked' if args.check else 'Refreshed'} {len(manifest['assets'])} asset SHA-256 values.")
