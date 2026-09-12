#include "StdioRpcOutput.h"

#include <algorithm>
#include <cstdio>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <io.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#endif

namespace Caesura {

StdioRpcOutput::StdioRpcOutput(int descriptor, std::function<void()> onFailure)
    : m_onFailure(std::move(onFailure)) {
    if (descriptor < 0) {
        m_failed.store(true);
        return;
    }
#if defined(_WIN32)
    m_descriptor = ::_dup(descriptor);
    if (m_descriptor >= 0) {
        if (!SetHandleInformation(reinterpret_cast<HANDLE>(::_get_osfhandle(m_descriptor)),
                                  HANDLE_FLAG_INHERIT, 0)) {
            ::_close(m_descriptor);
            m_descriptor = -1;
        }
    }
#else
    m_descriptor = ::dup(descriptor);
    if (m_descriptor >= 0) {
        const int flags = ::fcntl(m_descriptor, F_GETFL, 0);
        m_originalFlags = flags;
        if (flags < 0 || ::fcntl(m_descriptor, F_SETFD, FD_CLOEXEC) < 0
            || ::fcntl(m_descriptor, F_SETFL, flags | O_NONBLOCK) < 0) {
            ::close(m_descriptor);
            m_descriptor = -1;
        }
#if defined(__APPLE__)
        if (m_descriptor >= 0) {
            // Darwin raises pipe EPIPE as a process-directed SIGPIPE, so the
            // writer's thread mask alone cannot protect the Engine owner.
            // Suppress generation on this stream; leave process handlers alone.
            m_originalNoSigPipe = ::fcntl(m_descriptor, F_GETNOSIGPIPE);
            if (m_originalNoSigPipe < 0
                || ::fcntl(m_descriptor, F_SETNOSIGPIPE, 1) < 0) {
                ::fcntl(m_descriptor, F_SETFL, m_originalFlags);
                ::close(m_descriptor);
                m_descriptor = -1;
            }
        }
#endif
    }
#endif
    // Never invoke a callback into RpcServer while it is being constructed.
    m_failed.store(m_descriptor < 0);
}

StdioRpcOutput::~StdioRpcOutput() {
    finish();
    if (m_descriptor >= 0) {
#if defined(_WIN32)
        ::_close(m_descriptor);
#else
        // dup shares status flags with the supplied descriptor. Restore them
        // for callers that did not redirect stdout (e.g. in-process tests).
        if (m_originalFlags >= 0) ::fcntl(m_descriptor, F_SETFL, m_originalFlags);
#if defined(__APPLE__)
        // Like O_NONBLOCK, Darwin's flag belongs to the shared open file.
        if (m_originalNoSigPipe >= 0)
            ::fcntl(m_descriptor, F_SETNOSIGPIPE, m_originalNoSigPipe);
#endif
        ::close(m_descriptor);
#endif
    }
}

bool StdioRpcOutput::writeLine(const std::string& json) {
    bool rejected = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_closing || m_failed.load()) return false;
        // Include the in-flight line in both budgets. Do not evict a queued
        // response or continue after a partially written protocol line.
        rejected = m_descriptor < 0 || json.size() >= MaxPendingBytes
            || m_pendingLines >= MaxPendingLines
            || json.size() + 1 > MaxPendingBytes - m_pendingBytes;
        if (!rejected) {
            m_lines.push_back(json + '\n');
            m_pendingBytes += json.size() + 1;
            ++m_pendingLines;
            if (!m_writer.joinable()) m_writer = std::thread([this] { run(); });
        }
    }
    if (rejected) fail();
    m_changed.notify_all();
    return !rejected;
}

void StdioRpcOutput::fail() noexcept {
    m_cancelled.store(true);
    if (!m_failed.exchange(true)) {
        // No payload, script text, or authentication data in this diagnostic.
        fprintf(stderr, "[RpcOutput] output_unavailable: protocol stream closed or backpressured\n");
        try {
            if (m_onFailure) m_onFailure();
        } catch (...) {
            // A failed notification cannot abort this worker or prevent its
            // completion handshake. The transport is already marked failed.
        }
    }
    m_changed.notify_all();
}

void StdioRpcOutput::finish(std::chrono::milliseconds drain) {
    std::unique_lock<std::mutex> lock(m_mutex);
    m_closing = true;
    m_changed.notify_all();
    if (!m_writer.joinable()) return;
    if (!m_changed.wait_for(lock, drain, [this] { return m_done; })) {
        lock.unlock();
        fail();
        lock.lock();
    }
    // Cancellation is retried to cover the interval between the writer's
    // atomic cancellation check and entering synchronous WriteFile.
    while (!m_done) {
#if defined(_WIN32)
        if (m_cancelled.load()) CancelSynchronousIo(m_writer.native_handle());
#endif
        m_changed.wait_for(lock, std::chrono::milliseconds(2));
    }
    lock.unlock();
    m_writer.join();
}

void StdioRpcOutput::run() {
#if !defined(_WIN32)
    // EPIPE is a transport failure, not permission to terminate the Engine.
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &signals, nullptr);
#endif
    for (;;) {
        std::string bytes;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_changed.wait(lock, [this] {
                return m_closing || m_cancelled.load() || !m_lines.empty();
            });
            if (m_cancelled.load() || (m_closing && m_lines.empty())) break;
            bytes = std::move(m_lines.front());
            m_lines.pop_front();
        }
        const bool written = writeBytes(bytes);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_pendingBytes -= bytes.size();
            --m_pendingLines;
        }
        if (!written) {
            fail();
            break;
        }
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_lines.clear();
        m_pendingBytes = 0;
        m_pendingLines = 0;
        m_done = true;
    }
    m_changed.notify_all();
}

bool StdioRpcOutput::writeBytes(const std::string& bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size() && !m_cancelled.load()) {
#if defined(_WIN32)
        const auto handle = reinterpret_cast<HANDLE>(::_get_osfhandle(m_descriptor));
        DWORD written = 0;
        const auto count = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - offset, 65536));
        if (!WriteFile(handle, bytes.data() + offset, count, &written, nullptr)
            || written == 0) return false;
        offset += written;
#else
        const ssize_t written = ::write(m_descriptor, bytes.data() + offset, bytes.size() - offset);
        if (written > 0) {
            offset += static_cast<std::size_t>(written);
        } else if (written < 0 && errno == EINTR) {
            continue;
        } else if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            pollfd descriptor{m_descriptor, POLLOUT, 0};
            const int ready = ::poll(&descriptor, 1, 50);
            if ((ready < 0 && errno != EINTR)
                || (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL))) return false;
        } else {
            return false;
        }
#endif
    }
    return offset == bytes.size();
}

} // namespace Caesura
