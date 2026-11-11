# Architecture

## Current implementation

The project follows the simulation-first boundary used by UrbanSimCore:

```text
GLFW host (input, camera, fixed-step loop) ── Vulkan renderer
             |
       totalsupremacy_core  <── C++ simulation and camera tests
             ^
             |
MCP stdio server ── private line protocol ── game control queue
```

`totalsupremacy_core` has no GLFW or Vulkan dependency. `Simulation` owns a
contiguous unit vector, initializes a repeatable formation, advances movement
on a fixed 30 Hz tick, and exposes selection and move-command operations.
Move commands calculate deterministic A* routes over an obstacle-inflated
20-unit grid whose cell costs include the absolute elevation change between
neighbors. Terrain elevation is a deterministic, smoothly interpolated
heightfield shared by pathfinding and rendering. A* adds a consistent lower
bound for unavoidable elevation change to its octile heuristic; Dijkstra is
available for opt-in comparisons. If a unit's start cell is conservatively
blocked, routing anchors to the nearest traversable cell with a terrain-clear
approach. Initial units are placed outside terrain collision. Units follow
waypoint routes, and blocked/adjusted orders are reported in the last-command
feedback. Elevation currently affects route cost and presentation, not unit
speed or terrain passability.
An experimental Vulkan compute benchmark batches per-unit searches through a
separate pathfinding backend, sharing CPU start/goal preparation and route
reconstruction. The simulation validates backend paths before applying them;
the normal application and MCP protocol still use CPU A*.
Rendering and camera state are presentation concerns, not authoritative game
state.

The desktop host owns the fixed-step accumulator and caps a frame's contribution
at 250 ms. `Renderer3D` batches terrain and procedural unit geometry into
per-frame Vulkan vertex buffers; a depth attachment handles visibility. GLSL
shaders are compiled to SPIR-V during the CMake build. The swapchain, depth
images, frame synchronization, and resize recreation are owned by the renderer.
The projection shader follows the orbit-camera conventions used by
UrbanSimCore's Vulkan visualization. Camera ray intersection drives ground
movement orders and pointer-anchored zoom; move-order picking intersects the
heightfield.

`--mcp-control` starts a private stdin/stdout control bridge. A reader thread
queues line-oriented requests; the main thread applies them to simulation and
camera state and returns one JSON response per line. This keeps simulation
mutations serialized with rendering and the fixed-step loop. The external
FastMCP server owns this child process and exposes typed tools over MCP stdio.
`--headless` omits GLFW and Vulkan while retaining the same core and MCP actions
for tests.

The MCP time mode starts paused. `advance_ticks` accepts bounded batches of
explicit ticks only while paused; interactive sessions start live. The bridge
supports state, paginated units, selection, movement, pause, explicit
advancement, camera pose, and stop operations. It opens no network socket.

## Boundaries and intended evolution

- **Simulation:** deterministic state and commands; no graphics or platform APIs.
- **Renderer:** Vulkan presentation and the 3D camera adapter; replaceable
  without changing simulation types.
- **MCP host:** Python FastMCP over stdio plus a process-owned local control pipe.
- **Application:** GLFW input, fixed-step loop, command queue, and profiling.
- **Tests:** behavior, camera projection/picking math, and end-to-end headless MCP
  control protocol.

The current simulation is single-threaded and has no networking, combat,
hierarchical navigation, persistence, content pipeline, or mod API. The
Vulkan/GLFW presentation and simulation control have only been validated on
macOS so far.
Add systems only when a playable slice requires them and preserve the explicit
command boundary as replay and multiplayer requirements become concrete.
