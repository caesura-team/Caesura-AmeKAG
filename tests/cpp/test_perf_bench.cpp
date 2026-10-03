// test_perf_bench.cpp — engine CPU hot-path benchmarks (round 25).
// Establishes baselines for the VN-critical paths that run every frame:
//   - Lua string/table throughput (text formatting is per-line in KAG)
//   - SmaSkinner CPU soft skinning (the GPU compute path's reference)
// Pure CPU, no window/GPU: deterministic, runs on every CI platform.
// Assertions use generous ceilings so slow CI runners stay green; the
// printed numbers are the regression signal.
#include "doctest.h"

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

#include "render/SmaSkinner.h"
#include "archive/CryptoEngine.h"
#include "nlohmann_json.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

// Opt-in raw observations. The external controller owns source/build/policy
// authentication; these records never assert release acceptance or speedup.
class BenchmarkProtocol {
public:
    BenchmarkProtocol() {
        constexpr std::array<const char*, 6> keys{{
            "CAESURA_BENCH_PROTOCOL", "CAESURA_BENCH_RUN_UUID",
            "CAESURA_BENCH_WORKLOAD_SHA256", "CAESURA_BENCH_WARMUPS",
            "CAESURA_BENCH_SAMPLES", "CAESURA_BENCH_SEED"}};
        std::array<std::string, 6> values;
        for (size_t i = 0; i < keys.size(); ++i) {
            if (const auto* value = std::getenv(keys[i])) {
                enabled_ = true;
                values[i] = value;
            }
        }
        if (!enabled_) return;
        auto hex = [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); };
        bool validUuid = values[1].size() == 36;
        for (size_t i = 0; validUuid && i < values[1].size(); ++i) {
            validUuid = (i == 8 || i == 13 || i == 18 || i == 23)
                ? values[1][i] == '-' : hex(values[1][i]);
        }
        bool validDigest = values[2].size() == 64;
        for (char c : values[2]) validDigest = validDigest && hex(c);
        if (values[0] != schema() || !validUuid || !validDigest ||
            values[3] != "2" || values[4] != "10" || values[5] != "0") {
            throw std::runtime_error("Invalid or incomplete CAESURA_BENCH protocol environment");
        }
        runUuid_ = values[1];
        emit({{"event", "header"}, {"workload_sha256", values[2]},
              {"seed", 0}, {"rng", "none_fixed_workload"}, {"warmups", 2}, {"samples", 10},
              {"metrics", {"lua_format_append_10000", "lua_table_reads_10000", "sma_skin_8192x10"}}});
    }

    bool enabled() const { return enabled_; }

    template <typename Work, typename Validate>
    void sample(const char* metric, int workUnits, int expected, double ceilingMs,
                Work&& work, Validate&& validate) {
        if (completed_.count(metric) != 0)
            throw std::runtime_error("Benchmark metric repeated in one process");
        for (int i = 0; i < 12; ++i) {
            const auto start = std::chrono::steady_clock::now();
            work();
            const auto finish = std::chrono::steady_clock::now();
            const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(finish - start).count();
            // Validation, output hashing and JSON serialization are outside timing.
            const std::string fingerprint = validate();
            REQUIRE_MESSAGE(ns > 0, "Sampler clock did not advance");
            CHECK_MESSAGE(static_cast<double>(ns) / 1e6 < ceilingMs,
                          "Original CPU benchmark ceiling exceeded");
            nlohmann::json row{{"event", "sample"}, {"metric", metric},
                {"phase", i < 2 ? "warmup" : "measurement"}, {"index", i < 2 ? i : i - 2},
                {"elapsed_ns", ns}, {"work_units", workUnits},
                {"correctness_ok", true}, {"observed_result", expected}};
            if (!fingerprint.empty()) row["result_sha256"] = fingerprint;
            emit(std::move(row));
        }
        completed_.insert(metric);
        if (completed_.size() == 3)
            emit({{"event", "footer"}, {"warmup_count", 6}, {"measurement_count", 30},
                  {"correctness_ok", true}});
    }

