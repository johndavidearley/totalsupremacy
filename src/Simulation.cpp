#include "rts/Simulation.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <queue>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace rts {
namespace {

constexpr std::array<std::pair<int, int>, 8> kPathNeighbors{{
    {-1, -1}, {0, -1}, {1, -1}, {-1, 0},
    {1, 0}, {-1, 1}, {0, 1}, {1, 1}
}};
constexpr std::array<int, 8> kPathGeometricCosts{{
    1414, 1000, 1414, 1000, 1000, 1414, 1000, 1414
}};

constexpr int kCellMilliunits =
    static_cast<int>(Simulation::kTerrainCellSize) * Simulation::kWorldMilliunits;
constexpr std::int64_t kMapMilliWidth =
    static_cast<std::int64_t>(Simulation::kMapWidth) * Simulation::kWorldMilliunits;
constexpr std::int64_t kMapMilliHeight =
    static_cast<std::int64_t>(Simulation::kMapHeight) * Simulation::kWorldMilliunits;

struct MilliPoint {
    std::int32_t x = 0;
    std::int32_t y = 0;
};

MilliPoint toMilli(Vec2 position) {
    return {Simulation::worldMilliunits(position.x),
            Simulation::worldMilliunits(position.y)};
}

Vec2 fromMilli(MilliPoint point) {
    return {Simulation::worldFromMilliunits(point.x),
            Simulation::worldFromMilliunits(point.y)};
}

MilliPoint clampMilli(std::int64_t x, std::int64_t y) {
    return {
        static_cast<std::int32_t>(std::clamp(x, std::int64_t{0}, kMapMilliWidth)),
        static_cast<std::int32_t>(std::clamp(y, std::int64_t{0}, kMapMilliHeight))
    };
}

MilliPoint cellCenterMilli(int index) {
    const int x = index % Simulation::kTerrainColumns;
    const int y = index / Simulation::kTerrainColumns;
    return {
        static_cast<std::int32_t>((2 * x + 1) * (kCellMilliunits / 2)),
        static_cast<std::int32_t>((2 * y + 1) * (kCellMilliunits / 2))
    };
}

std::int32_t columnCenterMilli(int column) {
    return static_cast<std::int32_t>((2 * column + 1) * (kCellMilliunits / 2));
}

int cellIndexForMilli(MilliPoint position) {
    const int x = std::clamp(position.x / kCellMilliunits, 0,
                             Simulation::kTerrainColumns - 1);
    const int y = std::clamp(position.y / kCellMilliunits, 0,
                             Simulation::kTerrainRows - 1);
    return y * Simulation::kTerrainColumns + x;
}

std::int64_t abs64(std::int64_t value) {
    return value < 0 ? -value : value;
}

// Round half away from zero. Distances are non-negative.
std::uint64_t geometricMilliCost(std::int64_t dx, std::int64_t dy) {
    const std::int64_t diagonal = std::min(abs64(dx), abs64(dy));
    const std::int64_t orthogonal = std::max(abs64(dx), abs64(dy)) - diagonal;
    const std::int64_t numerator = diagonal * 1414 + orthogonal * 1000;
    return static_cast<std::uint64_t>(
        (numerator + kCellMilliunits / 2) / kCellMilliunits);
}

Vec2 formationDestination(Vec2 destination, std::size_t columns,
                          std::size_t index) {
    const std::int64_t originX = Simulation::worldMilliunits(destination.x);
    const std::int64_t originY = Simulation::worldMilliunits(destination.y);
    const std::int64_t column = static_cast<std::int64_t>(index % columns);
    const std::int64_t row = static_cast<std::int64_t>(index / columns);
    const std::int64_t span = static_cast<std::int64_t>(columns - 1);
    // (column - (columns - 1) / 2) * 14 units, in milliunits.
    constexpr std::int64_t halfSpacing = 7 * Simulation::kWorldMilliunits;
    return fromMilli(clampMilli(
        originX + (2 * column - span) * halfSpacing,
        originY + (2 * row - span) * halfSpacing));
}

// Floor of the square root. The double estimate is corrected with integer
// comparisons so the result does not depend on libm rounding.
std::int64_t floorSqrt(std::int64_t value) {
    if (value <= 0) {
        return 0;
    }
    auto estimate = static_cast<std::int64_t>(std::sqrt(static_cast<double>(value)));
    if (estimate < 0) {
        estimate = 0;
    }
    while (estimate > 0 && estimate > value / estimate) {
        --estimate;
    }
    while (estimate < std::numeric_limits<std::int64_t>::max() &&
           estimate + 1 <= value / (estimate + 1)) {
        ++estimate;
    }
    return estimate;
}

// exp(-t) in thousandths. tMilli is thousandths of the exponent. The series
// and the exp(-1) scale are integer, so the height grid does not call libm.
std::int32_t expNegMilli(std::int32_t tMilli) {
    if (tMilli <= 0) {
        return 1000;
    }
    if (tMilli >= 16000) {
        return 0;
    }
    const int whole = tMilli / 1000;
    const int fraction = tMilli % 1000;
    std::int64_t term = 1000000000LL;
    std::int64_t sum = term;
    const std::int64_t fractionBillionths =
        static_cast<std::int64_t>(fraction) * 1000000LL;
    for (int n = 1; n <= 14; ++n) {
        term = term * fractionBillionths / (1000000000LL * n);
        if (term == 0) {
            break;
        }
        if ((n % 2) != 0) {
            sum -= term;
        } else {
            sum += term;
        }
    }
    std::int64_t scale = 1000000000LL;
    for (int i = 0; i < whole; ++i) {
        scale = scale * 367879441LL / 1000000000LL;
    }
    const std::int64_t billionths = sum * scale / 1000000000LL;
    return static_cast<std::int32_t>((billionths + 500000LL) / 1000000LL);
}

std::int32_t smoothstepMilli(std::int32_t tMilli) {
    if (tMilli <= 0) {
        return 0;
    }
    if (tMilli >= 1000) {
        return 1000;
    }
    const std::int64_t t = tMilli;
    return static_cast<std::int32_t>(t * t * (3000 - 2 * t) / 1000000LL);
}

std::int32_t gaussianMilli(std::int64_t dx, std::int64_t radiusX,
                           std::int64_t dz, std::int64_t radiusZ) {
    const std::int64_t norm =
        dx * dx * 1000 / (radiusX * radiusX) +
        dz * dz * 1000 / (radiusZ * radiusZ);
    const std::int64_t exponent = std::min(norm / 2, std::int64_t{20000});
    return expNegMilli(static_cast<std::int32_t>(exponent));
}

// Heights are thousandths of a height unit. Coordinates are world milliunits.
std::int32_t generatedElevationMilli(std::int32_t x, std::int32_t z) {
    const auto hill = [x, z](int centerX, int centerZ, int radiusX, int radiusZ,
                             int heightMilli) {
        const std::int64_t dx =
            static_cast<std::int64_t>(x) - static_cast<std::int64_t>(centerX) * 1000;
        const std::int64_t dz =
            static_cast<std::int64_t>(z) - static_cast<std::int64_t>(centerZ) * 1000;
        const std::int32_t weight = gaussianMilli(
            dx, static_cast<std::int64_t>(radiusX) * 1000,
            dz, static_cast<std::int64_t>(radiusZ) * 1000);
        return static_cast<std::int32_t>(
            static_cast<std::int64_t>(heightMilli) * weight / 1000);
    };
    // Western ridge at z=650, from x=160 to x=1100, with a pass at x=560.
    // The shoulders stay high so the cheap crossing is the pass.
    constexpr std::int32_t ridgeWest = 160000;
    constexpr std::int32_t ridgeEast = 1100000;
    constexpr std::int32_t ridgeFade = 70000;
    std::int32_t span = 0;
    if (x > ridgeWest && x < ridgeEast) {
        span = 1000;
        if (x < ridgeWest + ridgeFade) {
            span = smoothstepMilli(
                static_cast<std::int32_t>(
                    (static_cast<std::int64_t>(x) - ridgeWest) * 1000 / ridgeFade));
        } else if (x > ridgeEast - ridgeFade) {
            span = smoothstepMilli(
                static_cast<std::int32_t>(
                    (static_cast<std::int64_t>(ridgeEast) - x) * 1000 / ridgeFade));
        }
    }
    const std::int64_t across = static_cast<std::int64_t>(z) - 650000;
    const std::int32_t crestWeight = gaussianMilli(across, 50000, 0, 50000);
    const std::int32_t crest = static_cast<std::int32_t>(
        static_cast<std::int64_t>(36000) * crestWeight / 1000 * span / 1000);
    const std::int64_t pass = static_cast<std::int64_t>(x) - 560000;
    const std::int32_t notch = gaussianMilli(pass, 80000, 0, 80000);
    const std::int32_t ridge = static_cast<std::int32_t>(
        static_cast<std::int64_t>(crest) * (1000 - notch) / 1000);
    return 4000 +
        hill(1520, 300, 320, 210, 11000) +
        hill(260, 1180, 240, 170, 9000) +
        ridge;
}

} // namespace

