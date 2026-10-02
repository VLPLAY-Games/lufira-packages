#!/usr/bin/env python3
"""lufira-packages — its own build, independent of LufiraOS-Builder.

Compiles every package source under base/ and user/ against LufiraOS's
userspace libc, packs each into a .lpg via lpg_pack (both tools/sources
pulled from a sibling LufiraOS checkout — same --lufira-repo pattern as
lufira-tests' build.py, used here purely as a source of libc/lpg_pack,
not because packages belong there), then regenerates index.json.

Nothing here is committed to git except the sources themselves and
index.json — build/ (the .elf/.lpg output) is gitignored. There's no
release server yet, so index.json's "lpg" field is a local path into
build/ for now; once a VPS exists, the same field becomes a real
download URL with no change to this script's output shape.
"""

import argparse
import shutil
import subprocess
from pathlib import Path

import build_index

# name -> (version, category, extra include dirs relative to --lufira-repo)
PACKAGES = {
    "cp": ("1.0.0", "base", []),
    "mv": ("1.0.0", "base", []),
    "ls": ("1.0.0", "base", []),
    "mkdir": ("1.0.0", "base", []),
    "rm": ("1.0.0", "base", []),
    "kill": ("1.0.0", "base", []),
    "ps": ("1.0.0", "base", []),
    "dlpg": ("1.0.0", "base", ["tools"]),  # lpg_format.h lives in LufiraOS/tools
    "du": ("1.0.1", "user", []),
    "df": ("1.0.0", "user", []),
    "free": ("1.0.0", "user", []),
    "cpuload": ("1.1.0", "user", []),
    # v0.7 plan, stage 5, continued ("as many commands out of the kernel
    # as possible") — ported from the dead kernel-native
    # kernel/shell/commands/*.c in LufiraOS.
    "cat": ("1.0.0", "base", []),
    "touch": ("1.0.0", "base", []),
    "write": ("1.0.0", "base", []),
    "chmod": ("1.0.0", "base", []),
    "chown": ("1.0.0", "base", []),
    "fg": ("1.0.0", "base", []),
    "bg": ("1.0.0", "base", []),
    "color": ("1.0.0", "base", []),
    "reset": ("1.0.0", "base", []),
    "reboot": ("1.0.0", "base", []),
    "shutdown": ("1.0.0", "base", []),
    "devmode": ("1.0.0", "base", []),
    # v0.7 plan, stage 5, continued — the rest of what was still only
    # reachable through the dead kernel-native kernel/shell/commands/*.c
    # (users.c/usb.c). mount/unmount stay shell builtins (shell.c), not
    # packages — see its own comment; mountls/mountcat/mountwrite from the
    # dead code are deliberately NOT ported, now obsolete: a mounted path
    # is just a normal path, so cd/ls/cat/cp/mkdir/rm/write already cover
    # them (once SYS_CHDIR also knows about FAT mounts — see syscall.c).
    "whoami": ("1.0.0", "base", []),
    "useradd": ("1.0.0", "base", []),
    "groupadd": ("1.0.0", "base", []),
    "passwd": ("1.0.0", "base", []),
    "usbinfo": ("1.0.0", "base", []),
    "usbread": ("1.0.0", "base", []),
    "usbwrite": ("1.0.0", "base", []),
}

LIBC_SOURCES = ["crt0.S", "src/string.c", "src/malloc.c", "src/printf.c", "src/stdlib.c"]

CC_FLAGS = [
    "-m64", "-ffreestanding", "-fno-builtin", "-fno-pic", "-fno-pie",
    "-mgeneral-regs-only", "-mno-red-zone", "-nostdlib", "-static",
    "-Wall", "-Wextra",
]


def _run(cmd, **kw) -> None:
    subprocess.run(cmd, check=True, **kw)


def compile_libc_objects(lufira_repo: Path, out_dir: Path) -> list:
    libc_dir = lufira_repo / "libc"
    flags = CC_FLAGS + [f"-I{libc_dir / 'include'}"]
    objs = []
    for rel in LIBC_SOURCES:
        src = libc_dir / rel
        obj = out_dir / (Path(rel).stem + ".o")
        _run(["gcc", *flags, "-c", str(src), "-o", str(obj)])
        objs.append(obj)
    return objs


