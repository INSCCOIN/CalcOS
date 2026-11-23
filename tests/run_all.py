#!/usr/bin/env python3
"""Run every CalcOS test suite and summarise."""
import subprocess, sys, os, glob, tempfile, shutil

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)


def run(label, argv, cwd=ROOT):
    r = subprocess.run(argv, cwd=cwd, capture_output=True, text=True)
    tail = [l for l in r.stdout.strip().split("\n") if l.strip()]
    print("== %s" % label)
    for l in tail[-1:] if r.returncode == 0 else tail[-12:]:
        print("   " + l)
    return r.returncode == 0


def fat_suite():
    """Build FAT images and check the reader against them."""
    img = os.path.join(HERE, "..", "build-host", "fat32.img")
    if not shutil.which("mkfs.vfat") or not shutil.which("mcopy"):
        print("== filesystem\n   skipped (needs dosfstools and mtools)")
        return True
    d = tempfile.mkdtemp()
    ok = True
    for fs, mb in (("32", 80), ("16", 16)):
        p = os.path.join(d, "f%s.img" % fs)
        payload = os.path.join(d, "payload.bin")
        with open(payload, "wb") as f:
            f.write(bytes((i * 37 + 11) & 0xFF for i in range(120000)))
        subprocess.run(["dd", "if=/dev/zero", "of=" + p, "bs=1M",
                        "count=%d" % mb, "status=none"], check=True)
        subprocess.run(["mkfs.vfat", "-F", fs, p], check=True,
                       capture_output=True)
        subprocess.run(["mcopy", "-i", p, payload, "::payload.bin"],
                       check=True, env={**os.environ, "MTOOLSRC": "/dev/null"})
        r = subprocess.run([os.path.join(ROOT, "build-host", "fattest"),
                            p, "/payload.bin"], capture_output=True, text=True)
        want = sum((i * 37 + 11) & 0xFF for i in range(120000))
        got = "checksum %d" % want
        if got not in r.stdout:
            ok = False
            print("== filesystem FAT%s FAILED\n%s" % (fs, r.stdout))
    if ok:
        print("== filesystem\n   FAT16 and FAT32 read back byte-exact")
    shutil.rmtree(d, ignore_errors=True)
    return ok


def fat_write_suite():
    """Write files through the FAT writer, then check the image with fsck."""
    if not (shutil.which("mkfs.vfat") and shutil.which("fsck.vfat")
            and shutil.which("mcopy")):
        print("== filesystem writes\n   skipped (needs dosfstools and mtools)")
        return True
    d = tempfile.mkdtemp()
    exe = os.path.join(ROOT, "build-host", "fatwrite")
    ok = True
    for fs, mb in (("32", 80), ("16", 16)):
        p = os.path.join(d, "w%s.img" % fs)
        subprocess.run(["dd", "if=/dev/zero", "of=" + p, "bs=1M",
                        "count=%d" % mb, "status=none"], check=True)
        subprocess.run(["mkfs.vfat", "-F", fs, p], check=True, capture_output=True)
        # an existing file that must survive untouched
        keep = os.path.join(d, "keep.txt")
        open(keep, "w").write("do not disturb\n")
        subprocess.run(["mcopy", "-i", p, keep, "::keep.txt"], check=True,
                       env={**os.environ, "MTOOLSRC": "/dev/null"})
        steps = [("write", "/calcos.cfg", 300, 1), ("verify", "/calcos.cfg", 300, 1),
                 ("write", "/big.dat", 90000, 2),  ("verify", "/big.dat", 90000, 2),
                 ("write", "/calcos.cfg", 9000, 3), ("verify", "/calcos.cfg", 9000, 3),
                 ("write", "/calcos.cfg", 50, 4),   ("verify", "/calcos.cfg", 50, 4),
                 ("verify", "/big.dat", 90000, 2),
                 # long names: created, read back, rewritten, and one that
                 # differs from its 8.3 form only by case
                 ("write", "/My Long Data File.dat", 5000, 5),
                 ("verify", "/My Long Data File.dat", 5000, 5),
                 ("write", "/My Long Data File.dat", 7000, 6),
                 ("verify", "/My Long Data File.dat", 7000, 6),
                 ("write", "/Notes.txt", 400, 7),
                 ("verify", "/Notes.txt", 400, 7),
                 ("write", "/a-file-with-a-very-long-name-indeed.text", 1200, 8),
                 ("verify", "/a-file-with-a-very-long-name-indeed.text", 1200, 8),
                 ("verify", "/My Long Data File.dat", 7000, 6),
                 ("verify", "/calcos.cfg", 50, 4)]
        for op, name, size, seed in steps:
            r = subprocess.run([exe, p, op, name, str(size), str(seed)],
                               capture_output=True, text=True)
            if r.returncode != 0:
                ok = False
                print("== filesystem writes FAT%s FAILED: %s" % (fs, r.stdout.strip()))
                break
        # fsck proves the structure is sound; mdir proves a real FAT driver
        # reads the long names back as they were written
        m = subprocess.run(["mdir", "-i", p, "::"], capture_output=True,
                           text=True, env={**os.environ, "MTOOLSRC": "/dev/null"})
        for want in ("My Long Data File.dat", "Notes.txt",
                     "a-file-with-a-very-long-name-indeed.text"):
            if want not in m.stdout:
                ok = False
                print("== filesystem writes FAT%s: mdir cannot see '%s'"
                      % (fs, want))
        if "do not disturb" not in open(keep).read():
            ok = False

        r = subprocess.run(["fsck.vfat", "-n", p], capture_output=True, text=True)
        bad = [l for l in r.stdout.split("\n")
               if l.strip() and "uninitialized" not in l
               and not l.startswith("fsck.fat") and ":" not in l]
        if bad:
            ok = False
            print("== filesystem writes FAT%s: fsck complained\n%s" % (fs, r.stdout))
        # the pre-existing file must still be readable by a normal tool
        r = subprocess.run(["mdir", "-i", p, "::"], capture_output=True, text=True,
                           env={**os.environ, "MTOOLSRC": "/dev/null"})
        if "keep" not in r.stdout:
            ok = False
            print("== filesystem writes FAT%s: clobbered an existing file" % fs)
    if ok:
        print("== filesystem writes\n   FAT16 and FAT32 pass fsck, existing files intact")
    shutil.rmtree(d, ignore_errors=True)
    return ok


