#include "rts/SceneGeometry.hpp"

#include "rts/Camera3D.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace rts {
namespace {

struct Color {
    float red;
    float green;
    float blue;
};

Color shade(Color color, float factor) {
    return {color.red * factor, color.green * factor, color.blue * factor};
}

void addTriangle(std::vector<SceneVertex>& vertices, WorldPoint a, WorldPoint b,
                 WorldPoint c, Color color) {
    vertices.push_back({a.x, a.y, a.z, color.red, color.green, color.blue});
    vertices.push_back({b.x, b.y, b.z, color.red, color.green, color.blue});
    vertices.push_back({c.x, c.y, c.z, color.red, color.green, color.blue});
}

void addQuad(std::vector<SceneVertex>& vertices, WorldPoint a, WorldPoint b,
             WorldPoint c, WorldPoint d, Color color) {
    addTriangle(vertices, a, b, c, color);
    addTriangle(vertices, a, c, d, color);
}

void addBox(std::vector<SceneVertex>& vertices, float x, float z, float width,
            float depth, float base, float height, Color color) {
    const float top = base + height;
    addQuad(vertices, {x, base, z}, {x + width, base, z},
            {x + width, top, z}, {x, top, z}, shade(color, 0.68F));
    addQuad(vertices, {x + width, base, z}, {x + width, base, z + depth},
            {x + width, top, z + depth}, {x + width, top, z}, shade(color, 0.84F));
    addQuad(vertices, {x + width, base, z + depth}, {x, base, z + depth},
            {x, top, z + depth}, {x + width, top, z + depth}, shade(color, 0.92F));
    addQuad(vertices, {x, base, z + depth}, {x, base, z},
            {x, top, z}, {x, top, z + depth}, shade(color, 0.74F));
    addQuad(vertices, {x, top, z}, {x + width, top, z},
            {x + width, top, z + depth}, {x, top, z + depth}, color);
}

void addLine(std::vector<SceneVertex>& vertices, Vec2 start, Vec2 end,
             float width, float startHeight, float endHeight, Color color) {
    const float dx = end.x - start.x;
    const float dz = end.y - start.y;
    const float length = std::sqrt(dx * dx + dz * dz);
    if (length < 0.001F) {
        return;
    }
    const float halfX = -dz * width / (2.0F * length);
    const float halfZ = dx * width / (2.0F * length);
    addQuad(vertices,
            {start.x + halfX, startHeight, start.y + halfZ},
            {end.x + halfX, endHeight, end.y + halfZ},
            {end.x - halfX, endHeight, end.y - halfZ},
            {start.x - halfX, startHeight, start.y - halfZ}, color);
}

void addTerrainLine(std::vector<SceneVertex>& vertices,
                    const Simulation& simulation, Vec2 start, Vec2 end,
                    float width, Color color) {
    const float dx = end.x - start.x;
    const float dz = end.y - start.y;
    const float length = std::sqrt(dx * dx + dz * dz);
    const int segments = std::max(1, static_cast<int>(
        std::ceil(length / Simulation::kTerrainCellSize)));
    Vec2 previous = start;
    for (int segment = 1; segment <= segments; ++segment) {
        const float fraction = static_cast<float>(segment) /
                               static_cast<float>(segments);
        const Vec2 next{start.x + dx * fraction, start.y + dz * fraction};
        addLine(vertices, previous, next, width,
                simulation.terrainElevation(previous) + 0.35F,
                simulation.terrainElevation(next) + 0.35F, color);
        previous = next;
    }
}

void addRing(std::vector<SceneVertex>& vertices, Vec2 center, float radius,
             float width, float height, Color color) {
    constexpr int segments = 20;
    constexpr float tau = 6.2831853071795864769F;
    for (int i = 0; i < segments; ++i) {
        const float a = tau * static_cast<float>(i) / static_cast<float>(segments);
        const float b = tau * static_cast<float>(i + 1) / static_cast<float>(segments);
        const Vec2 p0{center.x + std::cos(a) * radius,
                      center.y + std::sin(a) * radius};
        const Vec2 p1{center.x + std::cos(b) * radius,
                      center.y + std::sin(b) * radius};
        const Vec2 q0{center.x + std::cos(a) * (radius + width),
                      center.y + std::sin(a) * (radius + width)};
        const Vec2 q1{center.x + std::cos(b) * (radius + width),
                      center.y + std::sin(b) * (radius + width)};
        addQuad(vertices, {p0.x, height, p0.y}, {p1.x, height, p1.y},
                {q1.x, height, q1.y}, {q0.x, height, q0.y}, color);
    }
}

} // namespace

