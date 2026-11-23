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
A one-unit move uses deterministic A* over an obstacle-inflated 20-unit grid
whose cell costs include the absolute elevation change between neighbors.
A group move builds one Dijkstra integration field from the traversable cell
nearest the order point, keeps each unit's formation slot, and prefers a
terrain-clear offset of the group's centroid route. Explicit A* and Dijkstra
remain available for per-unit comparison. Terrain elevation is a deterministic,
smoothly interpolated heightfield shared by pathfinding and rendering. A*
adds a consistent lower bound for unavoidable elevation change to its octile
heuristic. If a unit's start cell is conservatively blocked, individual
routing anchors to the nearest traversable cell with a terrain-clear approach.
Initial units are placed outside terrain collision. Routes are written on a
0.001-world-unit lattice: formation slots, the group centroid, offset
waypoints, and the samples that test a segment against obstacles. Units follow
those waypoints in integer milliunits. One tick travels 2,400 milliunits,
equal to 72 world units per second at 30 Hz. Terrain elevation uses the same
kind of integer sample, in thousandths of a height unit. Blocked and adjusted
orders are reported in the last-command feedback.
The heightfield is a low base, two broad rises, and a western ridge from
x=160 to x=1100 at z=650 with a pass at x=560. Samples are integer
milli-heights. Each grid edge adds
`ceil(abs(milliHeightChange) * 180 / 1000)` to its geometric cost. That
makes a short detour through the pass cheaper
than climbing the crest, while a gentle slope stays cheaper than leaving a
straight route. Elevation affects route cost and presentation, not unit speed
or terrain passability.

Authoritative gameplay has a command log and a state hash. The log records
caller-facing `selectUnit`, `selectUnits`, `clearSelection`, `moveSelected`
(the destination in milliunits and the CPU algorithm), `setTeam`,
`setArchetype`, `attackSelected`, and `step`. Appended moves and attacks
wait behind the active order. `selectNearest` records the unit it resolved,
or a clear when it misses. A path-backend move is not a replay command.
`replay` applies that log to a fresh simulation of the same unit count by
calling those methods, so the replay records an equal log. `stateHash` is
FNV-1a 64 over little-endian bytes: format version 6, the tick, each unit in
vector order (id, position and destination milliunits, next waypoint index,
selection, team, archetype, alive, health, attack target, attack cooldown,
queued orders, and path waypoints in milliunits), each shot in flight, then the last move (validity, requested milliunit destination,
ordered and routed counts, and issue tick). The hash excludes
the camera, the pause flag, pathfinding metrics, static obstacles, and the
command log. `findReplayDivergence` replays two logs on fresh simulations
and stops at the first command or state-hash disagreement. This is the
replay and desync check. It is not a peer session or a multiplayer
implementation.

An experimental Vulkan compute benchmark batches per-unit searches through a
separate pathfinding backend, sharing CPU start/goal preparation and route
reconstruction. The simulation validates backend paths before applying them.
The normal application and MCP protocol use the shared flow field for group
orders and CPU A* for one-unit orders. The GPU benchmark still compares its
per-unit searches with explicit CPU A*.
Rendering and camera state are presentation concerns, not authoritative game
state.

The desktop host owns the fixed-step accumulator and caps a frame's contribution
at 250 ms. `Renderer3D` batches terrain and procedural unit geometry into
per-frame Vulkan vertex buffers; a depth attachment handles visibility. GLSL
shaders are compiled to SPIR-V during the CMake build. The swapchain, depth
images, frame synchronization, and resize recreation are owned by the renderer.
The drawable size is the Vulkan surface extent when the surface defines one,
and the host sizes the camera and pointer from that size.
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

The MCP time mode starts paused. While the control bridge is attached, the
simulation advances only through explicit ticks, including after the pause
flag is cleared. `advance_ticks` accepts bounded batches and still requires
the pause flag. Interactive sessions without the bridge start live and step
from the wall clock. The bridge
supports state, paginated units, selection, movement, pause, explicit
advancement, camera pose, and stop operations. The state response includes
`state_hash`, the decimal `stateHash` value. It opens no network socket.

## Boundaries and intended evolution

- **Simulation:** deterministic state, command log, and state hash; no graphics or platform APIs.
- **Renderer:** Vulkan presentation and the 3D camera adapter; replaceable
  without changing simulation types.
- **MCP host:** Python FastMCP over stdio plus a process-owned local control pipe.
- **Application:** GLFW input, fixed-step loop, command queue, and profiling.
- **Tests:** behavior, camera projection/picking math, and end-to-end headless MCP
  control protocol.

The current simulation is single-threaded. Combat has two archetypes that
share one movement speed. Infantry has 100 health, 10 damage, and a 40-unit
range. A ranger has 70 health, 6 damage, and a 70-unit range. An attacker
beyond its own range paths to the target with A* and stops on entering
range. Infantry reloads in 10 ticks and a ranger in 20. The timer counts
down every tick the unit is alive. After movement, a ready attacker
launches a shot and then reloads. The shot moves 8 world units per tick
toward its target and deals its damage on arrival. A unit at 0 health stays in
place. A later immediate move or attack clears
the queue. An appended order waits until the active move finishes or its
attack target is gone. There is no networking, hierarchical navigation,
persistence, content pipeline, or mod API. The Vulkan/GLFW presentation and
simulation control have only been validated on macOS so far.
Add systems only when a playable slice requires them. The command log,
state hash, and local divergence check are the replay format. Lockstep
networking was evaluated and is not implemented.
