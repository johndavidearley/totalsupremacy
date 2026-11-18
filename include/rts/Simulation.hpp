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
    std::uint8_t team = 0;
    std::int32_t health = 100;
    std::uint32_t attackTarget = 0;
    bool alive = true;
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
    Dijkstra,
    // One shared integration field for every selected unit. A one-unit order
    // still uses A*; there is no group route to share.
    FlowField
};

// Caller-facing commands. selectNearest records the unit it resolved, or a
// clear when it misses. A path-backend move is not a command.
enum class SimulationCommandKind : std::uint8_t {
    SelectUnit = 1,
    SelectUnits = 2,
    ClearSelection = 3,
    Move = 4,
    Step = 5,
    SetTeam = 6,
    Attack = 7,
};

struct SimulationCommand {
    SimulationCommandKind kind = SimulationCommandKind::Step;
    std::uint32_t unitId = 0;
    std::vector<std::uint32_t> unitIds;
    std::int32_t destinationMilliX = 0;
    std::int32_t destinationMilliZ = 0;
    std::uint8_t team = 0;
    PathfindingAlgorithm algorithm = PathfindingAlgorithm::FlowField;

    bool operator==(const SimulationCommand&) const = default;
};

enum class ReplayDivergenceKind : std::uint8_t {
    None = 0,
    Command = 1,
    State = 2,
};

// Result of comparing two command logs on fresh simulations.
// None: both logs and every hash match; commandIndex is the log length.
// Command: a payload differs, or one log continues. The hashes are from
// before that command. State: those commands matched and the hashes did
// not. beforeCommands means the fresh simulations differed.
struct ReplayDivergence {
    ReplayDivergenceKind kind = ReplayDivergenceKind::None;
    std::size_t commandIndex = 0;
    bool beforeCommands = false;
    std::uint64_t leftHash = 0;
    std::uint64_t rightHash = 0;
};

struct PathfindingMetrics {
    std::uint64_t startCellsExamined = 0;
    std::uint64_t goalCellsExamined = 0;
    std::uint64_t aStarNodesExpanded = 0;
    std::uint64_t dijkstraNodesExpanded = 0;
    std::uint64_t flowFieldNodesExpanded = 0;
    std::uint64_t formationPaths = 0;
    std::uint64_t formationFallbacks = 0;
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
    // Movement is stored in thousandths of a world unit. One tick travels
    // exactly 2.4 world units, which is an integer number of milliunits.
    static constexpr int kWorldMilliunits = 1000;
    static constexpr int kMilliunitsPerTick = 2400;
    static_assert(static_cast<int>(kUnitSpeed * kWorldMilliunits) ==
                      kMilliunitsPerTick * 30,
                  "milliunits per tick must match unit speed at 30 Hz");
    static constexpr float kMapWidth = 2000.0F;
    static constexpr float kMapHeight = 1400.0F;
    static constexpr float kTerrainCellSize = 20.0F;
    // Height-unit to path-cost scale. A short detour through the western pass
    // is cheaper than climbing that ridge; a gentle slope is not. Heights are
    // stored in thousandths of a height unit.
    static constexpr int kElevationMilliunits = 1000;
    static constexpr int kElevationCostPerHeightUnit = 180;
    static constexpr float kElevationCostPerUnit =
        static_cast<float>(kElevationCostPerHeightUnit);
    static constexpr int kTerrainColumns =
        static_cast<int>(kMapWidth / kTerrainCellSize);
    // One infantry archetype. Damage is applied after movement on the same tick.
    static constexpr std::int32_t kUnitHealth = 100;
    static constexpr std::int32_t kAttackDamage = 10;
    static constexpr std::int32_t kAttackRangeMilliunits = 40 * kWorldMilliunits;
    static constexpr int kTerrainRows =
        static_cast<int>(kMapHeight / kTerrainCellSize);

    explicit Simulation(std::size_t unitCount = 1000);

    // Round half away from zero. Map positions round-trip through these
    // conversions for every milliunit in the battlefield.
    static std::int32_t worldMilliunits(float world) noexcept;
    static float worldFromMilliunits(std::int32_t milliunits) noexcept;

    std::int32_t terrainElevationMilliunits(Vec2 position) const noexcept;
    // ceil(|height change| * 180), with the height change in milliunits.
    static int elevationTransitionCost(std::int32_t fromMilliunits,
                                       std::int32_t toMilliunits) noexcept;
    static float elevationFromMilliunits(std::int32_t milliunits) noexcept;

