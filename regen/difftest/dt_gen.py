"""Build the differential test: every lifted function as it was (NAME__o)
and as a pass rewrote it (NAME__n), in TUs of SLICE functions, plus a stub
for every declared function and the tables dt_rt.c walks.

  python3 dt_gen.py <orig gen dir> <new gen dir> <out dir> [slice]   (run.sh does this)

DT_ONLY=<file>: only the functions named in it (one a line) are built and
run. DT_REAL=<file>: calls to the functions named in it run their original
body (NAME__o) instead of the logging stub -- for jsrf_inline_leaves.py,
whose rewritten callers do that work themselves; those functions are built
too.
"""
import os
import pathlib
import re
import sys


def names_from(var):
    p = os.environ.get(var)
    return set(pathlib.Path(p).read_text().split()) if p else None

FUNC = re.compile(r"^void ([A-Za-z_][A-Za-z0-9_]*)\(void\)$")
DECL = re.compile(r"^void ([A-Za-z_][A-Za-z0-9_]*)\(void\);", re.M)

PRE = """#define RECOMP_GENERATED_CODE
#define RECOMP_REG_LOCALS 1
#include "recomp_funcs.h"
void dt_preempt(void);
void dt_spin_hint(void);
#undef RECOMP_PREEMPT
#define RECOMP_PREEMPT() dt_preempt()
/* jsrf_copy_loops.py's one memcpy for a loop that was a preemption point
 * every turn but the last: the same count, and taken only where the loop
 * would not have run out of them first. */
int dt_preempt_room(unsigned long n);
void dt_preempt_n(unsigned long n);
#undef RECOMP_BLOCK_COPY_ENTER
#undef RECOMP_BLOCK_COPY_DONE
#define RECOMP_BLOCK_COPY_ENTER(turns) dt_preempt_room((turns) - 1)
#define RECOMP_BLOCK_COPY_DONE(turns) dt_preempt_n((turns) - 1)
#define RECOMP_SPIN_HINT() dt_spin_hint()
#include <math.h>
#include <stdio.h>
#include <string.h>
struct dt_fn { const char *name; void (*o)(void); void (*n)(void); };
/* The lifter's rep-prefix loops and copies run as long as ecx says, and a
 * random ecx can say four billion: both versions count the same steps and
 * stop at the same one. */
int dt_step(void);
void *dt_memset(void *d, int c, size_t n);
void *dt_memcpy(void *d, const void *s, size_t n);
#define memset(d, c, n) dt_memset((d), (c), (n))
#define memcpy(d, s, n) dt_memcpy((d), (s), (n))
"""
# The same instrumentation in both versions of a function.
LOOPS = [
    (re.compile(r"for \((\w+) = 0; \1 < (ecx|_n); \1\+\+\)"),
     lambda m: f"for ({m.group(1)} = 0; {m.group(1)} < {m.group(2)} && dt_step(); {m.group(1)}++)"),
    (re.compile(r"while \(ecx != 0\)"), lambda m: "while (ecx != 0 && dt_step())"),
]


def instrument(lines):
    out = []
    for line in lines:
        for rx, fn in LOOPS:
            line = rx.sub(fn, line)
        out.append(line)
    return out


def functions(path):
    lines = path.read_text().split("\n")
    out = {}
    order = []
    i = 0
    while i < len(lines):
        m = FUNC.match(lines[i])
        if m and i + 1 < len(lines) and lines[i + 1] == "{":
            j = i + 2
            while lines[j] != "}":
                j += 1
            name = m.group(1)
            assert name not in out, name
            out[name] = lines[i + 1:j + 1]
            order.append(name)
            i = j + 1
            continue
        i += 1
    return out, order


def main():
    orig, new, outd = map(pathlib.Path, sys.argv[1:4])
    slice_n = int(sys.argv[4]) if len(sys.argv) > 4 else 250
    outd.mkdir(parents=True, exist_ok=True)
    files = sorted(orig.glob("recomp_0*.c")) + [orig / "recomp_split.c",
                                                 orig / "recomp_stubs_unresolved.c"]
    tables = []          # (tag, number of functions)
    only, real = names_from("DT_ONLY"), names_from("DT_REAL") or set()
    for f in files:
        fo, order = functions(f)
        fn, order_n = functions(new / f.name)
        assert order == order_n, f.name
        if only is not None:
            order = [nm for nm in order if nm in only or nm in real]
        for k in range(0, len(order), slice_n):
            names = order[k:k + slice_n]
            tag = f"{f.stem}_{k // slice_n}"
            parts = [PRE]
            for nm in names:
                parts.append(f"void {nm}__o(void)")
                parts.extend(instrument(fo[nm]))
                parts.append(f"void {nm}__n(void)")
                parts.extend(instrument(fn[nm]))
            parts.append(f"const struct dt_fn dt_t_{tag}[] = {{")
            for nm in names:
                parts.append(f'    {{"{nm}", {nm}__o, {nm}__n}},')
            parts.append("};")
            (outd / f"t_{tag}.c").write_text("\n".join(parts) + "\n")
            tables.append((tag, len(names)))
    decls = sorted(set(DECL.findall((new / "recomp_funcs.h").read_text())))
    st = ['#include <stdint.h>', 'void dt_callee(uint32_t id, uint32_t kind);']
    for i, d in enumerate(decls):
        m = re.match(r"sub_([0-9A-Fa-f]{8})$", d)
        ident = int(m.group(1), 16) if m else 0xF0000000 + i
        if d in real:
            st.append(f"void {d}__o(void); void {d}(void) {{ {d}__o(); }}")
        else:
            st.append(f"void {d}(void) {{ dt_callee(0x{ident:08X}u, 1); }}")
    (outd / "stubs.c").write_text("\n".join(st) + "\n")
    tb = ["struct dt_fn { const char *name; void (*o)(void); void (*n)(void); };"]
    tb += [f"extern const struct dt_fn dt_t_{t}[];" for t, _ in tables]
    tb.append("const struct dt_fn *const dt_tables[] = {"
              + ", ".join(f"dt_t_{t}" for t, _ in tables) + "};")
    tb.append("const int dt_table_sizes[] = {" + ", ".join(str(n) for _, n in tables) + "};")
    tb.append(f"const int dt_ntables = {len(tables)};")
    (outd / "tables.c").write_text("\n".join(tb) + "\n")
    sizes = [n for _, n in tables]
    print(f"{len(tables)} TUs, {sum(sizes)} functions, {len(decls)} stubs")


if __name__ == "__main__":
    main()
