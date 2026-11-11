#pragma once

#include "rts/Camera3D.hpp"
#include "rts/Simulation.hpp"

#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace rts {

namespace detail {

inline bool visibleForSelection(const ProjectedPoint& point,
                                const Camera3D& camera) noexcept {
    if (!std::isfinite(point.x) || !std::isfinite(point.y) ||
        !std::isfinite(point.depth)) {
        return false;
    }
    if (camera.perspective) {
        return point.clipW >= Camera3D::nearPlane && point.clipW <= camera.farPlane();
    }
    return point.depth >= 0.0F && point.depth <= 1.0F;
}

} // namespace detail

inline void selectAtScreen(Simulation& simulation, const Camera3D& camera,
                           float x, float y) {
    constexpr float maxScreenDistanceSquared = 18.0F * 18.0F;
    float nearestDistanceSquared = maxScreenDistanceSquared;
    float nearestDepth = std::numeric_limits<float>::max();
    const Unit* nearest = nullptr;

    for (const auto& unit : simulation.units()) {
        const float unitHeight = simulation.terrainElevation(unit.position) + 8.5F;
        const WorldPoint world{unit.position.x, unitHeight, unit.position.y};
        const auto point = camera.project(world);
        if (!detail::visibleForSelection(point, camera)) {
            continue;
        }
        const float dx = point.x - x;
        const float dy = point.y - y;
        const float distanceSquared = dx * dx + dy * dy;
        const float depth = camera.viewDepth(world);
        if (distanceSquared < nearestDistanceSquared ||
            (distanceSquared == nearestDistanceSquared && depth < nearestDepth)) {
            nearestDistanceSquared = distanceSquared;
            nearestDepth = depth;
            nearest = &unit;
        }
    }

    if (nearest != nullptr) {
        simulation.selectUnit(nearest->id);
    } else {
        simulation.clearSelection();
    }
}

inline std::size_t selectInScreenRectangle(Simulation& simulation,
                                           const Camera3D& camera,
                                           Vec2 first, Vec2 second) {
    const float minimumX = std::min(first.x, second.x);
    const float maximumX = std::max(first.x, second.x);
    const float minimumY = std::min(first.y, second.y);
    const float maximumY = std::max(first.y, second.y);
    std::vector<std::uint32_t> selectedIds;

    for (const auto& unit : simulation.units()) {
        const auto point = camera.project({
            unit.position.x,
            simulation.terrainElevation(unit.position) + 8.5F,
            unit.position.y
        });
        if (detail::visibleForSelection(point, camera) &&
            point.x >= minimumX && point.x <= maximumX &&
            point.y >= minimumY && point.y <= maximumY) {
            selectedIds.push_back(unit.id);
        }
    }
    return simulation.selectUnits(selectedIds);
}

} // namespace rts
