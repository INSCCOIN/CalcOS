#!/usr/bin/env python3
"""Run CalcOS-compiled Thumb-2 under Unicorn with a Python runtime.

The compiler emits calls to firmware entry points. Here each of those is a
synthetic address; a code hook intercepts it, performs the operation in
Python and returns to the caller. That gives a full end-to-end check:
C source in, real machine code out, executed, answer compared.
"""
import struct, subprocess, os, math, cmath
from unicorn import *
from unicorn.arm_const import *

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CCRUN = os.path.join(ROOT, "build-host", "ccrun")

CODE_REGION = (0x20000000, 0x00080000)   # code + data + heap
STACK_TOP   = 0x20070000
RT_REGION   = (0x08000000, 0x00001000)
STOP        = 0x00900000


class Program:
    def __init__(self, path):
        r = subprocess.run([CCRUN, path], capture_output=True, text=True)
        if r.returncode:
            raise RuntimeError(r.stderr.strip() or "compile failed")
        self.fns, self.vars, self.rt = {}, {}, {}
        lines = r.stdout.strip().split("\n")
        i = 0
        while i < len(lines):
            p = lines[i].split()
            if p[0] == "code_base":   self.code_base = int(p[1], 16)
            elif p[0] == "data_base": self.data_base = int(p[1], 16)
            elif p[0] == "rt_base":   self.rt_base = int(p[1], 16)
            elif p[0] == "rt":        self.rt[p[1]] = int(p[2], 16)
            elif p[0] == "fn":        self.fns[p[1]] = int(p[2], 16)
            elif p[0] == "var":       self.vars[p[1]] = (int(p[2], 16), int(p[3]))
            elif p[0] == "code":
                i += 1
                h = lines[i].strip() if i < len(lines) else ""
                self.code = bytes.fromhex(h) if h else b""
            elif p[0] == "data":
                i += 1
                h = lines[i].strip() if i < len(lines) else ""
                self.data = bytes.fromhex(h) if h else b""
            i += 1
        self.rt_by_addr = {v: k for k, v in self.rt.items()}


def _d(lo, hi):
    return struct.unpack("<d", struct.pack("<II", lo & 0xFFFFFFFF, hi & 0xFFFFFFFF))[0]


def _split(d):
    return struct.unpack("<II", struct.pack("<d", d))


def _s32(v):
    return v - 0x100000000 if v >= 0x80000000 else v