namespace {

SimulationCommand makeCommand(SimulationCommandKind kind,
                              std::uint32_t unitId = 0) {
    SimulationCommand command;
    command.kind = kind;
    command.unitId = unitId;
    return command;
}

void applyCommand(Simulation& simulation, const SimulationCommand& command) {
    switch (command.kind) {
    case SimulationCommandKind::SelectUnit:
        simulation.selectUnit(command.unitId);
        break;
    case SimulationCommandKind::SelectUnits:
        simulation.selectUnits(command.unitIds);
        break;
    case SimulationCommandKind::ClearSelection:
        simulation.clearSelection();
        break;
    case SimulationCommandKind::Move:
        simulation.moveSelected(
            {Simulation::worldFromMilliunits(command.destinationMilliX),
             Simulation::worldFromMilliunits(command.destinationMilliZ)},
            command.algorithm, command.append);
        break;
    case SimulationCommandKind::Step:
        simulation.step();
        break;
    case SimulationCommandKind::SetTeam:
        simulation.setTeam(command.unitId, command.team);
        break;
    case SimulationCommandKind::Attack:
        simulation.attackSelected(command.unitId, command.append);
        break;
    case SimulationCommandKind::SetArchetype:
        simulation.setArchetype(
            command.unitId, static_cast<UnitArchetype>(command.archetype));
        break;
    }
}

} // namespace

Simulation::Simulation(std::size_t unitCount, Battlefield battlefield)
    : battlefield_(battlefield) {
    constexpr std::array<Vec2, 6> ridgePositions{{
        {534.0F, 580.0F}, {586.0F, 580.0F},
        {540.0F, 550.0F}, {580.0F, 550.0F},
        {548.0F, 720.0F}, {575.0F, 745.0F}
    }};
    if (battlefield_ != Battlefield::Sandbox) {
        unitCount = ridgePositions.size();
    }
    units_.reserve(unitCount);
    terrainObstacles_.reserve(26);
    for (int row = 0; row < 14; ++row) {
        for (int column = 0; column < 20; ++column) {
            const auto variation = static_cast<unsigned>(row * 73 + column * 97);
            if (variation % 11U != 0U) {
                continue;
            }
            const float x = static_cast<float>(column) * 100.0F;
            const float z = static_cast<float>(row) * 100.0F;
            terrainObstacles_.push_back({
                {x + 24.0F, z + 29.0F},
                {x + 46.0F, z + 48.0F},
                3.0F + static_cast<float>(variation % 4U)
            });
        }
    }
    constexpr float pathCellSize = kTerrainCellSize;
    constexpr int pathColumns = kTerrainColumns;
    constexpr int pathRows = kTerrainRows;
    constexpr float pathClearance = 6.5F + pathCellSize * 0.75F;
    terrainVertexElevationMilliunits_.resize(
        static_cast<std::size_t>((pathColumns + 1) * (pathRows + 1)));
    for (int row = 0; row <= pathRows; ++row) {
        for (int column = 0; column <= pathColumns; ++column) {
            terrainVertexElevationMilliunits_[static_cast<std::size_t>(
                row * (pathColumns + 1) + column)] = generatedElevationMilli(
                    column * kCellMilliunits, row * kCellMilliunits);
        }
    }
    terrainCellElevationMilliunits_.resize(
        static_cast<std::size_t>(pathColumns * pathRows));
    for (int row = 0; row < pathRows; ++row) {
        for (int column = 0; column < pathColumns; ++column) {
            const Vec2 center{
                (static_cast<float>(column) + 0.5F) * pathCellSize,
                (static_cast<float>(row) + 0.5F) * pathCellSize
            };
            terrainCellElevationMilliunits_[static_cast<std::size_t>(
                row * pathColumns + column)] = terrainElevationMilliunits(center);
        }
    }
    terrainTransitionCosts_.resize(
        static_cast<std::size_t>(pathColumns * pathRows));
    for (int row = 0; row < pathRows; ++row) {
        for (int column = 0; column < pathColumns; ++column) {
            const auto cellIndex = static_cast<std::size_t>(
                row * pathColumns + column);
            for (std::size_t neighbor = 0; neighbor < kPathNeighbors.size();
                 ++neighbor) {
                const int nextX = column + kPathNeighbors[neighbor].first;
                const int nextY = row + kPathNeighbors[neighbor].second;
                if (nextX < 0 || nextX >= pathColumns ||
                    nextY < 0 || nextY >= pathRows) {
                    continue;
                }
                const auto nextIndex = static_cast<std::size_t>(
                    nextY * pathColumns + nextX);
                const int elevationCost = elevationTransitionCost(
                    terrainCellElevationMilliunits_[cellIndex],
                    terrainCellElevationMilliunits_[nextIndex]);
                terrainTransitionCosts_[cellIndex][neighbor] =
                    kPathGeometricCosts[neighbor] + elevationCost;
            }
        }
    }
    blockedPathCells_.resize(static_cast<std::size_t>(pathColumns * pathRows));
    for (int row = 0; row < pathRows; ++row) {
        for (int column = 0; column < pathColumns; ++column) {
            const Vec2 center{
                (static_cast<float>(column) + 0.5F) * pathCellSize,
                (static_cast<float>(row) + 0.5F) * pathCellSize
            };
            const bool blocked = std::any_of(
                terrainObstacles_.begin(), terrainObstacles_.end(),
                [center](const TerrainObstacle& obstacle) {
                    return center.x >= obstacle.minimum.x - pathClearance &&
                           center.x <= obstacle.maximum.x + pathClearance &&
                           center.y >= obstacle.minimum.y - pathClearance &&
                           center.y <= obstacle.maximum.y + pathClearance;
                });
            blockedPathCells_[static_cast<std::size_t>(row * pathColumns + column)] =
                blocked ? 1U : 0U;
        }
    }

    constexpr std::size_t columns = 40;
    constexpr float spacing = 26.0F;
    for (std::size_t i = 0; i < unitCount; ++i) {
        const auto column = i % columns;
        const auto row = i / columns;
        Vec2 position{
            380.0F + static_cast<float>(column) * spacing,
            250.0F + static_cast<float>(row) * spacing
        };
        if (battlefield_ != Battlefield::Sandbox) {
            position = ridgePositions[i];
        }
        if (blockedByTerrain(position)) {
            bool relocated = false;
            for (float radius = 4.0F; radius <= 200.0F && !relocated; radius += 4.0F) {
                for (float offsetY = -radius; offsetY <= radius && !relocated;
                     offsetY += 4.0F) {
                    for (float offsetX = -radius; offsetX <= radius; offsetX += 4.0F) {
                        if (std::abs(offsetX) != radius &&
                            std::abs(offsetY) != radius) {
                            continue;
                        }
                        const Vec2 candidate{position.x + offsetX, position.y + offsetY};
                        if (candidate.x >= 0.0F && candidate.x <= kMapWidth &&
                            candidate.y >= 0.0F && candidate.y <= kMapHeight &&
                            !blockedByTerrain(candidate)) {
                            position = candidate;
                            relocated = true;
                            break;
                        }
                    }
                }
            }
            if (!relocated) {
                throw std::runtime_error(
                    "Unable to place a starting unit outside terrain obstacles.");
            }
        }
        Unit unit;
        unit.id = static_cast<std::uint32_t>(i + 1);
        unit.position = position;
        unit.destination = position;
        unit.health = kUnitHealth;
        if (battlefield_ != Battlefield::Sandbox) {
            unit.team = i >= 4 ? 1 : 0;
            if (i == 2 || i == 3 || i == 5) {
                unit.archetype = UnitArchetype::Ranger;
                unit.health = archetypeHealth(unit.archetype);
            }
        }
        units_.push_back(std::move(unit));
    }
}

std::int32_t Simulation::terrainElevationMilliunits(Vec2 position) const noexcept {
    constexpr std::int32_t mapWidth = static_cast<std::int32_t>(kMapWidth) * kWorldMilliunits;
    constexpr std::int32_t mapHeight = static_cast<std::int32_t>(kMapHeight) * kWorldMilliunits;
    const std::int32_t x = std::clamp(worldMilliunits(position.x), 0, mapWidth);
    const std::int32_t z = std::clamp(worldMilliunits(position.y), 0, mapHeight);
    const int column = std::min(x / kCellMilliunits, kTerrainColumns - 1);
    const int row = std::min(z / kCellMilliunits, kTerrainRows - 1);
    const int xFraction = x - column * kCellMilliunits;
    const int zFraction = z - row * kCellMilliunits;
    const auto vertex = [this](int columnIndex, int rowIndex) -> std::int64_t {
        return terrainVertexElevationMilliunits_[static_cast<std::size_t>(
            rowIndex * (kTerrainColumns + 1) + columnIndex)];
    };
    const std::int64_t cell = kCellMilliunits;
    const std::int64_t top =
        vertex(column, row) * (cell - xFraction) +
        vertex(column + 1, row) * xFraction;
    const std::int64_t bottom =
        vertex(column, row + 1) * (cell - xFraction) +
        vertex(column + 1, row + 1) * xFraction;
    const std::int64_t area = cell * cell;
    return static_cast<std::int32_t>(
        (top * (cell - zFraction) + bottom * zFraction + area / 2) / area);
}

float Simulation::terrainElevation(Vec2 position) const noexcept {
    return elevationFromMilliunits(terrainElevationMilliunits(position));
}

void Simulation::restartBattlefield() {
    *this = Simulation(units_.size(), battlefield_);
}

void Simulation::updatePassObjective() {
    if (objectiveStatus_ != ObjectiveStatus::Active) {
        return;
    }
    bool blueAlive = false;
    bool infantryInPass = false;
    bool redInPass = false;
    for (const auto& unit : units_) {
        if (!unit.alive) {
            continue;
        }
        const std::int64_t dx = worldMilliunits(unit.position.x) - worldMilliunits(kPassCenter.x);
        const std::int64_t dz = worldMilliunits(unit.position.y) - worldMilliunits(kPassCenter.y);
        constexpr std::int64_t radius = kPassRadius * 1000;
        const bool inPass = dx * dx + dz * dz <= radius * radius;
        if (unit.team == 0) {
            blueAlive = true;
            infantryInPass |= inPass && unit.archetype == UnitArchetype::Infantry;
        } else if (unit.team == 1) {
            redInPass |= inPass;
        }
    }
    holdTicks_ = infantryInPass && !redInPass ? holdTicks_ + 1 : 0;
    if (holdTicks_ == kPassHoldTicks) {
        objectiveStatus_ = ObjectiveStatus::Victory;
    } else if (!blueAlive || tickCount_ >= kPassDeadline) {
        objectiveStatus_ = ObjectiveStatus::Defeat;
    }
}

