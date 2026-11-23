# Host build: develops and tests everything except the board drivers.
# Firmware is CMake + pico-sdk (see CMakeLists.txt).

CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Icc -Isrc -Iplatform -Iplatform/picocalc
LDLIBS  ?= -lm

# The device's static buffers sit at fixed addresses, and a program saved to
# flash holds addresses into them. Linking the host build non-PIE gives it
# the same property, which is what lets 'ccs' and 'use' be tested here.
CFLAGS  += -no-pie
LDLIBS  += -no-pie
B        = build-host

CORE = src/num.c src/value.c src/parse.c src/eval.c src/builtins.c src/settings.c \
       src/shell.c src/runtime.c src/lineedit.c src/units.c src/store.c src/bignum.c
CCSRC = cc/emit.c cc/lex.c cc/gen.c

all: $(B)/calcos $(B)/ccrun $(B)/encgen $(B)/fattest $(B)/fatwrite \
     $(B)/lcdsim $(B)/kbdsim $(B)/screensim $(B)/sbtest $(B)/bigtest $(B)/sbtest

$(B):
	mkdir -p $(B)

HDRS = src/calc.h src/settings.h src/cprog.h cc/cc.h cc/emit.h

$(B)/calcos: $(CORE) $(CCSRC) platform/posix/plat_posix.c platform/gfx_shapes.c platform/font.c $(HDRS) | $(B)
	$(CC) $(CFLAGS) -o $@ $(filter %.c,$^) $(LDLIBS)

$(B)/ccrun: tools/ccrun.c $(CCSRC) $(CORE) platform/posix/plat_posix.c platform/gfx_shapes.c platform/font.c | $(B)
	$(CC) $(CFLAGS) -DPLAT_NO_MAIN -o $@ $^ $(LDLIBS)

$(B)/encgen: tests/encgen.c cc/emit.c | $(B)
	$(CC) $(CFLAGS) -o $@ $^

$(B)/fattest: tests/fattest.c platform/picocalc/sdfat.c | $(B)
	$(CC) $(CFLAGS) -DFAT_HOST_TEST -o $@ $^

$(B)/fatwrite: tests/fatwrite.c platform/picocalc/sdfat.c | $(B)
	$(CC) $(CFLAGS) -DFAT_HOST_TEST -o $@ $^

# The board drivers compiled against stub pico-sdk headers and run against a
# model of the panel and the keyboard controller.
SIMFLAGS = -std=c11 -O1 -Wall -Itests/fakepico -Iplatform/picocalc -Isrc

$(B)/lcdsim: tests/lcdsim.c platform/picocalc/lcd.c platform/font.c | $(B)
	$(CC) $(SIMFLAGS) -o $@ $^

$(B)/screensim: tests/screensim.c platform/picocalc/plat_picocalc.c \
		platform/picocalc/lcd.c platform/picocalc/keyboard.c \
		platform/scrollback.c platform/gfx_shapes.c platform/font.c \
		$(CORE) $(CCSRC) | $(B)
	$(CC) $(CFLAGS) -Itests/fakepico -Iplatform/picocalc -DPLAT_NO_MAIN \
		-o $@ $^ $(LDLIBS)

$(B)/bigtest: tests/bigtest.c src/bignum.c | $(B)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(B)/sbtest: tests/sbtest.c platform/scrollback.c | $(B)
	$(CC) $(CFLAGS) -Iplatform -o $@ $^

$(B)/kbdsim: tests/kbdsim.c platform/picocalc/keyboard.c | $(B)
	$(CC) $(SIMFLAGS) -o $@ $^

test: all
	@python3 tests/run_all.py

font:
	python3 tools/make_font.py > platform/font.c

readme:
	python3 tools/make_readme.py

clean:
	rm -rf $(B)

.PHONY: all test font readme clean
