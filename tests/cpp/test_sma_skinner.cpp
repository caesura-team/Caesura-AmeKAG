// test_sma_skinner.cpp - SMA CPU soft-skinning math tests (Battle 4d S2).
// Pure, deterministic, GPU-free: world-pose application + per-vertex
// weight blending (SmaSkinner.h) plus the SmaMeshRenderer no-GPU contract
// (deferred-gpu pattern: without bgfx every operation is a safe no-op).
#include "doctest.h"
#include "render/SmaSkinner.h"
#include "render/SmaMeshRenderer.h"
#include <cmath>
#include <algorithm>

using namespace Caesura;

namespace {
constexpr float kEps = 1e-4f;

SMAMesh makeQuad() {
    SMAMesh mesh;
    mesh.vertices = {
        {0.f, 0.f, 0.f, 0.f, 0, 1.f, 1, 0.f},
        {1.f, 0.f, 1.f, 0.f, 0, 1.f, 1, 0.f},
        {1.f, 1.f, 1.f, 1.f, 0, 1.f, 1, 0.f},
        {0.f, 1.f, 0.f, 1.f, 0, 1.f, 1, 0.f},
    };
    mesh.indices = {0, 1, 2, 0, 2, 3};
    return mesh;
}
} // namespace

TEST_CASE("applyBonePose rotates about the origin") {
    BonePose pose;
    pose.rot = 3.14159265f / 2.f; // 90 degrees CCW
    float x, y;
    applyBonePose(pose, 1.f, 0.f, x, y);
    CHECK(std::fabs(x - 0.f) < kEps);
    CHECK(std::fabs(y - 1.f) < kEps);
}

TEST_CASE("applyBonePose scales and offsets") {
    BonePose pose;
    pose.scale = 2.f;
    pose.ox = 10.f;
    pose.oy = -3.f;
    float x, y;
    applyBonePose(pose, 1.f, 1.f, x, y);
    CHECK(std::fabs(x - 12.f) < kEps);
    CHECK(std::fabs(y + 1.f) < kEps); // 1*2 + (-3) = -1
}

TEST_CASE("skinMesh single-bone mesh") {
    SMAMesh mesh = makeQuad();
    std::vector<BonePose> poses(2);
    poses[0].ox = 5.f; // translate only
    std::vector<SmaSkinnedVertex> out;
    skinMesh(mesh, poses, out);
    REQUIRE(out.size() == 4);
    CHECK(std::fabs(out[0].x - 5.f) < kEps);
    CHECK(std::fabs(out[2].x - 6.f) < kEps); // 1 + 5
    CHECK(std::fabs(out[2].u - 1.f) < kEps); // UV untouched
    CHECK(std::fabs(out[2].v - 1.f) < kEps);
}

TEST_CASE("skinMesh blends two weighted bones") {
    SMAMesh mesh = makeQuad();
    // vertex 0: 50/50 between bone0 (+10 x) and bone1 (+20 x) -> +15
    mesh.vertices[0].bone0 = 0;
    mesh.vertices[0].w0 = 0.5f;
    mesh.vertices[0].bone1 = 1;
    mesh.vertices[0].w1 = 0.5f;
    std::vector<BonePose> poses(2);
    poses[0].ox = 10.f;
    poses[1].ox = 20.f;
    std::vector<SmaSkinnedVertex> out;
    skinMesh(mesh, poses, out);
    CHECK(std::fabs(out[0].x - 15.f) < kEps);
}

TEST_CASE("skinMesh weight normalization (25/75)") {
    SMAMesh mesh = makeQuad();
    mesh.vertices[0].bone0 = 0;
    mesh.vertices[0].w0 = 1.f;
    mesh.vertices[0].bone1 = 1;
    mesh.vertices[0].w1 = 3.f; // sum 4 -> normalized 0.25 / 0.75
    std::vector<BonePose> poses(2);
    poses[0].ox = 0.f;
    poses[1].ox = 4.f;
    std::vector<SmaSkinnedVertex> out;
    skinMesh(mesh, poses, out);
    CHECK(std::fabs(out[0].x - 3.f) < kEps); // 0.25*0 + 0.75*4
}

TEST_CASE("skinMesh zero-weight vertex stays in place") {
    SMAMesh mesh = makeQuad();
    mesh.vertices[1].bone0 = 0;
    mesh.vertices[1].w0 = 0.f;
    mesh.vertices[1].bone1 = 1;
    mesh.vertices[1].w1 = 0.f;
    std::vector<BonePose> poses(2);
    poses[0].ox = 100.f;
    poses[1].ox = 200.f;
    std::vector<SmaSkinnedVertex> out;
    skinMesh(mesh, poses, out);
    CHECK(std::fabs(out[1].x - 1.f) < kEps);
    CHECK(std::fabs(out[1].y - 0.f) < kEps);
}

