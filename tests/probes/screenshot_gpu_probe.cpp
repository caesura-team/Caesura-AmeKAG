// Opt-in real D3D11 observations. This is a standalone composition-root probe,
// not a headless doctest and not a replacement implementation of screenshots.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define SDL_MAIN_HANDLED
#include "entry/Engine.h"
#include "entry/EngineConfig.h"
#include "audio/NullAudioBackend.h"
#include "di/BackendRegistry.h"
#include "di/api/ThreadAssert.h"
#include "platform/api/IPlatformBackend.h"
#include "render/BgfxRenderDevice.h"
#include "render/api/ITextureManager.h"
#include "resource/api/IImageDecoder.h"
#include "script/api/ILuaManager.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <bgfx/bgfx.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <windows.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include <lua.h>
}

namespace {
using namespace Caesura;
namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;
constexpr uint32_t kWidth = 640, kHeight = 360;
constexpr size_t kPageBytes = size_t(kWidth) * kHeight * 4;
constexpr std::array<uint8_t, 3> kTextureColor{214, 62, 43};
constexpr std::array<uint8_t, 3> kRttColor{39, 181, 116};
constexpr const char* kFont = "assets/fonts/NotoSansCJKsc-Regular.otf";

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct FontReference {
    Bytes coverage = Bytes(size_t(kWidth) * kHeight, 0);
    json description;
};

// Independent CPU oracle: rasterize the fixed font directly through FreeType.
// It never calls TextRenderer layout/buildQuadVertices or reads a GPU result to
// define the expected mask. Positions follow the fixture's baseline/advances.
FontReference rasterFontReference() {
    struct FreeTypeOwner {
        FT_Library library = nullptr;
        FT_Face face = nullptr;
        ~FreeTypeOwner() {
            if (face) FT_Done_Face(face);
            if (library) FT_Done_FreeType(library);
        }
    } font;
    require(FT_Init_FreeType(&font.library) == 0, "U16 reference FreeType initialization failed");
    require(FT_New_Face(font.library, kFont, 0, &font.face) == 0, "U16 reference font could not load");
    require(FT_Set_Pixel_Sizes(font.face, 0, 28) == 0, "U16 reference pixel size rejected");
    const double ascent = double(font.face->size->metrics.ascender) / 64.0;
    require(std::floor(ascent) == ascent, "U16 fixed reference requires an integer hinted ascender");
    int major = 0, minor = 0, patch = 0;
    FT_Library_Version(font.library, &major, &minor, &patch);
    FontReference result;
    result.description = { {"font", kFont}, {"pixel_size", 28}, {"text", "GPU 15"},
        {"x", 432}, {"y", 96}, {"ascender", ascent},
        {"freetype_version", std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch)},
        {"glyphs", json::array()}, {"oracle", "Independent FreeType FT_Load_Char with FT_LOAD_RENDER; opaque bitmap texels"} };
    int penX = 432;
    const int baseline = 96 + static_cast<int>(ascent);
    for (const unsigned char ch : std::string("GPU 15")) {
        require(FT_Load_Char(font.face, ch, FT_LOAD_RENDER) == 0, "U16 reference glyph rasterization failed");
        const auto& bitmap = font.face->glyph->bitmap;
        const int left = penX + font.face->glyph->bitmap_left;
        const int top = baseline - font.face->glyph->bitmap_top;
        const int advance = static_cast<int>(font.face->glyph->advance.x >> 6);
        result.description["glyphs"].push_back({{"codepoint", ch}, {"left", left}, {"top", top},
            {"width", bitmap.width}, {"height", bitmap.rows}, {"advance", advance}});
        if (bitmap.width && bitmap.rows) {
            require(bitmap.buffer && bitmap.pixel_mode == FT_PIXEL_MODE_GRAY
                && bitmap.num_grays == 256 && std::abs(bitmap.pitch) >= static_cast<int>(bitmap.width),
                "U16 reference requires valid 8-bit grayscale raster rows");
            for (unsigned y = 0; y < bitmap.rows; ++y) {
                const auto* row = bitmap.buffer + ptrdiff_t(y) * bitmap.pitch;
                for (unsigned x = 0; x < bitmap.width; ++x) {
                    const int targetX = left + static_cast<int>(x), targetY = top + static_cast<int>(y);
                    require(targetX >= 0 && targetX < int(kWidth) && targetY >= 0 && targetY < int(kHeight),
                        "U16 reference glyph lies outside the fixed viewport");
                    auto& destination = result.coverage[size_t(targetY) * kWidth + size_t(targetX)];
                    destination = std::max(destination, row[x]);
                }
            }
        }
        penX += advance;
    }
    result.description["opaque_pixels"] = std::count(result.coverage.begin(), result.coverage.end(), uint8_t(255));
    return result;
}

