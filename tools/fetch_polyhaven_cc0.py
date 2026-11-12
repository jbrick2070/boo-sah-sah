"""Fetch and record SHA-256 provenance for CC0-1.0 PolyHaven PBR materials, plants, and dawn HDRI."""
from pathlib import Path
import hashlib
import json
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
OUT_DIR = ROOT / "third_party" / "polyhaven_cc0"

TEXTURES = [
    "white_oak_veneer",
    "american_walnut_veneer",
    "plastered_wall_02",
    "clay_plaster",
    "long_white_tiles",
    "terrazzo_tiles",
    "marble_01",
    "rough_linen",
    "granite_tile_02",
    "concrete_pavers_02",
    "asphalt_02",
    "concrete_moss",
]

MODELS = [
    "calathea_orbifolia_01",
    "potted_plant_04",
    "fern_02",
]

HDRIS = [
    "qwantani_dawn_puresky",
]

HEADERS = {"User-Agent": "BooSahSah-AssetPipeline/1.0"}


def get_json(url: str) -> dict:
    req = urllib.request.Request(url, headers=HEADERS)
    with urllib.request.urlopen(req, timeout=20) as resp:
        return json.loads(resp.read().decode("utf-8"))


def download_file(url: str, dest: Path) -> str:
    dest.parent.mkdir(parents=True, exist_ok=True)
    if not dest.exists():
        req = urllib.request.Request(url, headers=HEADERS)
        with urllib.request.urlopen(req, timeout=30) as resp:
            dest.write_bytes(resp.read())
    return hashlib.sha256(dest.read_bytes()).hexdigest()


def pick_png_or_jpg(map_dict: dict) -> tuple[str, str]:
    res = map_dict.get("1k", {})
    for fmt in ("png", "jpg"):
        if fmt in res:
            return res[fmt]["url"], fmt
    raise KeyError(f"No 1k png/jpg in {list(res.keys())}")


def main() -> None:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    records = []

    for tid in TEXTURES:
        info = get_json(f"https://api.polyhaven.com/info/{tid}")
        files = get_json(f"https://api.polyhaven.com/files/{tid}")
        tdir = OUT_DIR / "textures" / tid
        downloaded = {}
        for channel in ("Diffuse", "nor_gl", "arm"):
            if channel in files:
                url, ext = pick_png_or_jpg(files[channel])
                dest = tdir / f"{channel.lower()}.{ext}"
                sha = download_file(url, dest)
                downloaded[channel.lower()] = {
                    "path": dest.relative_to(ROOT).as_posix(),
                    "url": url,
                    "sha256": sha,
                }
        records.append({
            "id": tid,
            "kind": "pbr_texture_set",
            "source": f"https://polyhaven.com/a/{tid}",
            "license": "CC0-1.0",
            "license_url": "https://creativecommons.org/publicdomain/zero/1.0/",
            "authors": sorted(info.get("authors", {}).keys()),
            "files": downloaded,
        })
        print(f"[OK] texture {tid} ({len(downloaded)} maps)")

    for mid in MODELS:
        info = get_json(f"https://api.polyhaven.com/info/{mid}")
        files = get_json(f"https://api.polyhaven.com/files/{mid}")
        mdir = OUT_DIR / "models" / mid
        gltf_entry = files["gltf"]["1k"]["gltf"]
        downloaded = {}
        main_dest = mdir / Path(gltf_entry["url"]).name
        downloaded["gltf"] = {
            "path": main_dest.relative_to(ROOT).as_posix(),
            "url": gltf_entry["url"],
            "sha256": download_file(gltf_entry["url"], main_dest),
        }
        for rel_path, inc_meta in gltf_entry.get("include", {}).items():
            inc_dest = mdir / rel_path
            sha = download_file(inc_meta["url"], inc_dest)
            downloaded[rel_path] = {
                "path": inc_dest.relative_to(ROOT).as_posix(),
                "url": inc_meta["url"],
                "sha256": sha,
            }
        if "Alpha" in files:
            a_url, a_ext = pick_png_or_jpg(files["Alpha"])
            a_dest = mdir / "textures" / f"{mid}_alpha_1k.{a_ext}"
            downloaded["alpha_1k"] = {
                "path": a_dest.relative_to(ROOT).as_posix(),
                "url": a_url,
                "sha256": download_file(a_url, a_dest),
            }
        records.append({
            "id": mid,
            "kind": "3d_model_gltf",
            "source": f"https://polyhaven.com/a/{mid}",
            "license": "CC0-1.0",
            "license_url": "https://creativecommons.org/publicdomain/zero/1.0/",
            "authors": sorted(info.get("authors", {}).keys()),
            "files": downloaded,
        })
        print(f"[OK] model   {mid} ({len(downloaded)} files)")

    for hid in HDRIS:
        info = get_json(f"https://api.polyhaven.com/info/{hid}")
        files = get_json(f"https://api.polyhaven.com/files/{hid}")
        exr_url = files["hdri"]["1k"]["exr"]["url"]
        dest = OUT_DIR / "hdris" / f"{hid}_1k.exr"
        sha = download_file(exr_url, dest)
        records.append({
            "id": hid,
            "kind": "hdri_environment",
            "source": f"https://polyhaven.com/a/{hid}",
            "license": "CC0-1.0",
            "license_url": "https://creativecommons.org/publicdomain/zero/1.0/",
            "authors": sorted(info.get("authors", {}).keys()),
            "files": {
                "exr_1k": {
                    "path": dest.relative_to(ROOT).as_posix(),
                    "url": exr_url,
                    "sha256": sha,
                }
            },
        })
        print(f"[OK] hdri    {hid}")

    prov_path = OUT_DIR / "provenance.json"
    prov_path.write_text(json.dumps({"schema": 1, "license": "CC0-1.0", "assets": records}, indent=2) + "\n", encoding="utf-8")
    print(f"Wrote {prov_path}")


if __name__ == "__main__":
    main()
