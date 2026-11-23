#!/usr/bin/env python3
"""CalcOS regression suite.

Each case is (expression, expected, tolerance).  A tolerance of None means the
printed string must match exactly; otherwise the result is parsed as a float and
compared with a relative tolerance.  Reference values come from exact identities
or from high-precision evaluation.
"""
import subprocess, sys, os, math

BIN = os.path.join(os.path.dirname(__file__), "..", "build-host", "calcos")

CASES = [
    # --- parser and basic arithmetic ---
    ("2+3*4",                      14, 0),
    ("2^3^2",                      512, 0),          # right associative
    ("-2^2",                       -4, 0),
    ("(1+2)*(3-1)/4",              1.5, 0),
    ("10 % 3",                     1, 0),
    ("5!",                         120, 1e-12),
    ("2 < 3",                      1, 0),

    # --- complex ---
    ("sqrt(-4)",                   "4i" if False else "2i", None),
    ("(3+4i)*(1-2i)",              "11 - 2i", None),
    ("abs(3+4i)",                  5, 0),
    ("re(exp(i*pi))",              -1, 1e-15),
    ("arg(i)",                     math.pi/2, 1e-15),

    # --- IEEE-754 inspection ---
    ("ulp(1)",                     2**-52, 0),
    ("nextup(1) - 1",              2**-52, 0),
    ("hex(0.5)",                   "0x1p-1", None),
    ("ulpdiff(0.1+0.2, 0.3)",      1, 0),
    ("isnan(0/0)",                 1, 0),
    ("exponent(1024)",             10, 0),

    # --- compensated primitives ---
    ("sum([1e16, 1, -1e16])",      1, 0),            # naive sum gives 0
    ("dot([1e8,1,-1e8],[1,1,1])",  1, 0),
    ("1/3*3 - 1",                  0, 0),            # rounds to exactly 1
    ("fma(1/3, 3, -1) != 0",       1, 0),            # fma sees the real residual

    # --- extended precision (double-double) ---
    ("hi(dd(1)/dd(3)) + lo(dd(1)/dd(3)) == hi(dd(1)/dd(3))", 1, 0),
    ("dd(2)^0.5",   "1.4142135623730950488016887242097e+0", None),
    ("dd(1)/dd(3)", "3.3333333333333333333333333333333e-1", None),
    ('dd("3.14159265358979323846264338327950")',
                    "3.1415926535897932384626433832795e+0", None),

    # --- intervals ---
    ("wid(iv(1,2)) >= 1",          1, 0),            # widths round outward
    ("inf(iv(1,2)*iv(3,4))",       3, 1e-15),
    ("sup(iv(1,2)*iv(3,4))",       8, 1e-15),
    ("inf(sin(iv(0,3.5))) < -0.35", 1, 0),
    ("sup(sin(iv(0,3.5))) >= 1",   1, 0),

    # --- special functions (references: exact identities / mpmath) ---
    ("gamma(0.5)^2",               math.pi, 1e-14),
    ("gamma(6)",                   120, 1e-14),
    ("lgamma(100)",                359.1342053695754, 1e-14),
    ("digamma(1)",                 -0.5772156649015329, 1e-13),
    ("erf(1)",                     0.8427007929497149, 1e-15),
    ("erfc(10)",                   2.0884875837625447e-45, 1e-12),
    ("zeta(2)",                    math.pi**2/6, 1e-14),
    ("zeta(-1)",                   -1/12, 1e-12),
    ("besselj0(1)",                0.7651976865579666, 1e-14),
    ("besselj0(20)",               0.16702466434058315, 1e-13),
    ("besselj1(1)",                0.4400505857449335, 1e-14),
    ("bessely0(1)",                0.08825696421567696, 1e-13),
    ("bessely1(5)",                0.14786314339122683, 1e-12),
    ("ei(1)",                      1.8951178163559368, 1e-14),
    ("li(2)",                      1.0451637801174927, 1e-14),
    ("binomial(50,25)",            126410606437752, 1e-9),

    # --- linear algebra ---
    ("det([4,-2;1,1])",            6, 1e-14),
    ("trace([1,2;3,4])",           5, 0),
    ("norm([3,4])",                5, 0),
    ("inv([4,-2;1,1])(1,1)",       1/6, 1e-14),
    ("solve([4,-2;1,1],[2;3])(1)", 4/3, 1e-15),
    ("solve([4,-2;1,1],[2;3])(2)", 5/3, 1e-15),
    ("cond(eye(3))",               1, 0),
    ("numel(fft([1,2,3,4]))",      4, 0),
    ("re(fft([1,2,3,4])(1))",      10, 1e-14),
    ("re(ifft(fft([1,2,3,4]))(2))", 2, 1e-14),

    # --- statistics ---
    ("mean([1,2,3,4])",            2.5, 0),
    ("median([3,1,2])",            2, 0),
    ("std([2,4,4,4,5,5,7,9])",     2.138089935299395, 1e-14),

    # --- numerical methods ---
    ("integrate(@(x) x^3, 0, 2)",          4, 1e-14),
    ("integrate(@(x) 1/sqrt(x), 0, 1)",    2, 1e-15),       # endpoint pole
    ("integrate(@(x) log(x), 0, 1)",       -1, 1e-15),
    ("integrate(@(x) exp(-x^2), 0, 10)",   math.sqrt(math.pi)/2, 1e-15),
    ("integrate(@(x) 1/(1+x^2), 0, 1)",    math.pi/4, 1e-15),
    ("integrate(@(x) sin(x), 0, 3)",       1-math.cos(3), 1e-14),
    ("root(@(x) x^2-2, 0, 2)",             math.sqrt(2), 1e-14),
    ("newton(@(x) cos(x)-x, 1)",           0.7390851332151607, 1e-13),
    ("diff(@(x) sin(x), 1)",               math.cos(1), 1e-13),
    ("diff(@(x) x^3, 2, 2)",               12, 1e-10),
    ("minimize(@(x) (x-2)^2+1, 0, 5)",     2, 1e-7),
    ("ode(@(t,y) -2*y, 0, 1, 1)",          math.exp(-2), 1e-9),
    ("nsum(@(k) 1/k^2, 1, 100000)",        math.pi**2/6 - 1e-5, 1e-7),
    ("polyval([1,0,-2], 3)",               7, 0),

    # --- language features ---
    ("f(x) = x^2 + 1",             "function defined", None),
    ("g(x,y) = x*y - 1",           "function defined", None),
    ("fib(n) = n < 2",             "function defined", None),
    ("numel(1:0.5:3)",             5, 0),
    ("sum(linspace(0,1,101))",     50.5, 1e-13),
    ("typeof(iv(1,2))",            "interval", None),
    ("typeof([1,2])",              "matrix", None),
    ("size([1,2;3,4])(2)",         2, 0),
    ("numel([ [1,2], [3,4] ])",    4, 0),
    ("[1,2;3,4]'(1,2)",            3, 0),   # transpose, then index

    # --- error handling must not crash the interpreter ---
    ("1/0",                        "inf", None),
    ("solve([1,1;1,1],[1;2])",     "error: matrix is singular", None),
    ("nosuchthing(2)",             "error: unknown name 'nosuchthing'", None),
    ("[1,2] + [1,2;3,4]",          "error: dimension mismatch: 1x2 vs 2x2", None),
    ("2 +",                        "error: unexpected end of expression", None),
    # --- units: a quantity is a number plus seven exponents ---
    ("5 m/s * 3 s",                "15 m", None),
    ("3 km + 200 m",               "3200 m", None),
    ("70 kg * 9.81 m/s^2",         "686.7 N", 1e-12),
    ("1 kg m/s^2",                 "1 N", None),
    ("16 m^2",                     "16 m^2", None),       # the power is the unit's
    ("sqrt(16 m^2)",               "4 m", None),
    ("abs(-3 m)",                  "3 m", None),
    ("60 mile/hour",               "26.8224 m/s", 1e-12),
    ("1 ft + 1 in",                "0.3302 m", 1e-12),
    ("(2 m)^3",                    "8 m^3", None),
    ("1 L",                        "0.001 m^3", None),
    ("to(3000 m, \"km\")",          3, 0),
    ("to(1 hour, \"min\")",         60, 0),
    ("conv(5, \"mile\", \"km\")",   8.04672, 1e-12),
    ("conv(100, \"C\", \"C\")",     100, 0),
    ("conv(1, \"kWh\", \"J\")",     3.6e6, 1e-12),
    ("to(1 atm, \"kPa\")",          101.325, 1e-12),
    ("to(1 year, \"day\")",         365.25, 1e-12),

    # --- physical constants ---
    ("c_light",                    "299792458 m/s", None),
    ("to(c_light, \"km/s\")",       299792.458, 1e-12),
    ("k_B * 300 K",                "4.141947e-21 J", 1e-9),
    ("const(\"N_A\") * 0",          "0 1/mol", None),
    ("h_planck * c_light / (500 nm)", "3.972891714e-19 J", 1e-9),
    ("g0",                         "9.80665 m/s^2", 1e-12),

    # --- statistics ---
    ("fit([0,1,2,3,4],[1,3,5,7,9])(1)",  2, 1e-12),   # slope
    ("fit([0,1,2,3,4],[1,3,5,7,9])(2)",  1, 1e-12),   # intercept
    ("fit([0,1,2,3,4],[1,3,5,7,9])(3)",  1, 1e-12),   # r, an exact fit
    ("fit([1,2,3,4],[2.1,3.9,6.2,7.8])(1)", 1.94, 1e-9),
    ("fit([1,2,3,4],[2.1,3.9,6.2,7.8])(2)", 0.15, 1e-9),
    ("corr([1,2,3],[2,4,6])",      1, 1e-12),
    ("corr([1,2,3],[6,4,2])",      -1, 1e-12),
    ("quantile([1,2,3,4,5], 0.5)", 3, 0),
    ("quantile([1,2,3,4], 0.5)",   2.5, 0),
    ("quantile([1,2,3,4,5], 0)",   1, 0),
    ("quantile([1,2,3,4,5], 1)",   5, 0),
    ("mode([1,2,2,3,2,4])",        2, 0),
    ("median([3,1,2])",            2, 0),
    ("std([2,4,4,4,5,5,7,9])",     2.138089935299395, 1e-12),

    # a fit through points whose x values are large and nearly equal: the
    # textbook formula loses almost all its digits here
    ("fit([1e9,1e9+1,1e9+2],[1,3,5])(1)", 2, 1e-9),
    # --- big integers at the prompt ---
    ("big(2)^100",                 "1267650600228229401496703205376", None),
    ("bigfact(30)",                "265252859812191058636308480000000", None),
    ("big(2)^64 - 1",              "18446744073709551615", None),
    ("powmod(2,1000,1000000007)",  688423210, 0),
    ("gcd(1071,462)",              21, 0),
    ("lcm(4,6)",                   12, 0),
    ("ndigits(bigfact(100))",      158, 0),
    ("big(\"123456789012345678901234567890\") * 2",
                                   "246913578024691357802469135780", None),
    ("big(10)^20 / big(10)^5",     "1000000000000000", None),   # exact
    ("big(7) / big(2)",            3.5, 1e-15),                 # not exact
    ("big(100) > big(99)",         1, 0),
    ("big(-5) + 5",                "0", None),
    ("str(big(2)^70)",             "1180591620717411303424", None),

    # --- convolution and correlation ---
    ("convolve([1,2,3],[1,1])(2)", 3, 0),
    ("convolve([1,2,3],[1,1])(3)", 5, 0),
    ("numel(convolve([1,2,3],[1,1]))", 4, 0),
    ("xcorr([1,2,3],[1])(2)",      2, 0),
]


def run(expr):
    p = subprocess.run([BIN, "-e", ":digits 17", expr],
                       capture_output=True, text=True, timeout=120)
    lines = [l for l in p.stdout.strip().split("\n") if l.strip()]
    return lines[-1].strip() if len(lines) > 1 else ""


def main():
    fails = []
    for expr, want, tol in CASES:
        got = run(expr)
        if tol is None:
            ok = (got == want)
        elif isinstance(want, str):
            # "686.7 N": the number within tolerance, the unit exactly
            try:
                wn, wu = want.split(" ", 1)
                gn, gu = got.split(" ", 1)
                ok = gu == wu and abs(float(gn) - float(wn)) <= \
                     tol * max(1.0, abs(float(wn)))
            except ValueError:
                ok = False
        else:
            try:
                g = float(got)
                ok = (g == want) if tol == 0 else \
                     abs(g - want) <= tol * max(1.0, abs(want))
            except ValueError:
                ok = False
        if not ok:
            fails.append((expr, want, got))
    total = len(CASES)
    for expr, want, got in fails:
        print("FAIL  %-42s want %-28s got %s" % (expr, want, got))
    print("\n%d/%d passed" % (total - len(fails), total))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
