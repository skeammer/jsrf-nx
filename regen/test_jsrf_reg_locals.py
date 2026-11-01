"""Cases for jsrf_reg_locals.py the generated tree may not happen to contain.

  python3 regen/test_jsrf_reg_locals.py
"""
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import jsrf_reg_locals as rl  # noqa: E402

FAILS = []


def body(*lines):
    return ["    RECOMP_PREEMPT();", "    uint32_t ebp = 0;", *lines]


def rewritten(b):
    out, *_ = rl.rewrite_function(b)
    return "\n".join(out)


def check(name, cond):
    if not cond:
        FAILS.append(name)
    print(("ok   " if cond else "FAIL ") + name)


def skips(name, b, why):
    try:
        rl.rewrite_function(b)
    except rl.Skip as e:
        check(name, why in str(e))
        return
    check(name + " (was rewritten)", False)


# Locals for exactly the registers named, the spill and reload macros, and
# the store at the end.
t = rewritten(body("    eax = MEM32(esp + 4);", "    sub_00012345();", "    ecx = eax;"))
check("locals for the registers used",
      "uint32_t g_eax = recomp_leaf_ld_eax(), g_ecx = recomp_leaf_ld_ecx(), "
      "g_esp = recomp_leaf_ld_esp();" in t and "g_edx" not in t)
check("call wrapped", "{ RECOMP_RL_SPILL sub_00012345(); RECOMP_RL_RELOAD }" in t)
check("stored at the end", t.rstrip().endswith("#undef RECOMP_RL_RELOAD")
      and "RECOMP_RL_SPILL /* registers back at the end */" in t)

# A tail call stores before the call and returns as it is.
t = rewritten(body("    eax = 1;", "    g_seh_ebp = ebp; sub_00012345(); return; /* tail jmp */"))
check("tail call", "g_seh_ebp = ebp; { RECOMP_RL_SPILL sub_00012345(); } return; /* tail jmp */" in t)

# A call that is an if's whole body is braced, and the return after it, which
# is not conditional, stores too.
t = rewritten(body("    if (eax) sub_00012345(); return;"))
check("conditional call then return",
      "if (eax) { RECOMP_RL_SPILL sub_00012345(); RECOMP_RL_RELOAD } "
      "{ RECOMP_RL_SPILL return; }" in t)
t = rewritten(body("    if (eax) goto loc_1; else sub_00012345();", "loc_1: ;"))
check("else call", "else { RECOMP_RL_SPILL sub_00012345(); RECOMP_RL_RELOAD }" in t)

# A conditional call on the line after its if.
t = rewritten(body("    if (eax)", "        sub_00012345(); return;"))
check("call under an if on the line before",
      "{ RECOMP_RL_SPILL sub_00012345(); RECOMP_RL_RELOAD } { RECOMP_RL_SPILL return; }" in t)

# Indirect call with the saved esp, ABI call, tail through a pointer.
t = rewritten(body(
    "    { uint32_t _icall_esp = g_esp;",
    "    { uint32_t _icall_target = MEM32(edx + 4); PUSH32(esp, 0x1u); "
    "RECOMP_ICALL_SAFE_AT(_icall_target, _icall_esp, 0x2u); } /* indirect call */",
    "    }",
    "    PUSH32(esp, 0x3u); RECOMP_ABI_CALL(0x4u, sub_00000004); /* call */",
    "    g_seh_ebp = ebp; RECOMP_ITAIL(MEM32(eax + 0xB8)); return;"))
check("indirect call", "PUSH32(esp, 0x1u); { RECOMP_RL_SPILL RECOMP_ICALL_SAFE_AT(_icall_target, "
      "_icall_esp, 0x2u); RECOMP_RL_RELOAD } }" in t)
check("ABI call", "PUSH32(esp, 0x3u); { RECOMP_RL_SPILL RECOMP_ABI_CALL(0x4u, sub_00000004); "
      "RECOMP_RL_RELOAD }" in t)
check("indirect tail", "{ RECOMP_RL_SPILL RECOMP_ITAIL(MEM32(eax + 0xB8)); } return;" in t)

# Calls in comments and strings are not calls; registers named only there
# get no locals.
t = rewritten(body("    eax = 1; /* sub_00012345(); ecx */", '    RECOMP_UNIMPL("mov edi, cr0", 0x5u);'))
check("comments and strings ignored",
      "g_ecx" not in t and "g_edi" not in t
      and '{ RECOMP_RL_SPILL RECOMP_UNIMPL("mov edi, cr0", 0x5u); RECOMP_RL_RELOAD }' in t)

# x87: the top index in a local, stored and loaded with the registers; a
# function that only defines the fp_* macros does not count.
t = rewritten(body("    #define fp_pop() (g_fp_top = (g_fp_top + 1u) & 7u)",
                   "    fp_push(1.0);", "    sub_00012345();"))
check("x87 top in a local", "int g_fp_top = recomp_fp_top_ld(); /* x87 top in a local */" in t
      and "recomp_fp_top_st(g_fp_top);" in t and "g_fp_top = recomp_fp_top_ld();" in t)
try:
    rl.rewrite_function(body("    #define fp_pop() (g_fp_top = (g_fp_top + 1u) & 7u)"))
    check("macro definition alone is not x87 use", False)
except rl.Skip as e:
    check("macro definition alone is not x87 use", "no registers" in str(e))

# Bitwise and is not an address taken.
t = rewritten(body("    eax = eax & ecx;", "    edx = (eax)&ebx;"))
check("bitwise and", "registers in locals" in t)

# What the pass refuses.
skips("unknown call", body("    eax = 1;", "    some_runtime_fn(eax);"), "calls some_runtime_fn")
skips("call over two lines", body("    eax = 1;", "    sub_00012345(", "    );"), "")
skips("call in an expression", body("    eax = 1 + RECOMP_ITAIL(eax);"), "inside an expression")
skips("address of a register", body("    helper_ptr = &eax;"), "address of a register")
skips("address of g_esp", body("    p = &g_esp;"), "address of a register")
skips("return in a macro", body("    #define BAIL() return;", "    eax = 1;"), "preprocessor")
skips("setjmp", body("    if (setjmp(*recomp_setjmp_slot(eax))) goto x;", "x: ;"), "setjmp")
skips("already rewritten", body("    uint32_t g_eax = recomp_leaf_ld_eax(); /* registers in locals */"),
      "already rewritten")
skips("nested call", body("    eax = 1;", "    RECOMP_UNIMPL(\"x\", (sub_00012345(), 0));"), "")

# Whole-file: idempotent, and the define lands before the include.
src = "\n".join(["#define RECOMP_GENERATED_CODE", '#include "recomp_funcs.h"', "",
                 "void sub_00000001(void)", "{", *body("    eax = 1;", "    return;"), "}", ""])
once, st = rl.rewrite(src)
twice, st2 = rl.rewrite(once)
check("file rewritten", st["rewritten"] == 1 and "#define RECOMP_REG_LOCALS 1\n\n#include" in once)
check("idempotent", once == twice and st2["rewritten"] == 0)

print(f"{len(FAILS)} failed" if FAILS else "all passed")
sys.exit(1 if FAILS else 0)
