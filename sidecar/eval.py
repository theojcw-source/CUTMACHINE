"""Reproducible 15-case prompt/model evaluation harness."""

from __future__ import annotations

import argparse
import io
import json
import os
import shutil
import tempfile
import urllib.error
import urllib.request
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
from .schema import PLANNER_RESPONSE_SCHEMA


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

# Kept outside CASES so the fixed historical corpus remains exactly 15 cases.
REFERENCE_CASE = EvalCase(
    "Raccourcis le premier plan de 2 secondes.", _trim(A1, "Tail", -50))


class _ReplayResponse:
    def __init__(self, body: bytes) -> None:
        self._body = body

    def __enter__(self) -> "_ReplayResponse":
        return self

    def __exit__(self, *args: Any) -> None:
        return None

    def read(self) -> bytes:
        return self._body


class TraceOpener:
    """Capture the actual HTTP request and response without changing parsing."""

    def __init__(self) -> None:
        self.request_url: str | None = None
        self.request_payload: dict[str, Any] | None = None
        self.response_text: str | None = None

    def reset(self) -> None:
        self.request_url = None
        self.request_payload = None
        self.response_text = None

    def __call__(self, request: Any, timeout: float) -> _ReplayResponse:
        self.request_url = request.full_url
        try:
            self.request_payload = json.loads(request.data)
        except (TypeError, json.JSONDecodeError):
            self.request_payload = None
        try:
            with urllib.request.urlopen(request, timeout=timeout) as response:
                body = response.read()
        except urllib.error.HTTPError as exc:
            body = exc.read()
            self.response_text = body.decode("utf-8", errors="replace")
            raise urllib.error.HTTPError(
                exc.url, exc.code, exc.msg, exc.headers, io.BytesIO(body)) from exc
        self.response_text = body.decode("utf-8", errors="replace")
        return _ReplayResponse(body)


def _raw_model_output(backend: str, response_text: str | None) -> Any:
    if response_text is None:
        return None
    try:
        response = json.loads(response_text)
    except json.JSONDecodeError:
        return response_text
    if backend == "ollama":
        try:
            return response["message"]["content"]
        except (KeyError, TypeError):
            return response
    blocks = response.get("content", []) if isinstance(response, dict) else []
    for block in blocks if isinstance(blocks, list) else []:
        if (isinstance(block, dict) and block.get("type") == "tool_use" and
                block.get("name") == AnthropicPlanner.TOOL_NAME):
            return block.get("input")
    return response


def _raw_operation(raw_output: Any) -> tuple[dict[str, Any] | None, bool]:
    value = raw_output
    if isinstance(value, str):
        try:
            value = json.loads(value)
        except json.JSONDecodeError:
            return None, False
    if not isinstance(value, dict):
        return None, False
    operation = value.get("operation")
    return (operation if isinstance(operation, dict) else None,
            operation is None and value.get("refusal") is not None)


