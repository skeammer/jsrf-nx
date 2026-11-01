# Jet Set Radio Future on Switch via xboxrecomp — Findings & Nuances

Date started: 2026-10-03 (session). Earlier NROs booted through the in-engine
city intro, and a wild-pointer crash there was mitigated 2026-10-04. Run 6 of
the MMIO/shared-memory build failed before game entry: its 128 MB shared-memory
request returned `0x10801`. The runtime now falls back to code-memory backing
when that request fails, and a rebuilt NRO is staged. This fallback and the
rendering/input changes still need a hardware run.

## Goal

Statically recompile the original Xbox JSRF executable (via xboxrecomp, a
from-scratch Xbox static-recompilation toolkit — not dynamic emulation) into a
Nintendo Switch homebrew `.nro`. The approach lifts the XBE's x86 code to C
(`src/recomp/gen/recomp_000N.c`), links it against a from-scratch Xbox
kernel/NV2A/APU/USB emulation layer (`xboxrecomp/src/...`), and targets
devkitPro's switch-dev toolchain. A parallel macOS port of the same toolkit
(`jsrf-recomp`, a sibling checkout) is further along and is the primary
reference for both working patterns and still-open, unresolved bugs.

## Repo layout (this checkout)

- `jsrf-switch/xboxrecomp/` — the recompiler/runtime toolkit (kernel, NV2A,
  APU, USB, platform shims). Shared logic lives here, not in the per-game gen.
- `jsrf-switch/src/recomp/gen/recomp_000N.c` — the lifted-to-C game code,
  one file per XBE code region. Treat these as close to append-only; a change
  to `recomp_types.h` (included by every gen file) forces a full ~30 min
  rebuild on this hardware, vs. ~3 min for a single `.c` file.
  See `jsrf_spin_hint.py`/`block.h` under this session's `files/tools/` for a
  reusable script that inserts a yield macro into poll loops in a gen file
  without touching the shared header.
- `jsrf-recomp/` — the macOS reference port of the same toolkit against the
  same game. Its `docs/jsrf/progress/*.md` files are a large, dated log of
  prior investigations — grep there before re-deriving something from
  scratch; several "obvious" theories are already tried and refuted in detail.
- `linux-run/` — an x86 Linux repro harness (see below) that is far faster to
  iterate on than real Switch hardware.

## Switch-specific memory-layout nuances (the big one)

The Xbox exposes a flat 32-bit guest address space: RAM (and 28 host-aliased
*mirrors* of it, for titles that rely on cache-locality tricks), a 64 MB
contiguous/pinned-physical arena, a 64 MB tiled GPU aperture, the NV2A
register aperture, MCPX southbridge devices (APU/AC97/USB/NIC), and a 1 MB
flash ROM aperture — all mapped at `guest_va + g_memory_offset` into host
memory (`xbox_memory_layout.c`).

**Guest memory on Switch: code memory by default, exactly as NFSU2-SW.**
Under hbloader every committed guest range is a heap block wrapped in a
code-memory object and mapped read-write at its guest address
(`svcCreateCodeMemory` + `svcControlCodeMemory(MapOwner, Perm_Rw)`). It costs
nothing beyond the heap block and the loader hints the SVCs. The cost: a
code-memory view is `GeneratedCode` memory, which `svcMapProcessMemory` refuses
with `0xD401` (InvalidMemoryState), so no second view can alias it. The tiled
aperture at `0xF0000000` therefore gets standalone memory, and the startup
warning "tiled aperture does NOT alias the contiguous window" is expected in
this mode.

- `RECOMP_NX_SHM=1` (shared-memory objects): **do not use under hbloader.**
  Every object is new physical memory charged to the process, and the hbloader
  heap has already taken nearly all of it (`3189 MB total / 3185 MB used` at
  boot), so anything over a few MB fails with `0x10801` LimitReached (runs 6
  and 7). NFSU2's README marks it experimental and warns it may kill the
  process on hardware. Turning it on here was our mistake, not an NFSU2
  precedent.
