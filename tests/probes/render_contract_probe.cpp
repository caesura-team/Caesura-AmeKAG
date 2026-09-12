// Explicit real-backend render contracts. Not launched by the headless suite.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif
#include "entry/Engine.h"
#include "entry/EngineConfig.h"
#include "audio/NullAudioBackend.h"
#include "di/BackendRegistry.h"
#include "di/api/ThreadAssert.h"
#include "platform/api/IPlatformBackend.h"
#include "render/BgfxRenderDevice.h"
#include "render/api/ITextureManager.h"
#include "resource/api/IImageDecoder.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <bgfx/bgfx.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace Caesura;
using json = nlohmann::json;
namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;
using Color = std::array<uint8_t, 4>;

void require(bool ok, const std::string& reason) { if (!ok) throw std::runtime_error(reason); }
std::string utf8(const fs::path& path) {
    const auto bytes = path.u8string();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}
Bytes readBytes(const fs::path& path, size_t limit = 64 * 1024 * 1024) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    require(bool(file), "Cannot open " + utf8(path));
    const auto size = file.tellg();
    require(size >= 0 && static_cast<uint64_t>(size) <= limit, "Input size outside probe limit");
    Bytes result(static_cast<size_t>(size));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(result.data()), static_cast<std::streamsize>(result.size()));
    require(bool(file), "Cannot read complete input");
    return result;
}
void writeBytes(const fs::path& path, const uint8_t* data, size_t size) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    require(bool(file), "Cannot create " + utf8(path));
    file.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    file.close();
    require(bool(file), "Cannot finish output");
}
void writeJson(const fs::path& path, const json& value) {
    const auto text = value.dump(2);
    writeBytes(path, reinterpret_cast<const uint8_t*>(text.data()), text.size());
}
std::string sha(const uint8_t* data, size_t size) {
    require(size <= ULONG_MAX, "Hash input exceeds the Windows API bound");
    struct HashOwner {
        BCRYPT_ALG_HANDLE algorithm = nullptr;
        BCRYPT_HASH_HANDLE hash = nullptr;
        Bytes object;
        ~HashOwner() { if (hash) BCryptDestroyHash(hash); if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0); }
    } owner;
    require(BCryptOpenAlgorithmProvider(&owner.algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0, "SHA256 provider failed");
    DWORD objectSize = 0, copied = 0;
    require(BCryptGetProperty(owner.algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectSize),
        sizeof(objectSize), &copied, 0) >= 0 && objectSize <= 65536, "SHA256 object size failed");
    owner.object.resize(objectSize);
    std::array<uint8_t, 32> digest{};
    require(BCryptCreateHash(owner.algorithm, &owner.hash, owner.object.data(), objectSize, nullptr, 0, 0) >= 0,
        "SHA256 creation failed");
    require(BCryptHashData(owner.hash, const_cast<PUCHAR>(data), static_cast<ULONG>(size), 0) >= 0
        && BCryptFinishHash(owner.hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0, "SHA256 failed");
    // The caller-owned object storage must outlive its hash handle.
    BCryptDestroyHash(owner.hash); owner.hash = nullptr;
    static constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (auto byte : digest) { result += hex[byte >> 4]; result += hex[byte & 15]; }
    return result;
}
std::string sha(const Bytes& bytes) { return sha(bytes.data(), bytes.size()); }
Color color(const json& value) {
    require(value.is_array() && (value.size() == 3 || value.size() == 4), "Invalid color recipe");
    Color result{0, 0, 0, 255};
    for (size_t i = 0; i < value.size(); ++i) {
        const int channel = value[i].get<int>();
        require(channel >= 0 && channel <= 255, "Color outside byte range");
        result[i] = static_cast<uint8_t>(channel);
    }
    return result;
}
uint32_t packed(Color c) { return uint32_t(c[0]) << 24 | uint32_t(c[1]) << 16 | uint32_t(c[2]) << 8 | c[3]; }
std::vector<uint32_t> codepoints(const std::string& text) {
    std::vector<uint32_t> result;
    for (size_t i = 0; i < text.size();) {
        uint8_t lead = static_cast<uint8_t>(text[i++]);
        unsigned extra = lead < 128 ? 0 : (lead >= 0xc2 && lead <= 0xdf) ? 1
            : (lead >= 0xe0 && lead <= 0xef) ? 2 : (lead >= 0xf0 && lead <= 0xf4) ? 3 : 99;
        require(extra <= 3 && i + extra <= text.size(), "Invalid UTF-8 fixture");
        uint32_t cp = extra ? lead & ((1u << (6 - extra)) - 1) : lead;
        for (unsigned n = 0; n < extra; ++n) {
            const uint8_t next = static_cast<uint8_t>(text[i++]);
            require((next & 0xc0) == 0x80, "Invalid UTF-8 continuation");
            cp = (cp << 6) | (next & 0x3f);
        }
        require(cp <= 0x10ffff && !(cp >= 0xd800 && cp <= 0xdfff)
            && (extra == 0 || cp >= (extra == 1 ? 0x80u : extra == 2 ? 0x800u : 0x10000u)), "Invalid UTF-8 codepoint");
        result.push_back(cp);
    }
    return result;
}

