#!/usr/bin/env python3
"""Generate the function and settings reference from the source tables.

Documentation that is retyped goes stale. These tables are the same ones the
firmware uses at runtime, so what the README lists is what the device has.
"""
import re, sys, os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

GROUPS = [
    ("Elementary", ["sqrt","cbrt","exp","log","log2","log10","log1p","expm1","abs",
                    "sign","floor","ceil","round","trunc","mod","rem","pow","fma",
                    "min","max","hypot"]),
    ("Trigonometry", ["sin","cos","tan","asin","acos","atan","atan2","sinh","cosh",
                      "tanh","asinh","acosh","atanh"]),
    ("Complex", ["re","im","conj","arg","polar"]),
    ("Special functions", ["gamma","lgamma","digamma","beta","erf","erfc","zeta",
                           "besselj0","besselj1","bessely0","bessely1","ei","li",
                           "factorial","binomial"]),
    ("Floating-point inspection", ["ulp","nextup","nextdown","relerr","ulpdiff",
                                   "bits","hex","exponent","significand","isnan","isinf"]),
    ("Extended precision and intervals", ["dd","hi","lo","iv","inf","sup","wid","mid"]),
    ("Building values", ["zeros","ones","eye","rand","randn","seed","linspace",
                         "size","numel","reshape","transpose","diag"]),
    ("Linear algebra", ["det","inv","solve","trace","norm","cond","dot","cross"]),
    ("Statistics", ["sum","prod","mean","var","std","median","mode","quantile",
                    "corr","fit","sort","cumsum"]),
    ("Units and constants", ["conv","to","unit","const"]),
    ("Exact integers", ["big","bigfact","powmod","gcd","lcm","ndigits","str"]),
    ("Numerical methods", ["polyval","integrate","root","newton","diff","minimize",
                           "ode","nsum","fft","ifft","convolve","xcorr"]),
    ("Other", ["plot","typeof","print"]),
]


def builtins():
    src = open(os.path.join(ROOT, "src", "builtins.c")).read()
    tbl = src[src.index("static const Builtin table[]"):]
    out = {}
    for m in re.finditer(r'\{"([A-Za-z0-9_]+)",\s*(\d+)\s*,\s*([A-Za-z0-9_]+)\s*,\s*'
                         r'bi_[A-Za-z0-9_]+\s*,\s*"((?:[^"\\]|\\.)*)"\s*\}', tbl):
        name, lo, hi, help_ = m.group(1), m.group(2), m.group(3), m.group(4)
        hi = "8" if not hi.isdigit() else hi
        args = lo if lo == hi else "%s-%s" % (lo, hi)
        out[name] = (args, help_.replace('\\"', '"'))
    return out


def settings():
    src = open(os.path.join(ROOT, "src", "settings.c")).read()
    tbl = src[src.index("static const Entry table[]"):]
    rows = []
    for m in re.finditer(r'\{\s*"([a-z]+)",\s*(V_\w+),\s*([^,]+),\s*([^,]+),\s*'
                         r'OFF\(\w+\),\s*\n?\s*"((?:[^"\\]|\\.)*)"\s*\}', tbl):
        rows.append((m.group(1), m.group(2), m.group(3).strip(),
                     m.group(4).strip(), m.group(5)))
    defaults = {}
    d = src[src.index("void settings_defaults"):src.index("/* ------", src.index("void settings_defaults"))]
    for m in re.finditer(r'g_set\.(\w+)\s*=\s*([^;]+);', d):
        defaults[m.group(1)] = m.group(2).strip()
    return rows, defaults


FIELD = {"cpu": "cpu_mhz", "splash": "splash", "fg": "fg", "bg": "bg",
         "font": "font", "backlight": "backlight", "kbdlight": "kbd_light",
         "blswap": "bl_swap", "lcdbits": "lcd_bits", "lcdmadctl": "lcd_madctl",
         "lcdinvert": "lcd_invert", "lcdspi": "lcd_spi_mhz",
         "digits": "digits", "ddigits": "ddigits", "deg": "deg"}


SIG = {"d": "double", "i": "int", "p": "char *", "v": "void"}