TEST_CASE("skinMesh out-of-range bone index is identity") {
    SMAMesh mesh = makeQuad();
    mesh.vertices[0].bone0 = 7; // no pose for bone 7
    std::vector<BonePose> poses(1);
    poses[0].ox = 9.f;
    std::vector<SmaSkinnedVertex> out;
    skinMesh(mesh, poses, out);
    CHECK(std::fabs(out[0].x - 0.f) < kEps);
    CHECK(std::fabs(out[0].y - 0.f) < kEps);
}

TEST_CASE("SmaMeshRenderer no-GPU contract (deferred)") {
    SmaMeshRenderer renderer; // bgfx not initialized in unit tests
    CHECK_FALSE(renderer.isInitialized());
    CHECK(renderer.meshCount() == 0);

    SMAMesh mesh = makeQuad();
    // createMesh without GPU: lazy init bails -> invalid handle, no crash
    MeshHandle h = renderer.createMesh(mesh);
    CHECK_FALSE(h);
    CHECK(renderer.meshCount() == 0);

    std::vector<BonePose> poses(1);
    poses[0].ox = 1.f;
    CHECK_NOTHROW(renderer.updateMesh(h, poses));
    CHECK_NOTHROW(renderer.destroyMesh(h));
    CHECK_NOTHROW(renderer.drawMesh(0, h, 1, 0.f, 0.f, 1.f, 1.f));
}

TEST_CASE("SmaMeshRenderer device-loss callbacks are safe without GPU") {
    SmaMeshRenderer renderer;
    CHECK_NOTHROW(renderer.onDeviceLost());
    CHECK_NOTHROW(renderer.onDeviceRestored());
    CHECK_FALSE(renderer.isInitialized());
    CHECK(renderer.meshCount() == 0);
}

// ===========================================================================
// S5: GPU skinning math — packBonePoses + compute-shader replica must
// agree with the CPU reference (skinMesh) exactly.
// ===========================================================================

TEST_CASE("S5 packBonePose matches applyBonePose") {
    // Deterministic pseudo-random poses.
    uint32_t seed = 0xC0FFEEu;
    auto rnd = [&seed]() {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>((seed >> 8) & 0xFFFFFF) / 16777216.f;
    };
    for (int i = 0; i < 200; ++i) {
        BonePose pose;
        pose.rot = (rnd() - 0.5f) * 6.2831853f;
        pose.scale = 0.2f + rnd() * 3.f;
        pose.ox = (rnd() - 0.5f) * 200.f;
        pose.oy = (rnd() - 0.5f) * 200.f;
        const float px = (rnd() - 0.5f) * 10.f;
        const float py = (rnd() - 0.5f) * 10.f;

        float cpuX, cpuY;
        applyBonePose(pose, px, py, cpuX, cpuY);
        float packed[4];
        packBonePose(pose, packed);
        // Shader math: x' = m0*x - m1*y + ox; y' = m1*x + m0*y + oy.
        const float gpuX = packed[0] * px - packed[1] * py + packed[2];
        const float gpuY = packed[1] * px + packed[0] * py + packed[3];
        CHECK(std::fabs(cpuX - gpuX) < 1e-5f);
        CHECK(std::fabs(cpuY - gpuY) < 1e-5f);
    }
}

// Headless packed-formula check, not evidence that shader bytecode executed.
// Actual compute execution and output isolation have separate render tests.
namespace {
void gpuSkinReplica(const SMAMesh& mesh,
                    const std::vector<BonePose>& poses,
                    std::vector<SmaSkinnedVertex>& out) {
    std::vector<float> bones;
    const size_t poseCount=std::min(poses.size(),size_t(UINT16_MAX)+1);
    packBonePoses(poses, poseCount, bones);
    out.resize(mesh.vertices.size());
    for (size_t i = 0; i < mesh.vertices.size(); ++i) {
        const SMAMeshVertex& v = mesh.vertices[i];
        float x = v.x, y = v.y;
        if (v.bone0 < poseCount) {
            const float* m0 = &bones[size_t(v.bone0) * 4];
            const float p0x = m0[0] * v.x - m0[1] * v.y + m0[2];
            const float p0y = m0[1] * v.x + m0[0] * v.y + m0[3];
            if (v.bone1 < poseCount) {
                const float wsum = v.w0 + v.w1;
                if (!(wsum <= 0.f)) {
                    const float* m1 = &bones[size_t(v.bone1) * 4];
                    const float p1x = m1[0] * v.x - m1[1] * v.y + m1[2];
                    const float p1y = m1[1] * v.x + m1[0] * v.y + m1[3];
                    x = (p0x * v.w0 + p1x * v.w1) / wsum;
                    y = (p0y * v.w0 + p1y * v.w1) / wsum;
                }
            } else {
                x = p0x;
                y = p0y;
            }
        }
        out[i] = { x, y, v.u, v.v };
    }
}
} // namespace

