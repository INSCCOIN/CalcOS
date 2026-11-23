#!/usr/bin/env python3
"""Check every Thumb-2 encoding the compiler emits against GNU as.

tests/encgen.c emits one instruction per line as hex, in the same order as
ASM below. Each line is assembled independently with arm-none-eabi-as for
Cortex-M33 and the bytes compared.
"""
import subprocess, sys, os, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

ASM = [
    # moves
    "mov r1, r2", "mov r8, r3", "mov r4, r11",
    "movs r0, #7", "movs r5, #255",
    "movw r0, #0x1234", "movw r9, #0xBEEF",
    "movt r0, #0x1234", "movt r7, #0xFFFF",
    # arithmetic
    "adds r0, r1, r2", "add.w r8, r1, r2",
    "subs r0, r1, r2", "sub.w r10, r11, r12",
    "mul r0, r1, r2", "mul r9, r10, r11",
    "sdiv r0, r1, r2", "udiv r3, r4, r5",
    "and.w r0, r1, r2", "orr.w r3, r4, r5", "eor.w r6, r7, r8", "bic.w r1, r2, r3",
    "lsl.w r0, r1, r2", "lsr.w r3, r4, r5", "asr.w r6, r7, r8",
    "lsl.w r0, r1, #5", "lsr.w r2, r3, #17", "asr.w r4, r5, #31",
    "rsb.w r0, r1, #0", "rsb.w r9, r10, #0",
    "mvn.w r0, r1",
    "addw r0, r1, #100", "addw r2, r3, #4000",
    "subw r4, r5, #1", "subw r6, r7, #2048",
    # compare
    "cmp r0, r1", "cmp.w r8, r9",
    "cmp r0, #10", "cmp.w r9, #10",
    # memory
    "ldr r0, [r1, #0]", "ldr r2, [r3, #124]",
    "ldr.w r0, [r1, #200]", "ldr.w r9, [r10, #8]",
    "str r0, [r1, #0]", "str r2, [r3, #124]", "str.w r0, [r1, #300]",
    "ldrb r0, [r1, #3]", "ldrb.w r0, [r1, #100]", "strb r2, [r3, #31]",
    "ldrh r0, [r1, #4]", "ldrh.w r0, [r1, #100]", "strh r2, [r3, #62]",
    "ldrsb.w r0, [r1, #5]", "ldrsh.w r2, [r3, #6]",
    "ldr r0, [sp, #16]", "str r1, [sp, #1020]", "ldr.w r9, [sp, #16]",
    # extension
    "sxtb r0, r1", "sxth r2, r3", "uxtb r4, r5", "uxth r6, r7",
    "sxtb.w r8, r9", "uxth.w r10, r11",
    # stack
    "push {r4, r5, lr}", "push.w {r4, r8}",
    "pop {r4, r5, pc}", "pop.w {r4, r8}",
    # subw, not sub.w: both are valid for 1024, we pick the 12-bit form
    "add sp, sp, #16", "sub sp, sp, #16", "subw sp, sp, #1024",
    # control flow (self-referential: offset 0 branching to 0)
    "bx lr", "blx r12",
    "b.w .", "beq.w .", "blt.w .",
]


def assemble(src):
    with tempfile.TemporaryDirectory() as d:
        s = os.path.join(d, "a.s")
        o = os.path.join(d, "a.o")
        with open(s, "w") as f:
            f.write(".syntax unified\n.thumb\n.cpu cortex-m33\n" + src + "\n")
        r = subprocess.run(["arm-none-eabi-as", "-mthumb", "-mcpu=cortex-m33",
                            "-o", o, s], capture_output=True, text=True)
        if r.returncode:
            return None, r.stderr.strip()
        r = subprocess.run(["arm-none-eabi-objcopy", "-O", "binary",
                            "--only-section=.text", o, o + ".bin"],
                           capture_output=True, text=True)
        if r.returncode:
            return None, r.stderr.strip()
        with open(o + ".bin", "rb") as f:
            return f.read().hex(), None


def main():
    exe = os.path.join(ROOT, "build-host", "encgen")
    got = subprocess.run([exe], capture_output=True, text=True).stdout.strip().split("\n")
    if len(got) != len(ASM):
        print("MISMATCH: encgen emitted %d instructions, ASM table has %d"
              % (len(got), len(ASM)))
        return 1
    fails = 0
    for mine, src in zip(got, ASM):
        want, err = assemble(src)
        if want is None:
            print("ASSEMBLER ERROR  %-28s %s" % (src, err))
            fails += 1
            continue
        if mine != want:
            print("FAIL  %-28s  mine=%-12s as=%s" % (src, mine, want))
            fails += 1
    print("\n%d/%d encodings match GNU as" % (len(ASM) - fails, len(ASM)))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
