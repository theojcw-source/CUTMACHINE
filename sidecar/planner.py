"""Backend-independent planning interface and HTTP implementations."""

from __future__ import annotations

import json
import os
import urllib.error
import urllib.request
from abc import ABC, abstractmethod
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable

from .schema import PLANNER_RESPONSE_SCHEMA


SYSTEM_PROMPT = """Tu pilotes CUTMACHINE, un éditeur vidéo déterministe.
Tu dois soumettre exactement une réponse structurée : une seule opération, ou un
refus motivé si la demande est impossible avec InsertClip, RemoveClip et TrimClip.
La réponse contient toujours les champs operation et refusal : celui qui n'est
pas utilisé vaut null.
Le schéma commun aux deux backends exige dans operation l'union des champs des
trois variantes. Choisis d'abord type et renseigne correctement ses champs ; les
champs sans rapport avec ce type sont des placeholders et seront ignorés.

Règles impératives :
- Les aliases (A1, A2...) servent uniquement à comprendre la demande.
- Dans l'opération, adresse toujours clips, pistes et sources par leur ULID complet.
- N'invente jamais un ULID absent de la vue de timeline.
- Produis une seule opération par tour, jamais une suite d'opérations.
- N'approxime pas une demande qui exige une autre capacité : refuse-la.
- Les pistes sont affichées par index à partir de zéro : « piste 1 » désigne
  index 0 et les aliases A*, « piste 2 » désigne index 1 et les aliases B*.
- Les secondes de la vue sont uniquement indicatives et ne servent jamais au
  calcul. Un temps {value, rate} vaut exactement value/rate seconde. Pour une
  cadence N/D, une frame vaut D ticks au rate N (exemple 30000/1001 : une frame
  est {value: 1001, rate: 30000}). À 25/1, 15 images s'écrivent donc
  {value: 15, rate: 25}, sans multiplication supplémentaire.
- TrimClip Tail : raccourcir donne un delta négatif, prolonger un delta positif.
  TrimClip Head : enlever le début donne un delta positif, récupérer des images
  antérieures donne un delta négatif.
- Pour InsertClip, laisse clip_id vide et exact_timeline vide.
- Pour RemoveClip, laisse exact_timeline vide.
- Pour TrimClip, laisse exact_clip à null.
"""


def _load_project_env() -> None:
    """Load the repository .env without replacing exported environment values."""
    path = Path(__file__).resolve().parents[1] / ".env"
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except FileNotFoundError:
        return
    for raw_line in lines:
        line = raw_line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        if line.startswith("export "):
            line = line[7:].lstrip()
        key, value = line.split("=", 1)
        key = key.strip()
        value = value.strip()
        if len(value) >= 2 and value[0] == value[-1] and value[0] in "'\"":
            value = value[1:-1]
        if key:
            os.environ.setdefault(key, value)


class PlannerError(RuntimeError):
    """A backend, transport, or structured-response failure."""


@dataclass(frozen=True)
class Plan:
    operation: dict[str, Any] | None = None
    refusal: str | None = None

    def __post_init__(self) -> None:
        if (self.operation is None) == (self.refusal is None):
            raise ValueError("a plan must contain exactly one operation or refusal")


class Planner(ABC):
    @property
    @abstractmethod
    def backend_name(self) -> str:
        """Stable name used by the REPL and evaluation report."""

    @abstractmethod
    def plan(
        self,
        timeline: dict[str, Any],
        instruction: str,
        previous_error: dict[str, Any] | None = None,
    ) -> Plan:
        """Return one operation or a motivated refusal."""


def _prompt(
    timeline: dict[str, Any],
    instruction: str,
    previous_error: dict[str, Any] | None,
) -> str:
    parts = [
        "Vue de timeline JSON :",
        json.dumps(timeline, ensure_ascii=False, separators=(",", ":")),
        "Instruction utilisateur :",
        instruction,
    ]
    if previous_error is not None:
        parts.extend(
            [
                "La première opération a été refusée par CUTMACHINE. Corrige-la "
                "une seule fois en tenant compte de cette erreur nommée :",
                json.dumps(previous_error, ensure_ascii=False, separators=(",", ":")),
            ]
        )
    return "\n".join(parts)


def _post_json(
    url: str,
    payload: dict[str, Any],
    headers: dict[str, str],
    timeout: float,
    opener: Callable[..., Any],
) -> dict[str, Any]:
    request = urllib.request.Request(
        url,
        data=json.dumps(payload, ensure_ascii=False).encode("utf-8"),
        headers={"Content-Type": "application/json", **headers},
        method="POST",
    )
    try:
        with opener(request, timeout=timeout) as response:
            body = response.read().decode("utf-8")
    except urllib.error.HTTPError as exc:
        detail = exc.read().decode("utf-8", errors="replace")
        raise PlannerError(f"HTTP {exc.code} from {url}: {detail}") from exc
    except (urllib.error.URLError, TimeoutError, OSError) as exc:
        raise PlannerError(f"unable to reach {url}: {exc}") from exc
    try:
        parsed = json.loads(body)
    except json.JSONDecodeError as exc:
        raise PlannerError(f"backend returned malformed JSON: {exc}") from exc
    if not isinstance(parsed, dict):
        raise PlannerError("backend response must be a JSON object")
    return parsed


