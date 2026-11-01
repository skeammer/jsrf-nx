"""Keep the x87 register stack in C locals inside each lifted function.

The lifter models the x87 stack as eight doubles in the thread's register
block and a top index: every fld, fmul, fstp is `g_fp_stack[(g_fp_top + k) & 7]`,
an index computed at run time, a load, the arithmetic and a store back. JSRF's
matrix and D3D code (0x00190A30, an inverse, is 290 such operations) spends
most of its time on that bookkeeping, and nothing of it can stay in a register.

In compiled x87 code the stack depth at each instruction is fixed: only where
it is relative to the top at entry varies. This pass works that depth out at
every statement of a function (d, 0 to 7, relative to a base x87_t that is the
top at entry), so every stack slot is a fixed one of eight locals,
x87_r0 .. x87_r7: `fp_top()` at depth 3 is x87_r3, and the compiler keeps them
in floating-point registers. The real stack is written back only where
something else can see it:
  - before every RECOMP_RL_SPILL (calls, returns, tail calls) the slots this
    path may have changed are stored and g_fp_top set to (x87_t + d) & 7;
  - after every RECOMP_RL_RELOAD (back from a call) the base is moved so that
    (x87_t + d) & 7 is the top the callee left -- a callee that returns a
    float on the stack has pushed one -- and the slots are loaded again.
The guest-visible state after the function -- stack contents, top, memory --
is the same as before; regen/difftest compares exactly that.

Every value pushed goes through recomp_x87_in (recomp_types.h), which the
compiler cannot see through: an fld of a float is a real conversion, so a
signalling NaN comes back quiet when it is stored again, as on the x87, rather
than the load and the store folded into a 4-byte copy.

A function is left as it was when the depth is not the same on every path
into a label, when the stack is pushed or popped under an `if` or in a loop,
when something reads or writes g_fp_stack or g_fp_top other than through the
lifter's forms, when it uses setjmp, or when it has more calls than x87 work
(the stores and loads around each call would cost more than they save).

Runs on generated C that jsrf_reg_locals.py (and jsrf_inline_leaves.py, which
must run before this) rewrote:

  python3 regen/jsrf_fp_locals.py <gen dir>            # report only
  python3 regen/jsrf_fp_locals.py <gen dir> --apply    # rewrite in place
  python3 regen/jsrf_fp_locals.py <gen dir> --why      # why each was left

Idempotent: a rewritten function is marked and not touched again.
"""
import collections
import pathlib
import re
import sys

FUNC = re.compile(r"^void (sub_[0-9A-F]{8})\(void\)$")
FP_DECL = "/* x87 top in a local */"
MARK = "/* x87 stack in locals */"
TOKEN = re.compile(r"""
    (?P<ws>\s+)
  | (?P<comment>/\*.*?\*/|//[^\n]*)
  | (?P<str>"(?:\\.|[^"\\\n])*"|'(?:\\.|[^'\\\n])*')
  | (?P<num>(?:0[xX][0-9A-Fa-f]+|\d+\.?\d*(?:[eE][+-]?\d+)?|\.\d+(?:[eE][+-]?\d+)?)[uUlLfF]*)
  | (?P<id>[A-Za-z_][A-Za-z0-9_]*)
  | (?P<op><<=|>>=|->|\+\+|--|<<|>>|<=|>=|==|!=|&&|\|\||[-+*/%&|^]=|[{}()\[\];,:?~!<>=+\-*/%&|^.])
""", re.S | re.X)


class Skip(Exception):
    pass


class Tok:
    __slots__ = ("kind", "text", "start", "end")

    def __init__(self, kind, text, start, end):
        self.kind, self.text, self.start, self.end = kind, text, start, end