private:
    static const char* schema() { return "caesura.cpu-benchmark.v1"; }
    void emit(nlohmann::json row) const {
        row["schema"] = schema();
        row["run_uuid"] = runUuid_;
        const auto bytes = row.dump();
        if (std::printf("[CAESURA_BENCH] %s\n", bytes.c_str()) < 0 || std::fflush(stdout) != 0)
            throw std::runtime_error("Could not publish benchmark observation");
    }
    bool enabled_ = false;
    std::string runUuid_;
    std::set<std::string> completed_;
};

BenchmarkProtocol& benchmarkProtocol() {
    static BenchmarkProtocol protocol;
    return protocol;
}

void sampleLua(BenchmarkProtocol& protocol, lua_State* L, const char* metric,
               const char* body, lua_Integer expected, double ceilingMs) {
    int status = LUA_OK;
    protocol.sample(metric, 10000, static_cast<int>(expected), ceilingMs,
        [&] { status = luaL_dostring(L, body); }, [&] {
            REQUIRE_MESSAGE(status == LUA_OK, "Measured Lua workload failed");
            REQUIRE(lua_isinteger(L, -1));
            REQUIRE(lua_tointeger(L, -1) == expected);
            lua_pop(L, 1);
            return std::string{};
        });
}

std::string skinFingerprint(const std::vector<Caesura::SmaSkinnedVertex>& vertices) {
    // Float32 bits, x/y/u/v in vertex order, explicitly little-endian. Do not
    // hash struct padding or native-endian memory. Both variants use this file.
    static_assert(sizeof(float) == sizeof(uint32_t), "Float32 workload required");
    std::vector<uint8_t> bytes;
    bytes.reserve(vertices.size() * 16);
    for (const auto& vertex : vertices) {
        for (float value : {vertex.x, vertex.y, vertex.u, vertex.v}) {
            REQUIRE(std::isfinite(value));
            uint32_t bits = 0;
            std::memcpy(&bits, &value, sizeof(bits));
            for (unsigned shift = 0; shift < 32; shift += 8)
                bytes.push_back(static_cast<uint8_t>(bits >> shift));
        }
    }
    uint8_t digest[32];
    Caesura::carc::CryptoEngine::sha256(bytes.data(), bytes.size(), digest);
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (uint8_t byte : digest) { result += hex[byte >> 4]; result += hex[byte & 15]; }
    return result;
}

template <typename F>
double measureMs(F&& fn) {
    const auto t0 = std::chrono::steady_clock::now();
    fn();
    const auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

// Build an 8k-vertex dual-bone mesh (same shape as the GPU perf test).
Caesura::SMAMesh makeBigMesh() {
    constexpr int kCols = 128;
    constexpr int kRows = 64;
    Caesura::SMAMesh mesh;
    mesh.vertices.reserve(kCols * kRows);
    for (int r = 0; r < kRows; ++r) {
        for (int c = 0; c < kCols; ++c) {
            Caesura::SMAMeshVertex v;
            v.x = static_cast<float>(c);
            v.y = static_cast<float>(r);
            v.u = static_cast<float>(c) / (kCols - 1);
            v.v = static_cast<float>(r) / (kRows - 1);
            v.bone0 = static_cast<uint16_t>((c + r * 3) % 64);
            v.w0 = 0.6f;
            v.bone1 = static_cast<uint16_t>((c * 7 + r) % 64);
            v.w1 = 0.4f;
            mesh.vertices.push_back(v);
        }
    }
    return mesh;
}

} // namespace

