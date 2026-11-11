#include "rts/Camera3D.hpp"
#include "rts/SceneGeometry.hpp"
#include "rts/ScreenSelection.hpp"
#include "rts/Simulation.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

bool isTerrainClear(const rts::Simulation& simulation, rts::Vec2 point) {
    constexpr float clearance = 6.5F;
    for (const auto& obstacle : simulation.terrainObstacles()) {
        if (point.x >= obstacle.minimum.x - clearance &&
            point.x <= obstacle.maximum.x + clearance &&
            point.y >= obstacle.minimum.y - clearance &&
            point.y <= obstacle.maximum.y + clearance) {
            return false;
        }
    }
    return true;
}

bool isTerrainClear(const rts::Simulation& simulation, rts::Vec2 start,
                    rts::Vec2 end) {
    const float dx = end.x - start.x;
    const float dy = end.y - start.y;
    const int samples = static_cast<int>(
        std::ceil(std::max(std::abs(dx), std::abs(dy))));
    for (int sample = 0; sample <= samples; ++sample) {
        const float fraction = samples == 0
            ? 0.0F
            : static_cast<float>(sample) / static_cast<float>(samples);
        if (!isTerrainClear(simulation,
                {start.x + dx * fraction, start.y + dy * fraction})) {
            return false;
        }
    }
    return true;
}

void testInitialPopulation() {
    const rts::Simulation simulation;
    require(simulation.units().size() == 1000, "default battlefield has 1,000 units");
    require(simulation.tickCount() == 0, "new simulation starts at tick zero");
    require(simulation.selectedCount() == 0, "new simulation has no selected units");
}

void testSceneGeometryScaling() {
    const rts::Simulation oneUnit(1);
    const rts::Simulation twoUnits(2);
    const auto oneUnitScene = rts::buildSceneGeometry(oneUnit);
    const auto twoUnitScene = rts::buildSceneGeometry(twoUnits);
    require(twoUnitScene.size() - oneUnitScene.size() == 90,
            "each unselected unit contributes 90 scene vertices");

    const auto overlay = rts::buildSelectionOverlayGeometry(
        {10.0F, 20.0F, 100.0F, 120.0F});
    require(overlay.size() == 24,
            "selection rectangle overlay contains four quads");
}

void testSelectionAndMovement() {
    rts::Simulation simulation(2);
    const auto firstPosition = simulation.units()[0].position;
    const auto secondPosition = simulation.units()[1].position;
    require(simulation.selectNearest(firstPosition, 1.0F), "unit can be selected by position");
    require(simulation.selectedCount() == 1, "selection contains one unit");
    require(simulation.moveSelected({firstPosition.x + 100.0F, firstPosition.y}) == 1,
            "move command is applied to selected unit");
    simulation.step();
    require(simulation.units()[0].position.x > firstPosition.x, "selected unit moves toward target");
    require(simulation.units()[1].position.x == secondPosition.x, "unselected unit does not move");
    require(simulation.tickCount() == 1, "simulation advances one fixed tick");
}

void testSelectionMissClearsSelection() {
    rts::Simulation simulation(1);
    const auto position = simulation.units()[0].position;
    require(simulation.selectNearest(position, 1.0F), "unit is initially selectable");
    require(!simulation.selectNearest({-1000.0F, -1000.0F}, 1.0F),
            "selection misses outside the requested radius");
    require(simulation.selectedCount() == 0, "miss clears previous selection");
}

void testMovementStopsAtDestination() {
    rts::Simulation simulation(1);
    const auto start = simulation.units()[0].position;
    const rts::Vec2 destination{start.x + 10.0F, start.y};
    simulation.selectNearest(start, 1.0F);
    simulation.moveSelected(destination);
    for (int i = 0; i < 10; ++i) {
        simulation.step();
    }
    require(std::abs(simulation.units()[0].position.x - destination.x) < 0.001F,
            "unit reaches its destination without overshooting");
    require(std::abs(simulation.units()[0].position.y - destination.y) < 0.001F,
            "unit reaches the destination on both axes");
}