def _classify_failure(
    raw_output: Any, actual: Any, expected: dict[str, Any], error: str | None,
    timeline: dict[str, Any],
) -> tuple[str, str, list[str]]:
    raw_operation, raw_refusal = _raw_operation(raw_output)
    if raw_refusal or (isinstance(actual, dict) and "refusal" in actual):
        return "D", "refus injustifié", []
    operation = (
        actual if isinstance(actual, dict) and "type" in actual
        else raw_operation
    )
    if operation is None:
        detail = error or "sortie structurée inexploitable"
        return "D", detail, []
    if operation.get("type") != expected.get("type"):
        return "A", (
            f"type {operation.get('type')!r} au lieu de {expected.get('type')!r}"
        ), []

    secondary: list[str] = []
    operation_type = expected["type"]
    id_fields = {
        "RemoveClip": ("clip_id",),
        "TrimClip": ("clip_id",),
        "InsertClip": ("track_id", "source_id"),
    }[operation_type]
    bad_ids = [field for field in id_fields
               if operation.get(field) != expected.get(field)]
    temporal_fields = {
        "RemoveClip": (),
        "TrimClip": ("delta",),
        "InsertClip": ("source_in", "duration", "timeline_in"),
    }[operation_type]
    bad_times = [field for field in temporal_fields
                 if not _time_equal(operation.get(field), expected.get(field))]
    if bad_ids:
        if bad_times:
            secondary.append("C: valeur temporelle également incorrecte (" +
                             ", ".join(bad_times) + ")")
        known_ids = {
            item.get("id")
            for collection in (timeline.get("sources", []),
                               timeline.get("tracks", []))
            for item in collection if isinstance(item, dict)
        }
        known_ids.update(
            item.get("id")
            for track in timeline.get("tracks", []) if isinstance(track, dict)
            for item in track.get("items", []) if isinstance(item, dict)
        )
        absent = [field for field in bad_ids
                  if operation.get(field) not in known_ids]
        if absent:
            return "B", "ULID absent de la vue (" + ", ".join(absent) + ")", secondary
        return "E", "mauvais objet sélectionné (" + ", ".join(bad_ids) + ")", secondary
    if operation_type == "TrimClip" and operation.get("edge") != expected.get("edge"):
        if bad_times:
            secondary.append("C: valeur temporelle également incorrecte (delta)")
        return "E", "bord de trim incorrect", secondary
    if bad_times:
        return "C", "valeur temporelle incorrecte (" + ", ".join(bad_times) + ")", []
    if error:
        if "ULID absent" in error:
            return "B", error, []
        return "D", error, []
    return "E", "divergence sémantique non couverte", []


def _breakpoint(error: str | None, passed: bool, actual: Any) -> str:
    if error:
        if error.startswith("HTTP ") or error.startswith("unable to reach"):
            return "transport_http"
        if "malformed JSON" in error or "returned no valid structured" in error:
            return "decodage_reponse_backend"
        if "ULID absent from the view" in error:
            return "validation_locale_ulid_avant_binaire"
        return "validation_locale_schema_avant_binaire"
    if isinstance(actual, dict) and "refusal" in actual:
        return "refus_modele_accepte_localement"
    return "termine" if passed else "comparaison_semantique_attendu_obtenu"


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


