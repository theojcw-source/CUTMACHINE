"""Backend-independent planning interface and HTTP implementations."""

from __future__ import annotations

import json
import os
import re
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
- Pour TrimClip, exprime l'intention sans signe interne : trim_action vaut
  Shorten pour raccourcir/enlever et Extend pour prolonger/récupérer ;
  trim_amount est une quantité strictement positive avec l'unité Frames ou
  Seconds. CUTMACHINE calculera le signe, le timebase et le delta canonique.
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
    resolved = _resolve_entities(timeline, instruction)
    parts = [
        "Vue de timeline JSON :",
        json.dumps(timeline, ensure_ascii=False, separators=(",", ":")),
        "Références déterministes résolues par CUTMACHINE :",
        json.dumps(resolved, ensure_ascii=False, separators=(",", ":")),
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


def _parse_plan(
    value: Any, timeline: dict[str, Any], instruction: str
) -> Plan:
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
    return Plan(operation=_normalize_operation(operation, timeline, instruction))


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


_ORDINALS = {
    "premier": 1, "première": 1,
    "deuxième": 2, "second": 2, "seconde": 2,
    "troisième": 3, "quatrième": 4, "cinquième": 5,
    "sixième": 6, "septième": 7, "huitième": 8,
    "neuvième": 9, "dixième": 10,
}


def _track_ordinal(text: str) -> int | None:
    numeric = re.search(r"\bpiste(?:\s+vid[eé]o)?\s+(\d+)\b", text)
    if numeric:
        return int(numeric.group(1))
    words = "|".join(_ORDINALS)
    named = re.search(
        rf"\b({words})\s+piste(?:\s+vid[eé]o)?\b", text)
    return _ORDINALS[named.group(1)] if named else None


def _clip_ordinal(text: str) -> int | None:
    words = "|".join(_ORDINALS)
    named = re.search(rf"\b({words})\s+(?:clip|plan)\b", text)
    if named:
        return _ORDINALS[named.group(1)]
    numeric = re.search(r"\b(?:clip|plan)\s+(\d+)\b", text)
    return int(numeric.group(1)) if numeric else None


def _resolve_entities(
    timeline: dict[str, Any], instruction: str
) -> dict[str, str]:
    text = instruction.casefold()
    tracks = [
        track for track in timeline.get("tracks", [])
        if isinstance(track, dict) and isinstance(track.get("id"), str)
    ]
    if re.search(r"\bpiste\s+vid[eé]o\b", text):
        tracks = [track for track in tracks if track.get("kind") == "video"]
    tracks.sort(key=lambda track: track.get("index", 0))
    resolution: dict[str, str] = {}

    track_number = _track_ordinal(text)
    selected_track = None
    if track_number is not None and 1 <= track_number <= len(tracks):
        selected_track = tracks[track_number - 1]
        resolution["track_id"] = selected_track["id"]

    clip_alias = _explicit_clip_alias(timeline, instruction)
    if clip_alias:
        resolution["clip_id"] = clip_alias
    else:
        ordinal = _clip_ordinal(text)
        candidates = selected_track.get("items", []) if selected_track else [
            item for track in tracks for item in track.get("items", [])
        ]
        clips = [
            item for item in candidates
            if isinstance(item, dict) and item.get("type") == "clip" and
            isinstance(item.get("id"), str)
        ]
        if ordinal is not None and 1 <= ordinal <= len(clips):
            resolution["clip_id"] = clips[ordinal - 1]["id"]
        elif "unique clip" in text and len(clips) == 1:
            resolution["clip_id"] = clips[0]["id"]

    matching_sources = [
        source["id"] for source in timeline.get("sources", [])
        if isinstance(source, dict) and isinstance(source.get("id"), str) and
        isinstance(source.get("file"), str) and
        source["file"].casefold() in text
    ]
    if len(matching_sources) == 1:
        resolution["source_id"] = matching_sources[0]
    return resolution


def _explicit_trim_intent(instruction: str) -> dict[str, Any] | None:
    text = instruction.casefold()
    amount = re.search(
        r"\b(\d+)\s+(?:premi[eè]res?\s+)?"
        r"(images?|frames?|secondes?)\b", text)
    if amount is None:
        return None
    if not re.search(
        r"\b(raccourc\w*|retir\w*|enl[eè]v\w*|coup\w*|"
        r"r[eé]cup[eè]r\w*|prolong\w*)\b", text
    ):
        return None
    extend = bool(re.search(r"\b(r[eé]cup[eè]r\w*|prolong\w*)\b", text))
    head = bool(re.search(
        r"\b(d[eé]but|premi[eè]res?|avant le d[eé]but)\b", text))
    return {
        "edge": "Head" if head else "Tail",
        "action": "Extend" if extend else "Shorten",
        "value": int(amount.group(1)),
        "unit": "Seconds" if amount.group(2).startswith("seconde") else "Frames",
    }


def _clip_rate(
    timeline: dict[str, Any], clip_id: str
) -> tuple[int, int] | None:
    source_id = None
    for track in timeline.get("tracks", []):
        for item in track.get("items", []) if isinstance(track, dict) else []:
            if isinstance(item, dict) and item.get("id") == clip_id:
                source_id = item.get("source_id")
                break
    for source in timeline.get("sources", []):
        if not isinstance(source, dict) or source.get("id") != source_id:
            continue
        match = re.fullmatch(r"(\d+)/(\d+)", str(source.get("frame_rate", "")))
        if match and int(match.group(1)) > 0 and int(match.group(2)) > 0:
            return int(match.group(1)), int(match.group(2))
    return None


def _timeline_rate(timeline: dict[str, Any]) -> tuple[int, int] | None:
    for source in timeline.get("sources", []):
        if not isinstance(source, dict):
            continue
        match = re.fullmatch(r"(\d+)/(\d+)", str(source.get("frame_rate", "")))
        if match and int(match.group(1)) > 0 and int(match.group(2)) > 0:
            return int(match.group(1)), int(match.group(2))
    return None


def _explicit_timeline_position(
    timeline: dict[str, Any], instruction: str,
    track: dict[str, Any] | None = None,
) -> dict[str, int] | None:
    text = instruction.casefold()
    frame = re.search(r"\b[àa] l['’]image\s+(\d+)\b", text)
    at_start = bool(re.search(
        r"\bau (?:tout )?d[eé]but (?:de )?(?:la )?piste\b", text))
    at_end = bool(re.search(r"\b[àa] la fin de la piste\b", text))
    if frame is None and not at_start and not at_end:
        return None
    rate = _timeline_rate(timeline)
    if rate is None:
        raise PlannerError("unable to determine the timeline presentation rate")
    numerator, denominator = rate
    frames = int(frame.group(1)) if frame else 0
    if at_end:
        if track is None:
            return None
        frames = max((
            int(item.get("timeline_in", {}).get("frames", 0)) +
            int(item.get("duration", {}).get("frames", 0))
            for item in track.get("items", [])
            if isinstance(item, dict) and item.get("type") == "clip"
        ), default=0)
    return {"value": frames * denominator, "rate": numerator}


def _explicit_clip_alias(
    timeline: dict[str, Any], instruction: str
) -> str | None:
    aliases = {
        item.get("alias", "").casefold(): item.get("id")
        for track in timeline.get("tracks", []) if isinstance(track, dict)
        for item in track.get("items", []) if isinstance(item, dict)
        if item.get("type") == "clip"
    }
    for token in re.findall(r"\b[a-z]+\d+\b", instruction.casefold()):
        if token in aliases and isinstance(aliases[token], str):
            return aliases[token]
    return None


def _trim_delta(
    amount: dict[str, Any], edge: str, action: str,
    timeline: dict[str, Any], clip_id: str,
) -> dict[str, int]:
    if (not isinstance(amount, dict) or
            set(amount) != {"value", "unit"} or
            type(amount["value"]) is not int or amount["value"] <= 0 or
            amount["unit"] not in {"Frames", "Seconds"}):
        raise PlannerError(
            "trim_amount must be a positive Frames or Seconds quantity")
    rate = _clip_rate(timeline, clip_id)
    if rate is None:
        raise PlannerError("unable to determine the trimmed clip media rate")
    numerator, denominator = rate
    ticks = amount["value"] * (
        denominator if amount["unit"] == "Frames" else numerator)
    shorten = action == "Shorten"
    positive = shorten if edge == "Head" else not shorten
    return {"value": ticks if positive else -ticks, "rate": numerator}


def _normalize_operation(
    operation: dict[str, Any], timeline: dict[str, Any], instruction: str = ""
) -> dict[str, Any]:
    source_ids, track_ids, clip_ids = _known_ids(timeline)
    resolved = _resolve_entities(timeline, instruction)
    operation_type = operation.get("type")
    explicit_trim = _explicit_trim_intent(instruction)
    if explicit_trim is not None:
        operation_type = "TrimClip"
    if operation_type == "InsertClip":
        required = {"track_id", "source_id", "source_in", "duration",
                    "timeline_in"}
        if not required.issubset(operation):
            raise PlannerError("InsertClip is missing required fields")
        track_id = resolved.get("track_id", operation["track_id"])
        source_id = resolved.get("source_id", operation["source_id"])
        if track_id not in track_ids:
            raise PlannerError("InsertClip references a track ULID absent from the view")
        if source_id not in source_ids:
            raise PlannerError("InsertClip references a source ULID absent from the view")
        _validate_time(operation["source_in"], "source_in", nonnegative=True)
        _validate_time(operation["duration"], "duration", positive=True)
        selected_track = next(
            (track for track in timeline.get("tracks", [])
             if isinstance(track, dict) and track.get("id") == track_id), None)
        timeline_in = (
            _explicit_timeline_position(timeline, instruction, selected_track)
            or operation["timeline_in"]
        )
        _validate_time(timeline_in, "timeline_in", nonnegative=True)
        return {
            "type": "InsertClip",
            "track_id": track_id,
            "source_id": source_id,
            "source_in": operation["source_in"],
            "duration": operation["duration"],
            "timeline_in": timeline_in,
            "clip_id": "",
            "exact_timeline": [],
        }
    elif operation_type == "RemoveClip":
        if "clip_id" not in operation:
            raise PlannerError("RemoveClip is missing clip_id")
        clip_id = resolved.get("clip_id", operation["clip_id"])
        if clip_id not in clip_ids:
            raise PlannerError("RemoveClip references a clip ULID absent from the view")
        return {
            "type": "RemoveClip",
            "clip_id": clip_id,
            "exact_timeline": [],
        }
    elif operation_type == "TrimClip":
        required = {"clip_id", "edge", "trim_action", "trim_amount"}
        if not required.issubset(operation):
            raise PlannerError("TrimClip is missing required fields")
        clip_id = resolved.get("clip_id", operation["clip_id"])
        if clip_id not in clip_ids:
            raise PlannerError("TrimClip references a clip ULID absent from the view")
        edge = explicit_trim["edge"] if explicit_trim else operation["edge"]
        action = (
            explicit_trim["action"] if explicit_trim
            else operation["trim_action"]
        )
        amount = (
            {"value": explicit_trim["value"], "unit": explicit_trim["unit"]}
            if explicit_trim else operation["trim_amount"]
        )
        if edge not in {"Head", "Tail"}:
            raise PlannerError("TrimClip edge must be Head or Tail")
        if action not in {"Shorten", "Extend"}:
            raise PlannerError("TrimClip trim_action must be Shorten or Extend")
        return {
            "type": "TrimClip",
            "clip_id": clip_id,
            "edge": edge,
            "delta": _trim_delta(
                amount, edge, action, timeline, clip_id),
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
        return _parse_plan(value, timeline, instruction)


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
                return _parse_plan(block.get("input"), timeline, instruction)
        raise PlannerError("Anthropic returned no submit_cutmachine_plan tool use")


def planner_from_environment() -> Planner:
    backend = os.environ.get("CUTMACHINE_BACKEND", "ollama").strip().lower()
    if backend == "ollama":
        return OllamaPlanner()
    if backend == "anthropic":
        return AnthropicPlanner()
    raise PlannerError(
        "CUTMACHINE_BACKEND must be 'ollama' or 'anthropic', got " + repr(backend))
