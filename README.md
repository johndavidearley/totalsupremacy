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

The renderer requires a display and Vulkan-capable driver. On Linux, install Vulkan headers,
the loader, shaderc, GLFW, and a Vulkan-capable driver from the distribution.

### Windows Release build

Install Visual Studio 2022 or Build Tools 2022 with the C++ desktop workload,
Windows SDK, and CMake tools (3.21 or later). Install the Windows Vulkan SDK and open a fresh
Developer PowerShell. If using a standalone SDK directory, set `VULKAN_SDK`
to that directory before configuring.

Run from the repository root:

```powershell
cmake --preset windows-msvc
cmake --build --preset windows-release --parallel
ctest --preset windows-release
.\build\windows-msvc\Release\TotalSupremacy.exe --skirmish
```

CMake downloads GLFW 3.4 on the first configure. GLFW and the Microsoft C++
runtime are linked statically. The Vulkan loader comes from the Windows
graphics driver installation. Double-click the Release executable to open
the sandbox.

CMake uses Python 3.10 or later for the control and skirmish replay tests.
Pass `-DPython3_EXECUTABLE=C:/path/to/python.exe` at configure time to select
an interpreter. Install `tools/mcp` into that environment to include the MCP
SDK test.

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

After completing the Windows Release build, run:

```powershell
.\build\windows-msvc\Release\totalsupremacy_gpu_benchmark.exe --gpu 1800
```

Linux validation is pending the first CI run.
The experimental benchmark selects the first compatible Vulkan compute device. Check its
`device` output to confirm it used the intended GPU, especially on hybrid
laptops. It compares each result against CPU A* and exits on a mismatch.
Details and the existing macOS baseline are in
[docs/PERFORMANCE.md](docs/PERFORMANCE.md).

## Current Slice

The desktop application renders a geometric 3D battlefield and 1,000 units,
with a smoothly elevated, depth-tested terrain heightfield and unit geometry,
camera orbit/pan/anchored zoom, orthographic and perspective views, unit
selection, and move orders. Group move orders share one elevation-aware flow
field and keep formation slots. One-unit orders use A*, and Dijkstra remains
available for benchmark comparison. Simulation updates run at a fixed
30 Hz. The window title reports unit/selection counts,
tick count, frame rate, and recent average/maximum tick duration. This remains
an initial playable engineering slice, not a complete RTS.

Controls: WASD or arrows pan; mouse wheel zooms toward the pointer; right-drag
orbits; middle-drag pans; T toggles perspective; left click selects the nearest
unit or left-drag selects a rectangle. Right-click a hostile unit to attack or
the ground to move. Hold Shift while right-clicking to queue either order.
Selected units show their routed paths and destinations, and active attack
targets are outlined in red. The window
title briefly reports how many units were successfully routed; Space
pauses/resumes; Home resets the camera; Escape exits.

Launch with `--skirmish` to command four blue infantry and rangers against two
red defenders at the western ridge pass. The defenders engage blue units that
enter weapon range. Eliminate both red units to win. The window title shows
survivor counts and the result. CTest runs `ridge_skirmish_replay` twice and
compares final state hashes.

Launch with `--hold-the-pass` for the objective scenario. Move blue infantry
into the marked circle and keep red units outside it for five uninterrupted
seconds. Rangers can cover the infantry while the red force counterattacks.
You have 16 seconds to complete the objective. The blue bar shows hold progress,
the gold bar shows time remaining, and the title shows the result. Press **R**
to restart either scenario. CTest replays both the win and loss twice.

## MCP control

The local MCP server uses stdio and launches the desktop game when
`launch_game` is called. It can also use `--headless` for automated sessions.
The simulation starts paused for MCP sessions. While that bridge is attached,
the wall clock does not step the simulation; `advance_ticks` is the only
clock and still requires the pause flag. Interactive play without the bridge
steps from the wall clock when unpaused.

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
issue move and attack orders, queue a follow-up order, assign teams and archetypes, inspect routing feedback,
pause/resume, advance exact ticks, and set the 3D camera. The `state` result includes `state_hash`, the decimal
FNV-1a 64-bit hash of authoritative gameplay. Camera pose and the pause flag
are not part of that hash. No network
listener is opened. Use `--headless` as an additional server argument for
automation that does not need a visible window.

Automated playtests replay a JSON scenario against a fresh paused game. Steps
are commands or assertions (`eq`, `count`, `lt`, `le`, `gt`, `ge`) on dotted
state fields. `--repeats 2` runs the scenario twice and compares a canonical
hash of the final state. Each run keeps `run.json`, `actions.jsonl`,
`game.log`, `scenario.json`, `canonical.json`, and `replay.json`.

```sh
PYTHONPATH=tools/mcp/src python3 -m totalsupremacy_mcp.scenarios \
  tools/mcp/examples/smoke.json \
  --executable "$PWD/build/TotalSupremacy" \
  --artifacts playtest-artifacts \
  --repeats 2
```

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