class HiddenPlatform final : public IPlatformBackend {
public:
    ~HiddenPlatform() override { shutdown(); }
    bool init(const char* title, int width, int height) override {
        if (window_) return true;
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) return false;
        initialized_ = true;
        const auto props = SDL_CreateProperties();
        if (!props) return false;
        SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, title);
        SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, width);
        SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, height);
        SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_HIDDEN_BOOLEAN, true);
        window_ = SDL_CreateWindowWithProperties(props);
        SDL_DestroyProperties(props);
        width_ = width; height_ = height;
        return window_ != nullptr;
    }
    void shutdown() override {
        if (window_) { SDL_DestroyWindow(window_); window_ = nullptr; }
        if (initialized_) { SDL_Quit(); initialized_ = false; }
    }
    bool pollEvent() override { SDL_Event event{}; return SDL_PollEvent(&event); }
    MouseState getMouseState() const override {
        MouseState value;
        value.leftDown = (SDL_GetMouseState(&value.x, &value.y) & SDL_BUTTON_LMASK) != 0;
        return value;
    }
    uint64_t getTicksMs() const override { return SDL_GetTicks(); }
    void* getNativeWindowHandle() const override {
        return window_ ? SDL_GetPointerProperty(SDL_GetWindowProperties(window_),
            SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr) : nullptr;
    }
    int getWindowWidth() const override { return width_; }
    int getWindowHeight() const override { return height_; }
    void setFullscreen(bool enabled) override {
        if (window_) SDL_SetWindowFullscreen(window_, enabled);
    }
    void resizeWindow(int width, int height) override {
        if (window_ && SDL_SetWindowSize(window_, width, height)) {
            width_ = width; height_ = height;
        }
    }
    const char* getBackendName() const override { return "SDL3 hidden real HWND"; }
    bool startTextInput() override { return window_ && SDL_StartTextInput(window_); }
    bool stopTextInput() override { return window_ && SDL_StopTextInput(window_); }
    bool setTextInputRect(int x, int y, int w, int h, int cursor) override {
        SDL_Rect rect{x, y, w, h};
        return window_ && SDL_SetTextInputArea(window_, &rect, cursor);
    }
    bool isTextInputActive() const override { return window_ && SDL_TextInputActive(window_); }
private:
    SDL_Window* window_ = nullptr;
    bool initialized_ = false;
    int width_ = kWidth, height_ = kHeight;
};

void writeBytes(const fs::path& path, const void* bytes, size_t count) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    require(bool(stream), "Cannot create observation file");
    stream.write(static_cast<const char*>(bytes), static_cast<std::streamsize>(count));
    stream.close();
    require(bool(stream), "Cannot finish observation file");
}

void writeReport(const fs::path& output, const json& report) {
    const auto content = report.dump(2);
    writeBytes(output / "result.json", content.data(), content.size());
}

const char* statusName(ScreenshotStatus status) {
    switch (status) {
    case ScreenshotStatus::Pending: return "Pending";
    case ScreenshotStatus::Completed: return "Completed";
    case ScreenshotStatus::Failed: return "Failed";
    case ScreenshotStatus::Cancelled: return "Cancelled";
    default: return "Unknown";
    }
}

json description(const ScreenshotResult& result) {
    return {{"request_id", result.ticket.requestId}, {"generation", result.ticket.generation},
        {"status", statusName(result.status)}, {"frame_id", result.frameId},
        {"width", result.width}, {"height", result.height}, {"png_bytes", result.png.size()},
        {"error", result.error}};
}

json fontDescription(const FontRestoreState& state) {
    return {{"active", state.active}, {"font", int(state.font)},
        {"asset_path", state.assetPath}, {"pixel_size", state.pixelSize}};
}

Bytes decodeBase64(const std::string& input) {
    if (input.empty() || input.size() % 4 != 0) return {};
    const std::string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    Bytes bytes;
    for (size_t offset = 0; offset < input.size(); offset += 4) {
        uint32_t word = 0;
        unsigned padding = 0;
        for (size_t i = 0; i < 4; ++i) {
            const char ch = input[offset + i];
            if (ch == '=') {
                if (i < 2 || offset + 4 != input.size()) return {};
                ++padding; word <<= 6;
            } else {
                const auto value = alphabet.find(ch);
                if (value == std::string::npos || padding) return {};
                word = (word << 6) | static_cast<uint32_t>(value);
            }
        }
        bytes.push_back(static_cast<uint8_t>(word >> 16));
        if (padding < 2) bytes.push_back(static_cast<uint8_t>(word >> 8));
        if (!padding) bytes.push_back(static_cast<uint8_t>(word));
    }
    return bytes;
}

