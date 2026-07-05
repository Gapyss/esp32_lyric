#!/usr/bin/env python3
"""Package and optionally sign the browser extension.

Default output is a deterministic MV3 zip suitable for Chrome/Edge unpacked
testing or store upload. If --firefox-sign is passed, this script delegates to
Mozilla's web-ext signer and expects WEB_EXT_API_KEY and WEB_EXT_API_SECRET in
the environment.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
EXT_DIR = ROOT / "browser_extension"
DIST_DIR = ROOT / "dist"
INCLUDE = ("manifest.json", "content_script.js", "popup.html", "popup.js", "README.md")


def validate_manifest(ext_dir: Path) -> dict[str, object]:
    manifest_path = ext_dir / "manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    required = ["manifest_version", "name", "version", "content_scripts"]
    missing = [key for key in required if key not in manifest]
    if missing:
        raise ValueError(f"manifest missing required key(s): {', '.join(missing)}")
    if manifest.get("manifest_version") != 3:
        raise ValueError("manifest_version must be 3")
    for rel in INCLUDE:
        if rel != "README.md" and not (ext_dir / rel).is_file():
            raise ValueError(f"extension file missing: {rel}")
    return manifest


def package_zip(ext_dir: Path, out_dir: Path) -> Path:
    manifest = validate_manifest(ext_dir)
    version = str(manifest["version"])
    out_dir.mkdir(parents=True, exist_ok=True)
    zip_path = out_dir / f"g4pys-lyrics-display-bridge-{version}.zip"
    with zipfile.ZipFile(zip_path, "w", compression=zipfile.ZIP_DEFLATED) as zf:
        for rel in INCLUDE:
            path = ext_dir / rel
            if not path.is_file():
                continue
            info = zipfile.ZipInfo(rel)
            info.date_time = (2026, 1, 1, 0, 0, 0)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            zf.writestr(info, path.read_bytes())
    return zip_path


def firefox_sign(ext_dir: Path, out_dir: Path) -> None:
    web_ext = shutil.which("web-ext")
    if web_ext is None:
        raise RuntimeError("web-ext is not installed; install it with npm before signing")
    if not os.environ.get("WEB_EXT_API_KEY") or not os.environ.get("WEB_EXT_API_SECRET"):
        raise RuntimeError("WEB_EXT_API_KEY and WEB_EXT_API_SECRET are required for Firefox signing")
    out_dir.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        [
            web_ext,
            "sign",
            "--source-dir",
            str(ext_dir),
            "--artifacts-dir",
            str(out_dir),
            "--channel",
            "unlisted",
        ],
        check=True,
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--extension-dir", type=Path, default=EXT_DIR)
    parser.add_argument("--out-dir", type=Path, default=DIST_DIR)
    parser.add_argument("--firefox-sign", action="store_true")
    args = parser.parse_args()

    zip_path = package_zip(args.extension_dir, args.out_dir)
    print(zip_path)
    if args.firefox_sign:
        firefox_sign(args.extension_dir, args.out_dir)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(1)
