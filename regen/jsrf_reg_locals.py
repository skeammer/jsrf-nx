"""Keep the lifted JSRF code's x86 registers in C locals.

The guest registers are thread-locals (g_eax .. g_esp, g_fp_top), and the
generated code is `#define eax g_eax` over them, so every register read and
write in a lifted function is a memory access. The lifted code is built
-fno-strict-aliasing, so any guest store may alias any register and none of
them ever stays in a host register: `ecx = eax + 1` after a store is a load
of eax, an add and a store of ecx. On the Switch's A57 that is most of what
the game's main thread does, spread over thousands of functions -- there is
no hot spot to fix by hand.

xboxrecomp's translator has the cure (_localize_registers and
_localize_x87_stack in tools/recomp/translator.py), but JSRF's generated code
predates it and carries hand edits that a regeneration would lose. This pass
does the same rewrite on the generated C text, so the edits survive:

  - each lifted function declares locals that shadow the globals by name
    (uint32_t g_eax = recomp_leaf_ld_eax(); ...), loaded after its
    pre-emption point, so the eax .. esp macros and the fp_* macros reach the
    locals. The x87 stack stays in memory; its top index becomes a local and
    the stack a pointer taken once.
  - every statement that can run other lifted code or look at the registers
    -- a direct, ABI-wrapped or indirect call, an indirect tail call, the
    unimplemented-instruction logger, the tracers, the debug service, the
    spin hint -- stores them back first and loads them again after;
  - every return, and the end of the body, stores them back.

So callers, callees, kernel bridges and interrupt delivery (which runs on its
own threads) see the registers exactly where they did. A tail call
(`sub_X(); return;`) stores before the call and returns without reloading.

Conservative by construction: a function is left alone (and reported) if it
calls anything this pass does not know to be harmless, if a call is not a
whole statement on one line, or if it has setjmp/SEH, preprocessor lines with
calls in them, or address-of a register. RECOMP_PREEMPT is not wrapped: it
only yields the guest lock (kernel_bridge.c), and other threads have their own
registers.

What changes for debugging: the watchdog and crash reports read the globals,
which inside a rewritten function are as of its last call, not the current
instruction.

  python3 regen/jsrf_reg_locals.py jsrf-switch/src/recomp/gen           # count
  python3 regen/jsrf_reg_locals.py jsrf-switch/src/recomp/gen --apply   # rewrite
  python3 regen/jsrf_reg_locals.py jsrf-switch/src/recomp/gen -v        # and why

Undo with git (git checkout -- jsrf-switch/src/recomp/gen). Idempotent: a
function already rewritten is skipped, so a second --apply changes nothing.
Needs the recomp_types.h that defines the recomp_leaf_ld_* helpers (the same
commit's). Checked by a differential test that runs every function both ways
from the same state and compares everything it does (see the commit).
"""
import hashlib
import pathlib
import re
import sys

FILE_MARKER = "/* jsrf_reg_locals:"
FN_MARKER = "/* registers in locals */"
FP_MARKER = "/* x87 top in a local */"

FILE_NOTE = FILE_MARKER + """ x86 registers in C locals inside each lifted
 * function (regen/jsrf_reg_locals.py). RECOMP_RL_SPILL stores them back
 * before anything that can run other code, RECOMP_RL_RELOAD loads them after;
 * both are #defined per function. RECOMP_REG_LOCALS tells recomp_types.h. */
"""
DEFINE = "#define RECOMP_REG_LOCALS 1\n"

FUNC = re.compile(r"^void [A-Za-z_][A-Za-z0-9_]*\(void\)$")
REGS = ("eax", "ecx", "edx", "ebx", "esi", "edi", "esp")
REG_USE = {r: re.compile(rf"\b(?:g_)?{r}\b") for r in REGS}
FP_USE = re.compile(r"\b(?:fp_push|fp_pop|fp_top|fp_st1?|g_fp_stack|g_fp_top)\b")