class HiddenPlatform final : public IPlatformBackend {
public:
    ~HiddenPlatform() override { shutdown(); }
    bool init(const char* title, int width, int height) override {
        if (window_) return true;
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) return false;
        initialized_ = true;
        const auto properties = SDL_CreateProperties();
        if (!properties) return false;
        SDL_SetStringProperty(properties, SDL_PROP_WINDOW_CREATE_TITLE_STRING, title);
        SDL_SetNumberProperty(properties, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, width);
        SDL_SetNumberProperty(properties, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, height);
        SDL_SetBooleanProperty(properties, SDL_PROP_WINDOW_CREATE_HIDDEN_BOOLEAN, true);
        // No nonclient frame: monitor DPI changes must not round the requested
        // fixture client area while the hidden window grows across displays.
        SDL_SetBooleanProperty(properties, SDL_PROP_WINDOW_CREATE_BORDERLESS_BOOLEAN, true);
        window_ = SDL_CreateWindowWithProperties(properties);
        SDL_DestroyProperties(properties);
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
        return window_ ? SDL_GetPointerProperty(SDL_GetWindowProperties(window_), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr) : nullptr;
    }
    int getWindowWidth() const override { return width_; }
    int getWindowHeight() const override { return height_; }
    void setFullscreen(bool value) override { if (window_) SDL_SetWindowFullscreen(window_, value); }
    void resizeWindow(int width, int height) override {
        require(window_ && SDL_SetWindowSize(window_, width, height), "SDL resize failed");
        require(SDL_SyncWindow(window_), "SDL resize synchronization failed");
        width_ = width; height_ = height;
        SDL_PumpEvents();
    }
    std::array<int, 2> drawableSize() const {
        int width = 0, height = 0;
        require(window_ && SDL_GetWindowSizeInPixels(window_, &width, &height), "Cannot read actual drawable size");
        return {width, height};
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
    int width_ = 640, height_ = 360;
};

// Independent CPU reference: only FreeType input/metrics and pixel-center
// bilinear sampling. No TextRenderer layout, vertices, atlas or candidate image.
class FontReference {
public:
    ~FontReference() { if (face_) FT_Done_Face(face_); if (library_) FT_Done_FreeType(library_); }
    void load(const fs::path& path, const json& contract) {
        bytes_ = readBytes(path);
        require(sha(bytes_) == contract.at("sha256").get<std::string>(), "Font input hash differs from the fixed contract");
        require(FT_Init_FreeType(&library_) == 0, "FreeType initialization failed");
        require(FT_New_Memory_Face(library_, bytes_.data(), static_cast<FT_Long>(bytes_.size()), 0, &face_) == 0,
            "FreeType face failed");
        require(FT_Set_Pixel_Sizes(face_, 0, contract.at("pixel_size").get<unsigned>()) == 0, "FreeType size failed");
        int major = 0, minor = 0, patch = 0;
        FT_Library_Version(library_, &major, &minor, &patch);
        report_ = {{"schema_version", 1}, {"font_path", contract.at("path")}, {"font_sha256", sha(bytes_)},
            {"freetype_version", std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch)},
            {"pixel_size", contract.at("pixel_size")}, {"load_flags", "FT_LOAD_DEFAULT"},
            {"render_mode", "FT_RENDER_MODE_NORMAL"}, {"sampling", "pixel-center-bilinear-zero-extended"},
            {"ascender", face_->size->metrics.ascender / 64.0}, {"references", json::array()}};
    }
    struct Raster { uint32_t codepoint = 0; unsigned index = 0; int left = 0, top = 0, w = 0, h = 0, advance = 0; Bytes alpha; };
    Raster glyph(uint32_t cp) {
        Raster value;
        value.codepoint = cp; value.index = FT_Get_Char_Index(face_, cp);
        require(cp == 32 || value.index != 0, "Reference font is missing a required codepoint");
        require(FT_Load_Glyph(face_, value.index, FT_LOAD_DEFAULT) == 0
            && FT_Render_Glyph(face_->glyph, FT_RENDER_MODE_NORMAL) == 0, "FreeType glyph render failed");
        const auto& bitmap = face_->glyph->bitmap;
        require(bitmap.pixel_mode == FT_PIXEL_MODE_GRAY || bitmap.width == 0, "Reference requires grayscale coverage");
        value.left = face_->glyph->bitmap_left; value.top = face_->glyph->bitmap_top;
        value.w = static_cast<int>(bitmap.width); value.h = static_cast<int>(bitmap.rows);
        // FT_LOAD_DEFAULT uses the fixed hinted font. Preserve its integer
        // pixel advance in the receipt instead of serializing an equal float.
        const auto advance = face_->glyph->advance.x;
        require(advance >= 0 && advance <= 256 * 64 && advance % 64 == 0,
            "Reference requires an integral hinted advance in [0, 256]");
        value.advance = static_cast<int>(advance / 64);
        value.alpha.resize(size_t(value.w) * value.h);
        for (int y = 0; y < value.h; ++y) {
            const auto* row = bitmap.buffer + (bitmap.pitch >= 0 ? y : value.h - 1 - y) * std::abs(bitmap.pitch);
            std::copy_n(row, value.w, value.alpha.data() + size_t(y) * value.w);
        }
        return value;
    }
    Bytes capture(const json& definition, const fs::path& output) {
        const int width = definition.at("width"), height = definition.at("height");
        const auto& font = definition.at("font");
        std::vector<std::vector<Raster>> runs;
        std::vector<double> advances;
        for (const auto& run : font.at("runs")) {
            std::vector<Raster> values;
            double total = 0;
            for (auto cp : codepoints(run.at("text"))) { values.push_back(glyph(cp)); total += values.back().advance; }
            runs.push_back(std::move(values)); advances.push_back(total);
        }
        Bytes coverage(size_t(width) * height, 0);
        json glyphs = json::array();
        for (size_t index = 0; index < runs.size(); ++index) {
            const auto& run = font.at("runs")[index];
            const double scale = run.at("scale"), y = run.at("y");
            double pen = run.at("x");
            if (run.contains("center_over")) {
                const auto base = run.at("center_over").get<size_t>();
                require(base < index, "Ruby base must precede its annotation");
                pen = font.at("runs")[base].at("x").get<double>()
                    + (advances[base] * font.at("runs")[base].at("scale").get<double>() - advances[index] * scale) / 2;
            }
            const double baseline = y + report_.at("ascender").get<double>() * scale;
            for (const auto& g : runs[index]) {
                const double left = pen + g.left * scale, top = baseline - g.top * scale;
                const double w = g.w * scale, h = g.h * scale;
                glyphs.push_back({{"run", index}, {"codepoint", g.codepoint}, {"glyph_index", g.index},
                    {"advance_x", g.advance}, {"bitmap_left", g.left}, {"bitmap_top", g.top},
                    {"bitmap_width", g.w}, {"bitmap_height", g.h}, {"pen_x", pen}, {"baseline", baseline},
                    {"left", left}, {"top", top}, {"width", w}, {"height", h}});
                auto sample = [&](int sx, int sy) -> double {
                    return sx >= 0 && sy >= 0 && sx < g.w && sy < g.h ? g.alpha[size_t(sy) * g.w + sx] : 0.0;
                };
                // The zero-extended FT bitmap has bilinear support half a
                // source texel outside its ink rectangle. Do not clip that
                // support to an API-specific polygon edge inclusion rule.
                const double halo = .5 * scale;
                for (int py = std::max(0, int(std::floor(top - halo))); py < std::min(height, int(std::ceil(top + h + halo))); ++py) {
                    for (int px = std::max(0, int(std::floor(left - halo))); px < std::min(width, int(std::ceil(left + w + halo))); ++px) {
                        const double sx = (px + .5 - left) / scale - .5, sy = (py + .5 - top) / scale - .5;
                        const int x0 = int(std::floor(sx)), y0 = int(std::floor(sy));
                        const double fx = sx - x0, fy = sy - y0;
                        const double alpha = (sample(x0, y0) * (1 - fx) + sample(x0 + 1, y0) * fx) * (1 - fy)
                            + (sample(x0, y0 + 1) * (1 - fx) + sample(x0 + 1, y0 + 1) * fx) * fy;
                        auto& value = coverage[size_t(py) * width + px];
                        value = static_cast<uint8_t>(std::clamp(std::lround(alpha + value * (1 - alpha / 255.0)), 0l, 255l));
                    }
                }
                pen += g.advance * scale;
            }
        }
        const std::string id = definition.at("id"), filename = id + ".coverage";
        writeBytes(output / filename, coverage.data(), coverage.size());
        size_t opaque = 0, edge = 0;
        for (auto value : coverage) { if (value == 255) ++opaque; else if (value) ++edge; }
        report_["references"].push_back({{"capture_id", id}, {"width", width}, {"height", height},
            {"coverage", filename}, {"coverage_sha256", sha(coverage)}, {"opaque_pixels", opaque},
            {"edge_pixels", edge}, {"glyphs", std::move(glyphs)}});
        writeJson(output / "font-reference.json", report_);
        return coverage;
    }