void Simulation::step() {
    const bool counterattack = battlefield_ == Battlefield::HoldThePass &&
        objectiveStatus_ == ObjectiveStatus::Active && tickCount_ >= kCounterattackTick;
    if (battlefield_ != Battlefield::Sandbox) {
        for (auto& defender : units_) {
            if (!defender.alive || defender.team != 1 || orderInProgress(defender)) {
                continue;
            }
            for (const auto& opponent : units_) {
                if (opponent.alive && opponent.team == 0 &&
                    (counterattack || withinAttackRange(defender, opponent.position))) {
                    defender.attackTarget = opponent.id;
                    break;
                }
            }
        }
    }
    for (auto& unit : units_) {
        refreshChase(unit);
    }
    for (auto& unit : units_) {
        if (!unit.alive || unit.nextWaypoint >= unit.path.size()) {
            continue;
        }
        // Waypoint crossings charge the floored length, so a tick can run
        // long by less than one milliunit per waypoint entered. The partial
        // step divides by that floor only when it exceeds the budget.
        std::int32_t remaining = kMilliunitsPerTick;
        std::int32_t x = worldMilliunits(unit.position.x);
        std::int32_t y = worldMilliunits(unit.position.y);
        bool cancelled = false;
        while (remaining > 0 && unit.nextWaypoint < unit.path.size()) {
            const Vec2 waypoint = unit.path[unit.nextWaypoint];
            const std::int32_t targetX = worldMilliunits(waypoint.x);
            const std::int32_t targetY = worldMilliunits(waypoint.y);
            const std::int64_t dx = static_cast<std::int64_t>(targetX) - x;
            const std::int64_t dy = static_cast<std::int64_t>(targetY) - y;
            const std::int64_t distanceSquared = dx * dx + dy * dy;
            const std::int64_t remainingSquared =
                static_cast<std::int64_t>(remaining) * remaining;
            if (distanceSquared <= remainingSquared) {
                x = targetX;
                y = targetY;
                remaining -= static_cast<std::int32_t>(floorSqrt(distanceSquared));
                ++unit.nextWaypoint;
                continue;
            }

            const std::int64_t distance = floorSqrt(distanceSquared);
            const std::int64_t stepX = dx * remaining / distance;
            const std::int64_t stepY = dy * remaining / distance;
            const Vec2 next{
                worldFromMilliunits(static_cast<std::int32_t>(x + stepX)),
                worldFromMilliunits(static_cast<std::int32_t>(y + stepY))
            };
            if (blockedByTerrain(next)) {
                unit.path.clear();
                unit.nextWaypoint = 0;
                cancelled = true;
                break;
            }
            x = static_cast<std::int32_t>(x + stepX);
            y = static_cast<std::int32_t>(y + stepY);
            remaining = 0;
        }
        unit.position = {worldFromMilliunits(x), worldFromMilliunits(y)};
        if (cancelled) {
            unit.destination = unit.position;
        }
    }
    advanceProjectiles();
    resolveAttacks();
    advanceOrders();
    ++tickCount_;
    if (battlefield_ == Battlefield::HoldThePass) {
        updatePassObjective();
    }
    commandLog_.push_back(makeCommand(SimulationCommandKind::Step));
}

bool Simulation::selectNearest(Vec2 position, float radius) {
    clearSelectedFlags();

    const float radiusSquared = radius * radius;
    float nearestDistanceSquared = radiusSquared;
    Unit* nearest = nullptr;
    for (auto& unit : units_) {
        if (!unit.alive) {
            continue;
        }
        const float dx = unit.position.x - position.x;
        const float dy = unit.position.y - position.y;
        const float distanceSquared = dx * dx + dy * dy;
        if (distanceSquared <= nearestDistanceSquared) {
            nearestDistanceSquared = distanceSquared;
            nearest = &unit;
        }
    }
    if (nearest == nullptr) {
        commandLog_.push_back(makeCommand(SimulationCommandKind::ClearSelection));
        return false;
    }
    nearest->selected = true;
    commandLog_.push_back(
        makeCommand(SimulationCommandKind::SelectUnit, nearest->id));
    return true;
}

bool Simulation::selectUnit(std::uint32_t id) {
    clearSelectedFlags();
    commandLog_.push_back(makeCommand(SimulationCommandKind::SelectUnit, id));
    const auto unit = std::find_if(units_.begin(), units_.end(),
        [id](const Unit& candidate) { return candidate.id == id; });
    if (unit == units_.end() || !unit->alive) {
        return false;
    }
    unit->selected = true;
    return true;
}

std::size_t Simulation::selectUnits(const std::vector<std::uint32_t>& ids) {
    const std::unordered_set<std::uint32_t> selectedIds(ids.begin(), ids.end());
    std::size_t count = 0;
    for (auto& unit : units_) {
        unit.selected = unit.alive && selectedIds.contains(unit.id);
        count += unit.selected ? 1U : 0U;
    }
    SimulationCommand command;
    command.kind = SimulationCommandKind::SelectUnits;
    command.unitIds = ids;
    commandLog_.push_back(std::move(command));
    return count;
}

void Simulation::clearSelectedFlags() noexcept {
    for (auto& unit : units_) {
        unit.selected = false;
    }
}

void Simulation::clearSelection() {
    clearSelectedFlags();
    commandLog_.push_back(makeCommand(SimulationCommandKind::ClearSelection));
}

std::size_t Simulation::moveSelected(Vec2 destination,
                                     PathfindingAlgorithm algorithm,
                                     bool append) {
    destination = fromMilli(toMilli(destination));
    const auto count = selectedCount();
    lastMoveCommand_ = {true, destination, count, 0, tickCount_};
    lastPathfindingMetrics_ = {};
    auto recordMove = [&] {
        SimulationCommand command;
        command.kind = SimulationCommandKind::Move;
        command.destinationMilliX = worldMilliunits(destination.x);
        command.destinationMilliZ = worldMilliunits(destination.y);
        command.algorithm = algorithm;
        command.append = append;
        commandLog_.push_back(std::move(command));
    };
    if (count == 0) {
        recordMove();
        return 0;
    }

    const auto columns = static_cast<std::size_t>(
        std::ceil(std::sqrt(static_cast<double>(count))));
    bool anyBusy = false;
    if (append) {
        for (const auto& unit : units_) {
            if (unit.selected && (orderInProgress(unit) || !unit.orders.empty())) {
                anyBusy = true;
                break;
            }
        }
    }
    if (append && anyBusy) {
        std::size_t index = 0;
        for (auto& unit : units_) {
            if (!unit.selected) {
                continue;
            }
            const auto slot = formationDestination(destination, columns, index);
            UnitOrder order;
            order.kind = UnitOrderKind::Move;
            order.destinationMilliX = worldMilliunits(slot.x);
            order.destinationMilliZ = worldMilliunits(slot.y);
            order.algorithm = static_cast<std::uint8_t>(PathfindingAlgorithm::AStar);
            if (orderInProgress(unit) || !unit.orders.empty()) {
                unit.orders.push_back(order);
            } else {
                beginOrder(unit, order);
                if (unit.nextWaypoint < unit.path.size()) {
                    ++lastMoveCommand_.unitsRouted;
                }
            }
            ++index;
        }
        recordMove();
        return count;
    }

    for (auto& unit : units_) {
        if (unit.selected) {
            unit.attackTarget = 0;
            unit.orders.clear();
        }
    }
    if (count > 1 && algorithm == PathfindingAlgorithm::FlowField) {
        routeFormation(destination, columns);
        recordMove();
        return count;
    }

    const auto individual = algorithm == PathfindingAlgorithm::FlowField
        ? PathfindingAlgorithm::AStar
        : algorithm;
    std::size_t index = 0;
    for (auto& unit : units_) {
        if (!unit.selected) {
            continue;
        }
        const auto target = formationDestination(destination, columns, index);
        unit.path.clear();
        unit.nextWaypoint = 0;
        if (findPath(unit.position, target, unit.path, unit.destination,
                     lastPathfindingMetrics_, individual)) {
            ++lastMoveCommand_.unitsRouted;
        } else {
            unit.destination = unit.position;
        }
        ++index;
    }
    recordMove();
    return count;
}

bool Simulation::setTeam(std::uint32_t id, std::uint8_t team) {
    SimulationCommand command;
    command.kind = SimulationCommandKind::SetTeam;
    command.unitId = id;
    command.team = team;
    commandLog_.push_back(std::move(command));
    Unit* unit = findUnit(id);
    if (unit == nullptr) {
        return false;
    }
    unit->team = team;
    return true;
}

bool Simulation::setArchetype(std::uint32_t id, UnitArchetype archetype) {
    SimulationCommand command;
    command.kind = SimulationCommandKind::SetArchetype;
    command.unitId = id;
    command.archetype = static_cast<std::uint8_t>(archetype);
    commandLog_.push_back(std::move(command));
    if (archetype != UnitArchetype::Infantry && archetype != UnitArchetype::Ranger) {
        return false;
    }
    Unit* unit = findUnit(id);
    if (unit == nullptr) {
        return false;
    }
    unit->archetype = archetype;
    const auto maximum = archetypeHealth(archetype);
    if (unit->alive && unit->health > maximum) {
        unit->health = maximum;
    }
    const auto cooldown = archetypeCooldown(archetype);
    if (unit->attackCooldown > cooldown) {
        unit->attackCooldown = cooldown;
    }
    if (unit->attackTarget != 0) {
        unit->path.clear();
        unit->nextWaypoint = 0;
        refreshChase(*unit);
    }
    return true;
}

