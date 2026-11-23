#!/usr/bin/env python3
"""Assemble README.md from docs/README.in.md plus the generated reference.

The function, settings and runtime tables come from the same source the
firmware uses, so the documentation cannot drift from the code.
"""
import subprocess, sys, os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GEN = os.path.join(ROOT, "tools", "gen_reference.py")


def section(name):
    r = subprocess.run([sys.executable, GEN, name], capture_output=True, text=True)
    if r.returncode:
        sys.exit("gen_reference.py %s failed:\n%s" % (name, r.stderr))
    return r.stdout.rstrip("\n")


def main():
    tpl = open(os.path.join(ROOT, "docs", "README.in.md")).read()
    for key, which in (("{{FUNCTIONS}}", "functions"),
                       ("{{SETTINGS}}", "settings"),
                       ("{{API}}", "api")):
        if key not in tpl:
            sys.exit("placeholder %s missing from the template" % key)
        tpl = tpl.replace(key, section(which))
    out = os.path.join(ROOT, "README.md")
    open(out, "w").write(tpl)
    print("wrote %s (%d lines)" % (out, tpl.count("\n") + 1))


if __name__ == "__main__":
    main()
