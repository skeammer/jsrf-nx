"""Inline small leaf lifted functions at their direct call sites.

JSRF's game thread spends a few percent in tiny helpers: sub_0014C850 and
sub_0014C870 (an x87 max/min of two floats, 1.1% and 1.3% of the thread in
their own bodies) and sub_0014C690 (cvtss2si, 1.1%), and as much again in
what calling them costs: the caller stores its registers to the register
block, pushes the return address, calls; the callee loads the registers,
does one or two instructions' work, stores them back and returns; the caller
loads them again. Inlined, the work happens on the caller's own locals.

Runs on generated C that jsrf_reg_locals.py has rewritten. A callee is
inlined when:
  - it makes no call that needs its registers stored (nothing wrapped in
    RECOMP_RL_SPILL ... RECOMP_RL_RELOAD), no tail jump, no setjmp;
  - every goto in it is to one of its own labels, and every return is
    `esp += N; { RECOMP_RL_SPILL return; }` on a line of its own;
  - it defines no macro but the x87 ones every function gets;
  - it has at most MAX_STATEMENTS statements (HOT_STATEMENTS for the
    functions in HOT, which the console's profile found hot), not counting
    labels and the flag snapshots' declarations.
A call site `{ RECOMP_RL_SPILL RECOMP_ABI_CALL(0x<va>u, sub_<va>); RECOMP_RL_RELOAD }`
becomes `{ /* sub_<va> inlined */ RECOMP_PREEMPT(); <statements> }`, on the
same line, when the caller has a local for every register the callee uses
(and the x87 top, if it uses that):
  - Straight-line, with one return at the end, the labels are left out.
  - Otherwise each label is renamed for the site (loc_X_i<return address>),
    and each return becomes a jump to sub_<va>_end<return address> at the
    end of the block.
The callee's own locals (flag snapshots, ebp) are declared again inside the
block, so the caller's are untouched, as they were across the call. The
return address is still pushed and popped: the guest stack holds the same
bytes afterwards. RECOMP_PREEMPT stays, so a guest-lock build has the same
preemption points. The callee itself stays, for indirect calls.

  python3 regen/jsrf_inline_leaves.py <gen dir>            # report only
  python3 regen/jsrf_inline_leaves.py <gen dir> --apply    # rewrite in place
  python3 regen/jsrf_inline_leaves.py <gen dir> --list     # the callees, one a line

Idempotent: an inlined site no longer has the call, and a function with an
inlined site is not inlined itself, so a second run finds nothing to do. On a
tree the first version (straight-line, 12 statements) rewrote, it inlines
the rest; that first version's sites read the same in both.
"""
import pathlib
import re
import sys

MAX_STATEMENTS = 24
# Longer ones the console profile found hot in JSRF's busiest scenes (the
# matrix library's 0x001BAB20 and 0x001BADB0, 0x0006E130, D3D's 0x00190750):
# inlined up to HOT_STATEMENTS.
HOT = {"sub_001BAB20", "sub_0006E130", "sub_001BADB0", "sub_00190750"}
HOT_STATEMENTS = 72

FUNC = re.compile(r"^void (sub_[0-9A-F]{8})\(void\)$")
LOCALS = re.compile(r"/\* registers in locals \*/\s*$")
LOCAL_NAME = re.compile(r"\b(g_e[a-ds][xpi]|g_e[sd]i|g_esp)\s*=")
FP_DECL = "/* x87 top in a local */"
RET = re.compile(r"^\s*esp \+= (0x[0-9A-Fa-f]+u?|\d+u?); \{ RECOMP_RL_SPILL return; \} /\* ret( \d+)? \*/\s*$")
LABEL = re.compile(r"^\s*(loc_[0-9A-F]{8}): ;\s*$")
GOTO = re.compile(r"\bgoto (loc_[0-9A-F]{8});")
LOCREF = re.compile(r"\b(loc_[0-9A-F]{8})\b")
SITE = re.compile(r"\{ RECOMP_RL_SPILL RECOMP_ABI_CALL\(0x([0-9A-F]{8})u, (sub_[0-9A-F]{8})\); RECOMP_RL_RELOAD \}")
FP_MACRO = re.compile(r"^\s*#\s*(define|undef)\s+fp_(push|pop|top|st1|st)\b")
# The flag snapshots every x87 function declares; not counted as work.
BOILERPLATE = re.compile(r"^(uint32_t _fa = 0, _fb = 0;|int32_t _fas = 0, _fbs = 0;|"
                         r"\(void\)_fa; \(void\)_fb; \(void\)_fas; \(void\)_fbs;)$")
