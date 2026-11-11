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

constexpr float kElevationCostPerUnit = 50.0F;
constexpr std::array<std::pair<int, int>, 8> kPathNeighbors{{
    {-1, -1}, {0, -1}, {1, -1}, {-1, 0},
    {1, 0}, {-1, 1}, {0, 1}, {1, 1}
}};
constexpr std::array<int, 8> kPathGeometricCosts{{
    1414, 1000, 1414, 1000, 1000, 1414, 1000, 1414
}};

Vec2 formationDestination(Vec2 destination, std::size_t columns,
                          std::size_t index) {
    const float center = static_cast<float>(columns - 1) * 0.5F;
    return {
        std::clamp(destination.x +
                       (static_cast<float>(index % columns) - center) * 14.0F,
                   0.0F, Simulation::kMapWidth),
        std::clamp(destination.y +
                       (static_cast<float>(index / columns) - center) * 14.0F,
                   0.0F, Simulation::kMapHeight)
    };
}

float generatedTerrainElevation(float x, float z) {
    const auto hill = [x, z](float centerX, float centerZ, float radiusX,
                             float radiusZ, float height) {
        const float dx = (x - centerX) / radiusX;
        const float dz = (z - centerZ) / radiusZ;
        return height * std::exp(-0.5F * (dx * dx + dz * dz));
    };
    return 3.0F +
        hill(700.0F, 470.0F, 240.0F, 190.0F, 27.0F) +
        hill(1370.0F, 930.0F, 280.0F, 220.0F, 34.0F) +
        hill(1080.0F, 260.0F, 170.0F, 140.0F, 16.0F);
}

} // namespace

