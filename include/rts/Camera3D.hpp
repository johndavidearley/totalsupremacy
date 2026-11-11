#pragma once

#include <algorithm>
#include <cmath>

namespace rts {

struct WorldPoint {
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
    bool valid = true;
};

struct WorldRay {
    WorldPoint origin;
    WorldPoint direction;
};

struct ProjectedPoint {
    float x = 0.0F;
    float y = 0.0F;
    float depth = 0.0F;
    float clipW = 1.0F;
};

class Camera3D {
public:
    float yaw = 45.0F;
    float pitch = 35.0F;
    float scale = 0.62F;
    float originX = 0.0F;
    float originY = 0.0F;
    float depthRange = 5000.0F;
    float centerX = 720.0F;
    float centerY = 450.0F;
    float focalLength = 1000.0F;
    bool perspective = false;

    static constexpr float nearPlane = 0.2F;

    float farPlane() const noexcept {
        return std::max(nearPlane + 1.0F, depthRange * 2.0F);
    }

    void setViewport(float width, float height) noexcept {
        centerX = width * 0.5F;
        centerY = height * 0.5F;
        focalLength = std::max(1.0F, height * 1.1F);
    }

    void focusAt(float x, float z) noexcept {
        const float a = yaw * radians;
        const float b = pitch * radians;
        originX = centerX - scale * (std::cos(a) * x - std::sin(a) * z);
        originY = centerY - scale * std::sin(b) *
            (std::sin(a) * x + std::cos(a) * z);
    }

    void pan(float screenX, float screenY) noexcept {
        originX += screenX;
        originY += screenY;
    }

    float viewDepth(WorldPoint point) const noexcept {
        const auto target = flatGround(centerX, centerY);
        const float a = yaw * radians;
        const float b = pitch * radians;
        return focalLength / scale - std::cos(b) *
            (std::sin(a) * (point.x - target.x) + std::cos(a) * (point.z - target.z)) -
            std::sin(b) * point.y;
    }

    ProjectedPoint project(WorldPoint point) const noexcept {
        if (!perspective) {
            return flatProject(point);
        }
        const auto target = flatGround(centerX, centerY);
        const float a = yaw * radians;
        const float b = pitch * radians;
        const float dx = point.x - target.x;
        const float dz = point.z - target.z;
        const float right = std::cos(a) * dx - std::sin(a) * dz;
        const float forward = std::sin(a) * dx + std::cos(a) * dz;
        float w = focalLength / scale - std::cos(b) * forward - std::sin(b) * point.y;
        if (std::abs(w) < 0.00001F) {
            w = std::copysign(0.00001F, w);
        }
        const float far = farPlane();
        return {
            centerX + focalLength * right / w,
            centerY + focalLength * (std::sin(b) * forward - std::cos(b) * point.y) / w,
            far / (far - nearPlane) - far * nearPlane / ((far - nearPlane) * w),
            w
        };
    }

    WorldRay ray(float x, float y) const noexcept {
        const float a = yaw * radians;
        const float b = pitch * radians;
        const WorldPoint normal{
            std::cos(b) * std::sin(a),
            std::sin(b),
            std::cos(b) * std::cos(a)
        };
        if (perspective) {
            const auto target = flatGround(centerX, centerY);
            const float distance = focalLength / scale;
            const float sx = (x - centerX) / focalLength;
            const float sy = (y - centerY) / focalLength;
            return {
                {target.x + normal.x * distance, normal.y * distance,
                 target.z + normal.z * distance},
                {-normal.x + std::cos(a) * sx + std::sin(b) * std::sin(a) * sy,
                 -normal.y - std::cos(b) * sy,
                 -normal.z - std::sin(a) * sx + std::sin(b) * std::cos(a) * sy}
            };
        }
        const auto point = flatGround(x, y);
        const float offset = depthRange * 0.5F -
            (normal.x * point.x + normal.z * point.z);
        return {
            {point.x + normal.x * offset, normal.y * offset,
             point.z + normal.z * offset},
            {-normal.x, -normal.y, -normal.z}
        };
    }