- `RECOMP_NX_PROCMEM=1` (process code memory; experimental, ours, not
  NFSU2's): how hbloader maps an NRO. `svcMapProcessCodeMemory` moves a heap
  block to the guest address and `svcSetProcessMemoryPermission(Perm_Rw)` makes
  it `AliasCodeData`, which `svcMapProcessMemory` *can* alias (the libnx JIT
  fallback uses the same SVCs). It needs the real process handle, because
  0x74/0x75 take no pseudo-handle and hbloader passes a real one, and a 64 KB
  map/alias/unmap self-test passes before it is used. The heap block is
  `Locked` while mapped and must never be freed until unmapped, and aliases
  must be unmapped before their source. Test it alone, after a default run.
  Success is the line "Tiled aperture alias verified".
- NFSU2's documented fallback for no aliasing is different: fold
  `0x80000000-0x83FFFFFF` onto low RAM inside `XBOX_PTR` and the address
  translations. It is not used here, because JSRF's image lives in low RAM
  (see the contiguous-window comment in `xbox_memory_layout.c`).
- **RAM mirrors are not mapped on Switch.** Horizon reservations are
  bookkeeping only. The Extended VMA reservation at guest `0x74000000`
  "succeeds" on top of mirrors 14-15, and a title allocation there would be
  handed mirror pages, silently overwriting low RAM. Code memory cannot map
  them anyway (`0xD401`), and JSRF runs fine without them.
- `RECOMP_NO_ALIAS=1` reproduces the no-alias layout on Linux/macOS.

Run 5's tiled-aperture probe failed and explicitly logged that the GPU would
sample empty textures. That is a plausible cause of the white textures and the
invisible skater, but it is **unproven**: it only matters if JSRF writes
surfaces through `0xF0000000` and samples them through `0x80000000`. Confirm
with a `RECOMP_NX_PROCMEM=1` run, where only the alias changes, before
treating it as the texture bug.

**Unmapped "holes" between apertures are a real crash source, and backing
them with zeroed RAM is cheap insurance.** The apertures above don't tile the
4 GB guest space edge-to-edge; the gaps between them (e.g., 8 MB between the
end of the 16 MB NV2A aperture at `0xFE000000` and the start of the 8 MB MCPX
aperture at `0xFE800000`; ~15 MB between the end of the 1 MB flash aperture at
`0xFF100000` and the 4 GB top) are completely unbacked host address space on
a host with no SIGSEGV-to-recoverable-fault handling (Switch/POSIX, unlike the
reference's structured-exception-handling-capable hosts). A wild guest pointer
that lands there is a hard, unrecoverable process crash instead of a survivable
garbage read/write. **The existing codebase's own stated philosophy
(`xbox_memory_layout.c` comments) is "a read of zero is survivable, a fault is
not" — applied already to NV2A/MCPX/flash themselves. Extend the same
`VirtualAlloc(addr, size, MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE)` pattern to
the small gaps between apertures too; it's a few MB of lazily-paged scratch,
effectively free, and converts a class of hard crashes into silent, recoverable
corruption.** This was applied 2026-10-04 (see the dated entry below) after a
hardware crash's logged guest VA landed exactly in one of these gaps, and the
macOS reference's own unsolved wild-pointer crash doc independently named the
same category of address range.

**The reference's "always-unmapped holes" list (for a *mirror-capable* host)
is a floor, not the full picture, on a no-alias Switch build** — with mirrors
off, far more of the address space is unbacked than on a host that can alias.
Don't assume the reference's enumerated hole list is exhaustive for this
target; derive gaps from this build's own aperture base/size `#define`s
(`XBOX_NV2A_BASE/SIZE`, `XBOX_MCPX_BASE/SIZE`, `XBOX_FLASH_BASE/SIZE`, etc.)
rather than trusting a list written for a different host.

## Diagnostics worth knowing about

- **`[CRASH] exception N at PC=... far=... (guest VA 0x...)`** — our own
  runtime's in-engine crash report, logged to `jsrf_log*.txt` before the
  process dies. Far more actionable than the Atmosphère crash report, which
  (for hbmenu-launched homebrew) only resolves the `hbl` loader stub as a
  module and cannot symbolicate the NRO itself.
- **`recent ICALL targets:` / `icalls so far:`** (`watchdog_dump` in
  `xbox_memory_layout.c`) — a 16-entry ring buffer of recent indirect-call
  (guest `call reg`/`call [mem]`) targets, dumped on crash and periodically.
  Repeating, clustered values here (e.g., four values cycling) right before a
  crash point at a corrupted function-pointer table / vtable / callback list,
  not a one-off wild jump — look at what the clustered addresses are offset
  from (a real aperture base is a strong clue, as it was here).
  `g_icall_count` (total, monotonic) separates "hung" from "merely slow": a
  pure CPU loop makes no kernel calls, so compare indirect-call deltas, not
  kernel-call deltas, to tell a spin from a stall.
- **`[PB] bad target #N ... resync`** (`nv2a_pb_scan.c`) — the NV2A pushbuffer
  scanner detected a malformed jump/call target in the GPU command stream and
  recovered by resyncing to PUT. Survivable by design; a rising count is a
  symptom of the same wild-pointer class of bug corrupting the command stream,
  not independently fatal.
- **`[hitch] frame N: ... ms -- ...`** — per-frame cost breakdown, useful for
  separating "slow" from "stuck": if the frame counter and timestamp keep
  advancing, the process is alive and working, not deadlocked.
- A `peek_readable()` helper and `RECOMP_PEEK=<va>[+off][,...]` env var exist
  for one-off guest-memory inspection against all mapped windows at once
  (RAM, contig, NV2A, MCPX, flash, and now the two gap regions) — cheaper than
  adding ad hoc prints when chasing a specific structure.
- **Input chain, end to end (`RECOMP_INPUT_DIAG`).** Each stage has its own
  line, so a lost press can be placed between two of them:
  - `[INPUT] ... buttons= analog= reports= synth=` (`usb_gamepad.c`): the
    host pad only. `buttons` is never what the game saw, and on Linux
    `InputGetState=1167` is constant (no host pad). `synth` counts reports
    carrying a `RECOMP_PAD_PRESS`/`RECOMP_PAD_SCRIPT` press.
  - `[OHCI0] report -> td … cbp … (va …)` (`ohci.c`): where each report was
    written, logged when the buffer or its resolved VA changes.
  - `[XAPI] report(rec=…) … kept= packet=` and `[XAPI] report census`
    (wrapper of XPP's report callback `sub_001BDE21`): what XPP copied into the
    device record.
  - `[XAPI] census: N XInputGetState calls, N with a button held`: what the
    game read. This is the line that proves input works.
- **`[KERNEL] MmGetPhysicalAddress(va) = 0x4xxxxxxx (pass-through, tagged)
  from 0x…`** — always on, first 16 call sites. Names every guest call site
  that hands pool, heap or stack memory to a device (input entry below). For
  JSRF: `0x001C365A` (XAPI's OHCI driver, pad report buffer) and `0x001A4E42`
  (DSOUND, sound-buffer pages for the APU).
- `RECOMP_PAD_PRESS=mask,period_ms,hold_ms` (e.g. `0x10,3000,300` pulses
  Start) and `RECOMP_PAD_SCRIPT="ms:buttons:hold,…"` inject synthetic presses
  into the report path. This is how the Linux harness tests input.
- **No Atmosphère crash report after a crash is expected.** Our exception
  handler logs `[CRASH]` and calls `svcExitProcess`. HOS shows its generic
  "software was closed" popup, but creport never sees an unhandled exception.
  The `[CRASH]` lines in `jsrf_log*.txt` are the report.
- **Horizon result codes** are `module | description << 9`.
  `0x10801` = kernel LimitReached (132): out of the process's physical-memory
  budget. `0xD401` = InvalidMemoryState (106): wrong memory kind, *not*
  missing permission. `0xE401` = InvalidHandle (114): a pseudo-handle passed
  where a real one is required. `0x4201` = NotImplemented (Eden's shared
  memory). `0xFA01` = InvalidState (`svcMapPhysicalMemory` without a system
  resource). `0x115D` = libnx binder (module 349) NoInit: a display error.
  A refused SVC that was *not* hinted kills the process instead of returning
  a code, so check `envIsSyscallHinted` before trying one.

## Switch runtime configuration (`jsrf_env.txt`)

`switch/build.sh` writes this next to the NRO. `load_env()` in `switch_nx.c`
reads it at boot: one `KEY=value` per line, `#` comments, and values override
the defaults. Current file (run 8):

```
# JSRF Switch Configuration
RECOMP_VBLANK=1
RECOMP_AC97_READY=plain
RECOMP_USB=1
RECOMP_PB_EXEC=1
RECOMP_QUIET=1
RECOMP_GIL=0
RECOMP_USB_TRACE=1
RECOMP_INPUT_DIAG=1
# Texture-alias experiment (uncomment for a later test run):
# RECOMP_NX_PROCMEM=1
# Overclock presets (uncomment if sys-clk is installed):
# JSRF_CPU_MHZ=1785
# JSRF_GPU_MHZ=921
# JSRF_MEM_MHZ=1600
```

| Variable | Effect | Parsing |
|---|---|---|
| `RECOMP_VBLANK` | 60 Hz NV2A vblank interrupt into the title's ISR/DPC (D3D waits on it) | on unless `0` |
| `RECOMP_AC97_READY=plain` | Emulated APU plus registered APU MMIO; fixes the AC97 loading spin | value checked |
| `RECOMP_USB` | OHCI controller emulation, needed for pads | **presence enables** |
| `RECOMP_PB_EXEC` | Execute the NV2A pushbuffer instead of skipping it | **presence enables** |
| `RECOMP_QUIET` | Less kernel logging | `1` only |
| `RECOMP_GIL` | Guest lock (NFSU2 default on); deadlocks JSRF, see the audit entry | `1` only |
| `RECOMP_USB_TRACE` / `RECOMP_INPUT_DIAG` | OHCI register trace and pad diagnostics | **presence enables** |
| `RECOMP_NX_PROCMEM` | Aliasing guest memory (experimental) | `1` only |
| `RECOMP_NX_SHM` | Shared-memory guest memory; **fails under hbloader** | `1` only |
| `RECOMP_GIL_EAGER` | With `RECOMP_GIL=1`: hand the lock over by guest priority, not FIFO | value starting with `1` |
| `RECOMP_VP_XFCTX` | Viewport/fog/eye methods also write vertex-program c[56..59] (2026-10-20) | on unless `0` |
| `RECOMP_VSH_MUL_ZERO` | Vertex programs use NV2A's 0 × anything = 0 (2026-10-20) | on unless `0` |
| `RECOMP_PB_2D` | Log methods on non-3D subchannels (first 4000) | off; any value not starting with `0` enables |
| `JSRF_CPU/GPU/MEM_MHZ` | Clock requests via clkrst/pcv, restored at exit | MHz |

Nuances: for the "presence enables" variables, `=0` does **not** turn them
off; delete or comment out the line. `src/main.c` also sets VBLANK,
AC97_READY, USB, PB_EXEC, GIL=0 and QUIET as required defaults, so deleting
those lines does not disable them either.

## Linux repro harness (fast iteration before burning a hardware cycle)

Real Switch hardware cycles are slow (SD card copy + boot) and contended with
other work on this machine. A podman-based x86 Linux build + headless Xvfb run
reproduces the Switch-relevant memory layout and most runtime behavior in
seconds-to-minutes instead:

- Build: `podman run --rm -v <repo>:/workspace:z -w /workspace/jsrf-switch
  localhost/jsrf-linuxdev:local2 bash -lc 'ninja -C build-linux -jN'`
- Run: `podman run --rm --cap-add=SYS_PTRACE --security-opt seccomp=unconfined
  -v <repo>:/workspace:z localhost/jsrf-linuxdev:local2
  /workspace/linux-run/run_shots.sh <secs> RECOMP_KERNEL_LOG_BUDGET=<n>
  [RECOMP_NO_ALIAS=1]` — takes periodic Xvfb screenshots (`shots/*.xwd`,
  convert with `linux-run/xwd2png.py`) and, if still running after `<secs>`,
  attaches gdb and dumps all thread backtraces (`gdb.out`) before killing it.
- `RECOMP_NO_ALIAS=1` is essential for Switch-realistic repro (see above).
- Cap podman memory (`--memory=2g --memory-swap=3g` or similar) when other
  heavy builds are running concurrently on the same machine.
- Caveat: this is NOT a substitute for a hardware test on timing-sensitive
  bugs. A 300s run with no repro does not prove a timing-dependent crash is
  fixed — it only lowers confidence it's frequent. Conversely, container/
  software-GL overhead means a scene that takes 60s here might take a couple
  of seconds on real hardware; don't read too much into absolute Linux timings
  when judging whether something on Switch is "stuck" vs. "slow".

## Dated entries

### 2026-10-04 — NV2A/MCPX and post-flash gap crash

**Symptom (hardware):** game now loads and renders into the in-engine city
intro (a milestone — see prior AC97/vblank/ICALL/spin-hint fixes, not detailed
here yet), then hard-crashes partway through.

**Root cause (strong circumstantial evidence, not fully proven):** our own
`[CRASH]` log showed `guest VA 0xFE3941B0` — inside the unbacked 8 MB gap
between the NV2A aperture's end (`0xFE000000`) and the MCPX aperture's start
(`0xFE800000`). The macOS reference's own unresolved wild-pointer investigation
(`jsrf-recomp/docs/jsrf/progress/PROGRESS_2026-09-18_THE_CRASH_IS_A_GARBAGE_TREE_ROOT.md`)
independently catalogued this exact address range as one of a handful of
permanently-unmapped guest holes and ranked five unconfirmed root-cause
hypotheses for *landing* there (wild store, freed-node reuse, cross-thread
race, lost-register epilogue, kernel/HLE bridge write) — this session did not
resolve which of those applies; it mitigated the crash instead.

A same-build second hardware run's crash log additionally showed
`eax=FD000000` (exactly `XBOX_NV2A_BASE`) and `recent ICALL targets:
FE000100 FE000104 FE000174 FE0000A4` repeating — i.e., addresses computed as
`NV2A_BASE + small_offset` (just past the 16 MB aperture) being used as
*indirect call* targets, not just data pointers. This is a sharper, more
specific lead than "a wild pointer somewhere" — something is treating an
NV2A-aperture-relative value as a function-pointer-table base. Not yet root
caused; flagged here for whoever picks this up next.

**Mitigation shipped:** back both small inter-aperture gaps
(`0xFE000000-0xFE7FFFFF`, `0xFF100000-0xFFFFFFFF`) as plain zeroed
`PAGE_READWRITE` memory, exactly the existing pattern already used for
NV2A/MCPX/flash themselves. Added to `xbox_memory_layout.c`, with matching
shutdown (`VirtualFree`) and `peek_readable()` entries.

**Validated:**
- Linux, `RECOMP_NO_ALIAS=1`, 300s: hit `[PB] bad target` 9 times (more than
  any prior run) with zero crashes — previously this was a hard crash.
- Hardware, same build: no more auto-crash at this point; the same
  `FE0000xx` indirect-call cluster recurs later in the log (293s) and the
  process keeps running instead of dying. Strong behavioral confirmation the
  mitigation is absorbing the intended failure mode.
- **Not a free lunch:** this converts a crash into unknown-correctness
  territory for whatever guest state that indirect call was supposed to
  touch. Watch for new, different symptoms downstream of where the crash used
  to be — see the next entry.

**Open gameplay symptoms from run 5 (not yet root-caused; run 6 stopped before
these could be retested):**
green tint across the whole scene, the skater model not visible (world/track
geometry renders fine), textures popping in/out, and the title/attract screen
not responding to controller input (could not "press start"). Whether any of
these are consequences of the gap-backing mitigation (silently-tolerated
corruption reaching further into rendering state) vs. pre-existing bugs only
now reachable because the game no longer crashes first, is an open question.

### 2026-10-04 — Run 5: input never reaches the pad backend

Run 5 loaded both `RECOMP_USB_TRACE=1` and `RECOMP_INPUT_DIAG=1` from
`jsrf_env.txt`, so the failed input was not caused by a stale configuration
file. `src/main.c` calls `xbox_OhciInit()`, and startup logs the OHCI controller
and MMIO ranges as registered. However, `jsrf_log5.txt` contains no
`[OHCIx] read`/`write` trace, no `operational after` message, and only
`[OHCI0] not started after 30 s`. It also has no `[INPUT]` or `[NX] pads`
diagnostic. The guest is not reaching the OHCI model; Switch pad mapping is
downstream and is not the first failing layer.

The generated XPP section is `0x001BC7C0-0x001C3F18`. Its 230 functions in
`recomp_0008.c` and two recovered entries in `recomp_stubs_unresolved.c` use
plain `MEM*` accesses and `RECOMP_MEM_WRITE*` stores; the generated
`recomp_types.h` contains no MMIO accessors. In contrast, the runtime has an
OHCI MMIO model and the toolkit template has `MMIO_RD/WR` accessors. The
toolkit's textual MMIO rewrite only handles raw `MEM*` forms, not the
`RECOMP_MEM_WRITE*` stores emitted by the JSRF-specific lifter. Therefore
adding only runtime flags, or only rewriting reads, is insufficient: both XPP
reads and stores must route through the model. This is the likely root cause
of the inactive controller; fix the JSRF code-generation path and generated
runtime header while preserving the existing Switch-specific generated-code
patches.

The same run still shows the tiled-aperture alias warning ("the GPU will sample
empty textures"), a strong lead for the white/empty textures and pop-in, plus
10 malformed pushbuffer targets and the recurring `FE0001xx` indirect-call
cluster. Those are separate rendering/corruption risks; the log does not yet
prove which causes the varying tint or missing skater. The Atmosphere report
associated with run 5 is a `User Break` in `hbl`, not a logged guest-memory
exception, and `jsrf_log5.txt` has no `[CRASH]` before it ends. The older
`jsrf_log3b.txt` still contains the real guest exception at `0xFE3941B0`.

### 2026-10-04 — Applying the NFSU2 MMIO and memory-mode precedents

NFSU2-SW is the relevant reference for the current input and alias symptoms:
its regeneration passes `--mmio-sections DSOUND,XPP`, and its runtime
initializes OHCI and registers the APU model. JSRF's custom lifter instead
emitted ordinary `MEM*` reads and memory-watch `RECOMP_MEM_WRITE*` stores for
those sections, while its generated header lacked MMIO dispatch. The fix is a
post-generation, section-scoped rewrite that runs after recovered entries are
added, routes raw 8/16/32-bit accesses and wrapped 8/16/32/64-bit/float/double
stores through the registered runtime model, and preserves the original guest
PC/function metadata and the normal memory-watch path. It is wired into the
regeneration script and applied to the current generated Switch C files.
The current Switch generated tree was patched in place to retain its existing
Switch-specific code; its `GENERATION_MANIFEST.txt` remains the prior full
generation's provenance and was not edited by hand. A future full regeneration
will hash the new rewriter and runtime template.
`RECOMP_MMIO_RUNTIME=1` is set by the Switch CMake target only; the shared
`jsrf-recomp` template maps these accessors back to ordinary memory when the
flag is absent, because the macOS reference runtime has no MMIO dispatcher.
The two hand-written DSOUND overrides remain outside the generated-code pass:
one intentionally skips an unimplemented DSP submission, and the other edits
the voice object's flags rather than device registers.
The Switch build staged `switch_sd/switch/jsrf/jsrf.nro` (20,047,247 bytes);
the first hardware run of that NRO failed during memory-layout initialization,
as detailed in the run 6 entry below. The adjacent `jsrf_env.txt` contains
`RECOMP_NX_SHM=1` and the USB/input diagnostics.
The rewrite selected 642 DSOUND/XPP functions and modified 632 across
`recomp_0008.c` and `recomp_stubs_unresolved.c`; a second pass made no further
changes. All eight focused rewriter tests passed, and both affected Linux
objects compiled with MMIO dispatch enabled.

The Switch environment opts into `RECOMP_NX_SHM=1`, and the staging script
retains that option plus the USB/input diagnostics. Run 6 reached shared-memory
mode but could not allocate its 128 MB mapping; the updated runtime now falls
back to code-memory backing for that mapping. Hardware validation still needs
OHCI read/write traces and an `operational` controller message for input, plus
a verified tiled-aperture alias for textures.

NFSMW-NX is an Xbox 360 PowerPC/ReXGlue port with a different renderer and is
not a direct source for the original-Xbox XPP/OHCI or NV2A alias fixes. Other
NFSU2 optimizations and title-specific patches were not transplanted without
matching evidence.

**Correction (after run 7):** the MMIO half of this entry follows NFSU2. The
memory half does not. NFSU2's default is code memory, and `RECOMP_NX_SHM` is
its experimental opt-in with a hardware warning. Enabling it caused runs 6
and 7. It has been removed; see the NFSU2 alignment entry below.

### 2026-10-04 — Run 6: shared-memory allocation fails before boot

`logs/jsrf_log6.txt` shows `RECOMP_NX_SHM=1` was loaded and the 4 KB
`svcCreateSharedMemory` probe selected the alias-capable mode. The first real
file mapping then requested 131,072 KB and failed with Horizon result
`0x10801`; the compatibility shim translated that to Win32 error 8, so
`xbox_MemoryLayoutInit()` returned false before loading XBE sections or running
game code. `src/main.c` sets the map to 128 MB by adding 64 MB of separate
reserve space to the 64 MB Xbox RAM map.

The `3189 MB total / 3185 MB used` system-memory line is not sufficient to
conclude that the console ran out of general memory: run 5 printed the same
figures while the code-memory mode successfully mapped the 128 MB guest region
and entered the game. The exact Horizon resource/size constraint behind
`0x10801` remains unknown. The matching fatal report
`logs/fatal_reports/01791139929_05446530aca7e000.log` has result `0x115D` but
no PC or stack trace. Since the initialization failure goes through the
Switch error applet in `src/main.c`, this report does not establish a separate
guest CPU crash after the Sphaira error was acknowledged.

`win32_compat.c` now falls back to code-memory backing for an individual file
mapping if creating its shared-memory object fails, while keeping
`RECOMP_NX_SHM=1` enabled for mappings that the system can allocate. The
fallback can restore startup, but it may not provide the aliases needed by
the tiled GPU aperture. The rebuilt NRO is staged at
`switch_sd/switch/jsrf/jsrf.nro` (20,047,247 bytes); the environment file is
unchanged. A hardware retest is still needed, and if the fallback log appears,
the texture-alias problem remains open.

**Correction (run 7):** the memory figures *do* explain `0x10801`. It is
kernel LimitReached. Code memory reuses heap blocks the hbloader heap has
already charged to the process (3185 of 3189 MB at boot). Each shared-memory
object is new memory charged against the ~4 MB left. Run 7's 4 KB probe
passed, and every object of 8 MB or more failed. The `0x115D` fatal is libnx
binder NoInit, a display error. It came after our NRO returned to hbloader,
which then starts the next NRO (Sphaira) in the same process. Most likely
Sphaira's display setup failed next to what our run left behind. Exiting is
fixed in run 8.

### 2026-10-04 — Run 7: shared mode leaves the NV2A aperture unmapped

`logs/jsrf_log7.txt`: all four memory SVC hints were present, and the
shared-memory probe selected shared mode. The RAM map (128 MB) and the
contiguous window (64 MB) failed with `0x10801` and took the run 6 fallback.
`nx_commit()`, which backs `VirtualAlloc(MEM_COMMIT)` at a fixed address, had
no fallback in shared mode. NV2A (16 MB), MCPX (8 MB), both gaps and the tiled
aperture stayed unmapped, and the log said so ("D3D register access will
fault"). At 1.55 s the title's first NV2A register access faulted:
`[CRASH] exception 257 ... (guest VA 0xFD001804)`. No creport was written
(see Diagnostics). `nx_commit()` now falls back to heap-backed memory. The
real fix is not using shared memory at all (run 8).

### 2026-10-04 — NFSU2 alignment audit

Checked against NFSU2-SW's `CLAUDE.md` Switch findings, README env table,
`src/switch_nx.c` and toolkit (local clone `jsrf-nx/nfsu2-sw/`).

Follows NFSU2:
- `jsrf-switch/xboxrecomp/` *is* NFSU2's vendored toolkit. Only
  `kernel_bridge.c`, `kernel_thunks.c`, `xbox_memory_layout.c/.h` and
  `win32_compat.c` differ. Its Switch fixes are inherited:
  - The `host_read`/`host_write` bounce buffer (never hand guest memory to
    Horizon IPC).
  - The OHCI 4 ms tick.
  - Thread spreading over cores and the unaligned-atomics spinlock.
  - Renderer fixes, the RAM log and the dir-handle leak fix.
- Input: DSOUND/XPP MMIO dispatch, OHCI and the APU model (entry above).
- Memory: code memory by default (restored for run 8).

Deliberate deviations, each with its reason:
- **`RECOMP_GIL=0`.** NFSU2 serialises guest threads under one lock. The lock
  is released at the `RECOMP_PREEMPT` yield points its lifter emits, and
  `recomp_spin_yield` runs every 64 poll turns. JSRF's lifter emits none, so
  with the lock a spinning thread starves the thread it waits for: deadlock
  (checkpoint 007). The macOS reference has no GIL either. **Risk:** NFSU2
  needed the lock against guest-thread races (its 0xAAAAAAAA stream-block
  race). JSRF's intermittent crashes and run-to-run graphics differences
  could be that class. Unproven. Testing it means emitting yield points, not
  flipping the variable.
- **No RAM mirrors on Switch** (memory section above).
- **Exit closes the application instead of returning to hbloader** (run 6).
  `__nx_applet_exit_mode = 1` makes libnx send AM SelfExit first, as it does
  for an NSO. A bare `svcExitProcess` from an application is reported as a
  crash (the run 7 popup). `envSetExitFuncPtr(svcExitProcess)` covers the
  other applet types. NFSU2 returns to hbloader.
- **`RECOMP_NX_PROCMEM=1`** is an opt-in experiment, not NFSU2's.

### 2026-10-04 — Run 8 build

Staged: `switch_sd/switch/jsrf/jsrf.nro` (20,051,343 bytes, sha256
`b8d9aaf1…`) and `jsrf_env.txt` (`RECOMP_NX_SHM` removed, `RECOMP_NX_PROCMEM`
commented out). This is the first hardware run of the NFSU2 MMIO input
rewrite, with memory back on NFSU2's default as in run 5. Expected startup
lines:
- `[NX] guest memory from code-memory views of heap blocks (no aliasing)`
- `RAM mirrors: not mapped on Switch`
- `Tiled aperture: 64 MB ... (standalone memory)`, followed by the "does NOT
  alias" warning (expected in this mode)
- no `0x10801`

Input evidence comes from the OHCI trace and controller lines
(`RECOMP_USB_TRACE`, `RECOMP_INPUT_DIAG`). Quitting, or an init error, now
returns to the HOME menu; a crash still shows the HOS popup.

### 2026-10-04 — Run 8 results: cutscene crash was pushbuffer desync

Run 8 booted cleanly, then crashed every time the first cutscene loaded. It
also showed white geometry, no PRESS START text, a glitchy audio loop and no
input. The crash reproduces on the Linux harness (`RECOMP_NO_ALIAS=1`) as
`[PB] bad target` resyncs followed by a wild jump. Two causes, both fixed the
way NFSU2 fixed them:
- **Fence mirror.** The host fence mirror wrote D3D's fence word on its own
  schedule, so `BlockOnTime` (`sub_00191440`) saw fences retire before the
  executor reached them, and the CPU overwrote pushbuffer the GPU had not
  consumed. With `RECOMP_PB_EXEC`, `fence_mirrors_tick()` now registers the
  semaphore (`nv2a_pb_set_semaphore_target`) and the executor's semaphore
  releases drive it; the mirror is off. Desyncs went from 9 to 2.
- **PGRAPH trap protocol.** The toolkit's default trap delivery loses traps
  and runs handlers twice. JSRF's PGRAPH handler `sub_00194210` (NFSU2's
  `sub_002F22F0`) is now wrapped as NFSU2 does: ISR enter, trap lock, sample
  NSOURCE, run the body, `nv2a_pb_trap_taken()`. Result: 0 bad targets and no
  crash over 260 s.

### 2026-10-04 — White geometry: NV2A immediate mode

The white was D3D's Begin/End immediate-mode draws. The city's stencil-shadow
quad should be black at alpha ~0.5 and was drawn opaque white. The executor
now implements immediate mode with xemu's semantics:
- every attribute method (0x1500–0x16CF, `SET_VERTEX_DATA*` 0x1880–0x1AFF)
  sets the current value;
- an attribute written after the first vertex becomes per-vertex, and earlier
  vertices are backfilled with its old value;
- the rest are drawn as constants;
- `SET_VERTEX_DATA4UB` is bytes / 255 with no swizzle (the XDK pre-swizzles).

The old path's BGRA diffuse also swapped red and blue (the cyan sky).
Validated on Linux: the city renders textured.

### 2026-10-04 — Input: pad reports written into the contiguous window

Symptom: Switch and Linux both saw Start for the first ~300 ms after the pad
was inserted, then never again, although the host pad kept pressing and the
packet number kept rising. Traced stage by stage (diagnostics section):
- The host source kept producing presses (`synth` rose).
- XPP's report callback received them only during the first pulse.
- The OHCI model kept writing to bus `010DE0CA`, but resolved it to VA
  `010DE0CA` for the first pulse and to **`810DE0CA`** afterwards.

Cause: `MmGetPhysicalAddress` returned non-contiguous VAs unchanged, and the
contiguous arena hands out physical addresses `[image end, 64 MB)` that grow
over time. JSRF's pool (`ExAllocatePool*` from `0x010DD000`) and heap
(`NtAllocateVirtualMemory` from `0x00F81000`) sit in those same numbers. The
pad's XInputOpen record is pool memory, and its report buffer is at
`0x010DE0CA`. Once the arena passed physical `0x010DE000` (within 3 s of
the pad appearing; it reached `0x01E15000` within 30 s),
`xbox_ContiguousIsPhysical()` claimed the address.
Every report then went into the window, overwriting 20 bytes of a contiguous
allocation every 8 ms, while XPP read the real buffer. The arena's design
comment had assumed only image data was pass-through. NFSU2 never hit this
because its pad record is contiguous, and the Mac port lifts XInput.

Fix (toolkit, so every title gets it):
- `MmGetPhysicalAddress` tags a pass-through VA in `[image end, 64 MB)` with
  `XBOX_BUS_PASSTHRU` (`0x40000000`).
- `xbox_BusPassthroughVa()` (`xbox_devbus.c`) undoes the tag. It is checked
  first by OHCI `bus_to_va`, APU `apu_phys` (before its 64 MB mask), NV2A
  `dma_resolve` and the AvSetDisplayMode framebuffer.
- Image VAs and VAs at or above 64 MB stay untagged.

Only two call sites receive tags: XAPI's OHCI driver (`0x001C365A`) and
DSOUND's APU page mapper (`sub_001A4E12` at `0x001A4E42`, which writes
`SET_CURRENT_INBUF_SGE_OFFSET`). The APU keeps the tag in its SGE entries
(`& 0xFFFFF000`), so the fix lands there too.

Linux validation (synthetic Start every 3 s against a no-press run):
- the report VA stays put;
- "with a button held" keeps rising (21 → 148);
- the logos and anti-graffiti screen are cut short;
- the title (`title.adx`) is reached at about 30 s of polling instead of 65 s.

**Audio:** DSOUND's title BGM ring is pool memory (`0x013EF000`), and the
arena had covered that physical address since early boot (a 396 KB
contiguous block at `0x813EC000`). Before the fix the APU played that
block's bytes as the BGM. Linux A/B on the same title segment, captured with
`RECOMP_AUDIO_DUMP=path,skip_s,len_s` and checked with
`linux-run/audio_loopcheck.py` (loudness-envelope autocorrelation):
- **without the tag:** a flat envelope (variation 0.14) repeating every
  370 ms (autocorrelation 0.94), the same loop for 40 s. This is run 8's
  "glitchy loop".
- **with the tag:** a varying envelope (0.32) and no periodicity above 0.30.

Block-hash repeat counts miss this, because a resampled loop is not
sample-identical from lap to lap.

**Lesson for future ports:** any title whose XDK libraries DMA from pool or
heap memory hits this as soon as the contiguous arena grows past them, and
the symptom looks like "works for a while". Check the tagged-caller log
early.

**Hand edits to the generated code** (a Mac `regenerate.sh` run loses them):
- `sub_00194210` (PGRAPH wrapper);
- XAPI trace wrappers: `sub_001BCBCC`, `sub_001BD5FF`, `sub_001BD621`,
  `sub_001C3BA1`, `sub_001C3C16`, `sub_001C3C22`, `sub_001C3E14` and
  `sub_001BDE21`.

Each body is renamed `_gen` in `recomp_0008.c`/`recomp_funcs.h`, and the
wrapper lives in `src/jsrf_manual_overrides.c`. Backups are the
`*.before-*` files next to them.

### 2026-10-04 — Run 9 build

Staged: `switch_sd/switch/jsrf/jsrf.nro` (20,067,727 bytes, sha256
`77955ce2…6c41`). `jsrf_env.txt` is unchanged from run 8. Contains the
desync fixes, the immediate-mode white fix and the bus-address tag (input
and audio). Expected log lines:
- `[KERNEL] MmGetPhysicalAddress(0x010D…) = 0x410D… (pass-through, tagged)
  from 0x001C365A` once the pad is inserted.
- `[OHCI0] report -> … (va 010D…)` and never a later `810D…` line.
- `[XAPI] census: … with a button held` rising while + or A is held.
- Title BGM that plays as music instead of a loop.

### 2026-10-04 — Handover

Packed as `~/jsrf-nx-handover-2026-10-04.zip`, with `jsrf-nx/` inside.

Contents:
- the source tree, without build directories;
- the run 9 NRO and its env file;
- the hardware logs;
- the Linux harness scripts and a `Containerfile` that rebuilds
  `jsrf-linuxdev:local2`;
- this document;
- a README with build commands and credits.

`regen/` holds what lived outside the tree:
- our uncommitted jsrf-recomp changes, as a patch against `4242eb6`: the MMIO
  rewrite pass, its tests, the `regenerate.sh` hook and the
  `recomp_types.h` accessors;
- `jsrf_spin_hint.py` and `block.h`, previously in session files. The
  script now loads the vendored `spin_hint.py`.

Left out:
- game data;
- jsrf-recomp (`andeecollard/jsrf-recomp` at `4242eb6`);
- nfsu2-sw (`antoxa2584x/nfsu2-sw` at `e7fa1f2`);
- the venv (`capstone`, `pillow`);
- build directories.

`xboxrecomp/src/apu/apu_shim.h` (LGPL) now carries a dated change notice,
because the LGPL requires one on modified files. `src/switch_nx.c` is
adapted from nfsu2-sw, which has no licence file. Neither reference repo
publishes lifted code or NROs.

### 2026-10-20 — Preemption points, vertex-program constants, NV2A multiply

Work from the handover zip without game data or a Switch toolchain: every
change below builds on Linux (gcc 13, `-O1 -g1`, about 2.5 min for a full
rebuild), and the two that can be tested without the game have tests. None
has been run against the game, and no NRO was built. Commits are on top of
the handover snapshot (`d0ae60d`). Run `linux-run/ab_run.sh` to compare each
change against the old behaviour on the harness.

**Guest lock (`RECOMP_GIL=1`) can now be tested.** The lifted code had no
preemption points: 0 `RECOMP_PREEMPT` in `gen/`, against NFSU2's translator,
which emits one at every function entry. So a guest thread spinning on
another's flag never let it run (the checkpoint 007 deadlock). The vendored
runtime already had the lock, `g_gil_contended` and `recomp_preempt()`.
- `regen/jsrf_preempt.py` (`d349383`) adds `RECOMP_PREEMPT()` to the
  generated C text, so the hand edits survive: 9000 function entries and
  7187 loop back edges (any `goto` to a label earlier in the same function;
  every goto loop has one). The lifter's own `while`/`for` loops for string
  ops and bit scans (about 420) get none, but they are bounded. It is upstream xboxrecomp's `--back-edge-yield`
  idea, applied after generation. Edges already marked `RECOMP_SPIN_HINT`
  are skipped. The pass is idempotent.
- With `RECOMP_GIL=0` the check is one load of a flag only the lock sets,
  so the default build behaves as before.
- **ADX guard fix (`f016693`).** A thread waiting for the guard kept the
  guest lock, so the guard's holder could never run to leave. Waiters now
  suspend the lock while waiting. It matters only with
  `RECOMP_ADX_SERIALIZE=1`, which nothing sets yet.
  `linux-run/tests/adx_gil_test.c` hangs on the old guard and passes on
  the new.
- To test: `RECOMP_GIL=1` (and `RECOMP_GIL_EAGER=1`) on the harness. Watch
  `gdb.out` for every guest thread parked in `gil_lock`, the frame count
  against the default, and the run-to-run graphics differences.

**Vertex-program constants c[56..59] (`a2697f5`).** On NV2A,
`SET_EYE_POSITION`, `SET_FOG_PLANE`, `SET_VIEWPORT_SCALE` and
`SET_VIEWPORT_OFFSET` write constants 56–59 (xemu's
`NV_IGRAPH_XF_XFCTX_EYEP/FOG/VPSCL/VPOFF`, in `nv2a/nv2a_regs.h`). The XDK
epilogue of every vertex program scales and offsets `oPos` with c[58] and
c[59]. The executor kept the offset for the fixed-function path only.
- On the Mac this put 714,240 vertex-program draws at the origin (fixed
  4 Sep, commit 1a93eeb). Upstream xboxrecomp fixed the viewport slots in
  0e9cff9 and the fog plane and eye position in #210. 0e9cff9 predates the
  fork point, but this executor (by way of nfsu2-sw) did not have it.
- JSRF draws its characters with vertex programs (about 21 draws a frame)
  and most of the world with fixed function, so this is the strongest lead
  for the **invisible skater**.
- `RECOMP_VP_XFCTX=0` restores the old behaviour.

**NV2A multiply rule (`8a7cbb0`).** NV2A gives 0 for 0 × anything,
infinity and NaN included. GLSL (and the CPU interpreter) gave NaN for
0 × inf, and a NaN in `oPos` drops the triangle.
- The Mac met it through a zero normal (RSQ(0) = +inf, then 0 × inf). Its
  NaN texture coordinates hid alpha-cut fences, and the Mac fixed them with
  this rule on MUL/MAD/DP3/DP4. **Weaker lead here:** this executor already
  returns 1.884467e+19 for RSQ(0) and RCP(0), so only an overflow or an
  infinite constant can reach the rule. The c[58]/c[59] fix is the
  stronger candidate for the skater.
- The rule now applies in `gl_vsh.c` (MUL, MAD, DP3, DPH, DP4, DST, both
  preludes) and in `vp_run`.
- `RECOMP_VSH_MUL_ZERO=0` restores IEEE multiplies, and with it the GLSL is
  unchanged.
- `linux-run/tests/vsh_glsl_test.c` writes shaders covering every opcode,
  and glslang compiles them for GL 3.3 and Vulkan.

**From nfsu2-sw after `e7fa1f2` (`452d65c`).**
- `RECOMP_PB_2D=1` logs the first 4000 methods on non-3D subchannels, which
  the executor drops. Any `[2D]` lines on the title are a lead for the
  missing PRESS START.
- `FLIP_STALL` timeouts now step the flip read index (one lost sync had
  stuck NFS Carbon at 3.7 fps). **This does nothing in JSRF yet.** The
  waiting branch runs only once something calls `nv2a_pb_flip_retired()`.
  NFSU2's PGRAPH wrapper does; JSRF's `sub_00194210` wrapper was ported
  without that bookkeeping (patch 1's notes). So FLIP_STALL never waits
  here, and the title can render faster than vblank retires flips, which
  overwrites D3D's two-entry flip queue. That is a separate lead for frame
  pacing and pop-in. Wiring it needs JSRF's flip head, as in nfsu2-sw
  `src/recomp_manual.c`.

**Ruled out statically** (Mac leads that this backend already handles):
- X8R8G8B8 alpha (forced opaque here);
- swizzled X1R5G5B5/A4R4G4B4 (mapped to their linear twins);
- two depth textures for one zeta (depth is keyed by zeta address);
- a render target missed by address (surface and texture both go through
  `dma_resolve`);
- texture wrap modes;
- CPU culling of vertex-program batches (the GL path has none).

Still possible: the title sampling an offset *inside* a surface, which only
`RECOMP_GL_WATCH` on a run can show.

**Never-taken branches.** jsrf-recomp's lifter turned 51 conditional jumps
whose flags it could not trace into `if (_flags ...)`, which is always
false. NFSU2 fixed this class in its translator (a924137).
`regen/jsrf_unresolved_flags.py` checks reachability: 18 are reachable, and
none are in rendering code. Most start mid-function, so their flags come
from before the entry. A full regeneration with a fixed translator would
settle them.

**Callback registers (`8c5b0f2`).** This is a port of upstream xboxrecomp
117a649. APCs, inline DPCs, `NtUserIoApcDispatcher` and
`KeSynchronizeExecution` run guest code on the thread that made the kernel
call, while its caller still uses the register globals. Each now saves and
restores eax..edi and esp. Upstream's case was Darkwatch resuming with an
APC's value in esi and faulting. That is cheap insurance against the open
`FE0000xx` indirect-call cluster. In this tree DPCs run on the timer
thread, not inline, so the `kernel_run_dpc` part does nothing yet; it is
kept as upstream has it. The ADX guard also no longer lets the guest lock
go inside an ISR or DPC (`xbox_gil_in_atomic()`).

**Upstream review (xboxrecomp to v0.15.0, nfsu2-sw to `d3bbc30`).** The
toolkit forked from xboxrecomp before v0.13, at 1409a7d, by way of
nfsu2-sw.
- Found applicable and ported: the four changes above.
- Checked and already fine here:
  - the arrayless-attribute fix (#210; `attr_const` already does it);
  - oPos.w = 0 (#188; GL turns it into w = 1e-6);
  - `fcmov` (lifted as conditional moves);
  - `lock xadd` flags (all 17 branches that follow one read its `_flags`);
  - `sahf` (both sites are data decoded as code: `add eax,[eax]` runs);
  - APC `esp` (#226; jsrf-recomp's restore is in).
- Not ported, because they need a regenerated `gen/` from upstream's
  lifter:
  - the lifter accuracy fixes (#183, #186, #198, #199, MXCSR);
  - function-pointer tables in code gaps (#227);
  - `RECOMP_STUB_LOG`.

  #199 matters for JSRF: its graphics-device wrapper `0x00154D70` came
  back 12 bytes short on the stack, a wild-pointer lead.
- Not ported, too large for one night: upstream's own guest lock
  (`RECOMP_GUEST_LOCK`, from 2111207, extended in #192) and the dispatch
  gate. The NFSU2 lock plus
  `jsrf_preempt.py` covers the same ground with the code already here.
- nfsu2-sw's off-screen culling (66b876d, default on) is not in this
  toolkit. If it is taken later, `RECOMP_CULL=0` is the first thing to try
  when a model goes missing.

### 2026-10-20 — Harness crash: guest read-only pages reused by the heap

The first `linux-run/ab_run.sh` run segfaulted (rc=139) in every variant
without the guest lock, at about frame 2,200–2,700 (about 40 s with Start
pulses).

**Cause.** gdb put the crash in `heap_alloc_locked`'s zero-fill of a
*reused* freed block, `0x0186A000`, 64 KB for a `NtAllocateVirtualMemory`
from `sub_00148261`. The game calls XAPI's `VirtualProtect` wrapper
(`sub_00145A99`, return `0x00145AB1`), which calls `NtProtectVirtualMemory`
on that same block: 288,000 bytes, `PAGE_READONLY`, over and over. On
POSIX that is a real `mprotect`. When the block was freed and reused, the
heap zero-filled read-only pages.

The Switch's `VirtualProtect` is a no-op, so the console never had this
crash; it was a harness-only divergence.

**Fix (`heap_reopen`).** Once any restrictive protection has been seen,
blocks the heap hands out are made read-write first. Restrictive calls are
logged as `[MEM]` lines.

**Validated.** `base`, 180 s: still running at frame 10,623 (before the
fix it died around frame 2,400). 32 `[MEM]` lines, all the call above.

### 2026-10-20 — Boot from the disc image

If `game/default.xbe` is missing, `main.c` looks for an `.iso` next to the
game folder (`SD:/switch/jsrf/*.iso`, or `JSRF_ISO=<path>`). It unpacks the
image once with `src/xiso_extract.c`, an XDVDFS reader.
- The image can be trimmed, or a full-disc dump with the game partition at
  the XGD1, XGD2 or XGD3 offset.
- The loading screen's bar shows real progress; `[iso]` lines go to the log.
- Files go to `game.part` and are renamed to `game` only when complete, so an
  interrupted unpack is redone on the next start.
- `JSRF_ISO_DELETE=1` deletes the image afterwards.
- Everything in the image is treated as untrusted:
  - offsets are bounds-checked;
  - unsafe names (`..`, slashes) are refused;
  - trees are walked with a stack and a visited map;
  - nesting is capped at 32.

Tests (`linux-run/tests/xiso/`):
- `mkxiso.py` builds images; XboxDev's extract-xiso reads them back
  identically.
- Unpacking is byte-identical for trimmed and full-disc layouts under ASan
  and UBSan. Truncated, junk, `..`-named, cyclic and oversized-entry images
  are refused or handled.
- The Linux binary booted from an image alone: first boot, second boot (no
  unpack), a leftover `.part`, an empty `game` folder and
  `JSRF_ISO_DELETE=1`.
- Not yet run on hardware.

### 2026-10-26 — x86 registers in C locals (`regen/jsrf_reg_locals.py`)

The busy streets ran at 30–50 fps at stock clocks. The game's main thread
was at 97% of a core with no hot spot: its time is spread over thousands of
lifted functions. Each of them keeps the x86 registers in thread-locals
(`#define eax g_eax`), so every register read and write is a memory access.
The code is built `-fno-strict-aliasing`, so no register stays in a host
register across a guest store.

xboxrecomp's translator fixes this (`_localize_registers`,
`_localize_x87_stack`), but our generated code predates that, and
regenerating would lose the hand edits. `regen/jsrf_reg_locals.py` does the
same rewrite on the generated C, the way `jsrf_preempt.py` did:
- Each lifted function declares locals that shadow `g_eax` .. `g_esp` by
  name, loaded after `RECOMP_PREEMPT()`. If it uses the x87 stack, it also
  keeps the stack's top index in a local; the stack stays in memory.
- Everything that can run other code or look at the registers stores them
  back first and loads them after: direct, ABI and indirect calls, indirect
  tail calls, `RECOMP_UNIMPL`, the tracers, the debug service and the spin
  hint. Every return and the end of the body store them too. A tail call
  (`sub_X(); return;`) stores and returns, with no reload.
- Indirect calls set esp and eax on their failure paths;
  `RECOMP_ICALL_FAIL_SYNC()` stores those two before the reload.
- `RECOMP_PREEMPT()` is not wrapped. It only yields the guest lock, and
  interrupts and DPCs run on their own threads with their own registers.
- Functions whose code the pass is unsure of are left alone: unknown calls,
  a call that is not a whole statement on one line, setjmp/SEH, address of a
  register. None of the 9,003 functions hit these. 8,943 were rewritten; the
  other 60 name no register. 63,369 calls are wrapped, and 1,756 functions
  use x87.
- The file defines `RECOMP_REG_LOCALS` before its include. `recomp_types.h`
  then leaves those nine names to the locals. Files not rewritten keep the
  fast names, so the header alone changes nothing.

**Register block.** With separate thread-locals, storing and loading seven
registers around a call took about 30 instructions: two adds per variable
for its address. On the Switch, `xbox_memory_layout.c` now lays them out as
one block in assembly: `eax ecx edx ebx esi edi esp fp_top`, then the x87
stack. A function finds them all from one address and stores and loads them
in pairs, four `stp` and four `ldp`. `recomp_fast_tls_selfcheck()` checks
the layout at boot. `-DRECOMP_NO_REG_BLOCK`, in both the runtime and the
generated code, goes back to separate variables.

Measured here, GCC 14 for aarch64 (`-O2`, Switch flags; devkitA64 has
GCC 15):

| | Before | After |
|---|---|---|
| Lifted code (`.text`) | 13.36 MB | 9.54 MB (−29%) |
| Compile time, all chunks | 552 s | 457 s (−17%; noisy, shared machine) |
| Peak compiler memory | up to 1.66 GB | up to 1.34 GB |

Every chunk shrank by 26–31%. So did the functions the busy-street profile
puts on top (`[perf]` profile, 95–141 s): XDK D3D's lazy state
`sub_00196520` 2288 → 1312 bytes, `sub_001993A0` 3664 → 2300,
`sub_00195420` 2476 → 1280, `sub_00190A30` 12272 → 6868; the game's
`sub_000110A0` 4236 → 3664 and `sub_0005BF00` 24204 → 19300. Fewer
instructions is not a frame rate, though. Not yet run on hardware.

**Tests.**
- `regen/test_jsrf_reg_locals.py`: the edge cases, such as conditional
  calls, tail calls, calls in comments and strings, and what the pass
  refuses.
- `regen/difftest/`: every lifted function runs twice, once as it was and
  once rewritten, from the same guest memory and registers. Each call lands
  in a stub that logs the registers it sees, then changes them, the x87 top
  and memory. Stores, MMIO, indirect-call failures and the spin hint are
  logged too. So are the final registers and a hash of all guest memory.
  The two logs must match. The lifter's rep loops and copies are bounded
  at the same step in both versions.
  - Result: all 9,003 functions, 20 runs each (180,060 runs, 3.25 billion
    events), with 0 mismatches. In 6,442 of the 7,722 functions that make
    calls, at least one run reached a call.
  - Removing the esi reload made 101 of 250 functions in one slice
    mismatch. Removing `RECOMP_ICALL_FAIL_SYNC` made 50 mismatch.
  - It is a host build (gcc, x86-64), so it tests the rewrite itself
    rather than the Switch's fast path. `run.sh` takes 10 minutes to build
    and about 30 minutes for 20 runs each.
- The register block's layout was checked by linking with GNU as and ld.
  Code compiled separately reaches the same addresses.

**To apply:** `python3 regen/jsrf_reg_locals.py jsrf-switch/src/recomp/gen
--apply`, then rebuild. The boot log says
`[BOOT] generated code: x86 registers in locals` when the pass has run.
Undo it with git.

**Debugging.** The watchdog and crash reports read the global registers.
Inside a rewritten function, those hold the values from its last call, not
from the current instruction.

### 2026-10-26 — Busy streets after registers in locals: the renderer thread

With registers in locals, light areas hold 60 fps. Busy streets run at
45–55 fps, with about 800 draws a frame. In the 85–125 s window of that run:
- The game thread was about 87% busy, 12% of it waiting in D3D's
  BlockOnFence for the executor.
- The executor/renderer thread was about 77% busy, with 19 ms a frame in
  the walk.
- The GPU took 9.5 ms a frame.

The CPU side is the limit, mainly the renderer thread. Inside
`dk_draw_raw_body`:
- The sampled texture hash took 20% (4% of the thread).
- The index loop took 25%. It was already vectorised; it stalls on stores
  into the uncached data ring behind the vertex copies (`memmove`, 8.5%).

Changes (`RECOMP_DK_*`, see `xboxrecomp/src/nv2a_dk/README.md`):
- **Texture checks.** A texture that passed 8 checks in a row is checked
  every 4th frame, each on its own phase (`RECOMP_DK_TEX_EVERY`).
  Paletted textures, changing ones and ones the executor rewrote are
  still checked every frame.
- **`RECOMP_DK_DATA_CACHED=1`.** Puts the data ring in CPU-cached memory
  and cleans it out before each submit. This is an A/B switch: which is
  faster on the console has to be measured.
- **The `[perf]` line** now shows the data-copy, texture-check and cleaning
  times, and the slow-path pushbuffer words by method (`the rest by
  method`).
- **`switch/prof_report.sh`** looks up all addresses with one addr2line
  run. It used to start one per address, so a `LINES=` report took minutes
  and could be cut off part way.

Runs A and B on the console (same streets, 90–130 s):
- **Texture checks:** 0.25 ms a frame at about 900 draws, down from about
  0.9 ms. About 1,000 checks a frame were put off.
- **Data ring:** uncached cost 0.68 ms per MB; cached and cleaned cost
  0.60 ms per MB, including the cleaning. In those streets the ring takes
  3–4 MB a frame, so cached is now the default (`RECOMP_DK_DATA_CACHED=0`
  for the old way).
- **`the rest by method`:** no method passed 1% of the words. The count
  cost 7% of `nv2a_pb_exec_run` itself, so it is opt-in now
  (`RECOMP_PB_REST=1`).

**Index lists.** In those runs each index was handled four times: unpacked
from the pushbuffer, scanned for the draw's range, rebased into a copy, then
narrowed into the ring. The scan and the rebase were 70% of
`draw_primitive` (7% of the thread). Now:
- The whole-run index paths keep the range as they unpack. Any other
  writer clears `idx_mm`, and then `raw_batch` scans as before.
- A back end registered with `NV2A_BACKEND_RAW_ABS_INDICES` (deko3d) gets
  the indices as given, plus `index_base`, and rebases while it copies
  them into the ring.
- `tests/nv2a_pb_runs` now runs all four combinations: word by word or
  whole runs, rebased or absolute. All give the same digest. It also tests
  a draw that mixes `ARRAY_ELEMENT16` with `DRAW_ARRAYS` or
  `ARRAY_ELEMENT32`. Breaking the tracked high mark fails it, and so does
  leaving `DRAW_ARRAYS` out of the clearing.

### 2026-10-27 — The renderer on a thread of its own (`nv2a_backend_queue.c`)

In the busy streets the executor thread did about 10.6 ms a frame of
pushbuffer walking and 8.3 ms of deko3d recording, one after the other, and
was the limit at stock clocks. Now (default; `RECOMP_DK_THREAD=0` for the old
way):
- The executor still walks the stream, but copies each back-end call into a
  ring. A thread of its own makes the deko3d calls, in order, on another core
  (`[QUEUE] renderer thread on core <n>` in the log).
- **What a record keeps:** the shadow blocks written since the last record,
  the transform program rows (when its version changes) and constant rows
  (the rows written since the batch before), the batch, its indices, the
  converted attributes (packed, as deko3d's ring wants them:
  `NV2A_BACKEND_RAW_PACKED`) and inline vertices (`attr_direct_local`).
  The thread keeps its own shadow, program and constants and hands deko3d
  those.
- **What it does not copy:** vertex arrays, textures and palettes. deko3d
  reads them from guest memory when it gets to the draw. That is safe because
  the executor holds back everything that tells the title the GPU is past a
  draw until the thread has made the calls before it: semaphore releases and
  PATT_COLOR0 are queued behind them (or stored at once when nothing that
  reads guest memory is queued), NOP traps wait for the thread, and the swap
  count moves with the frame's draws. 2D copies wait until the thread is
  idle.
- At most two flips queued; records go over in batches of 16 KB or 16
  records, and at every flip, wait and end of a walk.
- New `[perf] renderer thread:` line: idle time, what the executor waited
  for, queue volume.

`tests/nv2a_backend_queue` walks a generated stream with deko3d-like back
ends directly and queued (slow back end, 64 KB ring, no spinning, one frame
ahead) and hashes everything the back end sees, including the semaphore and
PATT_COLOR0 values at each call. All 18 runs match. Breaking the store
ordering, the 2D-copy wait, the constant rows or the attribute values fails
it. On the PC (2 vCPUs) the queue adds about 0.5 us a draw to the executor:
0.82 -> 1.33 ms for a 900-draw frame with a back end that takes no time.
The executor no longer pays deko3d's ~9 us a draw. Not yet measured on the
console.

### 2026-10-27 — Cheaper calls on the game thread

The game thread is the other half of the limit (about 85% busy in the busy
streets, 12% of that waiting on fences). Two changes cut what calls cost it:

- **Indirect calls remember their target** (`RECOMP_ICALL_LOOKUP`,
  `recomp_types.h`; `recomp_icall_resolve`, `src/recomp_manual.c`). Each of
  the 9,384 indirect call sites has a 64-bit word: the VA it last called and
  that function's offset, written and read whole. A match skips
  `recomp_lookup_manual` and `recomp_lookup` (the 14 MB flat table: 1.9% of
  the game thread, mostly cache misses). Kernel thunks are still looked up
  every time, since that lookup selects the thunk. Tested by
  `regen/icall_cache/run.sh` (four threads, shared sites, changing targets).
- **Small helpers inlined** (`regen/jsrf_inline_leaves.py`, run on the
  generated tree after `jsrf_reg_locals.py`). 370 straight-line leaf
  functions of up to 12 statements -- the x87 max/min `sub_0014C850` and
  `sub_0014C870`, `cvtss2si` `sub_0014C690`, the matrix library's
  `sub_001BAA50` (954 sites), array accessors -- are inlined at 5,418 direct
  call sites. The call's register stores and loads go, the return address
  is still pushed and popped, and `RECOMP_PREEMPT` stays. Code grows about
  4%. Checked by `regen/test_jsrf_inline_leaves.py` and by the difftest: every
  changed caller run against the original, with calls to the inlined
  functions running their real bodies (`DT_REAL`).

### 2026-10-27 — First run with the renderer thread: 57–60 fps; the game held back by its own pushbuffer wait

The first console run with the renderer on its own thread held 57–60 fps in
10-second averages (worst 53.7 at ~740 draws a frame), up from 40–50, with
short dips to ~40. The game thread was then the busiest at 76–95%. In the
whole-run profile, its top function, `sub_00191440` (5.3% self, ~16% with
the kernel waits under it), is D3D's wait for pushbuffer room: it spins
until `*(device+0x34)` -- the semaphore -- passes the space it needs. The
queue held semaphore releases back until the renderer thread had made every
draw before them, so whenever that thread was behind (a present waiting for
the screen), the game waited too.

Now the queue copies each draw's vertex arrays into its record
(`RECOMP_QUEUE_COPY_VERTICES`, default 1). A queued draw then reads only
textures and palettes from guest memory later, so releases, PATT_COLOR0, the
swap count and NOP traps go through at once. The cost is a copy on the
executor, which had time to spare (40–60% busy). `RECOMP_QUEUE_COPY_VERTICES=0`
brings back the first version. `tests/nv2a_backend_queue` runs both: held
back, the progress words must match the direct run's; copied, everything
else must.

`RECOMP_DK_HITCH_MS=<ms>` logs a `[hitch]` line for every frame over that
time (e.g. 22). The profile report's "during the hitches" tables then show
what each thread did in the dips, without FROM/TO. The report's window test
is now a table lookup, so thousands of windows cost nothing.

### 2026-10-27 — The dips are the heaviest scenes; more inlining

With `RECOMP_DK_HITCH_MS=22`, a run logged 429 frames over 22 ms (15 s).
316 of them had 1,000–1,400 draws, against 500–800 in an ordinary busy
frame. They ran about 25 ms each for as long as the scene lasted, and file
loading caused only about 30. In those frames:
- The game thread was flat out, waiting only ~6%.
- The renderer thread took ~12 ms a frame and the executor was ~70% busy.
- The game thread's time went to the game's own XDK D3D (0x0018CB40–0x0019E340,
  ~15–20%), the matrix library (~6%) and game logic spread thin.

`jsrf_inline_leaves.py` now also inlines callees with branches and several
returns (labels renamed per site, returns jump to the block's end), those
that read the inherited frame (`g_seh_ebp`), and up to 24 statements. Four
the profile found hot may have up to 72: 0x001BAB20, 0x001BADB0,
0x0006E130 and 0x00190750. Run on a tree the first version rewrote, it
inlines 735 more functions at 8,077 more sites. Among them are the matrix
stack's 0x001BAAA0 (725 sites) and 0x001BA9C0 (648). The generated code's
text grows from 9.4 to 10.8 MB. The difftest found 0 mismatches in 2,811
functions, 20 runs each. Popping 4 bytes too many on 0x001BAB20's inlined
returns was caught in 112 of its 339 callers within 5 runs.

### 2026-10-27 — The game's floating point in registers

With fix3, a run with `RECOMP_DK_HITCH_MS=22` showed the same picture. In the
slow stretches the game thread was 94% busy and the renderer thread idle half
the time. The slow frames took 22–25 ms. The matrix helpers left the top of the
profile (inlined), but the dips stayed.

What was left in the hot D3D and maths code was mostly bookkeeping:
- **x87 stack in memory.** The lifter keeps the x87 stack in the thread's
  register block. Every fld/fmul/fstp computed `(top + k) & 7`, loaded, did the
  arithmetic and stored back.
- **SSE registers in memory.** Every SSE result was stored to the thread-local
  `g_xmmN`. The 16-byte `RecompXmm` union sat in two general registers, so
  every lane was put together with bit inserts.
- **MMX copy loop.** The index copy in 0x001993A0 took about 70 instructions
  per 64 bytes.

Three passes and a header change:
- **`regen/jsrf_copy_loops.py`.** Gives that MMX loop a front door: when the
  ranges don't overlap, one memcpy, with esi/edi/ecx/mm0–7 and the flag
  snapshots as the last turn leaves them. 1 loop.
- **`regen/jsrf_fp_locals.py`.** Works out the x87 depth at every statement,
  relative to the top at entry, and makes each slot one of eight locals.
  - The slots a path changed are stored before each spill (calls, returns).
  - After each reload, the base moves to the top the callee left.
  - A function is left alone when the depth differs into a label, when the top
    moves under an `if`, or when it has more calls than x87 work.
  - 686 functions, among them 0x00190A30 (a 4×4 inverse), 0x00195420,
    0x0017C3E8, 0x00017CC0 and 0x001560C0.
- **`regen/jsrf_simd_locals.py`.** Gives xmm0–7, mm0–7, `g_fp_cmp` and
  `g_fp_cc` locals, stored by `RECOMP_RL_SPILL` and loaded by
  `RECOMP_RL_RELOAD`, as the general registers already were. 986 functions.
- **`recomp_types.h` (and the runtime's `xbox_memory_layout.h`).**
  `RecompXmm` also holds the lanes as a GCC vector (`RECOMP_XMM_VECTOR`).
  - mulps/addps/shufps/movups become single NEON operations.
  - `-DRECOMP_XMM_NO_VECTOR` gives the old union.
  - `regen/xmm_vector/run.sh` checks the vector helpers against the lane-wise
    ones bit for bit: 3 million checks, NaNs, infinities and denormals included.

With the values in registers the compiler could see through two things the
memory round trips had hidden:
- **Fused multiply-adds.** It fused 182 in one file. SSE and the x87 round
  each step, so the generated code is now built with `-ffp-contract=off`.
- **fld/fstp of a float folded into a copy.** A float loaded to the x87 stack
  and stored again should come back quiet if it was a signalling NaN, as on the
  x87; folded, it came back unchanged. The difftest caught this. GCC folds it
  even with `-fsignaling-nans`, so `jsrf_fp_locals.py` passes every pushed
  value through `recomp_x87_in`, an empty asm the compiler cannot see through.

Instructions per function on the Switch (aarch64 GCC 14, -O2):

| Function | Before | After |
|---|---|---|
| 0x00190A30 | 1,716 | 932 |
| 0x00190750 | 434 | 112 |
| 0x001BAB20 | 177 | 90 |

The difftest now also compares xmm/mm/compare state. Its callees change st(0),
xmm0/1, mm0 and the compare result. `DT_NO_NAN=1` keeps NaNs and infinities
out of the random state. Which NaN an operation returns when both operands
are NaNs is not fixed by C, and a pass that shows the compiler more of the
arithmetic can change it. For the 1,264 changed functions, 20 runs each,
with `DT_NO_NAN=1`: 0 mismatches in 25,280 runs (a call reached in 10,648 of
them, in 647 functions). With NaNs left in, 4 of the 1,264 functions differ,
each only in which NaN comes out. Three of them have two NaN operands. In
0x001A016E the original kept a signalling NaN through fld/fstp: GCC had
forwarded the stored double and folded the conversions there too. The
rewritten code quiets it, as the x87 does.

### 2026-10-27 — 99th Street soft lock: seven callbacks never lifted

After the four police spotlights in 99th Street are sprayed, the gates stay
shut and the player is trapped. The log of that run has 100 lines of
`[ICALL] Failed to resolve VA 0x000E3E80`, one a frame (the log stops at 100),
starting as the last spotlight went down. The same lines are in an earlier run
(3c83230d). `RECOMP_DK_THREAD=0` made no difference: this is not the renderer.

0x000E3E80 is the spotlight's "beaten" handler, a 34-instruction thiscall. It:
- sets 0x2000 in `[this+0x90]`;
- sets state 5 in `[this+0xBC]`;
- calls the sub-object at `+0x44` through its vtable;
- calls 0x0005E7B0, 0x000C5B50 and 0x000C5B10;
- tail-jumps to 0x000C1E60.

It was never lifted. The static initializer at 0x0018BF40 fills a callback
table at 0x00258474 with `mov eax, <function>; mov [table], eax`. jsrf-recomp's
discovery follows calls, jumps, vtables and data pointers, but not an
immediate loaded into a register, so the gap between `sub_000E3E00` and
`sub_000E3F10` was padding to it.

Scanning every lifted function for code addresses loaded as immediates turns
up the same table's other unlifted entries:
- 0x000E9E90
- 0x000EA7A0
- 0x000EA8C0
- 0x000EAA30
- 0x000EAD00
- 0x000EC3E0

These are presumably other enemies' handlers. Three more such addresses are
thread entries passed to the CreateThread wrapper 0x00147F53: 0x00127F70,
0x0012F220 and 0x0017B390. No log has shown them, so they are left for now.
The rest are C++ unwind funclets.

`regen/jsrf_lift_missed.py` lifts the seven:
- It runs jsrf-recomp (4242eb6 plus `jsrf-recomp-local.patch`) on the user's
  own `default.xbe`, with them as extra seeds.
- It checks each function's text against the SHA-256 this was tested with.
- It writes `gen/recomp_missed.c`.
- That file is raw generator output. Registers stay thread-locals, which
  `recomp_types.h` supports for files the passes did not rewrite.
- `recomp_lookup_manual` hands the seven to indirect calls. They are weak, so
  a tree without the file still links and logs `[MISSED]`.

**Capstone 5 renumbered the generic operand types.** `CS_OP_MEM` is 128 in
capstone 5 and 3 in its x86 module. jsrf-recomp compares against the generic
ones, so under capstone 5 every memory operand is lifted as a register named
`r0`. The script runs the tools with the x86 values in their place.

With that fix, 12 neighbouring functions re-lifted the same way are byte for
byte the handover's generated code (d0ae60d). The seven come out identical
whether discovery is seeded with this repository's runtime call database or
only the checked-in seeds.


### 2026-10-31 — Preemption points compiled out; the game thread on a core of its own

No new console data since fix3. The last profile's slow frames had the game
thread busy about 87% of the time, with the rest spread thin over thousands
of lifted functions, so this round looked for costs that every function pays.

**Preemption points.** `regen/jsrf_preempt.py` put `RECOMP_PREEMPT()` at
every function entry and loop back edge, inlined helpers included. It is a
load and a branch to `recomp_preempt()`. It only does anything with
`RECOMP_GIL=1`: nothing sets `g_gil_contended` while the guest lock is off,
and off is the default (`main.c`, `jsrf_env.txt`). Even so, the compiler has
to allow for the call everywhere:
- values live across it go into callee-saved registers, saved in every
  prologue;
- the SSE locals go to the host stack, since only the low halves of v8–v15
  survive a call.

Compiling them out (devkitA64, the Switch flags) gave:

| | With | Without |
|---|---|---|
| Chunk 0 (game code), all functions | 1,532,300 B | 1,333,820 B (−13%) |
| Chunk 8 (XDK D3D), all functions | 1,042,512 B | 991,328 B (−4.9%) |
| `sub_000110A0` (scene walk) | 6,416 | 4,832 (−25%) |
| `sub_00017CC0` | 1,796 | 1,216 (−32%) |
| `sub_00043EC0` | 9,228 | 7,756 (−16%) |
| `sub_00196520` (D3D lazy state) | 1,368 | 1,124 (−18%; saves 2 registers, not 8) |
| `sub_001962B0` | 4,120 | 3,756 (−9%) |
| `sub_001993A0` (indexed draw) | 2,644 | 2,456 (−7%) |

CMake option `JSRF_PREEMPT_POINTS` (default OFF) passes
`-DRECOMP_NO_PREEMPT_POINTS` to the generated code. `recomp_types.h` then
defines `RECOMP_PREEMPT()` as nothing, before each file's own `#ifndef`.
Such a build refuses `RECOMP_GIL=1` at boot. The boot log says
`[BOOT] generated code: no preemption points`. No gen change and no regen:
only a header, `CMakeLists.txt` and `main.c`.

Tried and left out:
- Plain (non-`volatile`) guest memory: −1.5% in chunk 0 and −2% in chunk 8,
  and −1.7% more on top of the above. Too little for the risk to poll loops
  and cross-thread stores.
- `__builtin_expect`/`cold` on the D3D section's MMIO checks: no change to
  the hot functions.

**Thread layout.** `xbox_nx_spread_thread` starts at core 1 (core 0 was the
loader's), and the game's thread is the first to call it, so the game got
core 1. `xbox_nx_place_beside` was written for a game on core 0. It put the
renderer thread on "neither the executor's core nor 0", which is core 1 again
(log: `renderer thread on core 1 (executor on 2)`). Every third other thread
(guest workers, the kernel timer thread) also had core 0, 1 or 2 in turn, all
at priority 59, which Horizon shares in 10 ms slices.

Now (`RECOMP_NX_LAYOUT`, default on; `=0` is the old way):
- The game thread prefers core 0 at host priority 0x3A, one above the
  guest's 59. A 59 thread Horizon moved onto core 0 while the game waited
  gives the core back as soon as the game wakes.
- Everything spread or placed beside the executor keeps to cores 1 and 2.
- All masks still allow cores 0–2, so a thread can still run elsewhere when
  its core is taken.
- The `[perf]` per-thread line now shows `=<%>@<preferred core>/<priority>`.

Neither change has been run on the console yet.


### 2026-11-01 — The boss that never appears: five more functions the lift missed

Daniel's run: the boss fight starts (photo: the corridor arena, HUD up) but
the boss never shows. The log has, from 406 s on, once per frame:

    [ICALL] Failed to resolve VA 0x000E9C30 caller=... (recent targets ... 000C58A0 000EA6A0 000E9C30)

Same cause as 99th Street. The run of static initializers at
0x0018BF40..0x0018C1xx fills the object callback tables at 0x00258474.. with
`mov eax, imm32; mov [table], eax`; 0x0018C000 stores 0x000E9C30 and
0x0018C0C0 stores 0x000EC290. The 0x0018BF40 fix took seven of them and left
these two.

**Survey.** Every instruction in the game's .text (walked from each dispatch
entry to the next) whose immediate points into .text at an address that does
not resolve (dispatch table + `recomp_lookup_manual`) and looks like a
function start (16-aligned, or after a ret/padding). Data dwords checked the
same way. What came out:

| Target | Reached by | Done |
|---|---|---|
| 0x000E9C30, 0x000EC290 | callback-table initializers | lifted |
| 0x00127F70, 0x0012F220, 0x0017B390 | `push imm32` start routine for CreateThread (0x00147F53) | lifted (not started in this run; would fail the same way) |
| ~200 at 0x00186BAB..0x0018ABEB | `push -1; push handler`: C++ EH funclets (`mov eax, FuncInfo; jmp 0x17C00F`) | no: only on a thrown exception |
| 0x0018C950, 0x0018CA10 | `push fn; call 0x17C53D` (atexit) | no: exit only |
| 0x0013F900 | `jmp $` | no |
| 0x00160785, 0x00142078, odd addresses | mid-instruction / data | no |

`regen/jsrf_lift_missed.py` now lifts 12. The first seven come out with the
same SHA-256 as before under capstone 5.0.7 (tested with 5.0.1). The lookup
in `recomp_manual.c` no longer limits the missed list to page 0x000E.

### 2026-11-01 — First run of the 10-31 build (handheld, stock clocks), and per-pass GPU timing

Handheld, system clocks, x1.5: 60 fps in every 10 s window but one (58.4);
game thread 52–70% of a core in the stage. After the stage started, only
single 22–23 ms frames. Left:
- Loading screen (48.9–50.6 s): 77/43/58 ms frames. The game rewrites
  `Partition5\Media\Mark\DEFAULT\JSRF_TEXS0/1.JTX` (1 MB each, O_TRUNC) and the
  write takes 88–122 ms on the SD card, on the game thread. Write-behind would
  need the close deferred too; not done.
- 50.8–51.9 s, the first second of the stage: ~45 frames at 22–26 ms, the
  renderer thread 70% in `frame_begin -> nvFenceWait` (GPU-bound). It starts
  as a 512x512 render target (0x80671000, image 768x768) is first sampled.
- `sub_00191440` (8% self on the game thread) is D3D's wait for pushbuffer
  space: idle time, not work.

`nv2a_dk.c`: a timestamp at each render-target change (`pass_mark` in
`bind_targets`), read back with the frame's. A frame over
`RECOMP_DK_GPU_SLOW_MS` (default 16, 0 = off) logs `[gpu] frame N: X ms, P
passes: <target> <w>x<h> (<image>) <ms> <draws> <binds>; ...`, longest first,
at most 300 lines.

## The runtime rewrite (2026-11-02)

The runtime began as nfsu2-sw's vendored copy of xboxrecomp. Everything in
it that was nfsu2-sw's own work has been rewritten, and code the Switch build
never ran has been removed, each piece tested on the console in a separate
`jsrf-test` install (`JSRF_HOME_NAME=jsrf-test`, `SD:/switch/jsrf-test/`):

- `src/switch_nx.c`, `src/main.c` (the boot), the D3D interrupt wrapper in
  `src/jsrf_manual_overrides.c`, `CMakeLists.txt` and `switch/build.sh`
  (Switch-only now), the input and USB stack (`xinput_nx.c`,
  `xinput_device.c`, `usb_gamepad.c`, `ohci.c`), the audio output
  (`apu_output.c`, replacing `apu_xaudio2.c`), the platform layer's Switch
  parts (`nx_threads.c`, `nx_memory.c`, the handle registry in
  `win32_compat.c`), the kernel (waits, thread suspend, IRQL and the dispatch
  lock, guest dispatcher objects, DPCs, vblank, the contiguous arena, the
  MMIO table, the interrupt ack and executor wake-up) and the pushbuffer
  executor.
- Removed as unreachable here: the OpenGL and Vulkan renderers, the D3D11 /
  GL HLE D3D8 layer (`src/d3d`), the standalone NV2A model (`src/nv2a`), the
  Media Foundation movie player (`src/video`), the Linux harness, the
  executor's CPU vertex-program interpreter, fixed-function transform,
  clipper and software rasteriser (every batch goes to deko3d through
  `draw_raw`), and the toolkit's offline tools, tests and docs.
- The contiguous arena was rewritten with separate used and free lists and
  checked against the old one on 3 million random operations (identical
  addresses and results); the CMP normal decoder and `timegm` likewise.

Settings that no longer exist: `RECOMP_GIL`, `RECOMP_GIL_EAGER` (the guest
lock: guest threads run in parallel, as JSRF always did), `RECOMP_GUEST_ONE_CORE`,
`RECOMP_NX_LAYOUT=0`, `RECOMP_NX_SHM`, `RECOMP_NX_PROCMEM` (code memory is the
only mode), `RECOMP_NX_GUEST_RT`, `RECOMP_AUDIO_DUMP`, `RECOMP_PAD_SCRIPT`,
`RECOMP_FB_WINDOW`, `RECOMP_FB_DUMP`, `RECOMP_FMV_HOST`, `RECOMP_RASTER_TEST`,
`RECOMP_FFP_TRACE`, `RECOMP_SKIP_TRACE`, `RECOMP_TRACE_FLIP`. Renamed:
`RECOMP_GL_DIRECT` is now `RECOMP_DK_DIRECT`, and `JSRF_GL=0` (boot with no
renderer) is now `JSRF_RENDER=0`. `RECOMP_AC97_READY` set to
anything now means the polled mode (`plain`): the write-trap mode needed
fault handling the Switch does not have.

The "Linux repro harness" section above describes a harness this tree no
longer has.
