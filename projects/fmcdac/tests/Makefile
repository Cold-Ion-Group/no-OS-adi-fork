# Host-side unit tests for awg_sched validation layer.
#
# Usage:
#   make -C projects/fmcdac/tests
#   make -C projects/fmcdac/tests run
#   make -C projects/fmcdac/tests clean

CC      := gcc
CFLAGS  := -std=c11 -Wall -Wextra -Wpedantic \
           -I../src/app \
           -I. \
           -DAWG_SCHED_ENABLE_LOAD_READBACK_VERIFY=0

SRC_DIR := ../src/app
TEST_BIN := awg_sched_validate_test

SRCS := awg_sched_validate_test.c \
        $(SRC_DIR)/awg_sched.c

all: $(TEST_BIN)

$(TEST_BIN): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^

run: $(TEST_BIN)
	./$(TEST_BIN)

clean:
	rm -f $(TEST_BIN)

.PHONY: all run clean