    WorldPoint ground(float x, float y) const noexcept {
        if (!perspective) {
            return flatGround(x, y);
        }
        const float a = yaw * radians;
        const float b = pitch * radians;
        const float sx = (x - centerX) / focalLength;
        const float sy = (y - centerY) / focalLength;
        const float rayX = -std::cos(b) * std::sin(a) + std::cos(a) * sx +
            std::sin(b) * std::sin(a) * sy;
        const float rayY = -std::sin(b) - std::cos(b) * sy;
        const float rayZ = -std::cos(b) * std::cos(a) - std::sin(a) * sx +
            std::sin(b) * std::cos(a) * sy;
        if (rayY >= -0.00001F) {
            return {0.0F, 0.0F, 0.0F, false};
        }
        const float distance = focalLength / scale;
        const float t = -distance * std::sin(b) / rayY;
        if (t < nearPlane || t > farPlane()) {
            return {0.0F, 0.0F, 0.0F, false};
        }
        const auto target = flatGround(centerX, centerY);
        return {
            target.x + distance * std::cos(b) * std::sin(a) + t * rayX,
            0.0F,
            target.z + distance * std::cos(b) * std::cos(a) + t * rayZ
        };
    }

    template <typename ElevationAt>
    WorldPoint ground(float x, float y, ElevationAt elevationAt) const {
        const auto worldRay = ray(x, y);
        if (worldRay.direction.y >= -0.00001F) {
            return {0.0F, 0.0F, 0.0F, false};
        }

        float distance = -worldRay.origin.y / worldRay.direction.y;
        for (int iteration = 0; iteration < 8; ++iteration) {
            const float worldX = worldRay.origin.x +
                                 worldRay.direction.x * distance;
            const float worldZ = worldRay.origin.z +
                                 worldRay.direction.z * distance;
            const float nextDistance =
                (elevationAt(worldX, worldZ) - worldRay.origin.y) /
                worldRay.direction.y;
            if (std::abs(nextDistance - distance) < 0.001F) {
                distance = nextDistance;
                break;
            }
            distance = nextDistance;
        }
        if (perspective && (distance < nearPlane || distance > farPlane())) {
            return {0.0F, 0.0F, 0.0F, false};
        }
        const float worldX = worldRay.origin.x +
                             worldRay.direction.x * distance;
        const float worldZ = worldRay.origin.z +
                             worldRay.direction.z * distance;
        return {worldX, elevationAt(worldX, worldZ), worldZ};
    }

    void orbit(float turn, float tilt) noexcept {
        yaw = std::fmod(yaw + turn, 360.0F);
        if (yaw < 0.0F) {
            yaw += 360.0F;
        }
        pitch = std::clamp(pitch + tilt, 20.0F, 80.0F);
    }

    void orbitAt(float x, float y, float turn, float tilt) noexcept {
        auto anchor = ground(x, y);
        if (!anchor.valid) {
            x = centerX;
            y = centerY;
            anchor = ground(x, y);
        }
        orbit(turn, tilt);
        centerOn(anchor, x, y);
    }

    void zoomAt(float x, float y, float newScale) noexcept {
        auto anchor = ground(x, y);
        if (!anchor.valid) {
            x = centerX;
            y = centerY;
            anchor = ground(x, y);
        }
        scale = std::clamp(newScale, 0.08F, 8.0F);
        centerOn(anchor, x, y);
    }

    void setPose(float newYaw, float newPitch, float newScale, float focusX,
                 float focusZ) noexcept {
        yaw = std::fmod(newYaw, 360.0F);
        if (yaw < 0.0F) {
            yaw += 360.0F;
        }
        pitch = std::clamp(newPitch, 20.0F, 80.0F);
        scale = std::clamp(newScale, 0.08F, 8.0F);
        focusAt(focusX, focusZ);
    }

    void centerOn(WorldPoint anchor, float screenX, float screenY) noexcept {
        const auto current = ground(screenX, screenY);
        if (!anchor.valid || !current.valid) {
            return;
        }
        const auto delta = flatProject({anchor.x - current.x, 0.0F,
                                        anchor.z - current.z});
        originX -= delta.x - originX;
        originY -= delta.y - originY;
    }

private:
    static constexpr float radians = 0.017453292519943295F;

    ProjectedPoint flatProject(WorldPoint point) const noexcept {
        const float a = yaw * radians;
        const float b = pitch * radians;
        const float right = std::cos(a) * point.x - std::sin(a) * point.z;
        const float forward = std::sin(a) * point.x + std::cos(a) * point.z;
        return {
            originX + scale * right,
            originY + scale * (std::sin(b) * forward - std::cos(b) * point.y),
            0.5F - (std::cos(b) * forward + std::sin(b) * point.y) / depthRange,
            1.0F
        };
    }

    WorldPoint flatGround(float x, float y) const noexcept {
        const float a = yaw * radians;
        const float b = pitch * radians;
        const float right = (x - originX) / scale;
        const float forward = (y - originY) / (scale * std::sin(b));
        return {
            std::cos(a) * right + std::sin(a) * forward,
            0.0F,
            -std::sin(a) * right + std::cos(a) * forward
        };
    }
};

} // namespace rts
