#!/usr/bin/env python3
"""Every example must compile, and the ones that only compute must run.

Examples are documentation that can rot: a function renamed in the runtime
breaks them silently, and the person who finds out is holding the hardware.
Compiling them here means the shipped examples always match the shipped
compiler.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ccemu import Machine, Program                   # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EX = os.path.join(ROOT, "examples")

fails = []
count = 0


def check(cond, what):
    global count
    count += 1
    if not cond:
        fails.append(what)
        print("FAIL: " + what)


progs = {}
for name in sorted(os.listdir(EX)):
    if not name.endswith(".c"):
        continue
    try:
        progs[name] = Program(os.path.join(EX, name))
        ok, why = True, ""
    except Exception as e:                           # noqa: BLE001
        ok, why = False, str(e)
    check(ok, "examples/%s compiles%s" % (name, "" if ok else ": " + why))

check(len(progs) >= 6, "the examples directory is not empty")

# bigint.c computes and prints; the answers are checkable
if "bigint.c" in progs:
    m = Machine(progs["bigint.c"])
    m.call("main")
    out = m.text()
    check("20! = 2432902008176640000" in out,
          "bigint.c computes 20! exactly, which needs 62 bits")
    check("fib(90) = 2880067194370816120" in out,
          "bigint.c computes the 90th Fibonacci number exactly")
    check("2^53+1 = 9007199254740993" in out,
          "bigint.c keeps the bit a double would drop")
    check("table[1](25) = 75025" in out,
          "bigint.c dispatches through a table of function pointers")
    check("called 3 times" in out, "bigint.c's static local counts across calls")

# hello.c is callable from the prompt, so its exported function must work
if "hello.c" in progs:
    m = Machine(progs["hello.c"])
    got = m.call("simpson", (0.0, 2.0, 1000.0), want_double=True)
    # integral of x^3 - 2x over [0,2] is 4 - 4 = 0
    check(abs(got) < 1e-9,
          "hello.c integrates x^3-2x over [0,2] to zero (got %g)" % got)

# store.c keeps its table in flash and reads it back by pointer
if "store.c" in progs:
    m = Machine(progs["store.c"])
    m.call("main")
    got = m.call("lookup", (500.0,), want_double=True)
    check(got == 250000.0,
          "store.c reads 500^2 back out of the flash store (got %g)" % got)

print("   %d/%d example checks passed" % (count - len(fails), count))
sys.exit(1 if fails else 0)