def _parse_plan(value: Any, timeline: dict[str, Any]) -> Plan:
    if not isinstance(value, dict):
        raise PlannerError("structured planner response must be an object")
    # Some older Ollama grammar implementations omit a required nullable field.
    # Accept that equivalent representation after still passing the full schema
    # in `format`; local validation remains authoritative.
    if set(value) == {"operation"}:
        value = {"operation": value["operation"], "refusal": None}
    elif set(value) == {"refusal"}:
        value = {"operation": None, "refusal": value["refusal"]}
    elif set(value) != {"operation", "refusal"}:
        raise PlannerError("response must contain operation and refusal")
    if value["operation"] is None:
        refusal = value["refusal"]
        if not isinstance(refusal, dict) or set(refusal) != {"reason"}:
            raise PlannerError("invalid structured refusal")
        reason = refusal["reason"]
        if not isinstance(reason, str) or not reason.strip():
            raise PlannerError("refusal reason must be non-empty")
        return Plan(refusal=reason.strip())
    if not isinstance(value["operation"], dict):
        raise PlannerError("operation must be an object or null")
    operation = value["operation"]
    return Plan(operation=_normalize_operation(operation, timeline))


def _validate_time(value: Any, field: str, *, positive: bool = False,
                   nonnegative: bool = False) -> None:
    if not isinstance(value, dict) or set(value) != {"value", "rate"}:
        raise PlannerError(f"{field} must contain integer value and rate")
    if type(value["value"]) is not int or type(value["rate"]) is not int:
        raise PlannerError(f"{field} value and rate must be integers")
    if value["rate"] <= 0:
        raise PlannerError(f"{field}.rate must be positive")
    if positive and value["value"] <= 0:
        raise PlannerError(f"{field}.value must be positive")
    if nonnegative and value["value"] < 0:
        raise PlannerError(f"{field}.value must be non-negative")


def _known_ids(timeline: dict[str, Any]) -> tuple[set[str], set[str], set[str]]:
    source_ids = {
        source["id"] for source in timeline.get("sources", [])
        if isinstance(source, dict) and isinstance(source.get("id"), str)
    }
    track_ids: set[str] = set()
    clip_ids: set[str] = set()
    for track in timeline.get("tracks", []):
        if not isinstance(track, dict):
            continue
        if isinstance(track.get("id"), str):
            track_ids.add(track["id"])
        for item in track.get("items", []):
            if (isinstance(item, dict) and item.get("type") == "clip" and
                    isinstance(item.get("id"), str)):
                clip_ids.add(item["id"])
    return source_ids, track_ids, clip_ids


def _normalize_operation(
    operation: dict[str, Any], timeline: dict[str, Any]
) -> dict[str, Any]:
    source_ids, track_ids, clip_ids = _known_ids(timeline)
    operation_type = operation.get("type")
    if operation_type == "InsertClip":
        required = {"track_id", "source_id", "source_in", "duration",
                    "timeline_in"}
        if not required.issubset(operation):
            raise PlannerError("InsertClip is missing required fields")
        if operation["track_id"] not in track_ids:
            raise PlannerError("InsertClip references a track ULID absent from the view")
        if operation["source_id"] not in source_ids:
            raise PlannerError("InsertClip references a source ULID absent from the view")
        _validate_time(operation["source_in"], "source_in", nonnegative=True)
        _validate_time(operation["duration"], "duration", positive=True)
        _validate_time(operation["timeline_in"], "timeline_in", nonnegative=True)
        return {
            "type": "InsertClip",
            "track_id": operation["track_id"],
            "source_id": operation["source_id"],
            "source_in": operation["source_in"],
            "duration": operation["duration"],
            "timeline_in": operation["timeline_in"],
            "clip_id": "",
            "exact_timeline": [],
        }
    elif operation_type == "RemoveClip":
        if "clip_id" not in operation:
            raise PlannerError("RemoveClip is missing clip_id")
        if operation["clip_id"] not in clip_ids:
            raise PlannerError("RemoveClip references a clip ULID absent from the view")
        return {
            "type": "RemoveClip",
            "clip_id": operation["clip_id"],
            "exact_timeline": [],
        }
    elif operation_type == "TrimClip":
        if not {"clip_id", "edge", "delta"}.issubset(operation):
            raise PlannerError("TrimClip is missing required fields")
        if operation["clip_id"] not in clip_ids:
            raise PlannerError("TrimClip references a clip ULID absent from the view")
        if operation["edge"] not in {"Head", "Tail"}:
            raise PlannerError("TrimClip edge must be Head or Tail")
        _validate_time(operation["delta"], "delta")
        return {
            "type": "TrimClip",
            "clip_id": operation["clip_id"],
            "edge": operation["edge"],
            "delta": operation["delta"],
            "exact_clip": None,
        }
    else:
        raise PlannerError("unknown CUTMACHINE operation type")