# Statements that can run other lifted code or read and write the registers
# behind the function's back. Each must be a whole statement: NAME(...);
WRAP_NAMES = (
    r"RECOMP_ABI_CALL(?:_[A-Z])?|RECOMP_ICALL(?:_SAFE(?:_AT)?)?|RECOMP_ITAIL|"
    r"RECOMP_UNIMPL|RECOMP_SPIN_HINT|RECOMP_TRACE_(?:ENTER|EXIT|ESP)|"
    r"recomp_debug_service|recomp_spin_(?:hint|wake|yield)|xbe_entry_point|"
    r"sub_[0-9A-Fa-f]{8}(?:_gen)?")
WRAP = re.compile(rf"\b(?:{WRAP_NAMES})\s*\(")
# A direct call of a lifted function takes no arguments.
SUB_CALL = re.compile(r"\bsub_[0-9A-Fa-f]{8}(?:_gen)?\s*\(\s*\)")

# Everything else a lifted body may call: macros and inline helpers from
# recomp_types.h that touch guest memory, flags, x87 slots, XMM/MMX registers
# (not localized) or nothing guest-visible, plus libc/libm. xbox_mmio_read and
# _write reach device models, which never look at the guest registers (an
# interrupt is delivered on the controller's own thread; see usb/ohci.c).
SAFE = set("""
if for while switch return sizeof defined
MEM8 MEM16 MEM32 MEM64 MEMF MEMD SMEM8 SMEM16 SMEM32 SMEM64
PUSH32 POP32 PUSH16 POP16 XBOX_PTR
RECOMP_MEM_WRITE8 RECOMP_MEM_WRITE16 RECOMP_MEM_WRITE32 RECOMP_MEM_WRITE64
RECOMP_MEM_WRITEF RECOMP_MEM_WRITED
RECOMP_MMIO_MEM_WRITE8 RECOMP_MMIO_MEM_WRITE16 RECOMP_MMIO_MEM_WRITE32
RECOMP_MMIO_MEM_WRITE64 RECOMP_MMIO_MEM_WRITEF RECOMP_MMIO_MEM_WRITED
MMIO_RD8 MMIO_RD16 MMIO_RD32 MMIO_WR8 MMIO_WR16 MMIO_WR32
FS_MEM8 FS_MEM16 FS_MEM32 FS_SMEM8 FS_SMEM16 FS_SMEM32 FS_SMEM64
TEST_Z TEST_NZ TEST_S TEST_NS
CMP_EQ CMP_NE CMP_B CMP_BE CMP_A CMP_AE CMP_L CMP_LE CMP_G CMP_GE
LO8 HI8 LO16 SET_LO8 SET_HI8 SET_LO16 ZX8 ZX16 SX8 SX16
ROL32 ROR32 ROL16 ROR16 ROL8 ROR8
RECOMP_PARITY8 RECOMP_DF_STEP RECOMP_FCMP RECOMP_FCMP_CC
RECOMP_ATOMIC_XADD8 RECOMP_ATOMIC_XADD16 RECOMP_ATOMIC_XADD32
RECOMP_ATOMIC_ADD32 RECOMP_ATOMIC_CAS32
RECOMP_ADD_FLAGS32 RECOMP_EFLAGS_ZF RECOMP_F2I_ROUND RECOMP_F2I_TRUNC
RECOMP_RCR RECOMP_RCL
fp_push fp_pop fp_top fp_st fp_st1
recomp_fist recomp_frndint recomp_fxam
memcpy memset memmove
fabs fabsf sqrt sqrtf sin cos tan atan atan2 log log2 log10 exp exp2
ldexp floor ceil trunc round fmod pow
xbox_ReadTimeStampCounter recomp_rdtsc
RECOMP_PREEMPT recomp_preempt __builtin_expect
""".split())
SAFE_PREFIX = ("XMM_", "MMX_")

IDENT_CALL = re.compile(r"\b([A-Za-z_][A-Za-z0-9_]*)\s*\(")
RETURN = re.compile(r"\breturn\b")
AMP_REG = re.compile(r"(?<!&)&(?!&)\s*(?:g_)?(?:" + "|".join(REGS) + r"|fp_top|fp_stack)\b")


