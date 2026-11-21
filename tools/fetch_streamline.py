"""Install NVIDIA's signed Windows x64 production Frame Generation SDK inputs."""
import hashlib
from pathlib import Path
import shutil
from urllib.request import urlopen
from zipfile import ZipFile

version = 'v2.14.16'
sha256 = '32041e51db0cab709a271b72a352281b690c2abbd669a827809401a5dd23956b'
root = Path(__file__).resolve().parents[1] / 'build' / 'dependencies'
root.mkdir(parents=True, exist_ok=True)
filename = f'streamline-sdk-{version}.zip'
archive = root / filename
if not archive.exists():
    with urlopen(f'https://github.com/NVIDIA-RTX/Streamline/releases/download/{version}/{filename}') as source:
        with archive.with_suffix('.partial').open('wb') as destination:
            shutil.copyfileobj(source, destination)
    archive.with_suffix('.partial').replace(archive)
digest = hashlib.sha256()
with archive.open('rb') as source:
    for chunk in iter(lambda: source.read(1048576), b''):
        digest.update(chunk)
if digest.hexdigest() != sha256:
    raise SystemExit('NVIDIA Streamline archive failed its release SHA-256 check')
destination = root / 'nvidia_streamline'
with ZipFile(archive) as sdk:
    names = [n for n in sdk.namelist() if n.startswith('include/') and not n.endswith('/')]
    names += ['license.txt', '3rd-party-licenses.md']
    names += ['bin/x64/' + n for n in ('sl.interposer.dll', 'sl.common.dll', 'sl.dlss_g.dll', 'sl.reflex.dll',
              'sl.pcl.dll', 'nvngx_dlssg.dll', 'nvngx_dlss.license.txt', 'reflex.license.txt')]
    for name in names:
        sdk.extract(name, destination)
