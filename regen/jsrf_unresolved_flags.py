"""List the lifter's "UNRESOLVED FLAGS, branch never taken" branches and say
which ones the function can actually reach.

jsrf-recomp's lifter emits `if (_flags /* jcc - UNRESOLVED FLAGS ... */)`
where it could not tell which instruction set the flags a conditional jump
reads: `_flags` is always 0, so the branch is never taken. NFSU2 hit the same
class (a flag join with two different setters) and fixed it in its translator
(nfsu2-sw a924137, `_materialised_joins`); in NFS Carbon it made the car drive
through every prop.

Many of JSRF's are in copies of code another function owns (a body that ran
on into its neighbour), where the block cannot be reached from the copy's own
entry and the real function resolves the branch. This script builds each
function's control-flow graph from the C text (labels, gotos, fall-through,
returns) and marks every such branch REACHABLE or unreachable, so only the
reachable ones need looking at.

  python3 regen/jsrf_unresolved_flags.py jsrf-switch/src/recomp/gen
"""
import pathlib
import re
import sys

FUNC = re.compile(r"^void ([A-Za-z_][A-Za-z0-9_]*)\(void\)$")
LABEL = re.compile(r"^(loc_[0-9A-Fa-f]{8}): ;")
GOTO = re.compile(r"\bgoto (loc_[0-9A-Fa-f]{8});")
ORIGINAL = re.compile(r"^ \* Original: (0x[0-9A-Fa-f]+)")
CATEGORY = re.compile(r"^ \* Category: (\S+)")


def terminal(stmt):
    """Does this statement end its block (no fall-through)?"""
    s = stmt.strip()
    if not s or s.startswith("/*") or s.startswith("#"):
        return False
    if s.startswith("if ") or s.startswith("if("):
        return False
    s = re.sub(r"/\*.*?\*/", "", s).strip()
    if s.endswith("return;") or re.search(r"\breturn;\s*\}?$", s):
        return True
    if re.match(r"^(\{ *RECOMP_PREEMPT\(\); *)?goto loc_[0-9A-Fa-f]{8}; *\}?$", s):
        return True
    return False


def analyse(body):
    """body: lines of one function. Returns [(line_index, reachable)]."""
    blocks = []                    # (label or None, [line indices])
    cur = [None, []]
    for i, line in enumerate(body):
        m = LABEL.match(line)
        if m:
            blocks.append(cur)
            cur = [m.group(1), []]
        cur[1].append(i)
    blocks.append(cur)
    index = {b[0]: k for k, b in enumerate(blocks) if b[0]}
    succ = []
    for k, (lab, idx) in enumerate(blocks):
        out = set()
        for i in idx:
            for g in GOTO.findall(body[i]):
                if g in index:
                    out.add(index[g])
            if terminal(body[i]):
                break
        ends = any(terminal(body[i]) for i in idx)
        if not ends and k + 1 < len(blocks):
            out.add(k + 1)
        succ.append(out)
    seen = {0}
    todo = [0]
    while todo:
        k = todo.pop()
        for n in succ[k]:
            if n not in seen:
                seen.add(n)
                todo.append(n)
    result = []
    for k, (lab, idx) in enumerate(blocks):
        live = k in seen
        for i in idx:
            if "UNRESOLVED FLAGS" in body[i]:
                result.append((i, live))
            if live and terminal(body[i]):
                live = False           # after a return or a plain goto
    return result


def main(argv):
    gen = pathlib.Path(argv[1] if len(argv) > 1 else "jsrf-switch/src/recomp/gen")
    files = sorted(gen.glob("recomp_0*.c")) + [gen / "recomp_stubs_unresolved.c"]
    total = reach = 0
    for p in files:
        lines = p.read_text().split("\n")
        i = 0
        orig = cat = "?"
        while i < len(lines):
            mo = ORIGINAL.match(lines[i])
            if mo:
                orig = mo.group(1)
            mc = CATEGORY.match(lines[i])
            if mc:
                cat = mc.group(1)
            m = FUNC.match(lines[i])
            if m and i + 1 < len(lines) and lines[i + 1] == "{":
                name = m.group(1)
                j = i + 2
                while j < len(lines) and lines[j] != "}":
                    j += 1
                body = lines[i + 2:j]
                for k, ok in analyse(body):
                    total += 1
                    reach += ok
                    text = body[k].strip()
                    jcc = re.search(r"/\* (j[a-z]+):", text)
                    tgt = GOTO.search(text)
                    dead = "dead code, label not in function" in text
                    print("%-10s %-24s %-12s %-5s -> %-13s %s:%d" % (
                        "REACHABLE" if ok else "unreach.", name, cat,
                        jcc.group(1) if jcc else "?",
                        (tgt.group(1) if tgt else "(outside fn)" if dead else "?"),
                        p.name, i + 3 + k))
                i = j
                orig = cat = "?"
            i += 1
    print("total %d, reachable %d" % (total, reach))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
