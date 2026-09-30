# PocketGame · Handheld Console Firmware (Allwinner V85X / 480×800 portrait)

[中文](README.md) ｜ **English**

[![Hardware / Solution inquiry](https://img.shields.io/badge/Hardware%20%2F%20Solution-inquiry-0A7AFF?style=flat-square)](https://www.zkswe.com) [![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg?style=flat-square)](LICENSE) [![Built with FlyThings MCP](https://img.shields.io/badge/Built%20with-FlyThings%20MCP-0A7AFF?style=flat-square)](https://github.com/KWolve/FlyThingsMCP)

> **GitHub (primary)** <https://github.com/KWolve/PocketGame> ｜ **Gitee mirror** <https://gitee.com/Kwolve/PocketGame>

---

## In one line: 3 buttons and one touch screen — 34 apps inside

A **palm-sized 480×800 portrait handheld** (Allwinner **V85X** / FlyThings) running
**21 games + 10 tools + 3 system pages**, with a launcher split into *Games / Tools / System*.

Game console, internet radio, network TV, LAN camera viewer, smart-home remote, clock & alarm,
pomodoro timer, calculator, learnable Bluetooth/IR remote… **all in a single firmware image.
Playable out of the box, source fully open.**

![Launcher · Games](docs/screenshots/01-launcher-games.png)
![Match-3](docs/screenshots/18-game-match3.png)
![Internet radio](docs/screenshots/20-radio.png)
![Smart home](docs/screenshots/22-ha.png)

> More screenshots in [`docs/screenshots/`](docs/screenshots/) (15 images, all captured on real hardware).

---

## 1. Why buy it

| # | Selling point | What it means for you |
|---|---|---|
| 1 | **34 uses from one device** | One screen replaces a cabinet of gadgets: handheld console + radio + TV + camera monitor + smart-home remote + clock & alarm + timers. **One BOM, one package, one SKU of stock** |
| 2 | **Low hardware bar** | Only **3 physical buttons + a touch screen + a speaker** are needed to run all 34 apps — no button matrix, so both mechanical and tooling cost come down |
| 3 | **All art is code-generated** | Icons, cards, nine-patch assets, a **3D desktop pet rendered offline with PBR**, video-to-pixel-sprite conversions… **all produced by Python scripts**, re-generable in one command and diffable — **rebranding for a customer doesn't wait on a design agency** |
| 4 | **Font library slimmed on purpose** | The font is subset from a system font down to "the characters the UI actually uses" — **a 10 MB-class font becomes a few hundred KB**, saving Flash cost directly |
| 5 | **Actually playable, not a demo collage** | All 34 apps are verified on real hardware every release via a **touch-free automated QA channel** (push commands over a device file channel → capture screen → per-pixel compare), not "tap it once and eyeball it" |
| 6 | **Built to be extended** | Pure source + **79 measured-on-hardware docs** (including a framework pitfalls list) + the official **FlyThings MCP**: adding games, adding app pages and changing UI all have documented paths and check tools |
| 7 | **MIT licensed** | This project's code is MIT — modify, sell and redistribute without licensing talks (third-party components keep their own licenses, see below) |

---

## 2. The 34 apps

**34 apps = 21 games + 10 tools + 3 system pages**, grouped in the launcher as *Games / Tools / System*.

### Games (21)

| Category | Apps |
|---|---|
| Classics | 2048, Tetris, Shoot-'em-up, Flappy-style, Snake, Minesweeper, Sokoban, Breakout |
| Touch-first | Whack-a-mole, Memory match, Gomoku, Number slide puzzle, Reaction timer |
| Match / fun | Match-3 (8×8 with level goals), Dice roll (3 dice, 3D tumble) |
| Kids' learning | Number connect-the-dots, Arithmetic bubbles (mental math), Spot the difference |
| Instruments & puzzle | Sudoku, Rhythm piano (8 lanes, falling notes), Drum pad (6 pads, falling hits) |

### Tools (10)

Pomodoro · Timer · Stopwatch · Calculator · Learnable Bluetooth/IR remote ·
Clock suite (world clock + alarm) · Network TV (HLS/IPTV) · Signal probe (WiFi/BT scan + hotspot hunter) ·
Internet radio (61 stations measured + dual-channel VU meter) · LAN camera viewer

### System (3)

WiFi status & settings · System settings · **Smart-home remote (Home Assistant)**

### Extras

Desktop pet (offline PBR render) · Elf sprite (real footage turned into a canvas sprite) · Screensaver
(poems / pixel animation) · Global nav bar · Status bar · Volume OSD · **In-house Pinyin IME**
(9-key keypad + candidates)

---

## 3. Hardware / platform specs

| Item | Spec |
|---|---|
| SoC | Allwinner **V851s** (FlyThings platform id `V85X`) |
| Display | **480×800 portrait** (framebuffer 480×1600, double-buffered), capacitive touch |
| Buttons | **3 physical buttons** (`gpio-keys`) + touch screen |
| Audio | On-board speaker (on-chip codec `card0`); separate I2S external DAC path |
| Wireless | WiFi + Bluetooth combo (RTL8733BS; BT stack `btstack`: HID / A2DP / SPP) |
| Storage | `/res` read-only system partition; **TF card** external storage supported (media/casting output prefers external storage) |
| Battery | Li-ion: ADC-sampled fuel gauge, charging/full detection, low-battery LED, software power-off |
| Time | **No RTC on this board** — time comes from NTP over the network (without it, HTTPS cert checks fail) |
| Orientation | Full **180° live flip** of screen + touch layer (lanyard-mounted upside-down case; one-tap, persisted) |
| OS | **FlyThings / EasyUI** application framework |
| Update | ADB push / `update.img` flashing (TF card, auto-upgrade on card insert, remote OTA channel) |

---

## 4. Three steps to get running

### ① Fill in the dependencies (this repo intentionally ships no binaries)

```bash
python tools/check_deps.py        # lists exactly what's missing
```

Follow [`docs/DEPENDENCIES.md`](docs/DEPENDENCIES.md) to place the FlyThings toolchain and vendor SDK.

### ② Build / package

```bash
fun install                       # fetch the official packages declared in Manifest.xml
fun build                         # compile
fun pack -p V85X --release-version 1.0.0 -o out/update.img   # produce the firmware image
```

### ③ Flash the device

```bash
tools/upgrade_device.sh 1.0.0     # package + ADB flash + verify
ITER=1 tools/upgrade_device.sh    # fast iteration: push to a temp dir only, no flashing
```

> **Red line**: `/res` and `/dev/mtd*` on the device are read-only system partitions — never
> `dd` / `mount` / `flash_erase`. The only correct path is `tools/upgrade_device.sh`.
> The three real traps of flashing (multi-device `fun launch` always fails, Git Bash path
> conversion, and the device rebooting itself so partitions won't mount) are documented in
> [`docs/BUILD.md`](docs/BUILD.md).

---

## 5. Buy & contact

| Channel | Where |
|---|---|
| 🏢 Company | **Shenzhen ZKSWE Technology Co., Ltd.** |
| 🌐 Website / docs | <https://www.zkswe.com> ｜ <https://developer.flythings.cn/> |
| 📞 Phone | +86 755-23019045 |
| 📍 Address | Room 1407, Tower A, Fenghuang Zhigu, Gongle Community, Xixiang Street, Bao'an District, Shenzhen, Guangdong, China |

> **For hardware, solutions or customisation (rebrand, extra games, feature changes) please contact
> the company directly.** This repository covers software and documentation only.

---

## 6. Secondary development: you'll want FlyThings MCP

Development on this project (UI changes / new apps / build / debug / packaging / real-device screen
capture / knowledge-base search) is powered by **FlyThings MCP** — plug it into your AI client
(Trae / Cursor / Claude Desktop / Kimi…) and **one sentence drives the whole toolchain**.
Its **release build is maintained and published separately**; this repo only references it, never vendors it.

| Purpose | Path |
|---|---|
| **Release version (where users get it, primary)** | <https://github.com/KWolve/FlyThingsMCP> |
| China mirror (Gitee) | <https://gitee.com/Kwolve/flythingsmcp_release> |
| Local path (same workspace, for verification, optional) | `tools/FlyThings_mcp_release/` |

Install and integration steps follow that release repo's README (current release `0.27.134-open`, 43 tools).

**Fastest start**: tell your AI → "clone and install `https://github.com/KWolve/FlyThingsMCP`".

---

## 7. Engineering highlights

These are the parts that actually took the time — and the parts most worth reading when you build on it:

- **Software-rendered canvas** (`src/core/PgCanvas.*`) — all 21 games share one drawing layer that
  doesn't depend on UI controls. Pixels are drawn into an in-memory bitmap, attached to a control,
  and pushed to the screen through the **hardware path**.
- **Subset font + fixed size tiers** (`tools/gen_font.py` → `font/pocketgame.ttf`) — cuts the font
  down to "the characters the UI actually uses", shrinking a 10 MB-class font to a few hundred KB.
  Canvas text only pre-bakes a limited set of size tiers, **compile-time constants**, so runtime
  bitmap upscaling can't stretch it.
- **Code-generated art** (`tools/gen_icons.py` / `gen_game_art.py` / `ios_theme.py` / `gen_ha_tiles.py`) —
  every icon, card, tile and nine-patch rounded asset is script-produced: regenerable in one command, diffable.
- **Offline PBR pipeline** (`tools/pet3d/`) — headless Chrome + three.js renders the 3D character,
  then converts it frame-by-frame into an embedded-friendly texture sequence.
- **Real footage → canvas sprite** (`tools/gen_elf_from_video.py`) — per-frame measurement → keying →
  uniform scaling → fixed anchor.
- **Touch-free on-device QA** (QA channel + `tools/grab.py` + `tools/pginj` + `tools/audit_resources.py`) —
  commands are pushed over a device file channel, screens are captured, pixels are compared. Nothing
  in this project is verified by "tapping it once".
- **Full asset audit tooling** (`tools/audit_resources.py`) — corner behaviour, nine-patch markers,
  the 1:1 rule, unreferenced assets: a 7-dimension automated check (all 373 assets pass).

Plus a **pitfalls list**: this framework has a number of counter-intuitive behaviours (overlays swallow
touch, a control's parent in JSON is **always a container** rather than another control, nine-patch and
transparency follow two different rule sets). They're written up in
[`docs/框架缺陷与踩坑清单-2026-09-27.md`](docs/框架缺陷与踩坑清单-2026-09-27.md) and the
[`docs/kb-*.md`](docs/README.md) notes ([`docs/README.md`](docs/README.md) indexes all of them) —
worth a skim before you start.

---

## 8. Project layout

```
PocketGame/
├── src/
│   ├── core/           games & apps (Pg*.cpp), software canvas, font runtime
│   ├── logic/          per-page logic (<name>Logic.cc, bound to ui/<name>.ftu by name)
│   ├── platform/       platform capabilities: audio / BT / network / streaming / sensors / storage
│   ├── media/          WAV playback, etc.
│   ├── ui/             custom controls (ToolPage)
│   ├── activity/       main activity
│   └── dependencies/   ⚠️ vendor SDK & third-party libs, NOT shipped here (see its README)
├── ui/                 UI source of truth: *.html (design source) → *.json → *.ftu (loaded by device)
├── resources/          assets: images/ audio/ certs/ iptv/ media/
├── font/               project font subset (generated by tools/gen_font.py)
├── tools/              generators + on-device QA tools + asset audit
├── docs/               measured docs (incl. screenshots/)
├── Manifest.xml        dependency declaration (official FlyThings packages)
├── package.properties  platform config (resolution / touch device / screensaver timeout / font path)
└── CHANGELOG.md        per-version dev log (with measured data and rationale for each release)
```

**The three most common extension paths** (details in [`docs/DEVELOPMENT.md`](docs/DEVELOPMENT.md)):

| Goal | What you touch |
|---|---|
| **Add a canvas game** | Write a `pg::Game` subclass (`src/core/Pg*.cpp`) + **add one row** at the end of `kAppTable` in `PgGames.cpp` |
| **Add an app page** | New `ui/<name>.html` + `src/logic/<name>Logic.cc` + **sync 6 registration points** (listed in the docs; miss one and you get a silent bug) |
| **Change the look** | Edit `ui/*.html` → run `tools/gen_ui.py` → (if text changed) `tools/gen_font.py` |

---

## 9. What this repository deliberately excludes

This repo is **source + docs only**; it intentionally ships no binaries:

| Excluded | Why | How to get it |
|---|---|---|
| Allwinner V85X SDK headers & static libs | Vendor SDK; distribution governed by the vendor agreement | [`docs/DEPENDENCIES.md`](docs/DEPENDENCIES.md) §B1 |
| ffmpeg / OpenSSL and other third-party libs | Redistribution constrained by their own licenses | §B2 / §B3 in the same file |
| FlyThings toolchain (`fun` / `fui.exe`) | Proprietary tool (~38 MB) | Install the FlyThings IDE |
| Build outputs (`out/`, `.fun/`, `Release/`) | Reproducible from source | `fun build` |

**So: a fresh clone will not compile immediately** — fill in the dependencies above first
(`python tools/check_deps.py` tells you what's missing).

A few tool scripts carried the original author's machine paths (e.g. `D:\zkswe\...`); those are all
now overridable via environment variables: `PG_ADB`, `PG_HTML2JSON`, `PG_MCP_UI_TOOLS`, `PG_MCP_FONTS`,
`PG_SERIAL`, `ZKSWE_MCP_FONTS`.

---

## 10. Known limitations / prerequisites (stated up front)

- **No RTC**: time is not retained across power cycles, so network time sync is required; offline,
  clock/alarm apps depend on the last successful sync
- **A clone won't compile as-is**: vendor SDK / toolchain / third-party libs are not in the repo (see §9)
- **Some features have prerequisites**: network TV / radio / camera viewer / smart-home remote need WiFi;
  the Bluetooth remote needs the on-board BT module to be brought up
- **Measurements are board-specific**: the conclusions in `docs/` come from this project's target board
  (V851s + 480×800); **re-measure item by item when you change boards**

---

## 11. Documentation

| Start here | |
|---|---|
| [`docs/BUILD.md`](docs/BUILD.md) | Build / package / flash |
| [`docs/DEPENDENCIES.md`](docs/DEPENDENCIES.md) | Getting the dependencies |
| [`docs/DEVELOPMENT.md`](docs/DEVELOPMENT.md) | Secondary development guide |
| [`docs/README.md`](docs/README.md) | **Index of all 79 documents** |
| [`docs/screenshots/`](docs/screenshots/) | 15 real-device screenshots (launcher pages + each app) |
| [`CHANGELOG.md`](CHANGELOG.md) | Per-version dev log with measured data and conclusions |

---

## 12. License

This project's code is released under **MIT** — see [`LICENSE`](LICENSE).

Third-party components pulled in through the FlyThings package manager and the vendor SDK
(EasyUI framework, ffmpeg, OpenSSL, zlib, civetweb, btstack, etc.) are **not** part of this
repository; their use and redistribution follow their own licenses.

---

## 13. Credits

- UI framework: [FlyThings / EasyUI](https://www.flythings.cn/)
- Platform: Allwinner V85X
- Font base: Source Han Sans (SIL OFL)
- Offline 3D rendering: three.js + headless Chrome

---

_Shenzhen ZKSWE Technology Co., Ltd. · [www.zkswe.com](https://www.zkswe.com) · [developer.flythings.cn](https://developer.flythings.cn/)_
