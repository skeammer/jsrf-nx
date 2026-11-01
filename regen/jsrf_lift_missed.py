"""Lift guest functions the generated code is missing, into gen/recomp_missed.c.

A static initializer at 0x0018BF40 fills a table of object callbacks at
0x00258474 with `mov eax, <function>; mov [table], eax`. jsrf-recomp's
discovery follows calls, jumps, vtables and data pointers, not immediates
loaded into a register, so seven of those callbacks were never lifted. The
first of them is 99th Street's "spotlight beaten" handler, 0x000E3E80: once
the last police spotlight is sprayed the game calls it every frame, the call
fails to resolve ([ICALL] Failed to resolve VA 0x000E3E80 in the log), and
the gates never open. The other six are the same table's handlers for other
enemies.

A survey of every immediate in the game's code (2026-11-01) found five more:
0x000E9C30 and 0x000EC290 from the neighbouring initializers at 0x0018C000
and 0x0018C0C0 (0x000E9C30 is called every frame once a boss fight starts;
without it the boss never appears), and 0x00127F70, 0x0012F220 and
0x0017B390, start routines pushed for CreateThread (0x00147F53). The rest of
the survey's hits are C++ unwind funclets, atexit destructors and data.

This runs jsrf-recomp (commit 4242eb6 with regen/jsrf-recomp-local.patch) on
your own default.xbe, with those seven addresses as extra seeds, lifts only
them, checks each against the text this repository was tested with (SHA-256
below), and writes them to <gen dir>/recomp_missed.c. CMake builds every .c
in the gen directory; src/recomp_manual.c hands the functions to indirect
calls. Nothing else in the gen directory changes. The file is generated code
from the game's executable, so it stays out of the repository like the rest.

  python3 regen/jsrf_lift_missed.py --recomp <jsrf-recomp dir> --xbe <default.xbe> <gen dir>

Needs Python 3 with capstone 5.0.1 (pip install capstone==5.0.1). A few minutes.

capstone 5 renumbered its generic operand types (CS_OP_MEM is 128 there, 3 in
the x86 module and in capstone 4), and jsrf-recomp compares against the
generic ones: under capstone 5 every memory operand comes out as a register
named "r0". The lift runs with the x86 module's values put in their place.
"""
import argparse
import hashlib
import io
import json
import os
import pathlib
import re
import runpy
import sys
import tempfile
from contextlib import redirect_stdout

MISSED = [0x000E3E80, 0x000E9E90, 0x000EA7A0, 0x000EA8C0, 0x000EAA30, 0x000EAD00, 0x000EC3E0,
          0x000E9C30, 0x000EC290, 0x00127F70, 0x0012F220, 0x0017B390]
WHY = ("a static initializer (0x0018BF40..0x0018C1xx) stores it in a callback table "
       "(mov eax, imm32; mov [table], eax), or a CreateThread call pushes it as the "
       "start routine (push imm32); nothing else reaches it")
# The generated text each function was tested with (header comment to closing
# brace, as tools.recomp prints it).
EXPECTED = {}   # filled in below
XBE_SHA = "fd19055756719893c466"   # first 20 hex digits of the US default.xbe's SHA-256

PREAMBLE = """/**
 * Jet Set Radio Future - guest functions the generator missed, lifted by
 * regen/jsrf_lift_missed.py from this machine's default.xbe. Do not edit;
 * run the script again instead.
 *
 * A static initializer (0x0018BF40) stores these in a callback table with
 * `mov eax, imm32`, which discovery does not follow, so they were never
 * lifted and an indirect call to one failed to resolve. 0x000E3E80 is the
 * 99th Street spotlight-beaten handler: without it the gates stay shut.
 * 0x000E9C30 is called every frame in a boss fight: without it the boss
 * never appears. Three more are CreateThread start routines.
 * src/recomp_manual.c hands them to indirect calls.
 *
 * Raw generator output: registers stay thread-locals here (no
 * RECOMP_REG_LOCALS), which recomp_types.h supports for any file the regen
 * passes did not rewrite.
 */

#define RECOMP_GENERATED_CODE
#include "recomp_funcs.h"

#ifndef RECOMP_PREEMPT
extern volatile int g_gil_contended;
void recomp_preempt(void);
#define RECOMP_PREEMPT() do { if (__builtin_expect(g_gil_contended, 0)) recomp_preempt(); } while (0)
#endif
#include <math.h>

"""