std::size_t Simulation::attackSelected(std::uint32_t targetId, bool append) {
    SimulationCommand command;
    command.kind = SimulationCommandKind::Attack;
    command.unitId = targetId;
    command.append = append;
    commandLog_.push_back(std::move(command));
    std::size_t count = 0;
    for (Unit& unit : units_) {
        if (!unit.selected || !unit.alive) {
            continue;
        }
        UnitOrder order;
        order.kind = UnitOrderKind::Attack;
        order.targetId = targetId;
        if (append && (orderInProgress(unit) || !unit.orders.empty())) {
            unit.orders.push_back(order);
        } else {
            unit.orders.clear();
            beginOrder(unit, order);
        }
        ++count;
    }
    return count;
}

bool Simulation::orderInProgress(const Unit& unit) {
    if (!unit.alive) {
        return false;
    }
    if (unit.attackTarget != 0) {
        const Unit* target = findUnit(unit.attackTarget);
        return target != nullptr && target->alive;
    }
    return unit.nextWaypoint < unit.path.size();
}

void Simulation::beginOrder(Unit& unit, const UnitOrder& order) {
    unit.path.clear();
    unit.nextWaypoint = 0;
    if (order.kind == UnitOrderKind::Attack) {
        unit.attackTarget = order.targetId;
        unit.destination = unit.position;
        refreshChase(unit);
        return;
    }
    unit.attackTarget = 0;
    PathfindingMetrics metrics;
    const Vec2 goal{
        worldFromMilliunits(order.destinationMilliX),
        worldFromMilliunits(order.destinationMilliZ)
    };
    const auto algorithm = static_cast<PathfindingAlgorithm>(order.algorithm);
    if (!findPath(unit.position, goal, unit.path, unit.destination, metrics, algorithm)) {
        unit.destination = unit.position;
    }
}

void Simulation::advanceOrders() {
    for (Unit& unit : units_) {
        if (!unit.alive) {
            unit.orders.clear();
            continue;
        }
        while (!unit.orders.empty() && !orderInProgress(unit)) {
            const UnitOrder next = unit.orders.front();
            unit.orders.erase(unit.orders.begin());
            beginOrder(unit, next);
        }
    }
}

bool Simulation::withinAttackRange(const Unit& attacker, Vec2 target) const noexcept {
    const std::int64_t dx = static_cast<std::int64_t>(worldMilliunits(attacker.position.x)) -
        worldMilliunits(target.x);
    const std::int64_t dy = static_cast<std::int64_t>(worldMilliunits(attacker.position.y)) -
        worldMilliunits(target.y);
    const std::int64_t range = archetypeRange(attacker.archetype);
    return dx * dx + dy * dy <= range * range;
}

void Simulation::refreshChase(Unit& attacker) {
    if (!attacker.alive || attacker.attackTarget == 0) {
        return;
    }
    const auto hold = [](Unit& unit) {
        unit.path.clear();
        unit.nextWaypoint = 0;
        unit.destination = unit.position;
    };
    const Unit* target = findUnit(attacker.attackTarget);
    if (target == nullptr || !target->alive || target->team == attacker.team ||
        withinAttackRange(attacker, target->position)) {
        hold(attacker);
        return;
    }
    // Keep the current path until it runs out. Replanning every tick would
    // repeat A* while the target is still ahead of that route.
    if (attacker.nextWaypoint < attacker.path.size()) {
        return;
    }
    PathfindingMetrics metrics;
    attacker.path.clear();
    attacker.nextWaypoint = 0;
    if (!findPath(attacker.position, target->position, attacker.path,
                  attacker.destination, metrics, PathfindingAlgorithm::AStar)) {
        hold(attacker);
    }
}

Unit* Simulation::findUnit(std::uint32_t id) noexcept {
    if (id == 0 || static_cast<std::size_t>(id) > units_.size()) {
        return nullptr;
    }
    Unit& unit = units_[static_cast<std::size_t>(id - 1)];
    return unit.id == id ? &unit : nullptr;
}

void Simulation::advanceProjectiles() {
    std::vector<std::int32_t> damage(units_.size(), 0);
    std::vector<Projectile> still;
    still.reserve(projectiles_.size());
    const std::int64_t speed = kProjectileMilliunitsPerTick;
    for (Projectile shot : projectiles_) {
        Unit* target = findUnit(shot.targetId);
        if (target == nullptr || !target->alive || target->team == shot.team) {
            continue;
        }
        const std::int64_t dx = static_cast<std::int64_t>(worldMilliunits(target->position.x)) -
            shot.x;
        const std::int64_t dz = static_cast<std::int64_t>(worldMilliunits(target->position.y)) -
            shot.z;
        const std::int64_t distanceSquared = dx * dx + dz * dz;
        if (distanceSquared <= speed * speed) {
            damage[static_cast<std::size_t>(target->id - 1)] += shot.damage;
            continue;
        }
        const std::int64_t distance = floorSqrt(distanceSquared);
        shot.x += static_cast<std::int32_t>(dx * speed / distance);
        shot.z += static_cast<std::int32_t>(dz * speed / distance);
        still.push_back(shot);
    }
    projectiles_ = std::move(still);
    applyDamage(damage);
}

void Simulation::resolveAttacks() {
    for (Unit& unit : units_) {
        if (unit.alive && unit.attackCooldown > 0) {
            --unit.attackCooldown;
        }
    }
    std::vector<std::uint8_t> fired(units_.size(), 0);
    for (std::size_t index = 0; index < units_.size(); ++index) {
        const Unit& attacker = units_[index];
        if (!attacker.alive || attacker.attackTarget == 0 || attacker.attackCooldown != 0) {
            continue;
        }
        const Unit* target = findUnit(attacker.attackTarget);
        if (target == nullptr || !target->alive || target->team == attacker.team ||
            !withinAttackRange(attacker, target->position)) {
            continue;
        }
        Projectile shot;
        shot.id = nextProjectileId_++;
        shot.x = worldMilliunits(attacker.position.x);
        shot.z = worldMilliunits(attacker.position.y);
        shot.targetId = attacker.attackTarget;
        shot.damage = archetypeDamage(attacker.archetype);
        shot.team = attacker.team;
        projectiles_.push_back(shot);
        fired[index] = 1;
    }
    for (std::size_t index = 0; index < units_.size(); ++index) {
        if (fired[index] != 0) {
            units_[index].attackCooldown = archetypeCooldown(units_[index].archetype);
        }
    }
}

void Simulation::applyDamage(const std::vector<std::int32_t>& damage) {
    for (std::size_t index = 0; index < units_.size(); ++index) {
        if (damage[index] <= 0) {
            continue;
        }
        Unit& unit = units_[index];
        if (!unit.alive) {
            continue;
        }
        if (unit.health > damage[index]) {
            unit.health -= damage[index];
            continue;
        }
        unit.health = 0;
        unit.alive = false;
        unit.selected = false;
        unit.attackTarget = 0;
        unit.attackCooldown = 0;
        unit.orders.clear();
        unit.path.clear();
        unit.nextWaypoint = 0;
        unit.destination = unit.position;
    }
}