private:
    Bytes bytes_;
    FT_Library library_ = nullptr;
    FT_Face face_ = nullptr;
    json report_;
};

ShaderTestFault parseFault(const std::string& name) {
    if (name == "none") return ShaderTestFault::None;
    if (name == "fallback-missing-fragment") return ShaderTestFault::FallbackMissingFragment;
    if (name == "blend-missing-fragment") return ShaderTestFault::BlendMissingFragment;
    if (name == "softblur-missing-fragment") return ShaderTestFault::SoftBlurMissingFragment;
    if (name == "transition-missing-fragment") return ShaderTestFault::TransitionMissingFragment;
    throw std::runtime_error("Unknown shader input fault");
}

struct Options { std::string backend, caseId; fs::path manifest, resources, output; };
Options arguments(int argc, wchar_t** argv) {
    require(argc == 11, "Expected --backend --case --manifest --resource-root --output-dir");
    Options result;
    std::set<std::wstring> seen;
    for (int i = 1; i < argc; i += 2) {
        std::wstring key = argv[i]; require(seen.insert(key).second, "Duplicate argument");
        if (key == L"--backend") result.backend = utf8(fs::path(argv[i + 1]));
        else if (key == L"--case") result.caseId = utf8(fs::path(argv[i + 1]));
        else if (key == L"--manifest") result.manifest = fs::canonical(argv[i + 1]);
        else if (key == L"--resource-root") result.resources = fs::canonical(argv[i + 1]);
        else if (key == L"--output-dir") result.output = fs::canonical(argv[i + 1]);
        else throw std::runtime_error("Unknown argument");
    }
    require(result.backend == "dx11" || result.backend == "opengl", "Only explicit D3D11/OpenGL backends are accepted");
    require(fs::is_directory(result.resources) && fs::is_directory(result.output)
        && !fs::exists(result.output / "result.json"), "Output must be a fresh existing case directory");
    return result;
}

