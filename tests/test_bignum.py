#!/usr/bin/env python3
"""The big-integer core, against Python's own integers.

Division is the piece that is easy to get subtly wrong — Knuth's algorithm D
has two correction steps that almost never fire, so a bug in them survives
any amount of casual testing. This throws a few thousand random pairs at it,
weighted toward the sizes and shapes where the corrections do fire: divisors
whose top limb is just below a power of two, operands that differ by one
limb, and values with long runs of 0xFFFFFFFF.
"""
import os
import random
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, "build-host", "bigtest")

fails = []
count = 0


def run(*args):
    r = subprocess.run([EXE] + [str(a) for a in args],
                       capture_output=True, text=True)
    return r.stdout.strip()


def check(got, want, what):
    global count
    count += 1
    if str(got) != str(want):
        fails.append(what)
        if len(fails) <= 10:
            print("FAIL: %s\n      got  %s\n      want %s" % (what, got, want))


# ------------------------------------------------------------- the basics

check(run("add", 0, 0), 0, "0 + 0")
check(run("add", "-5", 5), 0, "-5 + 5 is zero, not minus zero")
check(run("sub", 5, 5), 0, "5 - 5")
check(run("mul", 0, "12345678901234567890"), 0, "anything times zero")
check(run("roundtrip", "-0"), 0, "minus zero parses as zero")
check(run("roundtrip", "0x1f"), 31, "hex parses")
check(run("roundtrip", "00123"), 123, "leading zeros")
check(run("roundtrip", "1_000_000"), 1000000, "digit separators are ignored")
check(run("div", 7, 0), "ERR", "division by zero is refused")
check(run("mod", 7, 0), "ERR", "so is the remainder")
check(run("fact", 0), 1, "0! is 1")
check(run("fact", 20), 2432902008176640000, "20!")
check(run("fact", 100), __import__("math").factorial(100), "100!")

# signs, which is where truncating division earns its keep
for a, b in [(7, 3), (-7, 3), (7, -3), (-7, -3), (10 ** 30 + 1, -7)]:
    q = abs(a) // abs(b)
    q = q if (a < 0) == (b < 0) else -q
    r = a - q * b
    check(run("div", a, b), q, "%d / %d" % (a, b))
    check(run("mod", a, b), r, "%d %% %d" % (a, b))

check(run("cmp", 100, 99), 1, "100 > 99")
check(run("cmp", -100, 99), -1, "-100 < 99")
check(run("cmp", "12345678901234567890", "12345678901234567890"), 0,
      "equal big values compare equal")
check(run("cmp", "-12345678901234567891", "-12345678901234567890"), -1,
      "negative values compare by magnitude the right way round")

# ------------------------------------------------------ powers and moduli

check(run("pow", 2, 100), 2 ** 100, "2^100")
check(run("pow", 3, 0), 1, "anything to the zero")
check(run("pow", -2, 101), (-2) ** 101, "a negative base keeps its sign")
check(run("powmod", 2, 1000, 1000000007), pow(2, 1000, 1000000007),
      "2^1000 mod a prime")
check(run("powmod", "123456789012345678901234567890", 65537,
          "1000000000000000003"),
      pow(123456789012345678901234567890, 65537, 1000000000000000003),
      "a modular exponentiation of the size RSA would use")
check(run("gcd", 1071, 462), 21, "gcd(1071, 462)")
check(run("gcd", 2 ** 64, 2 ** 32), 2 ** 32, "gcd of two powers of two")
check(run("gcd", 0, 17), 17, "gcd with zero")

check(run("shl", 1, 200), 2 ** 200, "shifting one up 200 places")
check(run("shr", 2 ** 200, 199), 2, "and back down")
check(run("shr", 5, 100), 0, "shifting past the end gives zero")

check(run("todouble", 2 ** 53), repr(float(2 ** 53)).rstrip("0").rstrip("."),
      "exact conversion to double")

# ------------------------------------------------- random, weighted to hurt

random.seed(20260220)


def nasty(bits):
    """Values of a shape that makes the correction steps in division fire."""
    kind = random.randrange(5)
    if kind == 0:
        return random.getrandbits(bits)
    if kind == 1:                       # long runs of ones
        return (1 << bits) - 1 - random.getrandbits(bits // 8 or 1)
    if kind == 2:                       # top limb just under a power of two
        return (1 << bits) - random.randrange(1, 1 << 16)
    if kind == 3:                       # a few limbs then zeros
        return random.getrandbits(32) << (bits - 32)
    return random.getrandbits(bits) | (1 << (bits - 1))


trials = 0
for _ in range(900):
    abits = random.choice([33, 64, 65, 96, 128, 200, 256, 512])
    bbits = random.choice([33, 64, 65, 96, 128, 200])
    a = nasty(abits)
    b = nasty(bbits) or 1
    if random.random() < 0.5:
        a = -a
    if random.random() < 0.3:
        b = -b
    q = abs(a) // abs(b)
    q = q if (a < 0) == (b < 0) else -q
    r = a - q * b
    got_q = run("div", a, b)
    got_r = run("mod", a, b)
    trials += 1
    if got_q != str(q) or got_r != str(r):
        check("%s,%s" % (got_q, got_r), "%d,%d" % (q, r),
              "divmod(%d, %d)" % (a, b))
    else:
        count += 2

for _ in range(300):
    a = random.getrandbits(random.choice([40, 128, 300]))
    b = random.getrandbits(random.choice([40, 128, 300]))
    if random.random() < 0.5:
        a = -a
    if random.random() < 0.5:
        b = -b
    for op, want in (("add", a + b), ("sub", a - b), ("mul", a * b)):
        got = run(op, a, b)
        trials += 1
        if got != str(want):
            check(got, want, "%s(%d, %d)" % (op, a, b))
        else:
            count += 1

print("   %d/%d big-integer checks passed (%d random)"
      % (count - len(fails), count, trials))
sys.exit(1 if fails else 0)