class Probe {
public:
    Probe(fs::path output, json& report) : output_(std::move(output)), report_(report) {
        EngineConfig config;
        config.width = kWidth; config.height = kHeight;
        config.title = "Caesura U15 screenshot GPU probe";
        config.editorMode = true;
        config.renderBackend = "dx11";
        platform_ = new HiddenPlatform;
        config.platform = platform_;
        config.render = new BgfxRenderDevice;
        config.audio = new NullAudioBackend;
        engine_ = std::make_unique<Engine>(std::move(config));
        require(engine_->init(), "Real Engine initialization failed");
        require(bgfx::getRendererType() == bgfx::RendererType::Direct3D11, "Actual Direct3D11 required");
        require(device().getRuntimeInfo().shaderReady, "Actual GPU shader setup failed");
        auto& registry = BackendRegistry::instance();
        textures_ = registry.getTextureManager();
        decoder_ = registry.getImageDecoder();
        vm_ = registry.getLuaManager();
        require(textures_ && decoder_ && vm_ && vm_->state(), "Engine services missing");
        require(fs::is_regular_file(kFont), "Required font fixture is absent");
        require(device().loadTTF(kFont, 28.0f), "Required real TTF font could not load");
        device().setFont(int(FontId::TTF));
        fontReference_ = rasterFontReference();
        report_["u16_font_reference"] = fontReference_.description;
        textureId_ = textures_->createSolidTexture(kTextureColor[0], kTextureColor[1], kTextureColor[2]);
        require(textures_->isValid(textureId_), "Manager-owned GPU texture creation failed");
        createRtt();
        installCallback("engine_render", &Probe::renderCallback, this);
        installCallback("engine_update", &Probe::updateCallback, this);
        bgfx::setDebug(BGFX_DEBUG_NONE);
        report_["host"] = {{"renderer", bgfx::getRendererName(bgfx::getRendererType())},
            {"backend", device().getBackendName()}, {"shader_ready", true},
            {"native_width", kWidth}, {"native_height", kHeight},
            {"platform", platform_->getBackendName()}, {"audio", "NullAudio; outside this probe's scope"}};
        report_["boundaries"] = {
            "Real D3D11 GPU and production Engine/renderer/screenshot queue/PNG decoder.",
            "No present or renderer frame advances while awaiting a ticket or Engine RPC result.",
            "Known RTT fixture uses a borrowed texture attachment and GPU clear; production blitViewport composites it.",
            "Recovery verifies retained TextureManager ID and TTF state; RTT is explicitly recreated, not content-restored.",
            "Post-core font-restoration failure and native OS device removal are unmeasured; explicit recoverDevice is exercised."
        };
        persist();
    }

    ~Probe() { releaseFixtureFramebuffer(); }
    IRenderDevice& device() { return engine_->renderDevice(); }
    void persist() { writeReport(output_, report_); }
    void check(const std::string& name, bool passed, json detail = json::object()) {
        report_["checks"].push_back({{"name", name}, {"passed", passed}, {"detail", std::move(detail)}});
        if (!passed) ++failures_;
        persist();
    }
    unsigned failures() const { return failures_; }

    void fill() {
        report_["boundaries"] = {
            "U16 fillViewport ownership observation: one real D3D11 child and one 96x96 RTT.",
            "Fixed sequence: sentinel S, first A, two normal presentation frames, cleared same A, cleared changed B, shutdown.",
            "Only the control/clear uses the borrowed fixture framebuffer; colored fills call the production IRenderDevice::fillViewport.",
            "Every tested fill clears the target to distinct S first, so retained old pixels cannot pass a skipped fill.",
            "PNG central-region RGB tolerance is 2; no frame pumping while awaiting a screenshot.",
            "A failure observes this pipeline; it does not alone distinguish stack makeRef lifetime from cache-handle ownership."
        };
        report_["fill_contract"] = {{"sentinel", {18, 35, 52, 255}}, {"A", {31, 97, 163, 255}},
            {"B", {163, 47, 89, 255}}, {"rtt_width", 96}, {"rtt_height", 96},
            {"backbuffer_clear", {5, 11, 17, 255}},
            {"sample_rect", {276, 116, 328, 168}}, {"rgb_tolerance", 2},
            {"normal_frames_between_A_uses", 2}};
        fillCheckpoint("ready");
        auto* native = dynamic_cast<BgfxRenderDevice*>(&device());
        const bool ready = native && device().isInitialized()
            && bgfx::getRendererType() == bgfx::RendererType::Direct3D11
            && device().getRuntimeInfo().shaderReady && device().getDefaultSampler().isValid()
            && bgfx::isValid(native->fallbackProgram()) && bgfx::isValid(native->getBlendProgram());
        check("fill_actual_D3D11_and_core_programs", ready);
        if (!ready) return;
        const std::array<uint8_t, 3> sentinel{18, 35, 52};
        const std::array<uint8_t, 3> first{31, 97, 163};
        const std::array<uint8_t, 3> changed{163, 47, 89};
        if (!fillCapture("fill_control", nullptr, sentinel)) return;
        // Stop if the independent clear/composite positive control failed.
        // Such a failure is not evidence that production fill reached its bug.
        if (failures_ != 0) return;
        if (!fillCapture("fill_first_a", &first, first)) return;
        for (unsigned frame = 1; frame <= 2; ++frame) {
            const std::string name = "fill_present_" + std::to_string(frame);
            fillCheckpoint(name + "_before");
            device().beginFrame();
            configureFillTarget(false);
            drawFillComposite();
            device().commit_frame();
            device().advanceFrame();
            ++fillAdvances_;
            fillCheckpoint(name + "_after");
        }
        if (!fillCapture("fill_same_a", &first, first)) return;
        if (!fillCapture("fill_changed_b", &changed, changed)) return;
        check("fill_fixed_six_presentations", fillAdvances_ == 6,
            {{"observed_owner_advances", fillAdvances_}, {"expected", 6}});
        fillCheckpoint("ready_for_shutdown");
    }

