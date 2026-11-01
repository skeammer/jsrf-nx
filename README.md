# Jet Set Radio Future → Nintendo Switch

A static recompilation of the original Xbox release of **Jet Set Radio Future**
(NTSC-U) as a Nintendo Switch homebrew app. The game's x86 code is lifted to C
and linked against models of the Xbox kernel, the NV2A GPU, the MCPX audio
chip and USB, running on libnx. The NV2A is drawn with **deko3d**, the
console's own GPU interface. It is not an emulator.

**This repository contains no game code or data.** The lifted C is generated
on your machine from your own copy of the game's `default.xbe`, and the game's
files come from your own disc. You need to own the game.

Not affiliated with or endorsed by SEGA or Smilebit.

## Status

Playable on hardware (handheld, stock clocks): menus, character select and the
streets run at or near 60 fps. The engineering log,
[`jsrf-nx-switch-findings.md`](jsrf-nx-switch-findings.md), has every fix with
its evidence, the runtime switches, the hardware runs and the open issues.

## What you need

- Your own dump of the NTSC-U disc (`default.xbe`, `Media/`, ...), either
  extracted or as an `.iso`.
- [jsrf-recomp](https://github.com/andeecollard/jsrf-recomp) at commit
  `4242eb6` (the generator), with Python 3, `capstone` and `pillow`.
- Docker or Podman with `docker.io/devkitpro/devkita64:20260219` (devkitA64,
  libnx, deko3d, SDL2).
- A Switch running homebrew (Atmosphère), launched with full memory (a title
  override: hold R while starting a game).

## Generating the lifted code

The generated sources go in `jsrf-switch/src/recomp/gen/` (see the README
there). From a jsrf-recomp checkout at `4242eb6`:

1. Apply `regen/jsrf-recomp-local.patch`.
2. Run `diagnostics/jsrf_first_fault/regenerate.sh` with your `default.xbe`
   and copy the output into `jsrf-switch/src/recomp/gen/`.
3. Run the passes over it, in this order (each script's header documents its
   arguments and its report-only mode):
   `regen/jsrf_spin_hint.py`, `regen/jsrf_preempt.py`,
   `regen/jsrf_reg_locals.py`, `regen/jsrf_inline_leaves.py`,
   `regen/jsrf_copy_loops.py`, `regen/jsrf_fp_locals.py`,
   `regen/jsrf_simd_locals.py`.
4. Lift the functions the generator misses:
   `python3 regen/jsrf_lift_missed.py --recomp <jsrf-recomp> --xbe <default.xbe> jsrf-switch/src/recomp/gen`
   (it checks your XBE is the US release and that the output matches what was
   tested).
5. Apply the hand edits listed under "Hand edits to the generated code" in
   the findings doc (a few D3D and XAPI functions are renamed `*_gen` and
   wrapped in `jsrf-switch/src/jsrf_manual_overrides.c`).

## Building

From the repository root:

    docker run --rm -e JOBS=4 -v "$PWD":/workspace -w /workspace/jsrf-switch \
      docker.io/devkitpro/devkita64:20260219 bash switch/build.sh

This writes `switch_sd/switch/jsrf/jsrf.nro`. If your extracted disc is in
`game_files/` next to `jsrf-switch/`, it is staged into
`switch_sd/switch/jsrf/game/` too. `JSRF_HOME_NAME=jsrf-test` builds a
side-by-side copy instead (`jsrf-test.nro` in `SD:/switch/jsrf-test/`, with its
own settings, shader cache and saves), for trying a change without touching
the main install. The homebrew menu icon is the port's own
(`jsrf-switch/assets/jsrf_icon.jpg`); a 256x256 JPEG of your own at
`jsrf-switch/assets/icon.jpg` replaces it and stays out of git.

## Installing

1. Copy `switch_sd/switch/` to the root of the SD card.
2. Put the game on the card, either way:
   - the disc image as `SD:/switch/jsrf/<anything>.iso`. It is unpacked into
     `game/` on the first start, with a progress bar (about 2.5 GB free
     needed);
   - or the extracted files in `SD:/switch/jsrf/game/`.
3. Start **Jet Set Radio Future** from the Homebrew Menu opened through a
   title override (full memory). The first start also builds the shader
   cache, which takes a minute or two.

Runtime switches go in `SD:/switch/jsrf/jsrf_env.txt` as `KEY=VALUE` lines;
they are documented in the findings doc. `RECOMP_NX_PROFILE=1` turns on the
sampling profiler (`prof.bin`, read with `jsrf-switch/switch/prof_report.sh`);
leave it off for playing.

## Layout

| Path | What |
|---|---|
| `jsrf-switch/src/` | JSRF-specific overrides and the Switch platform layer |
| `jsrf-switch/src/recomp/gen/` | The generated code goes here (not included) |
| `jsrf-switch/xboxrecomp/` | The recompilation runtime, Switch-only: kernel, pushbuffer executor, deko3d renderer (`src/nv2a_dk/`), APU, USB and input, and the platform layer (`src/platform/`) |
| `jsrf-switch/switch/` | Switch build, shader pack and profile report scripts |
| `jsrf-switch/tools/` | Shader pack builder, profile reader and scanners |
| `regen/` | Passes over the generated C, the jsrf-recomp patch and their tests |
| `jsrf-nx-switch-findings.md` | The engineering log |

## Credits

This port stands on other people's work.

- **Jlancaster94**: testing on hardware. Most of the bugs fixed here were his
  findings.
- **xboxrecomp** by sp00nz and contributors,
  <https://github.com/sp00nznet/xboxrecomp> (MIT): the recompiler and the
  runtime (kernel replacement, NV2A renderer, MCPX APU model, OHCI USB and pad
  model). Contributors are listed in `jsrf-switch/xboxrecomp/CONTRIBUTORS.md`.
- **nfsu2-sw** by antoxa2584x, <https://github.com/antoxa2584x/nfsu2-sw>: the
  Switch port whose methods this follows. The runtime here began as nfsu2-sw's
  vendored copy of xboxrecomp, and this port's boot and build files began as
  its own. Everything that was nfsu2-sw's work (the Switch platform layer,
  thread and memory handling, input, USB, audio output, the pushbuffer
  executor and kernel changes, the boot code, the D3D interrupt wrapper and
  the build files) has since been rewritten for this repository. Its changes to the xemu-derived APU files remain, under
  those files' LGPL-2.1-or-later licence.
