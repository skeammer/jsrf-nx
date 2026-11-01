"""Cases for jsrf_fp_locals.py and jsrf_simd_locals.py.

  python3 regen/test_jsrf_fp_locals.py

What the rewritten code computes is checked by regen/difftest (every changed
function against its original); these check what the passes take on and
leave, and that they run once.
"""
import pathlib
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import jsrf_fp_locals as fl    # noqa: E402
import jsrf_simd_locals as sl  # noqa: E402

FAILS = []


def check(name, cond):
    if not cond:
        FAILS.append(name)
    print(("ok   " if cond else "FAIL ") + name)


def fn(name, body):
    regs = ["eax", "ecx", "esp"]
    return ([f"void {name}(void)", "{", "    RECOMP_PREEMPT();",
             "    uint32_t " + ", ".join(f"g_{r} = recomp_leaf_ld_{r}()" for r in regs)
             + "; /* registers in locals */",
             "    double *const g_fp_stack = recomp_fp_base(); int g_fp_top = recomp_fp_top_ld();"
             " /* x87 top in a local */",
             "#define RECOMP_RL_SPILL recomp_leaf_st_eax(g_eax); recomp_fp_top_st(g_fp_top);",
             "#define RECOMP_RL_RELOAD g_eax = recomp_leaf_ld_eax(); g_fp_top = recomp_fp_top_ld();",
             "    #define fp_push(v) do { double _fp_value = (v); \\",
             "        g_fp_top = (g_fp_top + 7u) & 7u; \\",
             "        g_fp_stack[g_fp_top] = _fp_value; } while (0)",
             "    #define fp_pop() (g_fp_top = (g_fp_top + 1u) & 7u)",
             "    #define fp_top() g_fp_stack[g_fp_top]",
             "", f"loc_{name[4:]}: ;"]
            + ["    " + b for b in body]
            + ["    RECOMP_RL_SPILL /* registers back at the end */",
               "#undef RECOMP_RL_SPILL", "#undef RECOMP_RL_RELOAD", "}", ""])


RET = "esp += 4; { RECOMP_RL_SPILL return; } /* ret */"
CALL = "PUSH32(esp, 0x1u); { RECOMP_RL_SPILL RECOMP_ABI_CALL(0x00009000u, sub_00009000); RECOMP_RL_RELOAD }"
cases = {
    "sub_00001000": ["fp_push(MEMF(eax)); /* fld */", "fp_push(MEMF(eax + 4));",
                     "fp_top() = fp_top() * fp_st1();",
                     "g_fp_stack[(g_fp_top + 1) & 7] = g_fp_stack[(g_fp_top + 1) & 7] + fp_top(); fp_pop();",
                     "RECOMP_MEM_WRITEF(1u, 2u, ecx, (float)fp_top()); fp_pop();", RET],
    # a branch around a compare: the same depth either way
    "sub_00001100": ["fp_push(MEMF(eax));",
                     "g_fp_cmp = RECOMP_FCMP(fp_top(), MEMF(ecx)); g_fp_cc = RECOMP_FCMP_CC(g_fp_cmp);",
                     "eax = (eax & 0xFFFF0000u) | (uint32_t)(uint16_t)(((g_fp_top & 7u) << 11) | g_fp_cc);",
                     "if (eax & 0x100) goto loc_00001120;", "fp_top() = fp_top() + 1.0;",
                     "loc_00001120: ;", "RECOMP_MEM_WRITEF(1u, 2u, ecx, (float)fp_top()); fp_pop();", RET],
    # a call: the callee may push (a float result), the base moves
    "sub_00001200": ["fp_push(MEMF(eax));", CALL, "fp_top() = fp_top() * fp_st1(); fp_pop();",
                     "RECOMP_MEM_WRITEF(1u, 2u, ecx, (float)fp_top()); fp_pop();", RET],
    # the depth differs into a label
    "sub_00001300": ["if (eax) goto loc_00001310;", "fp_push(MEMF(eax));",
                     "loc_00001310: ;", "fp_pop();", RET],
    # a push under an if
    "sub_00001400": ["if (eax) fp_push(1.0);", RET],
    # code after a label nothing jumps to is left as it was
    "sub_00001500": ["fp_push(MEMF(eax));", "fp_pop();", RET, "loc_00001510: ;", "fp_push(2.0);", RET],
    # mostly calls: left
    "sub_00001600": ["fp_push(MEMF(eax));", "fp_pop();"] + [CALL] * 8 + [RET],
}

