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
import re
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


def count_srt_cues(srt_path: str) -> int:
    """Cues in an SRT, counted the way Resolve counts subtitle items."""
    with open(srt_path, encoding="utf-8-sig") as handle:
        blocks = [b for b in re.split(r"\r?\n\r?\n", handle.read().strip())
                  if b.strip()]
    return len(blocks)


def send_timeline(resolve: Any, timeline: dict[str, Any],
                  name: str | None = None,
                  srt_path: str | None = None) -> str:
    """Rebuilds a CUTMACHINE cut as a new Resolve timeline.

    `name` overrides the montage's own name. Resolve refuses a duplicate
    timeline name, so without an override a cut can only ever be sent once:
    every later revision of the same montage would be rejected. This is where
    the operator says where it lands, not what it contains.

    `srt_path` adds a subtitle track. It is appended *first*, while the
    timeline is still empty: AppendToTimeline puts a subtitle element after
    whatever the timeline already holds, not at the start of its own track, so
    subtitling a filled timeline lands every cue past the last clip and
    doubles the timeline's length without raising. `recordFrame` does not
    work around it -- it is ignored on a subtitle element -- so the order
    below is the only way. The clipInfo carries nothing but the item and the
    track: adding startFrame, endFrame, recordFrame or mediaType crashes the
    bridge.
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
    cues = 0
    if srt_path:
        cues = count_srt_cues(srt_path)
        if not created.AddTrack("subtitle"):
            raise ResolveBridgeError(
                "Resolve a refusé d'ajouter une piste de sous-titres.")
        # Resolve caches imported media by path, so a rewritten .srt comes
        # back as the previous version. The caller is responsible for handing
        # a fresh filename per revision; the cue count below is what catches
        # it when they do not.
        imported = media_pool.ImportMedia([srt_path]) or []
        if not imported:
            raise ResolveBridgeError(
                f"Resolve n'a pas importé le fichier de sous-titres : "
                f"{srt_path}")
        if not media_pool.AppendToTimeline(
                [{"mediaPoolItem": imported[0], "trackIndex": 1}]):
            raise ResolveBridgeError(
                "AppendToTimeline a refusé les sous-titres.")
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
    titres = "" if not srt_path else f", {cues} sous-titre(s)"
    return (f"{len(infos)} plan(s) → timeline « {name} » dans Resolve"
            f"{couches}{titres}")


FLATTEN_SCHEMA = "cutmachine.resolve-flatten.v1"


def integral_rate(text: str) -> int:
    """Resolve reports a frame rate as a float string; only integers survive.

    PHILOSOPHY.md principle 4 forbids a float from becoming a rate, so this
    never builds one: it accepts the string only when it denotes a whole
    number, and refuses everything else rather than rounding 23.976 into 24.
    The value is used to *check* agreement between the montage and the
    flattening map, never to construct a RationalTime.
    """
    try:
        value = float(text)
    except (TypeError, ValueError):
        raise ResolveBridgeError(f"cadence illisible : {text!r}")
    if value != int(value):
        raise ResolveBridgeError(
            f"cadence non entière ({text}) : le remappage vers les rushes "
            "sources n'est exact qu'à cadence entière.")
    return int(value)


def flatten_timeline(timeline: Any) -> dict[str, Any]:
    """Maps a Resolve timeline to the source frame behind every output frame.

    RESOLVE-2026-09 -- a flattened render is what CUTMACHINE can hear, and the
    original rushes are what the montage must come back on. That round trip
    needs one fact per output frame: which source file it came from, and at
    which frame in it. This produces exactly that, as contiguous segments.

    Bounds come from GetStart/GetEnd/GetDuration and the in-point from
    GetSourceStartFrame. GetSourceEndFrame is deliberately unused: measured
    across the six LISAAMOD268 interviews it disagrees with the item duration
    by one frame on some clips and two on others (C125: dur 216, span 215;
    C126: dur 138, span 136), while GetEnd-GetStart equals GetDuration on
    every single item. Deriving the source length from it would silently drop
    a frame or two per clip.

    Refuses rather than guesses: a second populated video track has no defined
    flattening, and a gap, a retime or an item with no file on disk each break
    the frame correspondence this exists to guarantee.
    """
    name = timeline.GetName()
    origin = int(timeline.GetStartFrame())
    rate = integral_rate(timeline.GetSetting("timelineFrameRate"))

    populated = []
    for track in range(1, int(timeline.GetTrackCount("video")) + 1):
        if timeline.GetItemListInTrack("video", track):
            populated.append(track)
    if not populated:
        raise ResolveBridgeError(f"« {name} » n'a aucun plan vidéo.")
    if len(populated) > 1:
        raise ResolveBridgeError(
            f"« {name} » a des plans sur {len(populated)} pistes vidéo. "
            "Un rendu à plat ne dit pas quelle couche est visible à chaque "
            "image : aplatis la timeline sur une seule piste, ou renonce au "
            "retour sur les rushes sources pour celle-ci.")

    segments: list[dict[str, Any]] = []
    cursor = origin
    for item in timeline.GetItemListInTrack("video", populated[0]):
        start, end = int(item.GetStart()), int(item.GetEnd())
        duration = int(item.GetDuration())
        label = item.GetName()
        if end - start != duration:
            raise ResolveBridgeError(
                f"« {name} » : le plan {label} est retimé "
                f"({end - start} images pour {duration} de source). "
                "Le remappage vers la source suppose une vitesse normale.")
        if start != cursor:
            raise ResolveBridgeError(
                f"« {name} » : trou de {start - cursor} image(s) avant "
                f"{label}. Un trou n'a aucun rush derrière lui.")
        media = item.GetMediaPoolItem()
        path = (media.GetClipProperty("File Path") or "").strip() if media \
            else ""
        if not path:
            raise ResolveBridgeError(
                f"« {name} » : le plan {label} n'a pas de fichier sur le "
                "disque (timeline imbriquée, clip composé ou générateur).")
        segments.append({
            "record_start": start - origin,
            "record_end": end - origin,
            "source_start": int(item.GetSourceStartFrame()),
            "path": path,
            "filename": os.path.basename(path),
        })
        cursor = end

    return {
        "name": name,
        "frame_rate": rate,
        "duration": cursor - origin,
        "segments": segments,
    }


def read_flattening(resolve: Any, prefix: str) -> dict[str, Any]:
    """Flattens every timeline whose name starts with `prefix`."""
    project = resolve.GetProjectManager().GetCurrentProject()
    if project is None:
        raise ResolveBridgeError("Aucun projet ouvert dans Resolve.")
    timelines = []
    for index in range(1, int(project.GetTimelineCount()) + 1):
        timeline = project.GetTimelineByIndex(index)
        if timeline.GetName().startswith(prefix):
            timelines.append(flatten_timeline(timeline))
    if not timelines:
        raise ResolveBridgeError(
            f"Aucune timeline ne commence par « {prefix} ».")
    timelines.sort(key=lambda entry: entry["name"])
    return {
        "schema": FLATTEN_SCHEMA,
        "project": project.GetName(),
        "timelines": timelines,
    }


def remap_to_sources(montage: dict[str, Any],
                     flattening: dict[str, Any]) -> dict[str, Any]:
    """Rewrites a cut made on a flattened render onto the original rushes.

    RESOLVE-2026-09 -- CUTMACHINE cannot decode BRAW, so the montage is built
    on a ProRes render of the whole interview. Its clips address that render's
    frames; Resolve must receive the rushes instead. A cut that straddles a
    join in the source timeline covers two rushes, so one montage clip can
    come back as several -- which is why this splits rather than translates.

    Both sides must agree on the cadence: the offsets carried here are frame
    counts, and a montage at another rate would move every cut.
    """
    if montage.get("schema") != TIMELINE_SCHEMA:
        raise ResolveBridgeError(
            f"schéma de montage inattendu : {montage.get('schema')}")
    rate = montage.get("frame_rate") or {}
    if int(rate.get("den", 0)) != 1 or \
            int(rate.get("num", 0)) != int(flattening["frame_rate"]):
        raise ResolveBridgeError(
            f"le montage est à {rate.get('num')}/{rate.get('den')} i/s et la "
            f"timeline source à {flattening['frame_rate']} : le remappage "
            "déplacerait les coupes.")

    segments = flattening["segments"]
    remapped: list[dict[str, Any]] = []
    for clip in montage.get("clips", []):
        start, end = int(clip["start_frame"]), int(clip["end_frame"])
        covered = 0
        for segment in segments:
            low = max(start, segment["record_start"])
            high = min(end, segment["record_end"])
            if low >= high:
                continue
            offset = low - segment["record_start"]
            source_in = segment["source_start"] + offset
            remapped.append({
                "path": segment["path"],
                "filename": segment["filename"],
                "start_frame": source_in,
                "end_frame": source_in + (high - low),
                "video_layer": int(clip.get("video_layer", 0)),
                # The montage's own position shifts by the same number of
                # frames as the piece taken from it, because both domains run
                # at the rate checked above.
                "record_frame": int(clip["record_frame"]) + (low - start),
                "with_audio": bool(clip.get("with_audio", False)),
            })
            covered += high - low
        if covered != end - start:
            raise ResolveBridgeError(
                f"un plan du montage ({start}-{end}) sort du rendu "
                f"cartographié ({covered} image(s) couverte(s) sur "
                f"{end - start}).")
    if not remapped:
        raise ResolveBridgeError("Aucun plan à remapper.")

    result = dict(montage)
    result["clips"] = remapped
    return result


RENDER_CODEC = ("mov", "ProRes422LT")
# RESOLVE-2026-09 -- the render exists to give CUTMACHINE something it can
# decode; the finished montage comes back on the BRAW, so the working master's
# resolution changes nothing about the cut. Half of the 2160x3840 timeline
# keeps the framing and a fraction of the encode time. The *rate* is what must
# not move: every frame number in the flattening map is a 25 i/s frame.
RENDER_WIDTH, RENDER_HEIGHT = 1080, 1920


def plan_render(flattened: dict[str, Any], target_dir: str) -> dict[str, Any]:
    """Render settings for one flattened timeline. Pure, so it can be tested.

    SelectAllFrames matters more than it looks: with MarkIn/MarkOut honoured
    instead, a stray in/out left in the timeline would render a fragment, and
    every frame number in the flattening map would point somewhere else.
    """
    return {
        "SelectAllFrames": True,
        "TargetDir": target_dir,
        "CustomName": flattened["name"],
        "UniqueFilenameStyle": 1,  # suffix, never overwrite silently
        "ExportVideo": True,
        "ExportAudio": True,
        "FormatWidth": RENDER_WIDTH,
        "FormatHeight": RENDER_HEIGHT,
    }


def run_renders(resolve: Any, flattening: dict[str, Any],
                target_dir: str, poll=None) -> list[dict[str, Any]]:
    """Queues one ProRes render per flattened timeline and runs only those.

    RESOLVE-2026-09 -- StartRendering() with no argument runs *every* queued
    job. The operator's own queue is none of our business, so this collects
    the ids it created and starts those alone; jobs already waiting stay
    waiting. DeleteAllRenderJobs is never called for the same reason.
    """
    project = resolve.GetProjectManager().GetCurrentProject()
    if project is None:
        raise ResolveBridgeError("Aucun projet ouvert dans Resolve.")
    os.makedirs(target_dir, exist_ok=True)

    if not project.SetCurrentRenderFormatAndCodec(*RENDER_CODEC):
        raise ResolveBridgeError(
            f"Resolve a refusé le format {RENDER_CODEC[0]}/{RENDER_CODEC[1]}.")

    queued = []
    for flattened in flattening["timelines"]:
        timeline = find_timeline(project, flattened["name"])
        if not project.SetCurrentTimeline(timeline):
            raise ResolveBridgeError(
                f"Resolve n'a pas pu activer « {flattened['name']} ».")
        if not project.SetRenderSettings(plan_render(flattened, target_dir)):
            raise ResolveBridgeError(
                f"Réglages de rendu refusés pour « {flattened['name']} ».")
        job = project.AddRenderJob()
        if not job:
            raise ResolveBridgeError(
                f"Resolve n'a pas créé de job pour « {flattened['name']} ».")
        queued.append({"job": job, "name": flattened["name"]})

    if not project.StartRendering([entry["job"] for entry in queued]):
        raise ResolveBridgeError("StartRendering a échoué.")
    if poll:
        poll(project, queued)
    return queued


def find_timeline(project: Any, name: str) -> Any:
    for index in range(1, int(project.GetTimelineCount()) + 1):
        timeline = project.GetTimelineByIndex(index)
        if timeline.GetName() == name:
            return timeline
    raise ResolveBridgeError(f"Timeline introuvable : « {name} ».")


def write_json(payload: dict[str, Any], destination: str,
               summary: str = "") -> int:
    text = json.dumps(payload, ensure_ascii=False, indent=1,
                      sort_keys=True) + "\n"
    if destination == "-":
        sys.stdout.write(text)
    else:
        with open(destination, "w", encoding="utf-8") as handle:
            handle.write(text)
        print(f"{summary} -> {destination}", file=sys.stderr)
    return 0


def wait_for_renders(project: Any, queued: list[dict[str, Any]]) -> None:
    """Blocks until every queued job leaves the running state.

    Reports per job rather than on IsRenderingInProgress alone: a job that
    fails stops the render without stopping the queue, and a silent partial
    success here would be ingested as if it were the whole interview.
    """
    import time
    pending = {entry["job"]: entry["name"] for entry in queued}
    done: dict[str, str] = {}
    while pending:
        time.sleep(5)
        for job, name in list(pending.items()):
            status = project.GetRenderJobStatus(job) or {}
            state = status.get("JobStatus", "")
            if state in ("Complete", "Failed", "Cancelled"):
                done[job] = state
                del pending[job]
                print(f"  {name} : {state}", file=sys.stderr)
    failed = [job for job, state in done.items() if state != "Complete"]
    if failed:
        raise ResolveBridgeError(
            f"{len(failed)} rendu(s) non terminé(s) : "
            + ", ".join(sorted(done[job] for job in failed)))


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
    parser.add_argument("--srt", metavar="SOUS-TITRES.srt",
                        help="Pose ce SRT sur une piste de sous-titres de la "
                             "timeline renvoyée par --send. Resolve met en "
                             "cache le média importé par son chemin : donne "
                             "un nom de fichier neuf à chaque révision, sur "
                             "un chemin durable.")
    parser.add_argument("--flatten", metavar="PRÉFIXE",
                        help="Cartographie les timelines dont le nom commence "
                             "par PRÉFIXE : pour chaque image de sortie, le "
                             "rush et l'image derrière elle.")
    parser.add_argument("--render", metavar="APLATISSEMENT.json",
                        help="Rend en ProRes chaque timeline cartographiée "
                             "par --flatten, et n'exécute que ses propres "
                             "jobs (la file existante n'est pas touchée).")
    parser.add_argument("--target-dir", metavar="DOSSIER",
                        help="Destination des rendus de --render.")
    parser.add_argument("--remap", metavar="MONTAGE.json",
                        help="Réécrit un montage fait sur le rendu à plat "
                             "pour qu'il pointe sur les rushes d'origine. "
                             "Demande --flatten-map et --timeline.")
    parser.add_argument("--flatten-map", metavar="APLATISSEMENT.json",
                        help="Cartographie produite par --flatten.")
    parser.add_argument("--timeline", metavar="NOM",
                        help="Timeline de la cartographie à utiliser pour "
                             "--remap.")
    arguments = parser.parse_args(argv)

    if arguments.flatten:
        try:
            flattening = read_flattening(connect(), arguments.flatten)
        except ResolveBridgeError as error:
            print(str(error), file=sys.stderr)
            return 1
        return write_json(flattening, arguments.output, summary=", ".join(
            f"{t['name'].split('_')[-1]}:{len(t['segments'])}"
            for t in flattening["timelines"]))

    if arguments.render:
        if not arguments.target_dir:
            print("--render demande --target-dir.", file=sys.stderr)
            return 1
        try:
            with open(arguments.render, encoding="utf-8") as handle:
                flattening = json.load(handle)
            queued = run_renders(connect(), flattening, arguments.target_dir,
                                 poll=wait_for_renders)
        except ResolveBridgeError as error:
            print(str(error), file=sys.stderr)
            return 1
        print(f"{len(queued)} rendu(s) terminé(s) dans "
              f"{arguments.target_dir}", file=sys.stderr)
        return 0

    if arguments.remap:
        if not arguments.flatten_map or not arguments.timeline:
            print("--remap demande --flatten-map et --timeline.",
                  file=sys.stderr)
            return 1
        try:
            with open(arguments.remap, encoding="utf-8") as handle:
                montage = json.load(handle)
            with open(arguments.flatten_map, encoding="utf-8") as handle:
                flattening = json.load(handle)
            wanted = [t for t in flattening["timelines"]
                      if t["name"] == arguments.timeline]
            if not wanted:
                raise ResolveBridgeError(
                    f"« {arguments.timeline} » absente de la cartographie.")
            remapped = remap_to_sources(montage, wanted[0])
        except ResolveBridgeError as error:
            print(str(error), file=sys.stderr)
            return 1
        return write_json(remapped, arguments.output,
                          summary=f"{len(remapped['clips'])} plan(s)")

    if arguments.send:
        try:
            with open(arguments.send, encoding="utf-8") as handle:
                timeline = json.load(handle)
            print(send_timeline(connect(), timeline, arguments.name,
                                arguments.srt),
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