class ContractProbe {
public:
    ContractProbe(const Options& options, const json& manifest, const json& definition, json& report)
        : options_(options), manifest_(manifest), definition_(definition), report_(report) {
        for (const auto& id : definition_.at("required_checks")) {
            const auto name = id.get<std::string>();
            require(checks_.emplace(name, report_["checks"].size()).second, "Duplicate required check");
            report_["checks"].push_back({{"id", name}, {"passed", false}, {"detail", {{"state", "not-executed"}}}});
        }
    }
    ~ContractProbe() { try { shutdown(); } catch (...) {} }
    void persist() { writeJson(options_.output / "result.json", report_); }
    void checkpoint(const std::string& phase) {
        writeJson(options_.output / "checkpoint.json", {{"phase", phase}, {"owner_advances", advances_}});
        persist();
        std::cout << "RENDER CONTRACT " << options_.backend << ' ' << options_.caseId << ' ' << phase << std::endl;
    }
    void check(const std::string& id, bool passed, json detail = json::object()) {
        require(checks_.contains(id) && evaluated_.insert(id).second, "Unexpected/duplicate check: " + id);
        report_["checks"][checks_.at(id)] = {{"id", id}, {"passed", passed}, {"detail", std::move(detail)}};
        persist();
    }
    void init() {
        checkpoint("before-init");
        EngineConfig config;
        config.width = width_; config.height = height_; config.title = "Caesura U16 render contracts";
        config.editorMode = true; config.renderBackend = options_.backend.c_str();
        platform_ = new HiddenPlatform; renderer_ = new BgfxRenderDevice;
        config.platform = platform_; config.render = renderer_; config.audio = new NullAudioBackend;
        engine_ = std::make_unique<Engine>(std::move(config));
        require(renderer_->setShaderTestFault(parseFault(definition_.at("fault_requested"))),
            "Requested shader fault is unavailable; configure CAESURA_RENDER_TEST_FAULTS=ON");
        require(engine_->init(), "Actual Engine initialization failed");
        contextAlive_ = true;
        const auto actual = bgfx::getRendererType();
        report_["actual_backend"] = actual == bgfx::RendererType::Direct3D11 ? "dx11"
            : actual == bgfx::RendererType::OpenGL ? "opengl" : "noop";
        report_["backend_name"] = bgfx::getRendererName(actual);
        check("actual_backend", report_["actual_backend"] == options_.backend,
            {{"requested", options_.backend}, {"observed", report_["backend_name"]}});
        require(report_["actual_backend"] == options_.backend, "Automatic backend fallback cannot satisfy an explicit contract");
        health();
        textures_ = BackendRegistry::instance().getTextureManager();
        decoder_ = BackendRegistry::instance().getImageDecoder();
        require(textures_ && decoder_, "Required production services missing");
        const auto builds = renderer_->shaderBuildReport();
        if (definition_.at("expected") == "core_failure") {
            check("fault_applied_once", builds.injectedFaultCount == 1);
            check("core_failed", !builds.coreProgramsReady && renderer_->renderingDisabledByShaders()
                && !device().getRuntimeInfo().shaderReady, {{"public_shader_ready", device().getRuntimeInfo().shaderReady}});
        } else {
            check("core_ready", builds.coreProgramsReady && device().getRuntimeInfo().shaderReady);
            check("not_ifh", !renderer_->renderingDisabledByShaders());
            require(builds.coreProgramsReady && !renderer_->renderingDisabledByShaders(), "Positive case has a disabled core");
            if (definition_.at("expected") == "optional_degrade") check("fault_applied_once", builds.injectedFaultCount == 1);
            // Do not clear IFH in fault cases. Normal debug text is left alone;
            // required pixels and actual backend independently reject IFH/Noop.
            font_ = std::make_unique<FontReference>();
            const auto fontPath = fs::u8path(manifest_.at("font").at("path").get<std::string>());
            font_->load(options_.resources / fontPath, manifest_.at("font"));
            require(device().loadTTF(utf8(fontPath).c_str(), 28.0f), "Actual TTF load failed");
            device().setFont(static_cast<int>(FontId::TTF));
        }
        checkpoint("initialized");
    }
    IRenderDevice& device() { return *renderer_; }
    void health() {
        const auto value = renderer_->shaderBuildReport();
        json failed = json::array();
        std::istringstream names(value.failedPrograms); std::string name;
        while (names >> name) failed.push_back(name);
        report_["shader_health"] = {{"core_ready", value.coreProgramsReady},
            {"rendering_disabled", renderer_->renderingDisabledByShaders()},
            {"ifh", renderer_->renderingDisabledByShaders()}, {"fault_requested", definition_.at("fault_requested")},
            {"fault_applied_count", value.injectedFaultCount}, {"failed_programs", failed}};
    }
    uint32_t solid(Color c) {
        const auto id = textures_->createSolidTexture(c[0], c[1], c[2], c[3]);
        require(textures_->isValid(id), "Manager-owned solid texture creation failed");
        textureIds_.push_back(id); return id;
    }
    uint32_t pixels(const Bytes& bytes, int width, int height, const std::string& key) {
        require(width > 0 && height > 0 && width <= 4096 && height <= 4096 && bytes.size() == size_t(width) * height * 4,
            "Invalid texture recipe dimensions");
        const auto id = textures_->loadTextureFromRGBA(bytes.data(), static_cast<uint16_t>(width), static_cast<uint16_t>(height), key);
        require(textures_->isValid(id), "Actual RGBA upload failed");
        textureIds_.push_back(id); return id;
    }
    RenderTextureHandle texture(uint32_t id) const {
        require(textures_->isValid(id), "Texture owner no longer contains a fixture");
        return RenderTextureHandle{static_cast<uint16_t>(textures_->getTextureHandle(id))};
    }
    void draw(uint32_t id, const json& rect, uint8_t opacity = 255) {
        require(rect.is_array() && rect.size() == 4, "Invalid drawing rectangle");
        device().blitTexture(VIEW_MAIN, textures_->getTextureHandle(id), rect[0], rect[1], rect[2], rect[3], opacity);
    }
    void begin(Color background) {
        device().beginFrame();
        device().setViewRect(VIEW_MAIN, 0, 0, static_cast<uint16_t>(width_), static_cast<uint16_t>(height_));
        device().setViewClear(VIEW_MAIN, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, packed(background), 1, 0);
        device().touch(VIEW_MAIN);
    }
    void present() {
        device().commit_frame(); device().advanceFrame(); ++advances_; platform_->postFrame();
    }
    void drawFonts(const json& definition) {
        if (!definition.contains("font")) return;
        const auto& data = definition.at("font"); const auto c = color(data.at("color"));
        const auto& runs = data.at("runs");
        for (size_t i = 0; i < runs.size(); ++i) {
            const auto& run = runs[i];
            if (run.contains("center_over")) continue;
            if (i + 1 < runs.size() && runs[i + 1].value("center_over", size_t(-1)) == i) {
                device().renderRuby(VIEW_MAIN, run.at("text"), runs[i + 1].at("text"), run.at("x"), run.at("y"), c[0], c[1], c[2], c[3]);
            } else {
                device().renderText(VIEW_MAIN, run.at("text"), run.at("x"), run.at("y"), c[0], c[1], c[2], c[3], run.at("scale"));
            }
        }
    }
    void frame(const json& definition, const std::function<void()>& drawScene,
               const std::function<void()>& afterBegin = {}) {
        const std::string id = definition.at("id"); checkpoint(id + ":before-frame");
        require(definition.at("width") == width_ && definition.at("height") == height_, "Capture requires a real resize first");
        Bytes coverage;
        if (definition.contains("font")) coverage = font_->capture(definition, options_.output);
        begin(color(definition.at("background")));
        if (afterBegin) afterBegin();
        drawScene(); drawFonts(definition);
        device().commit_frame();
        auto result = device().requestScreenshot(ScreenshotOptions{});
        require(result.status == ScreenshotStatus::Pending && bool(result.ticket), "Actual screenshot admission failed");
        const auto ticket = result.ticket;
        device().advanceFrame(); ++advances_; platform_->postFrame();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        do {
            result = device().takeScreenshot(ticket);
            if (result.status != ScreenshotStatus::Pending) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < deadline);
        if (result.status == ScreenshotStatus::Pending) { device().cancelScreenshot(ticket); device().takeScreenshot(ticket); }
        require(result.status == ScreenshotStatus::Completed && !result.png.empty(), "Screenshot did not complete within the bounded wait");
        const auto decoded = decoder_->decode(result.png.data(), result.png.size(), size_t(width_) * height_ * 4);
        require(decoded.ok && decoded.width == width_ && decoded.height == height_
            && decoded.rgba.size() == size_t(width_) * height_ * 4, "Production PNG decode/dimensions failed");
        const bool ticketOk = result.ticket.requestId == ticket.requestId && result.ticket.generation == ticket.generation
            && result.frameId > previousFrame_ && (!generation_ || result.ticket.generation == generation_);
        previousFrame_ = result.frameId; generation_ = result.ticket.generation;
        const std::string png = id + ".png", raw = id + ".rgba";
        writeBytes(options_.output / png, result.png.data(), result.png.size());
        writeBytes(options_.output / raw, decoded.rgba.data(), decoded.rgba.size());
        report_["captures"].push_back({{"id", id}, {"png", png}, {"rgba", raw}, {"width", width_}, {"height", height_},
            {"png_sha256", sha(result.png)}, {"rgba_sha256", sha(decoded.rgba)},
            {"ticket", {{"request_id", ticket.requestId}, {"generation", ticket.generation}, {"frame_id", result.frameId}, {"status", "Completed"}}}});
        // The independent driver owns the mathematical pixel acceptance. This
        // named check proves only actual ticket/decode/output completion.
        check("capture:" + id, ticketOk, {{"owner_advances", advances_}, {"wait_frame_advances", 0}});
        captured_[id] = decoded.rgba;
        checkpoint(id + ":captured");
    }
    void run();
    void shutdown() {
        if (shutdown_) return;
        shutdown_ = true;
        checkpoint("before-shutdown");
        if (contextAlive_) {
            device().clearPostFx();
            releaseFixture();
            for (auto target : targets_) device().destroyRenderTarget(target);
            if (textures_) for (auto id : textureIds_) textures_->destroyTexture(id);
        }
        if (engine_) engine_->shutdown();
        contextAlive_ = false;
        report_["shutdown_completed"] = true;
        check("shutdown", true);
        checkpoint("after-shutdown");
    }
    bool passed() const {
        return std::all_of(report_.at("checks").begin(), report_.at("checks").end(), [](const json& item) { return item.at("passed").get<bool>(); });
    }
private:
    void releaseFixture() { if (bgfx::isValid(fixture_)) { bgfx::destroy(fixture_); fixture_ = BGFX_INVALID_HANDLE; } }
    void makeTarget(int w, int h) {
        target_ = device().createRenderTarget(w, h); require(bool(target_), "Actual RTT creation failed");
        targets_.push_back(target_);
        const bgfx::TextureHandle handle{device().getViewportTexture(target_).idx};
        require(bgfx::isValid(handle), "RTT attachment missing");
        fixture_ = bgfx::createFrameBuffer(1, &handle, false);
        require(bgfx::isValid(fixture_), "Borrowed control framebuffer creation failed");
    }
    void targetClear(Color c, bool enabled = true) {
        bgfx::setViewFrameBuffer(VIEW_RTT, fixture_);
        device().setViewRect(VIEW_RTT, 0, 0, 96, 96);
        device().setViewClear(VIEW_RTT, enabled ? BGFX_CLEAR_COLOR : BGFX_CLEAR_NONE, packed(c), 1, 0);
        device().touch(VIEW_RTT);
    }
    void drawTarget(const json& rect) { device().blitViewport(target_, VIEW_MAIN, rect[0], rect[1], rect[2], rect[3]); }
    void resize(int width, int height) {
        platform_->resizeWindow(width, height);
        const auto size = platform_->drawableSize();
        require(size[0] == width && size[1] == height,
            "Actual drawable size " + std::to_string(size[0]) + "x" + std::to_string(size[1])
            + " differs from requested " + std::to_string(width) + "x" + std::to_string(height));
        device().setPresentSize(width, height); device().resize(width, height);
        width_ = width; height_ = height;
    }
    void fillCase();
    void alphaCase();
    void transitionCase();
    void lutCase();
    void optionalCase();
    void coreFailure();
    std::map<std::string, uint32_t> makeNamedTextures(const json& recipe) {
        std::map<std::string, uint32_t> ids;
        for (const auto& [name, value] : recipe.at("textures").items()) ids[name] = solid(color(value));
        return ids;
    }
    void namedDraws(const json& recipe, const std::map<std::string, uint32_t>& ids) {
        for (const auto& item : recipe.at("draws")) draw(ids.at(item.at("texture")), item.at("rect_xywh"), static_cast<uint8_t>(item.value("opacity", 255)));
    }
    const Options& options_;
    const json& manifest_;
    const json& definition_;
    json& report_;
    std::map<std::string, size_t> checks_;
    std::set<std::string> evaluated_;
    std::unique_ptr<Engine> engine_;
    HiddenPlatform* platform_ = nullptr;
    BgfxRenderDevice* renderer_ = nullptr;
    ITextureManager* textures_ = nullptr;
    IImageDecoder* decoder_ = nullptr;
    std::unique_ptr<FontReference> font_;
    std::vector<uint32_t> textureIds_;
    std::vector<ViewportHandle> targets_;
    ViewportHandle target_;
    bgfx::FrameBufferHandle fixture_ = BGFX_INVALID_HANDLE;
    std::map<std::string, Bytes> captured_;
    int width_ = 640, height_ = 360;
    uint64_t advances_ = 0, previousFrame_ = 0, generation_ = 0;
    bool contextAlive_ = false, shutdown_ = false;
};

