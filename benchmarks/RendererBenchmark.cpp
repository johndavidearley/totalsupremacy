#include "rts/Camera3D.hpp"
#include "rts/Renderer3D.hpp"
#include "rts/Simulation.hpp"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifndef TOTALSUPREMACY_SHADER_DIR
#define TOTALSUPREMACY_SHADER_DIR "shaders"
#endif

#ifndef TOTALSUPREMACY_BUILD_TYPE
#define TOTALSUPREMACY_BUILD_TYPE "unknown"
#endif

namespace {

constexpr int kWindowWidth = 1440;
constexpr int kWindowHeight = 900;
constexpr std::size_t kDeviceWarmupFrames = 90;
constexpr std::size_t kWarmupFrames = 30;
constexpr std::size_t kMeasuredFrames = 180;
constexpr std::size_t kDrainFrames = 3;
constexpr std::size_t kSelectedUnits = 100;
constexpr std::size_t kPopulationSizes[]{250, 500, 1000, 1500, 1800};
constexpr rts::Vec2 kDestination{1800.0F, 1300.0F};

struct GlfwLifetime {
    GLFWwindow* window = nullptr;

    ~GlfwLifetime() {
        if (window != nullptr) {
            glfwDestroyWindow(window);
        }
        glfwTerminate();
    }
};

double percentile(std::vector<double> values, double fraction) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const auto index = static_cast<std::size_t>(
        std::ceil(fraction * static_cast<double>(values.size()))) - 1;
    return values[index];
}

void appendStats(const rts::RendererStats& stats,
                 std::uint64_t measuredFirstFrame,
                 std::uint64_t measuredLastFrame,
                 std::uint64_t& lastGpuFrame,
                 std::vector<double>& sceneBuild,
                 std::vector<double>& upload,
                 std::vector<double>& commandRecord,
                 std::vector<double>& gpuRender) {
    if (stats.submittedFrame >= measuredFirstFrame &&
        stats.submittedFrame <= measuredLastFrame) {
        sceneBuild.push_back(stats.sceneBuildMilliseconds);
        upload.push_back(stats.vertexUploadMilliseconds);
        commandRecord.push_back(stats.commandRecordMilliseconds);
    }
    if (stats.gpuRenderTimingValid &&
        stats.gpuTimingFrame >= measuredFirstFrame &&
        stats.gpuTimingFrame <= measuredLastFrame &&
        stats.gpuTimingFrame != lastGpuFrame) {
        gpuRender.push_back(stats.gpuRenderMilliseconds);
        lastGpuFrame = stats.gpuTimingFrame;
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
              << "logical_threads," << std::thread::hardware_concurrency() << '\n'
              << "window_requested," << kWindowWidth << 'x' << kWindowHeight << '\n'
              << "device_warmup_frames," << kDeviceWarmupFrames << '\n'
              << "warmup_frames," << kWarmupFrames << '\n'
              << "measured_frames," << kMeasuredFrames << '\n'
              << "selected_units," << kSelectedUnits << '\n'
              << "shader_dir," << TOTALSUPREMACY_SHADER_DIR << '\n';
}

void benchmarkScenario(GLFWwindow* window, rts::Renderer3D& renderer,
                       rts::Camera3D& camera,
                       std::size_t population, bool selected,
                       std::uint32_t renderWidth, std::uint32_t renderHeight) {
    rts::Simulation simulation(population);
    if (selected) {
        std::vector<std::uint32_t> ids(kSelectedUnits);
        std::iota(ids.begin(), ids.end(), 1U);
        if (simulation.selectUnits(ids) != ids.size()) {
            throw std::runtime_error("Renderer benchmark failed to select units.");
        }
        simulation.moveSelected(kDestination);
        const auto& feedback = simulation.lastMoveCommand();
        if (feedback.unitsOrdered != ids.size() ||
            feedback.unitsRouted != ids.size()) {
            throw std::runtime_error("Renderer benchmark failed to route selected units.");
        }
    }

    for (std::size_t frame = 0; frame < kWarmupFrames; ++frame) {
        glfwPollEvents();
        if (glfwWindowShouldClose(window) == GLFW_TRUE) {
            throw std::runtime_error("Renderer benchmark window was closed.");
        }
        renderer.draw(simulation, camera);
    }

    const auto firstFrame = renderer.stats().submittedFrame + 1;
    const auto lastFrame = firstFrame + kMeasuredFrames - 1;
    std::uint64_t lastGpuFrame = 0;
    std::vector<double> sceneBuild;
    std::vector<double> upload;
    std::vector<double> commandRecord;
    std::vector<double> gpuRender;
    sceneBuild.reserve(kMeasuredFrames);
    upload.reserve(kMeasuredFrames);
    commandRecord.reserve(kMeasuredFrames);
    gpuRender.reserve(kMeasuredFrames);

    for (std::size_t frame = 0; frame < kMeasuredFrames; ++frame) {
        glfwPollEvents();
        if (glfwWindowShouldClose(window) == GLFW_TRUE) {
            throw std::runtime_error("Renderer benchmark window was closed.");
        }
        renderer.draw(simulation, camera);
        appendStats(renderer.stats(), firstFrame, lastFrame, lastGpuFrame,
                    sceneBuild, upload, commandRecord, gpuRender);
    }

    for (std::size_t frame = 0; frame < kDrainFrames; ++frame) {
        renderer.draw(simulation, camera);
        appendStats(renderer.stats(), firstFrame, lastFrame, lastGpuFrame,
                    sceneBuild, upload, commandRecord, gpuRender);
    }
    if (sceneBuild.size() != kMeasuredFrames ||
        upload.size() != kMeasuredFrames ||
        commandRecord.size() != kMeasuredFrames) {
        throw std::runtime_error("Renderer benchmark did not collect every CPU sample.");
    }

    const auto stats = renderer.stats();
    const double vertexBytes =
        static_cast<double>(stats.sceneVertexCount) * sizeof(rts::SceneVertex);
    std::cout << population << ',' << (selected ? "selected" : "unselected") << ','
              << (selected ? kSelectedUnits : 0) << ',' << renderWidth << ','
              << renderHeight << ',' << stats.sceneVertexCount << ','
              << std::fixed << std::setprecision(3)
              << percentile(sceneBuild, 0.50) << ','
              << percentile(sceneBuild, 0.95) << ','
              << percentile(upload, 0.50) << ','
              << percentile(upload, 0.95) << ','
              << percentile(commandRecord, 0.50) << ','
              << percentile(commandRecord, 0.95) << ',';
    if (gpuRender.empty()) {
        std::cout << "n/a,n/a,0\n";
    } else {
        std::cout << percentile(gpuRender, 0.50) << ','
                  << percentile(gpuRender, 0.95) << ',' << gpuRender.size() << '\n';
    }
    std::cerr << "Completed " << population << " units ("
              << (selected ? "100 selected" : "none selected") << "), "
              << stats.sceneVertexCount << " vertices, "
              << std::fixed << std::setprecision(2) << vertexBytes / (1024.0 * 1024.0)
              << " MiB scene data, GPU timestamp samples " << gpuRender.size()
              << ".\n";
}

} // namespace