def evaluate(
    planner: Planner,
    timeline: dict[str, Any],
    cases: tuple[EvalCase, ...],
    run: int,
    opener: TraceOpener,
    records: list[dict[str, Any]],
    trace_path: Path,
    trace_document: dict[str, Any],
) -> tuple[int, int]:
    successes = 0
    print(f"\nBackend: {planner.backend_name}, run {run}")
    for index, case in enumerate(cases, start=1):
        opener.reset()
        error: str | None = None
        try:
            plan: Plan = planner.plan(timeline, case.instruction)
            passed = plan.operation is not None and operations_equal(
                plan.operation, case.expected)
            actual: Any = plan.operation if plan.operation is not None else {
                "refusal": plan.refusal}
        except PlannerError as exc:
            passed = False
            error = str(exc)
            actual = {"planner_error": str(exc)}
        raw_output = _raw_model_output(
            planner.backend_name, opener.response_text)
        family: str | None = None
        classification: str | None = None
        secondary: list[str] = []
        if not passed:
            family, classification, secondary = _classify_failure(
                raw_output, actual, case.expected, error, timeline)
        request = opener.request_payload or {}
        records.append({
            "backend": planner.backend_name,
            "run": run,
            "case": index if index <= len(CASES) else "reference",
            "instruction": case.instruction,
            "expected_operation": case.expected,
            "raw_model_output": raw_output,
            "final_operation_or_refusal": actual,
            "passed": passed,
            "breakpoint": _breakpoint(error, passed, actual),
            "error": error,
            "failure_family": family,
            "failure_detail": classification,
            "secondary_causes": secondary,
            "http_evidence": {
                "url": opener.request_url,
                "format_present": "format" in request,
                "format": request.get("format"),
                "input_schema": (
                    request.get("tools", [{}])[0].get("input_schema")
                    if isinstance(request.get("tools"), list) and
                    request.get("tools") else None
                ),
            },
            "raw_http_response": opener.response_text,
        })
        trace_path.write_text(
            json.dumps(trace_document, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )
        successes += int(passed)
        print(f"{index:02d} {'PASS' if passed else 'FAIL'} — {case.instruction}")
        if not passed:
            print("   attendu:", json.dumps(case.expected, ensure_ascii=False))
            print("   obtenu  :", json.dumps(actual, ensure_ascii=False))
    rate = 100.0 * successes / len(cases)
    print(f"Résultat {planner.backend_name} run {run}: "
          f"{successes}/{len(cases)} ({rate:.1f} %)")
    return successes, len(cases)


def _direct_apply_control(
    binary: CutmachineBinary, document: Path
) -> list[dict[str, Any]]:
    results: list[dict[str, Any]] = []
    with tempfile.TemporaryDirectory() as directory:
        for index, case in enumerate(CASES, start=1):
            copy = Path(directory) / f"case-{index}.json"
            shutil.copyfile(document, copy)
            result = binary.apply_operation(copy, case.expected)
            results.append({
                "case": index,
                "accepted": result.ok,
                "error": result.error,
                "detail": result.detail,
            })
    return results


def _request_controls(records: list[dict[str, Any]]) -> dict[str, Any]:
    ollama = next((record for record in records
                   if record["backend"] == "ollama"), None)
    anthropic = next((record for record in records
                      if record["backend"] == "anthropic"), None)
    ollama_format = ollama["http_evidence"]["format"] if ollama else None
    anthropic_schema = (
        anthropic["http_evidence"]["input_schema"] if anthropic else None)
    return {
        "ollama_format_present_in_emitted_http_request": bool(
            ollama and ollama["http_evidence"]["format_present"]),
        "ollama_format_equals_shared_schema": ollama_format == PLANNER_RESPONSE_SCHEMA,
        "anthropic_input_schema_equals_shared_schema": (
            anthropic_schema == PLANNER_RESPONSE_SCHEMA),
        "emitted_backend_schemas_strictly_identical": (
            ollama_format is not None and ollama_format == anthropic_schema),
    }


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
    parser.add_argument("--ollama-runs", type=int, default=1)
    parser.add_argument(
        "--trace", type=Path,
        default=Path(__file__).resolve().parents[1] / "eval-trace.json")
    args = parser.parse_args()
    trace: dict[str, Any] = {
        "document": str(args.document.resolve()),
        "cases": len(CASES),
        "reference_case_added_separately": True,
        "records": [],
        "controls": {},
    }
    try:
        binary = CutmachineBinary(args.binary)
        timeline = binary.describe(args.document)
        compact_view = json.dumps(
            timeline, ensure_ascii=False, separators=(",", ":"))
        trace["controls"]["describe_view"] = {
            "utf8_bytes": len(compact_view.encode("utf-8")),
            "characters": len(compact_view),
        }
        trace["controls"]["direct_apply_expected_operations"] = (
            _direct_apply_control(binary, args.document))
        cases = CASES + (REFERENCE_CASE,)
        totals: list[tuple[int, int]] = []
        if args.backend in {"ollama", "all"}:
            for run in range(1, args.ollama_runs + 1):
                opener = TraceOpener()
                totals.append(evaluate(
                    OllamaPlanner(model=args.model, opener=opener), timeline,
                    cases, run, opener, trace["records"], args.trace, trace))
        if args.backend in {"anthropic", "all"}:
            opener = TraceOpener()
            totals.append(evaluate(
                AnthropicPlanner(model=args.model, opener=opener), timeline,
                cases, 1, opener, trace["records"], args.trace, trace))
        trace["controls"].update(_request_controls(trace["records"]))
        trace["controls"]["ulid_validation_order"] = (
            "planner local validation before any binary call; eval model path "
            "does not invoke --apply-op")
        args.trace.write_text(
            json.dumps(trace, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )
    except (BinaryError, PlannerError) as exc:
        print(f"Erreur d'évaluation : {exc}")
        return 1
    return 0 if all(success == total for success, total in totals) else 1


if __name__ == "__main__":
    raise SystemExit(main())
