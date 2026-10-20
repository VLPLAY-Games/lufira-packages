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
import re
import shutil
import subprocess
from pathlib import Path

import build_index

_VERSION_H_RE = re.compile(
    r"#define\s+LUFIRA_VERSION_(MAJOR|MINOR|PATCH)\s+(\d+)"
)


def read_kernel_version(lufira_repo: Path) -> str:
    """Parses kernel/version.h (LufiraOS repo) with a plain regex — no need
    to compile anything just to learn the three numbers. This is the ONE
    place lufira-packages' build reads kernel compatibility from; every
    .lpg manifest below writes exactly this string as its "kernel_version="
    line (lpg_pack.c refuses to pack without it) — see the field's own
    comment in LufiraOS/tools/lpg_format.h for why it's a build-time fact
    stamped by this script, not a per-package choice."""
    text = (lufira_repo / "kernel" / "version.h").read_text()
    parts = dict(_VERSION_H_RE.findall(text))
    missing = [k for k in ("MAJOR", "MINOR", "PATCH") if k not in parts]
    if missing:
        raise RuntimeError(f"kernel/version.h: missing LUFIRA_VERSION_{missing[0]}")
    return f"{parts['MAJOR']}.{parts['MINOR']}.{parts['PATCH']}"


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
    "du": ("1.0.0", "user", []),
    "df": ("1.0.0", "user", []),
    "free": ("1.0.0", "user", []),
    "cpuload": ("1.0.0", "user", []),
    "beep": ("1.0.0", "user", []),
    "mixer": ("1.0.0", "user", []),
    "music": ("1.0.0", "user", []),
    "ping": ("1.0.0", "user", []),
    "ifconfig": ("1.0.0", "user", []),
    "wget": ("1.0.0", "user", []),
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
    "date": ("1.0.0", "base", []),
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
    "mount": ("1.0.0", "base", []),
    "umount": ("1.0.0", "base", []),
    # v0.8 (GUI+WM), этап 3: WM вынесен из ядра в обычный userspace-
    # процесс — обычный пакет, как и всё остальное здесь (в отличие от
    # shell.elf, у него нет кернел-стороннего авто-респауна/защиты от
    # удаления: пользователь запускает его вручную, напр. "runbg
    # /bin/wm.elf", тем же способом, что запускал бы X11/Wayland).
    "wm": ("1.0.0", "apps", []),
    "text_demo": ("1.0.0", "apps", []),
    # v0.8 (GUI+WM), Фаза 2: настоящие приложения — блокнот (файловый I/O)
    # и файловый менеджер (поверх gui_listbox_t).
    "notepad": ("1.0.0", "apps", []),
    "files": ("1.0.0", "apps", []),
    # v0.8 (GUI+WM), этап 4: desktop.c УДАЛЁН — выбор программ теперь прямо
    # на рабочем столе (ярлыки, рисует сам wm.c), не отдельное окно-
    # лаунчер. counter_demo УДАЛЁН (чисто учебная демка первого среза,
    # дальше не нужна). Новые приложения — калькулятор, системная
    # информация, настоящий терминал (поверх /bin/shell.elf).
    "calc": ("1.0.0", "apps", []),
    "sysinfo": ("1.0.0", "apps", []),
    "terminal": ("1.0.0", "apps", []),
    # v0.9: "Settings" — одно окно с вкладками (Audio/Programs/Autostart/
    # System) поверх SYS_SOUND_INFO/SYS_SOUND_SET_VOLUME (новые syscall'ы,
    # см. LufiraOS/kernel/system/syscall/syscall.h) и уже существующих
    # SYS_MEMINFO/SYS_STATFS/SYS_CPULOAD/SYS_PSLIST.
    "settings": ("1.0.0", "apps", []),
    # v0.9.5: звук вынесен из ядра в userspace-демон — см. разбор архитектуры
    # в daemons/soundd.c. Запускается самим кернелом при загрузке
    # (spawn_daemon_process(), kernel.c), не пользователем — но это обычный
    # .lpg-пакет, как и всё остальное здесь, не что-то отдельное от dlpg.
    "soundd": ("1.0.0", "daemons", []),
}

