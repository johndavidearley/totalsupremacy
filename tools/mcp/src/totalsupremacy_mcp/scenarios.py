"""Replay a declarative playtest scenario. Assertions never execute code."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import select
import subprocess
import time
import uuid
from pathlib import Path
from typing import Any


class PlaytestError(RuntimeError):
    """A scenario step failed. Evidence stays in the run directory."""


_OPERATORS = {"eq", "count", "lt", "le", "gt", "ge"}
_COMMANDS = {
    "state",
    "units",
    "select",
    "select_units",
    "clear_selection",
    "move",
    "pause",
    "advance",
    "camera",
}


def evaluate_assertion(document: dict, field: str, op: str, expected: Any) -> dict:
    current: Any = document
    try:
        for component in field.split("."):
            current = (
                current[int(component)]
                if isinstance(current, list)
                else current[component]
            )
    except (KeyError, IndexError, ValueError, TypeError) as error:
        raise PlaytestError(f"Unknown assertion field: {field}") from error
    if op == "count":
        if not isinstance(current, (list, dict, str)):
            raise PlaytestError("Count assertion requires a collection")
        passed = len(current) == expected
    elif op == "eq":
        passed = current == expected
    elif op in ("lt", "le", "gt", "ge"):
        if type(current) not in (int, float) or type(expected) not in (int, float):
            raise PlaytestError("Numeric assertion requires numbers")
        passed = {
            "lt": lambda: current < expected,
            "le": lambda: current <= expected,
            "gt": lambda: current > expected,
            "ge": lambda: current >= expected,
        }[op]()
    else:
        raise PlaytestError("Assertion operator must be eq, count, lt, le, gt, or ge")
    result = {"field": field, "op": op, "expected": expected, "actual": current, "passed": passed}
    if not passed:
        raise PlaytestError("Assertion failed: " + json.dumps(result))
    return result


def _load_scenario(path: Path) -> dict:
    scenario = json.loads(path.read_text(encoding="utf-8"))
    if scenario.get("version") != 1 or not isinstance(scenario.get("steps"), list):
        raise PlaytestError("Expected a version 1 scenario with a steps list")
    if len(scenario["steps"]) > 10000:
        raise PlaytestError("Scenario has more than 10000 steps")
    for step in scenario["steps"]:
        if not isinstance(step, dict):
            raise PlaytestError("Scenario step must be an object")
        delay = step.get("delay_ms", 0)
        if type(delay) is not int or not 0 <= delay <= 60000:
            raise PlaytestError("Scenario delay must be 0..60000 ms")
        if "assert" in step:
            assertion = step["assert"]
            if not isinstance(assertion, dict) or assertion.get("op") not in _OPERATORS:
                raise PlaytestError("Assertion operator must be eq, count, lt, le, gt, or ge")
            if "field" not in assertion or "expected" not in assertion:
                raise PlaytestError("Assertion requires field and expected")
            continue
        if step.get("command") not in _COMMANDS:
            raise PlaytestError("Unsupported scenario command")
    return scenario


def _unit_ids(args: dict) -> list[int]:
    if "ids" in args:
        ids = args["ids"]
    elif "first" in args and "count" in args:
        ids = list(range(int(args["first"]), int(args["first"]) + int(args["count"])))
    else:
        raise PlaytestError("select_units requires ids or first and count")
    if not ids or len(ids) > 1000 or len(set(ids)) != len(ids):
        raise PlaytestError("select_units requires 1..1000 unique ids")
    if any(type(unit_id) is not int or unit_id < 1 for unit_id in ids):
        raise PlaytestError("select_units ids must be positive integers")
    return ids


def _command_line(step: dict) -> list[str]:
    command = step["command"]
    args = step.get("args") or {}
    if command == "state":
        return ["state"]
    if command == "units":
        return [f"units {int(args.get('offset', 0))} {int(args.get('limit', 64))}"]
    if command == "select":
        return [f"select {int(args['id'])}"]
    if command == "select_units":
        ids = _unit_ids(args)
        return [f"select_units {len(ids)} " + " ".join(str(unit_id) for unit_id in ids)]
    if command == "clear_selection":
        return ["clear_selection"]
    if command == "move":
        return [f"move {float(args['x'])} {float(args['z'])}"]
    if command == "set_team":
        unit_id = int(args["id"])
        team = int(args["team"])
        if unit_id < 1 or not 0 <= team <= 255:
            raise PlaytestError("set_team requires a unit id and a team in 0..255")
        return [f"set_team {unit_id} {team}"]
    if command == "attack":
        target = int(args.get("target", 0))
        if target < 0:
            raise PlaytestError("attack target must be a unit id or 0")
        return [f"attack {target}"]
    if command == "pause":
        paused = args.get("paused", True)
        return [f"pause {1 if paused else 0}"]
    if command == "advance":
        ticks = int(args["ticks"])
        if not 1 <= ticks <= 7200:
            raise PlaytestError("advance ticks must be in [1, 7200]")
        return [f"advance {batch}" for batch in _batches(ticks, 600)]
    if command == "camera":
        perspective = 1 if args.get("perspective", False) else 0
        return [
            "camera "
            f"{float(args['yaw'])} {float(args['pitch'])} {float(args['scale'])} "
            f"{float(args['focus_x'])} {float(args['focus_z'])} {perspective}"
        ]
    raise PlaytestError("Unsupported scenario command")


def _batches(total: int, size: int) -> list[int]:
    batches = []
    while total > 0:
        batches.append(min(size, total))
        total -= size
    return batches


class GameRun:
    def __init__(self, executable: Path, artifacts: Path, scenario: Path, timeout: float):
        self.executable = executable
        self.timeout = timeout
        stamp = time.strftime("%Y%m%d-%H%M%S") + "-" + uuid.uuid4().hex[:8]
        self.run_dir = artifacts / stamp
        self.process: subprocess.Popen[str] | None = None
        self.log_file = None
        self.last_result: dict[str, Any] = {}

    def launch(self, *, headless: bool) -> None:
        self.run_dir.mkdir(parents=True)
        self.log_file = (self.run_dir / "game.log").open("w", encoding="utf-8")
        arguments = [str(self.executable), "--mcp-control"]
        if headless:
            arguments.append("--headless")
        self.process = subprocess.Popen(
            arguments,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=self.log_file,
            text=True,
            cwd=self.run_dir,
        )
        (self.run_dir / "run.json").write_text(
            json.dumps(
                {
                    "version": 1,
                    "executable": str(self.executable),
                    "headless": headless,
                    "pid": self.process.pid,
                },
                indent=2,
            ),
            encoding="utf-8",
        )

    def trace(self, entry: dict) -> None:
        with (self.run_dir / "actions.jsonl").open("a", encoding="utf-8") as stream:
            stream.write(json.dumps(entry, allow_nan=False) + "\n")

    def request_line(self, line: str) -> dict:
        process = self.process
        if process is None or process.poll() is not None or process.stdin is None or process.stdout is None:
            raise PlaytestError("No usable game session; see game.log")
        process.stdin.write(line + "\n")
        process.stdin.flush()
        ready, _, _ = select.select([process.stdout], [], [], self.timeout)
        if not ready:
            raise PlaytestError(f"Timed out waiting for a response to {line.split()[0]}")
        raw = process.stdout.readline()
        if not raw:
            raise PlaytestError("Game exited without responding; see game.log")
        try:
            response = json.loads(raw)
        except json.JSONDecodeError as error:
            raise PlaytestError("Game returned malformed JSON; see game.log") from error
        self.trace({"command": line.split()[0], "ok": response.get("ok")})
        if response.get("ok") is not True:
            raise PlaytestError(str(response.get("error", "command failed")))
        result = response.get("result", {})
        if not isinstance(result, dict):
            raise PlaytestError("Game response result must be an object")
        self.last_result = result
        return result

    def execute(self, step: dict) -> None:
        if "assert" in step:
            assertion = step["assert"]
            section = assertion.get("section", "state")
            query = assertion.get("query") or {}
            if section == "state":
                document = self.request_line("state")
            elif section == "units":
                document = self.request_line(
                    f"units {int(query.get('offset', 0))} {int(query.get('limit', 64))}"
                )
            else:
                raise PlaytestError("Assertion section must be state or units")
            try:
                result = evaluate_assertion(
                    document, assertion["field"], assertion["op"], assertion["expected"]
                )
            except PlaytestError as error:
                self.trace({"assertion": assertion, "failure": str(error)})
                raise
            self.trace({"assertion": assertion, "result": result})
            return

        expected_error = step.get("expected_error")
        for line in _command_line(step):
            try:
                self.request_line(line)
            except PlaytestError as error:
                if expected_error != str(error):
                    raise
                self.trace({"command": line.split()[0], "expected_error": expected_error})
                return
        if expected_error is not None:
            raise PlaytestError("Scenario failure unexpectedly succeeded")

    def snapshot(self, *, require_idle: bool) -> str:
        state = self.request_line("state")
        total = int(state["unit_count"])
        units: list[dict] = []
        problems: list[str] = []
        offset = 0
        while offset < total:
            page = self.request_line(f"units {offset} 128")
            batch = page["units"]
            if not batch:
                break
            for unit in batch:
                if require_idle and (
                    unit["path_remaining"] != 0
                    or not math.isclose(unit["x"], unit["target_x"], abs_tol=0.05)
                    or not math.isclose(unit["z"], unit["target_z"], abs_tol=0.05)
                ):
                    if len(problems) < 5:
                        problems.append(
                            f"id {unit['id']} at ({unit['x']}, {unit['z']}) "
                            f"target ({unit['target_x']}, {unit['target_z']}) "
                            f"path_remaining {unit['path_remaining']}"
                        )
            units.extend(batch)
            offset += len(batch)
        if problems:
            extra = "" if len(problems) < 5 else " (first five shown)"
            raise PlaytestError("Units still moving or short of their targets" + extra + ": " + "; ".join(problems))
        canonical = json.dumps({"state": state, "units": units}, sort_keys=True, separators=(",", ":"))
        (self.run_dir / "canonical.json").write_text(canonical, encoding="utf-8")
        return hashlib.sha256(canonical.encode()).hexdigest()

    def finish(self, report: dict) -> None:
        (self.run_dir / "replay.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
        process = self.process
        if process is not None and process.poll() is None and process.stdin is not None:
            try:
                process.stdin.write("stop\n")
                process.stdin.flush()
                process.wait(timeout=5)
            except Exception:
                process.kill()
                process.wait(timeout=5)
        if self.log_file is not None:
            self.log_file.close()
            self.log_file = None


def replay(executable: Path, scenario_path: Path, artifacts: Path, repeats: int, timeout: float, headless: bool) -> dict:
    if repeats not in (1, 2):
        raise PlaytestError("Replay repeats must be 1 or 2")
    scenario = _load_scenario(scenario_path)
    artifacts.mkdir(parents=True, exist_ok=True)
    reports = []
    for _ in range(repeats):
        run = GameRun(executable, artifacts, scenario_path, timeout)
        report: dict[str, Any] = {"passed": False, "steps": 0}
        try:
            run.launch(headless=headless)
            report["run_directory"] = str(run.run_dir)
            (run.run_dir / "scenario.json").write_text(scenario_path.read_text(encoding="utf-8"), encoding="utf-8")
            for index, step in enumerate(scenario["steps"]):
                run.execute(step)
                report["steps"] = index + 1
            report["canonical_hash"] = run.snapshot(require_idle=bool(scenario.get("require_idle")))
            report["passed"] = True
        except PlaytestError as error:
            report["error"] = str(error)
            if run.run_dir.exists():
                run.finish(report)
            raise PlaytestError(f"Replay failed; evidence at {run.run_dir}: {error}") from error
        run.finish(report)
        reports.append(report)
    if repeats == 2 and reports[0].get("canonical_hash") != reports[1].get("canonical_hash"):
        raise PlaytestError(
            "Canonical game state differs between replays: "
            f"{reports[0].get('canonical_hash')} != {reports[1].get('canonical_hash')}"
        )
    return {"passed": True, "runs": reports, "identical": repeats == 2}


def main() -> None:
    parser = argparse.ArgumentParser(description="Replay a Total Supremacy playtest scenario")
    parser.add_argument("scenario")
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--artifacts", type=Path, default=Path("playtest-artifacts"))
    parser.add_argument("--repeats", type=int, choices=(1, 2), default=1)
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument("--window", action="store_true", help="Open the rendered game instead of --headless")
    args = parser.parse_args()
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("--timeout must be finite and positive")
    executable = args.executable.expanduser().resolve()
    if not executable.is_file():
        parser.error(f"Playtest executable not found: {executable}")
    result = replay(
        executable,
        Path(args.scenario),
        args.artifacts,
        args.repeats,
        args.timeout,
        headless=not args.window,
    )
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