def runtime_api():
    """The entry points compiled C can call, from the firmware's own table."""
    src = open(os.path.join(ROOT, "src", "runtime.c")).read()
    tbl = src[src.index("static const RtSym rt_table[]"):]
    out = []
    for m in re.finditer(r'RT\("([A-Za-z0-9_]+)",\s*[A-Za-z0-9_]+,\s*"([a-z.]+)"\)', tbl):
        name, sig = m.group(1), m.group(2)
        if name.startswith("__aeabi_"):
            continue                      # emitted by the compiler, not called by hand
        ret = SIG.get(sig[0], "int")
        params = []
        for ch in sig[1:]:
            if ch == ".":
                params.append("...")
            else:
                params.append(SIG.get(ch, "int"))
        out.append((name, ret, params))
    return out


def emit_functions():
    b = builtins()
    seen = set()
    for title, names in GROUPS:
        rows = [(n, b[n]) for n in names if n in b]
        if not rows:
            continue
        print("**%s**\n" % title)
        print("| Function | Args | Description |")
        print("|---|---|---|")
        for n, (args, h) in rows:
            print("| `%s` | %s | %s |" % (n, args, h))
            seen.add(n)
        print()
    missing = sorted(set(b) - seen)
    if missing:
        print("**Uncategorised**\n")
        print("| Function | Args | Description |")
        print("|---|---|---|")
        for n in missing:
            print("| `%s` | %s | %s |" % (n, b[n][0], b[n][1]))
        print()
    print("%d functions in total." % len(b))


def emit_api():
    api = runtime_api()
    groups = [("Console", ["printf", "putchar", "puts"]),
              ("Maths", ["sin","cos","tan","asin","acos","atan","atan2",
                         "sinh","cosh","tanh","exp","log","log10","log2",
                         "pow","sqrt","cbrt","fabs","floor","ceil","round",
                         "trunc","fmod","hypot","fmin","fmax"]),
              ("The calculator's own special functions",
               [n for n, _, _ in api if n.endswith("_fn")]),
              ("Memory and strings", ["malloc","free","memset","memcpy","strlen",
                                      "strcmp","strcpy","strncpy","strcat","abs"]),
              ("Drawing on the panel",
               [n for n, _, _ in api if n.startswith("plot_")] + ["rgb"]),
              ("Keyboard, time and sound",
               ["getchar", "getkey", "kbhit", "getline_", "millis", "micros",
                "delay", "tone", "notone"]),
              ("Files on the card",
               ["fopen", "fclose", "fread", "fwrite", "fgets", "fputs",
                "fseek", "ftell", "fsize", "feof", "remove"]),
              ("The flash store, outside RAM",
               ["store_pages", "store_page_size", "store_read", "store_write",
                "store_erase"]),
              ("Exact integers", [n for n, _, _ in api
                                  if n.startswith("big")]),
              ("Numeric helpers", ["fft", "convolve", "solve"]),
              ("The second core", ["par_for", "cores"]),
              ]
    byname = {n: (r, p) for n, r, p in api}
    for title, names in groups:
        names = [n for n in names if n in byname]
        if not names:
            continue
        print("*%s*\n" % title)
        print("```c")
        for n in names:
            ret, params = byname[n]
            print("%s %s(%s);" % (ret, n, ", ".join(params) if params else "void"))
        print("```\n")
    shown = sum(len([n for n in names if n in byname]) for _, names in groups)
    print("%d entry points, of which %d are listed above; the rest are the\n"
          "soft-float and 64-bit helpers the compiler emits for you.\n"
          "\n"
          "Declaring what you use is optional but worth it: the compiler reads a\n"
          "declaration to convert arguments, so `sqrt(2)` widens its argument only\n"
          "where `double sqrt(double);` is in scope. The names above are built in,\n"
          "so `#include <stdio.h>` and friends are satisfied by doing nothing."
          % (len(api), shown))


def emit_settings():
    rows, defaults = settings()
    print("| Key | Default | Range | Description |")
    print("|---|---|---|---|")
    for name, typ, lo, hi, help_ in rows:
        dv = defaults.get(FIELD.get(name, name), "")
        if dv.startswith("0x"):
            dv = dv.rstrip("u").rstrip("U")
        rng = "on / off" if typ == "V_BOOL" else "%s to %s" % (lo, hi)
        print("| `%s` | `%s` | %s | %s |" % (name, dv, rng, help_))


def main():
    what = sys.argv[1] if len(sys.argv) > 1 else "all"
    if what in ("all", "functions"): emit_functions()
    if what in ("all", "api"): emit_api()
    if what in ("all", "settings"): emit_settings()


if __name__ == "__main__":
    main()
