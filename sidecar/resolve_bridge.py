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
import unicodedata
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


TIMELINE_SCHEMA = "cutmachine.resolve-timeline.v2"


def normalized(path: str) -> str:
    """macOS hands the same path back in either Unicode normalization.

    CUTMACHINE resolves media paths through the filesystem, which yields NFD
    ("Anthropoce" + combining grave); Resolve reports what it stored, usually
    NFC. Comparing the raw strings silently fails on every accented folder
    name, so both sides are folded before matching.
    """
    return unicodedata.normalize("NFC", path)


def index_media_pool(root: Any) -> dict[str, Any]:
    """Maps every clip in the Media Pool by absolute path, then by filename."""
    by_path: dict[str, Any] = {}
    by_name: dict[str, Any] = {}

    def walk(folder: Any) -> None:
        for clip in folder.GetClipList() or []:
            path = (clip.GetClipProperty("File Path") or "").strip()
            if path:
                by_path.setdefault(normalized(path), clip)
                name = (clip.GetClipProperty("File Name") or "").strip()
                if name:
                    by_name.setdefault(normalized(name), clip)
        for child in folder.GetSubFolderList() or []:
            walk(child)

    walk(root)
    # The filename fallback only helps when it is unambiguous, which the
    # setdefault above does not guarantee; callers treat it as a hint.
    return {"by_path": by_path, "by_name": by_name}


def send_timeline(resolve: Any, timeline: dict[str, Any],
                  name: str | None = None) -> str:
    """Rebuilds a CUTMACHINE cut as a new Resolve timeline.

    `name` overrides the montage's own name. Resolve refuses a duplicate
    timeline name, so without an override a cut can only ever be sent once:
    every later revision of the same montage would be rejected. This is where
    the operator says where it lands, not what it contains.
    """
    if timeline.get("schema") != TIMELINE_SCHEMA:
        raise ResolveBridgeError(
            f"schéma inattendu : {timeline.get('schema')} "
            f"(attendu {TIMELINE_SCHEMA})")
    project = resolve.GetProjectManager().GetCurrentProject()
    if project is None:
        raise ResolveBridgeError("Aucun projet ouvert dans Resolve.")
    media_pool = project.GetMediaPool()
    index = index_media_pool(media_pool.GetRootFolder())

    pending = []
    missing = []
    layers = 1
    for clip in timeline.get("clips", []):
        item = index["by_path"].get(normalized(clip["path"]))
        if item is None:
            item = index["by_name"].get(normalized(clip.get("filename", "")))
        if item is None:
            missing.append(clip.get("filename") or clip["path"])
            continue
        layer = int(clip.get("video_layer", 0))
        layers = max(layers, layer + 1)
        info = {
            "mediaPoolItem": item,
            "startFrame": int(clip["start_frame"]),
            "endFrame": int(clip["end_frame"]),
            "trackIndex": layer + 1,
        }
        if not clip.get("with_audio", False):
            # mediaType 1 = image seule. Sans ça, Resolve rapporte le son du
            # plan de coupe et le pose sous l'interview qu'il recouvre.
            info["mediaType"] = 1
        pending.append((info, int(clip.get("record_frame", 0))))
    if missing:
        raise ResolveBridgeError(
            "Rushes absents du Media Pool : " + ", ".join(sorted(set(missing))))
    if not pending:
        raise ResolveBridgeError("Aucun plan à envoyer.")

    name = name or timeline.get("name") or "CUTMACHINE"
    created = media_pool.CreateEmptyTimeline(name)
    if not created:
        raise ResolveBridgeError(
            f"Resolve a refusé de créer la timeline « {name} » "
            "(un nom identique existe peut-être déjà).")
    # Une timeline neuve n'a qu'une piste vidéo : chaque couche de recouvrement
    # a besoin de la sienne avant qu'AppendToTimeline puisse l'adresser.
    while int(created.GetTrackCount("video")) < layers:
        if not created.AddTrack("video"):
            raise ResolveBridgeError(
                "Resolve a refusé d'ajouter une piste vidéo.")
    # Resolve compte les images depuis le timecode de départ de la séquence
    # (90000 pour 01:00:00:00 à 25 i/s) ; CUTMACHINE compte depuis zéro. Sans
    # ce décalage, les plans repartiraient bout à bout au lieu de se poser où
    # le montage les place.
    start = int(created.GetStartFrame())
    infos = []
    for info, record_frame in pending:
        info["recordFrame"] = start + record_frame
        infos.append(info)
    if not media_pool.AppendToTimeline(infos):
        raise ResolveBridgeError("AppendToTimeline a échoué.")
    couches = "" if layers == 1 else f", sur {layers} pistes vidéo"
    return f"{len(infos)} plan(s) → timeline « {name} » dans Resolve{couches}"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Exporte les chutiers du projet Resolve ouvert en "
                    "manifeste d'import CUTMACHINE.")
    parser.add_argument("--output", "-o", default="-",
                        help="Fichier de sortie ('-' pour la sortie standard)")
    parser.add_argument("--send", metavar="TIMELINE.json",
                        help="Renvoie dans Resolve une timeline exportée par "
                             "`cutmachine --export-resolve-timeline`")
    parser.add_argument("--name", metavar="NOM",
                        help="Nom de la timeline créée dans Resolve "
                             "(par défaut celui du montage). Resolve refuse "
                             "un nom déjà pris : sans ça, on ne peut pas "
                             "renvoyer une révision du même montage.")
    arguments = parser.parse_args(argv)

    if arguments.send:
        try:
            with open(arguments.send, encoding="utf-8") as handle:
                timeline = json.load(handle)
            print(send_timeline(connect(), timeline, arguments.name),
                  file=sys.stderr)
        except ResolveBridgeError as error:
            print(str(error), file=sys.stderr)
            return 1
        return 0

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
