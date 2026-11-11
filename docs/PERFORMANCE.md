# Performance

## Targets

The initial product targets are not measurements: 1080p/60 FPS on a
representative midrange GPU, thousands of active units, large maps, and stable
simulation under combat. This slice does not establish those capabilities.

## Current instrumentation

The GLFW window title displays frame rate and average/maximum simulation tick
duration, plus average/maximum CPU scene-and-overlay build duration and the
generated vertex count. It separately displays CPU vertex-buffer upload and
command-buffer recording time, plus GPU render-pass time when Vulkan graphics
queue timestamps are supported. GPU timestamps are collected asynchronously
after the corresponding frame fence completes and can lag the current frame.
The GPU measurement covers the render pass, not presentation or display
latency. CPU geometry build, upload, and command recording exclude GPU
execution; values are local observations, not benchmark claims. Geometry for
the battlefield is submitted in batched Vulkan draws with depth testing. There
is no automated timing threshold in the test suite.

### Live Vulkan renderer benchmark

Run the visible-window benchmark on a machine with a Vulkan-capable display:

```sh
cmake --build build --target totalsupremacy_renderer_benchmark --parallel 2
./build/totalsupremacy_renderer_benchmark
```

It uses a non-resizable 1440x900 window and records the actual Vulkan swapchain
extent in each row. The run first warms the GPU with 90 frames of the largest
selected-unit scene, then takes 30 per-case warmup frames and 180 measured
frames for each population (250 through 1,800) in unselected and 100-selected
modes. Selected units receive a far-corner move order before timing; route
planning is excluded from the frame measurements.

Output includes scene-build, host-visible vertex upload, and CPU command-record
median/p95 times, plus asynchronous Vulkan timestamp median/p95 for the
render-pass command interval. GPU results are collected after their frame
fences and are not presentation latency or display time. Resize, driver,
window/compositor, and power settings can affect the measurements; compare only
runs with matching swapchain size and hardware.

One Release run on the Intel macOS development machine (Apple Clang 17,
MoltenVK 1.4.2, Intel Iris Plus Graphics 655, 1440x900 swapchain) produced:

| Units | Mode | Scene median/p95 (ms) | Upload median/p95 (ms) | Commands median/p95 (ms) | GPU pass median/p95 (ms) |
|---:|---|---:|---:|---:|---:|
| 250 | Unselected | 0.220 / 0.266 | 0.052 / 0.070 | 0.010 / 0.032 | 1.935 / 7.364 |
| 250 | 100 selected | 0.763 / 0.935 | 0.199 / 0.253 | 0.010 / 0.012 | 2.058 / 7.494 |
| 500 | Unselected | 0.419 / 0.503 | 0.110 / 0.137 | 0.010 / 0.013 | 1.990 / 7.435 |
| 500 | 100 selected | 1.600 / 1.979 | 0.245 / 0.403 | 0.010 / 0.016 | 2.159 / 7.536 |
| 1,000 | Unselected | 0.817 / 0.977 | 0.222 / 0.283 | 0.010 / 0.013 | 2.124 / 7.563 |
| 1,000 | 100 selected | 1.976 / 2.410 | 0.394 / 0.544 | 0.011 / 0.016 | 2.230 / 7.744 |
| 1,500 | Unselected | 1.760 / 2.220 | 0.315 / 0.480 | 0.011 / 0.014 | 2.254 / 7.743 |
| 1,500 | 100 selected | 2.229 / 2.832 | 0.521 / 0.637 | 0.011 / 0.014 | 2.379 / 7.909 |
| 1,800 | Unselected | 1.964 / 2.475 | 0.427 / 0.552 | 0.011 / 0.015 | 2.276 / 7.832 |
| 1,800 | 100 selected | 3.435 / 4.163 | 0.534 / 0.649 | 0.010 / 0.013 | 2.371 / 8.006 |

This is a local baseline, not a performance budget. GPU p95 spikes are visible
despite much lower medians; repeat on target hardware before setting a frame
budget or changing rendering strategy.

#### Live renderer with elevated terrain

A later Release run on the same Intel macOS machine used a 1440x900 Vulkan
swapchain and the elevated terrain mesh:

