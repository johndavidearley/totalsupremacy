#include "rts/Simulation.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

#ifndef TOTALSUPREMACY_BUILD_TYPE
#define TOTALSUPREMACY_BUILD_TYPE "unknown"
#endif

namespace {

using Clock = std::chrono::steady_clock;
constexpr std::size_t kRouteTrials = 5;
constexpr std::size_t kStepTrials = 9;
constexpr int kTicksPerStepTrial = 120;
constexpr std::size_t kPopulationSizes[]{250, 500, 1000, 1500, 1800};

struct RouteScenario {
    std::string_view name;
    rts::Vec2 destination;
};

constexpr RouteScenario kRouteScenarios[]{
    {"nearby", {520.0F, 350.0F}},
    {"center", {1000.0F, 700.0F}},
    {"far-corner", {1800.0F, 1300.0F}}
};

struct PathfindingScenario {
    std::string_view name;
    rts::PathfindingAlgorithm algorithm;
};

constexpr PathfindingScenario kPathfindingScenarios[]{
    {"astar", rts::PathfindingAlgorithm::AStar},
    {"dijkstra", rts::PathfindingAlgorithm::Dijkstra}
};

double elapsedMilliseconds(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

double percentile(std::vector<double> values, double fraction) {
    std::sort(values.begin(), values.end());
    const auto index = static_cast<std::size_t>(
        std::ceil(fraction * static_cast<double>(values.size()))) - 1;
    return values[index];
}

std::vector<std::uint32_t> unitIds(std::size_t count) {
    std::vector<std::uint32_t> ids(count);
    std::iota(ids.begin(), ids.end(), 1U);
    return ids;
}

void orderAllUnits(rts::Simulation& simulation,
                   const std::vector<std::uint32_t>& ids,
                   rts::Vec2 destination) {
    if (simulation.selectUnits(ids) != ids.size()) {
        throw std::runtime_error("Benchmark failed to select every unit.");
    }
    simulation.moveSelected(destination);
    const auto& feedback = simulation.lastMoveCommand();
    if (feedback.unitsOrdered != ids.size() || feedback.unitsRouted != ids.size()) {
        throw std::runtime_error("Benchmark failed to route every selected unit.");
    }
}

void printCompiler() {
#if defined(__clang__)
    std::cout << "compiler,Clang " << __clang_major__ << '.' << __clang_minor__ << '\n';
#elif defined(__GNUC__)
    std::cout << "compiler,GCC " << __GNUC__ << '.' << __GNUC_MINOR__ << '\n';
#elif defined(_MSC_VER)
    std::cout << "compiler,MSVC " << _MSC_VER << '\n';
#else
    std::cout << "compiler,unknown\n";
#endif
    std::cout << "build_type," << TOTALSUPREMACY_BUILD_TYPE << '\n'
              << "logical_threads," << std::thread::hardware_concurrency() << '\n';
}

void benchmarkPopulation(std::size_t population) {
    const auto ids = unitIds(population);
    std::vector<double> stepTimesPerTick;
    stepTimesPerTick.reserve(kStepTrials);
    std::uint64_t positionChecksum = 0;
    for (std::size_t trial = 0; trial < kStepTrials; ++trial) {
        rts::Simulation simulation(population);
        orderAllUnits(simulation, ids, kRouteScenarios[2].destination);
        const auto start = Clock::now();
        for (int tick = 0; tick < kTicksPerStepTrial; ++tick) {
            simulation.step();
        }
        const auto end = Clock::now();
        stepTimesPerTick.push_back(
            elapsedMilliseconds(start, end) / static_cast<double>(kTicksPerStepTrial));
        for (const auto& unit : simulation.units()) {
            positionChecksum += static_cast<std::uint64_t>(unit.position.x * 100.0F);
            positionChecksum += static_cast<std::uint64_t>(unit.position.y * 100.0F);
        }
    }
    const double stepMedian = percentile(stepTimesPerTick, 0.50);
    const double stepP95 = percentile(stepTimesPerTick, 0.95);

    for (const auto& scenario : kRouteScenarios) {
        for (const auto& pathfinding : kPathfindingScenarios) {
            std::vector<double> routeTimes;
            routeTimes.reserve(kRouteTrials);
            rts::PathfindingMetrics totalMetrics;
            for (std::size_t trial = 0; trial < kRouteTrials; ++trial) {
                rts::Simulation simulation(population);
                if (simulation.selectUnits(ids) != ids.size()) {
                    throw std::runtime_error("Benchmark failed to select every unit.");
                }
                const auto start = Clock::now();
                simulation.moveSelected(scenario.destination, pathfinding.algorithm);
                const auto end = Clock::now();
                const auto& feedback = simulation.lastMoveCommand();
                if (feedback.unitsOrdered != ids.size() ||
                    feedback.unitsRouted != ids.size()) {
                    throw std::runtime_error(
                        "Benchmark failed to route every selected unit.");
                }
                routeTimes.push_back(elapsedMilliseconds(start, end));
                const auto& metrics = simulation.lastPathfindingMetrics();
                totalMetrics.startCellsExamined += metrics.startCellsExamined;
                totalMetrics.goalCellsExamined += metrics.goalCellsExamined;
                totalMetrics.aStarNodesExpanded += metrics.aStarNodesExpanded;
                totalMetrics.dijkstraNodesExpanded += metrics.dijkstraNodesExpanded;
                totalMetrics.pathCost += metrics.pathCost;
            }

            const double routedUnits = static_cast<double>(kRouteTrials * population);
            const auto expanded = pathfinding.algorithm == rts::PathfindingAlgorithm::AStar
                ? totalMetrics.aStarNodesExpanded
                : totalMetrics.dijkstraNodesExpanded;
            std::cout << population << ',' << scenario.name << ','
                      << pathfinding.name << ','
                      << scenario.destination.x << ',' << scenario.destination.y << ','
                      << std::fixed << std::setprecision(3)
                      << percentile(routeTimes, 0.50) << ','
                      << percentile(routeTimes, 0.95) << ','
                      << static_cast<double>(totalMetrics.goalCellsExamined) / routedUnits
                      << ','
                      << static_cast<double>(totalMetrics.startCellsExamined) / routedUnits
                      << ','
                      << static_cast<double>(expanded) / routedUnits << ','
                      << static_cast<double>(totalMetrics.pathCost) / routedUnits
                      << ',' << stepMedian << ',' << stepP95 << ','
                      << positionChecksum << '\n';
        }
    }
}

} // namespace

int main() {
    try {
        printCompiler();
        std::cout << "route_trials," << kRouteTrials << '\n'
                  << "step_trials," << kStepTrials << '\n'
                  << "ticks_per_step_trial," << kTicksPerStepTrial << '\n'
                  << "route_scenarios," << std::size(kRouteScenarios) << '\n'
                  << "pathfinding_algorithms," << std::size(kPathfindingScenarios) << '\n'
                  << "units,scenario,algorithm,destination_x,destination_y,route_median_ms,"
                     "route_p95_ms,goal_cells_per_unit,start_cells_per_unit,"
                     "expanded_nodes_per_unit,path_cost_per_unit,step_median_ms_per_tick,"
                     "step_p95_ms_per_tick,position_checksum\n";
        for (const auto population : kPopulationSizes) {
            benchmarkPopulation(population);
        }
    } catch (const std::exception& error) {
        std::cerr << "Benchmark failed: " << error.what() << '\n';
        return 1;
    }
}
