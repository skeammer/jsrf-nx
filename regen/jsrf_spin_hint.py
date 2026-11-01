"""Mark the lifted code's hardware poll loops with RECOMP_SPIN_HINT().

A title waiting on the GPU, the sound chip or another thread spins on a word
of memory:

    loc_0019151B: ;
        eax = MEM32(ecx + 0x34);
        ...
        if (CMP_B(_fa, _fb)) goto loc_0019151B;

On the Xbox that thread was time-sliced against whatever would end the wait.
Here it would hold a whole host core re-reading one word, while the thread it
waits for shares what is left of the Switch's three. RECOMP_SPIN_HINT() on the
back edge (recomp_types.h: a CPU yield hint, and a host yield now and then)
gives the core away.

Only loops that cannot be doing anything but waiting are marked:
  - a single block: the label, up to 40 lines, and a conditional `goto` back
    to the label as its last line;
  - nothing that writes memory, calls a function, touches the guest stack, or
    uses a runtime helper (other than the pure flag helpers listed below);
  - at least one memory read, and every read at an address the loop does not
    change (a loop that walks memory is a search, not a wait);
  - no register carried from one turn to the next (a counter or an
    accumulator makes a loop that computes and ends by itself).

The pass works on the generated C text, before the later regen passes.
Each file it marks needs block.h pasted near its top.

  python3 regen/jsrf_spin_hint.py jsrf-switch/src/recomp/gen           # count
  python3 regen/jsrf_spin_hint.py jsrf-switch/src/recomp/gen --apply   # rewrite
"""
import pathlib
import re
import sys

MAX_BODY_LINES = 40

GUEST_REGS = ("eax", "ebx", "ecx", "edx", "esi", "edi", "ebp", "esp")
_reg_alt = "|".join(GUEST_REGS)
# A guest register as a whole word.
REG_WORD = re.compile(r"(?<![A-Za-z0-9_])(" + _reg_alt + r")(?![A-Za-z0-9_])")
# An assignment to a guest register (plain, compound, or ++/--).
REG_ASSIGN = re.compile(r"(?<![A-Za-z0-9_.>])(" + _reg_alt +
                        r")\s*(?:<<=|>>=|\+=|-=|\*=|/=|%=|&=|\|=|\^=|=(?!=)|\+\+|--)")
# Operators that read their own target as well (shifts are not counted, as in
# the pass the lifted code was first marked with).
SELF_READING = ("++", "--", "+=", "-=", "|=", "&=", "^=", "*=", "/=", "%=")

MEM_READ = re.compile(r"(?<![A-Za-z0-9_])(?:S?MEM(?:8|16|32|64|F|D)|MMIO_RD(?:8|16|32))\(")
MEM_WRITE = re.compile(r"(?<![A-Za-z0-9_])S?MEM(?:8|16|32|64|F|D)\([^;]*\)\s*"
                       r"(?:<<=|>>=|\+=|-=|\*=|/=|%=|&=|\|=|\^=|=(?!=))")
# The runtime's flag helpers compute and touch nothing: allowed in a poll.
PURE_HELPERS = r"(?:PARITY8|FCMP|FCMP_CC|DF_STEP|EFLAGS_ZF|ADD_FLAGS32)\("
DISQUALIFY = re.compile(
    r"(?<![A-Za-z0-9_])(?:"
    r"sub_[0-9A-Fa-f]{8}\w*\s*\(|"            # a call
    r"RECOMP_ICALL|ICALL|"                      # an indirect call
    r"PUSH\d*\(|POP\d*\(|"                      # the guest stack
    r"MMIO_WR|RECOMP_ATOMIC|__sync|__atomic|"   # stores and atomics
    r"return\b|longjmp|recomp_|fp_|"            # leaving, runtime calls, x87
    r"RECOMP_(?!" + PURE_HELPERS + r")[A-Z_0-9]+\s*\("
    r")")

BLOCK_LABEL = re.compile(r"^(loc_[0-9A-Fa-f]+): ;\s*$")
ANY_LABEL = re.compile(r"^\s*loc_[0-9A-Fa-f]+: ;")


def call_arguments(text, after_open):
    """The text between the '(' just before `after_open` and its ')'."""
    depth, end = 1, after_open
    while end < len(text) and depth:
        c = text[end]
        depth += c == "("
        depth -= c == ")"
        end += 1
    return text[after_open:end - 1]


def only_waits(body_lines):
    """Is this loop body a pure wait (see the module notes)?"""
    text = "\n".join(body_lines)
    if DISQUALIFY.search(text) or MEM_WRITE.search(text):
        return False
    reads = [m.end() for m in MEM_READ.finditer(text)]
    if not reads:
        return False
    assigned = set(REG_ASSIGN.findall(text))
    for at in reads:
        if assigned & set(REG_WORD.findall(call_arguments(text, at))):
            return False
    # Every register the loop assigns must be assigned before it is read in
    # this turn: a value read first came from the turn before.
    fresh = set()
    for line in body_lines:
        code = line.split("/*", 1)[0]
        a = REG_ASSIGN.search(code)
        if not a:
            if (set(REG_WORD.findall(code)) & assigned) - fresh:
                return False
            continue
        op = code[a.end() - 2:a.end()]
        reads_here = code[:a.start()] + code[a.end():]
        if op in SELF_READING or code[a.end() - 1] != "=":
            reads_here += " " + a.group(1)
        if (set(REG_WORD.findall(reads_here)) & assigned) - fresh:
            return False
        fresh.add(a.group(1))
    return True


def mark(code):
    """The C text with every pure-wait loop's back edge hinted, and the count."""
    lines = code.split("\n")
    result = list(lines)
    count = 0
    i = 0
    while i < len(lines):
        label = BLOCK_LABEL.match(lines[i].strip())
        if not label:
            i += 1
            continue
        back_edge = "goto " + label.group(1) + ";"
        end = i + 1
        while end < len(lines) and end - i <= MAX_BODY_LINES:
            line = lines[end]
            if ANY_LABEL.match(line) or line.startswith("}") or "goto " in line:
                break
            end += 1
        if end < len(lines) and end - i <= MAX_BODY_LINES:
            last = lines[end]
            tail = last.strip()
            if (tail.startswith("if (")
                    and re.search(re.escape(back_edge) + r"\s*(?:/\*.*\*/)?$", tail)
                    and last.count("goto ") == 1
                    and only_waits(lines[i + 1:end])):
                at = last.index(back_edge)
                result[end] = (last[:at] + "{ RECOMP_SPIN_HINT(); " + back_edge + " }"
                               + last[at + len(back_edge):])
                count += 1
        i = end if end > i else i + 1
    return "\n".join(result), count


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    apply = "--apply" in sys.argv
    total = 0
    for path in sorted(pathlib.Path(sys.argv[1]).glob("recomp_0*.c")):
        text, n = mark(path.read_text())
        total += n
        print(f"{path.name}: {n}")
        if apply and n:
            path.write_text(text)
    print("total", total)


if __name__ == "__main__":
    main()
