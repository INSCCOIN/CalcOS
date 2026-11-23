#!/usr/bin/env python3
"""Shell commands that touch storage: the flash store from the prompt.

The host build backs the store with a file instead of memory-mapped flash,
so a page written here can be read back in a *fresh process* — which is the
property that matters, since the point of the store is that it outlives both
the program and the power.
"""
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, "build-host", "calcos")

fails = []
count = 0


def check(cond, what):
    global count
    count += 1
    if not cond:
        fails.append(what)
        print("FAIL: " + what)


class Card:
    """A directory standing in for the SD card, kept across runs."""

    def __init__(self):
        self.dir = tempfile.mkdtemp()

    def write(self, name, text):
        with open(os.path.join(self.dir, name), "w") as f:
            f.write(text)

    def read(self, name):
        with open(os.path.join(self.dir, name), "rb") as f:
            return f.read()

    def run(self, *cmds):
        r = subprocess.run([EXE, "-e"] + list(cmds), capture_output=True,
                           text=True, cwd=self.dir,
                           env={**os.environ, "COLUMNS": "40"})
        return r.stdout


c = Card()
out = c.run("store")
check("128 pages of 4096 bytes" in out, "store reports its size")
check("0 used, 128 free" in out, "a fresh store is empty")

c.write("note.txt", "Hello from the card.\n")
out = c.run("store save 3 note.txt")
check("21 bytes into page 3" in out, "store save copies a file into a page")

# a *new* process must see it: this is the whole point of the store
out = c.run("store")
check("1 used, 127 free" in out, "the page is still there in a later session")

out = c.run("store dump 3")
check("Hello fr" in out, "store dump shows the bytes as characters")
check("48656C6C6F206672" in out, "store dump shows them as hex too")
check("page 3, 0-127 of 4096" in out, "store dump says what part it showed")

out = c.run("store dump 3 128")
check("0080 " in out, "store dump takes an offset")

out = c.run("store map")
check("#" in out, "store map marks a page that holds data")

out = c.run("store load 3 copy.bin")
check("page 3 to copy.bin" in out, "store load writes a page out to the card")
data = c.read("copy.bin")
check(len(data) == 4096, "the written page is a whole page")
check(data.startswith(b"Hello from the card.\n"), "and holds what was saved")
check(data[21:].strip(b"\xff") == b"", "with the rest left erased")

out = c.run("store erase 3", "store")
check("erased page 3" in out and "0 used" in out, "store erase frees a page")

# a page number outside the store is refused rather than wrapping
out = c.run("store dump 999")
check("usage" in out, "a page number past the end is refused")
out = c.run("store erase 999")
check("usage" in out, "so is erasing one")

# erase all
c.run("store save 1 note.txt", "store save 2 note.txt")
out = c.run("store")
check("2 used" in out, "two pages in use")
out = c.run("store erase all")
check("erased 2 page(s)" in out, "store erase all clears only what was used")

out = c.run("store nonsense")
check("unknown: store nonsense" in out, "an unknown subcommand says so")

# (a page written by a compiled program is checked in test_runtime.py,
# where the machine code actually runs)

# ------------------------------------------------- named objects (2.2)

c = Card()
c.write("data.txt", "A named object in flash.\n")
out = c.run("store format", "store put greeting data.txt", "store list")
check("greeting" in out and "25 data" in out, "store put names an object")

out = c.run("store list")
check("greeting" in out, "the name is still there in a later session")

out = c.run("store view greeting")
check("A named " in out, "store view shows an object's bytes")

out = c.run("store free")
check("object(s)" in out, "store free counts the objects")

out = c.run("store get greeting copy.txt")
check(c.read("copy.txt") == b"A named object in flash.\n",
      "store get writes the object back out byte for byte")

# replacing an object: the old copy must survive until the new one is in
c.write("bigger.txt", "x" * 5000)
out = c.run("store put greeting bigger.txt", "store list")
check("5000" in out, "an object can be replaced with a larger one")
out = c.run("store get greeting back.txt")
check(len(c.read("back.txt")) == 5000, "and the new contents are what is read")

out = c.run("store rm greeting", "store list")
check("nothing stored" in out, "store rm removes it")

# names are not page numbers: two objects coexist and keep their own pages
c.run("store format")
c.write("a.txt", "first\n")
c.write("b.txt", "second\n")
c.run("store put one a.txt", "store put two b.txt")
out = c.run("store view one")
check("first" in out, "the first object reads back")
out = c.run("store view two")
check("second" in out, "and so does the second")

out = c.run("store put " + "x" * 40 + " a.txt")
check("characters" in out, "an over-long name is refused with a reason")

out = c.run("store view nosuch")
check("no object" in out, "asking for a name that is not there says so")

# ------------------------------------------- programs in flash (2.2)

c = Card()
c.write("p.c", "double sq(double x){ return x*x; }\n"
               "int main(void){ return 0; }\n")
out = c.run("store format", "ccs demo p.c")
check("stored as demo" in out, "ccs keeps a compiled program in flash")

# a fresh process, with the source file still present but not used
out = c.run("use demo", "cfuncs")
check("demo loaded" in out, "use loads it back in a later session")
check("sq/1" in out, "and its functions are there")

out = c.run("store list")
check("program" in out, "a stored program is listed as a program")

# with no card file at all, the program still loads
os.remove(os.path.join(c.dir, "p.c"))
out = c.run("use demo", "cfuncs")
check("sq/1" in out, "a stored program needs neither compiler nor source")

out = c.run("use nosuch")
check("no object called nosuch" in out, "loading a name that is not there says so")

c.write("d.txt", "not a program\n")
c.run("store put notprog d.txt")
out = c.run("use notprog")
check("not a program" in out, "loading a data object as a program is refused")

# a program named "boot" loads at startup, with no command at all
c.write("q.c", "double twice(double x){ return 2*x; }\n")
c.run("ccs boot q.c")
r = subprocess.run([EXE, "-e", "cfuncs"], capture_output=True, text=True,
                   cwd=c.dir, env={**os.environ, "COLUMNS": "40"})
check("boot:" in r.stdout and "twice/1" in r.stdout,
      "a program stored as 'boot' loads at startup by itself")

print("   %d/%d shell storage tests passed" % (count - len(fails), count))
sys.exit(1 if fails else 0)
