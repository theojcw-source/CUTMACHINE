"""DaVinci Resolve Media Pool -> CUTMACHINE import manifest.

External scripting (a process driving a running Resolve from outside) is a
Studio-only capability, so this module is the one part of the pipeline that
cannot run in CI: it is split into a pure `collect()` over duck-typed folder
objects, unit-tested with fakes, and a thin `connect()` that loads Blackmagic's
module. Nothing here decides anything about media: the manifest carries
identity and organisation only -- path, name, bin -- because FFmpeg remains the
single source of truth for rate and duration on the CUTMACHINE side. Resolve
reports FPS as a float, and a float must never reach a RationalTime.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from typing import Any


SCHEMA = "cutmachine.resolve-manifest.v1"

MACOS_SCRIPT_API = (
    "/Library/Application Support/Blackmagic Design/DaVinci Resolve"
    "/Developer/Scripting"
)
MACOS_SCRIPT_LIB = (
    "/Applications/DaVinci Resolve/DaVinci Resolve.app/Contents/Libraries"
    "/Fusion/fusionscript.so"
)


class ResolveBridgeError(RuntimeError):
    """Raised with a message meant to be shown to the user, in French."""


def connect(api_dir: str | None = None, lib_path: str | None = None) -> Any:
    """Returns the Resolve application object, or raises ResolveBridgeError."""
    api = api_dir or os.environ.get("RESOLVE_SCRIPT_API", MACOS_SCRIPT_API)
    lib = lib_path or os.environ.get("RESOLVE_SCRIPT_LIB", MACOS_SCRIPT_LIB)
    if not os.path.isdir(api):
        raise ResolveBridgeError(
            f"API de scripting Resolve introuvable : {api}. "
            "DaVinci Resolve Studio doit être installé.")
    os.environ["RESOLVE_SCRIPT_API"] = api
    os.environ["RESOLVE_SCRIPT_LIB"] = lib
    modules = os.path.join(api, "Modules")
    if modules not in sys.path:
        sys.path.append(modules)
    try:
        import DaVinciResolveScript as bmd  # type: ignore
    except ImportError as error:  # pragma: no cover - depends on the install
        raise ResolveBridgeError(
            f"Module DaVinciResolveScript illisible : {error}") from error
    resolve = bmd.scriptapp("Resolve")
    if resolve is None:
        raise ResolveBridgeError(
            "Aucune instance de Resolve ne répond. Lance DaVinci Resolve "
            "Studio, ouvre le projet, puis relance la commande. Le scripting "
            "externe est réservé à la version Studio.")
    return resolve


def collect(root: Any) -> dict[str, list[dict[str, Any]]]:
    """Walks a Media Pool folder tree into flat bin and clip lists.

    Bin keys are opaque intra-manifest references ("b0", "b1", ...) rather than
    name paths: Resolve allows '/' inside a bin name and allows two siblings to
    share a name, so a path key would need escaping and would still collide.
    The importer matches on (name, parent) anyway to stay idempotent.

    The root folder maps to the project root, which is the empty parent key --
    CUTMACHINE has no bin object for it. Media Pool entries with no file path
    (timelines, compound clips, generators) are reported as skipped rather than
    silently dropped.
    """
    bins: list[dict[str, Any]] = []
    clips: list[dict[str, Any]] = []
    skipped: list[dict[str, Any]] = []
    counter = 0

    def walk(folder: Any, key: str) -> None:
        nonlocal counter
        for clip in folder.GetClipList() or []:
            properties = clip.GetClipProperty() or {}
            path = (properties.get("File Path") or "").strip()
            name = (properties.get("Clip Name")
                    or properties.get("File Name") or "").strip()
            if not path:
                skipped.append({
                    "name": name,
                    "bin_key": key,
                    "reason": properties.get("Type") or "sans fichier",
                })
                continue
            entry = {"path": path, "name": name, "bin_key": key}
            unique_id = getattr(clip, "GetUniqueId", None)
            if callable(unique_id):
                identifier = unique_id()
                if identifier:
                    entry["resolve_uid"] = identifier
            clips.append(entry)
        for child in folder.GetSubFolderList() or []:
            counter += 1
            child_key = f"b{counter}"
            bins.append({
                "key": child_key,
                "name": child.GetName(),
                "parent_key": key,
            })
            walk(child, child_key)

    walk(root, "")
    return {"bins": bins, "clips": clips, "skipped": skipped}


def build_manifest(resolve: Any) -> dict[str, Any]:
    project = resolve.GetProjectManager().GetCurrentProject()
    if project is None:
        raise ResolveBridgeError("Aucun projet ouvert dans Resolve.")
    media_pool = project.GetMediaPool()
    manifest: dict[str, Any] = {
        "schema": SCHEMA,
        "project": project.GetName(),
        "resolve_version": resolve.GetVersionString(),
    }
    manifest.update(collect(media_pool.GetRootFolder()))
    return manifest


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Exporte les chutiers du projet Resolve ouvert en "
                    "manifeste d'import CUTMACHINE.")
    parser.add_argument("--output", "-o", default="-",
                        help="Fichier de sortie ('-' pour la sortie standard)")
    arguments = parser.parse_args(argv)
    try:
        manifest = build_manifest(connect())
    except ResolveBridgeError as error:
        print(str(error), file=sys.stderr)
        return 1
    text = json.dumps(manifest, ensure_ascii=False, indent=1,
                      sort_keys=True) + "\n"
    if arguments.output == "-":
        sys.stdout.write(text)
    else:
        with open(arguments.output, "w", encoding="utf-8") as handle:
            handle.write(text)
        print(f"{len(manifest['clips'])} rush(es), "
              f"{len(manifest['bins'])} chutier(s) -> {arguments.output}",
              file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