class OllamaPlanner(Planner):
    def __init__(
        self,
        model: str | None = None,
        base_url: str | None = None,
        timeout: float = 120.0,
        opener: Callable[..., Any] = urllib.request.urlopen,
    ) -> None:
        _load_project_env()
        self.model = (model or os.environ.get("CUTMACHINE_OLLAMA_MODEL") or
                      os.environ.get("CUTMACHINE_MODEL", "qwen3:8b"))
        self.base_url = (base_url or os.environ.get(
            "CUTMACHINE_OLLAMA_URL", "http://localhost:11434")).rstrip("/")
        self.timeout = timeout
        self._opener = opener

    @property
    def backend_name(self) -> str:
        return "ollama"

    def plan(self, timeline: dict[str, Any], instruction: str,
             previous_error: dict[str, Any] | None = None) -> Plan:
        payload = {
            "model": self.model,
            "stream": False,
            "format": PLANNER_RESPONSE_SCHEMA,
            "options": {"temperature": 0},
            "messages": [
                {"role": "system", "content": SYSTEM_PROMPT},
                {"role": "user", "content": _prompt(
                    timeline, instruction, previous_error)},
            ],
        }
        response = _post_json(f"{self.base_url}/api/chat", payload, {},
                              self.timeout, self._opener)
        try:
            content = response["message"]["content"]
            value = json.loads(content)
        except (KeyError, TypeError, json.JSONDecodeError) as exc:
            raise PlannerError("Ollama returned no valid structured content") from exc
        return _parse_plan(value, timeline)


class AnthropicPlanner(Planner):
    TOOL_NAME = "submit_cutmachine_plan"

    def __init__(
        self,
        model: str | None = None,
        api_key: str | None = None,
        base_url: str | None = None,
        timeout: float = 120.0,
        opener: Callable[..., Any] = urllib.request.urlopen,
    ) -> None:
        _load_project_env()
        self.model = (model or os.environ.get("CUTMACHINE_ANTHROPIC_MODEL") or
                      os.environ.get("CUTMACHINE_MODEL", "claude-sonnet-4-5"))
        self.api_key = api_key or os.environ.get("ANTHROPIC_API_KEY", "")
        if not self.api_key:
            raise PlannerError("ANTHROPIC_API_KEY is required")
        self.base_url = (base_url or os.environ.get(
            "CUTMACHINE_ANTHROPIC_URL", "https://api.anthropic.com")).rstrip("/")
        self.timeout = timeout
        self._opener = opener

    @property
    def backend_name(self) -> str:
        return "anthropic"

    def plan(self, timeline: dict[str, Any], instruction: str,
             previous_error: dict[str, Any] | None = None) -> Plan:
        payload = {
            "model": self.model,
            "max_tokens": 1024,
            "system": SYSTEM_PROMPT,
            "messages": [{"role": "user", "content": _prompt(
                timeline, instruction, previous_error)}],
            "tools": [{
                "name": self.TOOL_NAME,
                "description": (
                    "Soumet exactement une opération CUTMACHINE exécutable ou "
                    "un refus motivé lorsque la demande est impossible."),
                "input_schema": PLANNER_RESPONSE_SCHEMA,
                "strict": True,
            }],
            "tool_choice": {"type": "tool", "name": self.TOOL_NAME},
        }
        response = _post_json(
            f"{self.base_url}/v1/messages", payload,
            {"x-api-key": self.api_key, "anthropic-version": "2023-06-01"},
            self.timeout, self._opener,
        )
        blocks = response.get("content", [])
        for block in blocks if isinstance(blocks, list) else []:
            if (isinstance(block, dict) and block.get("type") == "tool_use" and
                    block.get("name") == self.TOOL_NAME):
                return _parse_plan(block.get("input"), timeline)
        raise PlannerError("Anthropic returned no submit_cutmachine_plan tool use")


def planner_from_environment() -> Planner:
    backend = os.environ.get("CUTMACHINE_BACKEND", "ollama").strip().lower()
    if backend == "ollama":
        return OllamaPlanner()
    if backend == "anthropic":
        return AnthropicPlanner()
    raise PlannerError(
        "CUTMACHINE_BACKEND must be 'ollama' or 'anthropic', got " + repr(backend))