    void renderer() {
        const auto beforeFont = device().captureFontState();
        auto cancelled = admit({}, "cancelled_admission");
        check("cancel_pending", device().cancelScreenshot(cancelled.ticket));
        const auto cancellation = device().takeScreenshot(cancelled.ticket);
        check("cancel_is_terminal_without_png", cancellation.status == ScreenshotStatus::Cancelled
            && cancellation.png.empty(), description(cancellation));
        check("cancel_terminal_consumed_once", device().takeScreenshot(cancelled.ticket).status == ScreenshotStatus::Unknown);

        drawDirect();
        auto native = admit({}, "native_admission");
        auto resized = admit({320, 180}, "resized_admission");
        check("requests_are_distinct", native.ticket.requestId != resized.ticket.requestId
            && native.ticket.generation == resized.ticket.generation);
        device().advanceFrame();
        native = await(native.ticket, "native");
        resized = await(resized.ticket, "resized");
        auto before = inspect("native", native, kWidth, kHeight);
        inspect("resized", resized, 320, 180);
        check("batch_captures_same_submission", native.frameId != 0 && native.frameId == resized.frameId,
            {{"native", native.frameId}, {"resized", resized.frameId}});
        check("cancelled_ticket_never_republishes", device().takeScreenshot(cancelled.ticket).status == ScreenshotStatus::Unknown);

        const auto stale = admit({}, "recovery_pending_admission");
        const auto oldRtt = rtt_;
        releaseFixtureFramebuffer();
        auto& registry = BackendRegistry::instance();
        registry.notifyDeviceLost();
        device().flagDeviceLost();
        const bool recovered = device().recoverDevice(platform_->getNativeWindowHandle(), kWidth, kHeight);
        check("explicit_recover_device", recovered);
        // If recreation failed, notifying listeners would create misleading evidence.
        if (!recovered) return;
        registry.notifyDeviceRestored();
        bgfx::setDebug(BGFX_DEBUG_NONE);
        check("actual_backend_after_recovery", bgfx::getRendererType() == bgfx::RendererType::Direct3D11
            && device().getRuntimeInfo().shaderReady);
        auto retired = device().takeScreenshot(stale.ticket);
        check("old_generation_pending_closed", retired.status == ScreenshotStatus::Cancelled
            && retired.png.empty(), description(retired));
        check("old_generation_terminal_consumed_once", device().takeScreenshot(stale.ticket).status == ScreenshotStatus::Unknown);
        check("old_rtt_invalidated", !device().getViewportTexture(oldRtt).isValid(), {{"old_viewport_id", oldRtt.id}});
        TextureSourceInfo source;
        check("manager_texture_id_restored", textures_->isValid(textureId_)
            && textures_->describeTexture(textureId_, source) && source.kind == TextureSourceKind::Color
            && source.color == std::array<uint8_t, 4>{kTextureColor[0], kTextureColor[1], kTextureColor[2], 255},
            {{"manager_id", textureId_}});
        const auto afterFont = device().captureFontState();
        check("ttf_description_retained", beforeFont.active && afterFont.active
            && beforeFont.font == afterFont.font && beforeFont.assetPath == afterFont.assetPath
            && beforeFont.pixelSize == afterFont.pixelSize,
            {{"before", fontDescription(beforeFont)}, {"after", fontDescription(afterFont)}});
        createRtt();
        check("new_rtt_has_distinct_viewport_id", rtt_.id != oldRtt.id);
        drawDirect();
        auto restored = admit({}, "restored_admission");
        check("recovery_generation_advanced", restored.ticket.generation > stale.ticket.generation
            && restored.ticket.requestId != stale.ticket.requestId,
            {{"old_generation", stale.ticket.generation}, {"new_generation", restored.ticket.generation}});
        device().advanceFrame();
        restored = await(restored.ticket, "restored");
        const auto after = inspect("restored", restored, kWidth, kHeight);
        check("restored_font_pixels_equal", before.ok && after.ok && fontPixels(before) == fontPixels(after));
        check("retired_ticket_stays_unknown_after_new_callback", device().takeScreenshot(stale.ticket).status == ScreenshotStatus::Unknown);

        // Exercise an actual failure after a live GPU existed. Invalid new
        // dimensions force core initialization to reject after the old context
        // is shut down; this is configuration failure, not OS device removal
        // or a simulated font-allocation failure.
        const auto failedRecoveryTicket = admit({}, "failed_recovery_pending_admission");
        releaseFixtureFramebuffer();
        registry.notifyDeviceLost();
        check("invalid_dimensions_recovery_fails",
            !device().recoverDevice(platform_->getNativeWindowHandle(), 0, kHeight));
        check("failed_recovery_closes_render_admission", !device().isInitialized());
        const auto rejected = device().requestScreenshot(ScreenshotOptions{});
        check("failed_recovery_rejects_new_ticket", rejected.status == ScreenshotStatus::Failed
            && !rejected.ticket, description(rejected));
        const auto failedRetirement = device().takeScreenshot(failedRecoveryTicket.ticket);
        check("failed_recovery_cancels_pending_ticket", failedRetirement.status == ScreenshotStatus::Cancelled
            && failedRetirement.png.empty(), description(failedRetirement));
        check("failed_recovery_terminal_consumed_once",
            device().takeScreenshot(failedRecoveryTicket.ticket).status == ScreenshotStatus::Unknown);
        device().beginFrame();
        device().commit_frame();
        device().advanceFrame();
        check("failed_recovery_frame_calls_return_safely", true);
    }