| Units | Mode | Scene median/p95 (ms) | Upload median/p95 (ms) | Commands median/p95 (ms) | GPU pass median/p95 (ms) |
|---:|---|---:|---:|---:|---:|
| 250 | Unselected | 0.964 / 1.140 | 0.181 / 0.238 | 0.010 / 0.014 | 2.885 / 8.472 |
| 250 | 100 selected | 2.059 / 2.469 | 0.325 / 0.398 | 0.010 / 0.013 | 2.379 / 8.579 |
| 1,000 | Unselected | 1.940 / 2.384 | 0.347 / 0.440 | 0.010 / 0.014 | 2.301 / 9.461 |
| 1,000 | 100 selected | 3.623 / 4.589 | 0.458 / 0.630 | 0.010 / 0.014 | 2.390 / 6.446 |
| 1,800 | Unselected | 3.522 / 4.322 | 0.478 / 0.564 | 0.010 / 0.012 | 1.595 / 6.245 |
| 1,800 | 100 selected | 3.901 / 4.889 | 0.614 / 0.706 | 0.010 / 0.012 | 2.632 / 6.343 |

This is one local run, not a controlled comparison with the earlier flat
terrain capture; GPU timing varies between runs. CPU scene construction and
upload increased with the tessellated mesh, while command-recording time
remained nearly unchanged. The GPU timestamp covers the render pass only.

## Headless simulation benchmark

Build and run the fixed workload from the repository root:

```sh
cmake --build build --target totalsupremacy_benchmark --parallel 2
./build/totalsupremacy_benchmark
```

The benchmark runs five route-planning trials and nine simulation-step trials
at 250, 500, 1,000, 1,500, and 1,800 units. Each step trial advances 120 ticks
after issuing the same far-corner move command to every unit. Route planning is
measured separately for nearby (520, 350), center (1,000, 700), and far-corner
(1,800, 1,300) destinations. Construction is excluded from route timing, and
route planning is excluded from step timing.

CSV output includes compiler, build type, logical thread count, workload
settings, median and p95 route latency, mean goal- and start-cell lookups per
unit, mean expanded nodes per unit, mean integer path cost per unit, median and
p95 per-tick simulation latency, and a position checksum to confirm the
workload advanced. Each route scenario runs with both A* and Dijkstra over the
same deterministic unit formations. The simulation API exposes these work
counters for the most recent move command; they do not alter the MCP protocol.
All units must route or the benchmark exits with an error.

The simulation regression suite also checks route geometry and completion for
four starting units (including a blocked-cell start) against nearby, center,
far-corner, map-edge, and obstacle-interior targets. It verifies all waypoint
segments and per-tick positions stay outside terrain collision and that each
route reaches its adjusted destination without cancellation.

The current starting formation fits within the 2,000 x 1,400 map through 1,800
units; larger populations would benchmark out-of-map initial positions rather
than a valid battlefield. Results are local observations, not performance
targets. Record the machine model and operating conditions alongside results
when comparing runs. Keep simulation and rendering measurements separate:
measure scene-build time and GPU frame time separately at fixed resolution and
camera pose. Do not parallelize hot loops until profiles show a specific
bottleneck and tests protect the resulting behavior.

### Experimental per-unit Vulkan compute benchmark

The opt-in compute prototype compares individual GPU A* searches with the
normal CPU implementation on the same initial units, formation offsets,
terrain, and obstacle rules:

```sh
cmake --build build --target totalsupremacy_gpu_benchmark --parallel 2
./build/totalsupremacy_gpu_benchmark --gpu 1800
```

The unit count must be between 1 and 1,800. The benchmark runs nearby, center,
and far-corner orders for three trials each, plus single-trial same-cell,
map-edge, and obstacle-interior checks. Every route is checked against CPU A*
for its individual integer grid cost and routed destination; malformed,
blocked, or corner-cutting GPU paths are rejected. A failure exits nonzero.
The benchmark runs without a window and requires a Vulkan compute device.
Regular gameplay and MCP orders continue to use CPU A*.

The GPU version executes one serial A* search per shader invocation in
synchronous batches of 32. Reported command latency includes CPU start/goal
preparation, packing the terrain and queries, submitting and waiting for
compute work, copying results, validating and reconstructing paths, and
installing the order. The CPU timing includes its complete move order.
Construction, Vulkan device/pipeline creation, selection, and the independent
per-unit CPU correctness comparisons are outside both timed intervals.
The prototype has no competing renderer work and does not measure isolated
shader execution or graphics-frame impact.