void testDeterministicSimulation() {
    rts::Simulation first(1000);
    rts::Simulation second(1000);
    const auto start = first.units()[0].position;
    require(first.selectNearest(start, 1.0F), "first simulation selects initial unit");
    require(second.selectNearest(start, 1.0F), "second simulation selects same initial unit");
    first.moveSelected({800.0F, 600.0F});
    second.moveSelected({800.0F, 600.0F});
    for (int i = 0; i < 90; ++i) {
        first.step();
        second.step();
    }
    require(first.tickCount() == second.tickCount(), "tick counts match for equal command streams");
    for (std::size_t i = 0; i < first.units().size(); ++i) {
        require(first.units()[i].position.x == second.units()[i].position.x &&
                first.units()[i].position.y == second.units()[i].position.y,
                "unit states match for equal command streams");
    }
}

void testSelectionById() {
    rts::Simulation simulation(3);
    require(simulation.selectUnit(2), "unit can be selected by stable ID");
    require(simulation.units()[1].selected, "selected ID maps to the matching unit");
    require(!simulation.selectUnit(99), "unknown unit ID is rejected");
    require(simulation.selectedCount() == 0, "unknown ID clears the previous selection");
}

void testMovementStaysInsideBattlefield() {
    rts::Simulation simulation(1);
    simulation.selectUnit(1);
    require(simulation.moveSelected({-50.0F, 5000.0F}) == 1,
            "out-of-bounds destination is accepted and bounded");
    require(simulation.units()[0].destination.x == 0.0F,
            "destination x is clamped to the map");
    require(simulation.units()[0].destination.y == rts::Simulation::kMapHeight,
            "destination z is clamped to the map");
}

void testMultiUnitOrderFeedback() {
    rts::Simulation simulation(4);
    require(simulation.selectUnits({1, 2, 4}) == 3,
            "multiple stable unit IDs can be selected together");
    require(simulation.moveSelected({600.0F, 500.0F}) == 3,
            "multi-unit move command applies to the whole selection");

    const auto& feedback = simulation.lastMoveCommand();
    require(feedback.valid && feedback.unitsOrdered == 3 &&
            feedback.unitsRouted == 3,
            "move feedback reports ordered and successfully routed units");
    require(feedback.requestedDestination.x == 600.0F &&
            feedback.requestedDestination.y == 500.0F &&
            feedback.issuedAtTick == 0,
            "move feedback retains the requested target and issue tick");
    require(!simulation.units()[2].selected,
            "multi-unit selection does not select unrequested units");
}

void testFullPopulationRouting() {
    rts::Simulation simulation(1000);
    std::vector<std::uint32_t> unitIds;
    unitIds.reserve(simulation.units().size());
    for (const auto& unit : simulation.units()) {
        unitIds.push_back(unit.id);
    }

    require(simulation.selectUnits(unitIds) == unitIds.size(),
            "all starting units can be selected together");
    simulation.moveSelected({700.0F, 600.0F});
    require(simulation.lastMoveCommand().unitsOrdered == unitIds.size() &&
            simulation.lastMoveCommand().unitsRouted == unitIds.size(),
            "all starting units receive terrain-aware routes");
}

