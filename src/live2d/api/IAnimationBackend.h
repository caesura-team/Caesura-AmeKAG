#pragma once
#include <cstddef>
#include <string>
#include <cstdint>
#include <span>

namespace Caesura {

// Abstract animation backend for Live2D, static sprites, and other model types.
// init() must succeed before model operations are used. loadModel() returns a
// positive handle on success and a non-positive value on failure. unloadModel()
// and shutdown() must be safe to call repeatedly.

class IAnimationBackend {
public:
    virtual ~IAnimationBackend() = default;

    virtual bool init() = 0;
    virtual void shutdown() = 0;

    // Owner-thread session query; do not race init/shutdown. True means the
    // Cubism implementation is initialized, not that a model or motion loaded.
    // Static-image fallbacks return false even when their init() succeeds.
    virtual bool isCubismAvailable() const = 0;

    // Model lifecycle
    virtual int  loadModel(const std::string& path, const std::string& name) = 0;
    virtual void unloadModel(int handle) = 0;
    virtual bool isLoaded(int handle) const = 0;
    // Includes hidden models. Clear releases models except the specified
    // prepared handles, without reinitializing the backend or reusing handles.
    virtual std::size_t loadedModelCount() const = 0;
    virtual void clearModels(std::span<const int> keep = {}) = 0;

    // Rendering
    virtual void showModel(int handle, float x, float y, float scale) = 0;
    virtual void hideModel(int handle) = 0;
    virtual void setOpacity(int handle, float opacity) = 0;
    virtual void render(float dt) = 0;

    // Animation control
    // A fade value of -1 leaves the model's authored duration in place.
    virtual bool playMotion(int handle, const std::string& name,
                            float fadeInSeconds = -1, float fadeOutSeconds = -1) = 0;
    // Weight zero removes the active expression. Success requires a named expression.
    virtual bool setExpression(int handle, const std::string& name, float weight = 1.0f) = 0;
    virtual void setParameter(int handle, const std::string& param, float value) = 0;

    // Opt a loaded Cubism model into the shared VOICE bus mouth drive.
    // Defaults off. Unsupported/invalid models return false without mutation.
    // Only models with a real ParamMouthOpenY range containing 0..1 qualify.
    virtual bool setVoiceLipSync(int modelHandle, bool enabled) = 0;

    virtual const char* name() const = 0;
};

} // namespace Caesura