def tokenize(text):
    """Code tokens of a function body; preprocessor lines are left out."""
    toks = []
    pos = 0
    n = len(text)
    line_start = True
    while pos < n:
        if line_start:
            m = re.match(r"[ \t]*#", text[pos:pos + 200])
            if m:
                # A directive, with its continuation lines.
                end = pos
                while True:
                    nl = text.find("\n", end)
                    if nl < 0:
                        end = n
                        break
                    if text[nl - 1] == "\\":
                        end = nl + 1
                        continue
                    end = nl
                    break
                pos = end
                continue
        m = TOKEN.match(text, pos)
        if not m:
            raise Skip(f"cannot read {text[pos:pos + 20]!r}")
        kind = m.lastgroup
        if kind == "ws":
            line_start = "\n" in m.group(0) or (line_start and True)
            if "\n" in m.group(0):
                line_start = True
        else:
            line_start = False
            if kind != "comment":
                toks.append(Tok(kind, m.group(0), m.start(), m.end()))
        pos = m.end()
    return toks


class Pass:
    """One walk over a function's statements at a given label table."""

    def __init__(self, toks, labels_in, final):
        self.t = toks
        self.labels_in = labels_in          # label -> (d, dirty) known so far
        self.edges = collections.defaultdict(set)   # label -> {(d, dirty)}
        self.final = final
        self.edits = []                     # (start, end, text)
        self.reads, self.writes = set(), set()
        self.calls = 0
        self.fpops = 0
        self.d = 0
        self.dirty = frozenset()
        self.spilled = False

    # -- helpers -----------------------------------------------------------
    def tok(self, i):
        return self.t[i].text if i < len(self.t) else ""

    def expect(self, i, text):
        if self.tok(i) != text:
            raise Skip(f"expected {text!r} at {self.tok(i)!r}")
        return i + 1

    def match_paren(self, i):
        """i at '(' or '['; index after the matching close."""
        open_, close = self.tok(i), {"(": ")", "[": "]", "{": "}"}[self.tok(i)]
        depth = 0
        while i < len(self.t):
            x = self.tok(i)
            if x in ("(", "[", "{"):
                depth += 1
            elif x in (")", "]", "}"):
                depth -= 1
                if depth == 0:
                    return i + 1
            i += 1
        raise Skip("unbalanced " + open_ + close)

    def edit(self, i, j, text):
        """Replace tokens i..j-1."""
        self.edits.append((self.t[i].start, self.t[j - 1].end, text))

    def slot(self, k):
        if self.d is None:
            raise Skip("x87 work in code no path reaches")
        return (self.d + k) & 7

    def reg(self, k, write=False):
        s = self.slot(k)
        (self.writes if write else self.reads).add(s)
        if write:
            self.dirty = self.dirty | {s}
        return f"x87_r{s}"

    # -- statements --------------------------------------------------------
    def block(self, i, cond):
        i = self.expect(i, "{")
        while self.tok(i) != "}":
            if i >= len(self.t):
                raise Skip("unclosed block")
            i = self.stmt(i, cond)
        return i + 1

    def merge(self, label, cond):
        if cond:
            raise Skip("a label under an if")
        came = set(self.edges.get(label, ())) | set(self.labels_in.get(label, ()))
        if self.d is not None:
            came.add((self.d, self.dirty))
        ds = {d for d, _ in came}
        if len(ds) > 1:
            raise Skip(f"depth differs into {label}")
        if not ds:
            self.d, self.dirty = None, frozenset()
            return
        self.d = ds.pop()
        self.dirty = frozenset().union(*(x for _, x in came))

    def stmt(self, i, cond):
        x = self.tok(i)
        if x == "{":
            return self.block(i, cond)
        if x == ";":
            return i + 1
        if x == "if":
            i = self.match_paren(i + 1)
            i = self.body(i)
            if self.tok(i) == "else":
                i = self.body(i + 1)
            return i
        if x in ("for", "while"):
            i = self.match_paren(i + 1)
            return self.body(i)
        if x == "do":
            i = self.body(i + 1)
            i = self.expect(i, "while")
            i = self.match_paren(i)
            return self.expect(i, ";")
        if x in ("switch", "case", "default", "setjmp", "longjmp"):
            raise Skip("has " + x)
        if x == "goto":
            label = self.tok(i + 1)
            if self.d is not None:
                self.edges[label].add((self.d, self.dirty))
            i = self.expect(i + 2, ";")
            if not cond:
                self.d, self.dirty = None, frozenset()
            return i
        if x == "return":
            while self.tok(i) != ";":
                i += 1
            self.spilled = False      # the path ends: the store was for it
            if not cond:
                self.d, self.dirty = None, frozenset()
            return i + 1
        if x in ("break", "continue"):
            if not cond:
                raise Skip(x + " outside a loop")
            return self.expect(i + 1, ";")
        if self.t[i].kind == "id" and self.tok(i + 1) == ":":
            self.merge(x, cond)
            return i + 2
        if x == "RECOMP_RL_SPILL":
            self.calls += 1
            if self.d is not None and self.final:
                st = "".join(f"g_fp_stack[(x87_t + {s}u) & 7u] = x87_r{s}; "
                             for s in sorted(self.dirty))
                self.edit(i, i + 1, f"{st}g_fp_top = (int)((x87_t + {self.d}u) & 7u); "
                          "RECOMP_RL_SPILL")
            self.spilled = True
            return i + 1
        if x == "RECOMP_RL_RELOAD":
            if not self.spilled:
                raise Skip("a reload with no store before it")
            self.spilled = False
            if self.d is not None:
                if self.final:
                    self.edit(i, i + 1, f"RECOMP_RL_RELOAD x87_t = (unsigned)(g_fp_top - {self.d}) & 7u; "
                              "X87_LOAD")
                self.dirty = frozenset()
            return i + 1
        return self.expr_stmt(i)

    def body(self, i):
        """The statement under an if or a loop: may not move the top."""
        d0 = self.d
        dirty0 = self.dirty
        spilled0 = self.spilled
        i = self.stmt(i, True)
        if self.d != d0 and self.d is not None and d0 is not None:
            raise Skip("the top moves under an if")
        if d0 is None:
            self.d = None
        else:
            self.d = d0
        # The body may have run or not: what it may have changed stays dirty.
        # A call under an if leaves its reload's base in x87_t on that path
        # and dirty empty there; the other path keeps its own: the union is
        # what either may have changed since its last load.
        self.dirty = dirty0 | self.dirty
        if self.spilled != spilled0:
            raise Skip("a store and its reload in different statements")
        return i

    def expr_stmt(self, i):
        j = i
        depth = 0
        while True:
            if j >= len(self.t):
                raise Skip("statement without ;")
            x = self.tok(j)
            if x in ("(", "[", "{"):
                depth += 1
            elif x in (")", "]", "}"):
                depth -= 1
                if depth < 0:
                    raise Skip("unbalanced statement")
            elif x == ";" and depth == 0:
                break
            j += 1
        # tokens i..j-1, ';' at j
        if self.spilled and any(self.tok(k) in ("fp_push", "fp_pop", "fp_top", "fp_st",
                                                "fp_st1", "g_fp_stack")
                                for k in range(i, j)):
            raise Skip("x87 work between a store and its reload")
        if self.d is None:
            # No path reaches it (a label nothing jumps to: the code after
            # it is another entry the lifter split off). Left as it was.
            return j + 1
        first = self.tok(i)
        if first == "fp_push":
            if self.tok(i + 1) != "(" or self.match_paren(i + 1) != j:
                raise Skip("fp_push inside an expression")
            inner = self.translate(i + 2, j - 1) or self.raw(i + 2, j - 1)
            self.fpops += 1
            if self.d is None:
                raise Skip("x87 work in code no path reaches")
            self.d = (self.d + 7) & 7
            r = self.reg(0, write=True)
            if self.final:
                self.edit(i, j, f"{r} = recomp_x87_in({inner})")
            return j + 1
        if first == "fp_pop":
            if j != i + 3 or self.tok(i + 1) != "(" or self.tok(i + 2) != ")":
                raise Skip("fp_pop inside an expression")
            self.fpops += 1
            if self.d is None:
                raise Skip("x87 work in code no path reaches")
            self.d = (self.d + 1) & 7
            if self.final:
                self.edit(i, j, "(void)0")
            return j + 1
        # An assignment to a stack slot: left side written, right side read.
        eq = None
        depth = 0
        for k in range(i, j):
            x = self.tok(k)
            if x in ("(", "["):
                depth += 1
            elif x in (")", "]"):
                depth -= 1
            elif x == "=" and depth == 0:
                eq = k
                break
        if eq is not None and self.is_slot(i, eq):
            rhs = self.translate(eq + 1, j) or self.raw(eq + 1, j)
            lhs = self.translate(i, eq, write=True)
            if self.final:
                self.edit(i, j, f"{lhs} = {rhs}")
            return j + 1
        out = self.translate(i, j)
        if self.final and out is not None:
            self.edit(i, j, out)
        return j + 1

    def slot_ref(self, k):
        """(index after it, offset) if a stack slot reference starts at k."""
        x = self.tok(k)
        if x == "fp_top" and self.tok(k + 1) == "(" and self.tok(k + 2) == ")":
            return k + 3, 0
        if x == "fp_st1" and self.tok(k + 1) == "(" and self.tok(k + 2) == ")":
            return k + 3, 1
        if x == "fp_st" and self.tok(k + 1) == "(" and self.t[k + 2].kind == "num" \
                and self.tok(k + 3) == ")":
            return k + 4, int(self.tok(k + 2).rstrip("uU"), 0)
        if x == "g_fp_stack" and self.tok(k + 1) == "[":
            seq = [self.tok(k + n) for n in range(2, 10)]
            if seq[:7] == ["(", "g_fp_top", "+", seq[3], ")", "&", "7"] and seq[7] == "]" \
                    and self.t[k + 5].kind == "num":
                return k + 10, int(seq[3].rstrip("uU"), 0)
            if seq[:2] == ["g_fp_top", "]"]:
                return k + 4, 0
        return None

    def is_slot(self, i, j):
        r = self.slot_ref(i)
        return r is not None and r[0] == j

    def translate(self, i, j, write=False):
        """Text of tokens i..j-1 with stack slots as locals; None if unchanged."""
        parts = []
        changed = False
        k = i
        prev_end = self.t[i].start if i < j else 0
        while k < j:
            r = self.slot_ref(k)
            if r:
                k2, off = r
                if k2 > j:
                    raise Skip("a slot across a statement")
                parts.append(self.gap(prev_end, self.t[k].start))
                parts.append(self.reg(off, write=write))
                prev_end = self.t[k2 - 1].end
                k = k2
                changed = True
                continue
            x = self.tok(k)
            if x in ("fp_push", "fp_pop", "fp_top", "fp_st", "fp_st1"):
                raise Skip(f"{x} in an expression")
            if x == "g_fp_stack":
                raise Skip("g_fp_stack other than as a slot")
            if x == "g_fp_top":
                if self.tok(k + 1) in ("=", "+=", "-=", "++", "--", "&=", "|=") or \
                        self.tok(k - 1) in ("++", "--"):
                    raise Skip("g_fp_top written")
                if self.d is None:
                    raise Skip("x87 top read in code no path reaches")
                parts.append(self.gap(prev_end, self.t[k].start))
                parts.append(f"(int)((x87_t + {self.d}u) & 7u)")
                prev_end = self.t[k].end
                k += 1
                changed = True
                continue
            if x in ("RECOMP_RL_SPILL", "RECOMP_RL_RELOAD"):
                raise Skip(x + " inside a statement")
            if x in ("setjmp", "longjmp", "recomp_fp_base", "recomp_fp_top_ld",
                     "recomp_fp_top_st"):
                raise Skip("has " + x)
            k += 1
        if not changed:
            return None
        parts.append(self.gap(prev_end, self.t[j - 1].end))
        return "".join(parts)

    def raw(self, i, j):
        return self.text[self.t[i].start:self.t[j - 1].end] if i < j else ""

    def gap(self, a, b):
        return self.text[a:b]


