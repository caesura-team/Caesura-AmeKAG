#pragma once
#ifdef CAESURA_LIVE2D

#include <Model/CubismUserModel.hpp>
#include <Motion/CubismMotionManager.hpp>

namespace Caesura {

// Non-owning access to the components owned by CubismUserModel.
struct Live2DUserModel : public Live2D::Cubism::Framework::CubismUserModel {
    auto* motionManager()     { return _motionManager; }
    auto* pose()              { return _pose; }

    // The general Cubism queue passes expression weight to all blend modes.
    Live2D::Cubism::Framework::CubismMotionManager weightedExpression;
};

} // namespace Caesura

#endif
