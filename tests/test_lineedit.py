#!/usr/bin/env python3
"""The command line: editing, history and completion.

The editor sits above the platform layer, so the host build runs exactly
the code the device runs. Special keys are single bytes above 127, which is
what the keyboard controller sends, so a key sequence can simply be piped
in. What comes back on stdout is what the panel would show, backspaces and
all, which is how the redrawing is checked rather than assumed.
"""
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, "build-host", "calcos")

UP, DOWN, LEFT, RIGHT = 0xB5, 0xB6, 0xB4, 0xB7
HOME, END, DEL = 0xD2, 0xD5, 0xD4

fails = []
count = 0


def check(cond, what):
    global count
    count += 1
    if not cond:
        fails.append(what)
        print("FAIL: " + what)


def feed(*parts):
    """Send a key sequence and return everything printed."""
    data = bytearray()
    for p in parts:
        if isinstance(p, int):
            data.append(p)
        else:
            data += p.encode("latin1")
    d = tempfile.mkdtemp()
    r = subprocess.run([EXE], input=bytes(data), capture_output=True, cwd=d)
    return r.stdout.decode("latin1")


def answers(out):
    """The result lines: what the calculator printed, prompts removed."""
    lines = []
    if ">" in out:
        out = out[out.index(">"):]           # drop the startup banner
    for line in out.split("\n"):
        if line.startswith(">") or not line.strip():
            continue
        if line.strip() == "bye":        # printed when stdin runs out
            continue
        lines.append(line.strip())
    return lines


# ------------------------------------------------------------- editing

out = feed("1+3", LEFT, LEFT, "2\n")
check("15" in answers(out), "left arrow moves the cursor so text inserts mid-line")

out = feed("12+3", HOME, "1\n")
check("115" in answers(out) or "4" not in answers(out)[:1],
      "home jumps to the start of the line")

out = feed("1+99", 8, 8, "2\n")
check("3" in answers(out), "backspace removes the character before the cursor")

out = feed("99+2", HOME, DEL, DEL, "1\n")
check("3" in answers(out), "delete removes the character under the cursor")

out = feed("2*3", HOME, END, "+1\n")
check("7" in answers(out), "end jumps back to the end of the line")

out = feed("1+1", 21, "7*6\n")            # ^U
check("42" in answers(out) and "2" not in answers(out),
      "ctrl-U discards the line being typed")

out = feed("6*7 xyz", 23, "\n")           # ^W
check("42" in answers(out), "ctrl-W deletes the word behind the cursor")

out = feed("5+5", 11, "\n", "")           # ^K at the end changes nothing
check("10" in answers(out), "ctrl-K at the end of the line leaves it alone")

out = feed("1+2", HOME, 11, "9*9\n")      # ^K from the start
check("81" in answers(out), "ctrl-K from the start clears to the end")

# ------------------------------------------------------------- history

out = feed("1+1\n", "2+2\n", UP, UP, "\n")
check(answers(out) == ["2", "4", "2"],
      "two presses of up recall the line before last")

out = feed("1+1\n", "2+2\n", UP, UP, DOWN, "\n")
check(answers(out)[-1] == "4", "down comes back towards the newest line")

out = feed("3+3\n", UP, "+1\n")
check(answers(out)[-1] == "7", "a recalled line can be edited before running")

out = feed("7*7", UP, "\n")
check(answers(out) == ["49"],
      "up with no history leaves what is being typed alone")

out = feed("1+1\n", "2+2\n", "history\n")
check("1 1+1" in out and "2 2+2" in out, "history lists what was typed")

out = feed("5+5\n", "5+5\n", "history\n")
check(out.count("5+5") == 3,          # two prompts and one listing
      "the same command twice in a row is only recorded once")

out = feed("1+1\n", "history clear\n", "history\n")
check(out.count("1+1") == 1, "history clear empties the list")

# ---------------------------------------------------------- completion

out = feed("sqr\t(16)\n")
check("4" in answers(out), "tab completes a function name and it then runs")
check("sqrt(" in out, "a completed function name is not followed by a space")

out = feed("hel\t\n")
check("commands" in out, "tab completes a command, which then runs")

out = feed("x = 7\n", "x\t\n")
check(answers(out)[-1] == "7", "tab completes a name the user defined")

out = feed("si\t\n")
check("sin" in out and "sinh" in out,
      "an ambiguous prefix lists the possibilities")

out = feed("zzzz\t\n")
check("zzzz" in out, "a prefix that matches nothing is left alone")

out = feed("fun\t\n")
check("funcs" in out, "a command prefix completes as far as it is unique")

# ------------------------------------------------------- startup files


def boot(files, keys=""):
    """Boot with these files on the card and return everything printed."""
    d = tempfile.mkdtemp()
    for name, text in files.items():
        with open(os.path.join(d, name), "w") as f:
            f.write(text)
    r = subprocess.run([EXE], input=keys.encode("latin1"),
                       capture_output=True, cwd=d)
    return r.stdout.decode("latin1")


out = boot({"startup.txt": "digits 12\nk = 42\n"}, "k\n")
check("running /startup.txt" in out, "a startup file is announced when it runs")
check("42" in out, "a variable set by the startup file is there afterwards")

out = boot({"startup.txt": "# only a comment\n\nx = 1\n"}, "x\n")
check("1" in out, "blank lines and comments in a startup file are skipped")

out = boot({"autoexec.c": "int main(void){ return 0; }\n"}, "")
check("compiling /autoexec.c" in out, "autoexec.c is compiled at boot")

out = boot({"autoexec.c": "int main(void){ return 1 + ; }\n"}, "")
check("compiling /autoexec.c" in out and "line" in out,
      "a broken autoexec.c reports its error instead of stopping the boot")
check("CalcOS" in out, "and the calculator still starts")

out = boot({}, "")
check("startup" not in out, "with no startup files, nothing is said about them")

# A unit name is only a unit where the user has not claimed the name.
out = feed("m = 5\n", "m * 2\n")
check(answers(out)[-1] == "10",
      "a variable named like a unit shadows the unit")

out = feed("3 m\n", "x = 2 m\n", "x + 1 m\n")
check(answers(out)[-1] == "3 m", "a quantity survives being stored in a variable")

print("   %d/%d line editor and startup tests passed" % (count - len(fails), count))
sys.exit(1 if fails else 0)