    void rpc() {
        // A controlled renderer frame before/after each actual Engine API call
        // makes extra normal advances observable through production frame IDs.
        drawDirect();
        auto previous = admit({}, "rpc_baseline_admission");
        device().advanceFrame();
        previous = await(previous.ticket, "rpc_baseline");
        inspect("rpc_baseline", previous, kWidth, kHeight);
        for (unsigned iteration = 0; iteration < 2; ++iteration) {
            background_ = iteration == 0 ? std::array<uint8_t, 3>{45, 27, 83}
                                         : std::array<uint8_t, 3>{26, 67, 42};
            const std::string name = iteration == 0 ? "rpc_320x180" : "rpc_native";
            const uint32_t width = iteration == 0 ? 320 : kWidth;
            const uint32_t height = iteration == 0 ? 180 : kHeight;
            auto witness = admit({}, name + "_witness_admission");
            const unsigned drawsBefore = renderCallbacks_;
            const unsigned updatesBefore = updateCallbacks_;
            vm_->resetInstructionBudget();
            const auto started = std::chrono::steady_clock::now();
            // Deliberately call the real Engine implementation. No helper thread
            // or pump can advance the GPU while this call waits for its result.
            const auto encoded = engine_->captureFrameForRpc(iteration == 0 ? 320 : 0, iteration == 0 ? 180 : 0);
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count();
            report_["rpc_calls"][name] = {{"elapsed_ms", elapsed}, {"base64_bytes", encoded.size()},
                {"render_callbacks", renderCallbacks_ - drawsBefore}, {"update_callbacks", updateCallbacks_ - updatesBefore}};
            check(name + "_updates_and_renders_once", renderCallbacks_ == drawsBefore + 1
                && updateCallbacks_ == updatesBefore + 1);
            check(name + "_returns_png", !encoded.empty(), {{"elapsed_ms", elapsed}});
            const auto rpcImage = inspectBytes(name, decodeBase64(encoded), width, height);
            witness = await(witness.ticket, name + "_witness");
            const auto witnessImage = inspect(name + "_witness", witness, kWidth, kHeight);
            check(name + "_single_present_before_return", witness.frameId != 0
                && witness.frameId == previous.frameId + 1,
                {{"previous_frame", previous.frameId}, {"witness_frame", witness.frameId}});
            if (rpcImage.ok && witnessImage.ok) {
                check(name + "_same_frame_pixels", sameScaledPixels(witnessImage, rpcImage));
            } else check(name + "_same_frame_pixels", false, {{"reason", "A required PNG was not decoded"}});
            drawDirect();
            auto following = admit({}, name + "_following_admission");
            device().advanceFrame();
            following = await(following.ticket, name + "_following");
            inspect(name + "_following", following, kWidth, kHeight);
            check(name + "_no_extra_advances_while_waiting", following.frameId != 0
                && following.frameId == witness.frameId + 1,
                {{"witness_frame", witness.frameId}, {"following_frame", following.frameId}});
            previous = std::move(following);
        }
    }

