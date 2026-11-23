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
selected living unit. A living enemy beyond 40 world units gets one A*
path to that target's current position. The path is kept until it runs
out, then rebuilt if the target is still out of range. Entering range
clears the path before the next move. After movement on that tick, each
living attacker deals 10 damage when the target is alive, on another team,
and inside that range. Distance is milliunits, compared without a square
root. A later move clears the attack. At 0 health the unit stays in the
vector, drops selection and its attack, and no longer moves or blocks
selection. There is no projectile or cooldown. A second archetype is ADR 010.

**Rationale:** The starter army is one team, so existing move orders do not
cause damage. A test or MCP session opts in by assigning another team.
Keeping the corpse in place preserves stable unit ids and the vector order
already covered by the state hash. The hash includes team, alive, health,
and attack target.

**Tradeoff:** The chase does not steer around a moving target until the
current path runs out. The desktop click path still issues moves. Orders
queue behind the active command in ADR 011.

## ADR 010 — Ranger is a longer-range infantry variant

**Decision:** Add one more archetype, ranger. Infantry stays at 100 health,
10 damage, and 40 world units of range. A ranger has 70 health, 6 damage,
and 70 world units of range. Both move at 2,400 milliunits per tick.
`setArchetype` clamps current health down to the new maximum and does not
heal it back. Starter units remain infantry.

**Rationale:** The two stat lines make range matter before a second
movement speed or a weapon system exists. A ranger can hit a target that an
infantry unit still has to chase. The state hash format is version 3 and
includes the archetype.

**Tradeoff:** There is still no projectile. Weapon cooldown is ADR 012.
The desktop click path still issues an immediate move.

## ADR 011 — Orders queue behind the active command

**Decision:** `moveSelected` and `attackSelected` take an append flag. The
default replaces the active order and clears the queue. Append on an idle
unit starts immediately, including a group flow-field move. Append on a
busy unit stores the order. A queued move remembers the formation slot from
issue time and routes with A* when it starts. The next order begins after
movement and attacks on the tick the current move runs out of path, or the
attack target is dead or missing. The new order's movement starts on the
following tick.

**Rationale:** Click-to-move stays a replacement, which is what the desktop
path already does. A queue lets a playtest or MCP session chain a move and
an attack without cancelling the first. The queue is part of the version 4
state hash, so replay sees it.

**Tradeoff:** A queued group does not rebuild a shared flow field when the
order starts. Each unit paths alone to the slot it was given. The desktop
click path has no shift-queue binding.

## ADR 012 — Weapons reload between shots

**Decision:** Each archetype has a cooldown in ticks. Infantry reloads in
10 ticks and a ranger in 20. A living unit's cooldown counts down every
tick, including while it chases or moves. After movement, a ready attacker
launches the shot in ADR 013, then reloads. A shot that is not in range
does not spend the cooldown.
Changing archetype clamps a longer remaining cooldown down to the new
weapon and does not add time. Death clears the cooldown. The state hash
format is version 5 and includes the remaining cooldown.

**Rationale:** The two stat lines previously fired every tick, so a fight
was over in a third of a second and the ranger's lower damage barely
mattered. A reload makes the longer range cost a slower shot. Counting
the timer down during a chase means a weapon that reloaded on the way can
fire on the tick it enters range.

**Tradeoff:** The shot's travel time is ADR 013. A reload only decides when
the next shot launches.

## ADR 013 — Shots travel to the target

**Decision:** A ready attacker launches a projectile instead of damaging on
the firing tick. The shot stores the attacker's damage and team and starts
at the attacker's position. On later ticks it moves 8 world units toward
the target's current position. It hits when the remaining distance is
within that step, then deals its stored damage. A shot whose target is
missing, dead, or on the firing team expires without damage. Clearing the
attack or moving the attacker leaves shots already in flight. The state
hash format is version 6 and includes each shot.

**Rationale:** Cooldown made the two weapons fire at different rates, and
the hit was still instant at any point inside range. Travel time makes a
longer shot arrive later, and a target can die or change team before the
shot lands. Integer milliunit steps keep the flight deterministic.

**Tradeoff:** The shot homes on the unit it was fired at. It does not
strike a different unit that crosses its path, and it does not continue
to a point on the ground if the target dies.
