#include "rts/Camera3D.hpp"
#include "rts/ControlBridge.hpp"
#include "rts/Renderer3D.hpp"
#include "rts/ScreenSelection.hpp"
#include "rts/Simulation.hpp"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

namespace {

using Clock = std::chrono::steady_clock;

struct DesktopHost {
    GLFWwindow* window = nullptr;
    bool initialized = false;

    ~DesktopHost() {
        if (window != nullptr) {
            glfwDestroyWindow(window);
        }
        if (initialized) {
            glfwTerminate();
        }
    }
};

struct InputState {
    double scrollY = 0.0;
};

struct Options {
    bool mcpControl = false;
    bool headless = false;
};

struct Profile {
    double totalMilliseconds = 0.0;
    double maximumMilliseconds = 0.0;
    std::uint64_t samples = 0;

    void record(double milliseconds) {
        totalMilliseconds += milliseconds;
        maximumMilliseconds = std::max(maximumMilliseconds, milliseconds);
        ++samples;
    }

    void reset() {
        totalMilliseconds = 0.0;
        maximumMilliseconds = 0.0;
        samples = 0;
    }
};

bool parseOptions(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string argument(argv[i]);
        if (argument == "--mcp-control") {
            options.mcpControl = true;
        } else if (argument == "--headless") {
            options.headless = true;
        } else {
            std::cerr << "Unknown argument: " << argument << '\n'
                      << "Usage: TotalSupremacy [--mcp-control [--headless]]\n";
            return false;
        }
    }
    if (options.headless && !options.mcpControl) {
        std::cerr << "--headless requires --mcp-control\n";
        return false;
    }
    return true;
}

void updateTitle(GLFWwindow* window, const rts::Simulation& simulation,
                 const Profile& profile, const Profile& sceneProfile,
                 const Profile& uploadProfile, const Profile& commandProfile,
                 const Profile& gpuProfile, std::size_t sceneVertexCount,
                 double framesPerSecond, bool paused,
                 const rts::Camera3D& camera) {
    const auto average = [](const Profile& sampleProfile) {
        return sampleProfile.samples == 0
            ? 0.0
            : sampleProfile.totalMilliseconds /
                  static_cast<double>(sampleProfile.samples);
    };
    const double averageMilliseconds = profile.samples == 0
        ? 0.0
        : profile.totalMilliseconds / static_cast<double>(profile.samples);
    const double averageSceneMilliseconds = average(sceneProfile);
    const auto& order = simulation.lastMoveCommand();
    const bool showOrder = order.valid &&
        simulation.tickCount() - order.issuedAtTick <= 90;
    const std::string title = "Total Supremacy 3D | " +
        std::to_string(simulation.units().size()) + " units | " +
        std::to_string(simulation.selectedCount()) + " selected | tick " +
        std::to_string(simulation.tickCount()) + (paused ? " | PAUSED" : " | LIVE") +
        (showOrder
            ? " | order " + std::to_string(order.unitsRouted) + "/" +
                  std::to_string(order.unitsOrdered) + " routed to " +
                  std::to_string(static_cast<int>(std::lround(
                      order.requestedDestination.x))) + "," +
                  std::to_string(static_cast<int>(std::lround(
                      order.requestedDestination.y)))
            : "") +
        " | " + std::to_string(static_cast<int>(std::lround(framesPerSecond))) +
        " FPS | tick avg/max " + std::to_string(averageMilliseconds) + "/" +
        std::to_string(profile.maximumMilliseconds) + " ms | scene build avg/max " +
        std::to_string(averageSceneMilliseconds) + "/" +
        std::to_string(sceneProfile.maximumMilliseconds) + " ms (" +
        std::to_string(sceneVertexCount) + " vertices) | upload " +
        std::to_string(average(uploadProfile)) + " ms | command " +
        std::to_string(average(commandProfile)) + " ms | GPU render " +
        (gpuProfile.samples == 0
            ? std::string("n/a")
            : std::to_string(average(gpuProfile)) + " ms") + " | " +
        (camera.perspective ? "perspective" : "orthographic");
    glfwSetWindowTitle(window, title.c_str());
}