- **jsrf-recomp** by andeecollard,
  <https://github.com/andeecollard/jsrf-recomp> (MIT): the macOS JSRF port and
  the generator of the lifted code.
- **JSRF Unleashed** by JuMatt: ideas, no code. Its fixes pointed at two
  problems this port then solved its own way: the GPU interrupt hang (Marice's
  analysis, PR #46, and the idea of a rescue for a lost wake) and the
  transform unit's texture-coordinate generation (reflective glass).
- **JetSetRadioFuture-Unleashed-NX**, Eladerino's Switch fork of it: the
  missing functions. Its function discovery, run against this port's XBE,
  listed functions the translation never found, which led to the survey that
  lifted the boss-fight and thread-start functions here. Nothing else of the
  fork is used.
- **xemu**, <https://github.com/xemu-project/xemu>: the toolkit's APU code is
  extracted from it (LGPL-2.1-or-later; see `jsrf-switch/xboxrecomp/NOTICE`),
  and its NV2A semantics are the reference throughout.
- **Cxbx-Reloaded** and **xboxdevwiki**: hardware and kernel references.
- Also used: **devkitPro** (devkitA64, libnx, deko3d, uam), **SDL2**,
  **capstone**, **Atmosphère** and **sys-clk**.

## Licences

- `jsrf-switch/xboxrecomp/` is MIT (`LICENSE`), except the xemu-derived files
  listed in its `NOTICE`, which are LGPL-2.1-or-later (`LICENSES/`).
- Third-party code under `jsrf-switch/xboxrecomp/third_party/` keeps its own
  licences.
- Jet Set Radio Future is © SEGA. No part of the game is distributed here.