BANNED = ("return", "RECOMP_RL_SPILL", "RECOMP_RL_RELOAD", "RECOMP_ABI_CALL",
          "RECOMP_ICALL", "RECOMP_ITAIL", "setjmp", "#", "RECOMP_UNIMPL",
          " inlined */")   # one level only: a caller this pass rewrote is not inlined itself
END = "@END@"


PUSHED = re.compile(r"PUSH32\(esp, 0x([0-9A-F]{8})u\); $")
RET_GOTO = re.compile(r"^esp \+= \S+; goto @END@; /\* ret( \d+)? \*/$")


class Skip(Exception):
    pass


def functions(lines):
    """(name, first line index of the body's '{', index of its '}')."""
    out = []
    i = 0
    while i < len(lines):
        m = FUNC.match(lines[i])
        if m and i + 1 < len(lines) and lines[i + 1] == "{":
            j = i + 2
            while lines[j] != "}":
                j += 1
            out.append((m.group(1), i + 1, j))
            i = j + 1
            continue
        i += 1
    return out


def reg_names(line):
    return set(LOCAL_NAME.findall(line))


def callee_body(body, name=""):
    """The statements to inline and what they need, or Skip."""
    regs, fp, stmts, label, ret = None, False, [], 0, 0
    labels, targets = set(), set()
    i = 0
    n = len(body)
    while i < n:
        line = body[i]
        s = line.strip()
        i += 1
        if not s:
            continue
        if s == "RECOMP_PREEMPT();" and regs is None and not stmts:
            continue
        if LOCALS.search(line):
            if regs is not None:
                raise Skip("two locals lines")
            regs = reg_names(line)
            continue
        if FP_DECL in line:
            fp = True
            continue
        if re.match(r"^#\s*(define|undef)\s+RECOMP_RL_(SPILL|RELOAD)\b", s):
            continue
        if FP_MACRO.match(line):
            while line.rstrip().endswith("\\") and i < n:     # the macro's other lines
                line = body[i]
                i += 1
            continue
        if s == "RECOMP_RL_SPILL /* registers back at the end */":
            if i < n and any(x.strip() and not x.strip().startswith("#") for x in body[i:]):
                raise Skip("code after the end store")
            continue
        m = LABEL.match(line)
        if m:
            label += 1
            labels.add(m.group(1))
            stmts.append(s)
            continue
        if RET.match(line):
            ret += 1
            stmts.append(re.sub(r"\{ RECOMP_RL_SPILL return; \} ", "goto " + END + "; ", s))
            continue
        if line.rstrip().endswith("\\"):
            raise Skip("a continued line")
        for b in BANNED:
            if b in s:
                raise Skip(f"has {b.strip()}")
        targets.update(GOTO.findall(s))
        if LOCREF.sub("", GOTO.sub("", s)).count("loc_"):
            raise Skip("names a label other than by goto")
        stmts.append(s)
    if regs is None:
        raise Skip("not rewritten (no locals line)")
    if not ret:
        raise Skip("no return")
    if not label:
        raise Skip("no entry label")
    if not targets <= labels:
        raise Skip("a goto out of the function")
    if sum(1 for x in stmts if not BOILERPLATE.match(x) and not LABEL.match(x)) > (
            HOT_STATEMENTS if name in HOT else MAX_STATEMENTS):
        raise Skip("too long")
    if any(x.count("/*") != x.count("*/") for x in stmts):
        raise Skip("a comment over several lines")
    if not targets and ret == 1 and RET_GOTO.match(stmts[-1]):
        # Straight-line, one return at the end: no labels, no jump to the
        # end (the first version's form; its sites read the same).
        stmts = [x for x in stmts if not LABEL.match(x)]
        stmts[-1] = stmts[-1].replace("goto " + END + "; ", "")
        return {"regs": regs, "fp": fp, "stmts": stmts, "jumps": False}
    return {"regs": regs, "fp": fp, "stmts": stmts, "jumps": True}


