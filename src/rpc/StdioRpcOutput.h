#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace Caesura {

// Owns a duplicate of the protocol descriptor. Producers never perform pipe
// I/O; a stalled consumer cannot hold the Engine owner inside pushLog().
class StdioRpcOutput final {
public:
    explicit StdioRpcOutput(int descriptor, std::function<void()> onFailure = {});
    ~StdioRpcOutput();
    StdioRpcOutput(const StdioRpcOutput&) = delete;
    StdioRpcOutput& operator=(const StdioRpcOutput&) = delete;

    bool writeLine(const std::string& json);
    // Called after the last response has been submitted. Healthy consumers
    // receive the final stop reply; stalled consumers get a bounded drain.
    void finish(std::chrono::milliseconds drain = std::chrono::milliseconds(1000));
    bool failed() const noexcept { return m_failed.load(); }
    bool ready() const noexcept { return m_descriptor >= 0 && !failed(); }

    static constexpr std::size_t MaxPendingBytes = 32u * 1024u * 1024u;
    static constexpr std::size_t MaxPendingLines = 256;

private:
    void run();
    bool writeBytes(const std::string& bytes);
    void fail() noexcept;

    int m_descriptor = -1;
#if !defined(_WIN32)
    int m_originalFlags = -1;
#endif
    std::function<void()> m_onFailure;
    std::mutex m_mutex;
    std::condition_variable m_changed;
    std::deque<std::string> m_lines;
    std::size_t m_pendingBytes = 0;
    std::size_t m_pendingLines = 0;
    bool m_closing = false;
    bool m_done = false;
    std::atomic<bool> m_cancelled{false};
    std::atomic<bool> m_failed{false};
    std::thread m_writer;
};

} // namespace Caesura
