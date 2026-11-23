# Examples

Copy these to the SD card and compile them on the device. Every one of them
is compiled by the test suite, and the ones that only compute are run, so
they match the compiler they ship with.

| File | What it shows |
|---|---|
| `hello.c` | numeric integration; `simpson(0, 2, 1000)` is callable from the prompt |
| `stats.c` | structs, arrays and statics; `variance(64)` from the prompt |
| `bigint.c` | 64-bit integers, function pointers, a table of them, a static local |
| `mandel.c` | graphics: the Mandelbrot set in sixteen colours, with a timer |
| `life.c` | an interactive loop: draw, `delay`, and `kbhit` to stop |
| `tune.c` | `tone` on the buzzer |
| `notes.c` | file I/O: data that outlives the program |
| `store.c` | the flash store: a table that costs no RAM to read |
| `numlib.c` | a library for flash: `ccs numlib /numlib.c`, then `use numlib` |

```
cc /hello.c
run
simpson(0, 3, 10000)
```

A program's own memory is gone when it ends. To keep anything, write it to
a file (`notes.c`) or to the flash store (`store.c`).

To run something at every boot, put it on the card as `/autoexec.c`, or put
calculator lines in `/startup.txt`. Hold escape while powering on to skip
both.

To keep a program when there is no card at all, compile it into flash:

```
ccs numlib /numlib.c      compile and keep it
use numlib                load it in any later session
nroot(2, 5)               call what it defines
```

Name it `boot` instead and it loads at every startup by itself.
