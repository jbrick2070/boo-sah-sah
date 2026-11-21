"""Fetch the x64 NGX Super Resolution SDK from the pinned official NVIDIA release."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
from pathlib import Path
from urllib.request import urlopen

REVISION = "9981c6350dc58c0ce21f024fd65e31e851c31689"  # v310.9.16
FILES = {
    "LICENSE.txt": "80756566d773cdac55179c5ae16c4495df7f73b5",
    "include/nvsdk_ngx.h": "ae6b49174049eed69cc377b2fffd3c901651f764",
    "include/nvsdk_ngx_defs.h": "807fa2475e90214f6846b9b4840be92e7bd66b5b",
    "include/nvsdk_ngx_params.h": "014d9f4adec600ea3677947b7f98258da9f9c7fc",
    "include/nvsdk_ngx_helpers.h": "e6a0f2581e1586240821afa7523a65a75654e847",
    "include/nvsdk_ngx_helpers_d3d.h": "664f99d95d3f8375304dc558ec601a66635ade7b",
    "include/nvsdk_ngx_helpers_cuda.h": "251ba9d5fcf0800bbc3fbb5e8870c61739c6d98a",
    "include/nvsdk_ngx_loader.h": "13756a449673327b08dfc9e6715937636de4ca48",
    "include/nvsdk_ngx_helpers_dlssd_d3d.h": "4e9c30f036be43bfe51c91e8a5d6a24bc218bfeb",
    "include/nvsdk_ngx_defs_dlssd.h": "8b73e3f347fab4f315443869ad57eeeb61168d7f",
    "include/nvsdk_ngx_params_dlssd.h": "6950ed305ba4d2eba1a2eab3c6463fec9ce10e6d",
    "lib/Windows_x86_64/x64/nvsdk_ngx_s.lib": "dc416cf7c8c863dd2b3f0edc5f701fcd85c3f4f9",
    "lib/Windows_x86_64/x64/nvsdk_ngx_s_dbg.lib": "50df95462ec0ce19f23181b81970d18e616d745e",
    "lib/Windows_x86_64/rel/nvngx_dlss.dll": "23c4672e4a31ef303e8f98ff4f0d146bd1718ccf",
    "lib/Windows_x86_64/rel/nvngx_dlssd.dll": "36efb7890a7f5754935647c6b3510e21a096512f",
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    root = parser.parse_args().directory

    def fetch(entry):
        name, expected = entry
        target = root / name
        def matches(data):
            return hashlib.sha1(f"blob {len(data)}\0".encode() + data).hexdigest() == expected
        if target.is_file() and matches(target.read_bytes()):
            return
        url = f"https://raw.githubusercontent.com/NVIDIA/DLSS/{REVISION}/{name}"
        with urlopen(url, timeout=120) as response:
            data = response.read()
        if not matches(data):
            raise ValueError(f"Official git object mismatch for {name}")
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)

    with ThreadPoolExecutor(max_workers=5) as pool:
        list(pool.map(fetch, FILES.items()))


if __name__ == "__main__":
    main()
