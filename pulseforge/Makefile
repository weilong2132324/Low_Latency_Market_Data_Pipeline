# Makefile - PulseForge: miniature low-latency market-data pipeline (learning lab).
#
# Phase 1 builds:
#   bin/publisher   - UDP tick publisher
#   bin/receiver    - UDP receiver + mutex/condvar queue + workers + shared stats
#   bin/monitor     - shared-memory dashboard (separate process)
# Plus unit tests (make test).
#
# Language: C++17 (g++). The lab is now C++ throughout:
#   - std::atomic instead of C11 _Atomic
#   - std::thread / std::mutex / std::condition_variable instead of pthreads
#   - RAII (Socket, SharedStatsRegion, std::vector) instead of the
#     manual "goto cleanup" pattern
#
# Why -D_POSIX_C_SOURCE=200809L?
#   -std=c++17 puts g++ in strict ISO mode, which hides POSIX APIs
#   (clock_nanosleep, sigaction, recvfrom, shm_open, ...). Re-enabling
#   the POSIX namespace is required for a POSIX-only project like this.
#   (g++ also predefines _GNU_SOURCE for libstdc++, which implies these.)

CXX     ?= g++
STD     := -std=c++17
WARN    := -Wall -Wextra -Wpedantic -Werror
POSIX   := -D_POSIX_C_SOURCE=200809L
INC     := -Iinclude
PTHREAD := -pthread

CXXFLAGS_RELEASE := $(STD) $(WARN) $(POSIX) $(INC) $(PTHREAD) -O2 -g
CXXFLAGS_DEBUG   := $(STD) $(WARN) $(POSIX) $(INC) $(PTHREAD) -O0 -g3 -DDEBUG
CXXFLAGS_ASAN    := $(STD) $(WARN) $(POSIX) $(INC) $(PTHREAD) -O1 -g -fsanitize=address -fno-omit-frame-pointer
CXXFLAGS_TSAN    := $(STD) $(WARN) $(POSIX) $(INC) $(PTHREAD) -O1 -g -fsanitize=thread

LDFLAGS_BASE := $(PTHREAD)
LDFLAGS_ASAN := $(PTHREAD) -fsanitize=address
LDFLAGS_TSAN := $(PTHREAD) -fsanitize=thread

# Default build flags (overridden by the debug/asan/tsan targets).
CXXFLAGS ?= $(CXXFLAGS_RELEASE)
LDFLAGS  ?= $(LDFLAGS_BASE)

BIN := bin
SRC := src

# Per-program source sets (a program = its .cpp + every shared module it
# needs, compiled in one invocation for simplicity).
PUBLISHER_SRCS := $(SRC)/publisher.cpp $(SRC)/time_util.cpp
RECEIVER_SRCS  := $(SRC)/receiver.cpp $(SRC)/time_util.cpp $(SRC)/queue_mutex.cpp $(SRC)/cpu_util.cpp $(SRC)/worker.cpp
MONITOR_SRCS   := $(SRC)/monitor.cpp $(SRC)/time_util.cpp

PROGRAMS := $(BIN)/publisher $(BIN)/receiver $(BIN)/monitor

TEST_MESSAGE_SRCS := tests/test_message.cpp
TEST_QUEUE_SRCS   := tests/test_queue_mutex.cpp $(SRC)/queue_mutex.cpp
TESTS := $(BIN)/test_message $(BIN)/test_queue_mutex

.PHONY: all build build-tests debug asan tsan helgrind test clean

all: build
	@echo "Build complete: $(PROGRAMS)"

debug:
	$(MAKE) build build-tests CXXFLAGS="$(CXXFLAGS_DEBUG)" LDFLAGS="$(LDFLAGS_BASE)"

asan:
	$(MAKE) build build-tests CXXFLAGS="$(CXXFLAGS_ASAN)" LDFLAGS="$(LDFLAGS_ASAN)"

tsan:
	$(MAKE) build build-tests CXXFLAGS="$(CXXFLAGS_TSAN)" LDFLAGS="$(LDFLAGS_TSAN)"

build: $(PROGRAMS)

build-tests: $(TESTS)

test: build-tests
	@echo "== test_message =="
	@$(BIN)/test_message
	@echo "== test_queue_mutex =="
	@$(BIN)/test_queue_mutex
	@echo "All tests passed."

$(BIN)/publisher: $(PUBLISHER_SRCS) include/message.hpp include/time_util.hpp include/common.hpp | $(BIN)
	$(CXX) $(CXXFLAGS) $(PUBLISHER_SRCS) -o $@ $(LDFLAGS)

$(BIN)/receiver: $(RECEIVER_SRCS) include/message.hpp include/time_util.hpp include/queue_mutex.hpp include/worker.hpp include/stats.hpp include/shared_stats.hpp include/cpu_util.hpp include/common.hpp | $(BIN)
	$(CXX) $(CXXFLAGS) $(RECEIVER_SRCS) -o $@ $(LDFLAGS)

$(BIN)/monitor: $(MONITOR_SRCS) include/stats.hpp include/shared_stats.hpp include/time_util.hpp include/common.hpp | $(BIN)
	$(CXX) $(CXXFLAGS) $(MONITOR_SRCS) -o $@ $(LDFLAGS)

$(BIN)/test_message: $(TEST_MESSAGE_SRCS) include/message.hpp | $(BIN)
	$(CXX) $(CXXFLAGS) $(TEST_MESSAGE_SRCS) -o $@ $(LDFLAGS)

$(BIN)/test_queue_mutex: $(TEST_QUEUE_SRCS) include/queue_mutex.hpp include/stats.hpp include/common.hpp | $(BIN)
	$(CXX) $(CXXFLAGS) $(TEST_QUEUE_SRCS) -o $@ $(LDFLAGS)

$(BIN):
	mkdir -p $(BIN)

helgrind:
	@echo "helgrind target is wired in Phase 3 (valgrind --tool=helgrind)."

clean:
	rm -rf $(BIN)