def scan(gen):
    files = sorted(gen.glob("recomp_0*.c")) + [gen / "recomp_split.c"]
    texts = {f: f.read_text().split("\n") for f in files if f.exists()}
    callees, why = {}, {}
    for f, lines in texts.items():
        for name, a, b in functions(lines):
            try:
                callees[name] = callee_body(lines[a + 1:b], name)
            except Skip as e:
                why[name] = str(e)
    return texts, callees, why


def caller_context(lines, a, b):
    regs, fp = set(), False
    for line in lines[a + 1:b]:
        if LOCALS.search(line):
            regs = reg_names(line)
        if FP_DECL in line:
            fp = True
    return regs, fp


def rewrite(texts, callees):
    stats = {"sites": 0, "skipped": 0, "files": 0}
    per = {}
    out = {}
    for f, lines in texts.items():
        changed = False
        new = list(lines)
        for name, a, b in functions(lines):
            regs = fp = None
            for k in range(a + 1, b):
                line = new[k]
                if "RECOMP_ABI_CALL" not in line:
                    continue

                def sub(m):
                    nonlocal regs, fp
                    va, callee = m.group(1), m.group(2)
                    c = callees.get(callee)
                    if not c or callee != "sub_" + va or callee == name:
                        return m.group(0)
                    if regs is None:
                        regs, fp = caller_context(lines, a, b)
                    if not c["regs"] <= regs or (c["fp"] and not fp):
                        stats["skipped"] += 1
                        return m.group(0)
                    stats["sites"] += 1
                    per[callee] = per.get(callee, 0) + 1
                    if not c["jumps"]:
                        return ("{ /* " + callee + " inlined */ RECOMP_PREEMPT(); "
                                + " ".join(c["stmts"]) + " }")
                    # Labels are the function's: each site gets its own names,
                    # and every return jumps to the block's end.
                    # Named after the call's return address: unique in the
                    # function, and the same whichever run inlined it.
                    pm = PUSHED.search(line[:m.start()])
                    k = pm.group(1) if pm else f"n{per[callee]}"
                    body = " ".join(c["stmts"])
                    body = LOCREF.sub(lambda q: f"{q.group(1)}_i{k}", body)
                    end = f"{callee}_end{k}"
                    body = body.replace(END, end)
                    return ("{ /* " + callee + " inlined */ RECOMP_PREEMPT(); " + body
                            + " " + end + ": ; }")

                line2 = SITE.sub(sub, line)
                if line2 != line:
                    new[k] = line2
                    changed = True
        if changed:
            out[f] = new
            stats["files"] += 1
    return out, stats, per


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if len(args) != 1:
        print(__doc__)
        sys.exit(2)
    gen = pathlib.Path(args[0])
    texts, callees, why = scan(gen)
    if "--list" in sys.argv:
        for n in sorted(callees):
            print(n)
        return
    out, stats, per = rewrite(texts, callees)
    print(f"{len(callees)} functions can be inlined; {stats['sites']} call sites inlined in "
          f"{stats['files']} files, {stats['skipped']} left (the caller lacks a local)")
    for n, c in sorted(per.items(), key=lambda x: -x[1])[:15]:
        print(f"  {n}: {c} sites, {len(callees[n]['stmts'])} statements")
    if "--apply" in sys.argv:
        for f, lines in out.items():
            f.write_text("\n".join(lines))
        print("applied")


if __name__ == "__main__":
    main()