One Release run on Intel Iris Plus Graphics 655 with MoltenVK 1.4.2 produced:

| Units | Scenario | CPU A* median (ms) | GPU prototype median (ms) |
|---:|---|---:|---:|
| 250 | Nearby | 3.517 | 80.627 |
| 250 | Center | 5.478 | 89.781 |
| 250 | Far corner | 25.804 | 209.345 |
| 1,800 | Nearby | 51.480 | 714.574 |
| 1,800 | Center | 25.794 | 723.431 |
| 1,800 | Far corner | 84.902 | 1,109.699 |

These are local measurements with three trials per case. This particular
per-unit implementation is substantially slower than CPU A* on this device;
it does not rule out different GPU algorithms, larger batches, other hardware,
or shared flow fields.

### Headless CPU scene-geometry benchmark

The renderer's CPU geometry generation is also available without a graphics
context:

```sh
cmake --build build --target totalsupremacy_scene_benchmark --parallel 2
./build/totalsupremacy_scene_benchmark
```

This runs five warmups and 31 timed scene builds at the same five population
sizes. It measures the normal unselected scene and a scene with 100 selected
units ordered toward the far corner, including their visible route lines and
markers. CSV reports median/p95 CPU build time, generated scene vertex/byte
counts, selection-overlay vertex count, and a geometry checksum. Simulation
construction and move planning are outside the timed interval. No Vulkan
device, buffer upload, or GPU is involved, so these numbers correspond to the
scene-generation part of the window's scene-build metric only.

One Release run on the Intel macOS development machine (Apple Clang 17,
8 reported logical threads) produced:

| Units | Mode | Median build (ms) | p95 build (ms) | Vertices |
|---:|---|---:|---:|---:|
| 250 | Unselected | 0.096 | 0.110 | 25,152 |
| 500 | Unselected | 0.181 | 0.251 | 47,652 |
| 1,000 | Unselected | 0.351 | 0.537 | 92,652 |
| 1,500 | Unselected | 0.707 | 0.956 | 137,652 |
| 1,800 | Unselected | 0.828 | 1.295 | 164,652 |
| 250 | 100 selected | 0.376 | 0.499 | 83,850 |
| 500 | 100 selected | 0.601 | 0.832 | 106,350 |
| 1,000 | 100 selected | 0.821 | 1.073 | 151,350 |
| 1,500 | 100 selected | 1.175 | 1.338 | 196,350 |
| 1,800 | 100 selected | 1.973 | 2.569 | 223,350 |

The selected-route case adds geometry proportional to the paths of the selected
units, not just their count. Treat these results as a local baseline, not a
performance budget.

### Initial local baseline

One Release run on the Intel macOS development machine (Apple Clang 17,
8 reported logical threads) produced:

| Units | Route median (ms) | Route p95 (ms) | Step median (ms/tick) | Step p95 (ms/tick) |
|---:|---:|---:|---:|---:|
| 250 | 23.717 | 24.274 | 0.008 | 0.011 |
| 500 | 45.539 | 46.420 | 0.018 | 0.024 |
| 1,000 | 98.251 | 99.089 | 0.038 | 0.091 |
| 1,500 | 172.375 | 199.799 | 0.054 | 0.067 |
| 1,800 | 215.733 | 310.698 | 0.077 | 0.087 |

These are a single local baseline, not a threshold or cross-machine comparison.
The original benchmark used only the far-corner destination. The current
multi-destination benchmark prints each scenario and its trial counts.

### Nearest-goal lookup optimization

The route planner previously scanned all 7,000 grid cells to find the nearest
traversable goal for every unit. It now searches outward from the destination
cell and stops once the closest possible unvisited cell is farther away than
the current best candidate. Equal-distance candidates are still resolved by
the lowest grid index, preserving the previous deterministic choice.

The optimized Release run, on the same machine and workload, produced:

| Units | Route median (ms) | Route p95 (ms) |
|---:|---:|---:|
| 250 | 16.369 | 17.115 |
| 500 | 33.120 | 33.480 |
| 1,000 | 72.241 | 73.987 |
| 1,500 | 133.715 | 137.263 |
| 1,800 | 169.251 | 170.424 |