void ContractProbe::alphaCase() {
    const auto& recipe = definition_.at("recipe");
    const auto& alpha = recipe.at("alpha_texture");
    const int w = alpha.at("width"), h = alpha.at("height");
    const auto rgb = color(alpha.at("rgb"));
    Bytes bitmap(size_t(w) * h * 4);
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
        const int q = (y >= h / 2 ? 2 : 0) + (x >= w / 2 ? 1 : 0);
        const size_t offset = (size_t(y) * w + x) * 4;
        std::copy_n(rgb.data(), 3, bitmap.data() + offset);
        bitmap[offset + 3] = alpha.at("quadrant_alpha")[q].get<uint8_t>();
    }
    const auto alphaId = pixels(bitmap, w, h, "render-contract-alpha");
    const auto opaqueId = solid(color(recipe.at("opacity_texture").at("rgb")));
    const auto named = makeNamedTextures(recipe);
    for (const auto& capture : definition_.at("captures")) {
        const std::string id = capture.at("id");
        frame(capture, [&] {
            if (id == "alpha-texture") draw(alphaId, alpha.at("rect_xywh"));
            else {
                const bool batch = id == "opacity-batched" || id == "multitexture-batched";
                if (batch) device().beginBatch();
                if (id.starts_with("opacity")) {
                    for (const auto& item : recipe.at("opacity_texture").at("draws"))
                        draw(opaqueId, item.at("rect_xywh"), item.at("opacity").get<uint8_t>());
                } else namedDraws(recipe, named);
                if (batch) device().flushBatch();
            }
        });
    }
}

