#pragma once
#ifdef CAESURA_LIVE2D

#include <Model/CubismUserModel.hpp>

namespace Caesura {

// Non-owning access to the components owned by CubismUserModel.
struct Live2DUserModel : public Live2D::Cubism::Framework::CubismUserModel {
    auto* motionManager()     { return _motionManager; }
    auto* expressionManager() { return _expressionManager; }
    auto* pose()              { return _pose; }
};

} // namespace Caesura

#endif
