/* Ожидание по абсолютному MONOTONIC deadline, отдельный объект на каждого waiter.
 * notify не захватывает mutex и допустим под mutex порта. ticket закрывает гонку до wait.
 * Windows: event + high-resolution timer; POSIX: неблокирующий pipe + pselect.
 * Ошибки исполнения забирает reporter через takeError(), I/O логов в таймере нет. */
#pragma once
#include "../../../src/call/call_video_playout.h"
#include "debug_config.h"
#include <atomic>
#include <algorithm>
#include <cerrno>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "../../../lib/platform_compat.h"
#ifndef _WIN32
#include <sys/select.h>
#endif

class CallVideoWake {
    std::atomic<uint64_t> serial{0};
    std::atomic<unsigned> error{0};
    bool timerRequired;
#ifdef _WIN32
    HANDLE event = nullptr, timer = nullptr;
#else
    int pipefd[2] = {-1, -1};
#endif
public:
    explicit CallVideoWake(bool withTimer = true) : timerRequired(withTimer) {
#ifdef _WIN32
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        constexpr DWORD highResolution = 0x00000002; // CREATE_WAITABLE_TIMER_HIGH_RESOLUTION (Windows 10 1803+).
        if (timerRequired) timer = CreateWaitableTimerExW(nullptr, nullptr, highResolution, TIMER_MODIFY_STATE | SYNCHRONIZE);
        if (timerRequired && !timer) {
            DEBUG_WARN(DEBUG_CATEGORY_CALL, "video high resolution timer unavailable error=%lu; using regular timer", GetLastError());
            timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
        }
        if (!event || (timerRequired && !timer)) DEBUG_ERROR(DEBUG_CATEGORY_CALL, "video wait handles unavailable event=%p timer=%p error=%lu",
            event, timer, GetLastError());
        else DEBUG_INFO(DEBUG_CATEGORY_CALL, "video wake ready event=%p timer=%p timed=%d", event, timer, timerRequired);
#else
        if (pipe(pipefd) || fcntl(pipefd[0], F_SETFL, O_NONBLOCK) < 0 || fcntl(pipefd[1], F_SETFL, O_NONBLOCK) < 0
            || fcntl(pipefd[0], F_SETFD, FD_CLOEXEC) < 0 || fcntl(pipefd[1], F_SETFD, FD_CLOEXEC) < 0) {
            DEBUG_ERROR(DEBUG_CATEGORY_CALL, "video wake pipe setup failed errno=%d", errno);
            for (int& fd : pipefd) { if (fd >= 0) close(fd); fd = -1; }
        }
#endif
    }
    ~CallVideoWake() {
#ifdef _WIN32
        if (timer) CloseHandle(timer);
        if (event) CloseHandle(event);
#else
        for (int fd : pipefd) if (fd >= 0) close(fd);
#endif
    }
    bool valid() const {
#ifdef _WIN32
        return event && (!timerRequired || timer);
#else
        return pipefd[0] >= 0 && pipefd[0] < FD_SETSIZE;
#endif
    }
    unsigned takeError() { return error.exchange(0); }
#ifdef _WIN32
    HANDLE nativeHandle() const { return event; }
#else
    int nativeHandle() const { return pipefd[0]; }
#endif
    // Только GUI notifier вместо wait(); чтение ограничено одним неблокирующим вызовом.
    void consume() {
#ifndef _WIN32
        char bytes[128];
        if (read(pipefd[0], bytes, sizeof(bytes)) < 0 && errno != EAGAIN) error.store(errno);
#endif
    }
    uint64_t ticket() const { return serial.load(std::memory_order_acquire); }
    void notify() {
        serial.fetch_add(1, std::memory_order_release);
#ifdef _WIN32
        if (event && !SetEvent(event)) error.store(GetLastError());
#else
        if (pipefd[1] >= 0) {
            const char byte = 0;
            if (write(pipefd[1], &byte, 1) < 0 && errno != EAGAIN) error.store(errno);
        }
#endif
    }
    void wait(int64_t deadline, uint64_t expected) {
        const int64_t remaining = deadline - call_video_playout_now_us();
        if (remaining <= 0 || ticket() != expected) return;
#ifdef _WIN32
        if (event && timer) {
            LARGE_INTEGER due; due.QuadPart = -remaining * 10;
            if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
                HANDLE handles[] = {event, timer};
                DWORD result = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
                if (result == WAIT_OBJECT_0 || result == WAIT_OBJECT_0 + 1) return;
            }
            error.store(GetLastError());
        }
#else
        if (!valid()) return;
        fd_set fds; FD_ZERO(&fds); FD_SET(pipefd[0], &fds);
        timespec timeout{remaining / 1000000, (remaining % 1000000) * 1000};
        int result = pselect(pipefd[0] + 1, &fds, nullptr, nullptr, &timeout, nullptr);
        if (result > 0) {
            char bytes[128];
            if (read(pipefd[0], bytes, sizeof(bytes)) < 0 && errno != EAGAIN) error.store(errno);
        } else if (result < 0 && errno != EINTR) error.store(errno);
#endif
    }
};
