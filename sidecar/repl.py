"""Interactive, stateless edit loop."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Callable

from .binary import BinaryError, CutmachineBinary
from .planner import Plan, Planner, PlannerError, planner_from_environment


def _confirm(input_fn: Callable[[str], str]) -> bool:
    return input_fn("Appliquer cette opération ? (o/n) ").strip().lower() in {
        "o", "oui", "y", "yes",
    }


def run_turn(
    planner: Planner,
    binary: CutmachineBinary,
    document: Path,
    instruction: str,
    input_fn: Callable[[str], str] = input,
    print_fn: Callable[[str], None] = print,
) -> None:
    timeline = binary.describe(document)
    previous_error = None
    for attempt in range(2):
        plan: Plan = planner.plan(timeline, instruction, previous_error)
        if plan.refusal is not None:
            print_fn(f"Refus du modèle : {plan.refusal}")
            return
        assert plan.operation is not None
        print_fn("Opération proposée :")
        print_fn(json.dumps(plan.operation, ensure_ascii=False, indent=2))
        if not _confirm(input_fn):
            print_fn("Opération annulée.")
            return
        result = binary.apply_operation(document, plan.operation)
        if result.ok:
            print_fn(json.dumps(
                {"ok": True, "doc_hash": result.doc_hash}, ensure_ascii=False))
            return
        error_payload = {
            "ok": False,
            "error": result.error,
            "detail": result.detail,
        }
        print_fn(json.dumps(error_payload, ensure_ascii=False))
        if attempt == 0:
            print_fn("L'erreur est renvoyée au modèle pour un unique second essai.")
            previous_error = error_payload
    print_fn("Abandon après deux opérations refusées.")


def main() -> int:
    parser = argparse.ArgumentParser(description="Pilote chat de CUTMACHINE")
    parser.add_argument("document", type=Path)
    args = parser.parse_args()
    try:
        planner = planner_from_environment()
        binary = CutmachineBinary()
        print(f"Backend : {planner.backend_name} — document : {args.document}")
        while True:
            try:
                instruction = input("cutmachine> ").strip()
            except EOFError:
                print()
                return 0
            if instruction.lower() in {"quit", "exit", "q"}:
                return 0
            if not instruction:
                continue
            run_turn(planner, binary, args.document, instruction)
    except (BinaryError, PlannerError, KeyboardInterrupt) as exc:
        print(f"Erreur : {exc}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
