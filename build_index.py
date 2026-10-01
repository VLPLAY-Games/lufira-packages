#!/usr/bin/env python3
"""Rebuilds index.json from the .lpg files in build/ (produced by build.py).

Format of .lpg — tools/lpg_format.h in the LufiraOS repository (packed
structs, little-endian); the parsing here is inline so this repository
doesn't need LufiraOS-Builder as a library.

The "lpg" field in each entry is a reference to the .lpg file itself.
There's no release server/VPS yet, so this is just a path into this
repository's own (gitignored) build/ directory for now; once a server
exists, this same field becomes a real download URL — no schema change
needed downstream.
"""

import hashlib
import json
import struct
from pathlib import Path

MAGIC = b"LPG1"
_HEADER_FMT = "<4s32sHHHBBII"
_HEADER_SIZE = struct.calcsize(_HEADER_FMT)
_DEP_FMT = "<32sHHH"
_DEP_SIZE = struct.calcsize(_DEP_FMT)

CATEGORY_NAMES = {0: "base", 1: "user"}


def _cstr(raw: bytes) -> str:
    return raw.split(b"\0", 1)[0].decode("ascii")


def parse_lpg_header(data: bytes):
    magic, name_raw, maj, minr, pat, category, _reserved, dep_count, _file_count = \
        struct.unpack_from(_HEADER_FMT, data, 0)
    if magic != MAGIC:
        raise ValueError("bad magic")

    deps = []
    off = _HEADER_SIZE
    for _ in range(dep_count):
        dname, dmaj, dminr, dpat = struct.unpack_from(_DEP_FMT, data, off)
        deps.append(_cstr(dname))
        off += _DEP_SIZE

    return {
        "name": _cstr(name_raw),
        "version": f"{maj}.{minr}.{pat}",
        "category": CATEGORY_NAMES.get(category, "unknown"),
        "dependencies": deps,
    }


def build_index(repo_root: Path, build_dir: Path) -> dict:
    packages = []
    for lpg_path in sorted(build_dir.glob("*.lpg")):
        data = lpg_path.read_bytes()
        meta = parse_lpg_header(data)
        meta["size"] = len(data)
        meta["sha256"] = hashlib.sha256(data).hexdigest()
        # Заглушка: до появления сервера релизов это путь внутри
        # самого репозитория (gitignored build/), не настоящий download-URL.
        meta["lpg"] = str(lpg_path.relative_to(repo_root))
        packages.append(meta)

    packages.sort(key=lambda p: (p["category"], p["name"]))
    return {"packages": packages}


def write_index(index: dict, out_path: Path) -> None:
    out_path.write_text(json.dumps(index, indent=2, ensure_ascii=False) + "\n")


def main() -> None:
    repo_root = Path(__file__).resolve().parent
    build_dir = repo_root / "build"
    index = build_index(repo_root, build_dir)
    out_path = repo_root / "index.json"
    write_index(index, out_path)
    print(f"wrote {out_path} ({len(index['packages'])} package(s))")


if __name__ == "__main__":
    main()
