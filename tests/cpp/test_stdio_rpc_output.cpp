#include "doctest.h"
#include "rpc/StdioRpcOutput.h"

#include <atomic>
#include <chrono>
#include <string>
#include <stdexcept>
#include <thread>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {
class OutputPipe {
public:
    OutputPipe() {
#if defined(_WIN32)
        if (::_pipe(m_descriptors, 4096, _O_BINARY | _O_NOINHERIT) != 0)
#else
        if (::pipe(m_descriptors) != 0)
#endif
            m_descriptors[0] = m_descriptors[1] = -1;
    }
    ~OutputPipe() { closeRead(); closeWrite(); }
    bool valid() const { return m_descriptors[0] >= 0 && m_descriptors[1] >= 0; }
    int writer() const { return m_descriptors[1]; }
    int read(char* bytes, unsigned count) {
#if defined(_WIN32)
        return ::_read(m_descriptors[0], bytes, count);
#else
        return static_cast<int>(::read(m_descriptors[0], bytes, count));
#endif
    }
    void closeRead() { close(m_descriptors[0]); }
    void closeWrite() { close(m_descriptors[1]); }
private:
    static void close(int& descriptor) {
        if (descriptor < 0) return;
#if defined(_WIN32)
        ::_close(descriptor);
#else
        ::close(descriptor);
#endif
        descriptor = -1;
    }
    int m_descriptors[2]{-1, -1};
};
}

TEST_CASE("Stdio RPC output preserves large lines and final reply order") {
    OutputPipe pipe;
    REQUIRE(pipe.valid());
    std::string observed;
    std::thread reader([&] {
        char bytes[4096];
        for (;;) {
            const int count = pipe.read(bytes, sizeof(bytes));
            if (count <= 0) break;
            observed.append(bytes, static_cast<std::size_t>(count));
        }
    });
    const std::string large(1024u * 1024u, 'X');
    {
        Caesura::StdioRpcOutput output(pipe.writer());
        pipe.closeWrite();
        CHECK(output.writeLine(large));
        CHECK(output.writeLine(R"({"id":3,"result":"ok"})"));
        output.finish(std::chrono::seconds(3));
        CHECK_FALSE(output.failed());
        output.finish();
        CHECK_FALSE(output.writeLine("after-close"));
    }
    reader.join();
    CHECK(observed == large + '\n' + "{\"id\":3,\"result\":\"ok\"}\n");
}

TEST_CASE("Stdio RPC output cancels a real full pipe without appending a tail line") {
    OutputPipe pipe;
    REQUIRE(pipe.valid());
    std::atomic<int> failures{0};
    std::string observed;
    {
        Caesura::StdioRpcOutput output(pipe.writer(), [&] { ++failures; });
        pipe.closeWrite();
        CHECK(output.writeLine(std::string(1024u * 1024u, 'B')));
        char first = 0;
        // A real byte from the pipe anchors the in-flight write. The consumer
        // then stops, with substantially more data than the kernel capacity.
        REQUIRE(pipe.read(&first, 1) == 1);
        CHECK(first == 'B');
        CHECK(output.writeLine("forbidden-tail"));
        const auto started = std::chrono::steady_clock::now();
        output.finish(std::chrono::milliseconds(25));
        CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(2));
        CHECK(output.failed());
        CHECK(failures == 1);
        output.finish();
        CHECK_FALSE(output.writeLine("after-cancel"));
    }
    char bytes[4096];
    for (;;) {
        const int count = pipe.read(bytes, sizeof(bytes));
        if (count <= 0) break;
        observed.append(bytes, static_cast<std::size_t>(count));
    }
    CHECK(observed.find("forbidden-tail") == std::string::npos);
    CHECK(observed.find('\n') == std::string::npos);
}

TEST_CASE("Stdio RPC output rejects overflow and wakes its owner once") {
    OutputPipe pipe;
    REQUIRE(pipe.valid());
    std::atomic<int> failures{0};
    Caesura::StdioRpcOutput output(pipe.writer(), [&] { ++failures; });
    CHECK(output.writeLine(std::string(1024u * 1024u, 'B')));
    char first = 0;
    REQUIRE(pipe.read(&first, 1) == 1);
    // Leave the initial line in flight and fill the count budget with tiny
    // messages. This checks the producer path without waiting for pipe I/O.
    const auto started = std::chrono::steady_clock::now();
    for (std::size_t i = 1; i < Caesura::StdioRpcOutput::MaxPendingLines; ++i)
        CHECK(output.writeLine("{}"));
    CHECK_FALSE(output.writeLine("overflow"));
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(2));
    CHECK(output.failed());
    CHECK(failures == 1);
    output.finish(std::chrono::milliseconds(0));
    CHECK(failures == 1);
}

TEST_CASE("Stdio RPC output handles closed consumers and invalid descriptors") {
    OutputPipe pipe;
    REQUIRE(pipe.valid());
#if defined(__APPLE__)
    int originalNoSigPipe = 0;
    SUBCASE("caller uses default SIGPIPE policy") {}
    SUBCASE("caller already suppresses SIGPIPE") { originalNoSigPipe = 1; }
    REQUIRE(::fcntl(pipe.writer(), F_SETNOSIGPIPE, originalNoSigPipe) == 0);
    const int originalFlags = ::fcntl(pipe.writer(), F_GETFL);
    REQUIRE(originalFlags >= 0);
#endif
    pipe.closeRead();
    {
        Caesura::StdioRpcOutput output(pipe.writer());
        REQUIRE(output.ready());
#if defined(__APPLE__)
        CHECK(::fcntl(pipe.writer(), F_GETNOSIGPIPE) == 1);
#endif
        CHECK(output.writeLine("closed-reader"));
        output.finish();
        CHECK(output.failed());
    }
#if defined(__APPLE__)
    CHECK(::fcntl(pipe.writer(), F_GETNOSIGPIPE) == originalNoSigPipe);
    CHECK(::fcntl(pipe.writer(), F_GETFL) == originalFlags);
#endif
    Caesura::StdioRpcOutput invalid(-1);
    CHECK_FALSE(invalid.writeLine("invalid"));
    CHECK(invalid.failed());
    invalid.finish();
}

TEST_CASE("Stdio RPC output enforces its byte budget without truncation") {
    OutputPipe pipe;
    REQUIRE(pipe.valid());
    Caesura::StdioRpcOutput output(pipe.writer());
    CHECK_FALSE(output.writeLine(std::string(Caesura::StdioRpcOutput::MaxPendingBytes, 'X')));
    CHECK(output.failed());
    output.finish();
}

TEST_CASE("Stdio RPC output isolates a throwing failure observer") {
    OutputPipe pipe;
    REQUIRE(pipe.valid());
    pipe.closeRead();
    std::atomic<int> failures{0};
    Caesura::StdioRpcOutput output(pipe.writer(), [&] {
        ++failures;
        throw std::runtime_error("failure observer also failed");
    });
    CHECK(output.writeLine("closed-reader"));
    output.finish();
    CHECK(output.failed());
    CHECK(failures == 1);
    CHECK_FALSE(output.writeLine("after-failure"));
}
