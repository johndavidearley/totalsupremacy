#include "rts/ControlBridge.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>
#include <unordered_set>

namespace rts {
namespace {

constexpr std::size_t maxUnitPageSize = 128;
constexpr int maxAdvanceTicks = 600;

std::string error(const char* message) {
    return std::string("{\"ok\":false,\"error\":\"") + message + "\"}";
}

bool noExtraArguments(std::istringstream& stream) {
    std::string extra;
    return !(stream >> extra);
}

std::string stateResponse(const Simulation& simulation, const Camera3D& camera,
                          bool paused) {
    const auto focus = camera.ground(camera.centerX, camera.centerY);
    std::ostringstream result;
    result << std::setprecision(7)
           << "{\"ok\":true,\"result\":{\"tick\":" << simulation.tickCount()
           << ",\"unit_count\":" << simulation.units().size()
           << ",\"selected_count\":" << simulation.selectedCount()
           << ",\"paused\":" << (paused ? "true" : "false")
           << ",\"last_move\":{\"valid\":"
           << (simulation.lastMoveCommand().valid ? "true" : "false")
           << ",\"requested_x\":" << simulation.lastMoveCommand().requestedDestination.x
           << ",\"requested_z\":" << simulation.lastMoveCommand().requestedDestination.y
           << ",\"units_ordered\":" << simulation.lastMoveCommand().unitsOrdered
           << ",\"units_routed\":" << simulation.lastMoveCommand().unitsRouted
           << ",\"issued_at_tick\":" << simulation.lastMoveCommand().issuedAtTick
           << "}"
           << ",\"camera\":{\"yaw\":" << camera.yaw
           << ",\"pitch\":" << camera.pitch
           << ",\"scale\":" << camera.scale
           << ",\"perspective\":" << (camera.perspective ? "true" : "false")
           << ",\"focus_x\":" << focus.x << ",\"focus_z\":" << focus.z
           << "}}}";
    return result.str();
}

} // namespace

ControlBridge::ControlBridge() : state_(std::make_shared<State>()) {
    const auto state = state_;
    std::thread([state] {
        std::string line;
        while (std::getline(std::cin, line)) {
            std::lock_guard lock(state->mutex);
            state->requests.push_back(std::move(line));
        }
        std::lock_guard lock(state->mutex);
        state->requests.emplace_back("stop");
    }).detach();
}

std::vector<std::string> ControlBridge::takePending() {
    std::vector<std::string> requests;
    std::lock_guard lock(state_->mutex);
    requests.reserve(state_->requests.size());
    while (!state_->requests.empty()) {
        requests.push_back(std::move(state_->requests.front()));
        state_->requests.pop_front();
    }
    return requests;
}

std::string ControlBridge::execute(const std::string& request, Simulation& simulation,
                                   Camera3D& camera, bool& paused, bool& running) {
    std::istringstream stream(request);
    std::string command;
    if (!(stream >> command)) {
        return error("empty command");
    }

    if (command == "state") {
        if (!noExtraArguments(stream)) {
            return error("state takes no arguments");
        }
        return stateResponse(simulation, camera, paused);
    }
    if (command == "units") {
        long long offset = 0;
        int limit = 0;
        if (!(stream >> offset >> limit) || !noExtraArguments(stream) || offset < 0 ||
            limit < 1 || static_cast<std::size_t>(limit) > maxUnitPageSize) {
            return error("units requires offset >= 0 and limit in [1, 128]");
        }
        std::ostringstream result;
        result << std::setprecision(7) << "{\"ok\":true,\"result\":{\"offset\":" << offset
               << ",\"total\":" << simulation.units().size() << ",\"units\":[";
        const auto start = static_cast<unsigned long long>(offset) >= simulation.units().size()
            ? simulation.units().size()
            : static_cast<std::size_t>(offset);
        const auto end = std::min(start + static_cast<std::size_t>(limit), simulation.units().size());
        for (std::size_t i = start; i < end; ++i) {
            const auto& unit = simulation.units()[i];
            if (i != start) {
                result << ',';
            }
            result << "{\"id\":" << unit.id
                   << ",\"x\":" << unit.position.x << ",\"z\":" << unit.position.y
                   << ",\"target_x\":" << unit.destination.x
                   << ",\"target_z\":" << unit.destination.y
                   << ",\"path_remaining\":"
                   << (unit.path.size() - unit.nextWaypoint)
                   << ",\"selected\":" << (unit.selected ? "true" : "false") << '}';
        }
        result << "]}}";
        return result.str();
    }
    if (command == "select") {
        unsigned long long id = 0;
        if (!(stream >> id) || !noExtraArguments(stream) ||
            id > static_cast<unsigned long long>(UINT32_MAX)) {
            return error("select requires a valid unit id");
        }
        if (!simulation.selectUnit(static_cast<std::uint32_t>(id))) {
            return error("unit id was not found");
        }
        return stateResponse(simulation, camera, paused);
    }
    if (command == "select_units") {
        std::size_t count = 0;
        if (!(stream >> count) || count == 0 || count > simulation.units().size()) {
            return error("select_units requires between 1 and the unit count IDs");
        }
        std::vector<std::uint32_t> ids;
        ids.reserve(count);
        std::unordered_set<std::uint32_t> uniqueIds;
        for (std::size_t i = 0; i < count; ++i) {
            unsigned long long id = 0;
            if (!(stream >> id) || id == 0 ||
                id > static_cast<unsigned long long>(UINT32_MAX) ||
                !uniqueIds.insert(static_cast<std::uint32_t>(id)).second) {
                return error("select_units requires unique valid unit IDs");
            }
            ids.push_back(static_cast<std::uint32_t>(id));
        }
        if (!noExtraArguments(stream)) {
            return error("select_units received too many IDs");
        }
        for (const auto id : ids) {
            if (std::none_of(simulation.units().begin(), simulation.units().end(),
                    [id](const Unit& unit) { return unit.id == id; })) {
                return error("select_units contains an unknown unit ID");
            }
        }
        simulation.selectUnits(ids);
        return stateResponse(simulation, camera, paused);
    }
    if (command == "clear_selection") {
        if (!noExtraArguments(stream)) {
            return error("clear_selection takes no arguments");
        }
        simulation.clearSelection();
        return stateResponse(simulation, camera, paused);
    }
    if (command == "move") {
        float x = 0.0F;
        float z = 0.0F;
        if (!(stream >> x >> z) || !noExtraArguments(stream) || !std::isfinite(x) ||
            !std::isfinite(z) || x < 0.0F || x > Simulation::kMapWidth ||
            z < 0.0F || z > Simulation::kMapHeight) {
            return error("move coordinates must be within the battlefield");
        }
        const auto moved = simulation.moveSelected({x, z});
        if (moved == 0) {
            return error("select one or more units before issuing a move");
        }
        return stateResponse(simulation, camera, paused);
    }
    if (command == "pause") {
        int value = 0;
        if (!(stream >> value) || !noExtraArguments(stream) || (value != 0 && value != 1)) {
            return error("pause requires 0 or 1");
        }
        paused = value == 1;
        return stateResponse(simulation, camera, paused);
    }
    if (command == "advance") {
        int ticks = 0;
        if (!(stream >> ticks) || !noExtraArguments(stream) ||
            ticks < 1 || ticks > maxAdvanceTicks) {
            return error("advance ticks must be in [1, 600]");
        }
        if (!paused) {
            return error("pause the simulation before advancing explicit ticks");
        }
        for (int i = 0; i < ticks; ++i) {
            simulation.step();
        }
        return stateResponse(simulation, camera, paused);
    }
    if (command == "camera") {
        float yaw = 0.0F;
        float pitch = 0.0F;
        float scale = 0.0F;
        float focusX = 0.0F;
        float focusZ = 0.0F;
        int perspective = 0;
        if (!(stream >> yaw >> pitch >> scale >> focusX >> focusZ >> perspective) ||
            !noExtraArguments(stream) || !std::isfinite(yaw) || !std::isfinite(pitch) ||
            !std::isfinite(scale) || !std::isfinite(focusX) || !std::isfinite(focusZ) ||
            focusX < 0.0F || focusX > Simulation::kMapWidth || focusZ < 0.0F ||
            focusZ > Simulation::kMapHeight || (perspective != 0 && perspective != 1)) {
            return error("camera requires finite pose values and an in-bounds focus");
        }
        camera.setPose(yaw, pitch, scale, focusX, focusZ);
        camera.perspective = perspective == 1;
        return stateResponse(simulation, camera, paused);
    }
    if (command == "stop") {
        if (!noExtraArguments(stream)) {
            return error("stop takes no arguments");
        }
        running = false;
        return stateResponse(simulation, camera, paused);
    }
    return error("unknown command");
}

} // namespace rts