void testPathfindingMetrics() {
    rts::Simulation first(1000);
    rts::Simulation second(1000);
    require(first.selectUnits({1, 2, 127}) == 3 &&
            second.selectUnits({1, 2, 127}) == 3,
            "metrics scenario selects the same units in both simulations");
    first.moveSelected({1800.0F, 1300.0F});
    second.moveSelected({1800.0F, 1300.0F});

    const auto& metrics = first.lastPathfindingMetrics();
    const auto& repeatedMetrics = second.lastPathfindingMetrics();
    require(metrics.goalCellsExamined > 0 &&
            metrics.aStarNodesExpanded > 0,
            "move command records goal lookup and A* expansion work");
    require(metrics.goalCellsExamined == repeatedMetrics.goalCellsExamined &&
            metrics.startCellsExamined == repeatedMetrics.startCellsExamined &&
            metrics.aStarNodesExpanded == repeatedMetrics.aStarNodesExpanded,
            "pathfinding work metrics are deterministic for the same orders");

    first.clearSelection();
    first.moveSelected({1000.0F, 700.0F});
    require(first.lastPathfindingMetrics().goalCellsExamined == 0 &&
            first.lastPathfindingMetrics().startCellsExamined == 0 &&
            first.lastPathfindingMetrics().aStarNodesExpanded == 0 &&
            first.lastPathfindingMetrics().dijkstraNodesExpanded == 0 &&
            first.lastPathfindingMetrics().pathCost == 0,
            "pathfinding metrics reset when no units are ordered");
}

void testDijkstraMatchesAStarRouteCost() {
    rts::Simulation aStar;
    rts::Simulation dijkstra;
    const std::vector<std::uint32_t> unitIds{1, 127, 400, 1000};
    require(std::abs(aStar.terrainElevation({700.0F, 470.0F}) -
                     aStar.terrainElevation({100.0F, 100.0F})) > 10.0F,
            "terrain elevation varies across the battlefield");
    require(aStar.selectUnits(unitIds) == unitIds.size() &&
            dijkstra.selectUnits(unitIds) == unitIds.size(),
            "both algorithms select the same units");
    aStar.moveSelected({1800.0F, 1300.0F});
    dijkstra.moveSelected({1800.0F, 1300.0F},
                          rts::PathfindingAlgorithm::Dijkstra);

    require(aStar.lastMoveCommand().unitsRouted == unitIds.size() &&
            dijkstra.lastMoveCommand().unitsRouted == unitIds.size(),
            "both algorithms route every selected unit");
    require(aStar.lastPathfindingMetrics().pathCost ==
                dijkstra.lastPathfindingMetrics().pathCost,
            "A* and Dijkstra find equal-cost paths on elevated terrain");
    require(aStar.lastPathfindingMetrics().aStarNodesExpanded > 0 &&
            aStar.lastPathfindingMetrics().dijkstraNodesExpanded == 0 &&
            dijkstra.lastPathfindingMetrics().dijkstraNodesExpanded > 0 &&
            dijkstra.lastPathfindingMetrics().aStarNodesExpanded == 0,
            "each algorithm records its own expanded-node count");
    std::uint64_t flatGridLowerBound = 0;
    for (const auto unitId : unitIds) {
        const auto& aStarUnit = aStar.units()[unitId - 1];
        const auto& dijkstraUnit = dijkstra.units()[unitId - 1];
        require(aStarUnit.destination.x == dijkstraUnit.destination.x &&
                aStarUnit.destination.y == dijkstraUnit.destination.y,
                "both algorithms choose the same routed destination");
        const int startX = static_cast<int>(
            aStarUnit.position.x / rts::Simulation::kTerrainCellSize);
        const int startY = static_cast<int>(
            aStarUnit.position.y / rts::Simulation::kTerrainCellSize);
        const int goalX = std::min(
            static_cast<int>(aStarUnit.destination.x /
                             rts::Simulation::kTerrainCellSize),
            rts::Simulation::kTerrainColumns - 1);
        const int goalY = std::min(
            static_cast<int>(aStarUnit.destination.y /
                             rts::Simulation::kTerrainCellSize),
            rts::Simulation::kTerrainRows - 1);
        const int dx = std::abs(goalX - startX);
        const int dy = std::abs(goalY - startY);
        const int diagonal = std::min(dx, dy);
        flatGridLowerBound += static_cast<std::uint64_t>(
            diagonal * 1414 + (std::max(dx, dy) - diagonal) * 1000);
    }
    require(aStar.lastPathfindingMetrics().pathCost > flatGridLowerBound,
            "elevation-aware route cost includes more than flat-grid movement cost");
}

