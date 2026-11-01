"""Turn the D3D library's MMX block-copy loop into one memcpy.

JSRF's hottest D3D function in its busiest scenes, 0x001993A0 (an indexed
draw: it copies the index list into the push buffer), copies with the
classic MMX loop -- eight movq loads, eight movntq stores, 64 bytes a turn,
ecx turns. Lifted, each turn is eight loads, eight stores of the MMX
registers to thread-local memory, eight loads back and eight stores: about 70
instructions for 64 bytes, on 400 KB of indices a frame.

The loop gets a front door: when the two ranges do not overlap, the whole
copy is one memcpy, and esi, edi, ecx, mm0 .. mm7 and the flag snapshots end
as the last turn would have left them (mm0 .. mm7 hold the last 64 bytes
read). Otherwise -- overlap, or ecx 0 (four billion turns) -- the loop runs
as it was. The loop was a preemption point every turn; the copy is one
(RECOMP_BLOCK_COPY_DONE, recomp_types.h; regen/difftest counts the turns).

  python3 regen/jsrf_copy_loops.py <gen dir>            # report only
  python3 regen/jsrf_copy_loops.py <gen dir> --apply    # rewrite in place

Run it before jsrf_simd_locals.py (which renames mm0 .. to locals).
Idempotent: a loop given its front door is marked.
"""
import pathlib
import re
import sys

MARK = "/* block copy as one memcpy */"
def off(k):
    """The lifter's spelling of esi + 8k: decimal below 10, hex above."""
    return "" if k == 0 else f" + {8 * k}" if 8 * k < 10 else f" + 0x{8 * k:X}"


HEAD = re.compile(r"^(?P<ind>\s*)(?P<label>loc_[0-9A-F]{8}): ;$")
BODY = [
    r"\(void\)0; /\* prefetchnta: cache/ordering hint, nothing to model \*/",
    r"\(void\)0; /\* prefetchnta: cache/ordering hint, nothing to model \*/",
] + [re.escape(f"mm{k} = MMX_MEM(esi{off(k)}); /* movq */") for k in range(8)] + [
    r"/\* nop \*/", r"/\* nop \*/",
] + [re.escape(f"MMX_STORE(edi{off(k)}, mm{k}); /* movntq */") for k in range(8)] + [
    r"_fb = \(uint32_t\)\(0x40\) & 0xFFFFFFFFu; _fbs = \(int32_t\)\(int32_t\)\(_fb\); /\* add source snapshot, before the write \*/",
    r"esi = esi \+ 0x40;",
    r"_fa = \(uint32_t\)\(esi\) & 0xFFFFFFFFu; _fas = \(int32_t\)\(int32_t\)\(_fa\); /\* add result snapshot \*/",
    r"_fb = \(uint32_t\)\(0x40\) & 0xFFFFFFFFu; _fbs = \(int32_t\)\(int32_t\)\(_fb\); /\* add source snapshot, before the write \*/",
    r"edi = edi \+ 0x40;",
    r"_fa = \(uint32_t\)\(edi\) & 0xFFFFFFFFu; _fas = \(int32_t\)\(int32_t\)\(_fa\); /\* add result snapshot \*/",
    r"ecx--;",
    r"_fa = \(uint32_t\)\(ecx\) & 0xFFFFFFFFu; _fas = \(int32_t\)\(int32_t\)_fa; /\* dec result snapshot; CF unchanged \*/",
    r"if \(\(_fa != 0\)\) \{ RECOMP_PREEMPT\(\); goto (?P<back>loc_[0-9A-F]{8}); \} /\* jne: not equal / not zero \*/",
]
BODY = [re.compile("^" + b + "$") for b in BODY]
AFTER = re.compile(r"^\s*(?P<label>loc_[0-9A-F]{8}): ;$")


def front_door(ind, after):
    mm = " ".join(f"mm{k} = MMX_MEM(esi - 0x{64 - 8 * k:X});" for k in range(8))
    # (memcpy) is the C library's even where a harness takes the name over.
    return (f"{ind}if (ecx != 0 && ecx < 0x01000000u && RECOMP_BLOCK_COPY_ENTER(ecx)) {{ {MARK}\n"
            f"{ind}    uint32_t _bn = ecx * 64u;\n"
            f"{ind}    if ((uint32_t)(edi + _bn) > edi && (uint32_t)(esi + _bn) > esi\n"
            f"{ind}        && ((uint32_t)(edi + _bn) <= esi || (uint32_t)(esi + _bn) <= edi)) {{\n"
            f"{ind}        uint32_t _bt = ecx;\n"
            f"{ind}        (memcpy)((void *)XBOX_PTR(edi), (const void *)XBOX_PTR(esi), _bn);\n"
            f"{ind}        esi += _bn; edi += _bn; ecx = 0;\n"
            f"{ind}        {mm}\n"
            f"{ind}        _fb = 0x40u; _fbs = 0x40; _fa = 0; _fas = 0;\n"
            f"{ind}        RECOMP_BLOCK_COPY_DONE(_bt); goto {after};\n"
            f"{ind}    }}\n"
            f"{ind}}}")


def rewrite(lines):
    out = []
    n = 0
    i = 0
    while i < len(lines):
        m = HEAD.match(lines[i])
        if m and i + 1 + len(BODY) < len(lines) and MARK not in lines[i + 1]:
            ok = True
            back = None
            for k, rx in enumerate(BODY):
                mm = rx.match(lines[i + 1 + k].strip())
                if not mm:
                    ok = False
                    break
                if "back" in rx.groupindex:
                    back = mm.group("back")
            j = i + 1 + len(BODY)
            # the label right after the loop (a blank line between)
            while ok and j < len(lines) and not lines[j].strip():
                j += 1
            am = AFTER.match(lines[j]) if ok and j < len(lines) else None
            if ok and back == m.group("label") and am:
                out.append(lines[i])
                out.append(front_door(m.group("ind") or "    ", am.group("label")))
                out.extend(lines[i + 1:i + 1 + len(BODY)])
                i += 1 + len(BODY)
                n += 1
                continue
        out.append(lines[i])
        i += 1
    return out, n


def main():
    args = [x for x in sys.argv[1:] if not x.startswith("--")]
    if len(args) != 1:
        print(__doc__)
        sys.exit(2)
    gen = pathlib.Path(args[0])
    total = 0
    for f in sorted(gen.glob("recomp_0*.c")) + [gen / "recomp_split.c"]:
        if not f.exists():
            continue
        lines = f.read_text().split("\n")
        out, n = rewrite(lines)
        if n:
            total += n
            print(f"  {f.name}: {n}")
            if "--apply" in sys.argv:
                f.write_text("\n".join(out))
    print(f"{total} MMX copy loops given a memcpy front door")
    if "--apply" in sys.argv:
        print("applied")


if __name__ == "__main__":
    main()
