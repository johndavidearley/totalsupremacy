# Architectural Decisions

## ADR 001 — Separate simulation from renderer

**Decision:** Keep the core simulation as a library with no windowing or
graphics dependency.

**Rationale:** Rendering/input technology can change without rewriting game
rules. This follows the simulation-first separation proven in the
`urban-sim-core` model project while keeping the first implementation small.

**Tradeoff:** The host still translates input into simulation calls. Each
caller-facing call appends to a command log, and `stateHash` covers
authoritative gameplay. Networking is not implemented.

## ADR 002 — Vulkan behind the GLFW desktop host

**Decision:** Use GLFW for window/input and Vulkan for depth-tested 3D
presentation. Compile GLSL shaders to SPIR-V as part of the CMake build.

**Rationale:** Match the Vulkan/GLFW stack used by UrbanSimCore while keeping
graphics APIs out of the simulation library. On Intel macOS, GLFW, Vulkan,
shaderc, and MoltenVK are installed through MacPorts.

**Tradeoff:** The renderer owns swapchain, synchronization, depth-image, and
resize lifecycle code. GPU-driven strategic-zoom representations and
larger-scale render benchmarks remain future work.

## ADR 003 — C++20 and fixed 30 Hz ticks

**Decision:** Use C++20 and a fixed 30 Hz simulation tick.

**Rationale:** The tick rate gives movement a stable time basis and reduces the
number of updates required by the first slice. Rendering is decoupled through
an accumulator in the host.

**Tradeoff:** Each tick moves units in integer milliunits: 2,400 milliunits,
which is 2.4 world units at 72 units per second. The integrator does not use
floating-point square root or division. Entering a waypoint charges the
floored length of that segment, so a tick can run long by less than one
milliunit per waypoint entered. Formation slots, the group centroid, waypoint
polylines, segment samples, and the geometric part of a polyline cost are
the same milliunit arithmetic. Elevation samples and edge costs use integer
milli-heights. Obstacle tests compare those lattice points with exact
rectangle bounds. Each caller-facing command is appended to a log, and
`stateHash` is an FNV-1a 64-bit digest of authoritative gameplay in
little-endian milliunits. Replaying the log on a fresh simulation of the
same size reproduces the hash. Camera pose, the pause flag, pathfinding
metrics, static obstacles, and the log itself are outside the hash.
Lockstep networking is not implemented.

## ADR 004 — Lightweight native tests

**Decision:** Use a small standalone test executable registered with CTest.

**Rationale:** The current C++ behaviors need only direct assertions, and the
core test target should build without a submodule or fetched dependency. MCP
integration tests use the MCP SDK already required by the Python server.

**Tradeoff:** Add a test framework when richer fixtures, parameterized tests,
or reporting justify the dependency.

## ADR 005 — MCP stdio host controls the owned game process

**Decision:** Run a Python FastMCP stdio server that launches the game as a
child process. The child exposes a bounded line-oriented local control protocol
only when started with `--mcp-control`; no network listener is opened.

**Rationale:** MCP remains outside the simulation and renderer. Game mutations
are queued to the application thread, so UI, MCP actions, and ticks do not race.
The model project's process-owned MCP session pattern is retained, with a
headless mode for tests.

**Tradeoff:** The MCP package requires Python 3.10+ and is installed separately.
MCP controls are serialized and limited to the currently implemented game
actions; resource/tool surface will grow with simulation features.

## ADR 006 — Shared flow field for group movement

**Decision:** Orders of two or more units build one Dijkstra integration field
from the traversable cell nearest the clicked point. Each unit keeps the
formation slot it already received. The route is the centroid spine with that
unit's offset interpolated from its current position to its slot, pulled onto
the spine where the offset would hit terrain. If the offset polyline is
blocked, the unit follows the field from its start and from its slot until
those paths meet. A one-unit order stays on A*. Explicit A* and Dijkstra
remain per-unit searches.

**Rationale:** Per-unit A* was the large-order cost: 79.325 ms median for
1,800 units routed to the far corner. The shared field expanded once for that
order and finished in 26.004 ms, with every unit placed on a formation path
and no individual-search fallback. The earlier per-unit GPU A* prototype was
slower than CPU A* on this machine, so another per-unit GPU search is not the
next pathfinding step.

**Tradeoff:** Installed group routes are not individually optimal. Their
reported travel cost runs about 6–12% above the per-unit A* grid cost. The
field is rebuilt for every group order. A click inside collision uses the
nearest open cell instead of failing the order.

## ADR 007 — Elevation cost prefers the western pass

**Decision:** Keep one elevation weight, `kElevationCostPerHeightUnit` of 180,
on the absolute height change of each grid edge. Store heights as integer
milli-heights and charge `ceil(|milli-height change| * 180 / 1000)`. Shape
the starter heightfield as a low base, two broad rises, and a ridge from
x=160 to x=1100 at z=650 with a pass at x=560, using an integer series for
the bumps. Do not change unit speed or make steep ground impassable.

**Rationale:** At the previous weight of 50, a steep cell added only a few
percent to a flat step, so optimal routes climbed the hills. The lead unit
ordered due north now shifts east onto the pass and stays well below the
crest. The same order on gentle ground stays on the straight line. A* and
Dijkstra still report the same cost. The integer series replaces `exp` so
the cached grid does not depend on libm. The GPU pathfinding heuristic still
multiplies the float view of those milli-heights by 180; edge costs still
come from the host table.

**Tradeoff:** Cross-ridge orders expand more search nodes than the old gentle
hills. Steep ground remains walkable, only more expensive. The weight is a
gameplay constant for this battlefield, not a measured performance budget.

## ADR 008 — Lockstep waits on a local divergence check

**Decision:** Do not add peers, sockets, or rollback. A future lockstep
session would exchange the caller-facing command log and compare `stateHash`
after each command. `findReplayDivergence` is that comparison on one machine.
Camera pose, the pause flag, and rendering stay outside the exchanged log.

**Rationale:** The command log replays to the same hash. The divergence check
stops at the first command whose payload differs, or the first command after
which the hashes differ. That identifies a desync without a network session.
The log retains every step for the life of the simulation, so a long match
needs an exchange policy before any peer protocol is worth adding.

**Tradeoff:** Two logs can be compared only where both are already in memory.
This decision does not choose a wire format, a tick budget, or a transport.

## ADR 009 — One ranged attack, resolved after movement

**Decision:** Every starter unit is one infantry archetype with 100 health.
`setTeam` assigns a team. `attackSelected` stores a target id on each
selected living unit. After movement on that tick, each living attacker
deals 10 damage to its target when the target is alive, on another team,
and within 40 world units. Distance is milliunits, compared without a
square root. At 0 health the unit stays in the vector, drops selection and
its attack, and no longer moves or blocks selection. There is no projectile,
cooldown, or second archetype.

**Rationale:** The starter army is one team, so existing move orders do not
cause damage. A test or MCP session opts in by assigning another team.
Keeping the corpse in place preserves stable unit ids and the vector order
already covered by the state hash. The hash format is version 2 and includes
team, alive, health, and attack target.

**Tradeoff:** An attacker does not chase. Out-of-range orders deal no damage
until something else closes the distance. The desktop click path still issues
moves. Multiple archetypes and a command queue remain future work.