def fix_capstone():
    import capstone
    import capstone.x86 as x86
    capstone.CS_OP_REG = x86.X86_OP_REG
    capstone.CS_OP_IMM = x86.X86_OP_IMM
    capstone.CS_OP_MEM = x86.X86_OP_MEM


def run_tool(module, argv):
    """python -m <module> <argv>, in this process (so the capstone fix holds)."""
    old = sys.argv
    sys.argv = [module] + argv
    try:
        runpy.run_module(module, run_name="__main__", alter_sys=True)
    except SystemExit as e:
        if e.code not in (0, None):
            raise RuntimeError(f"{module} exited with {e.code}")
    finally:
        sys.argv = old


def extract(text, va):
    """The function's text: its /** header through the closing brace."""
    name = f"sub_{va:08X}"
    m = re.search(r"/\*\*\n \* " + name + r"\n.*?\nvoid " + name + r"\(void\)\n\{\n.*?\n\}\n",
                  text, re.S)
    if not m:
        raise RuntimeError(f"{name}: not in tools.recomp's output")
    return m.group(0)


def lift(recomp_dir, xbe, work):
    os.chdir(recomp_dir)
    sys.path.insert(0, str(recomp_dir))
    fix_capstone()
    seeds = work / "missed_seeds.json"
    seeds.write_text(json.dumps([{"start": f"0x{a:08X}", "source": "callback table",
                                  "why": WHY} for a in MISSED], indent=2))
    analysis = work / "jsrf_analysis.json"
    print("[lift] analysing the XBE")
    with redirect_stdout(io.StringIO()):
        run_tool("tools.xbe_parser", [str(xbe), "--json", str(analysis), "--quiet"])
    d = "diagnostics/jsrf_first_fault"
    print("[lift] finding functions (with the missed ones as seeds)")
    with redirect_stdout(io.StringIO()):
        run_tool("tools.disasm", [str(xbe), "--analysis-json", str(analysis),
                                  "--output", str(work / "disasm"),
                                  "--seed-functions", f"{d}/thread_start_seed.json",
                                  "--seed-functions", f"{d}/icall_seed.json",
                                  "--seed-functions", f"{d}/startup_entries.json",
                                  "--seed-functions", f"{d}/missed_prologue_seed.json",
                                  "--seed-functions", str(seeds),
                                  "--function-bounds", f"{d}/function_bounds.json",
                                  "--force"])
    print("[lift] classifying")
    with redirect_stdout(io.StringIO()):
        run_tool("tools.func_id", [str(xbe), "--functions", str(work / "disasm/functions.json"),
                                   "--strings", str(work / "disasm/strings.json"),
                                   "--xrefs", str(work / "disasm/xrefs.json"),
                                   "--output", str(work / "func-id")])
    out = {}
    for va in MISSED:
        print(f"[lift] sub_{va:08X}")
        buf = io.StringIO()
        with redirect_stdout(buf):
            run_tool("tools.recomp", [str(xbe), "-f", f"0x{va:08X}",
                                      "--functions", str(work / "disasm/functions.json"),
                                      "--labels", str(work / "disasm/labels.json"),
                                      "--func-id-dir", str(work / "func-id"),
                                      "-o", str(work / "recomp"),
                                      "--exclude-manual", f"{d}/jsrf_manual_overrides.c"])
        out[va] = extract(buf.getvalue(), va)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("gen", type=pathlib.Path)
    ap.add_argument("--recomp", type=pathlib.Path, required=True,
                    help="jsrf-recomp checkout (4242eb6 + regen/jsrf-recomp-local.patch)")
    ap.add_argument("--xbe", type=pathlib.Path, required=True)
    ap.add_argument("--print", action="store_true",
                    help="print each function's SHA-256 instead of checking it")
    a = ap.parse_args()
    gen, recomp, xbe = a.gen.resolve(), a.recomp.resolve(), a.xbe.resolve()
    if not (gen / "recomp_funcs.h").exists():
        sys.exit(f"{gen}: no recomp_funcs.h (give the gen directory)")
    if not (recomp / "tools/recomp/__main__.py").exists():
        sys.exit(f"{recomp}: not a jsrf-recomp checkout")
    sha = hashlib.sha256(xbe.read_bytes()).hexdigest()
    if not sha.startswith(XBE_SHA):
        sys.exit(f"{xbe}: SHA-256 {sha[:20]}..., not the US default.xbe this was tested with "
                 f"({XBE_SHA}...)")
    with tempfile.TemporaryDirectory() as t:
        funcs = lift(recomp, xbe, pathlib.Path(t))
    bad = []
    for va, text in funcs.items():
        h = hashlib.sha256(text.encode()).hexdigest()
        if a.print:
            print(f"    0x{va:08X}: \"{h}\",")
        elif EXPECTED.get(va) != h:
            bad.append(va)
    if a.print:
        return
    if bad:
        import capstone
        sys.exit("lifted text differs from what was tested for "
                 + ", ".join(f"sub_{v:08X}" for v in bad)
                 + f" -- nothing written. Tested with capstone 5.0.1 (this is "
                 f"{capstone.__version__}; pip install capstone==5.0.1) and jsrf-recomp "
                 "4242eb6 with regen/jsrf-recomp-local.patch applied")
    # Every function they call must exist in this tree.
    decl = set(re.findall(r"void (sub_[0-9A-F]{8})\(void\);", (gen / "recomp_funcs.h").read_text()))
    own = {f"sub_{v:08X}" for v in MISSED}
    body = "".join(funcs.values())
    missing = sorted(set(re.findall(r"\b(sub_[0-9A-F]{8})\(\)", body)
                         + re.findall(r"RECOMP_ABI_CALL\(0x[0-9A-F]{8}u, (sub_[0-9A-F]{8})\)", body))
                     - decl - own)
    if missing:
        sys.exit("they call functions this tree lacks: " + ", ".join(missing) + "; nothing written")
    decls = "".join(f"void sub_{v:08X}(void);\n" for v in MISSED)
    (gen / "recomp_missed.c").write_text(PREAMBLE + decls + "\n" + "\n".join(funcs[v] for v in MISSED))
    print(f"wrote {gen / 'recomp_missed.c'}: {len(MISSED)} functions")


