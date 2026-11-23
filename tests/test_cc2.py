#!/usr/bin/env python3
"""The language features added in 2.0: 64-bit integers, function pointers,
static locals and #include.

As everywhere else here, each case is compiled by the on-device compiler and
the machine code it produces is executed. A 64-bit result that a double
could not hold is the point of several of these: if the compiler quietly
fell back to doubles, 2^53+1 would come back as 2^53.
"""
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ccemu import Machine, Program, run_source          # noqa: E402

fails = []
count = 0


def check(cond, what):
    global count
    count += 1
    if not cond:
        fails.append(what)
        print("FAIL: " + what)


def run(src, want, what, fn="main"):
    try:
        got = run_source(src).call(fn)
    except Exception as e:                               # noqa: BLE001
        check(False, "%s: %s" % (what, e))
        return
    check(got == want, "%s: got %r, expected %r" % (what, got, want))


# ------------------------------------------------------------ long long

run("long long f(void){ return 4000000000LL * 3LL; }"
    "int main(void){ return (int)(f() / 1000000LL); }",
    12000, "a product that overflows 32 bits stays exact")

run("int main(void){ long long a = 2147483647LL; a = a + a; return (int)(a >> 31); }",
    1, "addition carries into the high word")

run("int main(void){ long long a = 1LL; a = a << 40; return (int)(a >> 35); }",
    32, "shifts move bits across the word boundary")

run("int main(void){ long long a = 9007199254740993LL; "
    "return (int)(a - 9007199254740992LL); }",
    1, "2^53+1 keeps its last bit, which a double would lose")

run("int main(void){ long long a = -7000000000LL; return (int)(a / 1000000000LL); }",
    -7, "signed division keeps its sign")

run("int main(void){ unsigned long long a = 18000000000000000000ULL; "
    "return (int)(a / 1000000000000000000ULL); }",
    18, "an unsigned value above 2^63 divides as unsigned")

run("int main(void){ long long a = 5000000000LL, b = 4999999999LL; "
    "return (a > b) + (a < b) * 10 + (a == b) * 100; }",
    1, "comparison uses the whole 64 bits")

run("int main(void){ long long a = 10000000001LL; return (int)(a % 7LL); }",
    5, "remainder is exact past 32 bits")

run("int main(void){ int i = -3; long long a = i; a = a * 1000000000LL; "
    "return (int)(a / 1000000000LL); }",
    -3, "an int widens to 64 bits with its sign")

run("int main(void){ long long a = 9000000000LL; double d = a; "
    "return (int)(d / 1000000000.0); }",
    9, "a 64-bit integer converts to double")

run("int main(void){ double d = 1.23e10; long long a = (long long)d; "
    "return (int)(a / 1000000000LL); }",
    12, "a cast from double truncates toward zero")

run("int main(void){ long long t[3]; t[0]=1LL<<40; t[1]=t[0]*2LL; t[2]=t[1]+t[0]; "
    "return (int)(t[2] >> 40); }",
    3, "an array of 64-bit integers indexes correctly")

run("long long fact(int n){ return n <= 1 ? 1LL : n * fact(n - 1); }"
    "int main(void){ return (int)(fact(20) / 1000000000000000LL); }",
    2432, "20! needs 62 bits and recursion carries it")

m = run_source('#include <stdio.h>\n'
               'int main(void){ long long a = 9007199254740993LL;'
               ' printf("%lld", a); return 0; }')
m.call("main")
check(m.text() == "9007199254740993",
      "printf %lld prints all 64 bits, not a rounded double")

# ------------------------------------------------------- function pointers

run("int five(void){ return 5; } int main(void){ int (*p)(void); p = five; return p(); }",
    5, "a call through a function pointer reaches the function")

run("int inc(int x){ return x + 1; } "
    "int main(void){ int (*p)(int); p = inc; return p(41); }",
    42, "an argument passes through an indirect call")

run("double twice(double x){ return x * 2.0; } "
    "double half(double x){ return x / 2.0; } "
    "double apply(double (*f)(double), double x){ return f(x); } "
    "int main(void){ return (int)(apply(twice, 10.0) + apply(half, 10.0)); }",
    25, "a function pointer parameter takes and returns doubles")

run("int a(void){return 1;} int b(void){return 2;} int c(void){return 4;} "
    "int main(void){ int (*t[3])(void); int s = 0, i; "
    "t[0]=a; t[1]=b; t[2]=c; for (i=0;i<3;i++) s += t[i](); return s; }",
    7, "an array of function pointers dispatches like a jump table")

run("int call(int (*f)(int), int v){ return f(v); } "
    "int dbl(int x){ return x * 2; } "
    "int main(void){ return call(dbl, 21); }",
    42, "a function used before it is defined still resolves")

run("int add(int a, int b){ return a + b; } "
    "int main(void){ int (*p)(int,int) = add; return p(20, 22); }",
    42, "a function pointer can be initialised where it is declared")

# ---------------------------------------------------------- static locals

run("int counter(void){ static int n = 10; n++; return n; } "
    "int main(void){ counter(); counter(); return counter(); }",
    13, "a static local keeps its value between calls")

run("double acc(double x){ static double t = 0.5; t = t + x; return t; } "
    "int main(void){ acc(1.0); acc(2.0); return (int)(acc(0.0) * 10); }",
    35, "a static double is initialised once, not on every call")

run("int tbl(int i){ static int t[4] = {5,6,7,8}; t[i] += 100; return t[i]; } "
    "int main(void){ tbl(2); return tbl(2); }",
    207, "a static array keeps its initialiser and its updates")

run("int z(void){ static int n; n++; return n; } "
    "int main(void){ z(); return z(); }",
    2, "a static local with no initialiser starts at zero")

run("int f(void){ static int n = 1; return n; } "
    "int g(void){ static int n = 2; return n; } "
    "int main(void){ return f() * 10 + g(); }",
    12, "statics in different functions are different objects")

# --------------------------------------------------------------- #include

d = tempfile.mkdtemp()
with open(os.path.join(d, "mylib.h"), "w") as f:
    f.write("#define SCALE 7\ndouble triple(double x){ return x * 3.0; }\n")
with open(os.path.join(d, "second.h"), "w") as f:
    f.write('#include "mylib.h"\nint extra(void){ return SCALE; }\n')
with open(os.path.join(d, "prog.c"), "w") as f:
    f.write('#include <stdio.h>\n#include "mylib.h"\n#include "second.h"\n'
            'int main(void){ return (int)triple(2.0) * SCALE + extra(); }\n')

try:
    m = Machine(Program(os.path.join(d, "prog.c")))
    got = m.call("main")
except Exception as e:                                   # noqa: BLE001
    got = "error: %s" % e
check(got == 49,
      "#include pulls in a header, and a header included twice is read once"
      " (got %r)" % (got,))

with open(os.path.join(d, "bad.c"), "w") as f:
    f.write('#include "nosuch.h"\nint main(void){ return 0; }\n')
try:
    Program(os.path.join(d, "bad.c"))
    check(False, "a missing include is reported as an error")
except RuntimeError as e:
    check("nosuch.h" in str(e), "a missing include names the file it wanted")

print("   %d/%d language tests passed" % (count - len(fails), count))
sys.exit(1 if fails else 0)
