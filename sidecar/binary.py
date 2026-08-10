"""Safe subprocess boundary for the CUTMACHINE executable."""

from __future__ import annotations

import json
import os
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Any


class BinaryError(RuntimeError):
    pass


@dataclass(frozen=True)
class ApplyResult:
    ok: bool
    doc_hash: str | None = None
    error: str | None = None
    detail: str | None = None


def _canonical_time(value: dict[str, Any]) -> dict[str, Any]:
    return {"value": value["value"], "rate": value["rate"]}


def _canonical_operation(operation: dict[str, Any]) -> dict[str, Any]:
    """Order the existing operation format as required by its C++ reader."""
    try:
        operation_type = operation["type"]
        if operation_type == "InsertClip":
            return {
                "type": operation_type,
                "track_id": operation["track_id"],
                "source_id": operation["source_id"],
                "source_in": _canonical_time(operation["source_in"]),
                "duration": _canonical_time(operation["duration"]),
                "timeline_in": _canonical_time(operation["timeline_in"]),
                "clip_id": operation["clip_id"],
                "exact_timeline": operation["exact_timeline"],
            }
        if operation_type == "RemoveClip":
            return {
                "type": operation_type,
                "clip_id": operation["clip_id"],
                "exact_timeline": operation["exact_timeline"],
            }
        if operation_type == "TrimClip":
            return {
                "type": operation_type,
                "clip_id": operation["clip_id"],
                "edge": operation["edge"],
                "delta": _canonical_time(operation["delta"]),
                "exact_clip": operation["exact_clip"],
            }
    except KeyError as exc:
        raise BinaryError(f"operation is missing {exc.args[0]!r}") from exc
    raise BinaryError(f"unknown operation type {operation_type!r}")


class CutmachineBinary:
    def __init__(self, executable: str | os.PathLike[str] | None = None) -> None:
        default = Path(__file__).resolve().parents[1] / "build" / "cutmachine"
        self.executable = str(
            executable or os.environ.get("CUTMACHINE_BINARY", default))

    def _run(self, arguments: list[str]) -> tuple[int, dict[str, Any]]:
        try:
            completed = subprocess.run(
                [self.executable, *arguments],
                check=False,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
        except OSError as exc:
            raise BinaryError(f"unable to execute {self.executable}: {exc}") from exc
        try:
            payload = json.loads(completed.stdout)
        except json.JSONDecodeError as exc:
            detail = completed.stderr.strip() or completed.stdout.strip()
            raise BinaryError(
                f"cutmachine returned non-JSON output (status {completed.returncode}): "
                f"{detail}") from exc
        if not isinstance(payload, dict):
            raise BinaryError("cutmachine stdout must be a JSON object")
        if completed.returncode not in (0, 1):
            detail = completed.stderr.strip() or payload
            raise BinaryError(
                f"cutmachine exited with status {completed.returncode}: {detail}")
        return completed.returncode, payload

    def describe(self, document: str | os.PathLike[str]) -> dict[str, Any]:
        status, payload = self._run(["--describe", str(document)])
        if status != 0:
            raise BinaryError(
                f"{payload.get('error', 'UnknownError')}: "
                f"{payload.get('detail', '')}")
        return payload

    def apply_operation(
        self, document: str | os.PathLike[str], operation: dict[str, Any]
    ) -> ApplyResult:
        status, payload = self._run([
            "--apply-op", str(document),
            json.dumps(_canonical_operation(operation), ensure_ascii=False,
                       separators=(",", ":")),
        ])
        if status == 0 and payload.get("ok") is True:
            return ApplyResult(ok=True, doc_hash=payload.get("doc_hash"))
        if status == 1 and payload.get("ok") is False:
            return ApplyResult(
                ok=False,
                error=str(payload.get("error", "InvalidOperation")),
                detail=str(payload.get("detail", "")),
            )
        raise BinaryError("cutmachine status and JSON payload disagree")
