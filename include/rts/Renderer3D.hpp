#pragma once

#include "rts/Camera3D.hpp"
#include "rts/SceneGeometry.hpp"
#include "rts/Simulation.hpp"

#include <memory>

struct GLFWwindow;

namespace rts {

struct RendererStats {
    double sceneBuildMilliseconds = 0.0;
    double vertexUploadMilliseconds = 0.0;
    double commandRecordMilliseconds = 0.0;
    double gpuRenderMilliseconds = 0.0;
    std::size_t sceneVertexCount = 0;
    std::uint32_t renderWidth = 0;
    std::uint32_t renderHeight = 0;
    std::uint64_t submittedFrame = 0;
    std::uint64_t gpuTimingFrame = 0;
    bool gpuRenderTimingValid = false;
};

class Renderer3D {
public:
    explicit Renderer3D(GLFWwindow* window);
    ~Renderer3D();

    Renderer3D(const Renderer3D&) = delete;
    Renderer3D& operator=(const Renderer3D&) = delete;

    void draw(const Simulation& simulation, const Camera3D& camera,
              const ScreenRectangle* selectionRectangle = nullptr);
    RendererStats stats() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace rts