bool mouseToDrawable(GLFWwindow* window, double x, double y, float& drawableX,
                     float& drawableY, int& width, int& height) {
    int windowWidth = 0;
    int windowHeight = 0;
    glfwGetFramebufferSize(window, &width, &height);
    glfwGetWindowSize(window, &windowWidth, &windowHeight);
    if (width <= 0 || height <= 0 || windowWidth <= 0 || windowHeight <= 0) {
        return false;
    }
    drawableX = static_cast<float>(x * width / windowWidth);
    drawableY = static_cast<float>(y * height / windowHeight);
    return true;
}

void scrollCallback(GLFWwindow* window, double, double y) {
    auto* input = static_cast<InputState*>(glfwGetWindowUserPointer(window));
    if (input != nullptr) {
        input->scrollY += y;
    }
}

void issueMoveAtScreen(rts::Simulation& simulation, const rts::Camera3D& camera,
                       float x, float y) {
    const auto target = camera.ground(x, y,
        [&simulation](float worldX, float worldZ) {
            return simulation.terrainElevation({worldX, worldZ});
        });
    if (target.valid && target.x >= 0.0F && target.x <= rts::Simulation::kMapWidth &&
        target.z >= 0.0F && target.z <= rts::Simulation::kMapHeight) {
        simulation.moveSelected({target.x, target.z});
    }
}

void resetCamera(rts::Camera3D& camera) {
    camera.perspective = false;
    camera.setPose(45.0F, 35.0F, 0.62F, rts::Simulation::kMapWidth * 0.5F,
                   rts::Simulation::kMapHeight * 0.5F);
}