    void shutdown() {
        if (report_["scenario"] == "fill") fillCheckpoint("before_shutdown");
        releaseFixtureFramebuffer();
        engine_->shutdown();
        report_["shutdown_completed"] = true;
        if (report_["scenario"] == "fill") fillCheckpoint("after_shutdown");
        persist();
    }

private:
    void fillCheckpoint(const std::string& name) {
        report_["fill_checkpoint"] = name;
        report_["fill_checkpoints"].push_back({{"name", name}, {"owner_advances", fillAdvances_}});
        persist();
        std::cout << "U16 FILL CHECKPOINT " << name << std::endl;
    }
    void configureFillTarget(bool clearSentinel) {
        bgfx::setViewFrameBuffer(VIEW_RTT, fixtureFramebuffer_);
        device().setViewRect(VIEW_RTT, 0, 0, 96, 96);
        device().setViewClear(VIEW_RTT, clearSentinel ? BGFX_CLEAR_COLOR : BGFX_CLEAR_NONE,
            0x122334ff, 1.0f, 0);
        device().touch(VIEW_RTT);
    }
    void drawFillComposite() {
        device().setViewRect(VIEW_MAIN, 0, 0, kWidth, kHeight);
        // Distinct from the RTT sentinel: a missing RTT blit must fail the
        // control instead of matching the untouched backbuffer by accident.
        device().setViewClear(VIEW_MAIN, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x050b11ff, 1.0f, 0);
        device().touch(VIEW_MAIN);
        // The manager-owned texture is a simultaneous sampler/program positive
        // control. No new texture is allocated between the two A requests.
        device().blitTexture(VIEW_MAIN, textures_->getTextureHandle(textureId_), 64, 96, 128, 96, 255);
        device().blitViewport(rtt_, VIEW_MAIN, 256, 96, 96, 96);
    }
    bool fillCapture(const std::string& name, const std::array<uint8_t, 3>* color,
                     const std::array<uint8_t, 3>& expected) {
        fillCheckpoint(name + "_before_frame");
        device().beginFrame();
        configureFillTarget(true);
        if (color) {
            fillCheckpoint(name + "_before_production_fill");
            device().fillViewport(rtt_, (*color)[0], (*color)[1], (*color)[2], 255);
            fillCheckpoint(name + "_after_production_fill");
        }
        drawFillComposite();
        device().commit_frame();
        auto result = admit({}, name + "_admission");
        if (result.status != ScreenshotStatus::Pending || !result.ticket) return false;
        fillCheckpoint(name + "_before_advance");
        device().advanceFrame();
        ++fillAdvances_;
        fillCheckpoint(name + "_after_advance");
        result = await(result.ticket, name);
        if (result.status != ScreenshotStatus::Completed) return false;
        check(name + "_submission_distance", result.frameId != 0
            && (fillPreviousFrame_ == 0 || (result.frameId > fillPreviousFrame_
                && result.frameId - fillPreviousFrame_ == fillAdvances_ - fillPreviousAdvance_)),
            {{"frame_id", result.frameId}, {"previous_frame", fillPreviousFrame_},
             {"owner_advances_since_previous", fillAdvances_ - fillPreviousAdvance_}});
        fillPreviousFrame_ = result.frameId;
        fillPreviousAdvance_ = fillAdvances_;
        if (!result.png.empty()) writeBytes(output_ / (name + ".png"), result.png.data(), result.png.size());
        const auto image = result.png.empty() ? DecodedImage{}
            : decoder_->decode(result.png.data(), result.png.size(), kPageBytes);
        const bool decoded = image.ok && image.width == kWidth && image.height == kHeight
            && image.rgba.size() == kPageBytes;
        check(name + "_decoded_dimensions", decoded,
            {{"width", image.width}, {"height", image.height}, {"png_bytes", result.png.size()}});
        if (!decoded) return false;
        check(name + "_manager_sampler_positive", solidRegion(image, 88, 120, 160, 160, kTextureColor));
        const size_t center = (size_t(144) * kWidth + 304) * 4;
        check(name + "_central_pixels", solidRegion(image, 276, 116, 328, 168, expected),
            {{"expected_rgb", expected}, {"center_rgb", {image.rgba[center], image.rgba[center + 1], image.rgba[center + 2]}},
             {"rgb_tolerance", 2}, {"clear_before_fill", {18, 35, 52}}});
        fillCheckpoint(name + "_captured");
        return true;
    }
    void installCallback(const char* name, lua_CFunction callback, void* value) {
        auto* state = vm_->state();
        lua_rawgeti(state, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS);
        lua_pushstring(state, name);
        lua_pushlightuserdata(state, value);
        lua_pushcclosure(state, callback, 1);
        lua_rawset(state, -3);
        lua_pop(state, 1);
    }
    static int renderCallback(lua_State* state) {
        auto* self = static_cast<Probe*>(lua_touserdata(state, lua_upvalueindex(1)));
        ++self->renderCallbacks_;
        self->drawScene();
        return 0;
    }
    static int updateCallback(lua_State* state) {
        auto* self = static_cast<Probe*>(lua_touserdata(state, lua_upvalueindex(1)));
        ++self->updateCallbacks_;
        return 0;
    }
    void createRtt() {
        rtt_ = device().createRenderTarget(96, 96);
        require(bool(rtt_), "Real render target creation failed");
        const auto texture = device().getViewportTexture(rtt_);
        require(texture.isValid(), "Real render target has no texture");
        const bgfx::TextureHandle attachment{texture.idx};
        // The renderer remains the texture owner. This framebuffer exists only
        // to seed the observed RTT with a known real GPU clear operation.
        fixtureFramebuffer_ = bgfx::createFrameBuffer(1, &attachment, false);
        require(bgfx::isValid(fixtureFramebuffer_), "Fixture framebuffer creation failed");
    }
    void releaseFixtureFramebuffer() {
        if (bgfx::isValid(fixtureFramebuffer_)) {
            bgfx::destroy(fixtureFramebuffer_);
            fixtureFramebuffer_ = BGFX_INVALID_HANDLE;
        }
    }
    void drawScene() {
        const uint32_t background = uint32_t(background_[0]) << 24 | uint32_t(background_[1]) << 16
            | uint32_t(background_[2]) << 8 | 255;
        bgfx::setViewFrameBuffer(VIEW_RTT, fixtureFramebuffer_);
        device().setViewRect(VIEW_RTT, 0, 0, 96, 96);
        device().setViewClear(VIEW_RTT, BGFX_CLEAR_COLOR, 0x27b574ff, 1.0f, 0);
        device().touch(VIEW_RTT);
        device().setViewRect(VIEW_MAIN, 0, 0, kWidth, kHeight);
        device().setViewClear(VIEW_MAIN, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, background, 1.0f, 0);
        device().touch(VIEW_MAIN);
        device().blitTexture(VIEW_MAIN, textures_->getTextureHandle(textureId_), 64, 96, 128, 96, 255);
        device().blitViewport(rtt_, VIEW_MAIN, 256, 96, 96, 96);
        device().renderText(VIEW_MAIN, "GPU 15", 432, 96, 255, 255, 255, 255);
    }
    void drawDirect() { device().beginFrame(); drawScene(); device().commit_frame(); }
    ScreenshotResult admit(ScreenshotOptions options, const std::string& name) {
        auto result = device().requestScreenshot(options);
        check(name, result.status == ScreenshotStatus::Pending && bool(result.ticket), description(result));
        return result;
    }
    ScreenshotResult await(ScreenshotTicket ticket, const std::string& name) {
        const auto started = std::chrono::steady_clock::now();
        const auto deadline = started + std::chrono::seconds(3);
        auto result = device().takeScreenshot(ticket);
        while (result.status == ScreenshotStatus::Pending && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            result = device().takeScreenshot(ticket);
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count();
        report_["captures"][name] = description(result);
        report_["captures"][name]["wait_ms"] = elapsed;
        report_["captures"][name]["wait_frame_advances"] = 0;
        check(name + "_ticket_identity", result.ticket.requestId == ticket.requestId
            && result.ticket.generation == ticket.generation, description(result));
        if (result.status == ScreenshotStatus::Pending) {
            // Preserve the observed Pending result; cleanup cannot turn failure
            // into a successful screenshot or publish on a later frame.
            const bool cancelled = device().cancelScreenshot(ticket);
            const auto terminal = device().takeScreenshot(ticket);
            report_["captures"][name]["timeout_cleanup"] = {{"cancelled", cancelled}, {"terminal", description(terminal)}};
        }
        check(name + "_completed_without_pumping", result.status == ScreenshotStatus::Completed, description(result));
        if (result.status != ScreenshotStatus::Pending) {
            check(name + "_terminal_consumed_once", device().takeScreenshot(ticket).status == ScreenshotStatus::Unknown);
        }
        return result;
    }
    DecodedImage inspect(const std::string& name, const ScreenshotResult& result, uint32_t width, uint32_t height) {
        check(name + "_ticket_and_dimensions", bool(result.ticket) && result.width == width && result.height == height,
            description(result));
        return inspectBytes(name, result.png, width, height);
    }
    DecodedImage inspectBytes(const std::string& name, const Bytes& png, uint32_t width, uint32_t height) {
        if (!png.empty()) writeBytes(output_ / (name + ".png"), png.data(), png.size());
        auto decoded = png.empty() ? DecodedImage{} : decoder_->decode(png.data(), png.size(), kPageBytes);
        const bool valid = decoded.ok && decoded.width == width && decoded.height == height
            && decoded.rgba.size() == size_t(width) * height * 4;
        check(name + "_decoded_dimensions", valid,
            {{"decoded", decoded.ok}, {"width", decoded.width}, {"height", decoded.height}, {"png_bytes", png.size()}});
        if (!valid) return {};
        check(name + "_background_pixels", solidRegion(decoded, 560, 260, 608, 304, background_));
        check(name + "_manager_texture_pixels", solidRegion(decoded, 88, 120, 160, 160, kTextureColor));
        check(name + "_rtt_pixels", solidRegion(decoded, 276, 116, 328, 168, kRttColor));
        size_t whitePixels = 0;
        for (int y = 88 * height / kHeight; y < int(156 * height / kHeight); ++y) {
            for (int x = 420 * width / kWidth; x < int(624 * width / kWidth); ++x) {
                const size_t offset = (size_t(y) * width + x) * 4;
                if (decoded.rgba[offset] > 200 && decoded.rgba[offset + 1] > 200 && decoded.rgba[offset + 2] > 200) ++whitePixels;
            }
        }
        check(name + "_ttf_visible", whitePixels >= (width == kWidth ? 100u : 20u), {{"white_glyph_pixels", whitePixels}});
        checkFontCoverage(name, decoded);
        return decoded;
    }
    void checkFontCoverage(const std::string& name, const DecodedImage& image) {
        size_t expected = 0, matched = 0;
        json missing = json::array();
        for (uint32_t y = 0; y < image.height; ++y) {
            for (uint32_t x = 0; x < image.width; ++x) {
                // The screenshot resize contract is center-sampled nearest
                // neighbor. The native mask is computed exclusively from FT.
                const auto sy = (uint64_t(y) * 2 + 1) * kHeight / (uint64_t(image.height) * 2);
                const auto sx = (uint64_t(x) * 2 + 1) * kWidth / (uint64_t(image.width) * 2);
                if (fontReference_.coverage[size_t(sy) * kWidth + size_t(sx)] != 255) continue;
                ++expected;
                const size_t offset = (size_t(y) * image.width + x) * 4;
                const bool white = image.rgba[offset] >= 253 && image.rgba[offset + 1] >= 253
                    && image.rgba[offset + 2] >= 253;
                if (white) ++matched;
                else if (missing.size() < 12) missing.push_back({{"x", x}, {"y", y},
                    {"rgb", {image.rgba[offset], image.rgba[offset + 1], image.rgba[offset + 2]}}});
            }
        }
        // Every independently expected opaque pixel must be white within +/-2.
        // Fixed minimums prevent an empty/misaligned oracle from passing. This
        // is U16 glyph correctness, separate from retained U15 visibility and
        // before/after equality checks, both of which admit half-drawn glyphs.
        const size_t minimum = image.width == kWidth ? 64 : 16;
        check(name + "_u16_ttf_opaque_coverage", expected >= minimum && matched == expected,
            {{"expected_opaque_pixels", expected}, {"matched_opaque_pixels", matched},
             {"minimum_reference_pixels", minimum}, {"required_coverage", 1.0},
             {"rgb_tolerance", 2}, {"first_missing", std::move(missing)}});
    }
    static bool solidRegion(const DecodedImage& image, int left, int top, int right, int bottom,
                            const std::array<uint8_t, 3>& color) {
        for (int y = top * image.height / kHeight; y < int(bottom * image.height / kHeight); ++y) {
            for (int x = left * image.width / kWidth; x < int(right * image.width / kWidth); ++x) {
                const size_t offset = (size_t(y) * image.width + x) * 4;
                for (size_t channel = 0; channel < color.size(); ++channel) {
                    if (std::abs(int(image.rgba[offset + channel]) - int(color[channel])) > 2) return false;
                }
            }
        }
        return true;
    }
    static Bytes fontPixels(const DecodedImage& image) {
        Bytes result;
        for (size_t y = 88; y < 156; ++y) {
            const auto first = image.rgba.begin() + (y * kWidth + 420) * 4;
            result.insert(result.end(), first, first + (624 - 420) * 4);
        }
        return result;
    }
    static bool sameScaledPixels(const DecodedImage& native, const DecodedImage& resized) {
        for (uint32_t y = 0; y < resized.height; ++y) {
            for (uint32_t x = 0; x < resized.width; ++x) {
                // The public resize policy samples the center of each output
                // pixel. At 640 -> 320 this selects source columns 1, 3, 5, ...
                const auto sourceY = (uint64_t(y) * 2 + 1) * native.height / (uint64_t(resized.height) * 2);
                const auto sourceX = (uint64_t(x) * 2 + 1) * native.width / (uint64_t(resized.width) * 2);
                const size_t source = (size_t(sourceY) * native.width + size_t(sourceX)) * 4;
                const size_t target = (size_t(y) * resized.width + x) * 4;
                if (!std::equal(native.rgba.begin() + source, native.rgba.begin() + source + 3,
                    resized.rgba.begin() + target)) return false;
            }
        }
        return true;
    }
    fs::path output_;
    json& report_;
    std::unique_ptr<Engine> engine_;
    HiddenPlatform* platform_ = nullptr;
    ITextureManager* textures_ = nullptr;
    IImageDecoder* decoder_ = nullptr;
    ILuaManager* vm_ = nullptr;
    FontReference fontReference_;
    uint32_t textureId_ = 0;
    ViewportHandle rtt_;
    bgfx::FrameBufferHandle fixtureFramebuffer_ = BGFX_INVALID_HANDLE;
    std::array<uint8_t, 3> background_{18, 35, 52};
    unsigned renderCallbacks_ = 0, updateCallbacks_ = 0, failures_ = 0;
    unsigned fillAdvances_ = 0, fillPreviousAdvance_ = 0;
    uint64_t fillPreviousFrame_ = 0;
};

struct Arguments { std::wstring scenario; fs::path resources, output; };
Arguments arguments(int argc, wchar_t** argv) {
    require(argc == 7, "Usage: --scenario renderer|rpc|fill --resource-root DIR --output-dir DIR");
    Arguments result;
    for (int index = 1; index < argc; index += 2) {
        const std::wstring key = argv[index];
        if (key == L"--scenario") result.scenario = argv[index + 1];
        else if (key == L"--resource-root") result.resources = argv[index + 1];
        else if (key == L"--output-dir") result.output = argv[index + 1];
        else throw std::runtime_error("Unknown probe argument");
    }
    require(result.scenario == L"renderer" || result.scenario == L"rpc" || result.scenario == L"fill", "Invalid probe scenario");
    require(fs::is_directory(result.resources) && fs::is_directory(result.output), "Missing probe directory");
    result.resources = fs::canonical(result.resources);
    result.output = fs::canonical(result.output);
    require(!fs::exists(result.output / "result.json"), "Refusing to reuse a prior observation file");
    return result;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    fs::path output;
    json report = {{"schema", 1}, {"status", "RUNNING"}, {"pid", GetCurrentProcessId()},
        {"checks", json::array()}, {"shutdown_completed", false}};
    try {
        const auto args = arguments(argc, argv);
        output = args.output;
        fs::current_path(args.resources);
        report["scenario"] = args.scenario == L"renderer" ? "renderer" : (args.scenario == L"rpc" ? "rpc" : "fill");
        writeReport(output, report);
        SDL_SetMainReady();
        detail::g_mainThreadId = std::this_thread::get_id();
        Probe probe(output, report);
        if (args.scenario == L"renderer") probe.renderer();
        else if (args.scenario == L"rpc") probe.rpc();
        else probe.fill();
        probe.shutdown();
        report["failed_checks"] = probe.failures();
        report["status"] = probe.failures() == 0 ? "PASS" : "FAIL";
        writeReport(output, report);
        std::cout << "U15 SCREENSHOT GPU PROBE " << report["status"] << '\n';
        return probe.failures() == 0 ? 0 : 1;
    } catch (const std::exception& error) {
        report["status"] = "FAIL";
        report["error"] = error.what();
        std::cerr << "U15 SCREENSHOT GPU PROBE FAILED: " << error.what() << '\n';
        if (!output.empty()) { try { writeReport(output, report); } catch (...) {} }
        return 1;
    }
}