void testElevationEdgeCostsAreReported() {
    rts::Simulation simulation(1000);
    require(simulation.selectUnit(1), "elevation-cost scenario selects its unit");
    require(simulation.moveSelected(
                {1800.0F, 1300.0F}, rts::PathfindingAlgorithm::AStar) == 1 &&
            simulation.lastMoveCommand().unitsRouted == 1,
            "elevation-cost scenario is routed");
    require(simulation.lastPathfindingMetrics().startCellsExamined == 0,
            "elevation-cost scenario begins in a traversable cell");

    const auto cellFor = [](float position, int cellCount) {
        return std::clamp(
            static_cast<int>(position / rts::Simulation::kTerrainCellSize),
            0, cellCount - 1);
    };
    const auto cellCenter = [](int x, int y) {
        return rts::Vec2{
            (static_cast<float>(x) + 0.5F) * rts::Simulation::kTerrainCellSize,
            (static_cast<float>(y) + 0.5F) * rts::Simulation::kTerrainCellSize
        };
    };
    const auto& unit = simulation.units()[0];
    int previousX = cellFor(unit.position.x,
                            rts::Simulation::kTerrainColumns);
    int previousY = cellFor(unit.position.y,
                            rts::Simulation::kTerrainRows);
    std::uint64_t expectedCost = 0;
    std::size_t elevationTransitions = 0;
    for (const auto waypoint : unit.path) {
        const int nextX = cellFor(waypoint.x,
                                  rts::Simulation::kTerrainColumns);
        const int nextY = cellFor(waypoint.y,
                                  rts::Simulation::kTerrainRows);
        const float centerX =
            (static_cast<float>(nextX) + 0.5F) *
            rts::Simulation::kTerrainCellSize;
        const float centerY =
            (static_cast<float>(nextY) + 0.5F) *
            rts::Simulation::kTerrainCellSize;
        if (waypoint.x != centerX || waypoint.y != centerY) {
            break;
        }
        const int dx = std::abs(nextX - previousX);
        const int dy = std::abs(nextY - previousY);
        if (dx == 0 && dy == 0) {
            continue;
        }
        require(dx <= 1 && dy <= 1,
                "elevation route traverses adjacent grid cells");
        const auto previousElevation =
            simulation.terrainElevation(cellCenter(previousX, previousY));
        const auto nextElevation =
            simulation.terrainElevation(cellCenter(nextX, nextY));
        const auto elevationCost = static_cast<std::uint64_t>(std::ceil(
            std::abs(nextElevation - previousElevation) * 50.0F));
        elevationTransitions += elevationCost > 0 ? 1U : 0U;
        expectedCost += (dx != 0 && dy != 0 ? 1414U : 1000U) +
                        elevationCost;
        previousX = nextX;
        previousY = nextY;
    }

    require(elevationTransitions > 0,
            "selected route crosses cells with different elevation");
    require(simulation.lastPathfindingMetrics().pathCost == expectedCost,
            "reported route cost includes every edge's elevation penalty (actual=" +
                std::to_string(simulation.lastPathfindingMetrics().pathCost) +
                ", expected=" + std::to_string(expectedCost) + ")");
}

