#!/usr/bin/env python3
"""Rebuilds index.json from the .lpg files in build/ (produced by build.py).

Format of .lpg — tools/lpg_format.h in the LufiraOS repository (packed
structs, little-endian); the parsing here is inline so this repository
doesn't need LufiraOS-Builder as a library.

The "lpg" field in each entry is a real download URL served straight out of
this repository itself (no separate release server/VPS exists yet, and none
is needed for this): every built .lpg is copied into RELEASE_DIR (committed
to git, unlike the gitignored build/) and the URL points at its raw.
githubusercontent.com address on the default branch. dlpg's new "sync"/
"upgrade" subcommands (lufira-packages/base/dlpg.c) fetch this exact file
(index.json itself, via REMOTE_INDEX_URL there) over SYS_NET_FETCH and then
follow each package's "lpg" URL the same way — so this field must always be
a URL dlpg can actually GET, not a path meaningful only on this machine.
"""

import hashlib
import json
import shutil
import struct
from pathlib import Path

MAGIC = b"LPG1"
_HEADER_FMT = "<4s32sHHHBBII"
_HEADER_SIZE = struct.calcsize(_HEADER_FMT)
_DEP_FMT = "<32sHHH"
_DEP_SIZE = struct.calcsize(_DEP_FMT)

CATEGORY_NAMES = {0: "base", 1: "user"}

# Тот же репозиторий/ветка, что REMOTE_INDEX_URL в dlpg.c.
RELEASE_RAW_BASE_URL = (
    "https://raw.githubusercontent.com/VLPLAY-Games/lufira-packages/refs/heads/main/release"
)


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


def build_index(repo_root: Path, build_dir: Path, release_dir: Path) -> dict:
    release_dir.mkdir(parents=True, exist_ok=True)

    packages = []
    for lpg_path in sorted(build_dir.glob("*.lpg")):
        data = lpg_path.read_bytes()
        meta = parse_lpg_header(data)
        meta["size"] = len(data)
        meta["sha256"] = hashlib.sha256(data).hexdigest()

        # Копия В РЕПОЗИТОРИЙ (release_dir, НЕ гитигнорится — в отличие от
        # build_dir) — это и есть тот самый файл, который реально отдаёт
        # raw.githubusercontent.com по URL ниже. Простая перезапись байтами
        # (не git-операция) — коммитить release/ после сборки, как и
        # index.json, отдельный шаг, этот скрипт сам git не трогает.
        shutil.copyfile(lpg_path, release_dir / lpg_path.name)
        meta["lpg"] = f"{RELEASE_RAW_BASE_URL}/{lpg_path.name}"

        packages.append(meta)

    packages.sort(key=lambda p: (p["category"], p["name"]))
    return {"packages": packages}


def write_index(index: dict, out_path: Path) -> None:
    out_path.write_text(json.dumps(index, indent=2, ensure_ascii=False) + "\n")


def main() -> None:
    repo_root = Path(__file__).resolve().parent
    build_dir = repo_root / "build"
    release_dir = repo_root / "release"
    index = build_index(repo_root, build_dir, release_dir)
    out_path = repo_root / "index.json"
    write_index(index, out_path)
    print(f"wrote {out_path} ({len(index['packages'])} package(s))")


if __name__ == "__main__":
    main()