int main() {
    try {
#if defined(__APPLE__)
        if (std::getenv("VK_DRIVER_FILES") == nullptr &&
            std::getenv("VK_ICD_FILENAMES") == nullptr) {
            const std::filesystem::path macPortsIcd =
                "/opt/local/share/vulkan/icd.d/MoltenVK_icd.json";
            if (std::filesystem::exists(macPortsIcd)) {
                setenv("VK_DRIVER_FILES", macPortsIcd.c_str(), 0);
            }
        }
#endif
        glfwSetErrorCallback([](int, const char* description) {
            std::cerr << "GLFW: " << description << '\n';
        });
        if (glfwInit() != GLFW_TRUE) {
            throw std::runtime_error("GLFW initialization failed.");
        }
        GlfwLifetime glfwLifetime;
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
        glfwWindowHint(GLFW_VISIBLE, GLFW_TRUE);
#if defined(__APPLE__)
        glfwWindowHint(GLFW_COCOA_RETINA_FRAMEBUFFER, GLFW_FALSE);
#endif
        glfwLifetime.window = glfwCreateWindow(
            kWindowWidth, kWindowHeight, "Total Supremacy Renderer Benchmark",
            nullptr, nullptr);
        if (glfwLifetime.window == nullptr) {
            throw std::runtime_error("Unable to create the benchmark window.");
        }

        printCompiler();
        int framebufferWidth = 0;
        int framebufferHeight = 0;
        glfwGetFramebufferSize(glfwLifetime.window,
                               &framebufferWidth, &framebufferHeight);
        std::cout << "window_framebuffer_reported," << framebufferWidth << 'x'
                  << framebufferHeight << '\n'
                  << "window_requested," << kWindowWidth << 'x' << kWindowHeight << '\n'
                  << "units,mode,selected_units,swapchain_width,swapchain_height,vertices,"
                     "scene_build_median_ms,scene_build_p95_ms,"
                     "upload_median_ms,upload_p95_ms,command_median_ms,"
                     "command_p95_ms,gpu_render_median_ms,gpu_render_p95_ms,"
                     "gpu_samples\n"
                  << std::flush;

        rts::Renderer3D renderer(glfwLifetime.window);
        rts::Camera3D camera;
        const auto initialRendererStats = renderer.stats();
        if (initialRendererStats.renderWidth == 0 ||
            initialRendererStats.renderHeight == 0) {
            throw std::runtime_error("Renderer created an invalid swapchain extent.");
        }
        camera.setViewport(static_cast<float>(initialRendererStats.renderWidth),
                           static_cast<float>(initialRendererStats.renderHeight));
        camera.setPose(45.0F, 35.0F, 0.62F,
                       rts::Simulation::kMapWidth * 0.5F,
                       rts::Simulation::kMapHeight * 0.5F);
        rts::Simulation deviceWarmup(1800);
        std::vector<std::uint32_t> warmupIds(kSelectedUnits);
        std::iota(warmupIds.begin(), warmupIds.end(), 1U);
        if (deviceWarmup.selectUnits(warmupIds) != warmupIds.size()) {
            throw std::runtime_error("Renderer benchmark failed to select warmup units.");
        }
        deviceWarmup.moveSelected(kDestination);
        if (deviceWarmup.lastMoveCommand().unitsRouted != warmupIds.size()) {
            throw std::runtime_error("Renderer benchmark failed to route warmup units.");
        }
        for (std::size_t frame = 0; frame < kDeviceWarmupFrames; ++frame) {
            glfwPollEvents();
            if (glfwWindowShouldClose(glfwLifetime.window) == GLFW_TRUE) {
                throw std::runtime_error("Renderer benchmark window was closed.");
            }
            renderer.draw(deviceWarmup, camera);
        }

        for (const auto population : kPopulationSizes) {
            benchmarkScenario(glfwLifetime.window, renderer, camera,
                              population, false, initialRendererStats.renderWidth,
                              initialRendererStats.renderHeight);
            benchmarkScenario(glfwLifetime.window, renderer, camera,
                              population, true, initialRendererStats.renderWidth,
                              initialRendererStats.renderHeight);
        }
    } catch (const std::exception& error) {
        std::cerr << "Renderer benchmark failed: " << error.what() << '\n';
        return 1;
    }
}