void testBatchedPathBackend() {
    class AdjacentBackend final : public rts::GridPathBackend {
    public:
        bool invalid = false;
        std::size_t calls = 0;
        std::vector<rts::GridPathResult> search(
            const std::vector<std::uint8_t>&,
            const std::vector<float>&,
            const std::vector<std::array<int, 8>>& costs,
            const std::vector<rts::GridPathQuery>& queries) override {
            ++calls;
            std::vector<rts::GridPathResult> results;
            for (const auto& query : queries) {
                if (invalid) {
                    results.push_back({true, 0, {query.goalNode + 2}});
                } else {
                    results.push_back({
                        true,
                        static_cast<std::uint64_t>(
                            costs[static_cast<std::size_t>(query.startNode)][4]),
                        {query.goalNode}
                    });
                }
            }
            return results;
        }
    } backend;

    rts::Simulation cpu(1);
    rts::Simulation batched(1);
    require(batched.moveSelected({410.0F, 250.0F}, backend) == 0 &&
            backend.calls == 0 && batched.lastMoveCommand().unitsRouted == 0,
            "an empty selection does not dispatch the backend");
    require(cpu.selectUnit(1) && batched.selectUnit(1),
            "batched route scenario selects its unit");
    const rts::Vec2 target{410.0F, 250.0F};
    cpu.moveSelected(target);
    batched.moveSelected(target, backend);
    require(batched.lastMoveCommand().unitsRouted == 1 &&
            cpu.lastPathfindingMetrics().pathCost ==
                batched.lastPathfindingMetrics().pathCost &&
            cpu.units()[0].destination.x == batched.units()[0].destination.x &&
            cpu.units()[0].destination.y == batched.units()[0].destination.y,
            "batched backend routes and reconstructs the same adjacent goal");

    backend.invalid = true;
    bool rejected = false;
    try {
        batched.moveSelected(target, backend);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    require(rejected && batched.lastMoveCommand().unitsRouted == 1 &&
            batched.units()[0].destination.x == cpu.units()[0].destination.x,
            "invalid backend routes are rejected without applying the order");
}

void testScreenRectangleSelection() {
    rts::Simulation simulation(4);
    rts::Camera3D camera;
    camera.setViewport(1280.0F, 800.0F);
    for (const bool perspective : {false, true}) {
        camera.perspective = perspective;
        camera.focusAt(500.0F, 350.0F);

        const auto projectedUnit = [&simulation, &camera](std::size_t index) {
            const auto& unit = simulation.units()[index];
            return camera.project({
                unit.position.x,
                simulation.terrainElevation(unit.position) + 8.5F,
                unit.position.y
            });
        };
        const auto first = projectedUnit(0);
        const auto second = projectedUnit(1);
        const auto third = projectedUnit(2);
        const rts::Vec2 firstCorner{first.x - 2.0F, first.y - 2.0F};
        const rts::Vec2 secondCorner{second.x + 2.0F, second.y + 2.0F};
        require(rts::selectInScreenRectangle(simulation, camera,
                    firstCorner, secondCorner) == 2,
                "screen rectangle selects each projected unit inside its bounds");
        require(simulation.units()[0].selected && simulation.units()[1].selected &&
                !simulation.units()[2].selected,
                "screen rectangle selection excludes projected units outside its bounds");

        require(rts::selectInScreenRectangle(simulation, camera,
                    secondCorner, firstCorner) == 2,
                "screen rectangle selection works when dragged in reverse");
    }
}

void testTerrainAwarePathfinding() {
    rts::Simulation simulation(1000);
    const rts::Unit* approachUnit = nullptr;
    const rts::TerrainObstacle* blockingObstacle = nullptr;
    constexpr float clearance = 6.5F;

    for (const auto& obstacle : simulation.terrainObstacles()) {
        for (const auto& unit : simulation.units()) {
            if (unit.position.x < obstacle.minimum.x - clearance - 20.0F &&
                unit.position.y >= obstacle.minimum.y - clearance &&
                unit.position.y <= obstacle.maximum.y + clearance) {
                approachUnit = &unit;
                blockingObstacle = &obstacle;
                break;
            }
        }
        if (approachUnit != nullptr) {
            break;
        }
    }
    require(approachUnit != nullptr && blockingObstacle != nullptr,
            "terrain scenario has a unit approaching an obstacle");

    const auto unitId = approachUnit->id;
    const auto start = approachUnit->position;
    require(simulation.selectUnit(unitId), "terrain scenario unit can be selected");
    require(simulation.moveSelected(
                {blockingObstacle->maximum.x + 50.0F, start.y}) == 1,
            "unit receives a move order across the obstacle");
    const auto& orderedUnit = simulation.units()[unitId - 1];
    require(!orderedUnit.path.empty(),
            "terrain-crossing move order creates a waypoint route");
    require(simulation.lastMoveCommand().unitsRouted == 1,
            "terrain-crossing move order reports a successful route");

    bool deviatedAroundObstacle = false;
    for (int tick = 0; tick < 500; ++tick) {
        simulation.step();
        const auto& position = simulation.units()[unitId - 1].position;
        if (std::abs(position.y - start.y) > 10.0F) {
            deviatedAroundObstacle = true;
        }
    }

    const auto& unit = simulation.units()[unitId - 1];
    require(deviatedAroundObstacle,
            "unit follows a detour around terrain instead of stopping at it");
    require(unit.nextWaypoint == unit.path.size(),
            "unit completes its path after reaching the final destination");
    require(std::abs(unit.position.x - unit.destination.x) < 0.01F &&
            std::abs(unit.position.y - unit.destination.y) < 0.01F,
            "unit reaches its routed destination without overshooting");
}

void testRouteQualityAcrossDestinations() {
    const std::vector<rts::PathfindingAlgorithm> algorithms{
        rts::PathfindingAlgorithm::AStar,
        rts::PathfindingAlgorithm::Dijkstra
    };
    const std::vector<std::uint32_t> unitIds{1, 127, 400, 1000};
    std::vector<std::uint64_t> aStarRouteCosts;
    std::vector<std::uint64_t> dijkstraRouteCosts;
    for (const auto algorithm : algorithms) {
        rts::Simulation simulation(1000);
        const auto& obstacle = simulation.terrainObstacles().front();
        const std::vector<rts::Vec2> destinations{
            {520.0F, 350.0F},
            {1000.0F, 700.0F},
            {1800.0F, 1300.0F},
            {0.0F, 0.0F},
            {rts::Simulation::kMapWidth, rts::Simulation::kMapHeight},
            {(obstacle.minimum.x + obstacle.maximum.x) * 0.5F,
             (obstacle.minimum.y + obstacle.maximum.y) * 0.5F}
        };

        for (const auto unitId : unitIds) {
            for (const auto requestedDestination : destinations) {
                require(simulation.selectUnit(unitId),
                        "route-quality scenario selects its unit");
                require(simulation.moveSelected(requestedDestination, algorithm) == 1 &&
                        simulation.lastMoveCommand().unitsRouted == 1,
                        "route-quality scenario produces a route");
                if (algorithm == rts::PathfindingAlgorithm::AStar) {
                    aStarRouteCosts.push_back(
                        simulation.lastPathfindingMetrics().pathCost);
                } else {
                    dijkstraRouteCosts.push_back(
                        simulation.lastPathfindingMetrics().pathCost);
                }

                const auto& orderedUnit = simulation.units()[unitId - 1];
                const auto originalPathSize = orderedUnit.path.size();
                require(originalPathSize > 0,
                        "route-quality command preserves at least one waypoint");
                rts::Vec2 previous = orderedUnit.position;
                for (const auto waypoint : orderedUnit.path) {
                    require(isTerrainClear(simulation, previous, waypoint),
                            "every waypoint segment clears terrain obstacles");
                    previous = waypoint;
                }
                require(isTerrainClear(simulation, previous, orderedUnit.destination),
                        "final routed segment clears terrain obstacles");
                require(isTerrainClear(simulation, orderedUnit.destination),
                        "routed destination is outside terrain collision");

                int ticks = 0;
                while (simulation.units()[unitId - 1].nextWaypoint <
                           simulation.units()[unitId - 1].path.size() &&
                       ticks < 2000) {
                    simulation.step();
                    ++ticks;
                    require(isTerrainClear(simulation,
                                simulation.units()[unitId - 1].position),
                            "unit remains outside terrain collision while following route");
                }
                const auto& completedUnit = simulation.units()[unitId - 1];
                require(ticks < 2000 &&
                        completedUnit.path.size() == originalPathSize &&
                        completedUnit.nextWaypoint == completedUnit.path.size(),
                        "unit completes its path without collision cancellation");
                require(std::abs(completedUnit.position.x - completedUnit.destination.x) <
                            0.01F &&
                        std::abs(completedUnit.position.y - completedUnit.destination.y) <
                            0.01F,
                        "unit reaches its routed destination");
            }
        }
    }
    require(aStarRouteCosts.size() == dijkstraRouteCosts.size(),
            "both algorithms run every terrain route-quality scenario");
    for (std::size_t route = 0; route < aStarRouteCosts.size(); ++route) {
        require(aStarRouteCosts[route] == dijkstraRouteCosts[route],
                "A* and Dijkstra agree on optimal costs across terrain scenarios");
    }
}

void testCameraGroundProjectionRoundTrip() {
    rts::Camera3D camera;
    camera.setViewport(1280.0F, 800.0F);
    camera.focusAt(1000.0F, 700.0F);

    for (const bool perspective : {false, true}) {
        camera.perspective = perspective;
        const rts::WorldPoint world{720.0F, 0.0F, 480.0F};
        const auto screen = camera.project(world);
        const auto recovered = camera.ground(screen.x, screen.y);
        require(recovered.valid, "projected terrain point can be picked");
        require(std::abs(recovered.x - world.x) < 0.01F,
                "camera projection round-trips the world x coordinate");
        require(std::abs(recovered.z - world.z) < 0.01F,
                "camera projection round-trips the world z coordinate");

        const auto beforeZoom = camera.ground(screen.x, screen.y);
        camera.zoomAt(screen.x, screen.y, camera.scale * 1.4F);
        const auto afterZoom = camera.ground(screen.x, screen.y);
        require(std::abs(beforeZoom.x - afterZoom.x) < 0.01F &&
                std::abs(beforeZoom.z - afterZoom.z) < 0.01F,
                "zoom remains anchored at the pointer");

        const auto beforeOrbit = camera.ground(screen.x, screen.y);
        camera.orbitAt(screen.x, screen.y, 13.0F, -4.0F);
        const auto afterOrbit = camera.ground(screen.x, screen.y);
        require(std::abs(beforeOrbit.x - afterOrbit.x) < 0.01F &&
                std::abs(beforeOrbit.z - afterOrbit.z) < 0.01F,
                "orbit remains anchored at the pointer");
    }
}

void testCameraElevatedGroundPicking() {
    rts::Simulation simulation(0);
    rts::Camera3D camera;
    camera.setViewport(1280.0F, 800.0F);
    camera.focusAt(700.0F, 470.0F);
    const rts::Vec2 groundPoint{700.0F, 470.0F};
    const rts::WorldPoint elevatedPoint{
        groundPoint.x, simulation.terrainElevation(groundPoint), groundPoint.y};

    for (const bool perspective : {false, true}) {
        camera.perspective = perspective;
        const auto screen = camera.project(elevatedPoint);
        const auto recovered = camera.ground(screen.x, screen.y,
            [&simulation](float x, float z) {
                return simulation.terrainElevation({x, z});
            });
        require(recovered.valid &&
                std::abs(recovered.x - elevatedPoint.x) < 0.1F &&
                std::abs(recovered.z - elevatedPoint.z) < 0.1F &&
                std::abs(recovered.y - elevatedPoint.y) < 0.01F,
                "camera picking intersects the elevated terrain in both projections");
    }
}

} // namespace

int main() {
    testInitialPopulation();
    testSceneGeometryScaling();
    testSelectionAndMovement();
    testSelectionMissClearsSelection();
    testMovementStopsAtDestination();
    testDeterministicSimulation();
    testSelectionById();
    testMovementStaysInsideBattlefield();
    testMultiUnitOrderFeedback();
    testFullPopulationRouting();
    testPathfindingMetrics();
    testDijkstraMatchesAStarRouteCost();
    testElevationEdgeCostsAreReported();
    testBatchedPathBackend();
    testScreenRectangleSelection();
    testTerrainAwarePathfinding();
    testRouteQualityAcrossDestinations();
    testCameraGroundProjectionRoundTrip();
    testCameraElevatedGroundPicking();
    std::cout << "All simulation tests passed.\n";
}