std::size_t Simulation::moveSelected(Vec2 destination, GridPathBackend& backend) {
    destination = fromMilli(toMilli(destination));
    const auto count = selectedCount();
    PathfindingMetrics metrics;
    std::vector<std::size_t> unitIndices;
    std::vector<PreparedPath> preparedPaths;
    std::vector<GridPathQuery> queries;
    unitIndices.reserve(count);
    preparedPaths.reserve(count);
    queries.reserve(count);

    const auto columns = static_cast<std::size_t>(
        std::ceil(std::sqrt(static_cast<double>(count))));
    for (std::size_t i = 0, selectedIndex = 0; i < units_.size(); ++i) {
        const auto& unit = units_[i];
        if (!unit.selected) {
            continue;
        }
        const auto target = formationDestination(destination, columns, selectedIndex);
        PreparedPath prepared;
        unitIndices.push_back(i);
        if (!preparePath(unit.position, target, prepared, metrics)) {
            prepared = {unit.position, target, -1, -1, -1};
        }
        preparedPaths.push_back(prepared);
        queries.push_back({prepared.startNode, prepared.goalNode});
        ++selectedIndex;
    }

    std::vector<float> cellElevations(terrainCellElevationMilliunits_.size());
    for (std::size_t cell = 0; cell < cellElevations.size(); ++cell) {
        cellElevations[cell] = elevationFromMilliunits(
            terrainCellElevationMilliunits_[cell]);
    }
    const auto results = count == 0 ? std::vector<GridPathResult>{} :
        backend.search(blockedPathCells_, cellElevations,
                       terrainTransitionCosts_, queries);
    if (results.size() != count) {
        throw std::runtime_error("Path backend returned the wrong number of results.");
    }
    constexpr int nodeCount = kTerrainColumns * kTerrainRows;
    for (std::size_t i = 0; i < count; ++i) {
        const auto& result = results[i];
        const auto& query = queries[i];
        if (!result.found) {
            continue;
        }
        if (query.startNode < 0 || query.goalNode < 0 ||
            result.nodes.size() > static_cast<std::size_t>(nodeCount)) {
            throw std::runtime_error("Path backend returned an invalid route.");
        }
        int previous = query.startNode;
        std::uint64_t totalCost = 0;
        for (const int node : result.nodes) {
            if (node < 0 || node >= nodeCount ||
                blockedPathCells_[static_cast<std::size_t>(node)] != 0) {
                throw std::runtime_error("Path backend returned a blocked route.");
            }
            const int dx = node % kTerrainColumns - previous % kTerrainColumns;
            const int dy = node / kTerrainColumns - previous / kTerrainColumns;
            const auto neighbor = std::find(kPathNeighbors.begin(),
                                            kPathNeighbors.end(),
                                            std::pair{dx, dy});
            if (neighbor == kPathNeighbors.end()) {
                throw std::runtime_error("Path backend returned non-adjacent cells.");
            }
            if (dx != 0 && dy != 0) {
                const int horizontal = (previous / kTerrainColumns) *
                    kTerrainColumns + node % kTerrainColumns;
                const int vertical = (node / kTerrainColumns) *
                    kTerrainColumns + previous % kTerrainColumns;
                if (blockedPathCells_[static_cast<std::size_t>(horizontal)] != 0 ||
                    blockedPathCells_[static_cast<std::size_t>(vertical)] != 0) {
                    throw std::runtime_error("Path backend returned a corner-cutting route.");
                }
            }
            const auto direction = static_cast<std::size_t>(
                std::distance(kPathNeighbors.begin(), neighbor));
            totalCost += static_cast<std::uint64_t>(
                terrainTransitionCosts_[static_cast<std::size_t>(previous)][direction]);
            previous = node;
        }
        if (previous != query.goalNode || totalCost != result.cost) {
            throw std::runtime_error("Path backend returned an incorrect route cost or goal.");
        }
    }

    MoveCommandFeedback feedback{true, destination, count, 0, tickCount_};
    for (const auto index : unitIndices) {
        units_[index].attackTarget = 0;
        units_[index].orders.clear();
    }
    for (std::size_t i = 0; i < count; ++i) {
        auto& unit = units_[unitIndices[i]];
        unit.path.clear();
        unit.nextWaypoint = 0;
        if (results[i].found) {
            finishPath(preparedPaths[i], results[i].nodes,
                       unit.path, unit.destination);
            metrics.pathCost += results[i].cost;
            ++feedback.unitsRouted;
        } else {
            unit.destination = unit.position;
        }
    }
    lastMoveCommand_ = feedback;
    lastPathfindingMetrics_ = metrics;
    return count;
}

std::size_t Simulation::selectedCount() const noexcept {
    return static_cast<std::size_t>(std::count_if(
        units_.begin(), units_.end(), [](const Unit& unit) { return unit.selected; }));
}

std::uint64_t Simulation::stateHash() const noexcept {
    // Version 6, little-endian: version u32, tick u64, each unit in vector
    // order (id u32, position and destination milliunits i32, next waypoint
    // u32, selected u8, team u8, archetype u8, alive u8, health i32, attack
    // target u32, attack cooldown i32, queued-order count u32, each order's
    // kind u8, destination milliunits i32, target u32, and algorithm u8,
    // path count u32, each waypoint's milliunits), the next projectile id,
    // each shot in flight (id u32, milliunits i32, target u32, damage i32,
    // team u8), then the last move
    // (valid u8, requested milliunits i32, ordered and routed counts u32,
    // issue tick u64). Version 7 uses the same fields for the ridge skirmish,
    // whose defenders acquire targets automatically. Version 8 appends
    // the hold timer (u32) and objective status (u8).
    constexpr std::uint64_t offset = 14695981039346656037ULL;
    constexpr std::uint64_t prime = 1099511628211ULL;
    std::uint64_t hash = offset;
    const auto mix = [&hash](std::uint64_t value, int bytes) noexcept {
        for (int byte = 0; byte < bytes; ++byte) {
            hash ^= static_cast<std::uint8_t>(value >> (8 * byte));
            hash *= prime;
        }
    };
    mix(battlefield_ == Battlefield::HoldThePass ? 8U
        : battlefield_ == Battlefield::RidgeSkirmish ? 7U : 6U, 4);
    mix(tickCount_, 8);
    for (const Unit& unit : units_) {
        const auto milli = [](float world) noexcept {
            return static_cast<std::uint32_t>(worldMilliunits(world));
        };
        mix(unit.id, 4);
        mix(milli(unit.position.x), 4);
        mix(milli(unit.position.y), 4);
        mix(milli(unit.destination.x), 4);
        mix(milli(unit.destination.y), 4);
        mix(static_cast<std::uint32_t>(unit.nextWaypoint), 4);
        mix(unit.selected ? 1U : 0U, 1);
        mix(unit.team, 1);
        mix(static_cast<std::uint8_t>(unit.archetype), 1);
        mix(unit.alive ? 1U : 0U, 1);
        mix(static_cast<std::uint32_t>(unit.health), 4);
        mix(unit.attackTarget, 4);
        mix(static_cast<std::uint32_t>(unit.attackCooldown), 4);
        mix(static_cast<std::uint32_t>(unit.orders.size()), 4);
        for (const UnitOrder& order : unit.orders) {
            mix(static_cast<std::uint8_t>(order.kind), 1);
            mix(static_cast<std::uint32_t>(order.destinationMilliX), 4);
            mix(static_cast<std::uint32_t>(order.destinationMilliZ), 4);
            mix(order.targetId, 4);
            mix(order.algorithm, 1);
        }
        mix(static_cast<std::uint32_t>(unit.path.size()), 4);
        for (const Vec2& waypoint : unit.path) {
            mix(milli(waypoint.x), 4);
            mix(milli(waypoint.y), 4);
        }
    }
    mix(nextProjectileId_, 4);
    mix(static_cast<std::uint32_t>(projectiles_.size()), 4);
    for (const Projectile& shot : projectiles_) {
        mix(shot.id, 4);
        mix(static_cast<std::uint32_t>(shot.x), 4);
        mix(static_cast<std::uint32_t>(shot.z), 4);
        mix(shot.targetId, 4);
        mix(static_cast<std::uint32_t>(shot.damage), 4);
        mix(shot.team, 1);
    }
    mix(lastMoveCommand_.valid ? 1U : 0U, 1);
    mix(static_cast<std::uint32_t>(
            worldMilliunits(lastMoveCommand_.requestedDestination.x)), 4);
    mix(static_cast<std::uint32_t>(
            worldMilliunits(lastMoveCommand_.requestedDestination.y)), 4);
    mix(static_cast<std::uint32_t>(lastMoveCommand_.unitsOrdered), 4);
    mix(static_cast<std::uint32_t>(lastMoveCommand_.unitsRouted), 4);
    mix(lastMoveCommand_.issuedAtTick, 8);
    if (battlefield_ == Battlefield::HoldThePass) {
        mix(static_cast<std::uint32_t>(holdTicks_), 4);
        mix(static_cast<std::uint8_t>(objectiveStatus_), 1);
    }
    return hash;
}

void Simulation::replay(Simulation& simulation,
                        const std::vector<SimulationCommand>& commands) {
    for (const SimulationCommand& command : commands) {
        applyCommand(simulation, command);
    }
}

ReplayDivergence Simulation::findReplayDivergence(
    std::size_t unitCount,
    const std::vector<SimulationCommand>& leftLog,
    const std::vector<SimulationCommand>& rightLog, Battlefield battlefield) {
    Simulation left(unitCount, battlefield);
    Simulation right(unitCount, battlefield);
    ReplayDivergence report;
    report.leftHash = left.stateHash();
    report.rightHash = right.stateHash();
    if (report.leftHash != report.rightHash) {
        report.kind = ReplayDivergenceKind::State;
        report.beforeCommands = true;
        return report;
    }

    const std::size_t shared = std::min(leftLog.size(), rightLog.size());
    for (std::size_t index = 0; index < shared; ++index) {
        if (leftLog[index] != rightLog[index]) {
            report.kind = ReplayDivergenceKind::Command;
            report.commandIndex = index;
            return report;
        }
        applyCommand(left, leftLog[index]);
        applyCommand(right, rightLog[index]);
        report.leftHash = left.stateHash();
        report.rightHash = right.stateHash();
        report.commandIndex = index;
        if (report.leftHash != report.rightHash) {
            report.kind = ReplayDivergenceKind::State;
            return report;
        }
    }
    if (leftLog.size() != rightLog.size()) {
        report.kind = ReplayDivergenceKind::Command;
        report.commandIndex = shared;
        return report;
    }
    report.kind = ReplayDivergenceKind::None;
    report.commandIndex = shared;
    return report;
}

bool Simulation::blockedByTerrain(Vec2 position) const noexcept {
    constexpr float clearance = 6.5F;
    return std::any_of(terrainObstacles_.begin(), terrainObstacles_.end(),
        [position](const TerrainObstacle& obstacle) {
            return position.x >= obstacle.minimum.x - clearance &&
                   position.x <= obstacle.maximum.x + clearance &&
                   position.y >= obstacle.minimum.y - clearance &&
                   position.y <= obstacle.maximum.y + clearance;
        });
}

