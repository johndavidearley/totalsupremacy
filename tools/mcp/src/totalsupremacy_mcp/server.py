"""FastMCP stdio server for a local Total Supremacy process."""

from __future__ import annotations

import argparse
import math
from contextlib import asynccontextmanager
from pathlib import Path
from typing import Annotated, Any

import anyio
from mcp.server.fastmcp import FastMCP
from mcp.types import ToolAnnotations
from pydantic import Field

from . import __version__
from .session import GameSession

UINT32_MAX = 4_294_967_295
MAP_WIDTH = 2000
MAP_HEIGHT = 1400


def create_server(session: GameSession) -> FastMCP:
    @asynccontextmanager
    async def lifespan(_server: FastMCP):
        try:
            yield {}
        finally:
            with anyio.CancelScope(shield=True):
                await session.stop()

    server = FastMCP(
        "Total Supremacy",
        instructions=(
            "Control one local Total Supremacy session. Launch it, inspect paginated "
            "unit state, select one or more units, and issue move orders with "
            "terrain-aware routing feedback. The simulation starts paused for "
            "deterministic MCP control; advance it explicitly with ticks."
        ),
        lifespan=lifespan,
    )
    read_only = ToolAnnotations(
        readOnlyHint=True, destructiveHint=False, openWorldHint=False
    )
    local_action = ToolAnnotations(
        readOnlyHint=False, destructiveHint=False, openWorldHint=False
    )

    @server.tool(annotations=local_action)
    async def launch_game() -> dict[str, Any]:
        """Start the 3D game paused and return its current state."""
        return await session.launch()

    @server.tool(annotations=read_only)
    async def get_game_state() -> dict[str, Any]:
        """Read tick, selection, latest move-routing feedback, pause, and camera state."""
        if not session.running:
            return {"running": False, "server_version": __version__}
        return {**await session.request("state"), "running": True}

    @server.tool(annotations=read_only)
    async def diagnostics() -> dict[str, Any]:
        """Read whether the owned game process is running and its recent stderr output."""
        return session.diagnostics()

    @server.tool(annotations=read_only)
    async def list_units(
        offset: Annotated[int, Field(ge=0)] = 0,
        limit: Annotated[int, Field(ge=1, le=128)] = 64,
    ) -> dict[str, Any]:
        """Read a page of units with positions, destinations, path counts, and selection."""
        return await session.request(f"units {offset} {limit}")

    @server.tool(annotations=local_action)
    async def select_unit(
        unit_id: Annotated[int, Field(ge=1, le=UINT32_MAX)],
    ) -> dict[str, Any]:
        """Select one unit by stable unit ID, replacing the current selection."""
        return await session.request(f"select {unit_id}")

    @server.tool(annotations=local_action)
    async def select_units(
        unit_ids: Annotated[
            list[Annotated[int, Field(ge=1, le=UINT32_MAX)]],
            Field(min_length=1, max_length=1000),
        ],
    ) -> dict[str, Any]:
        """Select multiple units by stable IDs, replacing the current selection."""
        command = f"select_units {len(unit_ids)} " + " ".join(map(str, unit_ids))
        return await session.request(command)

    @server.tool(annotations=local_action)
    async def clear_selection() -> dict[str, Any]:
        """Clear the current unit selection."""
        return await session.request("clear_selection")

    @server.tool(annotations=local_action)
    async def move_selected(
        x: Annotated[float, Field(ge=0, le=MAP_WIDTH, allow_inf_nan=False)],
        z: Annotated[float, Field(ge=0, le=MAP_HEIGHT, allow_inf_nan=False)],
    ) -> dict[str, Any]:
        """Order selected units to move to a battlefield position."""
        return await session.request(f"move {x:.7g} {z:.7g}")

    @server.tool(annotations=local_action)
    async def set_paused(paused: bool) -> dict[str, Any]:
        """Pause or resume real-time simulation."""
        return await session.request(f"pause {1 if paused else 0}")

    @server.tool(annotations=local_action)
    async def advance_ticks(
        ticks: Annotated[int, Field(ge=1, le=600)] = 1,
    ) -> dict[str, Any]:
        """Advance an exact number of ticks; the game must be paused."""
        return await session.request(f"advance {ticks}")

    @server.tool(annotations=local_action)
    async def set_camera(
        yaw: Annotated[float, Field(ge=-3600, le=3600, allow_inf_nan=False)],
        pitch: Annotated[float, Field(ge=20, le=80, allow_inf_nan=False)],
        scale: Annotated[float, Field(ge=0.08, le=8, allow_inf_nan=False)],
        focus_x: Annotated[float, Field(ge=0, le=MAP_WIDTH, allow_inf_nan=False)],
        focus_z: Annotated[float, Field(ge=0, le=MAP_HEIGHT, allow_inf_nan=False)],
        perspective: bool = False,
    ) -> dict[str, Any]:
        """Set the orbit camera yaw, pitch, zoom, focus, and projection mode."""
        return await session.request(
            f"camera {yaw:.7g} {pitch:.7g} {scale:.7g} "
            f"{focus_x:.7g} {focus_z:.7g} {1 if perspective else 0}"
        )

    @server.tool(annotations=local_action)
    async def stop_game() -> dict[str, Any]:
        """Close the current game process."""
        return await session.stop()

    @server.resource("totalsupremacy://controls")
    async def controls() -> str:
        """Describe the interactive 3D camera and mouse controls."""
        return (
            "WASD/arrows pan; wheel zooms toward the pointer; right-drag orbits; "
            "middle-drag pans; T toggles perspective; Home resets the camera; "
            "left-click selects a unit or drag-selects a group; right-click "
            "issues a path-routed move order; Space "
            "pauses/resumes; Escape exits."
        )

    @server.resource("totalsupremacy://state")
    async def current_state() -> dict[str, Any]:
        """Expose current game state as a resource."""
        return await get_game_state()

    return server


def main() -> None:
    parser = argparse.ArgumentParser(description="Total Supremacy MCP server")
    project_root = Path(__file__).resolve().parents[4]
    parser.add_argument(
        "--game-executable",
        type=Path,
        default=project_root / "build" / "TotalSupremacy",
        help="Path to the built TotalSupremacy executable",
    )
    parser.add_argument(
        "--headless",
        action="store_true",
        help="Run the simulation without opening the 3D window",
    )
    parser.add_argument("--timeout", type=float, default=10.0)
    args = parser.parse_args()

    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("--timeout must be finite and positive")

    session = GameSession(
        args.game_executable, headless=args.headless, timeout=args.timeout
    )
    create_server(session).run(transport="stdio")


if __name__ == "__main__":
    main()