def compile_lpg_pack(lufira_repo: Path, out_dir: Path) -> Path:
    lpg_pack_bin = out_dir / "lpg_pack"
    _run(["gcc", "-O2", "-Wall", "-Wextra", "-o", str(lpg_pack_bin),
          str(lufira_repo / "tools" / "lpg_pack.c")])
    return lpg_pack_bin


def build_package(repo_root: Path, lufira_repo: Path, out_dir: Path, libc_objs: list,
                   lpg_pack_bin: Path, name: str, version: str, category: str,
                   extra_includes: list) -> Path:
    src = repo_root / category / f"{name}.c"
    flags = CC_FLAGS + [f"-I{lufira_repo / 'libc' / 'include'}"]
    for inc in extra_includes:
        flags.append(f"-I{lufira_repo / inc}")

    obj = out_dir / f"{name}.o"
    _run(["gcc", *flags, "-c", str(src), "-o", str(obj)])

    elf = out_dir / f"{name}.elf"
    _run(["ld", "-m", "elf_x86_64", "-static", "-nostdlib", "-no-pie",
          "-o", str(elf), str(obj), *[str(o) for o in libc_objs]])

    manifest = (
        f"name={name}\n"
        f"version={version}\n"
        f"category={category}\n"
        "depends=\n"
        "[files]\n"
        f"{elf} /bin/{name}.elf 755\n"
    )
    manifest_path = out_dir / f"{name}.manifest"
    manifest_path.write_text(manifest)

    lpg_path = out_dir / f"{name}.lpg"
    _run([str(lpg_pack_bin), str(manifest_path), str(lpg_path)])
    return lpg_path


def build_shell(repo_root: Path, lufira_repo: Path, out_dir: Path, libc_objs: list) -> Path:
    """shell.elf — NOT a dlpg package (no manifest/.lpg): the kernel loads
    this exact file directly on every boot/respawn (spawn_shell_process(),
    LufiraOS/kernel/kernel.c), so dlpg must never see it in
    /etc/packages/installed or be able to "remove" it. Lives in its own
    shell/ folder here (not base/ or user/) for the same reason — it isn't
    a package, just source that happens to also need libc.
    """
    src = repo_root / "shell" / "shell.c"
    flags = CC_FLAGS + [f"-I{lufira_repo / 'libc' / 'include'}"]

    obj = out_dir / "shell.o"
    _run(["gcc", *flags, "-c", str(src), "-o", str(obj)])

    elf = out_dir / "shell.elf"
    _run(["ld", "-m", "elf_x86_64", "-static", "-nostdlib", "-no-pie",
          "-o", str(elf), str(obj), *[str(o) for o in libc_objs]])
    return elf


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lufira-repo", type=Path,
                         default=Path(__file__).resolve().parent.parent / "LufiraOS",
                         help="path to the LufiraOS repository, needed only for libc/lpg_pack "
                              "sources (default: sibling of this repository)")
    parser.add_argument("--out-dir", type=Path, default=Path("build"))
    args = parser.parse_args()

    repo_root = Path(__file__).resolve().parent
    out_dir = args.out_dir.resolve()
    if out_dir.exists():
        shutil.rmtree(out_dir)
    out_dir.mkdir(parents=True)

    print("=== Compiling libc objects ===")
    libc_objs = compile_libc_objects(args.lufira_repo, out_dir)

    print("=== Compiling lpg_pack ===")
    lpg_pack_bin = compile_lpg_pack(args.lufira_repo, out_dir)

    print(f"=== Building {len(PACKAGES)} package(s) ===")
    for name, (version, category, extra_includes) in sorted(PACKAGES.items()):
        lpg_path = build_package(repo_root, args.lufira_repo, out_dir, libc_objs,
                                  lpg_pack_bin, name, version, category, extra_includes)
        print(f"  built {lpg_path}")

    print("=== Building shell.elf (direct-stage, not a package) ===")
    shell_elf = build_shell(repo_root, args.lufira_repo, out_dir, libc_objs)
    print(f"  built {shell_elf}")

    print("=== Regenerating index.json ===")
    index = build_index.build_index(repo_root, out_dir)
    index_path = repo_root / "index.json"
    build_index.write_index(index, index_path)
    print(f"index written: {index_path} ({len(index['packages'])} package(s))")


if __name__ == "__main__":
    main()