std::vector<SceneVertex> buildSceneGeometry(const Simulation& simulation) {
    std::vector<SceneVertex> vertices;
    vertices.reserve(100000);
    constexpr float mapWidth = Simulation::kMapWidth;
    constexpr float mapHeight = Simulation::kMapHeight;

    addQuad(vertices, {0.0F, -2.0F, 0.0F}, {mapWidth, -2.0F, 0.0F},
            {mapWidth, -2.0F, mapHeight}, {0.0F, -2.0F, mapHeight},
            {0.16F, 0.2F, 0.18F});

    for (int row = 0; row < Simulation::kTerrainRows; ++row) {
        for (int column = 0; column < Simulation::kTerrainColumns; ++column) {
            const auto variation = static_cast<unsigned>(row * 73 + column * 97);
            const float shadeValue =
                0.23F + static_cast<float>((variation * 37U) % 17U) * 0.003F;
            const float x = static_cast<float>(column) *
                            Simulation::kTerrainCellSize;
            const float z = static_cast<float>(row) *
                            Simulation::kTerrainCellSize;
            const float nextX = x + Simulation::kTerrainCellSize;
            const float nextZ = z + Simulation::kTerrainCellSize;
            const WorldPoint topLeft{x, simulation.terrainElevation({x, z}), z};
            const WorldPoint topRight{
                nextX, simulation.terrainElevation({nextX, z}), z};
            const WorldPoint bottomRight{
                nextX, simulation.terrainElevation({nextX, nextZ}), nextZ};
            const WorldPoint bottomLeft{
                x, simulation.terrainElevation({x, nextZ}), nextZ};
            const float relief = std::clamp(
                (simulation.terrainElevation({x + Simulation::kTerrainCellSize * 0.5F,
                                              z + Simulation::kTerrainCellSize * 0.5F}) -
                 3.0F) / 40.0F, 0.0F, 1.0F);
            addQuad(vertices, topLeft, topRight, bottomRight, bottomLeft,
                    {shadeValue + relief * 0.07F,
                     shadeValue + 0.045F + relief * 0.08F,
                     shadeValue - 0.01F + relief * 0.015F});
        }
    }

    for (const auto& obstacle : simulation.terrainObstacles()) {
        const Vec2 center{
            (obstacle.minimum.x + obstacle.maximum.x) * 0.5F,
            (obstacle.minimum.y + obstacle.maximum.y) * 0.5F
        };
        addBox(vertices, obstacle.minimum.x, obstacle.minimum.y,
               obstacle.maximum.x - obstacle.minimum.x,
               obstacle.maximum.y - obstacle.minimum.y,
               simulation.terrainElevation(center), obstacle.height,
               {0.25F, 0.34F, 0.25F});
    }

    const Color grid{0.31F, 0.39F, 0.31F};
    for (int x = 0; x <= static_cast<int>(mapWidth); x += 100) {
        addTerrainLine(vertices, simulation,
                       {static_cast<float>(x), 0.0F},
                       {static_cast<float>(x), mapHeight}, 1.0F, grid);
    }
    for (int z = 0; z <= static_cast<int>(mapHeight); z += 100) {
        addTerrainLine(vertices, simulation,
                       {0.0F, static_cast<float>(z)},
                       {mapWidth, static_cast<float>(z)}, 1.0F, grid);
    }

    const Color unitColor{0.3F, 0.66F, 0.83F};
    const Color selectedColor{1.0F, 0.76F, 0.23F};
    const Color orderColor{0.9F, 0.84F, 0.49F};
    for (const auto& unit : simulation.units()) {
        const float x = unit.position.x;
        const float z = unit.position.y;
        const float ground = simulation.terrainElevation(unit.position);
        if (unit.selected) {
            Vec2 routeStart = unit.position;
            for (std::size_t waypoint = unit.nextWaypoint;
                 waypoint < unit.path.size(); ++waypoint) {
                const auto routeEnd = unit.path[waypoint];
                addLine(vertices, routeStart, routeEnd, 1.5F,
                        simulation.terrainElevation(routeStart) + 0.5F,
                        simulation.terrainElevation(routeEnd) + 0.5F,
                        orderColor);
                routeStart = unit.path[waypoint];
            }
            addRing(vertices, unit.destination, 8.0F, 1.6F,
                    simulation.terrainElevation(unit.destination) + 0.4F,
                    selectedColor);
            addRing(vertices, unit.position, 9.0F, 1.5F, ground + 0.4F,
                    selectedColor);
        }
        addBox(vertices, x - 5.5F, z - 6.5F, 11.0F, 13.0F,
               ground + 0.05F, 7.0F,
               unit.selected ? selectedColor : unitColor);
        addBox(vertices, x - 3.3F, z - 3.0F, 6.6F, 6.0F,
               ground + 7.05F, 3.2F,
               unit.selected ? Color{1.0F, 0.84F, 0.43F}
                             : Color{0.56F, 0.77F, 0.88F});
        addBox(vertices, x + 2.5F, z - 1.15F, 5.8F, 2.3F,
               ground + 8.0F, 1.25F,
               unit.selected ? selectedColor : Color{0.72F, 0.81F, 0.78F});
    }
    const auto& feedback = simulation.lastMoveCommand();
    if (feedback.valid && simulation.tickCount() - feedback.issuedAtTick <= 90) {
        addRing(vertices, feedback.requestedDestination, 17.0F, 2.0F,
                simulation.terrainElevation(feedback.requestedDestination) + 0.45F,
                {0.24F, 0.95F, 0.96F});
    }
    return vertices;
}

std::vector<SceneVertex> buildSelectionOverlayGeometry(
    const ScreenRectangle& rectangle) {
    const float left = std::min(rectangle.x0, rectangle.x1);
    const float right = std::max(rectangle.x0, rectangle.x1);
    const float top = std::min(rectangle.y0, rectangle.y1);
    const float bottom = std::max(rectangle.y0, rectangle.y1);
    constexpr float border = 1.5F;
    const Color color{0.25F, 0.9F, 1.0F};
    std::vector<SceneVertex> overlay;
    overlay.reserve(24);
    addQuad(overlay, {left, top, 0.0F}, {right, top, 0.0F},
            {right, top + border, 0.0F}, {left, top + border, 0.0F}, color);
    addQuad(overlay, {left, bottom - border, 0.0F},
            {right, bottom - border, 0.0F}, {right, bottom, 0.0F},
            {left, bottom, 0.0F}, color);
    addQuad(overlay, {left, top, 0.0F}, {left + border, top, 0.0F},
            {left + border, bottom, 0.0F}, {left, bottom, 0.0F}, color);
    addQuad(overlay, {right - border, top, 0.0F}, {right, top, 0.0F},
            {right, bottom, 0.0F}, {right - border, bottom, 0.0F}, color);
    return overlay;
}

} // namespace rts