int run(const Options& options) {
    DesktopHost desktop;
    InputState input;
    std::unique_ptr<rts::Renderer3D> renderer;

    if (!options.headless) {
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
            std::cerr << "GLFW initialization failed.\n";
            return 1;
        }
        desktop.initialized = true;
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
        desktop.window = glfwCreateWindow(1440, 900, "Total Supremacy 3D",
                                          nullptr, nullptr);
        if (desktop.window == nullptr) {
            std::cerr << "GLFW window creation failed.\n";
            return 1;
        }
        glfwSetWindowUserPointer(desktop.window, &input);
        glfwSetScrollCallback(desktop.window, scrollCallback);
        renderer = std::make_unique<rts::Renderer3D>(desktop.window);
    }

    rts::Simulation simulation(1000);
    rts::Camera3D camera;
    camera.setViewport(1440.0F, 900.0F);
    resetCamera(camera);
    if (desktop.window != nullptr) {
        int width = 0;
        int height = 0;
        glfwGetFramebufferSize(desktop.window, &width, &height);
        if (width > 0 && height > 0) {
            camera.setViewport(static_cast<float>(width), static_cast<float>(height));
            resetCamera(camera);
        }
    }

    std::unique_ptr<rts::ControlBridge> control;
    if (options.mcpControl) {
        control = std::make_unique<rts::ControlBridge>();
    }

    Profile profile;
    Profile sceneProfile;
    Profile uploadProfile;
    Profile commandProfile;
    Profile gpuProfile;
    std::size_t sceneVertexCount = 0;
    std::uint64_t lastGpuTimingFrame = 0;
    bool running = true;
    bool paused = options.mcpControl;
    bool rightButtonDown = false;
    bool rightButtonDragged = false;
    bool middleButtonDown = false;
    bool leftButtonDown = false;
    bool selectionDragged = false;
    float selectionStartX = 0.0F;
    float selectionStartY = 0.0F;
    rts::ScreenRectangle selectionRectangle;
    float previousMouseX = 0.0F;
    float previousMouseY = 0.0F;
    bool havePreviousMouse = false;
    int previousFramebufferWidth = 0;
    int previousFramebufferHeight = 0;
    int previousSpace = GLFW_RELEASE;
    int previousT = GLFW_RELEASE;
    int previousHome = GLFW_RELEASE;
    int previousEscape = GLFW_RELEASE;
    int previousLeftButton = GLFW_RELEASE;
    int previousRightButton = GLFW_RELEASE;
    int previousMiddleButton = GLFW_RELEASE;
    double accumulator = 0.0;
    std::uint64_t frameCount = 0;
    auto previousFrame = Clock::now();
    auto titleUpdate = previousFrame;

    while (running) {
        if (desktop.window != nullptr) {
            glfwPollEvents();
            if (glfwWindowShouldClose(desktop.window) == GLFW_TRUE) {
                running = false;
            }
        }
        const auto frameStart = Clock::now();
        const double frameSeconds = std::chrono::duration<double>(
            frameStart - previousFrame).count();
        previousFrame = frameStart;
        accumulator += std::min(frameSeconds, 0.25);

        if (control) {
            for (const auto& request : control->takePending()) {
                std::cout << rts::ControlBridge::execute(request, simulation, camera,
                                                         paused, running)
                          << '\n' << std::flush;
            }
        }
        if (!running) {
            break;
        }

        if (desktop.window != nullptr) {
            const auto keyPressed = [window = desktop.window](int key, int& previous) {
                const int current = glfwGetKey(window, key);
                const bool pressed = current == GLFW_PRESS && previous != GLFW_PRESS;
                previous = current;
                return pressed;
            };
            if (keyPressed(GLFW_KEY_ESCAPE, previousEscape)) {
                running = false;
            }
            if (keyPressed(GLFW_KEY_SPACE, previousSpace)) {
                paused = !paused;
                accumulator = 0.0;
            }
            if (keyPressed(GLFW_KEY_T, previousT)) {
                camera.perspective = !camera.perspective;
            }
            if (keyPressed(GLFW_KEY_HOME, previousHome)) {
                resetCamera(camera);
            }

            int width = 0;
            int height = 0;
            glfwGetFramebufferSize(desktop.window, &width, &height);
            if (width != previousFramebufferWidth ||
                height != previousFramebufferHeight) {
                const auto focus = camera.ground(camera.centerX, camera.centerY);
                previousFramebufferWidth = width;
                previousFramebufferHeight = height;
                if (width > 0 && height > 0) {
                    camera.setViewport(static_cast<float>(width),
                                       static_cast<float>(height));
                    if (focus.valid) {
                        camera.focusAt(focus.x, focus.z);
                    }
                }
            }

            double cursorX = 0.0;
            double cursorY = 0.0;
            glfwGetCursorPos(desktop.window, &cursorX, &cursorY);
            float x = 0.0F;
            float y = 0.0F;
            int drawableWidth = 0;
            int drawableHeight = 0;
            if (mouseToDrawable(desktop.window, cursorX, cursorY, x, y,
                                drawableWidth, drawableHeight)) {
                const int leftButton = glfwGetMouseButton(
                    desktop.window, GLFW_MOUSE_BUTTON_LEFT);
                const int rightButton = glfwGetMouseButton(
                    desktop.window, GLFW_MOUSE_BUTTON_RIGHT);
                const int middleButton = glfwGetMouseButton(
                    desktop.window, GLFW_MOUSE_BUTTON_MIDDLE);
                const bool leftPressed = leftButton == GLFW_PRESS &&
                                         previousLeftButton != GLFW_PRESS;
                const bool rightPressed = rightButton == GLFW_PRESS &&
                                          previousRightButton != GLFW_PRESS;
                const bool middlePressed = middleButton == GLFW_PRESS &&
                                           previousMiddleButton != GLFW_PRESS;
                const bool leftReleased = leftButton != GLFW_PRESS &&
                                          previousLeftButton == GLFW_PRESS;
                const bool rightReleased = rightButton != GLFW_PRESS &&
                                           previousRightButton == GLFW_PRESS;
                const bool middleReleased = middleButton != GLFW_PRESS &&
                                            previousMiddleButton == GLFW_PRESS;

                if (leftPressed) {
                    leftButtonDown = true;
                    selectionDragged = false;
                    selectionStartX = x;
                    selectionStartY = y;
                    rts::selectAtScreen(simulation, camera, x, y);
                }
                if (rightPressed) {
                    rightButtonDown = true;
                    rightButtonDragged = false;
                }
                if (middlePressed) {
                    middleButtonDown = true;
                }

                if (havePreviousMouse) {
                    const float deltaX = x - previousMouseX;
                    const float deltaY = y - previousMouseY;
                    if (leftButtonDown) {
                        const float dragX = x - selectionStartX;
                        const float dragY = y - selectionStartY;
                        selectionDragged = selectionDragged ||
                            dragX * dragX + dragY * dragY > 16.0F;
                        selectionRectangle = {
                            selectionStartX, selectionStartY, x, y
                        };
                    }
                    if (rightButtonDown) {
                        if (std::abs(deltaX) + std::abs(deltaY) > 2.0F) {
                            rightButtonDragged = true;
                        }
                        if (rightButtonDragged) {
                            camera.orbitAt(x, y, deltaX * 0.35F, deltaY * 0.25F);
                        }
                    } else if (middleButtonDown) {
                        camera.pan(deltaX, deltaY);
                    }
                }

                if (leftReleased && leftButtonDown) {
                    const float dragX = x - selectionStartX;
                    const float dragY = y - selectionStartY;
                    selectionDragged = selectionDragged ||
                        dragX * dragX + dragY * dragY > 16.0F;
                    if (selectionDragged) {
                        rts::selectInScreenRectangle(
                            simulation, camera,
                            {selectionStartX, selectionStartY}, {x, y});
                    }
                    leftButtonDown = false;
                    selectionDragged = false;
                }
                if (rightReleased && rightButtonDown) {
                    if (!rightButtonDragged) {
                        issueMoveAtScreen(simulation, camera, x, y);
                    }
                    rightButtonDown = false;
                    rightButtonDragged = false;
                }
                if (middleReleased) {
                    middleButtonDown = false;
                }
                previousLeftButton = leftButton;
                previousRightButton = rightButton;
                previousMiddleButton = middleButton;
                previousMouseX = x;
                previousMouseY = y;
                havePreviousMouse = true;

                if (input.scrollY != 0.0) {
                    camera.zoomAt(x, y, camera.scale *
                        std::pow(1.15F, static_cast<float>(input.scrollY)));
                    input.scrollY = 0.0;
                }
            }

            if (!running) {
                break;
            }
            const auto keyDown = [window = desktop.window](int key) {
                return glfwGetKey(window, key) == GLFW_PRESS;
            };
            const float panSpeed = 520.0F * static_cast<float>(frameSeconds);
            if (keyDown(GLFW_KEY_A) || keyDown(GLFW_KEY_LEFT)) camera.pan(panSpeed, 0.0F);
            if (keyDown(GLFW_KEY_D) || keyDown(GLFW_KEY_RIGHT)) camera.pan(-panSpeed, 0.0F);
            if (keyDown(GLFW_KEY_W) || keyDown(GLFW_KEY_UP)) camera.pan(0.0F, panSpeed);
            if (keyDown(GLFW_KEY_S) || keyDown(GLFW_KEY_DOWN)) camera.pan(0.0F, -panSpeed);
        }

        if (paused) {
            accumulator = 0.0;
        } else {
            while (accumulator >= rts::Simulation::kFixedStepSeconds) {
                const auto tickStart = Clock::now();
                simulation.step();
                const auto tickEnd = Clock::now();
                profile.record(std::chrono::duration<double, std::milli>(
                    tickEnd - tickStart).count());
                accumulator -= rts::Simulation::kFixedStepSeconds;
            }
        }

        if (renderer) {
            renderer->draw(simulation, camera,
                           selectionDragged ? &selectionRectangle : nullptr);
            const auto rendererStats = renderer->stats();
            sceneProfile.record(rendererStats.sceneBuildMilliseconds);
            uploadProfile.record(rendererStats.vertexUploadMilliseconds);
            commandProfile.record(rendererStats.commandRecordMilliseconds);
            sceneVertexCount = rendererStats.sceneVertexCount;
            if (rendererStats.gpuRenderTimingValid &&
                rendererStats.gpuTimingFrame != lastGpuTimingFrame) {
                gpuProfile.record(rendererStats.gpuRenderMilliseconds);
                lastGpuTimingFrame = rendererStats.gpuTimingFrame;
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
        }

        ++frameCount;
        const auto now = Clock::now();
        const double titleElapsed = std::chrono::duration<double>(now - titleUpdate).count();
        if (desktop.window != nullptr && titleElapsed >= 1.0) {
            updateTitle(desktop.window, simulation, profile, sceneProfile,
                        uploadProfile, commandProfile, gpuProfile,
                        sceneVertexCount,
                        titleElapsed > 0.0 ? static_cast<double>(frameCount) / titleElapsed : 0.0,
                        paused, camera);
            frameCount = 0;
            titleUpdate = now;
            profile.reset();
            sceneProfile.reset();
            uploadProfile.reset();
            commandProfile.reset();
            gpuProfile.reset();
        }
    }

    renderer.reset();
    control.reset();
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parseOptions(argc, argv, options)) {
        return 2;
    }

    try {
        return run(options);
    } catch (const std::exception& exception) {
        std::cerr << "Application failed: " << exception.what() << '\n';
        return 1;
    }
}
