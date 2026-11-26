# Roadmap

This is a forward-looking sequence, not a claim that the listed features exist.

## Phase 1 — Playable foundation

- [x] C++ simulation library separate from presentation.
- [x] Depth-tested 3D battlefield, camera orbit/pan/zoom, and projection toggle.
- [x] 1,000 units, selection, move orders, fixed-step simulation, and core tests.
- [x] MCP stdio tools for state inspection, unit actions, camera, and exact ticks.
- [x] Screen-space rectangle selection and terrain obstacle collision.
- [x] Multi-unit command feedback and terrain-aware waypoint routing.
- [x] Deterministic terrain heightfield, elevation-aware grid costs, and
  A*/Dijkstra path-cost comparison.
- [x] Formation-level pathfinding. Group orders share one flow field and keep
  per-unit formation slots. One-unit orders stay on A*.
- [x] Verify the Windows Release build, CTest suite, and Vulkan window.
- [x] Support move, attack, and queued orders in a playable desktop skirmish.
- [ ] Verify the Linux Release build and headless tests through CI.

## Phase 2 — Tactical simulation

- Unit archetypes, health, weapons, targeting, damage, and destruction.
- Formation and command-queue behavior.
- Profile and optimize simulation/pathfinding at large unit counts.
- Stable unit identity and serialized command replay.

## Phase 3 — World systems

- Large maps, terrain-aware movement, hierarchical navigation, and water.
- Resource production/consumption, construction, repair, and fog of war.
- Opponent behaviors and basic strategic objectives.

## Phase 4 — Scale and multiplayer foundations

- Profile before introducing parallel simulation work.
- [x] Define deterministic command and state-hash formats.
- [x] Evaluate lockstep networking and replay/desync diagnostics. The check
  is local. No peer session is implemented.
- Add performance budgets from repeatable measurements.

## Phase 5 — Production direction

- Evaluate the long-term renderer/backend strategy against real feature and
  performance requirements; the current Vulkan backend is an initial renderer.
- Add authoring tools, data-driven unit definitions, scenarios, and mod support.
- Expand platform CI and release packaging.

Group orders now use the shared flow field measured in `docs/PERFORMANCE.md`.
Elevation cost and the western ridge are tuned so routes take the pass
instead of the crest. The local GPU render-pass p95 spike was a per-frame
swapchain rebuild, not the pass itself. Movement, path construction, and
elevation costs use integer arithmetic.
A 1,800-unit group order spends its time sampling per-unit offsets of the
shared spine. A segment that misses every obstacle is rejected before those
samples. The flow-field travel cost is unchanged.
The simulation records a caller-facing command log and an FNV-1a state hash;
replaying the log reproduces the hash. Two logs can be compared locally at
the first differing command. A selected unit chases an enemy with A* and
attacks inside its own range. Infantry reaches 40 world units and reloads
in 10 ticks; a ranger reaches 70 and reloads in 20. A shot travels 8
world units per tick and hits when it reaches its target. Dead units stay in the
vector. An appended move or attack waits
behind the active order; an immediate order clears that queue. Multiplayer
is not implemented. Next, set performance
budgets from representative target hardware. The local measurements are not
yet budgets. Windows is verified locally, and Linux is pending a CI run.
