/*
 * deadlock_demo.cpp - a deliberate, reproducible deadlock lab.
 *
 * Responsibility: give the debugging-lab part of this project something
 * real to practice on. Two threads lock the SAME two mutexes in
 * OPPOSITE order (classic lock-order inversion):
 *   thread A: lock(mu_a) -> sleep -> lock(mu_b)
 *   thread B: lock(mu_b) -> sleep -> lock(mu_a)
 * The sleep is there only to make the unlucky interleaving reliable
 * instead of a once-in-a-million race - with it, thread A holds mu_a
 * and wants mu_b while thread B holds mu_b and wants mu_a: neither can
 * proceed, forever.
 *
 * How to use this lab (see docs/debugging_labs.md for the full walk-
 * through):
 *   ./bin/deadlock_demo &
 *   gdb -p $!                      # attach to the hung process
 *     (gdb) info threads           # both threads BLOCKED in pthread_mutex_lock
 *     (gdb) thread 2
 *     (gdb) bt                     # see it waiting on mu_b while holding mu_a
 *   Or: timeout 5 helgrind ./bin/deadlock_demo   (helgrind flags the
 *   lock-order inversion directly, before it even hangs)
 *
 * The fix (not implemented here on purpose - that is the exercise) is
 * to always acquire shared mutexes in a single globally-agreed order,
 * or to use std::lock(mu_a, mu_b) which acquires both atomically
 * without a hand-rolled ordering convention.
 */

#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>
#include <unistd.h>

namespace {
std::mutex mu_a;
std::mutex mu_b;

void thread_a() {
    std::printf("thread A: locking mu_a\n");
    std::lock_guard<std::mutex> lock_a(mu_a);
    std::printf("thread A: holds mu_a, sleeping before locking mu_b\n");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    std::printf("thread A: locking mu_b (will block: B holds it)\n");
    std::lock_guard<std::mutex> lock_b(mu_b);
    std::printf("thread A: holds both (unreachable in this demo)\n");
}

void thread_b() {
    std::printf("thread B: locking mu_b\n");
    std::lock_guard<std::mutex> lock_b(mu_b);
    std::printf("thread B: holds mu_b, sleeping before locking mu_a\n");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    std::printf("thread B: locking mu_a (will block: A holds it)\n");
    std::lock_guard<std::mutex> lock_a(mu_a);
    std::printf("thread B: holds both (unreachable in this demo)\n");
}
} // namespace

int main() {
    std::printf("deadlock_demo: two threads will lock mu_a/mu_b in opposite "
                "order and hang forever.\n"
                "Attach a debugger now (pid %d), or run under helgrind.\n",
                static_cast<int>(getpid()));

    std::thread a(thread_a);
    std::thread b(thread_b);
    a.join();
    b.join();

    std::printf("deadlock_demo: unreachable - if you see this, the "
                "interleaving did not line up; rerun.\n");
    return 0;
}