void ContractProbe::fillCase() {
    const auto& recipe = definition_.at("recipe");
    const auto sentinel = color(recipe.at("sentinel")), a = color(recipe.at("A")), b = color(recipe.at("B"));
    makeTarget(96, 96);
    const auto& orientation = recipe.at("orientation");
    const auto topColor = solid(color(orientation.at("top")));
    const auto bottomColor = solid(color(orientation.at("bottom")));
    bool rebuilt = false, resized = false, returned = false;
    for (const auto& capture : definition_.at("captures")) {
        const std::string id = capture.at("id");
        if (id == "fill-same-a") {
            for (unsigned i = 0; i < 2; ++i) {
                begin(color(capture.at("background"))); targetClear(sentinel, false);
                drawTarget(recipe.at("rect_xywh")); present();
            }
        }
        if (id == "resized") { resize(800, 450); resized = true; }
        if (id == "returned") { resize(640, 360); returned = true; }
        if (id == "recreated") {
            const auto old = target_;
            releaseFixture(); device().destroyRenderTarget(old);
            const bool invalid = !device().getViewportTexture(old).isValid();
            makeTarget(96, 96); rebuilt = invalid && target_ != old;
        }
        frame(capture, [&] {
            targetClear(sentinel);
            if (id == "rtt-orientation") {
                // Real draws into the same RTT used by fill/recreation. The
                // logical canvas maps to the 96x96 target through VIEW_RTT.
                const auto drawHalf = [&](uint32_t owner, const json& rect) {
                    device().blitTexture(VIEW_RTT, textures_->getTextureHandle(owner),
                        rect[0], rect[1], rect[2], rect[3], 255);
                };
                drawHalf(topColor, orientation.at("top_rect_xywh"));
                drawHalf(bottomColor, orientation.at("bottom_rect_xywh"));
            } else if (id != "fill-control") {
                const auto c = id == "fill-first-a" || id == "fill-same-a" || id == "recreated" ? a : b;
                device().fillViewport(target_, c[0], c[1], c[2], c[3]);
            }
            drawTarget(id == "resized" ? recipe.at("resized_rect_xywh") : recipe.at("rect_xywh"));
        });
    }
    check("fill_cache_reuse", captured_.at("fill-first-a") == captured_.at("fill-same-a"),
        {{"normal_frames_between_A_uses", 2}});
    check("resize_roundtrip", resized && returned && platform_->drawableSize() == std::array<int, 2>{640, 360});
    check("rtt_recreated", rebuilt);
}

void ContractProbe::transitionCase() {
    const auto& recipe = definition_.at("recipe");
    const auto from = solid(color(recipe.at("from"))), to = solid(color(recipe.at("to")));
    const auto& rule = recipe.at("rule");
    const int w = rule.at("width"), h = rule.at("height");
    Bytes data(size_t(w) * h * 4);
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
        const auto value = rule.at(x < w / 2 ? "left" : "right").get<uint8_t>();
        const size_t offset = (size_t(y) * w + x) * 4;
        data[offset] = data[offset + 1] = data[offset + 2] = value; data[offset + 3] = 255;
    }
    const auto ruleId = pixels(data, w, h, "render-contract-transition-rule");
    for (const auto& capture : definition_.at("captures")) frame(capture, [&] {
        const auto& params = capture.at("parameters");
        device().submitTransition(VIEW_MAIN, texture(from), texture(to), texture(ruleId),
            params.at("method"), params.at("progress"));
    });
}

