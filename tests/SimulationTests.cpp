#include "rts/Camera3D.hpp"
#include "rts/SceneGeometry.hpp"
#include "rts/ScreenSelection.hpp"
#include "rts/Simulation.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
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

bool onMilliunitLattice(rts::Vec2 point) {
    return rts::Simulation::worldFromMilliunits(
               rts::Simulation::worldMilliunits(point.x)) == point.x &&
           rts::Simulation::worldFromMilliunits(
               rts::Simulation::worldMilliunits(point.y)) == point.y;
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

void testMilliunitMovement() {
    for (std::int32_t milli = 0;
         milli <= static_cast<std::int32_t>(rts::Simulation::kMapWidth *
                                            rts::Simulation::kWorldMilliunits);
         ++milli) {
        const float world = rts::Simulation::worldFromMilliunits(milli);
        if (rts::Simulation::worldMilliunits(world) != milli) {
            require(false, "milliunit round trip failed at " + std::to_string(milli));
        }
    }
    require(rts::Simulation::worldMilliunits(4.9375F) == 4938 &&
                rts::Simulation::worldMilliunits(-4.9375F) == -4938,
            "milliunit rounding ties away from zero");
    require(rts::Simulation::elevationTransitionCost(0, 0) == 0 &&
                rts::Simulation::elevationTransitionCost(0, 1000) == 180 &&
                rts::Simulation::elevationTransitionCost(1000, 0) == 180 &&
                rts::Simulation::elevationTransitionCost(0, 1) == 1,
            "elevation edge cost is an integer ceiling");

    rts::Simulation simulation(1);
    const auto start = simulation.units()[0].position;
    require(simulation.selectUnit(1), "milliunit scenario selects the starter unit");
    require(simulation.moveSelected({start.x + 100.0F, start.y}) == 1,
            "milliunit scenario routes a clear eastward order");
    require(onMilliunitLattice(simulation.units()[0].destination),
            "routed destination lies on the milliunit lattice");
    for (const auto waypoint : simulation.units()[0].path) {
        require(onMilliunitLattice(waypoint),
                "route waypoints lie on the milliunit lattice");
    }
    simulation.step();
    const auto once = simulation.units()[0].position;
    require(once.y == start.y, "a horizontal segment keeps its milliunit row");
    require(rts::Simulation::worldMilliunits(once.x) -
                    rts::Simulation::worldMilliunits(start.x) ==
                rts::Simulation::kMilliunitsPerTick,
            "one tick advances exactly 2400 milliunits");
    for (int tick = 0; tick < 4; ++tick) {
        simulation.step();
    }
    require(simulation.units()[0].position.x == start.x + 12.0F &&
                simulation.units()[0].position.y == start.y,
            "five ticks advance 12 world units on a clear segment");
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
    require(first.stateHash() == second.stateHash(),
            "fresh simulations share a state hash");
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
    require(first.stateHash() == second.stateHash(),
            "state hashes match for equal command streams");
    require(first.commandLog() == second.commandLog(),
            "command logs match for equal command streams");
    for (std::size_t i = 0; i < first.units().size(); ++i) {
        require(first.units()[i].position.x == second.units()[i].position.x &&
                first.units()[i].position.y == second.units()[i].position.y,
                "unit states match for equal command streams");
    }

    rts::Simulation replayed(1000);
    rts::Simulation::replay(replayed, first.commandLog());
    require(replayed.stateHash() == first.stateHash(),
            "replaying the command log reproduces the state hash");
    require(replayed.commandLog() == first.commandLog(),
            "replay records the same command log");

    const auto beforeExtraStep = first.stateHash();
    first.step();
    require(first.stateHash() != beforeExtraStep,
            "one additional step changes the state hash");
}

void testCommandLogRecordsCallerCommands() {
    rts::Simulation simulation(3);
    const auto initialHash = simulation.stateHash();
    simulation.clearSelection();
    require(simulation.stateHash() == initialHash,
            "clearing an empty selection leaves the state hash unchanged");
    require(simulation.commandLog().size() == 1 &&
            simulation.commandLog().front().kind ==
                rts::SimulationCommandKind::ClearSelection,
            "clearing an empty selection is still logged");

    require(!simulation.selectNearest({0.0F, 0.0F}, 1.0F),
            "select nearest misses when no unit is in range");
    require(simulation.commandLog().back().kind ==
                rts::SimulationCommandKind::ClearSelection,
            "a missed nearest selection logs a clear");

    require(!simulation.selectUnit(99), "unknown unit ID is rejected");
    require(simulation.commandLog().back().kind ==
                rts::SimulationCommandKind::SelectUnit &&
            simulation.commandLog().back().unitId == 99,
            "a failed select by ID still logs that ID");

    require(simulation.selectUnits({3, 1}) == 2,
            "select units accepts the given IDs");
    require(simulation.commandLog().back().kind ==
                rts::SimulationCommandKind::SelectUnits &&
            simulation.commandLog().back().unitIds.size() == 2 &&
            simulation.commandLog().back().unitIds[0] == 3 &&
            simulation.commandLog().back().unitIds[1] == 1,
            "select units logs the caller ID list in order");

    simulation.clearSelection();
    require(simulation.moveSelected({4.9375F, 0.0F},
                                    rts::PathfindingAlgorithm::Dijkstra) == 0,
            "a move with no selection is still recorded");
    const auto& move = simulation.commandLog().back();
    require(move.kind == rts::SimulationCommandKind::Move &&
            move.algorithm == rts::PathfindingAlgorithm::Dijkstra &&
            move.destinationMilliX == 4938 &&
            move.destinationMilliZ == 0,
            "a move logs the quantized destination and algorithm");
    require(simulation.lastMoveCommand().requestedDestination.x ==
                rts::Simulation::worldFromMilliunits(4938) &&
            simulation.lastMoveCommand().requestedDestination.y == 0.0F,
            "the recorded move uses the logged destination");

    rts::Simulation replayed(3);
    rts::Simulation::replay(replayed, simulation.commandLog());
    require(replayed.stateHash() == simulation.stateHash() &&
            replayed.commandLog() == simulation.commandLog(),
            "replaying selection and empty move commands matches");

    rts::Simulation group(4);
    require(group.selectUnits({1, 2}) == 2, "group replay selects two units");
    require(group.moveSelected({600.0F, 500.0F}) == 2,
            "group replay orders both units");
    require(group.commandLog().back().kind == rts::SimulationCommandKind::Move &&
            group.commandLog().back().algorithm ==
                rts::PathfindingAlgorithm::FlowField,
            "a group order logs the flow-field move");
    for (int step = 0; step < 5; ++step) {
        group.step();
    }
    rts::Simulation groupReplay(4);
    rts::Simulation::replay(groupReplay, group.commandLog());
    require(groupReplay.stateHash() == group.stateHash() &&
            groupReplay.commandLog() == group.commandLog(),
            "replaying a group move reproduces the state hash");
}

void testReplayDivergence() {
    const auto empty = rts::Simulation::findReplayDivergence(1, {}, {});
    require(empty.kind == rts::ReplayDivergenceKind::None &&
            empty.commandIndex == 0 && !empty.beforeCommands &&
            empty.leftHash == empty.rightHash,
            "empty command logs match");

    rts::Simulation original(8);
    require(original.selectUnits({1, 2}) == 2, "divergence scenario selects two units");
    require(original.moveSelected({600.0F, 500.0F}) == 2,
            "divergence scenario orders both units");
    for (int step = 0; step < 3; ++step) {
        original.step();
    }

    const auto matched = rts::Simulation::findReplayDivergence(
        8, original.commandLog(), original.commandLog());
    require(matched.kind == rts::ReplayDivergenceKind::None &&
            matched.commandIndex == original.commandLog().size() &&
            matched.leftHash == original.stateHash() &&
            matched.leftHash == matched.rightHash,
            "equal command logs report no divergence");

    auto shorter = original.commandLog();
    shorter.pop_back();
    const auto length = rts::Simulation::findReplayDivergence(
        8, original.commandLog(), shorter);
    require(length.kind == rts::ReplayDivergenceKind::Command &&
            length.commandIndex == shorter.size() &&
            length.leftHash == length.rightHash &&
            !length.beforeCommands,
            "a longer command log diverges at the extra command");

    auto altered = original.commandLog();
    std::size_t moveIndex = altered.size();
    for (std::size_t index = 0; index < altered.size(); ++index) {
        if (altered[index].kind == rts::SimulationCommandKind::Move) {
            moveIndex = index;
            altered[index].destinationMilliX += 1000;
            break;
        }
    }
    require(moveIndex < altered.size(), "divergence scenario contains a move");
    rts::Simulation prefix(8);
    require(prefix.selectUnits({1, 2}) == 2, "prefix simulation repeats the selection");
    const auto command = rts::Simulation::findReplayDivergence(
        8, original.commandLog(), altered);
    require(command.kind == rts::ReplayDivergenceKind::Command &&
            command.commandIndex == moveIndex &&
            command.leftHash == prefix.stateHash() &&
            command.leftHash == command.rightHash,
            "a changed move is reported before it is applied");
}

std::int64_t milliDistanceSquared(rts::Vec2 from, rts::Vec2 to) {
    const std::int64_t dx = rts::Simulation::worldMilliunits(from.x) -
        rts::Simulation::worldMilliunits(to.x);
    const std::int64_t dy = rts::Simulation::worldMilliunits(from.y) -
        rts::Simulation::worldMilliunits(to.y);
    return dx * dx + dy * dy;
}

void testAttackDamagesEnemyInRange() {
    rts::Simulation simulation(8);
    const auto range = static_cast<std::int64_t>(rts::Simulation::kAttackRangeMilliunits);
    require(simulation.units()[0].health == rts::Simulation::kUnitHealth &&
            simulation.units()[0].alive && simulation.units()[0].team == 0,
            "units start alive at full health on team 0");
    require(milliDistanceSquared(simulation.units()[0].position,
                                  simulation.units()[1].position) <= range * range,
            "the second starter unit begins inside weapon range");
    const std::int64_t chaseGap = range + rts::Simulation::kMilliunitsPerTick;
    std::uint32_t farId = 0;
    for (const auto& unit : simulation.units()) {
        if (unit.id == 1) {
            continue;
        }
        if (milliDistanceSquared(simulation.units()[0].position, unit.position) >
            chaseGap * chaseGap) {
            farId = unit.id;
            break;
        }
    }
    require(farId != 0, "a starter unit sits beyond one chase tick");

    require(simulation.setTeam(2, 1), "an enemy team can be assigned");
    require(simulation.selectUnit(1), "the attacker can be selected");
    const auto held = simulation.units()[0].position;
    require(simulation.attackSelected(2) == 1, "the attack order applies to the selection");
    require(simulation.units()[0].nextWaypoint >= simulation.units()[0].path.size(),
            "an attacker already in range holds still");
    require(simulation.commandLog().back().kind == rts::SimulationCommandKind::Attack &&
            simulation.commandLog().back().unitId == 2,
            "an attack logs the target id");

    const auto sceneBeforeShot = rts::buildSceneGeometry(simulation).size();
    simulation.step();
    require(simulation.units()[1].alive &&
            simulation.units()[1].health == rts::Simulation::kUnitHealth &&
            simulation.units()[0].attackCooldown == rts::Simulation::kAttackCooldownTicks &&
            simulation.projectiles().size() == 1 &&
            simulation.projectiles()[0].targetId == 2 &&
            simulation.projectiles()[0].damage == rts::Simulation::kAttackDamage &&
            simulation.projectiles()[0].x == rts::Simulation::worldMilliunits(held.x) &&
            simulation.projectiles()[0].z == rts::Simulation::worldMilliunits(held.y),
            "the first in-range tick launches a shot and starts the cooldown");
    require(rts::buildSceneGeometry(simulation).size() - sceneBeforeShot == 30,
            "a projectile adds one box to the scene");
    require(simulation.units()[0].health == rts::Simulation::kUnitHealth,
            "the attacker is not damaged by its own shot");
    simulation.step();
    require(simulation.projectiles().size() == 1 &&
            simulation.projectiles()[0].x > rts::Simulation::worldMilliunits(held.x) &&
            simulation.units()[1].health == rts::Simulation::kUnitHealth,
            "the shot moves toward the target before it hits");
    int flight = 1;
    while (simulation.units()[1].health == rts::Simulation::kUnitHealth && flight < 20) {
        simulation.step();
        ++flight;
    }
    require(flight > 1 && flight < 20 &&
            simulation.units()[1].health ==
                rts::Simulation::kUnitHealth - rts::Simulation::kAttackDamage &&
            simulation.projectiles().empty() &&
            simulation.units()[0].attackCooldown > 0,
            "the shot deals its damage when it arrives");
    const auto healthAfterHit = simulation.units()[1].health;
    const auto cooldownAfterHit = simulation.units()[0].attackCooldown;
    for (int tick = 1; tick < cooldownAfterHit; ++tick) {
        simulation.step();
        require(simulation.units()[1].health == healthAfterHit &&
                simulation.projectiles().empty() &&
                simulation.units()[0].attackCooldown == cooldownAfterHit - tick,
                "the cooldown counts down without another shot");
    }
    simulation.step();
    require(simulation.units()[1].health == healthAfterHit &&
            simulation.projectiles().size() == 1 &&
            simulation.units()[0].attackCooldown == rts::Simulation::kAttackCooldownTicks,
            "the next shot launches when the cooldown reaches zero");
    int hits = 1;
    int guard = 0;
    while (simulation.units()[1].alive && guard < 200) {
        const auto before = simulation.units()[1].health;
        simulation.step();
        ++guard;
        if (simulation.units()[1].health != before) {
            ++hits;
            require(!simulation.units()[1].alive ||
                    simulation.units()[1].health ==
                        rts::Simulation::kUnitHealth - hits * rts::Simulation::kAttackDamage,
                    "each arrived shot deals one infantry hit");
        }
    }
    const auto corpse = simulation.units()[1].position;
    require(hits == 10 && !simulation.units()[1].alive &&
            simulation.units()[1].health == 0 && simulation.units().size() == 8,
            "ten infantry shots remove the unit and keep its vector slot");
    require(simulation.selectUnits({1, 2}) == 1 &&
            simulation.units()[0].selected && !simulation.units()[1].selected,
            "a dead id is omitted from a multi-select");
    simulation.step();
    require(simulation.units()[1].health == 0 &&
            simulation.units()[1].position.x == corpse.x &&
            simulation.units()[1].position.y == corpse.y &&
            simulation.units()[0].position.x == held.x &&
            simulation.units()[0].position.y == held.y,
            "a dead unit keeps its position and the attacker stays in range");

    rts::Simulation replayed(8);
    rts::Simulation::replay(replayed, simulation.commandLog());
    require(replayed.stateHash() == simulation.stateHash() &&
            replayed.commandLog() == simulation.commandLog() &&
            !replayed.units()[1].alive,
            "replaying an attack reproduces the casualty");

    auto altered = simulation.commandLog();
    std::size_t attackIndex = altered.size();
    for (std::size_t index = 0; index < altered.size(); ++index) {
        if (altered[index].kind == rts::SimulationCommandKind::Attack) {
            attackIndex = index;
            altered[index].unitId = 1;
            break;
        }
    }
    const auto diverged = rts::Simulation::findReplayDivergence(
        8, simulation.commandLog(), altered);
    require(attackIndex < altered.size() &&
            diverged.kind == rts::ReplayDivergenceKind::Command &&
            diverged.commandIndex == attackIndex,
            "a changed attack target is a command divergence");

    rts::Simulation friendly(8);
    require(friendly.selectUnit(1) && friendly.attackSelected(2) == 1,
            "a same-team attack order is accepted");
    require(friendly.units()[0].nextWaypoint >= friendly.units()[0].path.size(),
            "a same-team attack does not start a chase");
    friendly.step();
    require(friendly.units()[1].health == rts::Simulation::kUnitHealth &&
            friendly.projectiles().empty(),
            "units on the same team do not fire at each other");

    rts::Simulation distant(8);
    require(distant.setTeam(farId, 1) && distant.selectUnit(1) &&
            distant.attackSelected(farId) == 1,
            "an out-of-range attack order is accepted");
    require(distant.units()[0].nextWaypoint < distant.units()[0].path.size(),
            "an out-of-range attack starts a chase path");
    const auto chaseStart = distant.units()[0].position;
    distant.step();
    require(distant.units()[farId - 1].health == rts::Simulation::kUnitHealth,
            "the first chase tick is still outside weapon range");
    require(milliDistanceSquared(distant.units()[0].position,
                                  distant.units()[farId - 1].position) <
                milliDistanceSquared(chaseStart, distant.units()[farId - 1].position),
            "the attacker closes distance");
    int chaseTicks = 1;
    while (milliDistanceSquared(distant.units()[0].position,
                                 distant.units()[farId - 1].position) > range * range &&
           chaseTicks < 400) {
        require(distant.units()[farId - 1].health == rts::Simulation::kUnitHealth,
                "the chase deals no damage before weapon range");
        distant.step();
        ++chaseTicks;
    }
    require(chaseTicks < 400 &&
            milliDistanceSquared(distant.units()[0].position,
                                 distant.units()[farId - 1].position) <= range * range,
            "the chase reaches weapon range");
    require(distant.units()[farId - 1].health == rts::Simulation::kUnitHealth &&
            distant.projectiles().size() == 1,
            "entering range launches a shot and does not hit yet");
    int flightTicks = 0;
    while (distant.units()[farId - 1].health == rts::Simulation::kUnitHealth &&
           flightTicks < 30) {
        distant.step();
        ++flightTicks;
    }
    require(flightTicks > 0 && flightTicks < 30 &&
            distant.units()[farId - 1].health ==
                rts::Simulation::kUnitHealth - rts::Simulation::kAttackDamage &&
            distant.projectiles().empty(),
            "the shot damages the target when it arrives");
    rts::Simulation chased(8);
    rts::Simulation::replay(chased, distant.commandLog());
    require(chased.stateHash() == distant.stateHash() &&
            chased.units()[farId - 1].health == distant.units()[farId - 1].health,
            "replaying a chase reproduces the approach and the hit");

    rts::Simulation cancelled(8);
    require(cancelled.setTeam(farId, 1) && cancelled.selectUnit(1) &&
            cancelled.attackSelected(farId) == 1,
            "cancel scenario starts a chase");
    require(cancelled.moveSelected({500.0F, 500.0F}) == 1 &&
            cancelled.units()[0].attackTarget == 0,
            "a move order clears the attack");
    cancelled.step();
    require(cancelled.units()[farId - 1].health == rts::Simulation::kUnitHealth,
            "a moved unit does not keep chasing");

    rts::Simulation idle(2);
    require(idle.attackSelected(1) == 0 &&
            idle.commandLog().back().kind == rts::SimulationCommandKind::Attack,
            "an attack with an empty selection is still logged");
    require(!idle.setTeam(99, 1), "an unknown unit id rejects the team assignment");

    rts::Simulation flying(8);
    require(flying.setTeam(2, 1) && flying.selectUnit(1) && flying.attackSelected(2) == 1,
            "flight scenario attacks the neighbor");
    flying.step();
    require(flying.projectiles().size() == 1, "flight scenario launches a shot");
    require(flying.moveSelected({500.0F, 500.0F}) == 1 && flying.units()[0].attackTarget == 0,
            "a move clears the attack and leaves the shot in flight");
    int flown = 0;
    while (!flying.projectiles().empty() && flown < 20) {
        flying.step();
        ++flown;
    }
    require(flown > 0 && flown < 20 && flying.projectiles().empty() &&
            flying.units()[1].health ==
                rts::Simulation::kUnitHealth - rts::Simulation::kAttackDamage,
            "a shot in flight still hits after the attacker moves");
}

void testRangerOutrangesInfantry() {
    constexpr std::size_t population = 80;
    rts::Simulation field(population);
    const auto infantryRange = static_cast<std::int64_t>(rts::Simulation::kAttackRangeMilliunits);
    const auto rangerRange = static_cast<std::int64_t>(rts::Simulation::kRangerRangeMilliunits);
    const auto chaseGap = infantryRange + rts::Simulation::kMilliunitsPerTick;
    std::uint32_t targetId = 0;
    for (const auto& unit : field.units()) {
        if (unit.id == 1) {
            continue;
        }
        const auto distance = milliDistanceSquared(field.units()[0].position, unit.position);
        if (distance > chaseGap * chaseGap && distance <= rangerRange * rangerRange) {
            targetId = unit.id;
            break;
        }
    }
    require(targetId != 0, "a starter unit sits between infantry and ranger range");

    rts::Simulation infantry(population);
    require(infantry.setTeam(targetId, 1) && infantry.selectUnit(1) &&
            infantry.attackSelected(targetId) == 1,
            "infantry comparison attacks the mid-range target");
    infantry.step();
    require(infantry.units()[targetId - 1].health == rts::Simulation::kUnitHealth,
            "infantry does not hit beyond its own range on the first tick");
    require(infantry.units()[0].nextWaypoint < infantry.units()[0].path.size(),
            "infantry chases a target beyond its range");

    rts::Simulation ranger(population);
    require(ranger.setArchetype(1, rts::UnitArchetype::Ranger),
            "the lead unit can become a ranger");
    require(ranger.units()[0].archetype == rts::UnitArchetype::Ranger &&
            ranger.units()[0].health == rts::Simulation::kRangerHealth,
            "becoming a ranger clamps health to the ranger maximum");
    require(ranger.setArchetype(1, rts::UnitArchetype::Infantry),
            "the lead unit can return to infantry");
    require(ranger.units()[0].archetype == rts::UnitArchetype::Infantry &&
            ranger.units()[0].health == rts::Simulation::kRangerHealth,
            "returning to infantry does not restore clamped health");
    require(ranger.setArchetype(1, rts::UnitArchetype::Ranger),
            "the lead unit becomes a ranger again");
    const auto held = ranger.units()[0].position;
    require(ranger.setTeam(targetId, 1) && ranger.selectUnit(1) &&
            ranger.attackSelected(targetId) == 1,
            "the ranger attacks the mid-range target");
    require(ranger.units()[0].nextWaypoint >= ranger.units()[0].path.size(),
            "a ranger already in range holds still");
    ranger.step();
    require(ranger.units()[targetId - 1].health == rts::Simulation::kUnitHealth &&
            ranger.units()[0].position.x == held.x &&
            ranger.units()[0].position.y == held.y &&
            ranger.units()[0].attackCooldown == rts::Simulation::kRangerCooldownTicks &&
            ranger.projectiles().size() == 1,
            "a ranger launches a shot at a target infantry cannot reach yet");
    int rangerFlight = 0;
    while (ranger.units()[targetId - 1].health == rts::Simulation::kUnitHealth &&
           rangerFlight < 40) {
        ranger.step();
        ++rangerFlight;
    }
    require(rangerFlight > 0 && rangerFlight < 40 &&
            ranger.units()[targetId - 1].health ==
                rts::Simulation::kUnitHealth - rts::Simulation::kRangerDamage &&
            ranger.projectiles().empty() &&
            ranger.units()[0].position.x == held.x &&
            ranger.units()[0].position.y == held.y &&
            ranger.units()[0].attackCooldown > 0,
            "the ranger shot arrives without the ranger moving");
    const auto healthAfterRangerShot = ranger.units()[targetId - 1].health;
    const auto cooldownAfterHit = ranger.units()[0].attackCooldown;
    for (int tick = 1; tick < cooldownAfterHit; ++tick) {
        ranger.step();
        require(ranger.units()[targetId - 1].health == healthAfterRangerShot &&
                ranger.projectiles().empty() &&
                ranger.units()[0].attackCooldown == cooldownAfterHit - tick,
                "a ranger does not fire again during its longer cooldown");
    }
    ranger.step();
    require(ranger.units()[targetId - 1].health == healthAfterRangerShot &&
            ranger.projectiles().size() == 1 &&
            ranger.units()[0].attackCooldown == rts::Simulation::kRangerCooldownTicks,
            "a ranger fires again when its cooldown reaches zero");
    require(ranger.setArchetype(1, rts::UnitArchetype::Infantry) &&
            ranger.units()[0].attackCooldown == rts::Simulation::kAttackCooldownTicks,
            "a shorter weapon clamps the remaining cooldown");
    require(ranger.setArchetype(1, rts::UnitArchetype::Ranger) &&
            ranger.units()[0].attackCooldown == rts::Simulation::kAttackCooldownTicks,
            "a longer weapon does not add cooldown");

    rts::Simulation replayed(population);
    rts::Simulation::replay(replayed, ranger.commandLog());
    require(replayed.stateHash() == ranger.stateHash() &&
            replayed.units()[0].archetype == rts::UnitArchetype::Ranger &&
            replayed.units()[targetId - 1].health == ranger.units()[targetId - 1].health,
            "replaying an archetype change reproduces the ranger hit");
    require(!ranger.setArchetype(99, rts::UnitArchetype::Ranger),
            "an unknown unit id rejects the archetype assignment");
}

void testCommandQueueRunsInOrder() {
    rts::Simulation simulation(1);
    const auto start = simulation.units()[0].position;
    require(simulation.selectUnit(1), "queue scenario selects the unit");
    require(simulation.moveSelected({start.x + 10.0F, start.y}) == 1,
            "the first move starts immediately");
    require(simulation.moveSelected({start.x + 30.0F, start.y},
                                    rts::PathfindingAlgorithm::AStar, true) == 1,
            "the second move queues behind the first");
    require(simulation.units()[0].orders.size() == 1 &&
            std::abs(simulation.units()[0].destination.x - (start.x + 10.0F)) < 1.0F,
            "the active destination stays the first move");
    require(simulation.moveSelected({start.x + 40.0F, start.y},
                                    rts::PathfindingAlgorithm::AStar, true) == 1 &&
            simulation.units()[0].orders.size() == 2,
            "another move appends at the back of the queue");

    int guard = 0;
    while (simulation.units()[0].orders.size() == 2 && guard < 40) {
        simulation.step();
        ++guard;
    }
    require(simulation.units()[0].orders.size() == 1 &&
            std::abs(simulation.units()[0].destination.x - (start.x + 30.0F)) < 1.0F,
            "the second move starts when the first one finishes");
    while (!simulation.units()[0].orders.empty() && guard < 80) {
        simulation.step();
        ++guard;
    }
    require(simulation.units()[0].orders.empty() &&
            std::abs(simulation.units()[0].destination.x - (start.x + 40.0F)) < 1.0F,
            "the third move starts when the second one finishes");
    while (simulation.units()[0].nextWaypoint < simulation.units()[0].path.size() &&
           guard < 120) {
        simulation.step();
        ++guard;
    }
    require(std::abs(simulation.units()[0].position.x - (start.x + 40.0F)) < 0.05F &&
            std::abs(simulation.units()[0].position.y - start.y) < 0.05F,
            "the unit arrives at the last queued destination");

    rts::Simulation replayed(1);
    rts::Simulation::replay(replayed, simulation.commandLog());
    require(replayed.stateHash() == simulation.stateHash() &&
            std::abs(replayed.units()[0].position.x - simulation.units()[0].position.x) < 0.001F,
            "replaying the queue reproduces the final position");

    rts::Simulation replaced(1);
    const auto origin = replaced.units()[0].position;
    require(replaced.selectUnit(1) &&
            replaced.moveSelected({origin.x + 10.0F, origin.y}) == 1 &&
            replaced.moveSelected({origin.x + 30.0F, origin.y},
                                  rts::PathfindingAlgorithm::AStar, true) == 1,
            "replace scenario queues a move");
    require(replaced.moveSelected({origin.x + 16.0F, origin.y}) == 1 &&
            replaced.units()[0].orders.empty(),
            "an immediate move clears the queue");

    rts::Simulation attack(4);
    require(attack.setTeam(2, 1) && attack.selectUnit(1) &&
            attack.moveSelected({attack.units()[0].position.x + 10.0F,
                                 attack.units()[0].position.y}) == 1 &&
            attack.attackSelected(2, true) == 1,
            "an attack can queue behind a move");
    require(attack.units()[0].orders.size() == 1 && attack.units()[0].attackTarget == 0,
            "the queued attack waits until the move finishes");
    attack.step();
    require(attack.units()[0].attackTarget == 0,
            "one tick of the move does not start the queued attack");
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
    first.moveSelected({1800.0F, 1300.0F}, rts::PathfindingAlgorithm::AStar);
    second.moveSelected({1800.0F, 1300.0F}, rts::PathfindingAlgorithm::AStar);

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
            first.lastPathfindingMetrics().flowFieldNodesExpanded == 0 &&
            first.lastPathfindingMetrics().formationPaths == 0 &&
            first.lastPathfindingMetrics().formationFallbacks == 0 &&
            first.lastPathfindingMetrics().pathCost == 0,
            "pathfinding metrics reset when no units are ordered");
}