Simulation::Simulation(std::size_t unitCount) {
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
    terrainVertexElevations_.resize(
        static_cast<std::size_t>((pathColumns + 1) * (pathRows + 1)));
    for (int row = 0; row <= pathRows; ++row) {
        for (int column = 0; column <= pathColumns; ++column) {
            const float x = static_cast<float>(column) * pathCellSize;
            const float z = static_cast<float>(row) * pathCellSize;
            terrainVertexElevations_[static_cast<std::size_t>(
                row * (pathColumns + 1) + column)] =
                generatedTerrainElevation(x, z);
        }
    }
    terrainCellElevations_.resize(
        static_cast<std::size_t>(pathColumns * pathRows));
    for (int row = 0; row < pathRows; ++row) {
        for (int column = 0; column < pathColumns; ++column) {
            const Vec2 center{
                (static_cast<float>(column) + 0.5F) * pathCellSize,
                (static_cast<float>(row) + 0.5F) * pathCellSize
            };
            terrainCellElevations_[static_cast<std::size_t>(
                row * pathColumns + column)] = terrainElevation(center);
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
                const float elevationDifference = std::abs(
                    terrainCellElevations_[nextIndex] -
                    terrainCellElevations_[cellIndex]);
                const int elevationCost = static_cast<int>(
                    std::ceil(elevationDifference * kElevationCostPerUnit));
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
        units_.push_back(std::move(unit));
    }
}

float Simulation::terrainElevation(Vec2 position) const noexcept {
    const float gridX = std::clamp(position.x, 0.0F, kMapWidth) /
                        kTerrainCellSize;
    const float gridZ = std::clamp(position.y, 0.0F, kMapHeight) /
                        kTerrainCellSize;
    const int column = std::min(static_cast<int>(gridX), kTerrainColumns - 1);
    const int row = std::min(static_cast<int>(gridZ), kTerrainRows - 1);
    const int nextColumn = column + 1;
    const int nextRow = row + 1;
    const float xFraction = gridX - static_cast<float>(column);
    const float zFraction = gridZ - static_cast<float>(row);
    const auto vertex = [this](int x, int z) {
        return terrainVertexElevations_[static_cast<std::size_t>(
            z * (kTerrainColumns + 1) + x)];
    };
    const float top = vertex(column, row) * (1.0F - xFraction) +
                      vertex(nextColumn, row) * xFraction;
    const float bottom = vertex(column, nextRow) * (1.0F - xFraction) +
                         vertex(nextColumn, nextRow) * xFraction;
    return top * (1.0F - zFraction) + bottom * zFraction;
}

void Simulation::step() {
    constexpr float tickDistance = kUnitSpeed * kFixedStepSeconds;
    for (auto& unit : units_) {
        float remainingDistance = tickDistance;
        while (remainingDistance > 0.0F && unit.nextWaypoint < unit.path.size()) {
            const auto waypoint = unit.path[unit.nextWaypoint];
            const float dx = waypoint.x - unit.position.x;
            const float dy = waypoint.y - unit.position.y;
            const float distanceSquared = dx * dx + dy * dy;
            if (distanceSquared <= remainingDistance * remainingDistance) {
                unit.position = waypoint;
                remainingDistance -= std::sqrt(distanceSquared);
                ++unit.nextWaypoint;
                continue;
            }

            const float distance = std::sqrt(distanceSquared);
            const Vec2 next{
                unit.position.x + dx * remainingDistance / distance,
                unit.position.y + dy * remainingDistance / distance
            };
            if (blockedByTerrain(next)) {
                unit.path.clear();
                unit.nextWaypoint = 0;
                unit.destination = unit.position;
                break;
            }
            unit.position = next;
            remainingDistance = 0.0F;
        }
    }
    ++tickCount_;
}

bool Simulation::selectNearest(Vec2 position, float radius) {
    clearSelection();

    const float radiusSquared = radius * radius;
    float nearestDistanceSquared = radiusSquared;
    Unit* nearest = nullptr;
    for (auto& unit : units_) {
        const float dx = unit.position.x - position.x;
        const float dy = unit.position.y - position.y;
        const float distanceSquared = dx * dx + dy * dy;
        if (distanceSquared <= nearestDistanceSquared) {
            nearestDistanceSquared = distanceSquared;
            nearest = &unit;
        }
    }
    if (nearest == nullptr) {
        return false;
    }
    nearest->selected = true;
    return true;
}

bool Simulation::selectUnit(std::uint32_t id) {
    clearSelection();
    const auto unit = std::find_if(units_.begin(), units_.end(),
        [id](const Unit& candidate) { return candidate.id == id; });
    if (unit == units_.end()) {
        return false;
    }
    unit->selected = true;
    return true;
}

std::size_t Simulation::selectUnits(const std::vector<std::uint32_t>& ids) {
    const std::unordered_set<std::uint32_t> selectedIds(ids.begin(), ids.end());
    std::size_t count = 0;
    for (auto& unit : units_) {
        unit.selected = selectedIds.contains(unit.id);
        count += unit.selected ? 1U : 0U;
    }
    return count;
}

void Simulation::clearSelection() noexcept {
    for (auto& unit : units_) {
        unit.selected = false;
    }
}

std::size_t Simulation::moveSelected(Vec2 destination,
                                     PathfindingAlgorithm algorithm) {
    const auto count = selectedCount();
    lastMoveCommand_ = {true, destination, count, 0, tickCount_};
    lastPathfindingMetrics_ = {};
    if (count == 0) {
        return 0;
    }

    const auto columns = static_cast<std::size_t>(
        std::ceil(std::sqrt(static_cast<double>(count))));
    std::size_t index = 0;
    for (auto& unit : units_) {
        if (!unit.selected) {
            continue;
        }
        const auto target = formationDestination(destination, columns, index);
        unit.path.clear();
        unit.nextWaypoint = 0;
        if (findPath(unit.position, target, unit.path, unit.destination,
                     lastPathfindingMetrics_, algorithm)) {
            ++lastMoveCommand_.unitsRouted;
        } else {
            unit.destination = unit.position;
        }
        ++index;
    }
    return count;
}

std::size_t Simulation::moveSelected(Vec2 destination, GridPathBackend& backend) {
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

    const auto results = count == 0 ? std::vector<GridPathResult>{} :
        backend.search(blockedPathCells_, terrainCellElevations_,
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
    const float dx = end.x - start.x;
    const float dy = end.y - start.y;
    const auto samples = static_cast<int>(
        std::ceil(std::max(std::abs(dx), std::abs(dy)) / 2.0F));
    for (int i = 1; i <= samples; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(samples);
        if (blockedByTerrain({start.x + dx * t, start.y + dy * t})) {
            return true;
        }
    }
    return false;
}

bool Simulation::preparePath(Vec2 start, Vec2 requestedDestination,
                             PreparedPath& prepared,
                             PathfindingMetrics& metrics) const {
    constexpr float cellSize = kTerrainCellSize;
    constexpr int columns = kTerrainColumns;
    constexpr int rows = kTerrainRows;
    const auto cellCenter = [](int index) {
        const int x = index % columns;
        const int y = index / columns;
        return Vec2{(static_cast<float>(x) + 0.5F) * cellSize,
                    (static_cast<float>(y) + 0.5F) * cellSize};
    };
    const auto cellForPosition = [](Vec2 position) {
        const int x = std::clamp(static_cast<int>(position.x / cellSize),
                                 0, columns - 1);
        const int y = std::clamp(static_cast<int>(position.y / cellSize),
                                 0, rows - 1);
        return y * columns + x;
    };
    const auto isBlocked = [this](int index) {
        return blockedPathCells_[static_cast<std::size_t>(index)] != 0;
    };

    int startNode = cellForPosition(start);
    const int positionStartNode = startNode;
    if (isBlocked(startNode)) {
        float nearestStartDistance = std::numeric_limits<float>::max();
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
                    const auto point = cellCenter(index);
                    const float dx = point.x - start.x;
                    const float dy = point.y - start.y;
                    const float distanceSquared = dx * dx + dy * dy;
                    if ((distanceSquared < nearestStartDistance ||
                         (distanceSquared == nearestStartDistance &&
                          index < accessibleStartNode)) &&
                        !segmentBlockedByTerrain(start, point)) {
                        nearestStartDistance = distanceSquared;
                        accessibleStartNode = index;
                    }
                }
            }

            float nearestUnsearchedAxisDistance = std::numeric_limits<float>::max();
            const int leftX = minX - 1;
            const int rightX = maxX + 1;
            const int topY = minY - 1;
            const int bottomY = maxY + 1;
            if (leftX >= 0) {
                nearestUnsearchedAxisDistance = std::min(
                    nearestUnsearchedAxisDistance,
                    std::abs((static_cast<float>(leftX) + 0.5F) * cellSize - start.x));
            }
            if (rightX < columns) {
                nearestUnsearchedAxisDistance = std::min(
                    nearestUnsearchedAxisDistance,
                    std::abs((static_cast<float>(rightX) + 0.5F) * cellSize - start.x));
            }
            if (topY >= 0) {
                nearestUnsearchedAxisDistance = std::min(
                    nearestUnsearchedAxisDistance,
                    std::abs((static_cast<float>(topY) + 0.5F) * cellSize - start.y));
            }
            if (bottomY < rows) {
                nearestUnsearchedAxisDistance = std::min(
                    nearestUnsearchedAxisDistance,
                    std::abs((static_cast<float>(bottomY) + 0.5F) * cellSize - start.y));
            }
            if (accessibleStartNode >= 0 &&
                nearestStartDistance < nearestUnsearchedAxisDistance *
                                           nearestUnsearchedAxisDistance) {
                break;
            }
        }
        if (accessibleStartNode < 0) {
            return false;
        }
        startNode = accessibleStartNode;
    }
    int goalNode = -1;
    float nearestGoalDistance = std::numeric_limits<float>::max();
    const int destinationX = cellForPosition(requestedDestination) % columns;
    const int destinationY = cellForPosition(requestedDestination) / columns;
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
                const auto point = cellCenter(index);
                const float dx = point.x - requestedDestination.x;
                const float dy = point.y - requestedDestination.y;
                const float distanceSquared = dx * dx + dy * dy;
                if (distanceSquared < nearestGoalDistance ||
                    (distanceSquared == nearestGoalDistance && index < goalNode)) {
                    nearestGoalDistance = distanceSquared;
                    goalNode = index;
                }
            }
        }

        float nearestUnsearchedAxisDistance = std::numeric_limits<float>::max();
        const int leftX = minX - 1;
        const int rightX = maxX + 1;
        const int topY = minY - 1;
        const int bottomY = maxY + 1;
        if (leftX >= 0) {
            nearestUnsearchedAxisDistance = std::min(
                nearestUnsearchedAxisDistance,
                std::abs((static_cast<float>(leftX) + 0.5F) * cellSize -
                         requestedDestination.x));
        }
        if (rightX < columns) {
            nearestUnsearchedAxisDistance = std::min(
                nearestUnsearchedAxisDistance,
                std::abs((static_cast<float>(rightX) + 0.5F) * cellSize -
                         requestedDestination.x));
        }
        if (topY >= 0) {
            nearestUnsearchedAxisDistance = std::min(
                nearestUnsearchedAxisDistance,
                std::abs((static_cast<float>(topY) + 0.5F) * cellSize -
                         requestedDestination.y));
        }
        if (bottomY < rows) {
            nearestUnsearchedAxisDistance = std::min(
                nearestUnsearchedAxisDistance,
                std::abs((static_cast<float>(bottomY) + 0.5F) * cellSize -
                         requestedDestination.y));
        }
        if (goalNode >= 0 &&
            nearestGoalDistance < nearestUnsearchedAxisDistance *
                                      nearestUnsearchedAxisDistance) {
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
        const float elevationDifference = std::abs(
            terrainCellElevations_[static_cast<std::size_t>(from)] -
            terrainCellElevations_[static_cast<std::size_t>(to)]);
        const int unavoidableElevationCost = static_cast<int>(
            elevationDifference * kElevationCostPerUnit);
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
    const auto cellCenter = [](int index) {
        const int x = index % kTerrainColumns;
        const int y = index / kTerrainColumns;
        return Vec2{(static_cast<float>(x) + 0.5F) * kTerrainCellSize,
                    (static_cast<float>(y) + 0.5F) * kTerrainCellSize};
    };
    const auto start = prepared.start;
    const auto requestedDestination = prepared.requestedDestination;
    path.clear();
    path.reserve(nodes.size() + 2);
    if (prepared.startNode != prepared.positionStartNode) {
        path.push_back(cellCenter(prepared.startNode));
    }
    for (const int node : nodes) {
        path.push_back(cellCenter(node));
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

} // namespace rts
