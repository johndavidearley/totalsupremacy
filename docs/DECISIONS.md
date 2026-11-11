# Architectural Decisions

## ADR 001 — Separate simulation from renderer

**Decision:** Keep the core simulation as a library with no windowing or
graphics dependency.

**Rationale:** Rendering/input technology can change without rewriting game
rules. This follows the simulation-first separation proven in the
`urban-sim-core` model project while keeping the first implementation small.

**Tradeoff:** The current host directly translates input to simulation calls;
a formal command stream is deferred until replay or networking is implemented.

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

**Tradeoff:** This does not yet establish cross-platform bitwise determinism;
floating-point movement must be replaced or constrained before deterministic
multiplayer is claimed.

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