# v0.8-мост, пункт 8 (динамическая линковка): crt0.S остаётся статически
# слинкованным В КАЖДЫЙ исполняемый файл (как и положено crt-объекту —
# это точка входа _start, её не бывает "общей"), а вот string/malloc/
# printf/stdlib теперь живут ОДИН раз в libc.so (SHARED_LIBC_SOURCES ниже)
# вместо того чтобы статически копироваться в каждый пакет — см.
# compile_libc_shared() и изменённую сборку в build_package()/
# build_shell() (ld -dynamic-linker вместо -static).
CRT0_SOURCE = "crt0.S"
# src/gui_widgets.c — v0.8 (GUI+WM), второй срез: кнопка/текстбокс поверх
# SYS_WIN_* (см. lufira/gui_widgets.h) — та же единственная libc.so, не
# отдельная библиотека (dynlink.c сегодня рассчитан ровно на одну, см.
# его же комментарий у DT_NEEDED).
# src/math.c — добавлена вместе с либм для doomgeneric (и не только):
# ЕДИНСТВЕННЫЙ файл libc, который собирается без -mgeneral-regs-only (см.
# FPU_SOURCES/SHARED_LIBC_CC_FLAGS_FPU ниже) — double по x86-64 SysV ABI
# обязан ходить через XMM-регистры, что -mgeneral-regs-only запрещает в
# принципе. Безопасно: kernel/system/process/process.c уже сохраняет/
# восстанавливает FXSAVE-область на каждом переключении процесса — см.
# длинный комментарий в LufiraOS/libc/include/math.h.
SHARED_LIBC_SOURCES = ["src/string.c", "src/malloc.c", "src/printf.c", "src/stdlib.c",
                       "src/gui_widgets.c", "src/vsnprintf.c"]
FPU_SOURCES = ["src/math.c"]

CC_FLAGS = [
    "-m64", "-ffreestanding", "-fno-builtin", "-fno-pic", "-fno-pie",
    "-mgeneral-regs-only", "-mno-red-zone", "-nostdlib", "-static",
    "-Wall", "-Wextra",
]

# -fPIC вместо -fno-pic/-fno-pie выше: ТОЛЬКО libc.so обязана быть
# позиционно-независимой (ET_DYN, грузится ядром по произвольному
# фиксированному VA — см. DYNLINK_LIBC_BASE в LufiraOS/kernel/system/elf/
# dynlink.h) — сами пакеты остаются non-PIE (ET_EXEC, свой обычный
# фиксированный адрес 0x400000, БЕЗ изменений в их собственной загрузке).
SHARED_LIBC_CC_FLAGS = [
    "-m64", "-ffreestanding", "-fno-builtin", "-fPIC",
    "-mgeneral-regs-only", "-mno-red-zone", "-nostdlib",
    "-Wall", "-Wextra",
]

# Та же база, но БЕЗ -mgeneral-regs-only — только для FPU_SOURCES выше.
SHARED_LIBC_CC_FLAGS_FPU = [f for f in SHARED_LIBC_CC_FLAGS if f != "-mgeneral-regs-only"]


def _run(cmd, **kw) -> None:
    subprocess.run(cmd, check=True, **kw)


def compile_crt0(lufira_repo: Path, out_dir: Path) -> Path:
    libc_dir = lufira_repo / "libc"
    flags = CC_FLAGS + [f"-I{libc_dir / 'include'}"]
    obj = out_dir / "crt0.o"
    _run(["gcc", *flags, "-c", str(libc_dir / CRT0_SOURCE), "-o", str(obj)])
    return obj


