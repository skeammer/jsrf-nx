"""Cases for jsrf_inline_leaves.py.

  python3 regen/test_jsrf_inline_leaves.py
"""
import pathlib
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import jsrf_inline_leaves as il  # noqa: E402

FAILS = []


def check(name, cond):
    if not cond:
        FAILS.append(name)
    print(("ok   " if cond else "FAIL ") + name)


def fn(name, regs, body, fp=False):
    out = [f"void {name}(void)", "{", "    RECOMP_PREEMPT();",
           "    uint32_t " + ", ".join(f"g_{r} = recomp_leaf_ld_{r}()" for r in regs)
           + "; /* registers in locals */"]
    if fp:
        out += ["    double *const g_fp_stack = recomp_fp_base(); int g_fp_top = recomp_fp_top_ld();"
                " /* x87 top in a local */"]
    out += ["#define RECOMP_RL_SPILL x", "#define RECOMP_RL_RELOAD y"]
    if fp:
        out += ["    #define fp_push(v) do { double _fp_value = (v); \\",
                "        g_fp_top = (g_fp_top + 7u) & 7u; \\",
                "        g_fp_stack[g_fp_top] = _fp_value; } while (0)",
                "    #define fp_pop() (g_fp_top = (g_fp_top + 1u) & 7u)"]
    out += ["", f"loc_{name[4:]}: ;"] + ["    " + b for b in body]
    if fp:
        out += ["    #undef fp_push", "    #undef fp_pop"]
    out += ["    RECOMP_RL_SPILL /* registers back at the end */",
            "#undef RECOMP_RL_SPILL", "#undef RECOMP_RL_RELOAD", "}", ""]
    return out


def call(va, ret="0x00011111u"):
    return (f"PUSH32(esp, {ret}); {{ RECOMP_RL_SPILL RECOMP_ABI_CALL(0x{va}u, sub_{va}); "
            f"RECOMP_RL_RELOAD }} /* call 0x{va} */")


RET = "esp += 4; { RECOMP_RL_SPILL return; } /* ret */"
leaf = fn("sub_00001000", ["eax", "esp"], ["eax = MEM32(esp + 4);", RET])
fpleaf = fn("sub_00001100", ["eax", "esp"], ["uint32_t _fa = 0, _fb = 0;",
                                              "fp_push(MEMF(esp + 4)); /* fld float */", RET], fp=True)
looping = fn("sub_00001200", ["eax", "esp"], ["eax = 1;", "if (eax) goto loc_00001200;",
                                              "if (eax == 2) goto loc_00001210;", RET,
                                              "loc_00001210: ;", "eax = 3;",
                                              "esp += 8; { RECOMP_RL_SPILL return; } /* ret 4 */"])
outside = fn("sub_00001400", ["eax", "esp"], ["if (eax) goto loc_00009999;", RET])
calls = fn("sub_00001300", ["eax", "esp"], ["{ RECOMP_RL_SPILL RECOMP_UNIMPL(\"x\", 1u); RECOMP_RL_RELOAD }", RET])
caller = fn("sub_00002000", ["eax", "ecx", "esp"],
            [call("00001000"), call("00001100"), call("00001200"), call("00001300"),
             "if (ecx) " + call("00001000"), RET])
caller_fp = fn("sub_00002100", ["eax", "esp"], [call("00001100"), RET], fp=True)
caller_noeax = fn("sub_00002200", ["ecx", "esp"], [call("00001000"), RET])

with tempfile.TemporaryDirectory() as d:
    gen = pathlib.Path(d)
    (gen / "recomp_0000.c").write_text("\n".join(leaf + fpleaf + looping + outside + calls + caller
                                                 + caller_fp + caller_noeax))
    texts, callees, why = il.scan(gen)
    check("straight-line leaf can be inlined", "sub_00001000" in callees)
    check("x87 leaf can be inlined", "sub_00001100" in callees and callees["sub_00001100"]["fp"])
    check("branches and two returns can be inlined", "sub_00001200" in callees
          and callees["sub_00001200"]["jumps"])
    check("a goto out of the function keeps it", "out of the function" in why.get("sub_00001400", ""))
    check("a wrapped call keeps it", "RECOMP_RL_SPILL" in why.get("sub_00001300", ""))
    out, stats, per = il.rewrite(texts, callees)
    text = "\n".join(next(iter(out.values())))
    check("inlined with the push kept",
          "PUSH32(esp, 0x00011111u); { /* sub_00001000 inlined */ RECOMP_PREEMPT(); "
          "eax = MEM32(esp + 4); esp += 4; /* ret */ } /* call 0x00001000 */" in text)
    check("under an if too", "if (ecx) PUSH32(esp, 0x00011111u); { /* sub_00001000 inlined */" in text)
    check("x87 leaf only where the caller has the x87 top",
          per.get("sub_00001100") == 1 and "RECOMP_ABI_CALL(0x00001100u, sub_00001100)" in text)
    check("a caller without the register keeps the call",
          text.count("RECOMP_ABI_CALL(0x00001000u, sub_00001000)") == 1)
    check("the callee's labels are left out", text.count("loc_00001000: ;") == 1)
    check("labels named per site, returns jump to the end",
          "{ /* sub_00001200 inlined */ RECOMP_PREEMPT(); loc_00001200_i00011111: ; eax = 1; "
          "if (eax) goto loc_00001200_i00011111; if (eax == 2) goto loc_00001210_i00011111; "
          "esp += 4; goto sub_00001200_end00011111; /* ret */ loc_00001210_i00011111: ; eax = 3; "
          "esp += 8; goto sub_00001200_end00011111; /* ret 4 */ sub_00001200_end00011111: ; }" in text)
    check("a wrapped call stays a call", "RECOMP_ABI_CALL(0x00001300u" in text)
    for f, lines in out.items():
        f.write_text("\n".join(lines))
    texts2, callees2, _ = il.scan(gen)
    out2, stats2, _ = il.rewrite(texts2, callees2)
    check("idempotent", stats2["sites"] == 0 and not out2)
    check("a rewritten caller is not inlined itself",
          "sub_00002100" not in callees2 and "sub_00002000" not in callees2)

print(f"{len(FAILS)} failed" if FAILS else "all passed")
sys.exit(1 if FAILS else 0)