def analyse(body_text):
    toks = tokenize(body_text)
    # The declaration line: everything before the first label is set-up.
    labels_in = {}
    for _ in range(12):
        p = Pass(toks, labels_in, final=False)
        p.text = body_text
        p.block(0, False)
        new = {k: frozenset(v) for k, v in p.edges.items()}
        # Labels seen: what the walk put into each.
        merged = {k: frozenset(set(labels_in.get(k, ())) | set(v)) for k, v in new.items()}
        for k in labels_in:
            merged.setdefault(k, labels_in[k])
        if merged == labels_in:
            break
        labels_in = merged
    else:
        raise Skip("no fixed point")
    p = Pass(toks, labels_in, final=True)
    p.text = body_text
    p.block(0, False)
    return p


def rewrite_function(lines, a, b):
    """lines[a] is '{', lines[b] is '}'. New lines or Skip."""
    decl = None
    for k in range(a + 1, b):
        if MARK in lines[k]:
            raise Skip("done already")
        if FP_DECL in lines[k]:
            decl = k
            break
    if decl is None:
        raise Skip("no x87 top in a local")
    if "int g_fp_top = recomp_fp_top_ld();" not in lines[decl]:
        raise Skip("unusual x87 declaration")
    # The walk starts after the declarations: from the line after the x87
    # one (the #defines are skipped by the tokenizer).
    head = "\n".join(lines[a:decl + 1]) + "\n"
    rest = "\n".join(lines[decl + 1:b + 1])
    # Wrap as a block for the walk: rest ends with the function's '}'.
    text = "{\n" + rest
    p = analyse(text)
    if p.fpops == 0 and not p.reads and not p.writes:
        raise Skip("no x87 work")
    used = sorted(p.reads | p.writes)
    # Each call costs a store of what changed and a load of every slot; each
    # x87 operation in the old form costs an index, a load and a store.
    work = p.fpops + len(p.reads) + len(p.writes)
    if p.calls > 1 and p.fpops * 3 < (p.calls - 1) * (len(used) + 2):
        raise Skip(f"more calls than x87 work ({p.calls} calls, {p.fpops} pushes/pops)")
    load = "".join(f"x87_r{s} = g_fp_stack[(x87_t + {s}u) & 7u]; " for s in used)
    out = text
    for start, end, new in sorted(p.edits, key=lambda e: -e[0]):
        out = out[:start] + new.replace("X87_LOAD", load.rstrip()) + out[end:]
    decls = (" unsigned x87_t = (unsigned)g_fp_top; double "
             + ", ".join(f"x87_r{s} = g_fp_stack[(x87_t + {s}u) & 7u]" for s in used)
             + "; " + MARK)
    new_decl = lines[decl] + decls
    body = out[2:]   # drop the "{\n" added for the walk
    new_lines = lines[a:decl] + [new_decl] + body.split("\n")
    assert new_lines[-1] == "}", new_lines[-1]
    return new_lines, work


