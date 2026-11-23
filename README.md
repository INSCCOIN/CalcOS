# CalcOS

A calculator operating system for the ClockworkPi PicoCalc (RP2350), with a C
compiler built in. You type expressions at a prompt; when an expression isn't
enough you write C on the SD card, compile it **on the device** to native
Cortex-M33 machine code, and call your functions from that same prompt.

```
> 0.1 + 0.2 - 0.3
5.55111512312578e-17
> bits(0.1)
sign=0 exp=1019(-4) frac=0x999999999999a  0x1.999999999999ap-4
> dd(2)^0.5                        extended precision, 32 digits
1.4142135623730950488016887242097e+0
> sin(iv(0, 3.5))                  a rigorous enclosure, not an estimate
[-0.350783227689620, 1]
> 5 m/s * 3 s                      quantities, checked by dimension
15 m
> to(c_light, "km/s")              and a library of physical constants
299792.458
> fit([0,1,2,3],[1,3,5,7])         least squares: slope, intercept, r
 2 1 1
> cc /hello.c
compiled /hello.c
1116 code, 39 data, 3 function(s)
> simpson(0, 2, 1000)              your C function, called from the prompt
-2.28557913336166e-16
> plot(@(x) sin(x)/x, -20, 20)
```

Version 2.2. 3 316 numbered checks across nineteen suites, including the C compiler executing real
machine code on an emulated Cortex-M33 and the board drivers running against
models of the panel and the keyboard controller.

**New in 2.2.** Named objects in the flash store, with a directory that
survives reboots. Programs compiled straight into flash, which load at boot
with no card in the machine. Long file names and 16 KB writes on the card.
Sprites, clipping and 16-colour images drawn from flash at no cost in RAM.
Exact integers of any size, at the prompt and from C. Convolution and
correlation. A parallel loop across both cores.

**New in 2.0.** 64-bit integers, function pointers, static locals and
`#include` in the compiler. A C runtime with keyboard input, a millisecond
timer, sound and a sixteen-colour 2D drawing API. Files a program can read
and write. A store in flash, outside RAM. Command history, line editing, tab
completion and scrollback. Units and physical constants at the prompt, and
regression over lists. `shutdown`, `restart`, and startup files.

---

## Contents