def compile_libc_shared(lufira_repo: Path, out_dir: Path) -> Path:
    """libc.so — ОДИН общий разделяемый объект (string/malloc/printf/
    stdlib), который ядро грузит и кэширует один раз на всю систему (см.
    dynlink.c) — экономия памяти настоящей динамической линковки.
    --hash-style=sysv ОБЯЗАТЕЛЕН: dynlink.c узнаёт число экспортных
    символов из nchain классического DT_HASH, GNU_HASH не читает вовсе
    (линейного перебора десятков символов достаточно — не горячий путь).
    """
    libc_dir = lufira_repo / "libc"
    flags = SHARED_LIBC_CC_FLAGS + [f"-I{libc_dir / 'include'}"]
    flags_fpu = SHARED_LIBC_CC_FLAGS_FPU + [f"-I{libc_dir / 'include'}"]
    objs = []
    for rel in SHARED_LIBC_SOURCES + FPU_SOURCES:
        src = libc_dir / rel
        obj = out_dir / (Path(rel).stem + ".pic.o")
        _run(["gcc", *(flags_fpu if rel in FPU_SOURCES else flags), "-c", str(src), "-o", str(obj)])
        objs.append(obj)

    libc_so = out_dir / "libc.so"
    _run(["ld", "-m", "elf_x86_64", "-shared", "-soname", "libc.so",
          "--hash-style=sysv", "-o", str(libc_so), *[str(o) for o in objs]])
    return libc_so


def compile_lpg_pack(lufira_repo: Path, out_dir: Path) -> Path:
    lpg_pack_bin = out_dir / "lpg_pack"
    _run(["gcc", "-O2", "-Wall", "-Wextra", "-o", str(lpg_pack_bin),
          str(lufira_repo / "tools" / "lpg_pack.c")])
    return lpg_pack_bin


def build_package(repo_root: Path, lufira_repo: Path, out_dir: Path, crt0_obj: Path,
                   lpg_pack_bin: Path, name: str, version: str, category: str,
                   extra_includes: list, kernel_version: str) -> Path:
    src = repo_root / category / f"{name}.c"
    flags = CC_FLAGS + [f"-I{lufira_repo / 'libc' / 'include'}"]
    for inc in extra_includes:
        flags.append(f"-I{lufira_repo / inc}")

    obj = out_dir / f"{name}.o"
    _run(["gcc", *flags, "-c", str(src), "-o", str(obj)])

    # v0.8-мост, пункт 8: -static убран (конфликтует с динамической
    # линковкой — ld иначе не ищет .so вовсе, см. комментарий у
    # compile_libc_shared()), crt0.o остаётся статическим, string/malloc/
    # printf/stdlib теперь резолвятся через -lc в libc.so (-L указывает на
    # ту же out_dir, где лежит сам libc.so). -dynamic-linker — чисто
    # информационный путь в PT_INTERP (само ядро его не читает, резолвит
    # сразу в exec(), см. dynlink.c), но настоящий путь на диске этой ОС
    # уместнее мусорного дефолтного /lib64/ld-linux-x86-64.so.2 от хоста.
    elf = out_dir / f"{name}.elf"
    _run(["ld", "-m", "elf_x86_64", "-nostdlib", "-no-pie",
          "-dynamic-linker", "/lib/ld.so",
          "-o", str(elf), str(crt0_obj), str(obj), "-L", str(out_dir), "-lc"])

    manifest = (
        f"name={name}\n"
        f"version={version}\n"
        f"category={category}\n"
        "depends=\n"
        f"kernel_version={kernel_version}\n"
        "[files]\n"
        f"{elf} /bin/{name}.elf 755\n"
    )
    manifest_path = out_dir / f"{name}.manifest"
    manifest_path.write_text(manifest)

    lpg_path = out_dir / f"{name}.lpg"
    _run([str(lpg_pack_bin), str(manifest_path), str(lpg_path)])
    return lpg_path


def build_shell(repo_root: Path, lufira_repo: Path, out_dir: Path, crt0_obj: Path) -> Path:
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

    # v0.8-мост, пункт 8 — тот же переход на динамическую линковку, что и
    # build_package() выше (см. его комментарий).
    elf = out_dir / "shell.elf"
    _run(["ld", "-m", "elf_x86_64", "-nostdlib", "-no-pie",
          "-dynamic-linker", "/lib/ld.so",
          "-o", str(elf), str(crt0_obj), str(obj), "-L", str(out_dir), "-lc"])
    return elf


