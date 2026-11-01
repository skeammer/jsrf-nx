"""Give the lifted JSRF code the guest lock's preemption points.

NFSU2's translator puts RECOMP_PREEMPT() at the entry of every lifted
function, and xboxrecomp's --back-edge-yield (v0.15) puts one before every
loop back edge. JSRF's generated code came from jsrf-recomp's lifter, which
emits neither, so with RECOMP_GIL=1 a thread that spins on another guest
thread's flag never lets that thread run: the deadlock behind RECOMP_GIL=0.

This pass adds both, on the generated C text, so the hand edits in gen/
survive (no regeneration):

  - entry:      RECOMP_PREEMPT(); as the first statement of each lifted
                function (`void <name>(void)` followed by `{`);
  - back edges: every `goto loc_X;` whose label appears earlier in the same
                function becomes `{ RECOMP_PREEMPT(); goto loc_X; }`. Every
                cycle in a function has at least one such edge, because a
                fall-through only runs forward in the text.

Edges the spin-hint pass already marked (RECOMP_SPIN_HINT, which yields every
64 turns) are left alone.

RECOMP_PREEMPT() is the toolkit's: one load of g_gil_contended, which only the
guest lock ever sets, so with RECOMP_GIL=0 it is a branch never taken.
recomp_preempt() (kernel_bridge.c) does nothing in ISRs, DPCs or code at
DISPATCH. The macro block is pasted after the recomp_funcs.h include, guarded,
so a definition already made by recomp_types.h (included through it) wins.

  python3 regen/jsrf_preempt.py jsrf-switch/src/recomp/gen           # count
  python3 regen/jsrf_preempt.py jsrf-switch/src/recomp/gen --apply   # rewrite
  python3 regen/jsrf_preempt.py jsrf-switch/src/recomp/gen --entry-only --apply

Idempotent: a file that already has the marker is skipped.
"""
import pathlib
import re
import sys

MARKER = "/* jsrf_preempt: RECOMP_PREEMPT"

BLOCK = MARKER + """ at function entries and loop back edges
 * (regen/jsrf_preempt.py). With RECOMP_GIL=1 a holder yields the guest lock
 * here once another guest thread has waited for it; otherwise this is one
 * load and a branch not taken. */
#ifndef RECOMP_PREEMPT
extern volatile int g_gil_contended;
void recomp_preempt(void);
#if defined(_MSC_VER)
#define RECOMP_PREEMPT() do { if (g_gil_contended) recomp_preempt(); } while (0)
#else
#define RECOMP_PREEMPT() do { if (__builtin_expect(g_gil_contended, 0)) recomp_preempt(); } while (0)
#endif
#endif
"""

INCLUDE = '#include "recomp_funcs.h"\n'
FUNC = re.compile(r"^void [A-Za-z_][A-Za-z0-9_]*\(void\)$")
LABEL = re.compile(r"^(loc_[0-9A-Fa-f]{8}): ;")
# A goto is one statement and `{ ...; goto L; }` is one statement too, so the
# swap is valid wherever the goto stands (after an if, a label or a `;`).
GOTO = re.compile(r"\bgoto (loc_[0-9A-Fa-f]{8});")


def rewrite(src, back_edges=True):
    """Return (new_src, functions, back_edges_marked)."""
    if MARKER in src:
        return src, 0, 0
    if INCLUDE not in src:
        raise ValueError("no recomp_funcs.h include to put the macro after")
    lines = src.split("\n")
    out = []
    nfunc = nedge = 0
    in_func = False
    seen = set()
    i = 0
    while i < len(lines):
        line = lines[i]
        if not in_func:
            out.append(line)
            if FUNC.match(line) and i + 1 < len(lines) and lines[i + 1] == "{":
                out.append("{")
                out.append("    RECOMP_PREEMPT();")
                nfunc += 1
                in_func = True
                seen = set()
                i += 2
                continue
            i += 1
            continue
        if line == "}":
            in_func = False
            out.append(line)
            i += 1
            continue
        m = LABEL.match(line)
        if m:
            seen.add(m.group(1))
        elif back_edges and "goto loc_" in line and "RECOMP_SPIN_HINT" not in line:
            def mark(g):
                nonlocal nedge
                if g.group(1) not in seen:
                    return g.group(0)
                nedge += 1
                return "{ RECOMP_PREEMPT(); goto %s; }" % g.group(1)
            line = GOTO.sub(mark, line)
        out.append(line)
        i += 1
    if in_func:
        raise ValueError("unterminated function at end of file")
    text = "\n".join(out)
    text = text.replace(INCLUDE, INCLUDE + "\n" + BLOCK, 1)
    return text, nfunc, nedge


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    gen = pathlib.Path(argv[1])
    apply = "--apply" in argv
    back = "--entry-only" not in argv
    files = sorted(gen.glob("recomp_0*.c")) + [gen / "recomp_stubs_unresolved.c"]
    tf = te = 0
    for p in files:
        if not p.exists():
            continue
        src = p.read_text()
        new, nf, ne = rewrite(src, back)
        tf += nf
        te += ne
        state = "already marked" if MARKER in src else f"{nf} entries, {ne} back edges"
        print(f"{p.name}: {state}")
        if apply and new != src:
            p.write_text(new)
    print(f"total: {tf} entries, {te} back edges" + ("" if apply else " (dry run)"))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
