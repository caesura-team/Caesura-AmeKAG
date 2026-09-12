#pragma once
#include <cstddef>
#include <string>
#include <cstdint>

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
    // Includes hidden models; clearing releases all models without changing
    // backend initialization or reusing their handles. Safe when already empty.
    virtual std::size_t loadedModelCount() const = 0;
    virtual void clearModels() = 0;

    // Rendering
    virtual void showModel(int handle, float x, float y, float scale) = 0;
    virtual void hideModel(int handle) = 0;
    virtual void setOpacity(int handle, float opacity) = 0;
    virtual void render(float dt) = 0;

    // Animation control
    virtual bool playMotion(int handle, const std::string& name) = 0;
    virtual void setExpression(int handle, const std::string& name) = 0;
    virtual void setParameter(int handle, const std::string& param, float value) = 0;

    virtual const char* name() const = 0;
};

} // namespace Caesura
