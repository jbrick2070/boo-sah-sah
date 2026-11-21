"""Check provenance and refresh SHA-256 digests of release assets and repository tools."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
from urllib.parse import urlsplit


ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "assets/manifest.json"


def validate(root, manifest, check):
    if manifest.get("schema") != 2:
        raise ValueError("Expected asset manifest schema 2")
    seen_ids = set()
    seen_paths = set()
    by_path = {}
    errors = []
    sections = [("assets", "assets"), ("source_tools", "tools")]
    if "source_audio" in manifest:
        sections.append(("source_audio", "assets"))
    for section, prefix in sections:
        if not isinstance(manifest.get(section), list) or not manifest[section]:
            raise ValueError(f"Expected entries in {section}")
        for asset in manifest[section]:
            if not isinstance(asset, dict) or not all(isinstance(asset.get(key), str) and asset[key].strip()
                    for key in ("id", "kind", "path", "provenance", "source_url", "author", "license")):
                raise ValueError(f"Missing provenance in {section}: {asset}")
            path = PurePosixPath(asset["path"])
            if (not path.parts or path.parts[0] != prefix or ".." in path.parts or path.as_posix() != asset["path"]
                    or any(ch in asset["path"] for ch in ":\\")):
                raise ValueError(f"Invalid {section} path: {path}")
            if section == "source_audio" and (path.parts[:2] != ("assets", "audio") or asset["kind"] != "audio"):
                raise ValueError(f"Invalid source audio entry: {path}")
            path_key = path.as_posix().casefold()
            if asset["id"] in seen_ids or path_key in seen_paths:
                raise ValueError(f"Duplicate id or path: {asset['id']}, {path}")
            seen_ids.add(asset["id"])
            seen_paths.add(path_key)
            by_path[path.as_posix()] = asset
            url = urlsplit(asset["source_url"])
            if url.scheme != "https" or not url.hostname or any(c.isspace() for c in asset["source_url"]):
                errors.append(f"{path}: expected an HTTPS source_url")
            if not isinstance(asset.get("attribution_required"), bool):
                errors.append(f"{path}: missing attribution_required flag")
            required_license = None
            if path.parts[:2] in (("assets", "audio"), ("assets", "cues")):
                required_license = "CC-BY-NC-4.0"
            elif path.suffix == ".hlsl" or section == "source_tools":
                required_license = ("MIT AND LicenseRef-NVIDIA-RTX-SDKs"
                                    if path.name == "m2_mesh.hlsl" else "MIT")
            if required_license and (asset["license"] != required_license or not asset["attribution_required"]):
                errors.append(f"{path}: expected {required_license} with its notice")
            with (root / path).open("rb") as stream:
                hasher = hashlib.sha256()
                for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                    hasher.update(chunk)
            actual = hasher.hexdigest()
            if check and asset.get("sha256") != actual:
                errors.append(f"{path}: missing/stale SHA-256")
            asset["sha256"] = actual

    # Check for unregistered files in the release asset families and source tools.
    families = {"assets/shaders/*.hlsl": "shader", "assets/cues/*.json": "cue_sheet",
                "assets/meshes/*.bmesh": "mesh", "assets/textures/*.btex": "texture",
                "assets/models/madame_n_*.xml": "model", "assets/models/madame_n_*.bin": "model",
                "assets/models/madame_n_*.json": "metadata", "assets/audio/*.mp3": "audio",
                "assets/audio/*.flac": "audio", "assets/audio/*.wav": "audio",
                "assets/neural/*.bin": "neural_weights",
                "assets/physics/*.bphys": "baked_physics",
                "tools/**/*.py": "tool", "tools/**/*.cpp": "tool"}
    for pattern, kind in families.items():
        for file in root.glob(pattern):
            relative = file.relative_to(root).as_posix()
            if relative not in by_path:
                errors.append(f"{relative}: unregistered file")
            elif by_path[relative]["kind"] != kind:
                errors.append(f"{relative}: expected kind '{kind}'")
    if errors:
        raise ValueError("\n".join(errors))
    return len(by_path)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="Report stale hashes without writing anything")
    args = parser.parse_args()
    try:
        manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
        count = validate(ROOT, manifest, args.check)
    except (OSError, ValueError) as exc:
        raise SystemExit(str(exc))
    if not args.check:
        MANIFEST.write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8", newline="\n")
    print(f"{'Checked' if args.check else 'Refreshed'} provenance and {count} SHA-256 digests.")
