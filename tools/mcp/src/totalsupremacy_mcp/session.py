"""Owns a game process and serializes its local control-protocol requests."""

from __future__ import annotations

import asyncio
import json
from collections import deque
from pathlib import Path
from typing import Any


class GameSessionError(RuntimeError):
    """A game-control request failed or the game process is unavailable."""


class GameSession:
    def __init__(
        self, executable: str | Path, *, headless: bool = False, timeout: float = 10.0
    ) -> None:
        self.executable = Path(executable).expanduser().resolve()
        self.headless = headless
        self.timeout = timeout
        self.process: asyncio.subprocess.Process | None = None
        self._lock = asyncio.Lock()
        self._stderr_task: asyncio.Task[None] | None = None
        self._logs: deque[str] = deque(maxlen=200)

    @property
    def running(self) -> bool:
        return self.process is not None and self.process.returncode is None

    def diagnostics(self, lines: int = 50) -> dict[str, Any]:
        return {"running": self.running, "recent_stderr": list(self._logs)[-lines:]}

    async def launch(self) -> dict[str, Any]:
        async with self._lock:
            if self.running:
                raise GameSessionError("A game session is already running")
            await self._terminate_unlocked()
            if not self.executable.is_file():
                raise GameSessionError(f"Game executable not found: {self.executable}")

            arguments = [str(self.executable), "--mcp-control"]
            if self.headless:
                arguments.append("--headless")
            process = await asyncio.create_subprocess_exec(
                *arguments,
                stdin=asyncio.subprocess.PIPE,
                stdout=asyncio.subprocess.PIPE,
                stderr=asyncio.subprocess.PIPE,
            )
            self.process = process
            self._stderr_task = asyncio.create_task(self._collect_stderr(process))
            try:
                state = await self._request_unlocked("state")
            except GameSessionError:
                await self._terminate_unlocked()
                raise
            return {**state, "running": True, "pid": process.pid}

    async def request(self, command: str) -> dict[str, Any]:
        async with self._lock:
            return await self._request_unlocked(command)

    async def stop(self) -> dict[str, Any]:
        async with self._lock:
            if not self.running:
                await self._terminate_unlocked()
                return {"stopped": True}
            result = await self._request_unlocked("stop")
            await self._terminate_unlocked()
            return {**result, "stopped": True}

    async def _request_unlocked(self, command: str) -> dict[str, Any]:
        process = self.process
        if (
            process is None
            or process.returncode is not None
            or process.stdin is None
            or process.stdout is None
        ):
            raise GameSessionError("No active game session; launch one first")

        try:
            process.stdin.write((command + "\n").encode("utf-8"))
            await asyncio.wait_for(process.stdin.drain(), timeout=self.timeout)
            line = await asyncio.wait_for(process.stdout.readline(), timeout=self.timeout)
            if not line:
                await self._terminate_unlocked()
                raise GameSessionError("Game exited without a control response")
            response = json.loads(line.decode("utf-8"))
        except GameSessionError:
            raise
        except (
            asyncio.TimeoutError,
            BrokenPipeError,
            ConnectionResetError,
            OSError,
            UnicodeDecodeError,
            json.JSONDecodeError,
        ) as error:
            await self._terminate_unlocked()
            raise GameSessionError(
                f"Game control request failed: {type(error).__name__}: {error}"
            ) from error

        if not isinstance(response, dict):
            raise GameSessionError("Invalid game control response")
        if response.get("ok") is not True:
            message = response.get("error", "Invalid game control response")
            if not isinstance(message, str):
                message = "Invalid game control response"
            raise GameSessionError(message)
        result = response.get("result")
        if not isinstance(result, dict):
            raise GameSessionError("Game control response has no result object")
        return result

    async def _collect_stderr(self, process: asyncio.subprocess.Process) -> None:
        if process.stderr is None:
            return
        while line := await process.stderr.readline():
            self._logs.append(line.decode("utf-8", errors="replace").rstrip())

    async def _terminate_unlocked(self) -> None:
        process = self.process
        self.process = None
        if process is not None and process.returncode is None:
            if process.stdin is not None:
                process.stdin.close()
            try:
                await asyncio.wait_for(process.wait(), timeout=2.0)
            except asyncio.TimeoutError:
                if process.returncode is None:
                    try:
                        process.kill()
                    except ProcessLookupError:
                        pass
                await process.wait()
        task = self._stderr_task
        self._stderr_task = None
        if task is not None:
            await task
