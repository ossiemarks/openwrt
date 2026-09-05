# Native test harness for the CSI analysis code.
#
#   make            build both harnesses
#   make vitals     run the full vitals sweep
#   make quick      run the reduced vitals sweep (fast iteration)
#   make sanity     one verbose vitals trial
#   make motion     run the presence/position/gesture sweep
#   make baseline   write the full vitals sweep to baseline.txt
#   make clean
#
# The real analysis sources are compiled directly; only g_state, g_config and
# the mqtt entry points are stubbed.

CC      ?= cc
CFLAGS  ?= -Wall -Wextra -O2 -std=c11
LDLIBS  := -lm -lpthread

SRCDIR  := ../src
ANALYSIS := $(SRCDIR)/analysis

COMMON_OBJS := synth.o stubs.o

VITALS_OBJS := $(COMMON_OBJS) vitals.o test_vitals.o
MOTION_OBJS := $(COMMON_OBJS) presence.o position.o gesture.o test_motion.o

BINS := test_vitals test_motion

.PHONY: all vitals quick sanity motion baseline clean

all: $(BINS)

test_vitals: $(VITALS_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

test_motion: $(MOTION_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

vitals.o: $(ANALYSIS)/vitals.c
	$(CC) $(CFLAGS) -c -o $@ $<

presence.o: $(ANALYSIS)/presence.c
	$(CC) $(CFLAGS) -c -o $@ $<

position.o: $(ANALYSIS)/position.c
	$(CC) $(CFLAGS) -c -o $@ $<

gesture.o: $(ANALYSIS)/gesture.c
	$(CC) $(CFLAGS) -c -o $@ $<

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

synth.o: synth.h
stubs.o: stubs.h
test_vitals.o: synth.h stubs.h
test_motion.o: synth.h stubs.h

vitals: test_vitals
	./test_vitals

quick: test_vitals
	./test_vitals --quick

sanity: test_vitals
	./test_vitals --sanity

motion: test_motion
	./test_motion

baseline: test_vitals test_motion
	{ ./test_vitals; echo; ./test_motion; } > baseline.txt
	@echo "wrote baseline.txt"

final: test_vitals test_motion
	{ ./test_vitals; echo; ./test_motion; } > final.txt
	@echo "wrote final.txt"

compare:
	@echo "--- baseline ---"; grep '^SUMMARY' baseline.txt
	@echo "--- final ---";    grep '^SUMMARY' final.txt

calibrate: test_motion
	./test_motion --calibrate

clean:
	rm -f $(BINS) *.o
