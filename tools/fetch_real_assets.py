#!/usr/bin/env python3
"""
Fetch genuine, real-world (non-AI) Tsingtao Beer imagery into assets/raw/.

Sources (Wikipedia / Wikimedia Commons):
  - assets/raw/tsingtao_logo.png   : File:Tsingtao_Beer_logo_3.png (en.wikipedia.org)
  - assets/raw/tsingtao_can.png    : File:Tsingtao_(Tsingtao_Brewery).jpg (commons.wikimedia.org)
  - assets/raw/tsingtao_bottle.png : File:TsingtaoBeer.jpg (commons.wikimedia.org)

If any target file already exists in assets/raw/, it is preserved untouched so
users can swap in custom photos anytime. Pass --force to re-download.
"""

import argparse
import io
import os
import sys
import urllib.request
from pathlib import Path
from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
RAW_DIR = ROOT / "assets" / "raw"

USER_AGENT = "TsingtaoExpress96AssetFetcher/1.0 (PS1 homebrew; non-commercial)"

SOURCES = {
    "tsingtao_logo.png": {
        "url": "https://upload.wikimedia.org/wikipedia/en/a/a4/Tsingtao_Beer_logo_3.png",
        "desc": "Genuine Tsingtao Brewery logo (en.wikipedia.org File:Tsingtao_Beer_logo_3.png)",
    },
    "tsingtao_can.png": {
        "url": "https://upload.wikimedia.org/wikipedia/commons/thumb/d/db/Tsingtao_%28Tsingtao_Brewery%29.jpg/500px-Tsingtao_%28Tsingtao_Brewery%29.jpg",
        "desc": "Genuine green Tsingtao 0.33L can photo (commons.wikimedia.org File:Tsingtao_(Tsingtao_Brewery).jpg)",
    },
    "tsingtao_bottle.png": {
        "url": "https://upload.wikimedia.org/wikipedia/commons/thumb/4/49/TsingtaoBeer.jpg/500px-TsingtaoBeer.jpg",
        "desc": "Genuine green Tsingtao bottle photo (commons.wikimedia.org File:TsingtaoBeer.jpg)",
    },
}


def fetch_image(url: str) -> Image.Image:
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(req, timeout=30) as resp:
        data = resp.read()
    im = Image.open(io.BytesIO(data))
    im.load()
    return im


def main() -> int:
    parser = argparse.ArgumentParser(description="Fetch genuine Tsingtao reference assets.")
    parser.add_argument("--force", action="store_true", help="Re-download even if file already exists.")
    args = parser.parse_args()

    RAW_DIR.mkdir(parents=True, exist_ok=True)

    for filename, info in SOURCES.items():
        dst = RAW_DIR / filename
        if dst.exists() and not args.force:
            try:
                with Image.open(dst) as existing:
                    existing.verify()
                with Image.open(dst) as existing:
                    print(f"[skip] {dst.relative_to(ROOT)} already exists ({existing.size[0]}x{existing.size[1]} {existing.mode})")
                continue
            except Exception as exc:
                print(f"[warn] Existing {dst} is invalid ({exc}); re-downloading...")

        print(f"[fetch] {info['desc']}")
        im = fetch_image(info["url"])
        # Preserve alpha if present, otherwise store clean RGB PNG
        if im.mode not in ("RGB", "RGBA"):
            im = im.convert("RGBA" if "A" in im.getbands() else "RGB")
        im.save(dst, format="PNG")
        print(f"[ok]    Saved {dst.relative_to(ROOT)} ({im.size[0]}x{im.size[1]} {im.mode}, {dst.stat().st_size} bytes)")

    return 0


if __name__ == "__main__":
    sys.exit(main())