# games/doom/ — порт doomgeneric (https://github.com/ozkl/doomgeneric),
# ~80 исходников движка (games/doom/src/, копия апстрима без изменений —
# все нужные платформенные отличия уже скрыты за его же собственным
# config.h, см. комментарии в games/doom/compat/) + свой compat-слой
# (games/doom/compat/ — FILE*/getenv/strdup/atoi/atof/sscanf, которых нет
# в LufiraOS libc, см. его же "Никаких файловых потоков" в stdio.h) +
# платформенный файл (games/doom/platform/doomgeneric_lufira.c — все 6
# DG_*-функций, см. его шапку). Не влезает в build_package() (тот
# рассчитан РОВНО на один .c на пакет) — отдельная функция, собирающая
# все файлы руками и паковающая всё тем же lpg_pack, что и остальные
# пакеты; "games" как category у manifest — то же самое, что "apps" у
# settings.lpg, просто для группировки по смыслу (в lpg-формате ЛЮБАЯ
# строка кроме "base" трактуется как LPG_CATEGORY_USER, см. lpg_pack.c).
#
# Единственные файлы, которым НУЖНА настоящая арифметика с float/double
# (compat.c: atof(); остальные — живые, не мёртвый код под #if 0, куски
# апстрима: g_game.c — счётчик FPS в статистике демо, i_video.c/v_video.c —
# ускорение мыши) — собираются БЕЗ -mgeneral-regs-only, тем же приёмом,
# что уже применяется к libc/src/math.c в compile_libc_shared() выше:
# -mgeneral-regs-only — это чисто кодогенерационное ограничение одной
# единицы трансляции, оно не меняет ABI вызова между файлами, так что
# смешивать их в одном бинарнике безопасно (здесь так же, как уже
# работает для libc.so).
DOOM_FPU_FILES = {"g_game.c", "i_video.c", "m_config.c", "v_video.c", "compat.c"}

# doom не в PACKAGES выше (свой build_doom_package(), не общий build_package()) —
# но LufiraOS-Builder's gui.py._load_package_catalog_local() всё равно должно
# видеть его версию/категорию для чекбокса в разделе "games", поэтому они —
# именованные константы, а не литералы прямо в манифесте ниже.
DOOM_PACKAGE_VERSION = "1.0.0"
DOOM_PACKAGE_CATEGORY = "games"


