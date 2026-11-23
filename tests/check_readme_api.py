#!/usr/bin/env python3
"""Every C declaration the README prints must actually compile.

The API reference is generated from the firmware's own table, so the names
cannot drift — but a wrong signature character would produce a declaration
that looks right and is not. Feeding the whole list through the real
compiler is the cheapest way to be sure the documentation is usable as
written.

Only the blocks under "What compiled C can call" are taken: elsewhere the
README shows fragments of programs, which are not declarations.
"""
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CCRUN = os.path.join(ROOT, "build-host", "ccrun")
README = os.path.join(ROOT, "README.md")

if not os.path.exists(README):
    print("   skipped: no README built")
    sys.exit(0)

md = open(README).read()
start = md.find("## What compiled C can call")
if start < 0:
    start = md.find("### What compiled C can call")
if start < 0:
    print("FAIL: the README has no C API section")
    sys.exit(1)
end = md.find("\n### ", start + 10)
if end < 0:
    end = len(md)
section = md[start:end]

decls = []
for block in re.findall(r"```c\n(.*?)```", section, re.S):
    for line in block.split("\n"):
        line = line.strip()
        if line.endswith(");") and "(" in line:
            decls.append(line)

if len(decls) < 60:
    print("FAIL: only %d declarations found; the section moved" % len(decls))
    sys.exit(1)

d = tempfile.mkdtemp()
path = os.path.join(d, "apicheck.c")
with open(path, "w") as f:
    f.write("\n".join(decls))
    f.write("\nint main(void){ return 0; }\n")

r = subprocess.run([CCRUN, path], capture_output=True, text=True)
if r.returncode:
    print("FAIL: " + r.stderr.strip())
    sys.exit(1)

print("   %d documented declarations compile" % len(decls))
