#pragma once

#include "rts/Camera3D.hpp"
#include "rts/Simulation.hpp"

#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace rts {

class ControlBridge {
public:
    ControlBridge();

    std::vector<std::string> takePending();
    static std::string execute(const std::string& request, Simulation& simulation,
                               Camera3D& camera, bool& paused, bool& running);

private:
    struct State {
        std::mutex mutex;
        std::deque<std::string> requests;
    };

    std::shared_ptr<State> state_;
};

} // namespace rts
