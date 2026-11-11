#pragma once

#include "rts/Simulation.hpp"

#include <vector>

namespace rts {

struct ScreenRectangle {
    float x0 = 0.0F;
    float y0 = 0.0F;
    float x1 = 0.0F;
    float y1 = 0.0F;
};

struct SceneVertex {
    float x;
    float y;
    float z;
    float red;
    float green;
    float blue;
};

std::vector<SceneVertex> buildSceneGeometry(const Simulation& simulation);
std::vector<SceneVertex> buildSelectionOverlayGeometry(
    const ScreenRectangle& rectangle);

} // namespace rts