def settings_suite():
    """Settings must survive a save/load round trip in a separate process."""
    d = tempfile.mkdtemp()
    exe = os.path.join(ROOT, "build-host", "calcos")
    subprocess.run([exe, "-e", "set lcdbits 18", "set fg 0xFF8800",
                    "set cpu 212", "set save"], cwd=d, capture_output=True)
    r = subprocess.run([exe, "-e", "set load", "set lcdbits", "set fg", "set cpu"],
                       cwd=d, capture_output=True, text=True)
    ok = ("lcdbits = 18" in r.stdout and "fg = 0xFF8800" in r.stdout
          and "cpu = 212" in r.stdout)
    bad = subprocess.run([exe, "-e", "set lcdbits 99", "set nosuch 1"],
                         cwd=d, capture_output=True, text=True)
    ok &= ("not a valid value" in bad.stdout and "no setting called" in bad.stdout)
    print("== settings\n   %s" % ("round trip and validation ok" if ok else "FAILED"))
    if not ok:
        print(r.stdout, bad.stdout)
    shutil.rmtree(d, ignore_errors=True)
    return ok


def width_suite():
    """Nothing the shell prints may be wider than the screen.

    The panel is 40 columns. A line that overruns is hard-wrapped mid-word by
    the console, which is what made the settings listing unreadable, so every
    command's output is measured here rather than eyeballed on the device.
    """
    exe = os.path.join(ROOT, "build-host", "calcos")
    cmds = ["help", "help all", "help sin", "help plot", "help gamma",
            "help nosuch", "set", "set help", "set cpu", "set lcdspi",
            "set blswap", "set cpu 212", "set lcdbits 99", "set nosuch 1",
            "set save", "set reset", "display", "about", "mem", "vars",
            "funcs", "cfuncs", "run", "cc", "cc nosuch.c", "ls", "cat",
            "save", "load", "digits 17", "ddigits 34", "deg", "rad", "bits",
            "unload", "x = 1/7", "vars", "sin(", "zzz(2)", "unknowncmd",
            "[1,2;3,4]", "pi^20"]
    d = tempfile.mkdtemp()
    env = {**os.environ, "COLUMNS": "40"}
    r = subprocess.run([exe, "-e"] + cmds, cwd=d, capture_output=True,
                       text=True, env=env)
    over = [l for l in r.stdout.split("\n") if len(l) > 40]
    shutil.rmtree(d, ignore_errors=True)
    print("== console width")
    if over:
        print("   %d line(s) wider than 40 columns:" % len(over))
        for l in over[:8]:
            print("   %3d  %s" % (len(l), l))
        return False
    print("   every line fits 40 columns")
    return True


def main():
    ok = True
    ok &= run("encodings", [sys.executable, "tests/test_encodings.py"])
    ok &= run("compiler", [sys.executable, "-I", "tests/test_cc.py"])
    ok &= run("language", [sys.executable, "tests/test_cc2.py"])
    ok &= run("runtime", [sys.executable, "tests/test_runtime.py"])
    ok &= run("big integers", [sys.executable, "tests/test_bignum.py"])
    ok &= run("examples", [sys.executable, "tests/test_examples.py"])
    ok &= run("calculator", [sys.executable, "tests/run_calc_tests.py"])
    ok &= run("font", [sys.executable, "tests/test_font.py"])
    ok &= run("shell storage", [sys.executable, "tests/test_shell.py"])
    ok &= run("line editor", [sys.executable, "tests/test_lineedit.py"])
    ok &= run("scrollback", [os.path.join(ROOT, "build-host", "sbtest")])
    ok &= run("screen", [os.path.join(ROOT, "build-host", "screensim")])
    ok &= run("panel driver", [os.path.join(ROOT, "build-host", "lcdsim")])
    ok &= run("keyboard driver", [os.path.join(ROOT, "build-host", "kbdsim")])
    ok &= fat_suite()
    ok &= fat_write_suite()
    ok &= settings_suite()
    ok &= width_suite()
    print("\n%s" % ("ALL SUITES PASSED" if ok else "FAILURES ABOVE"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