def takes_address(code):
    """A unary & on a register: one not after an operand (an identifier, a
    constant, ')' or ']'), which would be a bitwise and."""
    for m in AMP_REG.finditer(code):
        before = code[:m.start()].rstrip()
        if not before or not (before[-1].isalnum() or before[-1] in "_)]"):
            return True
    return False


FORBID = re.compile(r"\b(?:setjmp|longjmp|_setjmp|RECOMP_SEH\w*|recomp_setjmp_slot|"
                    r"recomp_guest_longjmp|recomp_leaf_\w+|recomp_fp_\w+|"
                    r"recomp_fsave|recomp_frstor|recomp_fxsave|recomp_fxrstor|"
                    r"recomp_fldenv|recomp_fstenv)\b")


class Skip(Exception):
    pass


def mask_lines(lines):
    """Each line with comments, strings and character constants blanked to
    spaces (same length), so regexes see only code at the right columns."""
    out = []
    in_comment = False
    for line in lines:
        m = []
        i, n = 0, len(line)
        while i < n:
            if in_comment:
                j = line.find("*/", i)
                if j < 0:
                    m.append(" " * (n - i))
                    i = n
                else:
                    m.append(" " * (j + 2 - i))
                    i = j + 2
                    in_comment = False
                continue
            c = line[i]
            if line.startswith("/*", i):
                in_comment = True
                m.append("  ")
                i += 2
            elif line.startswith("//", i):
                m.append(" " * (n - i))
                i = n
            elif c in "\"'":
                j = i + 1
                while j < n and line[j] != c:
                    j += 2 if line[j] == "\\" else 1
                j = min(j + 1, n)
                m.append(c + " " * (j - i - 2) + c if j - i >= 2 else " " * (j - i))
                i = j
            else:
                m.append(c)
                i += 1
        out.append("".join(m))
    return out


def preproc_flags(masked):
    """True for lines that belong to a preprocessor directive (with its
    backslash continuations)."""
    flags = []
    cont = False
    for ml in masked:
        is_pp = cont or ml.lstrip().startswith("#")
        flags.append(is_pp)
        cont = is_pp and ml.rstrip().endswith("\\")
    return flags


def match_paren(s, i):
    """Index of the ')' matching the '(' at s[i], or -1 if not on this line."""
    depth = 0
    for j in range(i, len(s)):
        if s[j] == "(":
            depth += 1
        elif s[j] == ")":
            depth -= 1
            if depth == 0:
                return j
    return -1


def code_before(masked, pp, li, col):
    """The code before (li, col), back to the last line with any, skipping
    preprocessor lines; '' at the start of the body."""
    s = masked[li][:col].rstrip()
    while not s and li > 0:
        li -= 1
        if not pp[li]:
            s = masked[li].rstrip()
    return s


