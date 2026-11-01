"""Keep the SSE and MMX registers, and the x87 compare result, in C locals.

jsrf_reg_locals.py put the general registers of each lifted function in
locals; xmm0 .. xmm7, mm0 .. mm7 and the x87 compare state (g_fp_cmp,
g_fp_cc) stayed thread-locals. Every SSE instruction of JSRF's matrix
library and D3D code stored its 16-byte result to the thread's memory, and
every guest store in between made the compiler load them all again, since it
cannot tell a guest pointer from them.

This pass gives each function that names them a local copy, x_xmm0 and so on,
taken at entry; RECOMP_RL_SPILL (before every call, return and tail call)
stores them back, and RECOMP_RL_RELOAD (after every call) loads them again,
the way the general registers already are. The macros xmm0 .. are rewritten
to the locals by name inside the function only. The thread's state when the
function calls out or returns is what it was before.

A function is left as it was when it was not rewritten by jsrf_reg_locals.py
(no RECOMP_RL_SPILL of its own), or when its calls outnumber its uses of
these registers (the stores and loads around each call would cost more).

  python3 regen/jsrf_simd_locals.py <gen dir>            # report only
  python3 regen/jsrf_simd_locals.py <gen dir> --apply    # rewrite in place

Idempotent: a rewritten function is marked and not touched again. Runs after
jsrf_inline_leaves.py (an inlined callee's uses are then the caller's).
"""
import pathlib
import re
import sys

FUNC = re.compile(r"^void (sub_[0-9A-F]{8})\(void\)$")
LOCALS = re.compile(r"/\* registers in locals \*/\s*$")
MARK = "/* SIMD registers in locals */"
SPILL_DEF = "#define RECOMP_RL_SPILL "
RELOAD_DEF = "#define RECOMP_RL_RELOAD "
# name in the generated code -> (type, thread-local)
STATE = {**{f"xmm{i}": ("RecompXmm", f"g_xmm{i}") for i in range(8)},
         **{f"mm{i}": ("RecompMmx", f"g_mm{i}") for i in range(8)},
         "g_fp_cmp": ("int", "g_fp_cmp"),
         "g_fp_cc": ("uint16_t", "g_fp_cc")}
NAME = re.compile(r"\b(xmm[0-7]|mm[0-7]|g_fp_cmp|g_fp_cc)\b")
CALL = re.compile(r"\bRECOMP_RL_SPILL\b")


class Skip(Exception):
    pass


def rewrite(lines, a, b):
    """lines[a] '{' .. lines[b] '}': the new body lines, or Skip."""
    loc = spill = reload = None
    for k in range(a + 1, b):
        s = lines[k]
        if MARK in s:
            raise Skip("done already")
        if loc is None and LOCALS.search(s):
            loc = k
        elif s.startswith(SPILL_DEF) and spill is None:
            spill = k
        elif s.startswith(RELOAD_DEF) and reload is None:
            reload = k
    if loc is None or spill is None or reload is None:
        raise Skip("not rewritten by jsrf_reg_locals.py")
    used, uses, calls = set(), 0, 0
    for k in range(a + 1, b):
        s = lines[k]
        if s.lstrip().startswith("#"):
            continue
        code = re.sub(r"/\*.*?\*/", "", s)
        found = NAME.findall(code)
        used.update(found)
        uses += len(found)
        calls += len(CALL.findall(code))
    if not used:
        raise Skip("names none of them")
    # The end-of-function spill and one per return are always there.
    if uses < (calls - 1) * len(used):
        raise Skip(f"more calls than uses ({calls} calls, {uses} uses of {len(used)})")
    order = sorted(used, key=lambda n: list(STATE).index(n))
    local = {n: "x_" + n.replace("g_", "") for n in order}
    decl = " ".join(f"{STATE[n][0]} {local[n]} = {STATE[n][1]};" for n in order)
    st = " ".join(f"{STATE[n][1]} = {local[n]};" for n in order)
    ld = " ".join(f"{local[n]} = {STATE[n][1]};" for n in order)
    out = list(lines[a:b + 1])
    rel = lambda k: k - a
    out[rel(loc)] = lines[loc] + " " + decl + " " + MARK
    out[rel(spill)] = lines[spill].rstrip() + " " + st
    out[rel(reload)] = lines[reload].rstrip() + " " + ld
    for k in range(a + 1, b):
        if k in (loc, spill, reload) or lines[k].lstrip().startswith("#"):
            continue
        out[rel(k)] = NAME.sub(lambda m: local[m.group(1)], lines[k])
    return out, order


def main():
    args = [x for x in sys.argv[1:] if not x.startswith("--")]
    if len(args) != 1:
        print(__doc__)
        sys.exit(2)
    gen = pathlib.Path(args[0])
    files = sorted(gen.glob("recomp_0*.c")) + [gen / "recomp_split.c"]
    done, left, per = [], {}, {}
    nfiles = 0
    for f in files:
        if not f.exists():
            continue
        lines = f.read_text().split("\n")
        out = []
        i = 0
        changed = False
        while i < len(lines):
            m = FUNC.match(lines[i])
            if m and i + 1 < len(lines) and lines[i + 1] == "{":
                j = i + 2
                while lines[j] != "}":
                    j += 1
                try:
                    new, order = rewrite(lines, i + 1, j)
                    out.append(lines[i])
                    out.extend(new)
                    done.append(m.group(1))
                    for n in order:
                        per[n] = per.get(n, 0) + 1
                    changed = True
                except Skip as e:
                    if "names none" not in str(e) and "not rewritten" not in str(e):
                        left[m.group(1)] = str(e)
                    out.extend(lines[i:j + 1])
                i = j + 1
                continue
            out.append(lines[i])
            i += 1
        if changed:
            nfiles += 1
            if "--apply" in sys.argv:
                f.write_text("\n".join(out))
    print(f"{len(done)} functions with SIMD registers in locals, in {nfiles} files; "
          f"{len(left)} left (more calls than uses)")
    print("  " + ", ".join(f"{n} {c}" for n, c in sorted(per.items(), key=lambda x: -x[1])))
    if "--why" in sys.argv:
        for n, w in left.items():
            print(n, w)
    if "--list" in sys.argv:
        for n in done:
            print(n)
    if "--apply" in sys.argv:
        print("applied")


if __name__ == "__main__":
    main()