class Machine:
    def __init__(self, prog):
        self.p = prog
        self.out = []
        self.heap = 0x20050000
        self.fb = {}            # (x, y) -> packed rgb
        self.clip = None
        self.auto = 0
        self.pal = [0x000000, 0xFFFFFF, 0xE02020, 0x20C040, 0x3060E0, 0xE0E020,
                    0xE020E0, 0x20E0E0, 0x808080, 0x404040, 0xC08020, 0x8020C0,
                    0x20A080, 0xA02040, 0x60A0E0, 0xC0C0C0]
        self.texts = []
        self.shown = self.hidden = 0
        self.clock_us = 0
        self.keys = []          # fed to getchar/getkey/kbhit
        self.lines = []         # fed to getline_
        self.tones = []
        self.files = {}         # path -> bytes, standing in for the card
        self.fhs = {}
        self.next_fh = 1
        self.bigs = {}                  # handle -> Python int
        self.next_big = 1
        self.store = {}                 # page -> 4096 bytes
        self.store_base = 0x20060000    # where a read is mapped for the program
        mu = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
        mu.mem_map(*CODE_REGION)
        mu.mem_map(*RT_REGION)
        mu.mem_map(STOP & ~0xFFF, 0x1000)
        mu.mem_write(prog.code_base, prog.code)
        if prog.data:
            mu.mem_write(prog.data_base, prog.data)
        mu.hook_add(UC_HOOK_CODE, self._hook,
                    begin=RT_REGION[0], end=RT_REGION[0] + RT_REGION[1] - 1)
        self.mu = mu

    # ---- helpers for the hooked runtime -----------------------------
    def _cstr(self, addr):
        out = b""
        while True:
            b = self.mu.mem_read(addr, 1)
            if b == b"\x00":
                break
            out += b
            addr += 1
            if len(out) > 4096:
                break
        return out.decode("latin1")

    def _ret(self, mu, val=None, dval=None):
        if dval is not None:
            lo, hi = _split(dval)
            mu.reg_write(UC_ARM_REG_R0, lo)
            mu.reg_write(UC_ARM_REG_R1, hi)
        elif val is not None:
            mu.reg_write(UC_ARM_REG_R0, val & 0xFFFFFFFF)
        mu.reg_write(UC_ARM_REG_PC, mu.reg_read(UC_ARM_REG_LR) | 1)

    def _printf(self, mu):
        fmt = self._cstr(mu.reg_read(UC_ARM_REG_R0))
        # Arguments follow AAPCS: r1..r3 then the stack. Doubles take an
        # even-aligned pair, matching what the compiler emits.
        core = [mu.reg_read(r) for r in (UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3)]
        sp = mu.reg_read(UC_ARM_REG_SP)
        idx = 0

        def next_word():
            nonlocal idx
            if idx < 3:
                v = core[idx]; idx += 1; return v
            off = (idx - 3) * 4
            idx += 1
            return struct.unpack("<I", self.mu.mem_read(sp + off, 4))[0]

        def next_wide():
            """A long long is passed exactly like a double: an even-aligned
            register pair, or eight bytes on the stack."""
            nonlocal idx
            if idx + 1 < 4 and (idx + 1) % 2 == 1:
                next_word()
            lo = next_word(); hi = next_word()
            return (hi << 32) | lo

        def next_double():
            nonlocal idx
            # register index in the full r0..r3 numbering is idx+1
            if idx + 1 < 4 and (idx + 1) % 2 == 1:
                next_word()            # pad to an even pair
            lo = next_word(); hi = next_word()
            return _d(lo, hi)

        res, i = "", 0
        while i < len(fmt):
            c = fmt[i]
            if c != "%":
                res += c; i += 1; continue
            j = i + 1
            spec = "%"
            nlong = 0
            while j < len(fmt) and fmt[j] in "-+ #0123456789.*lhz":
                if fmt[j] == "l":
                    nlong += 1
                elif fmt[j] not in "hz":
                    spec += fmt[j]
                j += 1
            if j >= len(fmt):
                break
            conv = fmt[j]
            if conv == "%":
                res += "%"
            elif conv in "diu":
                if nlong >= 2:
                    v = next_wide()
                    if conv != "u" and v >= 1 << 63:
                        v -= 1 << 64
                else:
                    v = next_word()
                    if conv != "u" and v >= 0x80000000:
                        v -= 0x100000000
                res += (spec + "d") % v
            elif conv in "xX":
                res += (spec + conv) % (next_wide() if nlong >= 2 else next_word())
            elif conv in "fFeEgG":
                res += (spec + conv) % next_double()
            elif conv == "c":
                res += chr(next_word() & 0xFF)
            elif conv == "s":
                res += (spec + "s") % self._cstr(next_word())
            elif conv == "p":
                res += "0x%x" % next_word()
            i = j + 1
        self.out.append(res)
        self._ret(mu, val=len(res))

    def _hook(self, mu, address, size, user):
        name = self.p.rt_by_addr.get(address)
        if name is None:
            return
        r0 = mu.reg_read(UC_ARM_REG_R0)
        r1 = mu.reg_read(UC_ARM_REG_R1)
        r2 = mu.reg_read(UC_ARM_REG_R2)
        r3 = mu.reg_read(UC_ARM_REG_R3)
        a = _d(r0, r1)
        b = _d(r2, r3)

        if name == "__aeabi_dadd":   return self._ret(mu, dval=a + b)
        if name == "__aeabi_dsub":   return self._ret(mu, dval=a - b)
        if name == "__aeabi_dmul":   return self._ret(mu, dval=a * b)
        if name == "__aeabi_ddiv":
            return self._ret(mu, dval=(a / b) if b != 0 else math.inf * (1 if a >= 0 else -1))
        if name == "__aeabi_i2d":
            v = r0 - 0x100000000 if r0 >= 0x80000000 else r0
            return self._ret(mu, dval=float(v))
        if name == "__aeabi_ui2d":   return self._ret(mu, dval=float(r0))
        if name == "__aeabi_d2iz":
            try:
                v = int(a)
            except (ValueError, OverflowError):
                v = 0
            return self._ret(mu, val=v)
        if name == "__aeabi_f2d":
            f = struct.unpack("<f", struct.pack("<I", r0))[0]
            return self._ret(mu, dval=float(f))
        if name == "__aeabi_d2f":
            return self._ret(mu, val=struct.unpack("<I", struct.pack("<f", a))[0])
        if name == "__aeabi_dcmplt": return self._ret(mu, val=1 if a < b else 0)
        if name == "__aeabi_dcmple": return self._ret(mu, val=1 if a <= b else 0)
        if name == "__aeabi_dcmpgt": return self._ret(mu, val=1 if a > b else 0)
        if name == "__aeabi_dcmpge": return self._ret(mu, val=1 if a >= b else 0)
        if name == "__aeabi_dcmpeq": return self._ret(mu, val=1 if a == b else 0)

        # ---- 64-bit integers: a pair of registers in, a pair out ----
        if name.startswith("__cos_"):
            return self._ll(mu, name[6:], r0, r1, r2, r3)

        if name == "printf":  return self._printf(mu)
        if name == "putchar":
            self.out.append(chr(r0 & 0xFF))
            return self._ret(mu, val=r0)
        if name == "puts":
            self.out.append(self._cstr(r0) + "\n")
            return self._ret(mu, val=0)

        one = {"sin": math.sin, "cos": math.cos, "tan": math.tan,
               "exp": math.exp, "sqrt": lambda x: math.sqrt(x) if x >= 0 else math.nan,
               "log": lambda x: math.log(x) if x > 0 else -math.inf,
               "fabs": abs, "floor": math.floor, "ceil": math.ceil,
               "erf_fn": math.erf, "gamma_fn": math.gamma,
               "exp2": lambda x: 2.0 ** x, "log1p": math.log1p,
               "expm1": math.expm1, "asinh": math.asinh,
               "acosh": lambda x: math.acosh(x) if x >= 1 else math.nan,
               "atanh": lambda x: math.atanh(x) if -1 < x < 1 else math.nan,
               "log10": math.log10, "log2": math.log2,
               "round": lambda x: math.floor(x + 0.5) if x >= 0 else math.ceil(x - 0.5),
               "trunc": math.trunc, "cbrt": lambda x: math.copysign(abs(x) ** (1 / 3), x),
               "sinh": math.sinh, "cosh": math.cosh, "tanh": math.tanh,
               "asin": math.asin, "acos": math.acos, "atan": math.atan,
               "nearbyint": round, "lgamma_fn": math.lgamma,
               "erfc_fn": math.erfc, "digamma_fn": lambda x: x,
               "besselj1_fn": lambda x: x, "zeta_fn": lambda x: x,
               "besselj0_fn": lambda x: x}
        if name in one:
            return self._ret(mu, dval=float(one[name](a)))
        if name == "pow":   return self._ret(mu, dval=float(a ** b))
        if name == "atan2": return self._ret(mu, dval=math.atan2(a, b))
        if name == "fmod":  return self._ret(mu, dval=math.fmod(a, b))
        if name == "hypot": return self._ret(mu, dval=math.hypot(a, b))
        if name == "fmin":  return self._ret(mu, dval=min(a, b))
        if name == "fmax":  return self._ret(mu, dval=max(a, b))
        if name == "copysign": return self._ret(mu, dval=math.copysign(a, b))
        if name == "remainder": return self._ret(mu, dval=math.remainder(a, b))
        if name == "ldexp":
            return self._ret(mu, dval=math.ldexp(a, _s32(r2)))

        if name == "malloc":
            p = self.heap
            self.heap += (r0 + 7) & ~7
            return self._ret(mu, val=p)
        if name == "free":   return self._ret(mu, val=0)
        if name == "memset":
            mu.mem_write(r0, bytes([r1 & 0xFF]) * r2)
            return self._ret(mu, val=r0)
        if name == "memcpy":
            mu.mem_write(r0, bytes(mu.mem_read(r1, r2)))
            return self._ret(mu, val=r0)
        if name == "strlen": return self._ret(mu, val=len(self._cstr(r0)))
        if name == "strcmp":
            x, y = self._cstr(r0), self._cstr(r1)
            return self._ret(mu, val=(x > y) - (x < y))
        if name == "strcpy":
            s = self._cstr(r1).encode("latin1") + b"\x00"
            mu.mem_write(r0, s)
            return self._ret(mu, val=r0)
        if name == "strncpy":
            src = self._cstr(r1).encode("latin1")[:r2]
            mu.mem_write(r0, src + b"\x00" * (r2 - len(src)))
            return self._ret(mu, val=r0)
        if name == "strcat":
            mu.mem_write(r0 + len(self._cstr(r0)),
                         self._cstr(r1).encode("latin1") + b"\x00")
            return self._ret(mu, val=r0)
        if name == "abs":
            v = r0 - 0x100000000 if r0 >= 0x80000000 else r0
            return self._ret(mu, val=abs(v))

        # ---- graphics: a real framebuffer, so drawing can be checked ----
        if name == "plot_clear":
            self.fb = {}
            self.clip = None
            return self._ret(mu, val=0)
        if name == "plot_point":
            self._put(_s32(r0), _s32(r1), r2)
            return self._ret(mu, val=0)
        if name == "plot_get":
            return self._ret(mu, val=self.fb.get((_s32(r0), _s32(r1)), 0))
        if name == "plot_width" or name == "plot_height":
            return self._ret(mu, val=320)
        if name == "rgb":
            return self._ret(mu, val=((r0 & 255) << 16) | ((r1 & 255) << 8) | (r2 & 255))
        if name in ("plot_line", "plot_rect", "plot_fill_rect",
                    "plot_circle", "plot_fill_circle"):
            self._shape(mu, name, r0, r1, r2, r3)
            return self._ret(mu, val=0)
        if name == "plot_palette":
            if 0 <= _s32(r0) < 16:
                self.pal[_s32(r0)] = r1 & 0xFFFFFF
            return self._ret(mu, val=0)
        if name == "plot_clip":
            sp = mu.reg_read(UC_ARM_REG_SP)
            self.clip = (_s32(r0), _s32(r1), _s32(r2), _s32(r3))
            del sp
            return self._ret(mu, val=0)
        if name == "plot_noclip":
            self.clip = None
            return self._ret(mu, val=0)
        if name == "plot_auto":
            self.auto = _s32(r0)
            return self._ret(mu, val=0)
        if name in ("plot_sprite", "plot_blit"):
            w, h = _s32(r2), _s32(r3)
            data_at = struct.unpack("<I", mu.mem_read(
                mu.reg_read(UC_ARM_REG_SP), 4))[0]
            stride = (w + 1) // 2
            raw = bytes(mu.mem_read(data_at, stride * h)) if w and h else b""
            for row in range(h):
                for col in range(w):
                    b = raw[row * stride + (col >> 1)]
                    idx = (b >> 4) if (col & 1) else (b & 0x0F)
                    if idx == 0 and name == "plot_sprite":
                        continue
                    self._put(_s32(r0) + col, _s32(r1) + row, self.pal[idx])
            return self._ret(mu, val=0)
        if name == "plot_text":
            self.texts.append((_s32(r0), _s32(r1), self._cstr(r2)))
            return self._ret(mu, val=0)
        if name in ("plot_flush", "plot_show"):
            self.shown += 1
            return self._ret(mu, val=0)
        if name == "plot_hide":
            self.hidden += 1
            return self._ret(mu, val=0)

        # ---- time, keys, sound ----
        if name == "millis":
            self.clock_us += 1000
            return self._ret(mu, val=self.clock_us // 1000)
        if name == "micros":
            self.clock_us += 1
            return self._ret(mu, val=self.clock_us)
        if name == "delay":
            self.clock_us += max(0, _s32(r0)) * 1000
            return self._ret(mu, val=0)
        if name == "tone":
            self.tones.append((_s32(r0), _s32(r1)))
            self.clock_us += max(0, _s32(r1)) * 1000
            return self._ret(mu, val=0)
        if name == "notone":
            self.tones.append((0, 0))
            return self._ret(mu, val=0)
        if name == "getchar" or name == "getkey":
            if self.keys:
                return self._ret(mu, val=self.keys.pop(0))
            return self._ret(mu, val=-1 if name == "getkey" else 10)
        if name == "kbhit":
            return self._ret(mu, val=1 if self.keys else 0)
        if name == "getline_":
            line = (self.lines.pop(0) if self.lines else "")
            b = line.encode("latin1")[: max(0, _s32(r1)) - 1] + b"\x00"
            mu.mem_write(r0, b)
            return self._ret(mu, val=len(b) - 1)

        # ---- numeric helpers ----
        if name == "fft":
            n, d = _s32(r2), _s32(r3)
            if n < 2 or (n & (n - 1)):
                return self._ret(mu, val=0)
            re = list(struct.unpack("<%dd" % n, mu.mem_read(r0, 8 * n)))
            im = list(struct.unpack("<%dd" % n, mu.mem_read(r1, 8 * n)))
            vals = [complex(a, b) for a, b in zip(re, im)]
            sign = 1 if d >= 0 else -1
            out = []
            for k in range(n):
                acc = 0j
                for t, v in enumerate(vals):
                    acc += v * cmath.exp(sign * 2j * math.pi * k * t / n)
                out.append(acc / n if d >= 0 else acc)
            mu.mem_write(r0, struct.pack("<%dd" % n, *[v.real for v in out]))
            mu.mem_write(r1, struct.pack("<%dd" % n, *[v.imag for v in out]))
            return self._ret(mu, val=1)
        if name == "convolve":
            na, nb = _s32(r1), _s32(r3)
            outp = struct.unpack("<I", mu.mem_read(
                mu.reg_read(UC_ARM_REG_SP), 4))[0]
            a = struct.unpack("<%dd" % na, mu.mem_read(r0, 8 * na))
            b = struct.unpack("<%dd" % nb, mu.mem_read(r2, 8 * nb))
            out = [0.0] * (na + nb - 1)
            for i, av in enumerate(a):
                for j, bv in enumerate(b):
                    out[i + j] += av * bv
            mu.mem_write(outp, struct.pack("<%dd" % len(out), *out))
            return self._ret(mu, val=0)
        if name == "solve":
            n = _s32(r2)
            A = [list(struct.unpack("<%dd" % n, mu.mem_read(r0 + 8 * n * i, 8 * n)))
                 for i in range(n)]
            b = list(struct.unpack("<%dd" % n, mu.mem_read(r1, 8 * n)))
            for k in range(n):
                piv = max(range(k, n), key=lambda i: abs(A[i][k]))
                if A[piv][k] == 0:
                    return self._ret(mu, val=0)
                A[k], A[piv] = A[piv], A[k]
                b[k], b[piv] = b[piv], b[k]
                for i in range(k + 1, n):
                    f = A[i][k] / A[k][k]
                    for j in range(k, n):
                        A[i][j] -= f * A[k][j]
                    b[i] -= f * b[k]
            x = [0.0] * n
            for i in range(n - 1, -1, -1):
                x[i] = (b[i] - sum(A[i][j] * x[j] for j in range(i + 1, n))) / A[i][i]
            mu.mem_write(r1, struct.pack("<%dd" % n, *x))
            return self._ret(mu, val=1)

        # ---- the second core: run the body serially, which is what the
        # host does too; what matters is that the callback is reached ----
        if name == "par_for":
            n = _s32(r0)
            for i in range(max(0, min(n, 4096))):
                self._call_fn(mu, r1, i, r2)
            return self._ret(mu, val=0)
        if name == "cores":
            return self._ret(mu, val=1)

        # ---- big integers: Python's own, behind a handle ----
        if name.startswith("big"):
            return self._big(mu, name[3:], r0, r1, r2, r3)

        # ---- the flash store ----
        if name == "store_pages":     return self._ret(mu, val=128)
        if name == "store_page_size": return self._ret(mu, val=4096)
        if name == "store_read":
            page = _s32(r0)
            if page < 0 or page >= 128:
                return self._ret(mu, val=0)
            addr = self.store_base + page * 4096
            mu.mem_write(addr, bytes(self.store.get(page, b"\xff" * 4096)))
            return self._ret(mu, val=addr)
        if name == "store_write":
            page, n = _s32(r0), _s32(r2)
            if page < 0 or page >= 128 or n < 0 or n > 4096:
                return self._ret(mu, val=0)
            data = bytes(mu.mem_read(r1, n)) if n else b""
            self.store[page] = data + b"\xff" * (4096 - n)
            return self._ret(mu, val=1)
        if name == "store_erase":
            self.store[_s32(r0)] = b"\xff" * 4096
            return self._ret(mu, val=1)

        # ---- files: a dict standing in for the card ----
        if name == "fopen":
            path, mode = self._cstr(r0), self._cstr(r1)
            if mode.startswith("r") and path not in self.files:
                return self._ret(mu, val=0)
            h = self.next_fh
            self.next_fh += 1
            data = self.files.get(path, b"") if mode.startswith(("r", "a")) else b""
            self.fhs[h] = {"path": path, "pos": len(data) if mode.startswith("a") else 0,
                           "data": bytearray(data), "w": not mode.startswith("r")}
            return self._ret(mu, val=0x40000000 | h)
        if name in ("fclose", "fread", "fwrite", "fgets", "fputs",
                    "fseek", "ftell", "fsize", "feof"):
            return self._file_call(mu, name, r0, r1, r2, r3)
        if name == "remove":
            return self._ret(mu, val=0 if self.files.pop(self._cstr(r0), None) is not None else -1)

        raise RuntimeError("unhandled runtime call: %s" % name)

    def _call_fn(self, mu, addr, i, ctx):
        """Call compiled code back, as par_for does on the device."""
        sp = mu.reg_read(UC_ARM_REG_SP)
        saved = [mu.reg_read(r) for r in
                 (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2,
                  UC_ARM_REG_R3, UC_ARM_REG_LR, UC_ARM_REG_PC)]
        mu.reg_write(UC_ARM_REG_R0, i & 0xFFFFFFFF)
        mu.reg_write(UC_ARM_REG_R1, ctx)
        mu.reg_write(UC_ARM_REG_LR, STOP | 1)
        mu.emu_start(addr | 1, STOP, 0, 20000000)
        mu.reg_write(UC_ARM_REG_SP, sp)
        for r, v in zip((UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2,
                         UC_ARM_REG_R3, UC_ARM_REG_LR, UC_ARM_REG_PC), saved):
            mu.reg_write(r, v)

    def _big(self, mu, op, r0, r1, r2, r3):
        """Model the big-integer handles with Python integers.

        The firmware's own arithmetic is checked exhaustively by
        tests/test_bignum.py; what matters here is that a compiled program
        passes and receives the handles correctly."""
        def new(v):
            h = self.next_big
            self.next_big += 1
            self.bigs[h] = v
            return self._ret(mu, val=0x50000000 | h)

        def get(h):
            return self.bigs.get(h & 0xFFFF, 0)

        if op == "new":    return new(_s32(r0))
        if op == "parse":  return new(int(self._cstr(r0), 0))
        if op == "copy":   return new(get(r0))
        if op == "free":
            self.bigs.pop(r0 & 0xFFFF, None)
            return self._ret(mu, val=0)
        if op == "add":    return new(get(r0) + get(r1))
        if op == "sub":    return new(get(r0) - get(r1))
        if op == "mul":    return new(get(r0) * get(r1))
        if op in ("div", "mod"):
            a, b = get(r0), get(r1)
            if b == 0:
                return self._ret(mu, val=0)
            q = abs(a) // abs(b)
            q = q if (a < 0) == (b < 0) else -q
            return new(q if op == "div" else a - q * b)
        if op == "pow":    return new(get(r0) ** max(0, _s32(r1)))
        if op == "powmod": return new(pow(get(r0), get(r1), get(r2)))
        if op == "gcd":
            return new(math.gcd(abs(get(r0)), abs(get(r1))))
        if op == "fact":
            return new(math.factorial(max(0, _s32(r0))))
        if op == "cmp":
            a, b = get(r0), get(r1)
            return self._ret(mu, val=(a > b) - (a < b))
        if op == "str":
            txt = str(get(r0)).encode("latin1")[: max(0, _s32(r2)) - 1]
            mu.mem_write(r1, txt + b"\x00")
            return self._ret(mu, val=len(txt))
        if op == "digits":
            return self._ret(mu, val=len(str(abs(get(r0)))))
        if op == "double":
            return self._ret(mu, dval=float(get(r0)))
        raise RuntimeError("unhandled big call: big%s" % op)

    def _ll(self, mu, op, r0, r1, r2, r3):
        """The 64-bit helpers. Operands arrive as r0:r1 and r2:r3."""
        def mk(lo, hi, signed):
            v = (hi << 32) | lo
            if signed and v >= 1 << 63:
                v -= 1 << 64
            return v

        def ret(v):
            v &= (1 << 64) - 1
            mu.reg_write(UC_ARM_REG_R0, v & 0xFFFFFFFF)
            mu.reg_write(UC_ARM_REG_R1, v >> 32)
            mu.reg_write(UC_ARM_REG_PC, mu.reg_read(UC_ARM_REG_LR) | 1)

        uns = op in ("uldiv", "ulmod", "ulcmp", "lushr", "ul2d", "u2l")
        a = mk(r0, r1, not uns)
        b = mk(r2, r3, not uns)

        if op == "ladd": return ret(a + b)
        if op == "lsub": return ret(a - b)
        if op == "lmul": return ret(a * b)
        if op in ("ldiv", "uldiv"):
            if b == 0: return ret(0)
            q = abs(a) // abs(b)
            return ret(q if (a < 0) == (b < 0) else -q)
        if op in ("lmod", "ulmod"):
            if b == 0: return ret(0)
            r = abs(a) % abs(b)
            return ret(r if a >= 0 else -r)
        if op == "land": return ret(a & b)
        if op == "lor":  return ret(a | b)
        if op == "lxor": return ret(a ^ b)
        if op == "lshl": return ret(a << (b & 63))
        if op == "lshr": return ret(a >> (b & 63))
        if op == "lushr":
            return ret((a & ((1 << 64) - 1)) >> (b & 63))
        if op in ("lcmp", "ulcmp"):
            return self._ret(mu, val=(a > b) - (a < b))
        if op == "i2l":  return ret(_s32(r0))
        if op == "u2l":  return ret(r0)
        if op in ("l2d", "ul2d"):
            return self._ret(mu, dval=float(mk(r0, r1, op == "l2d")))
        if op == "d2l":
            d = _d(r0, r1)
            try:
                v = int(d)
            except (ValueError, OverflowError):
                v = 0
            return ret(v)
        raise RuntimeError("unhandled 64-bit helper: %s" % op)

    # ---- helpers for the models above -------------------------------
    def _put(self, x, y, c):
        """Honour the clip rectangle, as the firmware's plat_gfx_point does."""
        if self.clip:
            cx, cy, cw, ch = self.clip
            if cw <= 0 or ch <= 0:
                return
            if x < cx or y < cy or x > cx + cw - 1 or y > cy + ch - 1:
                return
        self.fb[(x, y)] = c

    def _shape(self, mu, name, r0, r1, r2, r3):
        x, y, p, q = _s32(r0), _s32(r1), _s32(r2), _s32(r3)
        colour = struct.unpack("<I", mu.mem_read(mu.reg_read(UC_ARM_REG_SP), 4))[0] \
                 if name in ("plot_line", "plot_rect", "plot_fill_rect") else _s32(r3)
        if name == "plot_line":
            self._line(x, y, p, q, colour)
        elif name == "plot_rect":
            self._line(x, y, x + p - 1, y, colour)
            self._line(x, y + q - 1, x + p - 1, y + q - 1, colour)
            self._line(x, y, x, y + q - 1, colour)
            self._line(x + p - 1, y, x + p - 1, y + q - 1, colour)
        elif name == "plot_fill_rect":
            for yy in range(y, y + q):
                for xx in range(x, x + p):
                    self._put(xx, yy, colour)
        elif name == "plot_circle":
            self._circle(x, y, p, colour, False)
        elif name == "plot_fill_circle":
            self._circle(x, y, p, colour, True)

    def _line(self, x0, y0, x1, y1, c):
        dx = abs(x1 - x0); sx = 1 if x0 < x1 else -1
        dy = -abs(y1 - y0); sy = 1 if y0 < y1 else -1
        err = dx + dy
        while True:
            self._put(x0, y0, c)
            if x0 == x1 and y0 == y1:
                break
            e2 = 2 * err
            if e2 >= dy: err += dy; x0 += sx
            if e2 <= dx: err += dx; y0 += sy

    def _circle(self, cx, cy, r, c, fill):
        x, y, err = r, 0, 1 - r
        if r < 0:
            return
        while x >= y:
            if fill:
                for a, b in ((-x, y), (-x, -y), (-y, x), (-y, -x)):
                    self._line(cx + a, cy + b, cx - a, cy + b, c)
            else:
                for a, b in ((x, y), (y, x), (-y, x), (-x, y),
                             (-x, -y), (-y, -x), (y, -x), (x, -y)):
                    self._put(cx + a, cy + b, c)
            y += 1
            if err < 0:
                err += 2 * y + 1
            else:
                x -= 1
                err += 2 * (y - x) + 1

    def _file_call(self, mu, name, r0, r1, r2, r3):
        def fh(v):
            return self.fhs.get(v & 0xFFFF)
        if name == "fclose":
            f = fh(r0)
            if f:
                if f["w"]:
                    self.files[f["path"]] = bytes(f["data"])
                self.fhs.pop(r0 & 0xFFFF, None)
            return self._ret(mu, val=0)
        if name == "fread":
            f = fh(r3)
            n = _s32(r1) * _s32(r2)
            if not f or n <= 0:
                return self._ret(mu, val=0)
            chunk = bytes(f["data"][f["pos"]:f["pos"] + n])
            f["pos"] += len(chunk)
            if chunk:
                mu.mem_write(r0, chunk)
            return self._ret(mu, val=len(chunk) // max(1, _s32(r1)))
        if name == "fwrite":
            f = fh(r3)
            n = _s32(r1) * _s32(r2)
            if not f or n <= 0:
                return self._ret(mu, val=0)
            data = bytes(mu.mem_read(r0, n))
            f["data"][f["pos"]:f["pos"] + n] = data
            f["pos"] += n
            return self._ret(mu, val=n // max(1, _s32(r1)))
        if name == "fgets":
            f = fh(r2)
            if not f:
                return self._ret(mu, val=0)
            end = f["data"].find(b"\n", f["pos"])
            end = len(f["data"]) if end < 0 else end + 1
            chunk = bytes(f["data"][f["pos"]:min(end, f["pos"] + _s32(r1) - 1)])
            f["pos"] += len(chunk)
            mu.mem_write(r0, chunk + b"\x00")
            return self._ret(mu, val=1 if chunk else 0)
        if name == "fputs":
            f = fh(r1)
            if not f:
                return self._ret(mu, val=-1)
            b = self._cstr(r0).encode("latin1")
            f["data"][f["pos"]:f["pos"] + len(b)] = b
            f["pos"] += len(b)
            return self._ret(mu, val=len(b))
        if name == "fseek":
            f = fh(r0)
            if not f:
                return self._ret(mu, val=-1)
            base = {0: 0, 1: f["pos"], 2: len(f["data"])}[_s32(r2)]
            f["pos"] = base + _s32(r1)
            return self._ret(mu, val=0)
        if name == "ftell":
            f = fh(r0)
            return self._ret(mu, val=f["pos"] if f else -1)
        if name == "fsize":
            f = fh(r0)
            return self._ret(mu, val=len(f["data"]) if f else -1)
        if name == "feof":
            f = fh(r0)
            return self._ret(mu, val=1 if (not f or f["pos"] >= len(f["data"])) else 0)
        return self._ret(mu, val=-1)

    # ---- calling into compiled code ---------------------------------
    def call(self, fname, args=(), want_double=False, timeout_insns=4000000):
        mu = self.mu
        addr = self.p.fns[fname]
        core, stack = [], []
        for a in args:
            if isinstance(a, float):
                if len(core) % 2 == 1:
                    core.append(0)
                lo, hi = _split(a)
                if len(core) + 2 <= 4:
                    core += [lo, hi]
                else:
                    stack += [lo, hi]
            else:
                if len(core) < 4:
                    core.append(a & 0xFFFFFFFF)
                else:
                    stack.append(a & 0xFFFFFFFF)
        sp = STACK_TOP - 0x100
        if stack:
            sp -= ((len(stack) * 4 + 7) & ~7)
            for k, w in enumerate(stack):
                mu.mem_write(sp + k * 4, struct.pack("<I", w))
        for k, w in enumerate(core):
            mu.reg_write([UC_ARM_REG_R0, UC_ARM_REG_R1,
                          UC_ARM_REG_R2, UC_ARM_REG_R3][k], w)
        mu.reg_write(UC_ARM_REG_SP, sp)
        mu.reg_write(UC_ARM_REG_LR, STOP | 1)
        mu.emu_start(addr | 1, STOP, 0, timeout_insns)
        if want_double:
            return _d(mu.reg_read(UC_ARM_REG_R0), mu.reg_read(UC_ARM_REG_R1))
        v = mu.reg_read(UC_ARM_REG_R0)
        return v - 0x100000000 if v >= 0x80000000 else v

    def text(self):
        return "".join(self.out)


def run_source(src, fname="t.c"):
    import tempfile
    d = tempfile.mkdtemp()
    p = os.path.join(d, fname)
    with open(p, "w") as f:
        f.write(src)
    return Machine(Program(p))
