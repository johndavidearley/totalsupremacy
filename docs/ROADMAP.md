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
- [ ] Formation-level pathfinding.
- [ ] Build/test verification on Windows and Linux; only macOS validated locally.

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
- Define deterministic command and state-hash formats.
- Evaluate lockstep networking and replay/desync diagnostics.
- Add performance budgets from repeatable measurements.

## Phase 5 — Production direction

- Evaluate the long-term renderer/backend strategy against real feature and
  performance requirements; the current Vulkan backend is an initial renderer.
- Add authoring tools, data-driven unit definitions, scenarios, and mod support.
- Expand platform CI and release packaging.

The headless scale benchmark is available; next, profile route-planning cost
and set performance budgets from representative target hardware.
