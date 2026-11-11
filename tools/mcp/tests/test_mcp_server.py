from __future__ import annotations

import asyncio
import json
import sys
from pathlib import Path

from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client


async def run_server_test(game_executable: Path) -> None:
    parameters = StdioServerParameters(
        command=sys.executable,
        args=[
            "-m",
            "totalsupremacy_mcp.server",
            "--game-executable",
            str(game_executable.resolve()),
            "--headless",
        ],
    )
    async with stdio_client(parameters) as (read_stream, write_stream):
        async with ClientSession(read_stream, write_stream) as client:
            await client.initialize()
            tools = await client.list_tools()
            tool_names = {tool.name for tool in tools.tools}
            assert {
                "launch_game",
                "get_game_state",
                "diagnostics",
                "list_units",
                "select_unit",
                "select_units",
                "move_selected",
                "set_paused",
                "advance_ticks",
                "set_camera",
                "stop_game",
            } <= tool_names

            launched = await client.call_tool("launch_game", {})
            assert launched.structuredContent["unit_count"] == 1000
            assert launched.structuredContent["paused"] is True

            units = await client.call_tool("list_units", {"offset": 0, "limit": 2})
            assert [unit["id"] for unit in units.structuredContent["units"]] == [1, 2]

            selected = await client.call_tool("select_units", {"unit_ids": [1, 2]})
            assert selected.structuredContent["selected_count"] == 2
            moved = await client.call_tool("move_selected", {"x": 500, "z": 500})
            assert moved.structuredContent["last_move"]["units_ordered"] == 2
            assert moved.structuredContent["last_move"]["units_routed"] == 2
            routed_units = await client.call_tool(
                "list_units", {"offset": 0, "limit": 2}
            )
            assert all(
                unit["path_remaining"] > 0
                for unit in routed_units.structuredContent["units"]
            )
            advanced = await client.call_tool("advance_ticks", {"ticks": 12})
            assert advanced.structuredContent["tick"] == 12

            camera = await client.call_tool(
                "set_camera",
                {
                    "yaw": 50,
                    "pitch": 42,
                    "scale": 0.4,
                    "focus_x": 1000,
                    "focus_z": 700,
                    "perspective": True,
                },
            )
            assert camera.structuredContent["camera"]["perspective"] is True

            resources = await client.list_resources()
            assert {str(resource.uri) for resource in resources.resources} >= {
                "totalsupremacy://controls",
                "totalsupremacy://state",
            }
            state_resource = await client.read_resource("totalsupremacy://state")
            state = json.loads(state_resource.contents[0].text)
            assert state["tick"] == 12

            stopped = await client.call_tool("stop_game", {})
            assert stopped.structuredContent["stopped"] is True


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_mcp_server.py GAME_EXECUTABLE")
    asyncio.run(run_server_test(Path(sys.argv[1])))
    print("MCP server integration tests passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