bool Simulation::segmentBlockedByTerrain(Vec2 start, Vec2 end) const noexcept {
    // Samples lie on the segment, so a segment whose padded bounds miss every
    // inflated obstacle cannot contain a blocked sample. The pad covers
    // milliunit-to-float rounding. Overlapping segments still sample.
    constexpr float clearance = 6.5F;
    constexpr float pad = 0.05F;
    const float minX = std::min(start.x, end.x) - pad;
    const float maxX = std::max(start.x, end.x) + pad;
    const float minY = std::min(start.y, end.y) - pad;
    const float maxY = std::max(start.y, end.y) + pad;
    bool overlaps = false;
    for (const TerrainObstacle& obstacle : terrainObstacles_) {
        if (maxX >= obstacle.minimum.x - clearance &&
            minX <= obstacle.maximum.x + clearance &&
            maxY >= obstacle.minimum.y - clearance &&
            minY <= obstacle.maximum.y + clearance) {
            overlaps = true;
            break;
        }
    }
    if (!overlaps) {
        return false;
    }

    const MilliPoint from = toMilli(start);
    const MilliPoint to = toMilli(end);
    const std::int64_t dx = static_cast<std::int64_t>(to.x) - from.x;
    const std::int64_t dy = static_cast<std::int64_t>(to.y) - from.y;
    constexpr std::int64_t sampleSpacing = 2 * Simulation::kWorldMilliunits;
    const std::int64_t span = std::max(abs64(dx), abs64(dy));
    const int samples = static_cast<int>((span + sampleSpacing - 1) / sampleSpacing);
    for (int i = 1; i <= samples; ++i) {
        const MilliPoint point{
            static_cast<std::int32_t>(from.x + dx * i / samples),
            static_cast<std::int32_t>(from.y + dy * i / samples)
        };
        if (blockedByTerrain(fromMilli(point))) {
            return true;
        }
    }
    return false;
}

bool Simulation::preparePath(Vec2 start, Vec2 requestedDestination,
                             PreparedPath& prepared,
                             PathfindingMetrics& metrics) const {
    constexpr int columns = kTerrainColumns;
    constexpr int rows = kTerrainRows;
    const MilliPoint startMilli = toMilli(start);
    const MilliPoint destinationMilli = toMilli(requestedDestination);
    const auto isBlocked = [this](int index) {
        return blockedPathCells_[static_cast<std::size_t>(index)] != 0;
    };
    const auto axisGap = [](int cell, std::int32_t origin, bool open) {
        if (!open) {
            return std::numeric_limits<std::int64_t>::max();
        }
        return abs64(static_cast<std::int64_t>(columnCenterMilli(cell)) - origin);
    };

    int startNode = cellIndexForMilli(startMilli);
    const int positionStartNode = startNode;
    if (isBlocked(startNode)) {
        std::int64_t nearestStartDistance = std::numeric_limits<std::int64_t>::max();
        int accessibleStartNode = -1;
        const int startX = startNode % columns;
        const int startY = startNode / columns;
        const int maxSearchRadius = std::max({
            startX, columns - 1 - startX, startY, rows - 1 - startY
        });
        for (int radius = 0; radius <= maxSearchRadius; ++radius) {
            const int minX = startX - radius;
            const int maxX = startX + radius;
            const int minY = startY - radius;
            const int maxY = startY + radius;
            for (int y = minY; y <= maxY; ++y) {
                for (int x = minX; x <= maxX; ++x) {
                    if (radius != 0 && x != minX && x != maxX &&
                        y != minY && y != maxY) {
                        continue;
                    }
                    if (x < 0 || x >= columns || y < 0 || y >= rows) {
                        continue;
                    }
                    const int index = y * columns + x;
                    ++metrics.startCellsExamined;
                    if (isBlocked(index)) {
                        continue;
                    }
                    const MilliPoint point = cellCenterMilli(index);
                    const std::int64_t dx =
                        static_cast<std::int64_t>(point.x) - startMilli.x;
                    const std::int64_t dy =
                        static_cast<std::int64_t>(point.y) - startMilli.y;
                    const std::int64_t distanceSquared = dx * dx + dy * dy;
                    if ((distanceSquared < nearestStartDistance ||
                         (distanceSquared == nearestStartDistance &&
                          index < accessibleStartNode)) &&
                        !segmentBlockedByTerrain(start, fromMilli(point))) {
                        nearestStartDistance = distanceSquared;
                        accessibleStartNode = index;
                    }
                }
            }

            const int leftX = minX - 1;
            const int rightX = maxX + 1;
            const int topY = minY - 1;
            const int bottomY = maxY + 1;
            const std::int64_t nearestUnsearchedAxisDistance = std::min({
                axisGap(leftX, startMilli.x, leftX >= 0),
                axisGap(rightX, startMilli.x, rightX < columns),
                axisGap(topY, startMilli.y, topY >= 0),
                axisGap(bottomY, startMilli.y, bottomY < rows)
            });
            if (accessibleStartNode >= 0 &&
                (nearestUnsearchedAxisDistance ==
                     std::numeric_limits<std::int64_t>::max() ||
                 nearestStartDistance < nearestUnsearchedAxisDistance *
                                            nearestUnsearchedAxisDistance)) {
                break;
            }
        }
        if (accessibleStartNode < 0) {
            return false;
        }
        startNode = accessibleStartNode;
    }
    int goalNode = -1;
    std::int64_t nearestGoalDistance = std::numeric_limits<std::int64_t>::max();
    const int destinationX = cellIndexForMilli(destinationMilli) % columns;
    const int destinationY = cellIndexForMilli(destinationMilli) / columns;
    const int maxSearchRadius = std::max({
        destinationX, columns - 1 - destinationX,
        destinationY, rows - 1 - destinationY
    });
    for (int radius = 0; radius <= maxSearchRadius; ++radius) {
        const int minX = destinationX - radius;
        const int maxX = destinationX + radius;
        const int minY = destinationY - radius;
        const int maxY = destinationY + radius;
        for (int y = minY; y <= maxY; ++y) {
            for (int x = minX; x <= maxX; ++x) {
                if (radius != 0 && x != minX && x != maxX &&
                    y != minY && y != maxY) {
                    continue;
                }
                if (x < 0 || x >= columns || y < 0 || y >= rows) {
                    continue;
                }
                const int index = y * columns + x;
                ++metrics.goalCellsExamined;
                if (isBlocked(index)) {
                    continue;
                }
                const MilliPoint point = cellCenterMilli(index);
                const std::int64_t dx =
                    static_cast<std::int64_t>(point.x) - destinationMilli.x;
                const std::int64_t dy =
                    static_cast<std::int64_t>(point.y) - destinationMilli.y;
                const std::int64_t distanceSquared = dx * dx + dy * dy;
                if (distanceSquared < nearestGoalDistance ||
                    (distanceSquared == nearestGoalDistance && index < goalNode)) {
                    nearestGoalDistance = distanceSquared;
                    goalNode = index;
                }
            }
        }

        const int leftX = minX - 1;
        const int rightX = maxX + 1;
        const int topY = minY - 1;
        const int bottomY = maxY + 1;
        const std::int64_t nearestUnsearchedAxisDistance = std::min({
            axisGap(leftX, destinationMilli.x, leftX >= 0),
            axisGap(rightX, destinationMilli.x, rightX < columns),
            axisGap(topY, destinationMilli.y, topY >= 0),
            axisGap(bottomY, destinationMilli.y, bottomY < rows)
        });
        if (goalNode >= 0 &&
            (nearestUnsearchedAxisDistance ==
                 std::numeric_limits<std::int64_t>::max() ||
             nearestGoalDistance < nearestUnsearchedAxisDistance *
                                       nearestUnsearchedAxisDistance)) {
            break;
        }
    }
    if (goalNode < 0) {
        return false;
    }

    prepared = {start, requestedDestination, positionStartNode, startNode, goalNode};
    return true;
}

