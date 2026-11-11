# Total Supremacy

A simulation-first real-time strategy prototype. The C++20 simulation is
independent from presentation and tools. The desktop host uses GLFW and Vulkan;
a separate MCP stdio server can launch and control the same game process.

## Build

Requirements: a C++20 compiler, CMake 3.20+, GLFW 3.3+, Vulkan headers and
loader, a Vulkan driver, and `glslc` from shaderc.

On Intel macOS, install the graphics dependencies with MacPorts:

```sh
sudo port install cmake glfw vulkan-headers vulkan-loader MoltenVK shaderc
```

MacPorts uses `/opt/local`; the app automatically selects its MoltenVK ICD
unless `VK_DRIVER_FILES` or `VK_ICD_FILENAMES` is already set.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build --output-on-failure
./build/TotalSupremacy
```

The renderer requires a display and Vulkan-capable driver. On Windows, install
the Vulkan SDK and GLFW development package. On Linux, install Vulkan headers,
the loader, shaderc, GLFW, and a Vulkan-capable driver from the distribution.

### GPU pathfinding benchmark on Linux

Install a C++20 compiler, CMake, GLFW development files, Vulkan headers and
loader, `glslc`, and a Vulkan-capable GPU driver using your distribution's
packages. Then run:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/totalsupremacy_gpu_benchmark --gpu 1800
```

### GPU pathfinding benchmark on Windows

Install Visual Studio's C++ build tools, CMake, the Vulkan SDK (including
`glslc` on `PATH`), an appropriate Vulkan GPU driver, and GLFW. For example,
with vcpkg installed and `VCPKG_ROOT` set in PowerShell:

```powershell
& "$env:VCPKG_ROOT/vcpkg.exe" install glfw3:x64-windows
cmake -S . -B build -A x64 "-DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" "-DVCPKG_TARGET_TRIPLET=x64-windows"
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
.\build\Release\totalsupremacy_gpu_benchmark.exe --gpu 1800
```

Linux and Windows builds have not yet been validated. The experimental
benchmark selects the first compatible Vulkan compute device; check its
`device` output to confirm it used the intended GPU, especially on hybrid
laptops. It compares each result against CPU A* and exits on a mismatch.
Details and the existing macOS baseline are in
[docs/PERFORMANCE.md](docs/PERFORMANCE.md).

## Current Slice

The desktop application renders a geometric 3D battlefield and 1,000 units,
with a smoothly elevated, depth-tested terrain heightfield and unit geometry,
camera orbit/pan/anchored zoom, orthographic and perspective views, unit
selection, and move orders. A* routes account for elevation changes, with
Dijkstra available for benchmark comparison. Simulation updates run at a fixed
30 Hz. The window title reports unit/selection counts,
tick count, frame rate, and recent average/maximum tick duration. This remains
an initial playable engineering slice, not a complete RTS.

Controls: WASD or arrows pan; mouse wheel zooms toward the pointer; right-drag
orbits; middle-drag pans; T toggles perspective; left click selects the nearest
unit or left-drag selects a rectangle; right click gives selected units a move
order. Selected units show their routed paths and destinations, and the window
title briefly reports how many units were successfully routed; Space
pauses/resumes; Home resets the camera; Escape exits.

## MCP control

The local MCP server uses stdio and launches the desktop game when
`launch_game` is called. It can also use `--headless` for automated sessions.
The simulation starts paused for MCP sessions; `advance_ticks` provides exact
fixed-step progression.

```sh
python3.10 -m venv .venv
.venv/bin/python -m pip install -e tools/mcp
.venv/bin/total-supremacy-mcp --game-executable ./build/TotalSupremacy
```

To include the MCP SDK end-to-end test in CTest, configure CMake with
`-DPython3_EXECUTABLE="$PWD/.venv/bin/python"`. Without an MCP-enabled Python
interpreter, CTest still runs the native control-protocol test.

In an MCP client configuration, register a stdio server with command
`/absolute/path/to/.venv/bin/total-supremacy-mcp` and argument
`--game-executable /absolute/path/to/build/TotalSupremacy`. The tools launch
and stop the game, inspect paginated unit state, select one or multiple units,
issue move orders, inspect routing feedback, pause/resume, advance exact ticks,
and set the 3D camera. No network
listener is opened. Use `--headless` as an additional server argument for
automation that does not need a visible window.

For example, the VS Code MCP entry is:

```json
{
  "servers": {
    "total-supremacy": {
      "type": "stdio",
      "command": "/absolute/path/to/.venv/bin/total-supremacy-mcp",
      "args": [
        "--game-executable",
        "/absolute/path/to/build/TotalSupremacy"
      ]
    }
  }
}
```

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md),
[docs/ROADMAP.md](docs/ROADMAP.md), [docs/DECISIONS.md](docs/DECISIONS.md),
[docs/PERFORMANCE.md](docs/PERFORMANCE.md), and
[docs/AGENT_STATE.md](docs/AGENT_STATE.md) for design and current progress.