Compared with the first recorded run, median route latency improved by about
22% at 1,800 units and 26% at 1,000 units. An earlier optimized run had a
310.698 ms p95 at 1,800 units, showing run-to-run noise; use repeated runs
before making threshold decisions. This one-destination benchmark predates the
work-counter and scenario additions below; its timings are historical.

### A* expansion profile

The current instrumented Release benchmark on the same machine produced the
following representative results:

| Units | Scenario | Route median (ms) | Goal cells/unit | Start cells/unit | A* expansions/unit |
|---:|---|---:|---:|---:|---:|
| 1,000 | Nearby | 40.288 | 2.400 | 182.000 | 135.733 |
| 1,000 | Center | 32.157 | 2.432 | 182.000 | 63.190 |
| 1,000 | Far corner | 78.750 | 2.146 | 182.000 | 506.880 |
| 1,800 | Nearby | 159.907 | 2.889 | 213.889 | 196.501 |
| 1,800 | Center | 133.851 | 3.018 | 213.889 | 71.886 |
| 1,800 | Far corner | 200.964 | 2.537 | 213.889 | 445.341 |

In this profile, goal lookup remained local (about 2–3 candidate cells per
unit), while far-corner orders expanded substantially more A* nodes than center
orders. The start-cell count was elevated because a small subset of units
started in conservatively blocked cells and scanned the grid for a clear
anchor. Counters are averages over successfully routed units and trials.

### A* tie-breaking and start-anchor lookup

A* now prefers the larger traveled cost when nodes have equal estimated total
cost. This remains optimal with the consistent octile heuristic, but reduces
expansion on symmetric routes. Selecting a traversable start anchor now searches
outward from the blocked start cell and stops when no unvisited cell can be
closer than the best terrain-clear anchor.

The resulting Release benchmark:

| Units | Scenario | Route median (ms) | Start cells/unit | A* expansions/unit |
|---:|---|---:|---:|---:|
| 1,000 | Nearby | 7.939 | 0.282 | 32.740 |
| 1,000 | Center | 5.684 | 0.282 | 18.585 |
| 1,000 | Far corner | 13.945 | 0.282 | 66.128 |
| 1,800 | Nearby | 18.125 | 0.328 | 41.206 |
| 1,800 | Center | 11.237 | 0.328 | 20.707 |
| 1,800 | Far corner | 20.638 | 0.328 | 59.299 |

At 1,800 units, far-corner median latency fell from 200.964 ms in the initial
instrumented run to 20.638 ms (about 90%). Start-anchor lookup fell from about
214 examined cells per unit on average to fewer than 0.4. The route-work
counters and full-route tests pass; keep the prior tables as historical
measurements, not current expectations.

### Flat-grid A* and Dijkstra comparison (before elevation)

The simulation exposes Dijkstra as an opt-in comparison algorithm; A* remains
the default for normal move orders. Both use the same eight-neighbor grid,
integer orthogonal/diagonal costs, obstacle rules, and deterministic start and
goal selection. Dijkstra uses a zero heuristic. Tests verify successful
terrain-clear routing and equal aggregate path costs for representative
destinations, including obstacle and blocked-start cases.

One Release run on the Intel macOS development machine (Apple Clang 17,
8 reported logical threads; five route trials per case) produced:

| Units | Scenario | Algorithm | Route median (ms) | Route p95 (ms) | Expanded nodes/unit | Path cost/unit |
|---:|---|---|---:|---:|---:|---:|
| 1,000 | Nearby | A* | 8.118 | 10.592 | 32.740 | 25,942.284 |
| 1,000 | Nearby | Dijkstra | 210.996 | 229.162 | 1,954.387 | 25,942.284 |
| 1,000 | Center | A* | 5.889 | 6.569 | 18.585 | 17,570.840 |
| 1,000 | Center | Dijkstra | 99.705 | 100.251 | 964.833 | 17,570.840 |
| 1,000 | Far corner | A* | 13.060 | 13.536 | 66.128 | 62,297.060 |
| 1,000 | Far corner | Dijkstra | 575.993 | 576.101 | 5,859.556 | 62,297.060 |
| 1,800 | Nearby | A* | 17.456 | 17.810 | 41.206 | 36,175.614 |
| 1,800 | Nearby | Dijkstra | 520.623 | 522.892 | 2,846.667 | 36,175.614 |
| 1,800 | Center | A* | 10.491 | 11.340 | 20.707 | 19,780.966 |
| 1,800 | Center | Dijkstra | 195.598 | 203.428 | 1,066.327 | 19,780.966 |
| 1,800 | Far corner | A* | 20.380 | 21.266 | 59.299 | 54,833.238 |
| 1,800 | Far corner | Dijkstra | 850.728 | 867.008 | 4,811.965 | 54,833.238 |