def main():
    args = [x for x in sys.argv[1:] if not x.startswith("--")]
    if len(args) != 1:
        print(__doc__)
        sys.exit(2)
    gen = pathlib.Path(args[0])
    files = sorted(gen.glob("recomp_0*.c")) + [gen / "recomp_split.c"]
    done, why = [], collections.Counter()
    whys = []
    changed_files = 0
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
                    new, _ = rewrite_function(lines, i + 1, j)
                    out.append(lines[i])
                    out.extend(new)
                    done.append(m.group(1))
                    changed = True
                except Skip as e:
                    msg = str(e)
                    if msg not in ("no x87 top in a local",):
                        why[re.sub(r"\b(loc|sub)_\w+", "L", msg)] += 1
                        whys.append((m.group(1), msg))
                    out.extend(lines[i:j + 1])
                i = j + 1
                continue
            out.append(lines[i])
            i += 1
        if changed:
            changed_files += 1
            if "--apply" in sys.argv:
                f.write_text("\n".join(out))
    print(f"{len(done)} functions with the x87 stack in locals, in {changed_files} files; "
          f"{sum(why.values())} left:")
    for k, v in why.most_common():
        print(f"  {v:5d}  {k}")
    if "--why" in sys.argv:
        for n, w in whys:
            print(n, w)
    if "--list" in sys.argv:
        for n in done:
            print(n)
    if "--apply" in sys.argv:
        print("applied")


if __name__ == "__main__":
    main()