    float terrainElevation(Vec2 position) const noexcept;
    void step();
    bool selectNearest(Vec2 position, float radius);
    bool selectUnit(std::uint32_t id);
    std::size_t selectUnits(const std::vector<std::uint32_t>& ids);
    void clearSelection();
    std::size_t moveSelected(
        Vec2 destination,
        PathfindingAlgorithm algorithm = PathfindingAlgorithm::FlowField);
    std::size_t moveSelected(Vec2 destination, GridPathBackend& backend);
    // Logs the assignment even when the id is unknown.
    bool setTeam(std::uint32_t id, std::uint8_t team);
    // Orders each selected living unit to attack targetId. Zero clears the order.
    std::size_t attackSelected(std::uint32_t targetId);

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
    const std::vector<SimulationCommand>& commandLog() const noexcept {
        return commandLog_;
    }
    // FNV-1a 64 of authoritative gameplay. The command log is not an input.
    std::uint64_t stateHash() const noexcept;
    // Applies commands to a fresh simulation of the same unit count by
    // calling the public methods, so the target records an equal log.
    static void replay(Simulation& simulation,
                       const std::vector<SimulationCommand>& commands);
    // Replays both logs on fresh simulations of unitCount and stops at the
    // first command or state-hash disagreement.
    static ReplayDivergence findReplayDivergence(
        std::size_t unitCount,
        const std::vector<SimulationCommand>& left,
        const std::vector<SimulationCommand>& right);
    std::size_t selectedCount() const noexcept;

private:
    void clearSelectedFlags() noexcept;
    Unit* findUnit(std::uint32_t id) noexcept;
    void resolveAttacks();
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

    struct FlowField {
        std::vector<int> costToAnchor;
        std::vector<int> towardAnchor;
        int anchor = -1;
    };

    bool buildFlowField(int anchor, std::vector<char>& needed,
                        FlowField& field);
    std::vector<int> flowChain(const FlowField& field, int node) const;
    bool mergedFlowNodes(const FlowField& field, int startNode, int goalNode,
                         std::vector<int>& nodes) const;
    bool formationOffsetPath(const std::vector<Vec2>& spine, Vec2 start,
                             Vec2 slot, std::vector<Vec2>& path,
                             Vec2& routedDestination) const;
    bool gridRouteCost(int startNode, const std::vector<int>& nodes,
                       std::uint64_t& cost) const;
    std::uint64_t polylineRouteCost(Vec2 start,
                                    const std::vector<Vec2>& path) const;
    void routeFormation(Vec2 destination, std::size_t columns);

    std::vector<Unit> units_;
    std::vector<TerrainObstacle> terrainObstacles_;
    std::vector<std::uint8_t> blockedPathCells_;
    std::vector<std::int32_t> terrainVertexElevationMilliunits_;
    std::vector<std::int32_t> terrainCellElevationMilliunits_;
    std::vector<std::array<int, 8>> terrainTransitionCosts_;
    MoveCommandFeedback lastMoveCommand_;
    PathfindingMetrics lastPathfindingMetrics_;
    std::vector<SimulationCommand> commandLog_;
    std::uint64_t tickCount_ = 0;
};

inline std::int32_t Simulation::worldMilliunits(float world) noexcept {
    const double scaled = static_cast<double>(world) * kWorldMilliunits;
    if (scaled >= 0.0) {
        return static_cast<std::int32_t>(scaled + 0.5);
    }
    return static_cast<std::int32_t>(scaled - 0.5);
}

inline float Simulation::worldFromMilliunits(std::int32_t milliunits) noexcept {
    return static_cast<float>(static_cast<double>(milliunits) / kWorldMilliunits);
}

inline int Simulation::elevationTransitionCost(std::int32_t fromMilliunits,
                                              std::int32_t toMilliunits) noexcept {
    const std::int64_t delta = fromMilliunits >= toMilliunits
        ? static_cast<std::int64_t>(fromMilliunits) - toMilliunits
        : static_cast<std::int64_t>(toMilliunits) - fromMilliunits;
    return static_cast<int>(
        (delta * kElevationCostPerHeightUnit + (kElevationMilliunits - 1)) /
        kElevationMilliunits);
}

inline float Simulation::elevationFromMilliunits(std::int32_t milliunits) noexcept {
    return static_cast<float>(static_cast<double>(milliunits) / kElevationMilliunits);
}

} // namespace rts
