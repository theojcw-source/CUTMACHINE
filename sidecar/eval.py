"""Reproducible 15-case prompt/model evaluation harness."""

from __future__ import annotations

import argparse
import json
import os
from dataclasses import dataclass
from fractions import Fraction
from pathlib import Path
from typing import Any

from .binary import BinaryError, CutmachineBinary
from .planner import (
    AnthropicPlanner,
    OllamaPlanner,
    Plan,
    Planner,
    PlannerError,
)


S1 = "01K40000000000000000000001"
S2 = "01K40000000000000000000002"
T1 = "01K40000000000000000000003"
T2 = "01K40000000000000000000007"
A1 = "01K40000000000000000000004"
A2 = "01K40000000000000000000005"
A3 = "01K40000000000000000000006"
B1 = "01K40000000000000000000008"


def _remove(clip_id: str) -> dict[str, Any]:
    return {"type": "RemoveClip", "clip_id": clip_id, "exact_timeline": []}


def _trim(clip_id: str, edge: str, frames: int) -> dict[str, Any]:
    return {
        "type": "TrimClip", "clip_id": clip_id, "edge": edge,
        "delta": {"value": frames, "rate": 25}, "exact_clip": None,
    }


def _insert(track_id: str, source_id: str, source_in: int,
            duration: int, timeline_in: int) -> dict[str, Any]:
    return {
        "type": "InsertClip", "track_id": track_id, "source_id": source_id,
        "source_in": {"value": source_in, "rate": 25},
        "duration": {"value": duration, "rate": 25},
        "timeline_in": {"value": timeline_in, "rate": 25},
        "clip_id": "", "exact_timeline": [],
    }


@dataclass(frozen=True)
class EvalCase:
    instruction: str
    expected: dict[str, Any]


CASES: tuple[EvalCase, ...] = (
    EvalCase("Supprime le clip A1.", _remove(A1)),
    EvalCase("Enlève le deuxième clip de la piste vidéo 1.", _remove(A2)),
    EvalCase("Supprime A3, le plan issu d'illustrations.mov.", _remove(A3)),
    EvalCase("Retire l'unique clip de la deuxième piste.", _remove(B1)),
    EvalCase("Raccourcis la fin de A1 de 10 images.", _trim(A1, "Tail", -10)),
    EvalCase("Retire 5 images à la fin de A2.", _trim(A2, "Tail", -5)),
    EvalCase("Coupe les 10 premières images de A3.", _trim(A3, "Head", 10)),
    EvalCase("Récupère 5 images avant le début actuel de A2.", _trim(A2, "Head", -5)),
    EvalCase("Prolonge la fin de A3 de 10 images.", _trim(A3, "Tail", 10)),
    EvalCase("Enlève 5 images au début de B1.", _trim(B1, "Head", 5)),
    EvalCase(
        "Sur la piste vidéo 1, insère à l'image 50 dix images de "
        "interview.mov à partir de sa source 200.",
        _insert(T1, S1, 200, 10, 50),
    ),
    EvalCase(
        "Au début de la piste vidéo 2, insère les 15 premières images de "
        "illustrations.mov.",
        _insert(T2, S2, 0, 15, 0),
    ),
    EvalCase(
        "Ajoute à la fin de la piste vidéo 1 vingt images de interview.mov "
        "depuis l'image source 300.",
        _insert(T1, S1, 300, 20, 150),
    ),
    EvalCase(
        "Dans le trou après A1, à l'image 50, place 10 images de "
        "illustrations.mov depuis l'image source 50.",
        _insert(T1, S2, 50, 10, 50),
    ),
    EvalCase(
        "Insère au tout début de la piste vidéo 2 cinq images de "
        "interview.mov à partir de l'image source 400.",
        _insert(T2, S1, 400, 5, 0),
    ),
)


def _time_equal(left: Any, right: Any) -> bool:
    try:
        return Fraction(left["value"], left["rate"]) == Fraction(
            right["value"], right["rate"])
    except (KeyError, TypeError, ValueError, ZeroDivisionError):
        return False


def operations_equal(actual: dict[str, Any], expected: dict[str, Any]) -> bool:
    """Compare edit semantics while tolerating equivalent rational timebases."""
    if actual.get("type") != expected.get("type"):
        return False
    operation_type = expected["type"]
    if operation_type == "RemoveClip":
        return actual.get("clip_id") == expected["clip_id"]
    if operation_type == "TrimClip":
        return (
            actual.get("clip_id") == expected["clip_id"]
            and actual.get("edge") == expected["edge"]
            and _time_equal(actual.get("delta"), expected["delta"])
        )
    if operation_type == "InsertClip":
        return (
            actual.get("track_id") == expected["track_id"]
            and actual.get("source_id") == expected["source_id"]
            and _time_equal(actual.get("source_in"), expected["source_in"])
            and _time_equal(actual.get("duration"), expected["duration"])
            and _time_equal(actual.get("timeline_in"), expected["timeline_in"])
        )
    return False


def evaluate(planner: Planner, timeline: dict[str, Any]) -> tuple[int, int]:
    successes = 0
    print(f"\nBackend: {planner.backend_name}")
    for index, case in enumerate(CASES, start=1):
        try:
            plan: Plan = planner.plan(timeline, case.instruction)
            passed = plan.operation is not None and operations_equal(
                plan.operation, case.expected)
            actual: Any = plan.operation if plan.operation is not None else {
                "refusal": plan.refusal}
        except PlannerError as exc:
            passed = False
            actual = {"planner_error": str(exc)}
        successes += int(passed)
        print(f"{index:02d} {'PASS' if passed else 'FAIL'} — {case.instruction}")
        if not passed:
            print("   attendu:", json.dumps(case.expected, ensure_ascii=False))
            print("   obtenu  :", json.dumps(actual, ensure_ascii=False))
    rate = 100.0 * successes / len(CASES)
    print(f"Résultat {planner.backend_name}: {successes}/{len(CASES)} ({rate:.1f} %)")
    return successes, len(CASES)


def _planners(name: str, model: str | None) -> list[Planner]:
    if name == "ollama":
        return [OllamaPlanner(model=model)]
    if name == "anthropic":
        return [AnthropicPlanner(model=model)]
    return [OllamaPlanner(model=model), AnthropicPlanner(model=model)]


def main() -> int:
    parser = argparse.ArgumentParser(description="Évalue les planners CUTMACHINE")
    parser.add_argument(
        "--backend", choices=("ollama", "anthropic", "all"),
        default=os.environ.get("CUTMACHINE_BACKEND", "ollama").lower())
    parser.add_argument("--model", help="remplace CUTMACHINE_MODEL")
    parser.add_argument(
        "--document", type=Path,
        default=Path(__file__).with_name("eval-document.json"))
    parser.add_argument("--binary", help="chemin du binaire cutmachine")
    args = parser.parse_args()
    try:
        timeline = CutmachineBinary(args.binary).describe(args.document)
        totals = [evaluate(planner, timeline)
                  for planner in _planners(args.backend, args.model)]
    except (BinaryError, PlannerError) as exc:
        print(f"Erreur d'évaluation : {exc}")
        return 1
    return 0 if all(success == total for success, total in totals) else 1


if __name__ == "__main__":
    raise SystemExit(main())