def build_doom_package(repo_root: Path, lufira_repo: Path, out_dir: Path,
                        crt0_obj: Path, lpg_pack_bin: Path, kernel_version: str) -> Path:
    doom_dir = repo_root / "games" / "doom"
    src_dir = doom_dir / "src"
    compat_dir = doom_dir / "compat"
    platform_dir = doom_dir / "platform"
    wad_path = doom_dir / "assets" / "doom1.wad"

    # compat_dir ПЕРВЫМ: его заголовки (stdio.h/stdlib.h/string.h и т.д.)
    # сами делают #include_next <...> — это должно найти НАСТОЯЩИЙ
    # одноимённый заголовок LufiraOS libc, который обязан идти в -I СТРОГО
    # после compat_dir, не до.
    include_flags = [
        f"-I{compat_dir}",
        f"-I{src_dir}",
        f"-I{platform_dir}",
        f"-I{lufira_repo / 'libc' / 'include'}",
    ]
    base_flags = [
        "-m64", "-ffreestanding", "-fno-builtin", "-fno-pic", "-fno-pie",
        "-mno-red-zone", "-nostdlib", "-static", "-Wall", "-Wextra",
        # == SCREENWIDTH/SCREENHEIGHT (320x200) — родной fb_scaling
        # движка тогда всегда 1 (без его собственного масштабирования);
        # настоящее увеличение под реальное разрешение экрана (заранее не
        # известное) делает сам doomgeneric_lufira.c во время кадра.
        "-DDOOMGENERIC_RESX=320", "-DDOOMGENERIC_RESY=200",
        # Включает dispatch на DG_sound_module/DG_music_module в i_sound.c
        # (вендорный файл, не трогаем) — сами структуры реализованы в
        # platform/doomgeneric_sound_lufira.c, НЕ в апстриме.
        "-DFEATURE_SOUND",
    ] + include_flags
    fpu_flags = base_flags
    nofpu_flags = base_flags + ["-mgeneral-regs-only"]

    obj_dir = out_dir / "doom_obj"
    obj_dir.mkdir(parents=True, exist_ok=True)

    sources = (sorted(src_dir.glob("*.c")) +
               [platform_dir / "doomgeneric_lufira.c",
                platform_dir / "doomgeneric_sound_lufira.c",
                compat_dir / "compat.c"])
    objs = []
    for src in sources:
        flags = fpu_flags if src.name in DOOM_FPU_FILES else nofpu_flags
        obj = obj_dir / (src.stem + ".o")
        _run(["gcc", *flags, "-c", str(src), "-o", str(obj)])
        objs.append(obj)

    elf = out_dir / "doom.elf"
    _run(["ld", "-m", "elf_x86_64", "-nostdlib", "-no-pie",
          "-dynamic-linker", "/lib/ld.so",
          "-o", str(elf), str(crt0_obj), *[str(o) for o in objs],
          "-L", str(out_dir), "-lc"])

    manifest = (
        "name=doom\n"
        f"version={DOOM_PACKAGE_VERSION}\n"
        f"category={DOOM_PACKAGE_CATEGORY}\n"
        "depends=\n"
        f"kernel_version={kernel_version}\n"
        "[files]\n"
        f"{elf} /bin/doom.elf 755\n"
        f"{wad_path} /usr/share/doom1.wad 644\n"
    )
    manifest_path = out_dir / "doom.manifest"
    manifest_path.write_text(manifest)

    lpg_path = out_dir / "doom.lpg"
    _run([str(lpg_pack_bin), str(manifest_path), str(lpg_path)])
    return lpg_path


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

    print("=== Compiling crt0.o ===")
    crt0_obj = compile_crt0(args.lufira_repo, out_dir)

    print("=== Compiling libc.so (shared) ===")
    libc_so = compile_libc_shared(args.lufira_repo, out_dir)
    print(f"  built {libc_so}")

    print("=== Compiling lpg_pack ===")
    lpg_pack_bin = compile_lpg_pack(args.lufira_repo, out_dir)

    kernel_version = read_kernel_version(args.lufira_repo)
    print(f"=== Target kernel version: {kernel_version} (kernel/version.h) ===")

    print(f"=== Building {len(PACKAGES)} package(s) ===")
    for name, (version, category, extra_includes) in sorted(PACKAGES.items()):
        lpg_path = build_package(repo_root, args.lufira_repo, out_dir, crt0_obj,
                                  lpg_pack_bin, name, version, category, extra_includes,
                                  kernel_version)
        print(f"  built {lpg_path}")

    print("=== Building shell.elf (direct-stage, not a package) ===")
    shell_elf = build_shell(repo_root, args.lufira_repo, out_dir, crt0_obj)
    print(f"  built {shell_elf}")

    print("=== Building doom.lpg (doomgeneric port) ===")
    doom_lpg = build_doom_package(repo_root, args.lufira_repo, out_dir, crt0_obj, lpg_pack_bin,
                                   kernel_version)
    print(f"  built {doom_lpg}")

    print("=== Regenerating index.json + release/ ===")
    release_dir = repo_root / "release"
    index = build_index.build_index(repo_root, out_dir, release_dir)
    index_path = repo_root / "index.json"
    build_index.write_index(index, index_path)
    print(f"index written: {index_path} ({len(index['packages'])} package(s))")
    print(f"release copies written to: {release_dir}")


if __name__ == "__main__":
    main()
