from __future__ import annotations

import json
import math
import subprocess
import sys


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_control.py GAME_EXECUTABLE")

    process = subprocess.Popen(
        [sys.argv[1], "--mcp-control", "--headless"],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )

    def request(command: str, *, succeeds: bool = True) -> dict:
        assert process.stdin is not None
        assert process.stdout is not None
        process.stdin.write(command + "\n")
        process.stdin.flush()
        line = process.stdout.readline()
        if not line:
            raise AssertionError(f"no response to {command!r}")
        response = json.loads(line)
        if response.get("ok") is not succeeds:
            raise AssertionError(f"unexpected response to {command!r}: {response}")
        return response.get("result", response)

    try:
        state = request("state")
        assert state["unit_count"] == 1000
        assert state["tick"] == 0
        assert state["paused"] is True

        page = request("units 0 2")
        assert page["total"] == 1000
        assert [unit["id"] for unit in page["units"]] == [1, 2]

        state = request("select_units 2 1 2")
        assert state["selected_count"] == 2
        state = request("move 500 500")
        assert state["selected_count"] == 2
        assert state["last_move"]["units_ordered"] == 2
        assert state["last_move"]["units_routed"] == 2
        assert math.isclose(state["last_move"]["requested_x"], 500)
        assert math.isclose(state["last_move"]["requested_z"], 500)
        state = request("advance 10")
        assert state["tick"] == 10
        moved = request("units 0 1")["units"][0]
        assert moved["x"] > 380
        assert moved["z"] > 250
        assert moved["path_remaining"] > 0

        request("pause 0")
        request("advance 1", succeeds=False)
        state = request("camera 52 42 0.4 1000 700 1")
        assert state["camera"]["perspective"] is True
        assert math.isclose(state["camera"]["focus_x"], 1000, abs_tol=0.01)
        request("select 99999", succeeds=False)
        request("select_units 2 1 1", succeeds=False)
        request("select_units 2 1 99999", succeeds=False)
        state = request("clear_selection")
        assert state["selected_count"] == 0

        request("stop")
        process.stdin.close()
        assert process.wait(timeout=3) == 0
        print("MCP game-control protocol tests passed.")
        return 0
    finally:
        if process.poll() is None:
            process.kill()
            process.wait(timeout=3)


if __name__ == "__main__":
    raise SystemExit(main())
