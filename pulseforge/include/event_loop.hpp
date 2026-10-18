/*
 * event_loop.hpp - RAII wrappers for Linux's "everything is an fd" event
 * sources: epoll, timerfd, signalfd.
 *
 * Responsibility: the exchange and the trader are each ONE thread
 * running ONE loop that waits on many things at once - sockets, a
 * periodic timer, Ctrl-C. On Linux all of those can be file descriptors,
 * so one epoll_wait() covers them all:
 *
 *   epoll     - "tell me which of these fds are ready". O(ready fds) per
 *               wakeup, unlike select/poll which rescan every fd (O(n)).
 *   timerfd   - a timer you read() like a socket. Becomes readable when
 *               it fires; read() returns how many times it expired since
 *               the last read (so a late loop knows it missed ticks).
 *   signalfd  - signals delivered as readable data instead of an
 *               asynchronous handler interrupting your code at a random
 *               instruction. No async-signal-safety rules to worry about:
 *               SIGINT becomes just another event in the loop.
 *
 * Why single-threaded event loops? Most HFT gateways and matching
 * engines look like this: one hot thread pinned to one core, no locks,
 * no context switches, deterministic ordering of events. Parallelism
 * comes from running several such loops (one per symbol partition or
 * per connection group), not from threads sharing a book.
 *
 * Edge- vs level-triggered: we use level-triggered (the default): epoll
 * keeps reporting an fd while it has unread data. Edge-triggered
 * (EPOLLET) reports only transitions and REQUIRES draining the fd until
 * EAGAIN every time, or you will never hear about the rest. Our handlers
 * drain until EAGAIN anyway, so either would work.
 *
 * Each wrapper owns its fd through the project's Socket RAII class
 * (common.hpp): it is a generic "close this fd in the destructor" owner.
 */

#ifndef PULSEFORGE_EVENT_LOOP_HPP
#define PULSEFORGE_EVENT_LOOP_HPP

#include "common.hpp"

#include <csignal>
#include <cstdint>
#include <initializer_list>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <unistd.h>

class Epoll {
public:
    Epoll() : fd_(epoll_create1(EPOLL_CLOEXEC)) { CHECK(fd_.get(), "epoll_create1"); }

    /* `tag` comes back in epoll_event.data.u64 when the fd is ready.
     * We store a small id there, never a raw pointer: if the object is
     * destroyed while an event for it is already sitting in this batch,
     * an id lookup fails safely; a pointer would dangle. */
    void add(int fd, uint32_t events, uint64_t tag) {
        epoll_event ev{};
        ev.events   = events;
        ev.data.u64 = tag;
        CHECK(epoll_ctl(fd_.get(), EPOLL_CTL_ADD, fd, &ev), "epoll_ctl(ADD)");
    }

    void modify(int fd, uint32_t events, uint64_t tag) {
        epoll_event ev{};
        ev.events   = events;
        ev.data.u64 = tag;
        CHECK(epoll_ctl(fd_.get(), EPOLL_CTL_MOD, fd, &ev), "epoll_ctl(MOD)");
    }

    void remove(int fd) { (void)epoll_ctl(fd_.get(), EPOLL_CTL_DEL, fd, nullptr); }

    /* timeout_ms: -1 = block, 0 = poll (busy-spin mode). Returns the
     * number of ready events (0 on timeout or EINTR). */
    int wait(epoll_event *events, int max, int timeout_ms) {
        int n = epoll_wait(fd_.get(), events, max, timeout_ms);
        if (n < 0) {
            if (errno == EINTR) {
                return 0;
            }
            throw SystemError("epoll_wait", errno);
        }
        return n;
    }

private:
    Socket fd_;
};

class TimerFd {
public:
    explicit TimerFd(uint64_t period_ns)
        : fd_(timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC)) {
        CHECK(fd_.get(), "timerfd_create");
        itimerspec its{};
        its.it_interval.tv_sec  = static_cast<time_t>(period_ns / 1000000000ULL);
        its.it_interval.tv_nsec = static_cast<long>(period_ns % 1000000000ULL);
        its.it_value            = its.it_interval; /* first expiry after one period */
        CHECK(timerfd_settime(fd_.get(), 0, &its, nullptr), "timerfd_settime");
    }

    int get() const { return fd_.get(); }

    /* Number of expirations since the last call (0 if none). */
    uint64_t drain() {
        uint64_t expirations = 0;
        if (::read(fd_.get(), &expirations, sizeof(expirations)) != sizeof(expirations)) {
            return 0;
        }
        return expirations;
    }

private:
    Socket fd_;
};

class SignalFd {
public:
    /* Blocks the signals for normal delivery (they would otherwise still
     * run the default handler and kill the process) and routes them to
     * an fd instead. Must be constructed before any thread is started,
     * because threads inherit the signal mask. */
    SignalFd(std::initializer_list<int> signals) : fd_(-1) {
        sigset_t mask;
        sigemptyset(&mask);
        for (int s : signals) {
            sigaddset(&mask, s);
        }
        CHECK(sigprocmask(SIG_BLOCK, &mask, nullptr), "sigprocmask");
        fd_ = Socket(signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC));
        CHECK(fd_.get(), "signalfd");
    }

    int get() const { return fd_.get(); }

    /* Returns the signal number, or 0 if none pending. */
    int read_signal() {
        signalfd_siginfo info{};
        if (::read(fd_.get(), &info, sizeof(info)) != sizeof(info)) {
            return 0;
        }
        return static_cast<int>(info.ssi_signo);
    }

private:
    Socket fd_;
};

#endif /* PULSEFORGE_EVENT_LOOP_HPP */
