#!/usr/bin/env python3
"""Fetch NVIDIA's PhysX SDK at a pinned commit and build NVIDIA Flow and the PhysX SDK from it.

Flow is the smoke and fire solver of the PhysX family. The app's PhysX suite (ENABLE_PHYSX) loads
the two libraries this builds, nvflow.dll and nvflowext.dll.

What it does, and what it deliberately does not:

* Clones https://github.com/NVIDIA-Omniverse/PhysX sparsely (only the flow/ and physx/ folders) into
  build/dependencies/physx-sdk and checks out the pinned commit. Git verifies the content.
* Runs the SDK's own package manager, packman, to fetch Flow's build tools from NVIDIA's package
  servers: Slang, DXC, premake, VsWhere and packman's own 7-Zip, about 70 MB in all.
  - The package cache stays inside build/dependencies (PM_PACKAGES_ROOT). Left to itself packman
    would create a packman-repo folder at the root of the drive and record it with `setx`.
  - It runs on the Python that runs this script (PM_PYTHON_EXT). Left to itself packman would
    download an interpreter over plain HTTP and run it unchecked.
* Generates Visual Studio projects with premake and builds the release configuration with MSBuild
  from the Visual Studio Build Tools already on the machine. Warnings are not treated as errors
  (the SDK predates the newest compiler's warnings); NVIDIA's source is not modified.

* Builds the PhysX SDK itself (physx/) with its CUDA GPU solvers, from a local preset that turns
  off the samples and compiles GPU code for fewer architectures (vc17win64boo, written into the
  checkout's buildtools/presets). This needs NVIDIA's CUDA 12.8 toolkit installed; without it,
  --cpu-only builds the CPU libraries alone.

Nothing here is redistributed with this repository: build/ is ignored by git.

Usage:
    python tools/fetch_physx_sdk.py            fetch and build
    python tools/fetch_physx_sdk.py --check    report what is present, change nothing
"""
from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path

REPO = "https://github.com/NVIDIA-Omniverse/PhysX.git"
COMMIT = "7d6f6ac8b83e1610bdf715f1f2f1d6b718aee586"  # tag ovphysx-0.8.0, PhysX SDK 5.13.0
ROOT = Path(__file__).resolve().parents[1]
DEPENDENCIES = ROOT / "build" / "dependencies"
SDK = DEPENDENCIES / "physx-sdk"
FLOW = SDK / "flow"
PACKMAN_CACHE = DEPENDENCIES / "packman-repo"
PHYSX = SDK / "physx"
CUDA = Path(os.environ.get("ProgramFiles", r"C:\Program Files")) / "NVIDIA GPU Computing Toolkit" / "CUDA" / "v12.8"
PRESET = "vc17win64boo"
OUTPUTS = [FLOW / "_build" / "windows-x86_64" / "release" / name for name in ("nvflow.dll", "nvflowext.dll")]
VS_CANDIDATES = [
    Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Microsoft Visual Studio" / "2022" / edition
    for edition in ("BuildTools", "Community", "Professional", "Enterprise")
] + [
    Path(os.environ.get("ProgramFiles", r"C:\Program Files")) / "Microsoft Visual Studio" / "2022" / edition
    for edition in ("BuildTools", "Community", "Professional", "Enterprise")
]


def run(command: list[str], cwd: Path, env: dict[str, str] | None = None) -> None:
    print(">", " ".join(str(part) for part in command), flush=True)
    subprocess.run(command, cwd=cwd, env=env, check=True)


def head_commit() -> str:
    if not (SDK / ".git").is_dir():
        return ""
    done = subprocess.run(["git", "rev-parse", "HEAD"], cwd=SDK, capture_output=True, text=True, check=False)
    return done.stdout.strip() if done.returncode == 0 else ""


def visual_studio() -> tuple[Path, Path]:
    """The MSVC toolset folder and the MSBuild folder of a Visual Studio 2022 install."""
    for root in VS_CANDIDATES:
        version_file = root / "VC" / "Auxiliary" / "Build" / "Microsoft.VCToolsVersion.default.txt"
        msbuild = root / "MSBuild" / "Current" / "Bin"
        if version_file.is_file() and (msbuild / "MSBuild.exe").is_file():
            return root / "VC" / "Tools" / "MSVC" / version_file.read_text().strip(), msbuild
    raise SystemExit("Visual Studio 2022 (Build Tools or an edition with the C++ workload) was not found.")