bool Simulation::findPath(Vec2 start, Vec2 requestedDestination,
                          std::vector<Vec2>& path, Vec2& routedDestination,
                          PathfindingMetrics& metrics,
                          PathfindingAlgorithm algorithm) const {
    PreparedPath prepared;
    if (!preparePath(start, requestedDestination, prepared, metrics)) {
        return false;
    }

    constexpr int columns = kTerrainColumns;
    constexpr int rows = kTerrainRows;
    constexpr int nodeCount = columns * rows;
    constexpr int orthogonalCost = 1000;
    constexpr int diagonalCost = 1414;
    const int startNode = prepared.startNode;
    const int goalNode = prepared.goalNode;
    const auto isBlocked = [this](int index) {
        return blockedPathCells_[static_cast<std::size_t>(index)] != 0;
    };
    struct OpenNode {
        int estimatedTotalCost;
        int cost;
        int index;
    };
    const auto compare = [algorithm](const OpenNode& left, const OpenNode& right) {
        if (left.estimatedTotalCost != right.estimatedTotalCost) {
            return left.estimatedTotalCost > right.estimatedTotalCost;
        }
        if (algorithm == PathfindingAlgorithm::AStar &&
            left.cost != right.cost) {
            return left.cost < right.cost;
        }
        return left.index > right.index;
    };
    std::priority_queue<OpenNode, std::vector<OpenNode>, decltype(compare)> open(compare);
    std::array<int, nodeCount> costFromStart;
    costFromStart.fill(std::numeric_limits<int>::max());
    std::array<int, nodeCount> parent;
    parent.fill(-1);
    std::array<bool, nodeCount> closed{};

    const auto heuristic = [this](int from, int to) {
        const int dx = std::abs(from % columns - to % columns);
        const int dy = std::abs(from / columns - to / columns);
        const int diagonal = std::min(dx, dy);
        const int horizontalCost =
            diagonal * diagonalCost +
            (std::max(dx, dy) - diagonal) * orthogonalCost;
        const std::int64_t fromElevation =
            terrainCellElevationMilliunits_[static_cast<std::size_t>(from)];
        const std::int64_t toElevation =
            terrainCellElevationMilliunits_[static_cast<std::size_t>(to)];
        const std::int64_t elevationDifference = fromElevation >= toElevation
            ? fromElevation - toElevation
            : toElevation - fromElevation;
        // Truncation keeps the lower bound admissible: edge costs use ceil.
        const int unavoidableElevationCost = static_cast<int>(
            elevationDifference * kElevationCostPerHeightUnit / kElevationMilliunits);
        return horizontalCost + unavoidableElevationCost;
    };
    const auto estimateRemainingCost = [&heuristic, algorithm](int from, int to) {
        return algorithm == PathfindingAlgorithm::AStar
            ? heuristic(from, to)
            : 0;
    };
    costFromStart[static_cast<std::size_t>(startNode)] = 0;
    open.push({estimateRemainingCost(startNode, goalNode), 0, startNode});

    while (!open.empty()) {
        const auto current = open.top();
        open.pop();
        const auto currentIndex = static_cast<std::size_t>(current.index);
        if (closed[currentIndex] || current.cost != costFromStart[currentIndex]) {
            continue;
        }
        if (current.index == goalNode) {
            break;
        }
        closed[currentIndex] = true;
        if (algorithm == PathfindingAlgorithm::AStar) {
            ++metrics.aStarNodesExpanded;
        } else {
            ++metrics.dijkstraNodesExpanded;
        }
        const int currentX = current.index % columns;
        const int currentY = current.index / columns;
        for (std::size_t neighbor = 0; neighbor < kPathNeighbors.size();
             ++neighbor) {
            const auto [dx, dy] = kPathNeighbors[neighbor];
            const int nextX = currentX + dx;
            const int nextY = currentY + dy;
            if (nextX < 0 || nextX >= columns || nextY < 0 || nextY >= rows) {
                continue;
            }
            const int nextIndex = nextY * columns + nextX;
            if (nextIndex != goalNode && isBlocked(nextIndex)) {
                continue;
            }
            if (dx != 0 && dy != 0) {
                const int horizontal = currentY * columns + nextX;
                const int vertical = nextY * columns + currentX;
                if ((horizontal != startNode && isBlocked(horizontal)) ||
                    (vertical != startNode && isBlocked(vertical))) {
                    continue;
                }
            }
            const auto nextIndexSize = static_cast<std::size_t>(nextIndex);
            const int newCost = current.cost +
                terrainTransitionCosts_[currentIndex][neighbor];
            if (newCost >= costFromStart[nextIndexSize]) {
                continue;
            }
            costFromStart[nextIndexSize] = newCost;
            parent[nextIndexSize] = current.index;
            open.push({newCost + estimateRemainingCost(nextIndex, goalNode),
                       newCost, nextIndex});
        }
    }
    if (goalNode != startNode &&
        parent[static_cast<std::size_t>(goalNode)] < 0) {
        return false;
    }
    metrics.pathCost += static_cast<std::uint64_t>(
        costFromStart[static_cast<std::size_t>(goalNode)]);

    std::vector<int> nodes;
    for (int current = goalNode; current != startNode;
         current = parent[static_cast<std::size_t>(current)]) {
        if (current < 0) {
            return false;
        }
        nodes.push_back(current);
    }
    std::reverse(nodes.begin(), nodes.end());
    finishPath(prepared, nodes, path, routedDestination);
    return true;
}

void Simulation::finishPath(const PreparedPath& prepared,
                            const std::vector<int>& nodes,
                            std::vector<Vec2>& path,
                            Vec2& routedDestination) const {
    const auto start = fromMilli(toMilli(prepared.start));
    const auto requestedDestination = fromMilli(toMilli(prepared.requestedDestination));
    path.clear();
    path.reserve(nodes.size() + 2);
    if (prepared.startNode != prepared.positionStartNode) {
        path.push_back(fromMilli(cellCenterMilli(prepared.startNode)));
    }
    for (const int node : nodes) {
        path.push_back(fromMilli(cellCenterMilli(node)));
    }

    const Vec2 lastNode = path.empty() ? start : path.back();
    if (!blockedByTerrain(requestedDestination) &&
        !segmentBlockedByTerrain(lastNode, requestedDestination)) {
        routedDestination = requestedDestination;
        if (path.empty() ||
            path.back().x != requestedDestination.x ||
            path.back().y != requestedDestination.y) {
            path.push_back(requestedDestination);
        }
    } else {
        routedDestination = lastNode;
        if (path.empty()) {
            path.push_back(routedDestination);
        }
    }
}

namespace {

constexpr int kNodeCount =
    Simulation::kTerrainColumns * Simulation::kTerrainRows;

Vec2 pathCellCenter(int index) {
    return fromMilli(cellCenterMilli(index));
}

int pathNeighborDirection(int from, int to) {
    const int dx = to % Simulation::kTerrainColumns -
                   from % Simulation::kTerrainColumns;
    const int dy = to / Simulation::kTerrainColumns -
                   from / Simulation::kTerrainColumns;
    for (std::size_t neighbor = 0; neighbor < kPathNeighbors.size(); ++neighbor) {
        if (kPathNeighbors[neighbor].first == dx &&
            kPathNeighbors[neighbor].second == dy) {
            return static_cast<int>(neighbor);
        }
    }
    return -1;
}

} // namespace

bool Simulation::buildFlowField(int anchor, std::vector<char>& needed,
                                FlowField& field) {
    if (anchor < 0 || anchor >= kNodeCount) {
        return false;
    }
    field.anchor = anchor;
    field.costToAnchor.assign(static_cast<std::size_t>(kNodeCount),
                               std::numeric_limits<int>::max());
    field.towardAnchor.assign(static_cast<std::size_t>(kNodeCount), -1);
    field.costToAnchor[static_cast<std::size_t>(anchor)] = 0;

    int remaining = 0;
    for (const char watched : needed) {
        remaining += watched != 0 ? 1 : 0;
    }
    if (remaining == 0) {
        return true;
    }

    struct OpenNode {
        int cost;
        int index;
    };
    const auto compare = [](const OpenNode& left, const OpenNode& right) {
        if (left.cost != right.cost) {
            return left.cost > right.cost;
        }
        return left.index > right.index;
    };
    std::priority_queue<OpenNode, std::vector<OpenNode>, decltype(compare)> open(
        compare);
    std::vector<char> closed(static_cast<std::size_t>(kNodeCount), 0);
    open.push({0, anchor});
    const auto isBlocked = [this](int index) {
        return blockedPathCells_[static_cast<std::size_t>(index)] != 0;
    };

    while (!open.empty()) {
        const auto current = open.top();
        open.pop();
        const auto currentIndex = static_cast<std::size_t>(current.index);
        if (closed[currentIndex] != 0 ||
            current.cost != field.costToAnchor[currentIndex]) {
            continue;
        }
        closed[currentIndex] = 1;
        ++lastPathfindingMetrics_.flowFieldNodesExpanded;
        if (needed[currentIndex] != 0) {
            needed[currentIndex] = 0;
            if (--remaining == 0) {
                break;
            }
        }

        const int currentX = current.index % kTerrainColumns;
        const int currentY = current.index / kTerrainColumns;
        for (std::size_t neighbor = 0; neighbor < kPathNeighbors.size();
             ++neighbor) {
            const auto [dx, dy] = kPathNeighbors[neighbor];
            const int nextX = currentX + dx;
            const int nextY = currentY + dy;
            if (nextX < 0 || nextX >= kTerrainColumns ||
                nextY < 0 || nextY >= kTerrainRows) {
                continue;
            }
            const int nextIndex = nextY * kTerrainColumns + nextX;
            if (isBlocked(nextIndex)) {
                continue;
            }
            if (dx != 0 && dy != 0) {
                const int horizontal = currentY * kTerrainColumns + nextX;
                const int vertical = nextY * kTerrainColumns + currentX;
                if (isBlocked(horizontal) || isBlocked(vertical)) {
                    continue;
                }
            }
            const auto nextIndexSize = static_cast<std::size_t>(nextIndex);
            const int newCost = current.cost +
                terrainTransitionCosts_[currentIndex][neighbor];
            if (newCost >= field.costToAnchor[nextIndexSize]) {
                continue;
            }
            field.costToAnchor[nextIndexSize] = newCost;
            field.towardAnchor[nextIndexSize] = current.index;
            open.push({newCost, nextIndex});
        }
    }
    return true;
}

std::vector<int> Simulation::flowChain(const FlowField& field, int node) const {
    std::vector<int> chain;
    if (node < 0 || node >= kNodeCount || field.anchor < 0) {
        return chain;
    }
    for (int guard = 0; guard <= kNodeCount; ++guard) {
        chain.push_back(node);
        if (node == field.anchor) {
            return chain;
        }
        const int next = field.towardAnchor[static_cast<std::size_t>(node)];
        if (next < 0 || next == node) {
            return {};
        }
        node = next;
    }
    return {};
}

bool Simulation::mergedFlowNodes(const FlowField& field, int startNode,
                                 int goalNode, std::vector<int>& nodes) const {
    nodes.clear();
    if (startNode < 0 || goalNode < 0) {
        return false;
    }
    if (startNode == goalNode) {
        return true;
    }
    const auto fromStart = flowChain(field, startNode);
    const auto fromGoal = flowChain(field, goalNode);
    if (fromStart.empty() || fromGoal.empty() ||
        fromStart.back() != field.anchor || fromGoal.back() != field.anchor) {
        return false;
    }

    std::vector<char> onGoal(static_cast<std::size_t>(kNodeCount), 0);
    for (const int node : fromGoal) {
        onGoal[static_cast<std::size_t>(node)] = 1;
    }
    std::size_t mergeIndex = 0;
    while (mergeIndex < fromStart.size() &&
           onGoal[static_cast<std::size_t>(fromStart[mergeIndex])] == 0) {
        ++mergeIndex;
    }
    if (mergeIndex == fromStart.size()) {
        return false;
    }

    for (std::size_t index = 1; index <= mergeIndex; ++index) {
        nodes.push_back(fromStart[index]);
    }
    const auto merge = std::find(fromGoal.begin(), fromGoal.end(),
                                 fromStart[mergeIndex]);
    if (merge == fromGoal.end()) {
        nodes.clear();
        return false;
    }
    for (auto step = merge; step != fromGoal.begin();) {
        --step;
        nodes.push_back(*step);
    }
    return true;
}

