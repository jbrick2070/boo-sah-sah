"""Fetch the pinned D3D12 Agility SDK preview and DXC preview used for tensor-core neural rendering.

Shader Model 6.10 LinAlg (thread-scope matrices, formerly Cooperative Vector) is a Microsoft preview:
  - Microsoft.Direct3D.D3D12 1.721.3-preview from nuget.org (D3D12Core.dll + headers, SDK version 721)
  - DirectX Shader Compiler v1.10.2605.24 from GitHub (dxc.exe + dx/linalg.h, compiles cs_6_10)
Both downloads are verified against pinned hashes before extraction. Configure CMake with
-DENABLE_LINALG_PREVIEW=ON afterwards. The preview runtime needs Windows Developer Mode and is
not allowed in Microsoft Store packages, so keep it to test-rig builds.

Usage: python tools/fetch_neural_sdk.py [build/dependencies]
"""
import argparse
import base64
import hashlib
import zipfile
from pathlib import Path
from urllib.request import urlopen

ROOT = Path(__file__).resolve().parents[1]
PACKAGES = [
    {
        "name": "d3d12_agility",
        "url": "https://api.nuget.org/v3-flatcontainer/microsoft.direct3d.d3d12/1.721.3-preview/"
               "microsoft.direct3d.d3d12.1.721.3-preview.nupkg",
        "sha512_b64": "gjDoWFJUy6ukTpxkBGDSvLxAJdGaVEG2xTDOju9Byw2GoM4Z+FNFEIzsKV39CNp5PRmofto8AtIaP+MUL9wGcQ==",
        "marker": "build/native/bin/x64/D3D12Core.dll",
    },
    {
        "name": "dxc_preview",
        "url": "https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.10.2605.24/"
               "dxc_preview_2026_05_22.zip",
        "sha256": "045e2cfd900135f640954553038febbc98692599c5606376726d00541dae69b6",
        "marker": "inc/hlsl/dx/linalg.h",
    },
]


def verified(pkg, data):
    if "sha512_b64" in pkg:
        return base64.b64encode(hashlib.sha512(data).digest()).decode() == pkg["sha512_b64"]
    return hashlib.sha256(data).hexdigest() == pkg["sha256"]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("directory", type=Path, nargs="?", default=ROOT / "build/dependencies")
    root = parser.parse_args().directory
    for pkg in PACKAGES:
        target = root / pkg["name"]
        if (target / pkg["marker"]).is_file():
            print(f"{pkg['name']}: already present")
            continue
        print(f"{pkg['name']}: downloading {pkg['url']}")
        with urlopen(pkg["url"], timeout=300) as response:
            data = response.read()
        if not verified(pkg, data):
            raise SystemExit(f"{pkg['name']}: hash mismatch, refusing to extract")
        archive = root / f"{pkg['name']}.download"
        root.mkdir(parents=True, exist_ok=True)
        archive.write_bytes(data)
        with zipfile.ZipFile(archive) as zf:
            for member in zf.namelist():
                resolved = (target / member).resolve()
                if not resolved.is_relative_to(target.resolve()):
                    raise SystemExit(f"{pkg['name']}: unsafe path in archive: {member}")
            zf.extractall(target)
        archive.unlink()
        print(f"{pkg['name']}: verified and extracted to {target}")


if __name__ == "__main__":
    main()
