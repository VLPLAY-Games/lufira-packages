# lufira-packages

![License](https://img.shields.io/badge/license-Apache--2.0-green)
![Status](https://img.shields.io/badge/status-alpha-orange)

**lufira-packages** holds [LufiraOS](https://github.com/VLPLAY-Games/LufiraOS)'s userspace: the `.lpg` package sources (shell, base commands, GUI window manager and apps), the package build system, and `index.json`, the package index served out of this repository's own [`release/`](release/) directory and consumed by `dlpg`.

Every package here is a normal freestanding ELF built against LufiraOS's minimal libc — nothing here lives in the kernel binary. This split exists so that drivers and core kernel subsystems stay in [LufiraOS](https://github.com/VLPLAY-Games/LufiraOS) while everything that can run in ring 3 (the shell, coreutils, the GUI window manager and its apps) is an ordinary installable package instead.

## What's here

- **`shell/`** — `shell.elf`, the real userspace shell (replaces the old kernel-native one): line editing, history, `cd`/`pwd`/`su`/`mount`/`unmount` builtins, `fork`+`exec`+`wait` for everything else.
- **`base/`** — coreutils and system commands (`cp`, `mv`, `ls`, `mkdir`, `rm`, `cat`, `touch`, `write`, `chmod`, `chown`, `kill`, `ps`, `fg`/`bg`, `color`/`reset`, `reboot`/`shutdown`, `devmode`, user/group management, USB block tools) plus **`dlpg`**, the package manager.
- **`user/`** — small utilities (`df`, `du`, `free`, `cpuload`) plus networking (`ifconfig`, `ping`, `wget`) and audio (`beep`, `music`, `mixer`).
- **`apps/`** — the GUI stack: `wm.elf` (the window manager — runs as an ordinary userspace process, not kernel code), plus windowed apps (`terminal`, `notepad`, `files`, `calc`, `sysinfo`, `settings`).
- **`games/doom/`** — a `doomgeneric` port (`doom.elf`), with sound, launched fullscreen from the desktop — its own package category (`games`), separate from `base`/`user`.
- **`build.py`** — builds every package against a sibling [LufiraOS](https://github.com/VLPLAY-Games/LufiraOS) checkout's libc, packs each into a `.lpg`, and regenerates `index.json` + `release/`.
- **`index.json`** / **`release/`** — the package index and the actual `.lpg` files it points at, both committed to this repository and served straight off `raw.githubusercontent.com` — no separate release server.

## Building

```bash
# Expects a sibling ../LufiraOS checkout by default (override with --lufira-repo).
python3 build.py
```

This compiles every package, packs it into `build/<name>.lpg`, copies each `.lpg` into `release/`, and regenerates `index.json` with real download URLs pointing back at this repository. [LufiraOS-Builder](https://github.com/VLPLAY-Games/LufiraOS-Builder) stages the resulting `.lpg` files onto a LufiraOS disk image at image-build time.

## The `.lpg` package format

A `.lpg` is a small packed binary format (`tools/lpg_format.h` in the LufiraOS repository): a header (name, semver, category, dependency count, minimum required kernel version), a dependency table (name + minimum version), a file table (install path, mode, offset), and the concatenated file contents. No archive library is used anywhere in this project — this is a deliberately simple, purpose-built format. `dlpg install`/`update` and `LufiraOS-Builder` both check the minimum-kernel-version field before installing/building, so a package built against a newer kernel feature can't silently end up on an older one.

## `dlpg` — the package manager

```
dlpg install <path.lpg>   # install a new package from a local .lpg (fails if already installed)
dlpg update  <path.lpg>   # (re)install from a local .lpg, even if already installed
dlpg list                  # list installed packages
dlpg remove  <name>        # uninstall a package and all its files
dlpg sync                  # download the current package index from this repository
dlpg upgrade [name]        # upgrade one (or every) installed package to the version in the synced index
```

`install`/`update`/`list`/`remove` work entirely offline against a local `.lpg` file and LufiraOS's own dependency-checked install/receipt/uninstall bookkeeping (`/etc/packages/`). Up to 64 packages can be installed at once (`MAX_INSTALLED` in `base/dlpg.c`).

`sync` and `upgrade` go over the network: `sync` fetches `index.json` straight from this repository (`https://raw.githubusercontent.com/VLPLAY-Games/lufira-packages/refs/heads/main/index.json`) and caches it locally; `upgrade` compares every installed package's version against that cached index and fetches+installs a newer `.lpg` from `release/` wherever one exists. Both ride on a single new syscall, `SYS_NET_FETCH` — LufiraOS resolves the hostname (DNS), opens the connection (TCP, or TLS 1.2 for `https://`), and hands `dlpg` the response body in one blocking call; `dlpg` itself never touches a socket directly. `upgrade` never installs a package that isn't already present — that's what `install`/`sync` are for.

## Project Structure

```
lufira-packages/
├── shell/            # shell.elf — the real userspace shell
├── base/             # coreutils + system commands + dlpg (package manager)
├── user/             # df/du/free/cpuload, ifconfig/ping/wget, beep/music/mixer
├── apps/              # wm.elf (window manager) + GUI apps (incl. settings)
├── games/doom/        # doomgeneric port (its own "games" package category)
├── common/            # shared headers (console escape codes, path helpers)
├── release/           # built .lpg files, committed — served via raw.githubusercontent.com
├── index.json         # package index, regenerated by build.py
├── build_index.py     # index.json + release/ generation, used by build.py
└── build.py           # build entry point
```

## License

This project is licensed under the Apache License 2.0. See the [LICENSE](LICENSE) file for details.