void ContractProbe::lutCase() {
    const auto& recipe = definition_.at("recipe");
    std::vector<uint32_t> patches;
    for (const auto& patch : recipe.at("patches")) patches.push_back(solid(color(patch.at("rgb"))));
    std::map<std::pair<int, std::string>, uint32_t> cubes;
    for (int n : {16, 64}) for (const std::string transform : {"identity", "swap_rb"}) {
        Bytes data(size_t(n) * n * n * 4);
        for (int b = 0; b < n; ++b) for (int g = 0; g < n; ++g) for (int r = 0; r < n; ++r) {
            const size_t offset = (size_t(g) * n * n + b * n + r) * 4;
            auto value = [n](int index) { return static_cast<uint8_t>(std::lround(index * 255.0 / (n - 1))); };
            data[offset] = value(transform == "identity" ? r : b); data[offset + 1] = value(g);
            data[offset + 2] = value(transform == "identity" ? b : r); data[offset + 3] = 255;
        }
        cubes[{n, transform}] = pixels(data, n * n, n, "render-contract-lut-" + std::to_string(n) + "-" + transform);
    }
    for (const auto& capture : definition_.at("captures")) {
        device().clearPostFx();
        const std::string captureId = capture.at("id");
        if (captureId.starts_with("postfx-")) {
            const auto& lifecycle = recipe.at("postfx_lifecycle");
            const auto& scenarios = lifecycle.at("scenarios");
            const auto scenario = std::find_if(scenarios.begin(), scenarios.end(), [&](const json& value) {
                return value.at("capture_id") == captureId;
            });
            require(scenario != scenarios.end(), "Unknown lifecycle observation");
            require(captured_.contains("borrowed-after-clear"), "Lifecycle extension must follow all v2 observations");
            const int width = lifecycle.at("canvas").at(0), height = lifecycle.at("canvas").at(1);
            if (width_ != width || height_ != height) resize(width, height);
            const std::string mode = scenario->at("mode");
            require(mode == "destroy-last" || mode == "invalid-only" || mode == "clear-after-begin"
                || mode == "swap-invalid-tail", "Unknown lifecycle operation sequence");
            IRenderDevice::PostFxHandle valid = 0;
            bool destroyed = false, clearedAfterBegin = false;
            json requests = json::array();
            if (mode != "invalid-only") {
                const auto& definition = lifecycle.at("valid_lut");
                const int size = definition.at("lut_size");
                const std::string transform = definition.at("transform");
                IRenderDevice::PostFxParams params;
                params.strength = definition.at("strength");
                params.lutSize = static_cast<uint8_t>(size);
                params.lutTexture = texture(cubes.at({size, transform}));
                valid = device().createPostFx(IRenderDevice::PostFxKind::Lut3D, params);
                require(valid != 0, "Lifecycle positive setup requires a real valid LUT stage");
                if (mode == "destroy-last") {
                    device().destroyPostFx(valid);
                    destroyed = true;
                }
            }
            for (const auto& invalid : scenario->at("invalid_requests")) {
                IRenderDevice::PostFxParams params;
                params.strength = invalid.at("strength");
                params.lutSize = invalid.at("lut_size").get<uint8_t>();
                const std::string source = invalid.at("texture");
                require(source == "swap-16" || source == "invalid", "Unknown invalid-input texture selector");
                params.lutTexture = source == "swap-16" ? texture(cubes.at({16, "swap_rb"})) : RenderTextureHandle{};
                const auto handle = device().createPostFx(IRenderDevice::PostFxKind::Lut3D, params);
                requests.push_back({{"label", invalid.at("label")}, {"handle", handle},
                    {"result", handle == 0 ? "rejected" : "accepted-requires-identity-pixels"}});
            }
            const bool activeBeforeBegin = device().isPostFxActive();
            std::function<void()> afterBegin;
            if (mode == "clear-after-begin") {
                require(scenario->at("after_begin") == "clear-postfx", "Mid-frame clear hook changed");
                afterBegin = [&] {
                    checkpoint(captureId + ":after-begin-before-clear");
                    device().clearPostFx();
                    clearedAfterBegin = true;
                };
            } else require(scenario->at("after_begin") == "none", "Unexpected lifecycle frame hook");
            frame(capture, [&] {
                for (size_t i = 0; i < patches.size(); ++i) draw(patches[i], recipe.at("patches")[i].at("rect_xywh"));
            }, afterBegin);
            const bool completed = report_["checks"][checks_.at("capture:" + captureId)]["passed"].get<bool>();
            const bool sequence = (mode != "destroy-last" || destroyed)
                && (mode != "clear-after-begin" || clearedAfterBegin)
                && requests.size() == scenario->at("invalid_requests").size();
            check(scenario->at("check_id"), completed && sequence,
                {{"operation", mode}, {"valid_stage_handle", valid}, {"invalid_requests", requests},
                 {"before_begin", scenario->at("before_begin")}, {"after_begin", scenario->at("after_begin")},
                 {"observed_active_before_begin", activeBeforeBegin}, {"observed_active_after_frame", device().isPostFxActive()},
                 {"completed_ticket", completed}, {"pixel_acceptance", "Independent baseline/swap reference; active flags are observations only"}});
            continue;
        }
        if (capture.at("id") == "borrowed-after-clear") {
            const auto& observation = recipe.at("borrowed_after_clear");
            require(captured_.contains("cleared") && !device().isPostFxActive(),
                "Borrowed atlas observation must follow the original cleared capture");
            const auto& canvas = observation.at("canvas");
            resize(canvas.at(0), canvas.at(1));
            frame(capture, [&] {
                // These are the original manager-owned LUTs from above. Do not
                // create/reload a texture to make this post-clear use succeed.
                for (const auto& atlas : observation.at("atlases")) {
                    const int size = atlas.at("lut_size");
                    const std::string transform = atlas.at("transform");
                    const auto id = cubes.at({size, transform});
                    const auto& rect = atlas.at("rect_xywh");
                    require(rect.at(2) == size * size && rect.at(3) == size,
                        "Borrowed atlas must be drawn at one source texel per output pixel");
                    draw(id, rect);
                }
            });
            continue;
        }
        const auto& params = capture.at("parameters"); const int size = params.at("lut_size");
        if (size) {
            IRenderDevice::PostFxParams effect;
            effect.strength = params.at("strength");
            effect.lutTexture = texture(cubes.at({size, params.at("transform").get<std::string>()}));
            effect.lutSize = static_cast<uint8_t>(size);
            require(device().createPostFx(IRenderDevice::PostFxKind::Lut3D, effect) != 0, "Actual LUT stage creation failed");
        }
        frame(capture, [&] {
            for (size_t i = 0; i < patches.size(); ++i) draw(patches[i], recipe.at("patches")[i].at("rect_xywh"));
        });
    }
    device().clearPostFx();
    json owners = json::array();
    for (const auto& atlas : recipe.at("borrowed_after_clear").at("atlases")) {
        const int size = atlas.at("lut_size");
        const std::string transform = atlas.at("transform");
        owners.push_back({{"lut_size", size}, {"transform", transform}, {"manager_id", cubes.at({size, transform})}});
    }
    const bool completed = captured_.contains("borrowed-after-clear")
        && report_["checks"][checks_.at("capture:borrowed-after-clear")]["passed"].get<bool>();
    check("lut_borrowed_texture_alive", completed && owners.size() == 4 && !device().isPostFxActive(),
        {{"observation", "Original LUT owner IDs reused by real GPU draws after clear; independent Python atlas pixels decide liveness"},
         {"capture_id", "borrowed-after-clear"}, {"completed_ticket", completed}, {"owners", owners}});
}