def rewrite_function(body):
    """body: the lines between the function's '{' and its closing '}'.
    Returns (new_body, n_regs, has_fp, n_sites) or raises Skip(reason)."""
    try:
        at = next(i for i, l in enumerate(body) if l.strip() == "RECOMP_PREEMPT();")
    except StopIteration:
        raise Skip("no RECOMP_PREEMPT() entry line")
    text = "\n".join(body)
    if FN_MARKER in text or FP_MARKER in text:
        raise Skip("already rewritten")
    masked = mask_lines(body)
    pp = preproc_flags(masked)
    code = "\n".join(ml for ml, p in zip(masked, pp) if not p)
    if FORBID.search(code):
        raise Skip("setjmp/SEH/x87 state save or a helper of this pass")
    if takes_address(code):
        raise Skip("address of a register taken")
    for k, (ml, p) in enumerate(zip(masked, pp)):
        if p and (WRAP.search(ml) or RETURN.search(ml)):
            raise Skip("call or return in a preprocessor line")
    for m in IDENT_CALL.finditer(code):
        name = m.group(1)
        if WRAP.match(m.group(0)) or name in SAFE or name.startswith(SAFE_PREFIX):
            continue
        raise Skip("calls " + name)
    # Brace balance, as a check that the closing '}' really closed the body.
    if code.count("{") != code.count("}"):
        raise Skip("unbalanced braces")

    used = [r for r in REGS if REG_USE[r].search(code)]
    has_fp = bool(FP_USE.search(code))
    if not used and not has_fp:
        raise Skip("no registers")

    spill = [f"recomp_leaf_st_{r}(g_{r});" for r in used]
    reload = [f"g_{r} = recomp_leaf_ld_{r}();" for r in used]
    if has_fp:
        spill.append("recomp_fp_top_st(g_fp_top);")
        reload.append("g_fp_top = recomp_fp_top_ld();")

    out = body[:at + 1]
    if used:
        out.append("    uint32_t " + ", ".join(f"g_{r} = recomp_leaf_ld_{r}()" for r in used)
                   + "; " + FN_MARKER)
    if has_fp:
        out.append("    double *const g_fp_stack = recomp_fp_base(); "
                   "int g_fp_top = recomp_fp_top_ld(); " + FP_MARKER)
    out.append("#define RECOMP_RL_SPILL " + " ".join(spill))
    out.append("#define RECOMP_RL_RELOAD " + " ".join(reload))

    sites = 0
    for li in range(at + 1, len(body)):
        line, ml = body[li], masked[li]
        if pp[li]:
            out.append(line)
            continue
        edits = []          # (start, end, replacement) on this line, by column
        spans = []          # (start, end) of each wrapped statement
        tail_returns = set()
        for m in WRAP.finditer(ml):
            s = m.start()
            if m.group(0).startswith("sub_") and not SUB_CALL.match(ml, s):
                raise Skip(f"sub_ call with arguments: {line.strip()[:60]}")
            po = m.end() - 1
            pc = match_paren(ml, po)
            if pc < 0:
                raise Skip(f"call spans lines: {line.strip()[:60]}")
            k = pc + 1
            while k < len(ml) and ml[k] == " ":
                k += 1
            if k >= len(ml) or ml[k] != ";":
                raise Skip(f"call is not a statement: {line.strip()[:60]}")
            if any(a <= s <= b for a, b in spans):
                raise Skip(f"nested calls: {line.strip()[:60]}")
            spans.append((s, k))
            before = code_before(masked, pp, li, s)
            last = before[-1:] if before else ""
            if before and last not in ";{}:)" and not before.endswith("else"):
                raise Skip(f"call inside an expression: {line.strip()[:60]}")
            # Conditional if it is the body of an if/else/loop without braces.
            conditional = last == ")" or before.endswith("else")
            rm = re.match(r"(\s*)return\s*;", ml[k + 1:])
            if rm and not conditional:
                # Tail call: the callee leaves the registers where the caller
                # of this function expects them; nothing to reload or store.
                tail_returns.add(k + 1 + len(rm.group(1)))
                edits.append((s, s, "{ RECOMP_RL_SPILL "))
                edits.append((k + 1, k + 1, " }"))
            else:
                edits.append((s, s, "{ RECOMP_RL_SPILL "))
                edits.append((k + 1, k + 1, " RECOMP_RL_RELOAD }"))
            sites += 1
        for m in RETURN.finditer(ml):
            s = m.start()
            rm = re.match(r"return\s*;", ml[s:])
            if not rm:
                raise Skip("return with a value")
            if s in tail_returns:
                continue
            e = s + len(rm.group(0))
            edits.append((s, s, "{ RECOMP_RL_SPILL "))
            edits.append((e, e, " }"))
        # Every edit is an insertion. Two at one column, or a site inside
        # another site's parentheses, would mean a call nested in a call.
        cols = sorted(a for a, _, _ in edits)
        if len(cols) != len(set(cols)):
            raise Skip(f"nested calls: {line.strip()[:60]}")
        for a, b, rep in sorted(edits, key=lambda t: t[0], reverse=True):
            line = line[:a] + rep + line[b:]
        out.append(line)

    out.append("    RECOMP_RL_SPILL /* registers back at the end */")
    out.append("#undef RECOMP_RL_SPILL")
    out.append("#undef RECOMP_RL_RELOAD")
    return out, len(used), has_fp, sites


