# Agent State

## Current milestone

3D and MCP architecture increment implemented: standalone C++20 simulation
library; GLFW/Vulkan host with SPIR-V shaders, swapchain/depth targets, frame
synchronization, and the UrbanSimCore-style orbit camera; 1,000 deterministic
starter units; fixed 30 Hz updates; selection and move orders; title-bar
profiling; and a Python FastMCP stdio server controlling the owned game process
through a queued local bridge. Screen-space drag selection works with both
camera projections. Multi-unit move feedback is visible in the window and MCP
state, and units route around shared obstacles and smooth elevation changes
using deterministic A*.

## Validation status

The full Release `-Werror` Vulkan build passes against the installed MacPorts
GLFW, Vulkan loader/headers, shaderc, and MoltenVK packages; both available
CTest tests pass. A full-population regression confirms all 1,000 selected
units receive routes, including units whose start cells are conservatively
blocked. A prior flat-terrain headless order routed all 1,000 units in 0.059
seconds. The live renderer
created a MoltenVK swapchain on Intel Iris Plus graphics during a 5-second
smoke test. MCP validation used Python 3.14 and `mcp==1.26.0`. No performance
target or non-macOS build has been verified.

A repeatable Release headless benchmark now measures route-command latency and
per-tick simulation cost at 250 through 1,800 units. Its first local baseline
is documented in `docs/PERFORMANCE.md`; at 1,000 units it measured 98.251 ms
median routing and 0.038 ms median per simulation tick.

Nearest traversable goal lookup now expands from the destination cell instead
of scanning the full 7,000-cell grid for every unit. The same benchmark
measured 72.241 ms median at 1,000 units and 169.251 ms at 1,800 units, about
26% and 22% lower than the first baseline respectively.

The benchmark now covers nearby, center, and far-corner destinations and
reports goal-cell scans, blocked-start scans, and A* node expansions per unit.
The 1,800-unit far-corner case averages 445 A* expansions per unit, compared
with about 2.5 goal-cell checks. A* larger-g tie-breaking and expanding
start-anchor lookup reduced the latest 1,800-unit far-corner median to
20.638 ms, with 59.299 expansions and 0.328 start-cell examinations per unit.
Route-quality regressions now cover multiple start locations and destination
types, validate each route segment and simulation position against terrain
collision, and advance units through route completion.
The live window title reports CPU scene-build average/maximum, vertex-buffer
upload and command-recording time, generated vertex count, and asynchronous
Vulkan GPU render-pass timestamps separately from simulation tick timing. An
eight-second MoltenVK smoke test passed with timestamp queries enabled on Intel
Iris Plus Graphics.
Scene-geometry generation has been separated into a renderer-independent CPU
geometry module and a headless benchmark. The first local 1,800-unit
baseline is 0.828 ms unselected and 1.973 ms with 100 routed selected units;
Vulkan upload and GPU work are excluded.
The live Vulkan benchmark profiles CPU scene build, upload, command recording,
and GPU render pass at a fixed 1440x900 swapchain. On Intel Iris Plus Graphics,
the 1,800-unit 100-selected case measured 3.435 ms median scene build,
0.534 ms upload, 0.010 ms command recording, and 2.371 ms GPU render pass.
The simulation supports an opt-in Dijkstra comparison while keeping A* as the
default. Both algorithms route the representative terrain scenarios with equal
integer path costs; the pre-elevation 1,800-unit benchmark showed Dijkstra
expanding roughly 51–99 times more nodes. Current elevation-aware comparison
results are summarized below and recorded in `docs/PERFORMANCE.md`.

The shared terrain is now a bilinearly interpolated heightfield used for
elevation-aware route edge costs and 3D terrain rendering. A* includes the
unavoidable endpoint elevation difference in its admissible lower bound.
Elevated-ground picking works in both camera projections. The current
1,800-unit far-corner route benchmark measured 80.522 ms A* median versus
1,070.811 ms for Dijkstra, with matching average integer path cost. Cached
elevation-adjusted transitions showed a roughly 6% lower median in the repeat
run without changing route work or cost counters; more trials are needed to
separate this modest change from noise. The
1,800-unit unselected CPU scene-build median is 2.125 ms for the tessellated
terrain mesh. A live 1440x900 Vulkan run measured 3.522 ms scene-build,
0.478 ms upload, 0.010 ms command recording, and 1.595 ms GPU render-pass
median for 1,800 unselected units. Full benchmark details are recorded in
`docs/PERFORMANCE.md`.

An opt-in Vulkan compute benchmark now runs per-unit A* in synchronous batches
of 32, checks every route cost and destination against CPU A*, and validates
the returned path before applying it. On the local Intel Iris Plus Graphics
655, a 1,800-unit far-corner order measured 1,109.699 ms median end-to-end
versus 84.902 ms for CPU A* over three trials. This prototype remains
benchmark-only; it does not establish performance of GPU flow fields or other
GPU implementations.

## Next steps

1. Evaluate whether shared flow fields or other group-routing approaches are
   appropriate for formations before further GPU optimization.
2. Tune elevation cost and terrain shape against desired gameplay, then
   re-profile pathfinding at large unit counts.
3. Re-run the route and live renderer benchmarks on target hardware and add
   fixed budgets after representative measurements.
4. Investigate GPU render-pass p95 spikes before choosing a rendering
   optimization.
5. Revisit movement representation and deterministic arithmetic before replay
   or multiplayer work.