TEST_CASE("S5 compute-shader replica agrees with CPU skinMesh") {
    // Randomized meshes + poses: 2-bone blends, single-bone, zero weight.
    uint32_t seed = 0x5A17u;
    auto rnd = [&seed]() {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>((seed >> 8) & 0xFFFFFF) / 16777216.f;
    };
    for (int iter = 0; iter < 30; ++iter) {
        const int verts = 8 + int(rnd() * 40);
        SMAMesh mesh;
        mesh.vertices.resize(verts);
        for (int i = 0; i < verts; ++i) {
            auto& v = mesh.vertices[i];
            v.x = (rnd() - 0.5f) * 20.f;
            v.y = (rnd() - 0.5f) * 20.f;
            v.u = rnd();
            v.v = rnd();
            v.bone0 = uint16_t(int(rnd() * 5));  // 0..4
            v.w0 = rnd();
            if (rnd() < 0.7f) {
                v.bone1 = uint16_t(int(rnd() * 5));
                v.w1 = rnd() * 2.f;
            } else {
                v.w1 = 0.f;
            }
        }
        std::vector<BonePose> poses(5);
        for (auto& p : poses) {
            p.rot = (rnd() - 0.5f) * 3.f;
            p.scale = 0.5f + rnd() * 2.f;
            p.ox = (rnd() - 0.5f) * 100.f;
            p.oy = (rnd() - 0.5f) * 100.f;
        }
        std::vector<SmaSkinnedVertex> cpu, gpu;
        skinMesh(mesh, poses, cpu);
        gpuSkinReplica(mesh, poses, gpu);
        REQUIRE(cpu.size() == gpu.size());
        for (size_t i = 0; i < cpu.size(); ++i) {
            CHECK(std::fabs(cpu[i].x - gpu[i].x) < 1e-4f);
            CHECK(std::fabs(cpu[i].y - gpu[i].y) < 1e-4f);
            CHECK(std::fabs(cpu[i].u - gpu[i].u) < 1e-6f);
            CHECK(std::fabs(cpu[i].v - gpu[i].v) < 1e-6f);
        }
    }
}

TEST_CASE("S5 packBonePoses identity rows for missing bones") {
    std::vector<BonePose> poses(2);
    poses[0].ox = 5.f;
    poses[1].ox = 7.f;
    std::vector<float> packed;
    packBonePoses(poses, 64, packed);
    REQUIRE(packed.size() == 64 * 4);
    CHECK(std::fabs(packed[0] - 1.f) < 1e-6f);    // identity row 0
    CHECK(std::fabs(packed[1]) < 1e-6f);
    CHECK(std::fabs(packed[2] - 5.f) < 1e-6f);
    CHECK(std::fabs(packed[3]) < 1e-6f);
    const float* row2 = &packed[2 * 4];
    CHECK(std::fabs(row2[0] - 1.f) < 1e-6f);      // unused bone 2 = identity
    CHECK(std::fabs(row2[1]) < 1e-6f);
    CHECK(std::fabs(row2[2]) < 1e-6f);
    CHECK(std::fabs(row2[3]) < 1e-6f);
}

TEST_CASE("S5 SkinMode state machine (no-GPU contract)") {
    SmaMeshRenderer renderer;
    CHECK(renderer.skinMode() == SkinMode::Auto);
    renderer.setSkinMode(SkinMode::Cpu);
    CHECK(renderer.skinMode() == SkinMode::Cpu);
    renderer.setSkinMode(SkinMode::Gpu);
    CHECK(renderer.skinMode() == SkinMode::Gpu);
    renderer.setSkinMode(SkinMode::Auto);
    CHECK(renderer.skinMode() == SkinMode::Auto);

    // Without bgfx everything stays a safe no-op regardless of mode.
    SMAMesh mesh = makeQuad();
    MeshHandle h = renderer.createMesh(mesh);
    CHECK_FALSE(h);
    std::vector<BonePose> poses(2);
    poses[0].ox = 1.f;
    renderer.setSkinMode(SkinMode::Gpu);
    CHECK_NOTHROW(renderer.updateMesh(h, poses));
    CHECK_NOTHROW(renderer.drawMesh(0, h, 1, 0.f, 0.f, 1.f, 1.f));
    CHECK(renderer.meshCount() == 0);
}