def check() -> int:
    commit = head_commit()
    print(f"checkout: {SDK} ({'pinned commit' if commit == COMMIT else commit or 'absent'})")
    for output in OUTPUTS:
        size = f"{output.stat().st_size / 1e6:.1f} MB" if output.is_file() else "absent"
        print(f"  {output.name}: {size}")
    return 0 if commit == COMMIT and all(output.is_file() for output in OUTPUTS) else 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true", help="report what is present and change nothing")
    parser.add_argument("--cpu-only", action="store_true", help="build the PhysX SDK without its CUDA GPU solvers")
    args = parser.parse_args()
    if args.check:
        return check()
    if sys.platform != "win32":
        raise SystemExit("This script builds the Windows libraries; run it on Windows.")

    DEPENDENCIES.mkdir(parents=True, exist_ok=True)
    if not (SDK / ".git").is_dir():
        run(["git", "clone", "--filter=blob:none", "--sparse", "--no-checkout", REPO, str(SDK)], DEPENDENCIES)
    run(["git", "sparse-checkout", "set", "flow", "physx"], SDK)
    if head_commit() != COMMIT:
        run(["git", "fetch", "--filter=blob:none", "origin", COMMIT], SDK)
        run(["git", "checkout", "--quiet", COMMIT], SDK)
    if head_commit() != COMMIT:
        raise SystemExit("The checkout is not at the pinned commit.")

    toolset, msbuild = visual_studio()
    env = dict(os.environ)
    env["PM_PACKAGES_ROOT"] = str(PACKMAN_CACHE)  # keep the cache here; no setx, no folder at the drive root
    env["PM_PYTHON_EXT"] = sys.executable         # no interpreter fetched over plain HTTP
    env["MSVS_TOOLSET"] = str(toolset)
    env["MSBUILD_DIR"] = str(msbuild)
    env["_CL_"] = "/WX-"                          # the newest compiler's new warnings are not errors
    run(["cmd", "/c", r"buildtools\packman\packman.cmd", "pull", "dependencies.xml", "-p", "windows-x86_64"], FLOW, env)
    run([str(FLOW / "external" / "premake" / "windows-x86_64" / "premake5.exe"), "--file=premake5.lua", "vs2022"], FLOW, env)
    run([str(msbuild / "MSBuild.exe"), r"_compiler\vs2022\nvflow.sln", "/p:Configuration=release", "/m", "/v:minimal", "/nologo"],
        FLOW, env)
    # The PhysX SDK: GPU solvers unless asked otherwise.
    gpu = not args.cpu_only
    if gpu and not (CUDA / "bin" / "nvcc.exe").is_file():
        raise SystemExit(f"CUDA 12.8 is not installed at {CUDA}; install it or pass --cpu-only.")
    base = (PHYSX / "buildtools" / "presets" / "public" / "vc17win64.xml").read_text(encoding="utf-8")
    preset = (base.replace('name="vc17win64"', f'name="{PRESET}"')
                  .replace('"PX_BUILDSNIPPETS" value="True"', '"PX_BUILDSNIPPETS" value="False"')
                  .replace('"PX_BUILDPVDRUNTIME" value="True"', '"PX_BUILDPVDRUNTIME" value="False"')
                  .replace('"PX_GENERATE_GPU_REDUCED_ARCHITECTURES" value="False"', '"PX_GENERATE_GPU_REDUCED_ARCHITECTURES" value="True"')
                  .replace('"PX_GENERATE_GPU_PROJECTS" value="True"', f'"PX_GENERATE_GPU_PROJECTS" value="{gpu}"')
                  .replace('"NV_USE_STATIC_WINCRT" value="False"', '"NV_USE_STATIC_WINCRT" value="True"')   # this project links the static C runtime
                  .replace('"NV_USE_DEBUG_WINCRT" value="True"', '"NV_USE_DEBUG_WINCRT" value="False"')
                  .replace("install/vc17win64/PhysX", f"install/{PRESET}/PhysX"))
    (PHYSX / "buildtools" / "presets" / f"{PRESET}.xml").write_text(preset, encoding="utf-8")
    if gpu:
        env["CUDA_PATH"] = env["CUDA_PATH_V12_8"] = str(CUDA)
        env["PATH"] = str(CUDA / "bin") + os.pathsep + env["PATH"]
    run(["cmd", "/c", str(PHYSX / "generate_projects.bat"), PRESET], PHYSX, env)
    run([str(msbuild / "MSBuild.exe"), str(PHYSX / "compiler" / PRESET / "PhysXSDK.sln"), "/p:Configuration=release", "/m",
         "/v:minimal", "/nologo"], PHYSX, env)

    missing = [output.name for output in OUTPUTS if not output.is_file()]
    if missing:
        raise SystemExit("Build finished without: " + ", ".join(missing))
    print("NVIDIA Flow is built. Configure with -DENABLE_PHYSX=ON to use it.")
    return check()


if __name__ == "__main__":
    sys.exit(main())