with tempfile.TemporaryDirectory() as d:
    gen = pathlib.Path(d)
    f = gen / "recomp_0000.c"
    f.write_text("\n".join(sum((fn(n, b) for n, b in cases.items()), [])))
    lines = f.read_text().split("\n")

    def body_of(name, src):
        a = src.index(f"void {name}(void)") + 1
        b = a
        while src[b] != "}":
            b += 1
        return a, b

    def run(name):
        a, b = body_of(name, lines)
        try:
            new, _ = fl.rewrite_function(lines, a, b)
            return "\n".join(new), None
        except fl.Skip as e:
            return None, str(e)

    t, why = run("sub_00001000")
    check("straight-line: slots are locals", t is not None and "x87_r7 = recomp_x87_in(MEMF(eax))" in t
          and "x87_r6 = x87_r6 * x87_r7" in t and "x87_r7 = x87_r7 + x87_r6" in t)
    check("straight-line: the top is stored at the return",
          t is not None and "g_fp_top = (int)((x87_t + 0u) & 7u); RECOMP_RL_SPILL return;" in t)
    check("straight-line: only changed slots stored",
          t is not None and "g_fp_stack[(x87_t + 6u) & 7u] = x87_r6; g_fp_stack[(x87_t + 7u) & 7u] = x87_r7;" in t)
    t, why = run("sub_00001100")
    check("branch to the same depth", t is not None and "(int)((x87_t + 7u) & 7u)" in t, )
    t, why = run("sub_00001200")
    check("call: stored before, base moved after",
          t is not None and "RECOMP_RL_RELOAD x87_t = (unsigned)(g_fp_top - 7) & 7u;" in t
          and "g_fp_stack[(x87_t + 7u) & 7u] = x87_r7; g_fp_top = (int)((x87_t + 7u) & 7u); RECOMP_RL_SPILL "
              "RECOMP_ABI_CALL" in t)
    t, why = run("sub_00001300")
    check("depth differs into a label: left", t is None and "depth differs" in why)
    t, why = run("sub_00001400")
    check("push under an if: left", t is None and why is not None)
    t, why = run("sub_00001500")
    check("unreachable code left as it was", t is not None and "fp_push(2.0);" in t and "recomp_x87_in(2.0)" not in t)
    t, why = run("sub_00001600")
    check("mostly calls: left", t is None and "more calls" in why)

    # The pass over the file, then again: nothing more to do.
    import subprocess
    here = pathlib.Path(__file__).resolve().parent
    out1 = subprocess.run([sys.executable, str(here / "jsrf_fp_locals.py"), d, "--apply"],
                          capture_output=True, text=True).stdout
    out2 = subprocess.run([sys.executable, str(here / "jsrf_fp_locals.py"), d],
                          capture_output=True, text=True).stdout
    check("fp pass applies", out1.startswith("4 functions"))
    check("fp pass idempotent", out2.startswith("0 functions"))

    # SIMD: locals and the stores around calls.
    simd = fn("sub_00002000", ["xmm0 = XMM_MUL(xmm0, XMM_MEM(eax));", CALL,
                               "xmm1 = XMM_ADD(xmm0, xmm1);", "XMM_STORE(ecx, xmm1);", RET])
    (gen / "recomp_0001.c").write_text("\n".join(simd))
    s1 = subprocess.run([sys.executable, str(here / "jsrf_simd_locals.py"), d, "--apply"],
                        capture_output=True, text=True).stdout
    txt = (gen / "recomp_0001.c").read_text()
    check("simd: locals declared from the thread's",
          "RecompXmm x_xmm0 = g_xmm0; RecompXmm x_xmm1 = g_xmm1;" in txt)
    check("simd: stored by the spill, loaded by the reload",
          "g_xmm0 = x_xmm0; g_xmm1 = x_xmm1;" in txt and "x_xmm0 = g_xmm0; x_xmm1 = g_xmm1;" in txt)
    check("simd: uses renamed", "x_xmm1 = XMM_ADD(x_xmm0, x_xmm1);" in txt)
    s2 = subprocess.run([sys.executable, str(here / "jsrf_simd_locals.py"), d],
                        capture_output=True, text=True).stdout
    check("simd pass idempotent", s2.startswith("0 functions"))

print(f"{len(FAILS)} failed" if FAILS else "all passed")
sys.exit(1 if FAILS else 0)
