#version 450

layout(location = 0) in vec3 position;
layout(location = 1) in vec3 color;
layout(location = 0) out vec3 vertexColor;

layout(push_constant) uniform Camera {
    vec2 viewport;
    vec2 origin;
    vec2 center;
    vec2 target;
    vec4 angles;
    float scale;
    float depthRange;
    float focalLength;
    float nearPlane;
    float farPlane;
    int perspective;
    int screenOverlay;
} camera;

void main() {
    vertexColor = color;
    if (camera.screenOverlay != 0) {
        gl_Position = vec4(
            2.0 * position.x / camera.viewport.x - 1.0,
            2.0 * position.y / camera.viewport.y - 1.0,
            0.0,
            1.0
        );
        return;
    }

    float cy = camera.angles.x;
    float sy = camera.angles.y;
    float cp = camera.angles.z;
    float sp = camera.angles.w;
    float right;
    float forward;
    float screenX;
    float screenY;
    float depth;
    float clipW = 1.0;
    if (camera.perspective != 0) {
        float dx = position.x - camera.target.x;
        float dz = position.z - camera.target.y;
        right = cy * dx - sy * dz;
        forward = sy * dx + cy * dz;
        clipW = camera.focalLength / camera.scale
              - cp * forward - sp * position.y;
        if (abs(clipW) < 0.00001) {
            clipW = clipW < 0.0 ? -0.00001 : 0.00001;
        }
        screenX = camera.center.x + camera.focalLength * right / clipW;
        screenY = camera.center.y + camera.focalLength
                * (sp * forward - cp * position.y) / clipW;
        depth = camera.farPlane / (camera.farPlane - camera.nearPlane)
              - camera.farPlane * camera.nearPlane
              / ((camera.farPlane - camera.nearPlane) * clipW);
    } else {
        right = cy * position.x - sy * position.z;
        forward = sy * position.x + cy * position.z;
        screenX = camera.origin.x + camera.scale * right;
        screenY = camera.origin.y + camera.scale
                * (sp * forward - cp * position.y);
        depth = 0.5 - (cp * forward + sp * position.y) / camera.depthRange;
    }

    gl_Position = vec4(
        2.0 * screenX / camera.viewport.x - 1.0,
        2.0 * screenY / camera.viewport.y - 1.0,
        depth,
        clipW
    );
}