EXPECTED.update({
    0x000E3E80: "603b62a8c120a31a7b3a4365e223f8b26e6219614c9fe7c3e6eda7c99fa3ed34",
    0x000E9E90: "d838d8026a406c184672ca76ee8ad27baba2ea364e49fff8c8bad8e5e708c1fa",
    0x000EA7A0: "4f15fe55db337045e43300b4fcc1385dd19b8d8809a81a79e0a39605814b98ba",
    0x000EA8C0: "284704aec84d562cace44e5fb585ed7fb85b8dfd0379e0f7f105689a0e595ff9",
    0x000EAA30: "5877fca8531ad7d4d317bf1034cbc880dc69d5726b5adae1003c9fdb666b92d7",
    0x000EAD00: "268477414ed24f5ade8e741a5403ee3a788b7e0cf40e1693aeaed6fb4d12b517",
    0x000EC3E0: "2ca889af51354d7f1968e1cfc05ca82bd404df3aa3db40fd372e612eb805a2d1",
    0x000E9C30: "9125c347d0c38ee06894dbd00b0fe444196bfe5b3e80a0413ebf7c5ca9375a1f",
    0x000EC290: "232daf2de23445dc62fc3b6c11d75b72d1c8c662e9e1e83df5fffcf46bc76a32",
    0x00127F70: "92d5413ebc913a6065c9e13ae80118c1f04b59968becb2076f2fc917bd598408",
    0x0012F220: "09cb035abd5c95099b03f46ee9dd4ad57ed762514a0549e1c7a92be73c3c4533",
    0x0017B390: "ccfd1c5251f0d715756c47e2c6d7e2991577a6af8d0aab56c6b38cddf3183d73",
})

if __name__ == "__main__":
    main()