// ---------------------------------------------------------------------------
// Lua VM: string formatting + table append (text/backlog hot path).
// ---------------------------------------------------------------------------
TEST_CASE("Perf: Lua string/table throughput") {
    auto& protocol = benchmarkProtocol();
    const std::unique_ptr<lua_State, decltype(&lua_close)> state(luaL_newstate(), &lua_close);
    lua_State* L = state.get();
    REQUIRE(L != nullptr);
    luaL_openlibs(L);

    const char* body =
        "local t = {} "
        "for i=1,10000 do "
        "  t[#t+1] = string.format('%d:%s', i, tostring(i)) "
        "end "
        "return #t";
    if (protocol.enabled()) {
        sampleLua(protocol, L, "lua_format_append_10000", body, 10000, 800.0);
        return;
    }
    const char* warmup = "for i=1,2000 do local s = string.format('%d:%s', i, tostring(i)) end";
    REQUIRE(luaL_dostring(L, warmup) == LUA_OK);
    const double ms = measureMs([&]() {
        REQUIRE(luaL_dostring(L, body) == LUA_OK);
    });
    lua_pop(L, 1);  // the return value

    MESSAGE("Lua throughput: 10k format+append = " << ms << " ms");
    // Generous ceiling: an order-of-magnitude regression must trip it.
    CHECK_MESSAGE(ms < 800.0,
                  "10k Lua format+append took " << ms << "ms (was <100ms locally)");
}

// ---------------------------------------------------------------------------
// Lua VM: table field access (KAG variable reads are table-heavy).
// ---------------------------------------------------------------------------
TEST_CASE("Perf: Lua table field access") {
    auto& protocol = benchmarkProtocol();
    const std::unique_ptr<lua_State, decltype(&lua_close)> state(luaL_newstate(), &lua_close);
    lua_State* L = state.get();
    REQUIRE(L != nullptr);
    luaL_openlibs(L);

    const char* setup = "local t = {} for i=1,100 do t[i] = {x=i, y=i*2} end return t";
    REQUIRE(luaL_dostring(L, setup) == LUA_OK);
    lua_setglobal(L, "perf_t");

    const char* body =
        "local t = perf_t; local acc = 0 "
        "for i=1,10000 do "
        "  local e = t[(i % 100) + 1]; acc = acc + e.x + e.y "
        "end "
        "return acc";
    if (protocol.enabled()) {
        sampleLua(protocol, L, "lua_table_reads_10000", body, 1515000, 400.0);
        return;
    }
    const double ms = measureMs([&]() {
        REQUIRE(luaL_dostring(L, body) == LUA_OK);
    });
    lua_pop(L, 1);

    MESSAGE("Lua throughput: 10k table reads = " << ms << " ms");
    CHECK_MESSAGE(ms < 400.0,
                  "10k Lua table reads took " << ms << "ms (regression?)");
}

// ---------------------------------------------------------------------------
// SmaSkinner CPU soft skinning (GPU compute reference; see the D3D11 perf
// test for the GPU side of the comparison).
// ---------------------------------------------------------------------------
TEST_CASE("Perf: SmaSkinner 8k-vertex soft skin") {
    auto& protocol = benchmarkProtocol();
    const auto mesh = makeBigMesh();
    std::vector<Caesura::BonePose> poses(64);
    for (size_t i = 0; i < poses.size(); ++i) {
        poses[i].rot = static_cast<float>((i * 7) % 360) * 0.01f;
        poses[i].scale = 0.9f + static_cast<float>((i * 13) % 20) * 0.01f;
        poses[i].ox = static_cast<float>((i * 3) % 50);
        poses[i].oy = static_cast<float>((i * 11) % 40);
    }

    std::vector<Caesura::SmaSkinnedVertex> out;
    if (protocol.enabled()) {
        protocol.sample("sma_skin_8192x10", 81920, 8192, 100.0,
            [&] { for (int i = 0; i < 10; ++i) Caesura::skinMesh(mesh, poses, out); },
            [&] {
                REQUIRE(out.size() == mesh.vertices.size());
                return skinFingerprint(out);
            });
        return;
    }
    Caesura::skinMesh(mesh, poses, out);  // warmup

    const double totalMs = measureMs([&]() {
        for (int i = 0; i < 10; ++i) Caesura::skinMesh(mesh, poses, out);
    });
    const double avgMs = totalMs / 10.0;
    MESSAGE("CPU skin 8k verts: " << avgMs << " ms/frame (GPU compute: ~0.08ms)");
    CHECK_MESSAGE(out.size() == mesh.vertices.size(), "skinned output size");
    // Local baseline is ~1.3ms; allow a wide margin for slow CI runners.
    CHECK_MESSAGE(avgMs < 10.0,
                  "CPU skin of 8k verts averaged " << avgMs << "ms (was ~1.3ms)");
}
