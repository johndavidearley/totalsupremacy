#include "rts/SceneGeometry.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <thread>
#include <vector>

#ifndef TOTALSUPREMACY_BUILD_TYPE
#define TOTALSUPREMACY_BUILD_TYPE "unknown"
#endif

namespace {

using Clock = std::chrono::steady_clock;
constexpr std::size_t kWarmupTrials = 5;
constexpr std::size_t kTimedTrials = 31;
constexpr std::size_t kPopulationSizes[]{250, 500, 1000, 1500, 1800};

struct SceneMode {
    const char* name;
    std::size_t selectedCount;
};

constexpr SceneMode kModes[]{
    {"unselected", 0},
    {"selected_100", 100}
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

std::uint64_t geometryChecksum(const std::vector<rts::SceneVertex>& vertices) {
    std::uint64_t checksum = vertices.size();
    for (std::size_t index = 0; index < vertices.size(); index += 31) {
        const auto& vertex = vertices[index];
        checksum += static_cast<std::uint64_t>(
            std::abs(vertex.x) + std::abs(vertex.y) + std::abs(vertex.z) +
            std::abs(vertex.red) * 100.0F +
            std::abs(vertex.green) * 100.0F +
            std::abs(vertex.blue) * 100.0F);
    }
    return checksum;
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
              << "logical_threads," << std::thread::hardware_concurrency() << '\n'
              << "warmup_trials," << kWarmupTrials << '\n'
              << "timed_trials," << kTimedTrials << '\n';
}

void benchmark(std::size_t population, const SceneMode& mode) {
    rts::Simulation simulation(population);
    if (mode.selectedCount > 0) {
        const auto ids = unitIds(mode.selectedCount);
        if (simulation.selectUnits(ids) != ids.size()) {
            throw std::runtime_error("Scene benchmark failed to select units.");
        }
        simulation.moveSelected({1800.0F, 1300.0F});
        if (simulation.lastMoveCommand().unitsRouted != ids.size()) {
            throw std::runtime_error("Scene benchmark failed to route selected units.");
        }
    }

    for (std::size_t trial = 0; trial < kWarmupTrials; ++trial) {
        static_cast<void>(rts::buildSceneGeometry(simulation));
    }

    std::vector<double> times;
    times.reserve(kTimedTrials);
    std::size_t vertexCount = 0;
    std::uint64_t checksum = 0;
    for (std::size_t trial = 0; trial < kTimedTrials; ++trial) {
        const auto start = Clock::now();
        const auto vertices = rts::buildSceneGeometry(simulation);
        const auto end = Clock::now();
        if (trial == 0) {
            vertexCount = vertices.size();
        } else if (vertices.size() != vertexCount) {
            throw std::runtime_error("Scene geometry vertex count was not deterministic.");
        }
        checksum += geometryChecksum(vertices);
        times.push_back(elapsedMilliseconds(start, end));
    }

    const auto overlay = rts::buildSelectionOverlayGeometry({100.0F, 100.0F,
                                                              500.0F, 350.0F});
    const double vertexBytes = static_cast<double>(vertexCount) *
                               static_cast<double>(sizeof(rts::SceneVertex));
    std::cout << population << ',' << mode.name << ',' << mode.selectedCount << ','
              << std::fixed << std::setprecision(3)
              << percentile(times, 0.50) << ',' << percentile(times, 0.95) << ','
              << vertexCount << ',' << vertexBytes << ',' << overlay.size() << ','
              << checksum << '\n';
}

} // namespace

int main() {
    try {
        printCompiler();
        std::cout << "units,mode,selected_units,scene_build_median_ms,"
                     "scene_build_p95_ms,scene_vertices,scene_bytes,"
                     "overlay_vertices,geometry_checksum\n";
        for (const auto population : kPopulationSizes) {
            for (const auto& mode : kModes) {
                benchmark(population, mode);
            }
        }
    } catch (const std::exception& error) {
        std::cerr << "Scene benchmark failed: " << error.what() << '\n';
        return 1;
    }
}
