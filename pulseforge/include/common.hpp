/*
 * common.hpp - shared definitions, error handling, and exit codes.
 *
 * Responsibility: the single place for cross-cutting conventions:
 *   - the SystemError exception + CHECK() helper (replaces the C
 *     "goto cleanup" pattern)
 *   - the RAII Socket wrapper
 *   - project-wide exit codes
 *   - common headers
 *
 * Thread-safety: exceptions and the Socket class carry no shared state;
 * safe to use anywhere.
 */

#ifndef PULSEFORGE_COMMON_HPP
#define PULSEFORGE_COMMON_HPP

/* Safety net: keep POSIX APIs visible even without the -D flag. */
#define _POSIX_C_SOURCE 200809L

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

/* Exit codes shared by all PulseForge programs. */
enum ExitCode : int {
    EXIT_OK      = 0,
    EXIT_USAGE   = 2, /* bad command line */
    EXIT_SYSFAIL = 3, /* syscall / library failure */
    EXIT_CFG     = 4, /* invalid configuration */
};

/*
 * SystemError - thrown by CHECK() when a POSIX call returns failure.
 * Carries the failing errno so a top-level catch in main() can print
 * "what failed: strerror(errno)" and return EXIT_SYSFAIL.
 */
class SystemError : public std::runtime_error {
public:
    SystemError(const std::string &what, int error_number) : std::runtime_error(what), error_number_(error_number) {}
    int code() const noexcept { return error_number_; }

private:
    int error_number_;
};

/*
 * CHECK(call, what) - run a call that returns < 0 on failure.
 *
 * On failure it throws SystemError(what, errno). Resources are owned by
 * RAII objects, so when the exception propagates out of main(), stack
 * unwinding runs their destructors and every exit path cleans up
 * automatically - no goto, no manual cleanup label.
 */
#define CHECK(call, what)                                                  \
    do {                                                                   \
        if ((call) < 0) {                                                  \
            throw SystemError((what), errno);                              \
        }                                      z                           \
    } while (0)

/*
 * Socket - RAII wrapper around a POSIX file descriptor.
 *
 * Ownership: the Socket owns the fd; closing happens in the destructor
 * (and on move-assignment). Movable but not copyable, so a Socket can
 * be stored in containers or returned from helpers without leaks.
 */
class Socket {
public:
    /* May be a valid fd or -1 on failure; check with operator bool /
     * get() < 0 immediately after construction. */
    Socket(int domain, int type, int protocol)
        : fd_(::socket(domain, type, protocol)) {}

    ~Socket() { close(); }

    Socket(const Socket &) = delete;
    Socket &operator=(const Socket &) = delete;

    Socket(Socket &&other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
    Socket &operator=(Socket &&other) noexcept {
        if (this != &other) {
            close();
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }

    /* Underlying fd (may be -1 if construction failed / closed). */
    int get() const noexcept { return fd_; }

    explicit operator bool() const noexcept { return fd_ >= 0; }

    void close() noexcept {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

private:
    int fd_ = -1;
};

#endif /* PULSEFORGE_COMMON_HPP */