Both searches produced the same optimal integer grid cost in each measured
case, while Dijkstra expanded roughly 51–99 times as many nodes as A* at 1,800
units. This validates A* as the default for individual unit routes in the
flat-cost grid used at the time of this run; it does not measure
elevation-aware path costs or shared flow-field searches. Results are local
observations, not performance targets.

### Elevation-aware pathfinding and terrain geometry

The simulation now uses a deterministic heightfield sampled at the 20-unit
terrain-grid vertices and bilinearly interpolated for ground placement and
rendering. Each traversable grid edge adds
`ceil(abs(heightChange) * 50)` integer cost units to its existing orthogonal
or diagonal movement cost. The A* heuristic adds the corresponding floor of
the unavoidable current-to-goal elevation change to the octile distance; this
remains a lower bound with the rounded edge costs. Dijkstra uses a zero
heuristic. Regression tests verify elevation varies, both algorithms agree on
route costs, and elevated ground picking round-trips in orthographic and
perspective views. The eight directional, elevation-adjusted edge costs are
precomputed with the terrain grid rather than recalculated for each expanded
neighbor.

Elevation currently changes route choice and the rendered height of terrain,
obstacles, units, routes, and markers. It does not change unit speed or make
steep terrain impassable.

One Release run on the Intel macOS development machine (Apple Clang 17,
8 reported logical threads; five route trials per case) produced:

| Units | Scenario | Algorithm | Route median (ms) | Expanded nodes/unit | Path cost/unit |
|---:|---|---|---:|---:|---:|
| 1,000 | Center | A* | 9.615 | 39.153 | 18,012.894 |
| 1,000 | Center | Dijkstra | 117.343 | 956.658 | 18,012.894 |
| 1,000 | Far corner | A* | 60.821 | 323.411 | 64,228.646 |
| 1,000 | Far corner | Dijkstra | 715.970 | 5,893.536 | 64,228.646 |
| 1,800 | Center | A* | 23.807 | 58.844 | 20,330.638 |
| 1,800 | Center | Dijkstra | 230.745 | 1,069.271 | 20,330.638 |
| 1,800 | Far corner | A* | 80.522 | 241.819 | 56,041.441 |
| 1,800 | Far corner | Dijkstra | 1,070.811 | 4,805.382 | 56,041.441 |

Both searches returned equal path costs for all benchmark cases. At 1,800
units, A* expanded about 20 times fewer nodes for the far-corner scenario and
about 18 times fewer for the center scenario. The terrain lower-bound heuristic
helps keep the additional route cost tractable, though the far-corner A*
median is higher than the preceding flat-grid measurement because elevation
changes the optimal routes and search work. Compared with the immediately
preceding five-trial run, these medians are about 6% lower for the
1,800-unit far-corner case after precomputing edge costs; expansion and path
cost counters are unchanged. This modest difference is indicative only, as
five trials are sensitive to run-to-run variation.

The rendered 20-unit heightfield increases scene geometry. The updated CPU
scene benchmark measured:

| Units | Mode | Median build (ms) | p95 build (ms) | Vertices |
|---:|---|---:|---:|---:|
| 1,000 | Unselected | 1.044 | 1.485 | 150,576 |
| 1,000 | 100 selected | 2.174 | 2.805 | 209,304 |
| 1,800 | Unselected | 2.125 | 2.692 | 222,576 |
| 1,800 | 100 selected | 2.621 | 3.284 | 281,304 |

These are local observations, not performance budgets. The scene benchmark
excludes vertex upload and GPU rendering; profile the live renderer separately
before drawing conclusions about frame time.