void testDijkstraMatchesAStarRouteCost() {
    rts::Simulation aStar;
    rts::Simulation dijkstra;
    const std::vector<std::uint32_t> unitIds{1, 127, 400, 1000};
    require(aStar.terrainElevation({380.0F, 650.0F}) -
                aStar.terrainElevation({100.0F, 100.0F}) > 20.0F,
            "terrain elevation varies across the battlefield");
    require(aStar.selectUnits(unitIds) == unitIds.size() &&
            dijkstra.selectUnits(unitIds) == unitIds.size(),
            "both algorithms select the same units");
    aStar.moveSelected({1800.0F, 1300.0F}, rts::PathfindingAlgorithm::AStar);
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
        const auto elevationCost = static_cast<std::uint64_t>(
            rts::Simulation::elevationTransitionCost(
                simulation.terrainElevationMilliunits(
                    cellCenter(previousX, previousY)),
                simulation.terrainElevationMilliunits(
                    cellCenter(nextX, nextY))));
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
    require(batched.commandLog().size() == 1 &&
            batched.commandLog().front().kind ==
                rts::SimulationCommandKind::SelectUnit,
            "backend move orders stay out of the command log");

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
    require(batched.commandLog().size() == 1,
            "a rejected backend route does not enter the command log");
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

std::vector<std::uint32_t> leadingUnitIds(std::size_t count) {
    std::vector<std::uint32_t> ids(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        ids[index] = index + 1;
    }
    return ids;
}

rts::Vec2 formationSlot(rts::Vec2 destination, std::size_t count, std::size_t index) {
    const auto columns = static_cast<std::size_t>(
        std::ceil(std::sqrt(static_cast<double>(count))));
    const std::int64_t originX = rts::Simulation::worldMilliunits(destination.x);
    const std::int64_t originY = rts::Simulation::worldMilliunits(destination.y);
    const std::int64_t column = static_cast<std::int64_t>(index % columns);
    const std::int64_t row = static_cast<std::int64_t>(index / columns);
    const std::int64_t span = static_cast<std::int64_t>(columns - 1);
    constexpr std::int64_t halfSpacing = 7 * rts::Simulation::kWorldMilliunits;
    const std::int64_t maxX =
        static_cast<std::int64_t>(rts::Simulation::kMapWidth) *
        rts::Simulation::kWorldMilliunits;
    const std::int64_t maxY =
        static_cast<std::int64_t>(rts::Simulation::kMapHeight) *
        rts::Simulation::kWorldMilliunits;
    const auto clampMilli = [](std::int64_t value, std::int64_t limit) {
        return static_cast<std::int32_t>(std::clamp(value, std::int64_t{0}, limit));
    };
    return {
        rts::Simulation::worldFromMilliunits(clampMilli(
            originX + (2 * column - span) * halfSpacing, maxX)),
        rts::Simulation::worldFromMilliunits(clampMilli(
            originY + (2 * row - span) * halfSpacing, maxY))
    };
}

bool followFormationUntilArrival(rts::Simulation& simulation) {
    for (int tick = 0; tick < 8000; ++tick) {
        bool moving = false;
        for (const auto& unit : simulation.units()) {
            if (unit.nextWaypoint < unit.path.size()) {
                moving = true;
            }
        }
        if (!moving) {
            return true;
        }
        simulation.step();
        for (const auto& unit : simulation.units()) {
            if (!isTerrainClear(simulation, unit.position) || unit.path.empty()) {
                return false;
            }
        }
    }
    return false;
}

void testSingleUnitOrderStaysOnIndividualAStar() {
    rts::Simulation flow(1);
    rts::Simulation astar(1);
    require(flow.selectUnit(1) && astar.selectUnit(1),
            "single-unit comparison selects the same unit");
    flow.moveSelected({1800.0F, 1300.0F});
    astar.moveSelected({1800.0F, 1300.0F}, rts::PathfindingAlgorithm::AStar);
    require(flow.lastPathfindingMetrics().flowFieldNodesExpanded == 0 &&
            flow.lastPathfindingMetrics().formationPaths == 0 &&
            flow.lastPathfindingMetrics().aStarNodesExpanded ==
                astar.lastPathfindingMetrics().aStarNodesExpanded &&
            flow.lastPathfindingMetrics().pathCost ==
                astar.lastPathfindingMetrics().pathCost &&
            flow.units()[0].destination.x == astar.units()[0].destination.x &&
            flow.units()[0].destination.y == astar.units()[0].destination.y &&
            flow.units()[0].path.size() == astar.units()[0].path.size(),
            "a one-unit order keeps the individual A* route");
}

void testFormationFlowField() {
    constexpr std::size_t count = 16;
    const auto ids = leadingUnitIds(count);
    rts::Simulation simulation(count);
    rts::Simulation repeated(count);
    require(simulation.selectUnits(ids) == count &&
            repeated.selectUnits(ids) == count,
            "formation scenario selects the same group twice");
    const rts::Vec2 destination{1000.0F, 700.0F};
    require(simulation.moveSelected(destination) == count &&
            repeated.moveSelected(destination) == count &&
            simulation.lastMoveCommand().unitsRouted == count,
            "formation order routes every selected unit");

    const auto& metrics = simulation.lastPathfindingMetrics();
    require(metrics.flowFieldNodesExpanded > 0 &&
            metrics.aStarNodesExpanded == 0 &&
            metrics.dijkstraNodesExpanded == 0 &&
            metrics.formationFallbacks == 0 &&
            metrics.formationPaths == count,
            "group order uses only the shared flow field");
    require(metrics.flowFieldNodesExpanded ==
                repeated.lastPathfindingMetrics().flowFieldNodesExpanded &&
            metrics.pathCost == repeated.lastPathfindingMetrics().pathCost,
            "formation routing is deterministic");

    for (std::size_t index = 0; index < count; ++index) {
        const auto& unit = simulation.units()[index];
        const auto& again = repeated.units()[index];
        const auto slot = formationSlot(destination, count, index);
        require(unit.destination.x == slot.x && unit.destination.y == slot.y,
                "formation order preserves each unit's slot");
        require(onMilliunitLattice(unit.destination),
                "formation slot lies on the milliunit lattice");
        require(unit.destination.x == again.destination.x &&
                unit.destination.y == again.destination.y &&
                unit.path.size() == again.path.size(),
                "repeated formation orders produce the same routes");
        require(!unit.path.empty() && unit.path.size() == again.path.size(),
                "formation unit receives waypoints");
        rts::Vec2 previous = unit.position;
        for (std::size_t waypointIndex = 0; waypointIndex < unit.path.size();
             ++waypointIndex) {
            const auto waypoint = unit.path[waypointIndex];
            const auto repeatedWaypoint = again.path[waypointIndex];
            require(onMilliunitLattice(waypoint),
                    "formation waypoint lies on the milliunit lattice");
            require(waypoint.x == repeatedWaypoint.x &&
                        waypoint.y == repeatedWaypoint.y,
                    "repeated formation orders produce the same waypoints");
            require(isTerrainClear(simulation, previous, waypoint),
                    "formation waypoint segment clears terrain");
            previous = waypoint;
        }
    }

    require(followFormationUntilArrival(simulation),
            "formation units reach their slots without terrain cancellation");
    for (const auto& unit : simulation.units()) {
        require(unit.nextWaypoint == unit.path.size() &&
                std::abs(unit.position.x - unit.destination.x) < 0.01F &&
                std::abs(unit.position.y - unit.destination.y) < 0.01F,
                "formation unit stops on its slot");
    }
}

void testFormationRoutesAcrossTerrain() {
    const auto ids = leadingUnitIds(24);
    rts::Simulation probe(ids.size());
    const auto& obstacle = probe.terrainObstacles().front();
    const std::vector<rts::Vec2> destinations{
        {520.0F, 350.0F},
        {1800.0F, 1300.0F},
        {0.0F, 0.0F},
        {rts::Simulation::kMapWidth, rts::Simulation::kMapHeight},
        {(obstacle.minimum.x + obstacle.maximum.x) * 0.5F,
         (obstacle.minimum.y + obstacle.maximum.y) * 0.5F}
    };
    for (const auto destination : destinations) {
        rts::Simulation simulation(ids.size());
        require(simulation.selectUnits(ids) == ids.size() &&
                simulation.moveSelected(destination) == ids.size() &&
                simulation.lastMoveCommand().unitsRouted == ids.size(),
                "formation order routes every unit around terrain");
        require(simulation.lastPathfindingMetrics().formationFallbacks == 0 &&
                simulation.lastPathfindingMetrics().flowFieldNodesExpanded > 0,
                "terrain formation order stays on the shared flow field");
        for (const auto& unit : simulation.units()) {
            require(isTerrainClear(simulation, unit.destination),
                    "adjusted formation destination stays outside terrain");
            rts::Vec2 previous = unit.position;
            for (const auto waypoint : unit.path) {
                require(isTerrainClear(simulation, previous, waypoint),
                        "terrain formation segment stays outside obstacles");
                previous = waypoint;
            }
        }
        require(followFormationUntilArrival(simulation),
                "terrain formation units complete their routes");
        for (const auto& unit : simulation.units()) {
            require(unit.nextWaypoint == unit.path.size() &&
                    std::abs(unit.position.x - unit.destination.x) < 0.01F &&
                    std::abs(unit.position.y - unit.destination.y) < 0.01F,
                    "terrain formation unit reaches its adjusted slot");
        }
    }
}

void testFormationFlowFieldSearchesLessThanPerUnitAStar() {
    constexpr std::size_t count = 200;
    const auto ids = leadingUnitIds(count);
    rts::Simulation flow(count);
    rts::Simulation astar(count);
    require(flow.selectUnits(ids) == count && astar.selectUnits(ids) == count,
            "search comparison selects the same group");
    const rts::Vec2 destination{1800.0F, 1300.0F};
    flow.moveSelected(destination, rts::PathfindingAlgorithm::FlowField);
    astar.moveSelected(destination, rts::PathfindingAlgorithm::AStar);
    require(flow.lastMoveCommand().unitsRouted == count &&
            astar.lastMoveCommand().unitsRouted == count,
            "both algorithms route the comparison group");
    const auto flowExpanded = flow.lastPathfindingMetrics().flowFieldNodesExpanded;
    const auto astarExpanded = astar.lastPathfindingMetrics().aStarNodesExpanded;
    require(flow.lastPathfindingMetrics().formationFallbacks == 0 &&
            flowExpanded > 0 && flowExpanded * 2 < astarExpanded,
            "shared flow field expands fewer nodes than per-unit A* (flow=" +
                std::to_string(flowExpanded) + ", astar=" +
                std::to_string(astarExpanded) + ")");
}

void testLargeFormationFlowFieldKeepsItsRouteCost() {
    constexpr std::size_t count = 1800;
    const auto ids = leadingUnitIds(count);
    rts::Simulation simulation(count);
    require(simulation.selectUnits(ids) == count &&
            simulation.moveSelected({1800.0F, 1300.0F}) == count &&
            simulation.lastMoveCommand().unitsRouted == count,
            "large formation order routes every unit");
    const auto& metrics = simulation.lastPathfindingMetrics();
    require(metrics.formationPaths == count && metrics.formationFallbacks == 0,
            "large formation order stays on offset paths");
    require(metrics.pathCost == 111320545ULL,
            "large far-corner flow cost stays " + std::to_string(metrics.pathCost));
}

void testRoutePrefersRidgePass() {
    const rts::Vec2 crest{380.0F, 650.0F};
    const rts::Vec2 pass{560.0F, 650.0F};
    rts::Simulation probe(1);
    const float crestElevation = probe.terrainElevation(crest);
    const float passElevation = probe.terrainElevation(pass);
    require(crestElevation - passElevation > 20.0F,
            "western ridge stands above its pass");

    const auto orderDueNorth = [&](rts::PathfindingAlgorithm algorithm) {
        rts::Simulation simulation(1);
        require(simulation.selectUnit(1), "pass scenario selects the lead unit");
        const auto start = simulation.units()[0].position;
        require(simulation.moveSelected({start.x, 1000.0F}, algorithm) == 1 &&
                simulation.lastMoveCommand().unitsRouted == 1,
                "pass scenario routes the lead unit");
        float highest = simulation.terrainElevation(start);
        float farthestEast = start.x;
        for (const auto point : simulation.units()[0].path) {
            highest = std::max(highest, simulation.terrainElevation(point));
            farthestEast = std::max(farthestEast, point.x);
        }
        require(farthestEast > start.x + 80.0F,
                "route shifts east toward the pass (reached x=" +
                    std::to_string(farthestEast) + ")");
        require(highest + 15.0F < crestElevation,
                "route stays below the ridge crest (highest=" +
                    std::to_string(highest) + ", crest=" +
                    std::to_string(crestElevation) + ")");
        return simulation.lastPathfindingMetrics().pathCost;
    };

    const auto aStarCost = orderDueNorth(rts::PathfindingAlgorithm::AStar);
    const auto dijkstraCost = orderDueNorth(rts::PathfindingAlgorithm::Dijkstra);
    require(aStarCost == dijkstraCost && aStarCost > 0,
            "both searches pay the same cost to use the pass");

    rts::Simulation gentle(1);
    require(gentle.selectUnit(1), "gentle-slope scenario selects the lead unit");
    const auto start = gentle.units()[0].position;
    require(gentle.moveSelected({start.x + 320.0F, start.y},
                                rts::PathfindingAlgorithm::AStar) == 1,
            "gentle-slope scenario routes the lead unit");
    for (const auto point : gentle.units()[0].path) {
        require(std::abs(point.y - start.y) <= rts::Simulation::kTerrainCellSize,
                "gentle slope keeps the route on its straight line");
    }
}

void testCameraElevatedGroundPicking() {
    rts::Simulation simulation(0);
    rts::Camera3D camera;
    camera.setViewport(1280.0F, 800.0F);
    camera.focusAt(380.0F, 650.0F);
    const rts::Vec2 groundPoint{380.0F, 650.0F};
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
    testMilliunitMovement();
    testSelectionAndMovement();
    testSelectionMissClearsSelection();
    testMovementStopsAtDestination();
    testDeterministicSimulation();
    testCommandLogRecordsCallerCommands();
    testReplayDivergence();
    testAttackDamagesEnemyInRange();
    testRangerOutrangesInfantry();
    testCommandQueueRunsInOrder();
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
    testSingleUnitOrderStaysOnIndividualAStar();
    testFormationFlowField();
    testFormationRoutesAcrossTerrain();
    testFormationFlowFieldSearchesLessThanPerUnitAStar();
    testLargeFormationFlowFieldKeepsItsRouteCost();
    testRoutePrefersRidgePass();
    testCameraGroundProjectionRoundTrip();
    testCameraElevatedGroundPicking();
    std::cout << "All simulation tests passed.\n";
}
