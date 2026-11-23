#!/usr/bin/env python3
"""The flash store must not overlap the firmware.

The store lives at the top of flash and the firmware is linked from the
bottom, so they only collide if the firmware grows by more than three
megabytes — but the consequence of being wrong is a board that erases its
own code and has to be reflashed over USB, so it is checked against the
real linked image rather than reasoned about.

Skipped when no firmware has been built.
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ELF = os.path.join(ROOT, "build", "calcos.elf")
SRC = os.path.join(ROOT, "platform", "picocalc", "pagestore.c")
XIP_BASE = 0x10000000


def macro(name, text, default=None):
    m = re.search(r"#define\s+%s\s+\(?([^)\n]*)\)?\s*$" % name, text, re.M)
    if not m:
        if default is not None:
            return default
        raise SystemExit("%s not found in pagestore.c" % name)
    return eval(m.group(1).replace("PICO_FLASH_SIZE_BYTES", "FLASH")
                .replace("STORE_BYTES", str(STORE_BYTES))
                if "STORE_BYTES" in m.group(1) or "PICO_FLASH" in m.group(1)
                else m.group(1), {"FLASH": FLASH})


FLASH = 4 * 1024 * 1024          # what CMakeLists defines for this board
text = open(SRC).read()
STORE_BYTES = macro("STORE_BYTES", text)
STORE_OFFSET = FLASH - STORE_BYTES
SECTOR = 4096

if not os.path.exists(ELF):
    print("   skipped: no firmware built")
    sys.exit(0)

out = subprocess.run(["arm-none-eabi-nm", ELF], capture_output=True, text=True)
if out.returncode:
    print("   skipped: arm-none-eabi-nm not available")
    sys.exit(0)

end = None
for line in out.stdout.split("\n"):
    parts = line.split()
    if len(parts) == 3 and parts[2] == "__flash_binary_end":
        end = int(parts[0], 16)
if end is None:
    print("FAIL: the firmware has no __flash_binary_end symbol")
    sys.exit(1)

store_start = XIP_BASE + STORE_OFFSET
used = end - XIP_BASE
fails = []

if end + SECTOR > store_start:
    fails.append("the firmware reaches 0x%08X and the store starts at 0x%08X"
                 % (end, store_start))
if STORE_OFFSET % SECTOR:
    fails.append("the store does not start on a 4 KB sector boundary")
if STORE_BYTES % SECTOR:
    fails.append("the store is not a whole number of sectors")

for f in fails:
    print("FAIL: " + f)
if fails:
    sys.exit(1)

print("   firmware uses %d KB, store is %d KB at 0x%08X, %d KB clear between"
      % (used // 1024, STORE_BYTES // 1024, store_start,
         (store_start - end) // 1024))