void ContractProbe::optionalCase() {
    const auto& recipe = definition_.at("recipe");
    const auto ids = makeNamedTextures(recipe);
    bool degraded = false;
    for (const auto& capture : definition_.at("captures")) {
        const bool apply = capture.at("id") == "degraded";
        if (apply && options_.caseId == "shader-optional-softblur") {
            IRenderDevice::PostFxParams params; params.strength = 1; params.radius = 2;
            const bool supported = device().isPostFxSupported(IRenderDevice::PostFxKind::SoftBlur);
            const auto handle = device().createPostFx(IRenderDevice::PostFxKind::SoftBlur, params);
            degraded = !supported && handle == 0;
        }
        frame(capture, [&] {
            namedDraws(recipe, ids);
            if (apply && options_.caseId == "shader-optional-transition") {
                device().submitTransition(VIEW_MAIN, texture(ids.at("A")), texture(ids.at("B")), {}, 0, .5f);
                degraded = !bgfx::isValid(renderer_->getTransitionProgram());
            }
        });
    }
    check("optional_degraded", degraded, {{"failed_programs", report_.at("shader_health").at("failed_programs")}});
    check("no_invalid_submit", !device().consumeDeviceLost(),
        {{"observation", "missing-program guard exercised; wrapper additionally rejects resource diagnostics"}});
}

void ContractProbe::coreFailure() {
    const auto id = solid({90, 120, 150, 255});
    device().beginFrame(); device().touch(VIEW_MAIN);
    draw(id, json::array({32, 48, 64, 64}));
    device().submitBlend(VIEW_MAIN, texture(id), texture(id), 0, 1, 1, 1);
    device().commit_frame();
    auto result = device().requestScreenshot(ScreenshotOptions{});
    check("capture_rejected", result.status == ScreenshotStatus::Failed && !result.ticket && result.png.empty(),
        {{"error", result.error}, {"request_id", result.ticket.requestId}});
    if (result.ticket) { device().cancelScreenshot(result.ticket); device().takeScreenshot(result.ticket); }
    device().advanceFrame(); ++advances_; platform_->postFrame();
    check("no_invalid_submit", !device().consumeDeviceLost(),
        {{"observation", "both core draw routes called under the missing-input failure; no device-lost signal"}});
}

void ContractProbe::run() {
    if (definition_.at("expected") == "core_failure") coreFailure();
    else if (options_.caseId == "text-cjk-ruby") {
        for (const auto& capture : definition_.at("captures")) frame(capture, [] {});
    } else if (options_.caseId == "alpha-layers-batch") alphaCase();
    else if (options_.caseId == "rtt-fill-resize") fillCase();
    else if (options_.caseId == "transition") transitionCase();
    else if (options_.caseId == "lut3d") lutCase();
    else if (definition_.at("expected") == "optional_degrade") optionalCase();
    else throw std::runtime_error("Unknown render contract case");
    health();
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    Options options;
    json report, manifest;
    std::unique_ptr<ContractProbe> probe;
    try {
        options = arguments(argc, argv);
        const auto bytes = readBytes(options.manifest, 1024 * 1024);
        manifest = json::parse(bytes);
        require(manifest.at("schema_version") == 1 && manifest.at("suite_id") == "u16-render-contracts-v4", "Unsupported manifest");
        const auto& cases = manifest.at("cases");
        const auto found = std::find_if(cases.begin(), cases.end(), [&](const json& value) { return value.at("id") == options.caseId; });
        require(found != cases.end(), "Case absent from manifest");
        wchar_t module[32768]{};
        const DWORD length = GetModuleFileNameW(nullptr, module, 32768);
        require(length > 0 && length < 32768, "Cannot identify actual executable");
        report = {{"schema_version", 1}, {"suite_id", manifest.at("suite_id")}, {"case_id", options.caseId},
            {"requested_backend", options.backend}, {"actual_backend", "noop"}, {"backend_name", "uninitialized"},
            {"binary_path", utf8(fs::canonical(module))}, {"pid", GetCurrentProcessId()}, {"expected", found->at("expected")},
            {"status", "FAIL"}, {"shutdown_completed", false},
            {"shader_health", {{"core_ready", false}, {"rendering_disabled", false}, {"ifh", false},
                {"fault_requested", found->at("fault_requested")}, {"fault_applied_count", 0}, {"failed_programs", json::array()}}},
            {"checks", json::array()}, {"captures", json::array()}};
        fs::current_path(options.resources);
        SDL_SetMainReady(); detail::g_mainThreadId = std::this_thread::get_id();
        probe = std::make_unique<ContractProbe>(options, manifest, *found, report);
        probe->init(); probe->run(); probe->shutdown();
        report["status"] = probe->passed()
            ? (found->at("expected") == "core_failure" ? "EXPECTED_FAILURE_OBSERVED" : "PASS") : "FAIL";
        probe->persist();
        const bool passed = probe->passed();
        // References to manifest/case remain valid until the probe is destroyed.
        probe.reset();
        return passed ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "RENDER CONTRACT FAILURE: " << error.what() << std::endl;
        if (probe) { try { probe->shutdown(); } catch (...) {} probe.reset(); }
        if (!options.output.empty()) {
            try { writeJson(options.output / "diagnostic.json", {{"error", error.what()}}); } catch (...) {}
            try { if (report.is_object()) { report["status"] = "FAIL"; writeJson(options.output / "result.json", report); } } catch (...) {}
        }
        return 1;
    }
}
