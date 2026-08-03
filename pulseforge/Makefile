# Makefile - PulseForge: miniature low-latency market-data pipeline (learning lab).
#
# Phase 2 builds:
#   bin/publisher   - UDP tick publisher
#   bin/receiver    - UDP receiver + mutex/condvar queue + workers + shared stats
#   bin/monitor     - shared-memory dashboard (separate process)
# Plus unit tests (make test).
#
# Why -D_POSIX_C_SOURCE=200809L?
#   -std=c11 puts gcc in strict ISO mode, which hides POSIX APIs
#   (clock_nanosleep, sigaction, recvfrom, shm_open, ...). Re-enabling
#   the POSIX namespace is required for a POSIX-only project like this.

CC      ?= gcc
STD     := -std=c11
WARN    := -Wall -Wextra -Wpedantic -Werror
POSIX   := -D_POSIX_C_SOURCE=200809L
INC     := -Iinclude
PTHREAD := -pthread

CFLAGS_RELEASE := $(STD) $(WARN) $(POSIX) $(INC) $(PTHREAD) -O2 -g
CFLAGS_DEBUG   := $(STD) $(WARN) $(POSIX) $(INC) $(PTHREAD) -O0 -g3 -DDEBUG
CFLAGS_ASAN    := $(STD) $(WARN) $(POSIX) $(INC) $(PTHREAD) -O1 -g -fsanitize=address -fno-omit-frame-pointer
CFLAGS_TSAN    := $(STD) $(WARN) $(POSIX) $(INC) $(PTHREAD) -O1 -g -fsanitize=thread

LDFLAGS       := $(PTHREAD)
LDFLAGS_ASAN  := $(PTHREAD) -fsanitize=address
LDFLAGS_TSAN  := $(PTHREAD) -fsanitize=thread

# Default build flags (overridden by the debug/asan/tsan targets).
CFLAGS ?= $(CFLAGS_RELEASE)
LDFLAGS ?= $(LDFLAGS)

BIN := bin
SRC := src

# Per-program source sets (a program = its .c + every shared module it
# needs, compiled in one invocation for simplicity).
PUBLISHER_SRCS := $(SRC)/publisher.c $(SRC)/time_util.c
RECEIVER_SRCS  := $(SRC)/receiver.c $(SRC)/time_util.c $(SRC)/queue_mutex.c $(SRC)/cpu_util.c $(SRC)/worker.c
MONITOR_SRCS   := $(SRC)/monitor.c $(SRC)/time_util.c

PROGRAMS := $(BIN)/publisher $(BIN)/receiver $(BIN)/monitor

TEST_MESSAGE_SRCS := tests/test_message.c
TEST_QUEUE_SRCS   := tests/test_queue_mutex.c $(SRC)/queue_mutex.c
TESTS := $(BIN)/test_message $(BIN)/test_queue_mutex

.PHONY: all build build-tests debug asan tsan helgrind test clean

all: build
	@echo "Build complete: $(PROGRAMS)"

debug:
	$(MAKE) build build-tests CFLAGS="$(CFLAGS_DEBUG)" LDFLAGS="$(LDFLAGS)"

asan:
	$(MAKE) build build-tests CFLAGS="$(CFLAGS_ASAN)" LDFLAGS="$(LDFLAGS_ASAN)"

tsan:
	$(MAKE) build build-tests CFLAGS="$(CFLAGS_TSAN)" LDFLAGS="$(LDFLAGS_TSAN)"

build: $(PROGRAMS)

build-tests: $(TESTS)

test: build-tests
	@echo "== test_message =="
	@$(BIN)/test_message
	@echo "== test_queue_mutex =="
	@$(BIN)/test_queue_mutex
	@echo "All tests passed."

$(BIN)/publisher: $(PUBLISHER_SRCS) include/message.h include/time_util.h include/common.h | $(BIN)
	$(CC) $(CFLAGS) $(PUBLISHER_SRCS) -o $@ $(LDFLAGS)

$(BIN)/receiver: $(RECEIVER_SRCS) include/message.h include/time_util.h include/queue_mutex.h include/worker.h include/stats.h include/shared_stats.h include/cpu_util.h include/common.h | $(BIN)
	$(CC) $(CFLAGS) $(RECEIVER_SRCS) -o $@ $(LDFLAGS)

$(BIN)/monitor: $(MONITOR_SRCS) include/stats.h include/shared_stats.h include/time_util.h include/common.h | $(BIN)
	$(CC) $(CFLAGS) $(MONITOR_SRCS) -o $@ $(LDFLAGS)

$(BIN)/test_message: $(TEST_MESSAGE_SRCS) include/message.h | $(BIN)
	$(CC) $(CFLAGS) $(TEST_MESSAGE_SRCS) -o $@ $(LDFLAGS)

$(BIN)/test_queue_mutex: $(TEST_QUEUE_SRCS) include/queue_mutex.h include/stats.h include/common.h | $(BIN)
	$(CC) $(CFLAGS) $(TEST_QUEUE_SRCS) -o $@ $(LDFLAGS)

$(BIN):
	mkdir -p $(BIN)

helgrind:
	@echo "helgrind target is wired in Phase 3 (valgrind --tool=helgrind)."

clean:
	rm -rf $(BIN)