bool Simulation::formationOffsetPath(const std::vector<Vec2>& spine, Vec2 start,
                                     Vec2 slot, std::vector<Vec2>& path,
                                     Vec2& routedDestination) const {
    if (spine.empty()) {
        return false;
    }
    const MilliPoint startMilli = toMilli(start);
    const MilliPoint slotMilli = toMilli(slot);
    const MilliPoint spineFront = toMilli(spine.front());
    const MilliPoint spineBack = toMilli(spine.back());
    const std::int64_t startOffsetX =
        static_cast<std::int64_t>(startMilli.x) - spineFront.x;
    const std::int64_t startOffsetY =
        static_cast<std::int64_t>(startMilli.y) - spineFront.y;
    const std::int64_t endOffsetX =
        static_cast<std::int64_t>(slotMilli.x) - spineBack.x;
    const std::int64_t endOffsetY =
        static_cast<std::int64_t>(slotMilli.y) - spineBack.y;
    std::vector<Vec2> candidate;
    candidate.reserve(spine.size() + 1);
    MilliPoint previous = startMilli;
    const std::int64_t steps = static_cast<std::int64_t>(spine.size());
    for (std::size_t index = 0; index < spine.size(); ++index) {
        const MilliPoint spinePoint = toMilli(spine[index]);
        const std::int64_t progress = static_cast<std::int64_t>(index) + 1;
        const std::int64_t offsetX =
            startOffsetX + (endOffsetX - startOffsetX) * progress / steps;
        const std::int64_t offsetY =
            startOffsetY + (endOffsetY - startOffsetY) * progress / steps;
        const MilliPoint desired = clampMilli(
            static_cast<std::int64_t>(spinePoint.x) + offsetX,
            static_cast<std::int64_t>(spinePoint.y) + offsetY);
        bool placed = false;
        for (int step = 0; step <= 4; ++step) {
            const MilliPoint point{
                static_cast<std::int32_t>(
                    desired.x + (static_cast<std::int64_t>(spinePoint.x) - desired.x) *
                                    step / 4),
                static_cast<std::int32_t>(
                    desired.y + (static_cast<std::int64_t>(spinePoint.y) - desired.y) *
                                    step / 4)
            };
            const Vec2 world = fromMilli(point);
            if (blockedByTerrain(world) ||
                segmentBlockedByTerrain(fromMilli(previous), world)) {
                continue;
            }
            if (candidate.empty() || candidate.back().x != world.x ||
                candidate.back().y != world.y) {
                candidate.push_back(world);
            }
            previous = point;
            placed = true;
            break;
        }
        if (!placed) {
            return false;
        }
    }

    const Vec2 slotWorld = fromMilli(slotMilli);
    const Vec2 previousWorld = fromMilli(previous);
    if (!blockedByTerrain(slotWorld) &&
        !segmentBlockedByTerrain(previousWorld, slotWorld)) {
        if (candidate.empty() || candidate.back().x != slotWorld.x ||
            candidate.back().y != slotWorld.y) {
            candidate.push_back(slotWorld);
        }
        routedDestination = slotWorld;
    } else if (!candidate.empty()) {
        routedDestination = candidate.back();
    } else {
        return false;
    }
    path = std::move(candidate);
    return !path.empty();
}

bool Simulation::gridRouteCost(int startNode, const std::vector<int>& nodes,
                               std::uint64_t& cost) const {
    cost = 0;
    int previous = startNode;
    for (const int node : nodes) {
        const int direction = pathNeighborDirection(previous, node);
        if (direction < 0 || previous < 0 || previous >= kNodeCount) {
            return false;
        }
        cost += static_cast<std::uint64_t>(
            terrainTransitionCosts_[static_cast<std::size_t>(previous)][
                static_cast<std::size_t>(direction)]);
        previous = node;
    }
    return true;
}

std::uint64_t Simulation::polylineRouteCost(
    Vec2 start, const std::vector<Vec2>& path) const {
    std::uint64_t total = 0;
    MilliPoint previous = toMilli(start);
    for (const auto point : path) {
        const MilliPoint current = toMilli(point);
        const auto geometric = geometricMilliCost(
            static_cast<std::int64_t>(current.x) - previous.x,
            static_cast<std::int64_t>(current.y) - previous.y);
        const auto elevation = static_cast<std::uint64_t>(elevationTransitionCost(
            terrainElevationMilliunits(fromMilli(previous)),
            terrainElevationMilliunits(point)));
        total += geometric + elevation;
        previous = current;
    }
    return total;
}

void Simulation::routeFormation(Vec2 destination, std::size_t columns) {
    const Vec2 clamped{
        std::clamp(destination.x, 0.0F, kMapWidth),
        std::clamp(destination.y, 0.0F, kMapHeight)
    };
    // The anchor is the traversable cell nearest the clicked point. Probing from
    // a placed unit avoids rejecting an order whose click lies inside collision:
    // start resolution requires a clear approach, while goal resolution does not.
    PreparedPath anchorPrepared;
    if (units_.empty() ||
        !preparePath(units_.front().position, clamped, anchorPrepared,
                     lastPathfindingMetrics_)) {
        for (auto& unit : units_) {
            if (!unit.selected) {
                continue;
            }
            unit.path.clear();
            unit.nextWaypoint = 0;
            unit.destination = unit.position;
        }
        return;
    }

    struct Assignment {
        std::size_t unitIndex = 0;
        PreparedPath prepared;
        Vec2 slot;
        bool ready = false;
    };
    std::vector<Assignment> assignments;
    assignments.reserve(lastMoveCommand_.unitsOrdered);
    std::int64_t centroidX = 0;
    std::int64_t centroidY = 0;
    std::size_t selectedIndex = 0;
    for (std::size_t index = 0; index < units_.size(); ++index) {
        auto& unit = units_[index];
        if (!unit.selected) {
            continue;
        }
        Assignment assignment;
        assignment.unitIndex = index;
        assignment.slot = formationDestination(destination, columns, selectedIndex);
        assignment.ready = preparePath(unit.position, assignment.slot,
                                        assignment.prepared,
                                        lastPathfindingMetrics_);
        centroidX += Simulation::worldMilliunits(unit.position.x);
        centroidY += Simulation::worldMilliunits(unit.position.y);
        assignments.push_back(assignment);
        unit.path.clear();
        unit.nextWaypoint = 0;
        unit.destination = unit.position;
        ++selectedIndex;
    }
    const auto count = static_cast<std::int64_t>(assignments.size());
    const Vec2 centroid = fromMilli({
        static_cast<std::int32_t>(count == 0 ? 0 : centroidX / count),
        static_cast<std::int32_t>(count == 0 ? 0 : centroidY / count)
    });

    std::vector<char> needed(static_cast<std::size_t>(kNodeCount), 0);
    const auto watch = [&needed, anchor = anchorPrepared.goalNode](int node) {
        if (node < 0 || node == anchor || node >= kNodeCount) {
            return;
        }
        needed[static_cast<std::size_t>(node)] = 1;
    };
    for (const auto& assignment : assignments) {
        if (!assignment.ready) {
            continue;
        }
        watch(assignment.prepared.startNode);
        watch(assignment.prepared.goalNode);
    }
    PreparedPath centroidPrepared;
    const bool centroidReady = preparePath(
        centroid, clamped, centroidPrepared, lastPathfindingMetrics_);
    if (centroidReady) {
        watch(centroidPrepared.startNode);
    }

    FlowField field;
    if (!buildFlowField(anchorPrepared.goalNode, needed, field)) {
        return;
    }

    std::vector<Vec2> spine;
    if (centroidReady) {
        const auto chain = flowChain(field, centroidPrepared.startNode);
        if (!chain.empty() && chain.back() == field.anchor) {
            spine.reserve(chain.size());
            for (const int node : chain) {
                spine.push_back(pathCellCenter(node));
            }
        }
    }

    for (const auto& assignment : assignments) {
        auto& unit = units_[assignment.unitIndex];
        if (!assignment.ready) {
            continue;
        }
        std::vector<Vec2> offsetPath;
        Vec2 routedDestination;
        if (!spine.empty() &&
            formationOffsetPath(spine, unit.position, assignment.slot,
                                offsetPath, routedDestination)) {
            unit.path = std::move(offsetPath);
            unit.destination = routedDestination;
            lastPathfindingMetrics_.pathCost +=
                polylineRouteCost(unit.position, unit.path);
            ++lastPathfindingMetrics_.formationPaths;
            ++lastMoveCommand_.unitsRouted;
            continue;
        }

        std::vector<int> nodes;
        std::uint64_t cost = 0;
        if (mergedFlowNodes(field, assignment.prepared.startNode,
                            assignment.prepared.goalNode, nodes) &&
            gridRouteCost(assignment.prepared.startNode, nodes, cost)) {
            finishPath(assignment.prepared, nodes, unit.path, unit.destination);
            lastPathfindingMetrics_.pathCost += cost;
            ++lastPathfindingMetrics_.formationPaths;
            ++lastMoveCommand_.unitsRouted;
            continue;
        }

        if (findPath(unit.position, assignment.slot, unit.path, unit.destination,
                     lastPathfindingMetrics_, PathfindingAlgorithm::AStar)) {
            ++lastPathfindingMetrics_.formationFallbacks;
            ++lastMoveCommand_.unitsRouted;
        }
    }
}

} // namespace rts