def rewrite(src, name="", report=None):
    """Return (new_src, stats dict)."""
    st = {"functions": 0, "rewritten": 0, "fp": 0, "sites": 0, "skipped": {}}
    lines = src.split("\n")
    out = []
    i = 0
    while i < len(lines):
        line = lines[i]
        if FUNC.match(line) and i + 1 < len(lines) and lines[i + 1] == "{":
            j = i + 2
            while j < len(lines) and lines[j] != "}":
                j += 1
            if j >= len(lines):
                raise ValueError(f"{name}: unterminated function at line {i + 1}")
            st["functions"] += 1
            body = lines[i + 2:j]
            try:
                new_body, nregs, has_fp, sites = rewrite_function(body)
                st["rewritten"] += 1
                st["fp"] += has_fp
                st["sites"] += sites
            except Skip as e:
                new_body = body
                why = str(e)
                key = why.split(":")[0]
                st["skipped"][key] = st["skipped"].get(key, 0) + 1
                if report is not None and key not in ("already rewritten", "no registers"):
                    report.append(f"{name}:{i + 1}: {line[5:-6]}: {why}")
            out.append(line)
            out.append("{")
            out.extend(new_body)
            out.append("}")
            i = j + 1
            continue
        out.append(line)
        i += 1
    text = "\n".join(out)
    if st["rewritten"] and FILE_MARKER not in text:
        inc = '#include "recomp_funcs.h"\n'
        if inc not in text:
            raise ValueError(f"{name}: no recomp_funcs.h include to put the note before")
        # Before the include: recomp_types.h then leaves the nine names to
        # the locals instead of renaming them to their fast accessors.
        text = text.replace(inc, FILE_NOTE + DEFINE + "\n" + inc, 1)
    return text, st


def gen_files(gen):
    files = sorted(gen.glob("recomp_0*.c")) + [gen / "recomp_split.c",
                                                gen / "recomp_stubs_unresolved.c"]
    return [p for p in files if p.exists()]


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    gen = pathlib.Path(argv[1])
    apply = "--apply" in argv
    verbose = "-v" in argv
    if apply:
        hdr = (gen / "recomp_types.h").read_text()
        if "recomp_leaf_ld_" not in hdr or "RECOMP_ICALL_FAIL_SYNC" not in hdr:
            print("recomp_types.h has no recomp_leaf_ld_ helpers: apply the patch that "
                  "comes with this script first")
            return 1
    tot = {"functions": 0, "rewritten": 0, "fp": 0, "sites": 0}
    skipped = {}
    report = []
    fp_in, fp_out = hashlib.sha256(), hashlib.sha256()
    for p in gen_files(gen):
        src = p.read_text()
        new, st = rewrite(src, p.name, report)
        fp_in.update(src.encode())
        fp_out.update(new.encode())
        for k in tot:
            tot[k] += st[k]
        for k, v in st["skipped"].items():
            skipped[k] = skipped.get(k, 0) + v
        print(f"{p.name}: {st['rewritten']} of {st['functions']} functions rewritten, "
              f"{st['sites']} calls wrapped, {st['fp']} with x87")
        if apply and new != src:
            p.write_text(new)
    print(f"total: {tot['rewritten']} of {tot['functions']} functions, "
          f"{tot['sites']} calls wrapped, {tot['fp']} with x87"
          + ("" if apply else " (dry run)"))
    for k, v in sorted(skipped.items(), key=lambda t: -t[1]):
        print(f"  left alone, {k}: {v}")
    # Same input, same output: the pass is deterministic, so two trees that
    # started alike end alike.
    print(f"input {fp_in.hexdigest()[:16]}, output {fp_out.hexdigest()[:16]}")
    if verbose:
        for r in report:
            print("  " + r)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
