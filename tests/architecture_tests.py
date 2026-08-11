#!/usr/bin/env python3
"""Executable architecture guard for PHILOSOPHY.md sections 2 and 3."""

from __future__ import annotations

import re
import sys
from pathlib import Path


failures: list[str] = []


def check(condition: bool, message: str) -> None:
    if not condition:
        failures.append(message)


def read(root: Path, relative: str) -> str:
    return (root / relative).read_text(encoding="utf-8")


def variant_members(header: str, alias: str) -> set[str]:
    match = re.search(
        rf"using\s+{re.escape(alias)}\s*=\s*std::variant<(.*?)>;",
        header,
        re.DOTALL,
    )
    if not match:
        failures.append(f"Operations.h no longer declares {alias}")
        return set()
    return set(re.findall(r"\b[A-Z][A-Za-z0-9_]*Operation\b", match.group(1)))


def between(text: str, start: str, end: str | None) -> str:
    begin = text.find(start)
    if begin < 0:
        failures.append(f"missing architecture marker: {start}")
        return ""
    if end is None:
        return text[begin:]
    finish = text.find(end, begin + len(start))
    if finish < 0:
        failures.append(f"missing architecture marker: {end}")
        return ""
    return text[begin:finish]


def class_public_methods(header: str, class_name: str) -> list[tuple[str, bool, bool]]:
    match = re.search(
        rf"class\s+{re.escape(class_name)}\s*\{{\s*public:(.*?)\n\}};",
        header,
        re.DOTALL,
    )
    if not match:
        failures.append(f"unable to inspect public API of {class_name}")
        return []
    body = re.sub(r"//.*", "", match.group(1))
    methods: list[tuple[str, bool, bool]] = []
    for declaration in body.split(";"):
        compact = " ".join(declaration.split())
        method = re.search(r"(?:^|\s)(~?[A-Za-z_]\w*)\s*\([^()]*\)\s*(const)?$", compact)
        if not method:
            continue
        methods.append(
            (method.group(1), "static " in compact, method.group(2) == "const")
        )
    return methods


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: architecture_tests.py SOURCE_ROOT", file=sys.stderr)
        return 2
    root = Path(sys.argv[1]).resolve()
    operations_h = read(root, "src/Operations.h")
    operations_cc = read(root, "src/Operations.cc")
    document_h = read(root, "src/Document.h")
    project_h = read(root, "src/Project.h")
    cli_h = read(root, "src/Cli.h")
    cli_cc = read(root, "src/Cli.cc")
    gui = read(root, "src/main.mm")

    timeline_operations = variant_members(operations_h, "Operation")
    project_operations = variant_members(operations_h, "ProjectOperation")
    all_operations = timeline_operations | project_operations
    check(bool(timeline_operations), "timeline Operation variant is empty")
    check(bool(project_operations), "ProjectOperation variant is empty")

    timeline_serializer = between(
        operations_cc, "std::string SerializeOperation(", "bool DeserializeOperation("
    )
    timeline_parser = between(
        operations_cc, "bool DeserializeOperation(", "std::string HexEncode("
    )
    project_serializer = between(
        operations_cc,
        "std::string SerializeProjectOperation(",
        "bool DeserializeProjectOperation(",
    )
    project_parser = between(
        operations_cc, "bool DeserializeProjectOperation(", None
    )
    for operation in sorted(timeline_operations):
        check(
            operation in timeline_serializer,
            f"{operation} is registered but missing from SerializeOperation",
        )
        check(
            operation in timeline_parser,
            f"{operation} is registered but missing from DeserializeOperation",
        )
    for operation in sorted(project_operations):
        check(
            operation in project_serializer,
            f"{operation} is registered but missing from SerializeProjectOperation",
        )
        check(
            operation in project_parser,
            f"{operation} is registered but missing from DeserializeProjectOperation",
        )

    # Public non-const lookup methods are used by the operation engine. CommitDocument
    # is the explicit boundary that reintegrates an already journaled timeline
    # snapshot. Every other new public mutator must name a registered counterpart.
    query_accessors = {
        "Document": {
            "FindSource",
            "FindLibraryMedia",
            "FindBin",
        "FindMarker",
        "FindTransition",
            "FindTrack",
            "FindClip",
            "FindTrackForClip",
        },
        "Project": {"ActiveTimeline", "FindTimeline"},
    }
    commit_boundaries = {"Project": {"CommitDocument", "CommitActiveDocument"}}
    public_mutators: dict[str, set[str]] = {"Document": set(), "Project": set()}
    mutator_name = re.compile(
        r"^(Add|Remove|Set|Update|Rename|Move|Activate|Relink|Delete|Clear|"
        r"Insert|Trim|Split|Join|Detach)(.*)$"
    )
    for class_name, header in (("Document", document_h), ("Project", project_h)):
        for method, is_static, is_const in class_public_methods(header, class_name):
            if (
                is_static
                or is_const
                or method in {class_name, f"~{class_name}"}
                or method in query_accessors[class_name]
                or method in commit_boundaries.get(class_name, set())
            ):
                continue
            public_mutators[class_name].add(method)
            candidates = {
                f"{method}Operation",
                f"{method.removeprefix(class_name)}Operation",
                f"{class_name}{method}Operation",
            }
            parts = mutator_name.match(method)
            if parts:
                candidates.add(
                    f"{parts.group(1)}{class_name}{parts.group(2)}Operation"
                )
            check(
                bool(candidates & all_operations),
                f"public mutator {class_name}::{method} has no registered Operation counterpart",
            )

    for method in sorted(public_mutators["Document"] | public_mutators["Project"]):
        check(
            re.search(rf"\.{re.escape(method)}\s*\(", gui) is None,
            f"GUI calls public mutator {method} directly instead of applying its Operation",
        )
    direct_project_writes = re.findall(
        r"self\.state->project\.([A-Za-z_]\w*)\s*(?:"
        r"(?:\+|-|\*|/)?=(?!=)|\.push_back\s*\(|\.erase\s*\(|\.clear\s*\()",
        gui,
    )
    check(
        not direct_project_writes,
        "GUI writes Project fields directly: "
        + ", ".join(sorted(set(direct_project_writes))),
    )

    gui_operation_tokens = set(
        re.findall(r"\b[A-Z][A-Za-z0-9_]*Operation\b", gui)
    )
    # Cocoa types and operation-building helper function names are not model
    # operation structs. Keeping this list narrow makes a new GUI-only model
    # operation fail until it joins one of the variants.
    gui_non_model_tokens = {
        "ApplyOperation",
        "GapDeleteOperation",
        "NSDragOperation",
        "ProjectOperation",
        "RedoProjectOperation",
        "TimelineCutOperation",
        "TimelineRangeDeleteOperation",
        "TimelineSourceEditOperation",
        "UndoProjectOperation",
    }
    unregistered_gui_operations = (
        gui_operation_tokens - all_operations - gui_non_model_tokens
    )
    check(
        not unregistered_gui_operations,
        "GUI references operation-like types outside the model variants: "
        + ", ".join(sorted(unregistered_gui_operations)),
    )

    gui_timeline_operations = gui_operation_tokens & timeline_operations
    gui_project_operations = gui_operation_tokens & project_operations
    if gui_timeline_operations:
        check(
            "ApplyOperationCommand" in cli_h
            and "int ApplyOperationCommand(" in cli_cc
            and "DeserializeOperation(" in cli_cc
            and '"--apply-op"' in gui,
            "GUI timeline operations have no generic headless CLI surface",
        )
    if gui_project_operations:
        check(
            "ApplyProjectOperationCommand" in cli_h
            and "int ApplyProjectOperationCommand(" in cli_cc
            and "DeserializeProjectOperation(" in cli_cc
            and '"--apply-project-op"' in gui,
            "GUI project operations have no generic headless CLI surface",
        )
    project_log_variables = set(
        re.findall(r"\bProjectEditLog\s+([A-Za-z_]\w*)", gui)
    )
    check(
        any(re.search(rf"\b{re.escape(name)}\.Apply\s*\(", gui)
            for name in project_log_variables),
        "GUI project mutations no longer visibly pass through ProjectEditLog::Apply",
    )

    if failures:
        for failure in failures:
            print(f"ARCHITECTURE FAILURE: {failure}", file=sys.stderr)
        return 1
    print(
        "architecture guard passed: "
        f"{len(timeline_operations)} timeline operations, "
        f"{len(project_operations)} project operations, "
        f"{len(gui_timeline_operations | gui_project_operations)} GUI operation types"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
