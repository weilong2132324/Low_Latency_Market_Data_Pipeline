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
QUEUE_BACKEND_SRCS := $(SRC)/queue_mutex.cpp $(SRC)/spsc_ring.cpp $(SRC)/mpmc_fanout.cpp $(SRC)/queue_backend.cpp

PUBLISHER_SRCS := $(SRC)/publisher.cpp $(SRC)/time_util.cpp $(SRC)/snapshot_server.cpp
RECEIVER_SRCS  := $(SRC)/receiver.cpp $(SRC)/time_util.cpp $(QUEUE_BACKEND_SRCS) $(SRC)/cpu_util.cpp $(SRC)/worker.cpp $(SRC)/bpf_filter.cpp $(SRC)/snapshot_client.cpp
MONITOR_SRCS   := $(SRC)/monitor.cpp $(SRC)/time_util.cpp
DEADLOCK_SRCS  := $(SRC)/deadlock_demo.cpp
RACE_SRCS      := $(SRC)/race_demo.cpp
TELEMETRY_SRCS := $(SRC)/telemetry_bridge.cpp $(SRC)/ws_server.cpp

PROGRAMS := $(BIN)/publisher $(BIN)/receiver $(BIN)/monitor $(BIN)/deadlock_demo $(BIN)/race_demo $(BIN)/telemetry_bridge

TEST_MESSAGE_SRCS           := tests/test_message.cpp
TEST_QUEUE_SRCS             := tests/test_queue_mutex.cpp $(SRC)/queue_mutex.cpp
TEST_SPSC_RING_SRCS         := tests/test_spsc_ring.cpp $(SRC)/spsc_ring.cpp
TEST_MPMC_FANOUT_SRCS       := tests/test_mpmc_fanout.cpp $(SRC)/mpmc_fanout.cpp
TEST_MEM_POOL_SRCS          := tests/test_mem_pool.cpp
TEST_LATENCY_HISTOGRAM_SRCS := tests/test_latency_histogram.cpp
TEST_HUGEPAGE_ALLOC_SRCS    := tests/test_hugepage_alloc.cpp
TESTS := $(BIN)/test_message $(BIN)/test_queue_mutex $(BIN)/test_spsc_ring $(BIN)/test_mpmc_fanout $(BIN)/test_mem_pool $(BIN)/test_latency_histogram $(BIN)/test_hugepage_alloc

BENCH_SRCS := bench/queue_bench.cpp $(SRC)/time_util.cpp $(QUEUE_BACKEND_SRCS)
BENCH_BIN  := $(BIN)/queue_bench

.PHONY: all build build-tests debug asan tsan helgrind test bench clean

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
	@echo "== test_spsc_ring =="
	@$(BIN)/test_spsc_ring
	@echo "== test_mpmc_fanout =="
	@$(BIN)/test_mpmc_fanout
	@echo "== test_mem_pool =="
	@$(BIN)/test_mem_pool
	@echo "== test_latency_histogram =="
	@$(BIN)/test_latency_histogram
	@echo "== test_hugepage_alloc =="
	@$(BIN)/test_hugepage_alloc
	@echo "All tests passed."

bench: $(BENCH_BIN)
	$(BENCH_BIN)

$(BIN)/publisher: $(PUBLISHER_SRCS) include/message.hpp include/time_util.hpp include/common.hpp | $(BIN)
	$(CXX) $(CXXFLAGS) $(PUBLISHER_SRCS) -o $@ $(LDFLAGS)

$(BIN)/receiver: $(RECEIVER_SRCS) include/message.hpp include/time_util.hpp include/queue_backend.hpp include/queue_mutex.hpp include/spsc_ring.hpp include/mpmc_fanout.hpp include/worker.hpp include/stats.hpp include/shared_stats.hpp include/cpu_util.hpp include/common.hpp | $(BIN)
	$(CXX) $(CXXFLAGS) $(RECEIVER_SRCS) -o $@ $(LDFLAGS)

$(BIN)/monitor: $(MONITOR_SRCS) include/stats.hpp include/shared_stats.hpp include/time_util.hpp include/common.hpp | $(BIN)
	$(CXX) $(CXXFLAGS) $(MONITOR_SRCS) -o $@ $(LDFLAGS)

$(BIN)/deadlock_demo: $(DEADLOCK_SRCS) | $(BIN)
	$(CXX) $(CXXFLAGS) $(DEADLOCK_SRCS) -o $@ $(LDFLAGS)

$(BIN)/race_demo: $(RACE_SRCS) | $(BIN)
	$(CXX) $(CXXFLAGS) $(RACE_SRCS) -o $@ $(LDFLAGS)

$(BIN)/telemetry_bridge: $(TELEMETRY_SRCS) include/ws_server.hpp include/sha1.hpp include/shared_events.hpp include/shared_stats.hpp | $(BIN)
	$(CXX) $(CXXFLAGS) $(TELEMETRY_SRCS) -o $@ $(LDFLAGS)

$(BENCH_BIN): $(BENCH_SRCS) include/latency_histogram.hpp include/queue_backend.hpp include/message.hpp include/time_util.hpp | $(BIN)
	$(CXX) $(CXXFLAGS) $(BENCH_SRCS) -o $@ $(LDFLAGS)

$(BIN)/test_message: $(TEST_MESSAGE_SRCS) include/message.hpp | $(BIN)
	$(CXX) $(CXXFLAGS) $(TEST_MESSAGE_SRCS) -o $@ $(LDFLAGS)

$(BIN)/test_queue_mutex: $(TEST_QUEUE_SRCS) include/queue_mutex.hpp include/stats.hpp include/common.hpp | $(BIN)
	$(CXX) $(CXXFLAGS) $(TEST_QUEUE_SRCS) -o $@ $(LDFLAGS)

$(BIN)/test_spsc_ring: $(TEST_SPSC_RING_SRCS) include/spsc_ring.hpp include/message.hpp | $(BIN)
	$(CXX) $(CXXFLAGS) $(TEST_SPSC_RING_SRCS) -o $@ $(LDFLAGS)

$(BIN)/test_mpmc_fanout: $(TEST_MPMC_FANOUT_SRCS) include/mpmc_fanout.hpp include/message.hpp | $(BIN)
	$(CXX) $(CXXFLAGS) $(TEST_MPMC_FANOUT_SRCS) -o $@ $(LDFLAGS)

$(BIN)/test_mem_pool: $(TEST_MEM_POOL_SRCS) include/mem_pool.hpp | $(BIN)
	$(CXX) $(CXXFLAGS) $(TEST_MEM_POOL_SRCS) -o $@ $(LDFLAGS)

$(BIN)/test_latency_histogram: $(TEST_LATENCY_HISTOGRAM_SRCS) include/latency_histogram.hpp | $(BIN)
	$(CXX) $(CXXFLAGS) $(TEST_LATENCY_HISTOGRAM_SRCS) -o $@ $(LDFLAGS)

$(BIN)/test_hugepage_alloc: $(TEST_HUGEPAGE_ALLOC_SRCS) include/hugepage_alloc.hpp | $(BIN)
	$(CXX) $(CXXFLAGS) $(TEST_HUGEPAGE_ALLOC_SRCS) -o $@ $(LDFLAGS)

$(BIN):
	mkdir -p $(BIN)

helgrind:
	@echo "Run: valgrind --tool=helgrind ./bin/deadlock_demo"
	@echo "Or:  valgrind --tool=helgrind ./bin/race_demo"
	@echo "See docs/debugging_labs.md for the full walkthrough."

clean:
	rm -rf $(BIN)
