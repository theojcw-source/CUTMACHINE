"""Single source of truth for planner structured output."""

from __future__ import annotations

from typing import Any


def _time(*, minimum: int | None = None) -> dict[str, Any]:
    # `minimum` is documented here for the exact operation variants below but
    # is enforced locally: Anthropic strict tools reject numeric bounds.
    del minimum
    value: dict[str, Any] = {"type": "integer"}
    return {
        "type": "object",
        "additionalProperties": False,
        "properties": {
            "value": value,
            "rate": {"type": "integer"},
        },
        "required": ["value", "rate"],
    }


RATIONAL_TIME_SCHEMA = _time()
NONNEGATIVE_TIME_SCHEMA = _time(minimum=0)
POSITIVE_TIME_SCHEMA = _time(minimum=1)

POSITIVE_QUANTITY_SCHEMA: dict[str, Any] = {
    "type": "object",
    "additionalProperties": False,
    "properties": {
        "value": {"type": "integer"},
        "unit": {"enum": ["Frames", "Seconds"]},
    },
    "required": ["value", "unit"],
}

EXACT_TIMELINE_ITEM_SCHEMA: dict[str, Any] = {
    "type": "object",
    "additionalProperties": False,
    "properties": {
        "clip_id": {"type": "string"},
        "timeline_in": RATIONAL_TIME_SCHEMA,
    },
    "required": ["clip_id", "timeline_in"],
}

INSERT_CLIP_SCHEMA: dict[str, Any] = {
    "type": "object",
    "additionalProperties": False,
    "properties": {
        "type": {"const": "InsertClip"},
        "track_id": {"type": "string"},
        "source_id": {"type": "string"},
        "source_in": NONNEGATIVE_TIME_SCHEMA,
        "duration": POSITIVE_TIME_SCHEMA,
        "timeline_in": NONNEGATIVE_TIME_SCHEMA,
        # CUTMACHINE generates the clip ULID and enriches the exact ripple state.
        "clip_id": {"const": ""},
        "exact_timeline": {"type": "array"},
    },
    "required": [
        "type", "track_id", "source_id", "source_in", "duration",
        "timeline_in", "clip_id", "exact_timeline",
    ],
}

REMOVE_CLIP_SCHEMA: dict[str, Any] = {
    "type": "object",
    "additionalProperties": False,
    "properties": {
        "type": {"const": "RemoveClip"},
        "clip_id": {"type": "string"},
        "exact_timeline": {"type": "array"},
    },
    "required": ["type", "clip_id", "exact_timeline"],
}

TRIM_CLIP_SCHEMA: dict[str, Any] = {
    "type": "object",
    "additionalProperties": False,
    "properties": {
        "type": {"const": "TrimClip"},
        "clip_id": {"type": "string"},
        "edge": {"enum": ["Head", "Tail"]},
        "trim_action": {"enum": ["Shorten", "Extend"]},
        "trim_amount": POSITIVE_QUANTITY_SCHEMA,
        "exact_clip": {"type": "null"},
    },
    "required": [
        "type", "clip_id", "edge", "trim_action", "trim_amount",
        "exact_clip",
    ],
}

# Common subset accepted by Ollama structured decoding and Anthropic strict
# tools. Both reject different JSON Schema composition keywords, so the
# discriminator and union of possible fields are expressed here; the exact
# per-operation required fields above are enforced again by planner.py.
OPERATION_SCHEMA: dict[str, Any] = {
    "type": "object",
    "additionalProperties": False,
    "properties": {
        "type": {"enum": ["InsertClip", "RemoveClip", "TrimClip"]},
        "track_id": {"type": "string"},
        "source_id": {"type": "string"},
        "source_in": NONNEGATIVE_TIME_SCHEMA,
        "duration": POSITIVE_TIME_SCHEMA,
        "timeline_in": NONNEGATIVE_TIME_SCHEMA,
        "clip_id": {"type": "string"},
        "exact_timeline": {"type": "array"},
        "edge": {"enum": ["Head", "Tail"]},
        "trim_action": {"enum": ["Shorten", "Extend"]},
        "trim_amount": POSITIVE_QUANTITY_SCHEMA,
        "exact_clip": {"type": "null"},
    },
    # Requiring the union is deliberate: conditional `required` needs schema
    # composition, unsupported by Anthropic strict tools. The adapter discards
    # fields irrelevant to the selected discriminator.
    "required": [
        "type", "track_id", "source_id", "source_in", "duration",
        "timeline_in", "clip_id", "exact_timeline", "edge", "trim_action",
        "trim_amount", "exact_clip",
    ],
}

REFUSAL_SCHEMA: dict[str, Any] = {
    "type": "object",
    "additionalProperties": False,
    "properties": {
        "reason": {"type": "string"},
    },
    "required": ["reason"],
}

# Both backends receive this exact object: Ollama in `format`, Anthropic in the
# sole tool's `input_schema`.
PLANNER_RESPONSE_SCHEMA: dict[str, Any] = {
    "title": "CUTMACHINE planner response",
    "type": "object",
    "additionalProperties": False,
    "properties": {
        "operation": {
            "type": ["object", "null"],
            "additionalProperties": False,
            "properties": OPERATION_SCHEMA["properties"],
            "required": OPERATION_SCHEMA["required"],
        },
        "refusal": {
            "type": ["object", "null"],
            "additionalProperties": False,
            "properties": REFUSAL_SCHEMA["properties"],
            "required": ["reason"],
        },
    },
    "required": ["operation", "refusal"],
}
