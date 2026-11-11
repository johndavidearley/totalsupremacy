#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace rts {

struct Vec2 {
    float x = 0.0F;
    float y = 0.0F;
};

struct Unit {
    std::uint32_t id = 0;
    Vec2 position;
    Vec2 destination;
    std::vector<Vec2> path;
    std::size_t nextWaypoint = 0;
    bool selected = false;
};

struct TerrainObstacle {
    Vec2 minimum;
    Vec2 maximum;
    float height = 0.0F;
};

struct MoveCommandFeedback {
    bool valid = false;
    Vec2 requestedDestination;
    std::size_t unitsOrdered = 0;
    std::size_t unitsRouted = 0;
    std::uint64_t issuedAtTick = 0;
};

enum class PathfindingAlgorithm {
    AStar,
    Dijkstra
};

struct PathfindingMetrics {
    std::uint64_t startCellsExamined = 0;
    std::uint64_t goalCellsExamined = 0;
    std::uint64_t aStarNodesExpanded = 0;
    std::uint64_t dijkstraNodesExpanded = 0;
    std::uint64_t pathCost = 0;
};

struct GridPathQuery {
    int startNode;
    int goalNode;
};

struct GridPathResult {
    bool found = false;
    std::uint64_t cost = 0;
    std::vector<int> nodes;
};

class GridPathBackend {
public:
    virtual ~GridPathBackend() = default;
    virtual std::vector<GridPathResult> search(
        const std::vector<std::uint8_t>& blocked,
        const std::vector<float>& elevations,
        const std::vector<std::array<int, 8>>& transitionCosts,
        const std::vector<GridPathQuery>& queries) = 0;
};

class Simulation {
public:
    static constexpr float kFixedStepSeconds = 1.0F / 30.0F;
    static constexpr float kUnitSpeed = 72.0F;
    static constexpr float kMapWidth = 2000.0F;
    static constexpr float kMapHeight = 1400.0F;
    static constexpr float kTerrainCellSize = 20.0F;
    static constexpr int kTerrainColumns =
        static_cast<int>(kMapWidth / kTerrainCellSize);
    static constexpr int kTerrainRows =
        static_cast<int>(kMapHeight / kTerrainCellSize);

    explicit Simulation(std::size_t unitCount = 1000);

    float terrainElevation(Vec2 position) const noexcept;
    void step();
    bool selectNearest(Vec2 position, float radius);
    bool selectUnit(std::uint32_t id);
    std::size_t selectUnits(const std::vector<std::uint32_t>& ids);
    void clearSelection() noexcept;
    std::size_t moveSelected(
        Vec2 destination,
        PathfindingAlgorithm algorithm = PathfindingAlgorithm::AStar);
    std::size_t moveSelected(Vec2 destination, GridPathBackend& backend);

    const std::vector<Unit>& units() const noexcept { return units_; }
    const std::vector<TerrainObstacle>& terrainObstacles() const noexcept {
        return terrainObstacles_;
    }
    const MoveCommandFeedback& lastMoveCommand() const noexcept {
        return lastMoveCommand_;
    }
    const PathfindingMetrics& lastPathfindingMetrics() const noexcept {
        return lastPathfindingMetrics_;
    }
    std::uint64_t tickCount() const noexcept { return tickCount_; }
    std::size_t selectedCount() const noexcept;

private:
    struct PreparedPath {
        Vec2 start;
        Vec2 requestedDestination;
        int positionStartNode = -1;
        int startNode = -1;
        int goalNode = -1;
    };

    bool blockedByTerrain(Vec2 position) const noexcept;
    bool segmentBlockedByTerrain(Vec2 start, Vec2 end) const noexcept;
    bool preparePath(Vec2 start, Vec2 destination, PreparedPath& prepared,
                     PathfindingMetrics& metrics) const;
    void finishPath(const PreparedPath& prepared, const std::vector<int>& nodes,
                    std::vector<Vec2>& path, Vec2& routedDestination) const;
    bool findPath(Vec2 start, Vec2 destination, std::vector<Vec2>& path,
                  Vec2& routedDestination, PathfindingMetrics& metrics,
                  PathfindingAlgorithm algorithm) const;

    std::vector<Unit> units_;
    std::vector<TerrainObstacle> terrainObstacles_;
    std::vector<std::uint8_t> blockedPathCells_;
    std::vector<float> terrainVertexElevations_;
    std::vector<float> terrainCellElevations_;
    std::vector<std::array<int, 8>> terrainTransitionCosts_;
    MoveCommandFeedback lastMoveCommand_;
    PathfindingMetrics lastPathfindingMetrics_;
    std::uint64_t tickCount_ = 0;
};

} // namespace rts