- [Flashing](#flashing) · [First boot](#first-boot) · [If the screen is blank](#if-the-screen-is-blank)
- [The calculator](#the-calculator) · [Editing](#editing-the-command-line) · [Commands](#commands)
- [Units and constants](#units-and-constants) · [Function reference](#function-reference)
- [Settings](#settings)
- [The C compiler](#the-c-compiler) · [What compiled C can call](#what-compiled-c-can-call)
- [Exact integers](#exact-integers)
- [Storage](#storage) · [The flash store](#the-flash-store) — [objects](#named-objects), [from the prompt](#from-the-prompt), [from C](#from-a-c-program) · [Programs in flash](#programs-in-flash) · [Startup files](#startup-files)
- [The second core](#the-second-core)
- [Clocking](#clocking) · [Display](#display)
- [Building](#building) · [Tests](#tests) · [Hardware](#hardware)

---

## Flashing

Hold BOOTSEL, plug in, and drop the matching file on the RP2350 drive:

| File | Module |
|---|---|
| `dist/calcos-2.2-pico2w.uf2` | Raspberry Pi Pico 2 W |
| `dist/calcos-2.2-pico2.uf2` | Raspberry Pi Pico 2 |

219 KB of flash, 236 KB of static RAM, leaving roughly 285 KB for heap and
stack.

## First boot

You should see a colour-bar test pattern for about a second, then the prompt.

**The console mirrors to USB serial.** Open `/dev/ttyACM0` at any baud rate
and you get the same prompt, plus a staged boot log:

```
[calcos] boot
[calcos] sd card mounted
[calcos] settings loaded
[calcos] clock set
[calcos] keyboard ready, backlight on
[calcos] panel initialised
[calcos] test pattern shown
[calcos] console ready
```

Where it stops tells you what failed. If it reaches `console ready`, the
system is running and anything still wrong is a display setting.

### If the screen is blank

Type `display` on the serial console. It prints the current panel state and
what to try. In order:

1. `set backlight 255` — the screen backlight is driven by the **keyboard**
   controller, not the RP2350. A working panel looks dead if it is off.
2. If the **keys** light up instead of the screen, this unit maps the two
   backlight registers the other way round: `set blswap 1`.
3. `set lcdbits 18` — some panels want 18-bit pixels rather than 16.
4. `set lcdmadctl 0x40` — if text is mirrored or rotated.
5. `set lcdspi 20` — if output is garbled rather than absent.
6. `display test` — colour bars, then white, then back to the console.

`set save` keeps whatever works, so it survives the next boot.

If the screen works but keys do not, the keyboard FIFO protocol at register
`0x09` varies between controller firmware revisions. Serial input keeps
working, so the machine stays usable.

---

## The calculator

### Value types

Four numeric kinds, promoted automatically when they meet:

| Kind | What it is |
|---|---|
| real / complex | IEEE-754 binary64, `2 + 3i` |
| extended | double-double, about 32 significant digits, `dd(2)` |
| interval | outward-rounded and rigorous, `iv(1, 2)` |
| matrix | complex elements, `[1,2;3,4]` |

Mixing promotes toward the stronger kind: `dd(1)/3` is extended,
`iv(1,2)*2` is an interval.

### Syntax

```
x = 3                      assignment
f(x) = x^2 + 1             define a function
@(x) sin(x)/x              a lambda, for integrate/root/diff/ode/plot
[1,2;3,4]                  matrix literal; blocks concatenate
1:0.5:3                    range
A(2,1)   v(3)              1-based indexing
A(1,2) = 7                 indexed assignment
2 + 3i                     complex literal
x'                         conjugate transpose
5!                         factorial
2^10                       power (right associative)
.*  ./  .^                 elementwise operations
;                          at end of line: compute but don't print
#                          comment
ans                        the last result
```

Constants: `pi` `e` `phi` `i` `eps` `inf` `nan`, plus the physical constants
below.

### Editing the command line

| Key | What it does |
|---|---|
| left / right | move the cursor |
| home / end, or ctrl-A / ctrl-E | start and end of the line |
| up / down | walk back through what you have typed |
| backspace / delete | remove before or under the cursor |
| tab | complete a command, function, variable or file name |
| ctrl-W | delete the word behind the cursor |
| ctrl-U / ctrl-K | clear the line, or from the cursor to its end |
| ctrl-C | abandon the line |
| ctrl-L | clear the screen |
| page up / page down | scroll back through what has gone past |
| F1 – F10 | a command each, below |

The function keys stand for the commands that are tedious to type on a thumb
keyboard. On an empty line the command runs; with something already typed the
word is inserted at the cursor instead, so a function key can never destroy a
line being worked on. F6 to F10 are the shifted F1 to F5.

| F1 | F2 | F3 | F4 | F5 | F6 | F7 | F8 | F9 | F10 |
|---|---|---|---|---|---|---|---|---|---|
| `help` | `vars` | `funcs` | `units` | `consts` | `mem` | `ls` | `display` | `history` | `clear` |

They are a table at the top of `src/lineedit.c` if you want different ones.

Tab decides what to offer from where you are: the first word of a line
completes against commands and function names, a word after `cc`, `cat`,
`load`, `save` or `rm` completes against what is on the card, and anything
else completes against functions and the names you have defined. A unique
match is filled in; an ambiguous one lists what is possible.

The last 240 lines that scroll off the top are kept, which is about nine
screens. Anything printed snaps the view back to the bottom.

`history` lists what you have typed, `history clear` forgets it.

### Units and constants

A number followed by the name of a unit is a quantity, and quantities carry
their dimensions through arithmetic:

```
> 5 m/s * 3 s
15 m
> 70 kg * 9.81 m/s^2
686.7 N
> 3 km + 200 m
3200 m
> 2 m + 3 s
error: cannot add m and s
```

Everything is held in SI base units and printed that way, with a familiar
name used where one fits exactly — `N`, `J`, `W`, `Pa`, `V`, `F`, `Hz` and
the rest. A power binds to the unit, so `16 m^2` is sixteen square metres.

A unit name only means a unit where you have not claimed the name yourself:
after `m = 5`, `m` is your variable.

Functions that would be meaningless on a quantity refuse it rather than
quietly dropping the unit, so `sin(1 m)` is an error. `abs`, `min`, `max`,
`sum`, `mean`, `median` and the rest of that family carry the unit through,
`sqrt` halves its exponents, and `var` squares them.

| | |
|---|---|
| `units` | list every unit name |
| `consts` | list every physical constant, with its value and what it is |
| `conv(5, "mile", "km")` | convert a plain number between units |
| `to(3000 m, "km")` | a quantity expressed in another unit, as a number |
| `unit("kWh")` | one of that unit, as a quantity |
| `const("k_B")` | a constant by name |

The constants are CODATA 2018 and carry their units: `c_light`, `h_planck`,
`hbar`, `q_e`, `k_B`, `N_A`, `R_gas`, `G_grav`, `g0`, `m_e`, `m_p`, `m_n`,
`amu`, `eps0`, `mu0`, `sigma_SB`, `R_inf`, `a0`, `mu_B`, `alpha_fs`,
`atm_p`, `T_zero`. Those defined exactly by the SI are marked as such by
`consts`.

```
> h_planck * c_light / (500 nm)
3.97289171429786e-19 J
```

### Exact integers

A double is exact up to 2^53 and an estimate after that. `big` crosses that
line: a big integer is exact however many digits it takes, up to about nine
thousand of them.

```
> big(2)^100
1267650600228229401496703205376
> bigfact(100)
93326215443944152681699238856266700490715968264381621468592963895217599993229915608941463976156518286253697920827223758251185210916864000000000000000000000000
> ndigits(bigfact(1000))
2568
> powmod(2, 1000, 1000000007)
688423210
```

A big integer spreads to anything it touches: `big(2)^64 - 1` is exact, and
so is `big(10)^20 / big(10)^5`. Division that does not come out exactly
falls back to a double rather than silently truncating — `big(7)/big(2)` is
3.5 — because that is nearly always what was meant.

| | |
|---|---|
| `big(x)` | from a whole number, or from a string for anything larger |
| `bigfact(n)` | an exact factorial |
| `powmod(a, b, m)` | a^b mod m, the operation that makes this worth having |
| `gcd(a, b)`, `lcm(a, b)` | exact |
| `ndigits(x)` | how many decimal digits a number has |
| `str(x)` | the number as a string |

The same integers are available to compiled C as handles — see
[What compiled C can call](#what-compiled-c-can-call). The arithmetic is
checked against Python's own integers over a few thousand random cases, with
the operands shaped to make the correction steps in the division algorithm
actually fire.

### Commands

A line is a command when its first word is one of these *and* you have not
defined a variable or function of that name — so `free = 3` keeps working.
A leading `:` always forces a command.

| Command | What it does |
|---|---|
| `help` | commands and syntax |
| `help NAME` | one function |
| `help all` | every function name |
| `cc FILE` | compile a C file from the card |
| `run` | run the compiled program's `main()` |
| `cfuncs` | list the functions the compiled program defines |
| `unload` | drop the compiled program |
| `ls [DIR]` | list the card |
| `cat FILE` | print a file |
| `set` | list every setting and its value (see [Settings](#settings)) |
| `set KEY` | one setting, with what it does |
| `set help` | every setting with its description |
| `set KEY VALUE` | change one |
| `set save` / `set load` / `set reset` | persist, re-read, or restore defaults |
| `display` | panel state and blank-screen help |
| `display test` | colour-bar test pattern |
| `display reset` | panel settings back to defaults and reapply |
| `vars` | variables and their values |
| `funcs` | functions you have defined |
| `save FILE` | write your variables to the card |
| `load FILE` | read a file back as input lines |
| `digits N` | significant digits shown (1–17) |
| `ddigits N` | digits shown for extended precision (1–34) |
| `deg` / `rad` | angle mode |
| `bits` | toggle showing IEEE-754 fields under every result |
| `mem` / `free` | memory in use |
| `clear` | clear the workspace |
| `about` | version |
| `units` / `consts` | list unit names, or physical constants |
| `store` | the flash store: `list`, `put`, `get`, `rm`, `view`, `free`, `format`, and the raw `map`, `dump`, `erase` (see [The flash store](#the-flash-store)) |
| `ccs NAME FILE` | compile into flash, so it survives a reboot |
| `use NAME` | load a program kept in flash |
| `history` | what you have typed; `history clear` forgets it |
| `rm FILE` | delete a file from the card |
| `restart` | reboot the board |
| `shutdown` | save settings, blank the panel and power down |
| `quit` / `exit` | leave the shell |

### Function reference

**Elementary**

| Function | Args | Description |
|---|---|---|
| `sqrt` | 1 | square root (complex, extended and interval aware) |
| `cbrt` | 1 | real cube root |
| `exp` | 1 | e^x |
| `log` | 1-2 | natural log, or log(x, base) |
| `log2` | 1 | base-2 log |
| `log10` | 1 | base-10 log |
| `log1p` | 1 | log(1+x), accurate for tiny x |
| `expm1` | 1 | exp(x)-1, accurate for tiny x |
| `abs` | 1 | absolute value / modulus |
| `sign` | 1 | -1, 0 or 1 |
| `floor` | 1 | round toward -inf |
| `ceil` | 1 | round toward +inf |
| `round` | 1 | round half away from zero |
| `trunc` | 1 | round toward zero |
| `mod` | 2 | floored modulus |
| `rem` | 2 | truncated remainder |
| `pow` | 2 | x^y |
| `fma` | 3 | fused multiply-add, one rounding |
| `min` | 1-8 | smallest value |
| `max` | 1-8 | largest value |
| `hypot` | 2 | sqrt(x^2+y^2) without overflow |

**Trigonometry**

| Function | Args | Description |
|---|---|---|
| `sin` | 1 | sine |
| `cos` | 1 | cosine |
| `tan` | 1 | tangent |
| `asin` | 1 | inverse sine |
| `acos` | 1 | inverse cosine |
| `atan` | 1 | inverse tangent |
| `atan2` | 2 | atan2(y, x) |
| `sinh` | 1 | hyperbolic sine |
| `cosh` | 1 | hyperbolic cosine |
| `tanh` | 1 | hyperbolic tangent |
| `asinh` | 1 | inverse hyperbolic sine |
| `acosh` | 1 | inverse hyperbolic cosine |
| `atanh` | 1 | inverse hyperbolic tangent |

**Complex**

| Function | Args | Description |
|---|---|---|
| `re` | 1 | real part |
| `im` | 1 | imaginary part |
| `conj` | 1 | complex conjugate |
| `arg` | 1 | argument (phase) |
| `polar` | 2 | polar(r, theta) -> complex |

**Special functions**

| Function | Args | Description |
|---|---|---|
| `gamma` | 1 | gamma function |
| `lgamma` | 1 | log gamma |
| `digamma` | 1 | psi(x) |
| `beta` | 2 | beta function |
| `erf` | 1 | error function |
| `erfc` | 1 | complementary error function |
| `zeta` | 1 | Riemann zeta |
| `besselj0` | 1 | Bessel J0 |
| `besselj1` | 1 | Bessel J1 |
| `bessely0` | 1 | Bessel Y0 |
| `bessely1` | 1 | Bessel Y1 |
| `ei` | 1 | exponential integral |
| `li` | 1 | logarithmic integral |
| `factorial` | 1 | x! via gamma |
| `binomial` | 2 | n choose k |

**Floating-point inspection**

| Function | Args | Description |
|---|---|---|
| `ulp` | 1 | spacing of doubles at x |
| `nextup` | 1 | next double toward +inf |
| `nextdown` | 1 | next double toward -inf |
| `relerr` | 2 | relative error of a against b |
| `ulpdiff` | 2 | signed difference in ulps |
| `bits` | 1 | IEEE-754 fields of x |
| `hex` | 1 | exact hex float form |
| `exponent` | 1 | binary exponent |
| `significand` | 1 | mantissa in [1,2) |
| `isnan` | 1 | 1 if NaN |
| `isinf` | 1 | 1 if infinite |

**Extended precision and intervals**

| Function | Args | Description |
|---|---|---|
| `dd` | 1 | to extended precision; dd("...") parses digits |
| `hi` | 1 | high limb of an extended value |
| `lo` | 1 | low limb of an extended value |
| `iv` | 1-2 | interval: iv(a,b) or iv(x) |
| `inf` | 1 | interval lower bound |
| `sup` | 1 | interval upper bound |
| `wid` | 1 | interval width |
| `mid` | 1 | interval midpoint |

**Building values**

| Function | Args | Description |
|---|---|---|
| `zeros` | 0-2 | zero matrix |
| `ones` | 0-2 | matrix of ones |
| `eye` | 0-2 | identity matrix |
| `rand` | 0-2 | uniform random |
| `randn` | 0 | standard normal sample |
| `seed` | 1 | seed the generator |
| `linspace` | 2-3 | evenly spaced row vector |
| `size` | 1 | [rows, cols] |
| `numel` | 1 | element count |
| `reshape` | 3 | reshape(m, rows, cols) |
| `transpose` | 1 | conjugate transpose |
| `diag` | 1 | diagonal to vector, or vector to diagonal |

**Linear algebra**

| Function | Args | Description |
|---|---|---|
| `det` | 1 | determinant (LU) |
| `inv` | 1 | matrix inverse |
| `solve` | 2 | solve A x = b with iterative refinement |
| `trace` | 1 | sum of the diagonal |
| `norm` | 1-2 | norm(x) or norm(x, p) |
| `cond` | 1 | 1-norm condition number |
| `dot` | 2 | exact-product dot product |
| `cross` | 2 | 3-vector cross product |

**Statistics**

| Function | Args | Description |
|---|---|---|
| `sum` | 1 | compensated sum |
| `prod` | 1 | product |
| `mean` | 1 | arithmetic mean |
| `var` | 1 | sample variance (two-pass) |
| `std` | 1 | sample standard deviation |
| `median` | 1 | median |
| `mode` | 1 | most frequent value in a list |
| `quantile` | 2 | quantile of a list, p from 0 to 1 |
| `corr` | 2 | correlation coefficient of two lists |
| `fit` | 2 | least-squares line through two lists: [slope, intercept, r] |
| `sort` | 1 | ascending sort |
| `cumsum` | 1 | running compensated sum |

**Units and constants**

| Function | Args | Description |
|---|---|---|
| `conv` | 3 | convert a number between units: conv(5,"mile","km") |
| `to` | 2 | a quantity expressed in another unit, as a plain number |
| `unit` | 1 | one of the named unit, as a quantity |
| `const` | 1 | a physical constant by name, with its unit |

**Exact integers**

| Function | Args | Description |
|---|---|---|
| `big` | 1 | an exact integer of any size: big(2)^100, big("123...") |
| `bigfact` | 1 | exact factorial |
| `powmod` | 3 | a^b mod m, exactly |
| `gcd` | 2 | greatest common divisor, exactly |
| `lcm` | 2 | least common multiple, exactly |
| `ndigits` | 1 | how many decimal digits a whole number has |
| `str` | 1 | a value as a string |

**Numerical methods**

| Function | Args | Description |
|---|---|---|
| `polyval` | 2 | Horner with fma; highest power first |
| `integrate` | 3 | integrate(f, a, b) by tanh-sinh |
| `root` | 3 | root(f, a, b) by Brent |
| `newton` | 2 | newton(f, x0) |
| `diff` | 2-3 | diff(f, x [, order]) by Richardson |
| `minimize` | 3 | minimize(f, a, b) by golden section |
| `ode` | 4-5 | ode(f, t0, y0, t1 [, tol]) RK45 |
| `nsum` | 3 | nsum(f, lo, hi) compensated series sum |
| `fft` | 1 | radix-2 FFT |
| `ifft` | 1 | inverse FFT |
| `convolve` | 2 | discrete convolution of two lists |
| `xcorr` | 2 | cross-correlation of two lists |

**Other**

| Function | Args | Description |
|---|---|---|
| `plot` | 3-5 | plot(f, x0, x1 [, y0, y1]) graph a function |
| `typeof` | 1 | name of a value's type |
| `print` | 1-8 | print values |

131 functions in total.

Compensated arithmetic is the default where it matters: `sum` is Neumaier,
`dot` accumulates exact products in double-double, `polyval` is fma-Horner,
matrix multiply accumulates in double-double, `var` is two-pass, and `solve`
does a step of iterative refinement on a double-double residual.

`integrate` is tanh–sinh, so an endpoint singularity still converges:
`integrate(@(x) log(x), 0, 1)` gives exactly −1. The coordinate matters more
than the rule, though — see the note under [Tests](#tests).

---

## Settings

System settings live in `/calcos.cfg` on the SD card: a plain text file you
can edit on a PC or change from the prompt.

```
set                   list everything, with what each one does
set lcdbits 18        change one
set save              write it to the card
set load              re-read it
set reset             back to defaults
```

| Key | Default | Range | Description |
|---|---|---|---|
| `cpu` | `150` | 100 to 300 | system clock in MHz, applied at the next boot |
| `splash` | `1` | on / off | show a display test pattern at startup |
| `fg` | `0xC8E6C8` | 0 to 0xFFFFFF | text colour, 0xRRGGBB |
| `bg` | `0x080A08` | 0 to 0xFFFFFF | background colour, 0xRRGGBB |
| `font` | `1` | 0 to 1 | 0 regular, 1 bold |
| `backlight` | `220` | 0 to 255 | screen backlight |
| `kbdlight` | `160` | 0 to 255 | keyboard backlight |
| `blswap` | `0` | on / off | set to 1 if the keys light up instead of the screen |
| `lcdbits` | `16` | 16 to 18 | panel pixel format: 16 or 18 |
| `lcdmadctl` | `0x48` | 0 to 0xFF | panel orientation byte, default 0x48 |
| `lcdinvert` | `1` | on / off | panel inversion, on for IPS |
| `lcdspi` | `0` | 0 to 150 | panel SPI clock in MHz, 0 means a quarter of the CPU |
| `digits` | `15` | 1 to 17 | significant digits shown |
| `ddigits` | `32` | 1 to 34 | digits shown for extended precision |
| `deg` | `0` | on / off | 1 for degrees, 0 for radians |

Colour, font, both backlights and all four panel settings apply the moment
you change them — the panel is re-initialised in place. Only `cpu` waits for
a reboot.

---

## The C compiler

Written from scratch for this project, MIT licensed, emitting **native
Thumb-2** — no bytecode, no interpreter. About 3,500 lines.

### What it supports

| | |
|---|---|
| Types | `char` `short` `int` `unsigned` `long long` `unsigned long long` `float` `double` `void`, pointers, pointers to pointers, pointers to functions, multi-dimensional arrays |
| Aggregates | `struct` `union` `enum` `typedef`, nested, with brace initializers |
| Statements | `if` `else` `while` `do` `for` `switch` `case` `default` `break` `continue` `return` `goto`, labels, blocks |
| Expressions | the full precedence ladder, `?:`, comma, casts, `sizeof`, `++` `--` prefix and postfix, all compound assignments, `.` and `->`, subscripting, calls |
| Functions | recursion, forward references, prototypes, up to 16 parameters, function pointers and arrays of them |
| Storage | `static` locals with initializers, initialised once when the program is loaded; globals; `typedef` |
| Preprocessor | `#define` object- and function-like, `#undef`, `#if` `#ifdef` `#ifndef` `#else` `#elif` `#endif`, `defined()`, `#include "file.h"` read from the card (angled includes are satisfied by doing nothing, because the runtime is built in) |
| Literals | decimal, hex, character, floating point with exponents, strings with escapes and adjacent concatenation |

`double` is real binary64, done through the same AEABI soft-float routines the
firmware itself uses, because the M33's FPU is single-precision only. `float`
is stored in 4 bytes and widened for arithmetic.

`long long` is a real 64-bit integer, not a double wearing a hat: it is kept
in a register pair and multiplied, divided, shifted and compared by firmware
helpers, so `(1LL << 53) + 1` keeps the bit a double would drop. `printf`
takes `%lld`. Note that `long` on its own is 32 bits here; only `long long`
is 64.

A function pointer is called through a register, so callbacks, dispatch
tables and simple vtables all work:

```c
double apply(double (*f)(double), double x) { return f(x); }
```

### Not supported

Variadic *definitions* (calling `printf` is fine), bitfields,
floating-point `switch`, `const` enforcement, and `long double`.

### The program model

A `.c` file is both a program and a library. `run` calls its `main()`, and any
function it defines is callable from the calculator prompt — provided its
parameters are all `double` or all `int`, and there are at most four.

```c
double simpson(double a, double b, double n) { ... }
```
```
> cc /hello.c
> simpson(0, 3, 10000)
```

### What compiled C can call

*Console*

```c
int printf(char *, ...);
int putchar(int);
int puts(char *);
```

*Maths*

```c
double sin(double);
double cos(double);
double tan(double);
double asin(double);
double acos(double);
double atan(double);
double atan2(double, double);
double sinh(double);
double cosh(double);
double tanh(double);
double exp(double);
double log(double);
double log10(double);
double log2(double);
double pow(double, double);
double sqrt(double);
double cbrt(double);
double fabs(double);
double floor(double);
double ceil(double);
double round(double);
double trunc(double);
double fmod(double, double);
double hypot(double, double);
double fmin(double, double);
double fmax(double, double);
```

*The calculator's own special functions*

```c
double gamma_fn(double);
double lgamma_fn(double);
double erf_fn(double);
double erfc_fn(double);
double zeta_fn(double);
double digamma_fn(double);
double besselj0_fn(double);
double besselj1_fn(double);
```

*Memory and strings*

```c
char * malloc(int);
void free(char *);
char * memset(char *, int, int);
char * memcpy(char *, char *, int);
int strlen(char *);
int strcmp(char *, char *);
char * strcpy(char *, char *);
char * strncpy(char *, char *, int);
char * strcat(char *, char *);
int abs(int);
```

*Drawing on the panel*

```c
void plot_clear(void);
void plot_point(int, int, int);
int plot_get(int, int);
void plot_line(int, int, int, int, int);
void plot_rect(int, int, int, int, int);
void plot_fill_rect(int, int, int, int, int);
void plot_circle(int, int, int, int);
void plot_fill_circle(int, int, int, int);
void plot_text(int, int, char *, int);
void plot_clip(int, int, int, int);
void plot_noclip(void);
void plot_palette(int, int);
void plot_sprite(int, int, int, int, char *);
void plot_blit(int, int, int, int, char *);
int plot_image(int, int, char *);
int plot_image_width(char *);
int plot_image_height(char *);
void plot_auto(int);
void plot_flush(void);
void plot_show(void);
void plot_hide(void);
int plot_width(void);
int plot_height(void);
int rgb(int, int, int);
```

*Keyboard, time and sound*

```c
int getchar(void);
int getkey(void);
int kbhit(void);
int getline_(char *, int);
int millis(void);
int micros(void);
void delay(int);
void tone(int, int);
void notone(void);
```

*Files on the card*

```c
char * fopen(char *, char *);
int fclose(char *);
int fread(char *, int, int, char *);
int fwrite(char *, int, int, char *);
int fgets(char *, int, char *);
int fputs(char *, char *);
int fseek(char *, int, int);
int ftell(char *);
int fsize(char *);
int feof(char *);
int remove(char *);
```

*The flash store, outside RAM*

```c
int store_pages(void);
int store_page_size(void);
char * store_read(int);
int store_write(int, char *, int);
int store_erase(int);
```

*Exact integers*

```c
char * bignew(int);
char * bigparse(char *);
char * bigcopy(char *);
void bigfree(char *);
char * bigadd(char *, char *);
char * bigsub(char *, char *);
char * bigmul(char *, char *);
char * bigdiv(char *, char *);
char * bigmod(char *, char *);
char * bigpow(char *, int);
char * bigpowmod(char *, char *, char *);
char * biggcd(char *, char *);
char * bigfact(int);
int bigcmp(char *, char *);
int bigstr(char *, char *, int);
int bigdigits(char *);
double bigdouble(char *);
```

*Numeric helpers*

```c
int fft(char *, char *, int, int);
void convolve(char *, int, char *, int, char *);
int solve(char *, char *, int);
```

*The second core*

```c
void par_for(int, char *, char *);
int cores(void);
```

148 entry points, of which 118 are listed above; the rest are the
soft-float and 64-bit helpers the compiler emits for you.

Declaring what you use is optional but worth it: the compiler reads a
declaration to convert arguments, so `sqrt(2)` widens its argument only
where `double sqrt(double);` is in scope. The names above are built in,
so `#include <stdio.h>` and friends are satisfied by doing nothing.

### Limits

| | |
|---|---|
| Compiled code | 128 KB |
| Compiled data and strings | 48 KB |
| Source file | 32 KB |
| Compiler working memory | 96 KB |

All four are constants at the top of `src/runtime.c`, and `mem` prints what
is actually free. 2.2 moved 32 KB out of the compiler's working memory and
into the code buffer after measuring the arena: the largest example uses
45 KB of it, so the rest was buying nothing.

### Sound

`tone(hz, ms)` plays a square wave on the buzzer and `notone()` stops it.
Two things keep a note from locking the machine up: the wait ends early if a
key is pressed, leaving the key for whoever asked for it, so a key stops a
tune part way through a note rather than at the end of one; and a single call
blocks for at most five seconds however it was written. A program that
returns while a tone is still playing has it stopped for it — the buzzer
cannot outlive the program that started it.

`tone(hz, 0)` starts a note and returns at once, for a program that wants to
do something else while it sounds.

### What a program may keep

A program's memory is gone the moment it ends, and `unload` or a reboot takes
the program with it. Two things outlive it:

- a **file on the card**, through `fopen` and friends. A file opened for
  writing is accumulated in RAM and written in one go when it is closed,
  because a FAT append interrupted by a flat battery is how a card gets
  corrupted. Up to 16 KB, one writer at a time; long names are fine.
- the **flash store**, below.

### Examples

In `examples/`, to copy onto the card. Every one of them is compiled by the
test suite, and the ones that only compute are run, so they match the
compiler that ships with them.

| File | What it shows |
|---|---|
| `hello.c` | numeric integration; `simpson(0, 2, 1000)` from the prompt |
| `stats.c` | structs, arrays and statics; `variance(64)` from the prompt |
| `bigint.c` | 64-bit integers, function pointers, a table of them, a static local |
| `mandel.c` | the Mandelbrot set in sixteen colours, with a timer |
| `life.c` | an interactive loop: draw, `delay`, `kbhit` to stop |
| `tune.c` | `tone` on the buzzer |
| `notes.c` | file I/O: data that outlives the program |
| `store.c` | the flash store: a table that costs no RAM to read |

---

## Storage

SD card, FAT16 or FAT32, with or without an MBR partition table. Reading
handles long file names and subdirectories.

Writing is deliberately narrow: whole files only, into an existing
directory. **Long names work** — created, overwritten and deleted — with a
`~1` short alias generated alongside, as every other FAT driver does. It never renames or moves anything, it rewrites every copy
of the FAT, and it only repoints a directory entry once the new data is safely
on the card — so an interrupted write leaves a consistent filesystem rather
than a corrupt one. On FAT32 the FSInfo free-cluster hint is marked unknown
after a change, which is the spec's own way of saying "recount", rather than
leaving a stale number behind.

This is checked rather than asserted: the host suite writes, overwrites,
grows and shrinks files on real FAT16 and FAT32 images, verifies the
contents byte for byte, confirms a pre-existing file is untouched, runs
`fsck.vfat` over the result, and then reads the long names back with
`mdir` — because `fsck` proves the structure is sound and only a real FAT
driver proves the names are right.

## The flash store

### What it is, and what it is not

There is no MMU on this part, so RAM cannot be paged in and out behind a
program's back: a pointer is a physical address and nothing can fault it in.
A page file in the usual sense is not possible here, and pretending otherwise
would be the wrong answer.

What *is* possible is the useful half of it. The RP2350 has 4 MB of flash and
the firmware uses about 240 KB of it. Flash is memory-mapped through the XIP
cache, so the top **512 KB** is set aside as a store of **128 pages of 4 KB**,
and reading one hands back a pointer straight into it. Data kept there
occupies **no SRAM at all** and reads at cache speed.

So it is not swap. It is a filing cabinet in the address space: you put
something in a drawer, and from then on you can read it for free.

| | |
|---|---|
| Size | 512 KB, 128 pages of 4096 bytes |
| Where | the top of flash, at `0x10380000` |
| Reading | a pointer; no copy, no RAM, cache speed |
| Writing | erase then program a whole page, tens of milliseconds |
| Lifetime | survives the program, power off, and reflashing the firmware |
| Wear | roughly 100,000 erases per page |

### Named objects

Page numbers are a poor way to keep track of anything, so page 0 holds a
directory and everything else is found by name. An object occupies a run of
whole pages; there are up to 84 of them.

| Command | What it does |
|---|---|
| `store` | a summary: pages, objects, what is free |
| `store list` | every object: name, size, kind, where it lives |
| `store free` | how much room is left |
| `store put NAME FILE` | copy a file from the card into a named object |
| `store get NAME FILE` | write an object back out to the card |
| `store view NAME [OFF]` | an object's bytes, as hex and characters |
| `store rm NAME` | delete one |
| `store format` | erase everything and start a fresh directory |

```
> store put table data.bin
4096 bytes stored
> store list
table             4096 data
  page 1, 1 page(s)
> store free
126 of 128 pages free
504 KB free, 8 KB used
```

**Replacing an object is crash-safe.** The new copy is written to pages the
old one is not using, and only then does the directory change to point at
it; if the power goes between the two, the old object is still there and
intact. The cost is that replacing something needs room for both copies for
a moment, which is the right trade on a part where a half-written directory
would lose the lot.

### From the prompt

The raw pages are still there underneath, for when a name is more ceremony
than the job needs:

| Command | What it does |
|---|---|
| `store map` | one character per page: `#` holds data, `.` is erased |
| `store dump N [OFF]` | a page's bytes, as hex and characters |
| `store save N FILE` | copy a file into page N |
| `store load N FILE` | copy page N out to a file |
| `store erase N` / `all` | erase a page, or every page in use |

A page that has only ever been erased reads as all `0xFF`, and that is how a
free page is told from a used one — nothing keeps a separate index, the
flash itself is the record.

### From a C program

```c
int   store_pages(void);          /* 128, or 0 if there is no store */
int   store_page_size(void);      /* 4096 */
void *store_read(int page);       /* a pointer into flash, or null */
int   store_write(int page, void *data, int len);
int   store_erase(int page);
```

Reading is the whole point:

```c
int *table = (int *)store_read(0);     /* no copy, no RAM */
printf("%d\n", table[500]);
```

Writing is the expensive side. A page must be erased before it can be
programmed, erasing takes tens of milliseconds with interrupts off, and a
page wears out after on the order of 100,000 erases — writing one per frame
would destroy the part in days. `store_write` erases and programs one whole
page; a short write leaves the rest of the page erased.

A worked example, `examples/store.c`: build a table once, then read it back
from the prompt as often as you like.

```c
#define N 1024
static int scratch[N];

int build(void) {
    int i;
    for (i = 0; i < N; i++) scratch[i] = i * i;
    return store_write(0, scratch, N * 4);
}

double lookup(double n) {
    int *table = (int *)store_read(0);
    int i = (int)n;
    if (!table || i < 0 || i >= N) return -1.0;
    return (double)table[i];
}
```

```
> cc /store.c
> run
> lookup(500)
250000
```

`scratch` costs 4 KB of RAM while it is being filled, and nothing afterwards:
once `build` has run, `lookup` reads the table out of flash with no RAM at
all, and it is still there after a power cycle.

### Programs in flash

A compiled program can live in the store too, which means it is there after
a reboot and runs with no card in the machine at all:

```
> ccs numlib /numlib.c        compile, and keep it in flash as "numlib"
compiled /numlib.c
2104 code, 312 data, 6 function(s)
stored as numlib, 2608 bytes
```

Then, in any later session:

```
> use numlib
numlib loaded: 2104 code, 312 data, 6 function(s)
> nroot(2, 5)
1.14869835499703501
```

Name it `boot` and it loads at every startup by itself, with no command and
no card. Holding escape at power-on skips that, as it skips everything else.

There is one restriction, and it is not negotiable: **a stored program only
runs on the firmware that compiled it.** Compiled code holds absolute
addresses into this firmware image — every call to `sin` or `printf` is the
real address of that function — so a snapshot records the build it came from
and the addresses its buffers were at, and refuses to load into anything
else. Reflash the firmware and `use` will tell you to compile it again
rather than jumping into whatever is now at that address.

`examples/numlib.c` is a library written for this: a handful of functions
that take and return doubles, so the prompt can call them.

### Things worth knowing

- **It survives reflashing.** The store is outside the UF2, so a firmware
  update leaves it alone. `store erase all` is the way to clear it.
- **It does not survive a bad page number.** Reads outside the store return
  null and writes do nothing, rather than wrapping onto the firmware.
- **It cannot overlap the firmware.** The store starts megabytes above the
  end of the linked image, which `tests/check_flash_map.py` checks against
  the real ELF rather than taking on trust.
- **Writing stops the world.** Erase and program run with interrupts off and
  the XIP window disabled, so nothing else happens for those milliseconds —
  including the USB console.
- **A name is at most 31 characters**, and there is room for 84 objects.
- **Use the card instead** if you need more than 512 KB, want to read the
  data on a computer, or will rewrite it often. A file has no wear limit.

## The second core

The RP2350 has two cores and until 2.2 only one was used. The second one now
runs a parallel loop:

```c
static void row(int y, void *ctx) { /* draw one row of the picture */ }
...
par_for(height, row, &state);       /* both cores, then back here */
```

`par_for(n, fn, ctx)` calls `fn(i, ctx)` for every `i` below `n`, splitting
the indices between the cores and returning when both have finished.
`cores()` says how many there are.

It is a parallel loop and not threads, deliberately. Threads would mean
making the whole calculator re-entrant for the sake of a part with no cache
coherency to speak of, and nearly all the slow work here — rows of a matrix,
rows of a picture, independent samples — is a loop whose iterations do not
depend on each other. That shape is safe by construction.

The rules for the body:

- **It must not depend on the other indices.** The indices are split
  statically, core 0 taking the even ones and core 1 the odd, so there is no
  shared counter and nothing to lock — and no ordering either.
- **It must not write to the flash store.** Writing flash turns the XIP
  window off, and core 1 is executing from flash. `store_write` and
  `store_erase` refuse while a parallel loop is running rather than taking
  the machine down with them.
- **It must not allocate or touch the calculator.** Nothing in the
  interpreter is re-entrant.

Matrix multiplication uses it already, for products big enough to be worth
the handover — below about four thousand multiply-adds the cost of waking
the other core exceeds what it saves.

## Startup files

If the card has either of these, they run at boot:

| File | What happens |
|---|---|
| `/startup.txt` | each line is run as if typed at the prompt |
| `/autoexec.c` | compiled, and its `main()` run |

`startup.txt` goes first. **Hold escape at power-on to skip both**, which is
the way back from a startup file that does something unwelcome — the same key
that keeps the default clock.

## Clocking

Starts at the part's own **150 MHz**. `set cpu 212` then `set save` raises it
at the next boot, with the core voltage lifted one step; if the clock is
refused the system stays where it is rather than risking a machine that will
not start.

**Hold Escape at power-on to keep the default clock** regardless of what the
card says. That is the way back from a setting the board will not take.

## Display

The console is 40 columns by 26 rows in an 8×12 cell — the same geometry
MMBasic uses on this hardware, because 40 columns is what a 320-pixel line
affords and 12 rows of height is what makes it legible.

The font is rendered from DejaVu Sans Mono by `tools/make_font.py`, not
copied from another project: MMBasic's own font is adapted from the Rinky-Dink
UTFT set, which is CC BY-NC-SA and so cannot go into an MIT-licensed program.
It is rendered at size 11 with a two-pixel top offset, which is the largest
rendering whose every glyph fits the cell whole — a size larger and the tails
of `g`, `y`, `p`, `q`, `j` are cut off by the bottom of the cell.
`tests/test_font.py` checks that, so the font cannot regress silently.
`make font` regenerates it; `CELL_W`/`CELL_H` in
`platform/picocalc/picocalc.h` plus a regenerated font gives other geometries.

Everything the shell prints is written to fit 40 columns, and prose is wrapped
at word boundaries by `cos_wrap` rather than left to hard-wrap mid-word.
`tests/run_all.py` runs every command and fails if any line comes out wider
than the screen.

A picture stays up until someone has looked at it. Text arriving while a
frame is on screen waits for a key before the console takes the panel back,
so a `plot` from the prompt is not wiped by the prompt that follows it. A
program that calls `plot_hide` is saying it has finished with the picture
itself, and that does not wait — which is what lets an animation redraw
without stopping.

Rows are marked dirty and each one is rendered into a single 320×12 line
buffer and sent in one transfer. Graphics use a 4-bit-per-pixel overlay with a
sixteen-colour palette: 50 KB, against the 200 KB a full RGB framebuffer would
need on a 520 KB part.

---

## Building

```sh
make && make test          # host build and the full suite
./build-host/calcos        # the calculator, on your workstation
```

Firmware:

```sh
export PICO_SDK_PATH=~/pico/pico-sdk
mkdir build && cd build
cmake -DPICO_BOARD=pico2_w ..
make -j
```

`make font` regenerates the console font; `python3 tools/gen_reference.py`
regenerates the reference tables in this file from the source.

## Tests

| Suite | What it proves |
|---|---|
| `tests/test_encodings.py` | 74 Thumb-2 encodings, byte-for-byte against `arm-none-eabi-as` |
| `tests/test_cc.py` | 129 programs compiled and **executed** on an emulated Cortex-M33 |
| `tests/test_cc2.py` | 27 checks of 64-bit integers, function pointers, statics and `#include`, all executed |
| `tests/test_runtime.py` | 29 checks of the C runtime — graphics, keys, timing, sound, files, the flash store — executed |
| `tests/test_examples.py` | every shipped example compiles, and the ones that compute are run |
| `tests/run_calc_tests.py` | 128 numeric results against exact identities and mpmath |
| `tests/test_lineedit.py` | 32 checks of editing, history, completion and startup files |
| `tests/test_font.py` | every glyph fits its cell, descenders intact |
| `tests/test_shell.py` | the store driven from the prompt — named objects, and programs compiled into flash and loaded back in a fresh process |
| `tests/test_bignum.py` | big integers against Python's own, with 1 800 random cases shaped to make the division's correction steps fire |
| `tests/sbtest.c` | the scrollback ring across wraparound |
| `tests/screensim.c` | the real console, editor and drivers against a modelled panel, with the glyphs read back as text |
| `tests/lcdsim.c` | the real LCD driver against a model of the ILI9488 |
| `tests/kbdsim.c` | the real keyboard driver against a model of the controller |
| `tests/fattest.c` | FAT16, FAT32 and MBR images read back byte-exact |
| `tests/fatwrite.c` | writes survive `fsck.vfat`, existing files untouched |
| `tests/check_flash_map.py` | the flash store does not overlap the linked firmware |
| console width | every command's output fits 40 columns |
| settings round trip | saved, reloaded in a fresh process, bad values rejected |

The compiler tests compile real C, load the Thumb-2 into Unicorn with the
AEABI and runtime calls hooked in Python, run it, and check the answer. Bugs
caught only there included a frame-size patch that overwrote the wrong
instruction, array decay that loaded `a[0]` instead of taking an address, and
arguments not being converted to their declared parameter types. The 2.0 work
added more: negation of a 64-bit value that only negated the low word, and a
constant-expression evaluator that could not cope with `char grid[W * H]`
because it worked by pattern-matching the instruction the expression
compiled to.

The line editor is above the platform layer, so the host build runs the same
code the device runs and key sequences can simply be piped in; what comes
back on stdout is what the panel would show, backspaces and all.

`screensim` goes a layer further and asks what is actually on the glass. It
links the real console, the real LCD driver and the real keyboard driver
against a modelled panel whose every pixel starts as "never painted", then
boots, runs a program that draws over the whole screen, and types at the
prompt. It exists because 2.0 shipped with two faults that no other suite
could see: the console repaints whole character rows, and 26 rows of 12
pixels leave 8 pixels at the bottom of a 320-pixel panel that nothing
painted, so a band of the last graphics frame stayed on screen; and the line
editor echoed with `plat_putc`, which only marks a row dirty, so nothing
typed appeared until the line was entered. Both reproduce in the simulator
and both are checked there now.

It reads the panel back as text, by matching each 8×12 cell against the
font, which is what makes a *drawing* bug visible to a test. 2.0.1 shipped a
backspace that built a perfectly correct string and painted the wrong thing:
the repaint was told the cursor's new column instead of the one the screen
was still on, so it started a character late and left the deleted character
duplicated at the front of the line. Every assertion the old line-editor
suite made was about the resulting string, so all of them passed. The
twenty-two editing cases here compare the glyphs on the glass.

The driver tests exist for the same reason, after a version shipped with a
panel that drew one thin dotted line. The LCD driver is compiled against stub
pico-sdk headers and run against a model of the ILI9488 that enforces the
controller's own rules: chip select frames a transaction, and parameters
belong to the command that opened it. The driver that shipped fails nine of
those checks, starting with "pixel format reached the controller". Nothing in
the source looked wrong; the panel model is what made it obvious.

One honest limitation, left visible rather than papered over:
`integrate(@(x) 1/sqrt(1-x^2), -1, 1)` gives π to only about 8 digits. The
rule is fine; the coordinate is not. Near x = 1 the node is `1 − δ` with
δ ≈ 1e-32, which is not a representable double, so the integrand is evaluated
at exactly 1. Move the singularity to zero and full precision returns:

```
> integrate(@(x) 1/sqrt(1-x^2), -1, 1)
3.1415926319591363
> 2*integrate(@(t) 1/sqrt(t*(2-t)), 0, 1)
3.141592653589794
```

## Hardware

| Function | Bus | Pins |
|---|---|---|
| LCD, ILI9488/ST7365P, 320×320, 16-bit | SPI1 | SCK 10, MOSI 11, MISO 12, CS 13, DC 14, RST 15 |
| SD card | SPI0 | MISO 16, CS 17, SCK 18, MOSI 19 |
| Keyboard, STM32 at 0x1F | I²C1 @ 20 kHz | SDA 6, SCL 7 |

Backlights are driven by the keyboard controller over I²C, not by the RP2350.

## Credit

The pin map, the ILI9488 register sequence and the keyboard FIFO protocol were
confirmed against [benob/picocalc-pshell](https://github.com/benob/picocalc-pshell)
(GPLv2) and [calcterm](https://github.com/ctorkildson-netizen/calcterm) (MIT).
The backlight register mapping came from
[picocalc-studio](https://github.com/hexadevti/picocalc-studio), which
documents it explicitly and disagrees with pshell's comments — hence the
`blswap` setting. The 8×12 console geometry follows
[PicoMite MMBasic](https://github.com/UKTailwind/PicoMite). No code or font
data was copied from any of them; they were read to get the hardware right.

## License

MIT. See `LICENSE`.
