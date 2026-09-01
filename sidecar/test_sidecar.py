from __future__ import annotations

import io
import json
import tempfile
import unicodedata
import unittest
from pathlib import Path
from typing import Any

from sidecar.binary import ApplyResult, CutmachineBinary
from sidecar.eval import A1, CASES, operations_equal
from sidecar.planner import (
    AnthropicPlanner,
    OllamaPlanner,
    Plan,
    Planner,
    PlannerError,
)
from sidecar.repl import run_turn
from sidecar.resolve_bridge import (
    SCHEMA,
    TIMELINE_SCHEMA,
    ResolveBridgeError,
    collect,
    index_media_pool,
    normalized,
    send_timeline,
)
from sidecar.schema import PLANNER_RESPONSE_SCHEMA


ROOT = Path(__file__).resolve().parents[1]
FIXTURE = Path(__file__).with_name("eval-document.json")
BINARY = ROOT / "build" / "cutmachine"


def create_project_package(parent: Path, name: str = "Fixture") -> Path:
    document_text = FIXTURE.read_text(encoding="utf-8")
    document = json.loads(document_text)
    timeline_id = document["sequence"]["id"]
    package = parent / f"{name}.cutmachine-project"
    timelines = package / "Timelines"
    timelines.mkdir(parents=True)
    (timelines / f"{timeline_id}.json").write_text(
        document_text, encoding="utf-8")
    project = {
        "project_format": "cutmachine-project",
        "project_version": 2,
        "id": "01K60000000000000000000001",
        "name": name,
        "active_timeline_id": timeline_id,
        "timeline_snapshots": [document_text],
        "bin_metadata": [],
        "timeline_bin_ids": [],
    }
    project_path = package / "project.cutmachine.json"
    project_path.write_text(json.dumps(project), encoding="utf-8")
    manifest = {
        "format": "cutmachine-collection",
        "version": 2,
        "project": "project.cutmachine.json",
        "timelines": [{
            "id": timeline_id,
            "path": f"Timelines/{timeline_id}.json",
        }],
        "media": [],
    }
    (package / "manifest.json").write_text(
        json.dumps(manifest, separators=(",", ":")), encoding="utf-8")
    return project_path


class FakeResponse:
    def __init__(self, payload: dict[str, Any]) -> None:
        self.payload = payload

    def __enter__(self) -> "FakeResponse":
        return self

    def __exit__(self, *args: Any) -> None:
        return None

    def read(self) -> bytes:
        return json.dumps(self.payload).encode()


class RecordingOpener:
    def __init__(self, response: dict[str, Any]) -> None:
        self.response = response
        self.request: Any = None
        self.timeout: float | None = None

    def __call__(self, request: Any, timeout: float) -> FakeResponse:
        self.request = request
        self.timeout = timeout
        return FakeResponse(self.response)

    @property
    def body(self) -> dict[str, Any]:
        return json.loads(self.request.data)


def remove_plan() -> dict[str, Any]:
    return {
        "operation": {
            "type": "RemoveClip", "clip_id": A1, "exact_timeline": [],
        },
        "refusal": None,
    }


def trim_plan(
    edge: str, action: str, amount: int, unit: str = "Frames"
) -> dict[str, Any]:
    return {
        "operation": {
            "type": "TrimClip",
            "clip_id": A1,
            "edge": edge,
            "trim_action": action,
            "trim_amount": {"value": amount, "unit": unit},
            "exact_clip": None,
        },
        "refusal": None,
    }


def add_marker_plan() -> dict[str, Any]:
    return {
        "operation": {
            "type": "AddMarker",
            "marker_id": "",
            "marker_name": "Sélection interview",
            "marker_time": {"value": 1001, "rate": 30000},
            "marker_color": "#33AAFF",
            "marker_category": "Montage",
        },
        "refusal": None,
    }


class PlannerProtocolTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        if not BINARY.exists():
            raise unittest.SkipTest("build/cutmachine is not available")
        cls.fixture_directory = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.fixture_directory.cleanup)
        project = create_project_package(
            Path(cls.fixture_directory.name), "Planner")
        cls.timeline = CutmachineBinary(BINARY).describe(project)

    def test_ollama_receives_shared_schema_in_format(self) -> None:
        opener = RecordingOpener({
            "message": {"content": json.dumps(remove_plan())},
        })
        planner = OllamaPlanner(model="test-model", opener=opener)
        plan = planner.plan(self.timeline, "Supprime A1")
        self.assertEqual(plan.operation, remove_plan()["operation"])
        self.assertEqual(opener.body["format"], PLANNER_RESPONSE_SCHEMA)
        self.assertFalse(opener.body["stream"])
        self.assertEqual(opener.body["options"]["temperature"], 0)
        self.assertTrue(opener.request.full_url.endswith("/api/chat"))

    def test_describe_exposes_sequence_format_to_the_planner(self) -> None:
        sequence = self.timeline["sequence"]
        self.assertGreater(sequence["width"], 0)
        self.assertGreater(sequence["height"], 0)
        self.assertRegex(sequence["frame_rate"], r"^\d+/\d+$")

    def test_shared_schema_uses_no_unsupported_composition_keywords(self) -> None:
        encoded = json.dumps(PLANNER_RESPONSE_SCHEMA)
        self.assertNotIn('"oneOf"', encoded)
        self.assertNotIn('"anyOf"', encoded)
        self.assertNotIn('"allOf"', encoded)

    def test_anthropic_receives_same_schema_as_tool_input(self) -> None:
        opener = RecordingOpener({
            "content": [{
                "type": "tool_use",
                "name": AnthropicPlanner.TOOL_NAME,
                "input": remove_plan(),
            }],
        })
        planner = AnthropicPlanner(
            model="test-model", api_key="test-key", opener=opener)
        plan = planner.plan(self.timeline, "Supprime A1")
        self.assertEqual(plan.operation, remove_plan()["operation"])
        tool = opener.body["tools"][0]
        self.assertEqual(tool["input_schema"], PLANNER_RESPONSE_SCHEMA)
        self.assertTrue(tool["strict"])
        self.assertEqual(
            opener.body["tool_choice"],
            {"type": "tool", "name": AnthropicPlanner.TOOL_NAME})
        self.assertTrue(opener.request.full_url.endswith("/v1/messages"))

    def test_refusal_is_supported_by_both_response_shapes(self) -> None:
        refusal = {
            "operation": None,
            "refusal": {"reason": "Deux opérations seraient nécessaires."},
        }
        ollama = OllamaPlanner(opener=RecordingOpener({
            "message": {"content": json.dumps(refusal)}}))
        anthropic = AnthropicPlanner(api_key="x", opener=RecordingOpener({
            "content": [{"type": "tool_use", "name": AnthropicPlanner.TOOL_NAME,
                         "input": refusal}]}))
        self.assertEqual(
            ollama.plan(self.timeline, "demande composée").refusal,
            refusal["refusal"]["reason"])
        self.assertEqual(
            anthropic.plan(self.timeline, "demande composée").refusal,
            refusal["refusal"]["reason"])

    def test_unknown_model_ulid_is_rejected_before_binary(self) -> None:
        invalid = remove_plan()
        invalid["operation"]["clip_id"] = "01K49999999999999999999999"
        planner = OllamaPlanner(opener=RecordingOpener({
            "message": {"content": json.dumps(invalid)}}))
        with self.assertRaisesRegex(PlannerError, "absent from the view"):
            planner.plan(self.timeline, "Supprime un clip inexistant")

    def test_older_ollama_missing_nullable_field_is_tolerated(self) -> None:
        response = remove_plan()["operation"]
        planner = OllamaPlanner(opener=RecordingOpener({
            "message": {"content": json.dumps({"operation": response})}}))
        self.assertEqual(
            planner.plan(self.timeline, "Supprime A1").operation, response)

    def test_backend_union_fields_are_normalized_to_canonical_operation(self) -> None:
        operation = {
            "type": "RemoveClip",
            "clip_id": A1,
            "edge": "Head",
            "trim_action": "Extend",
            "trim_amount": {"value": 99, "unit": "Frames"},
            "track_id": "irrelevant",
        }
        planner = AnthropicPlanner(api_key="x", opener=RecordingOpener({
            "content": [{"type": "tool_use", "name": AnthropicPlanner.TOOL_NAME,
                         "input": {"operation": operation,
                                   "refusal": {"reason": "irrelevant"}}}]}))
        self.assertEqual(
            planner.plan(self.timeline, "Supprime A1").operation,
            remove_plan()["operation"],
        )

    def test_trim_intent_is_converted_to_internal_delta_sign(self) -> None:
        expectations = (
            ("Head", "Shorten", 10),
            ("Head", "Extend", -10),
            ("Tail", "Shorten", -10),
            ("Tail", "Extend", 10),
        )
        for edge, action, expected_delta in expectations:
            with self.subTest(edge=edge, action=action):
                planner = OllamaPlanner(opener=RecordingOpener({
                    "message": {"content": json.dumps(
                        trim_plan(edge, action, 10))},
                }))
                operation = planner.plan(self.timeline, "trim").operation
                self.assertIsNotNone(operation)
                self.assertEqual(
                    operation["delta"],
                    {"value": expected_delta, "rate": 25},
                )

    def test_marker_plan_is_normalized_to_canonical_operation(self) -> None:
        planner = OllamaPlanner(opener=RecordingOpener({
            "message": {"content": json.dumps(add_marker_plan())},
        }))
        operation = planner.plan(
            self.timeline, "Ajoute un marqueur pour la sélection interview"
        ).operation
        self.assertEqual(operation, {
            "type": "AddMarker",
            "marker": {
                "id": "",
                "name": "Sélection interview",
                "time": {"value": 1001, "rate": 30000},
                "color": "#33AAFF",
                "category": "Montage",
            },
            "insertion_index": -1,
        })

    def test_marker_alias_overrides_a_model_guessed_id(self) -> None:
        timeline = json.loads(json.dumps(self.timeline))
        marker_id = "01K81111111111111111111111"
        timeline["markers"] = [{
            "alias": "K1", "id": marker_id, "name": "Choix",
            "time": {"frames": 10, "seconds": 0.4},
            "color": "#33AAFF", "category": "Montage",
        }]
        response = {
            "operation": {
                "type": "RemoveMarker",
                "marker_id": "01K89999999999999999999999",
            },
            "refusal": None,
        }
        planner = OllamaPlanner(opener=RecordingOpener({
            "message": {"content": json.dumps(response)},
        }))
        self.assertEqual(
            planner.plan(timeline, "Supprime K1").operation,
            {"type": "RemoveMarker", "marker_id": marker_id},
        )

    def test_explicit_trim_language_controls_sign_timebase_and_type(self) -> None:
        response = trim_plan("Head", "Shorten", 99)
        response["operation"]["type"] = "InsertClip"
        planner = OllamaPlanner(opener=RecordingOpener({
            "message": {"content": json.dumps(response)},
        }))
        operation = planner.plan(
            self.timeline, "Récupère 2 secondes avant le début actuel de A1."
        ).operation
        self.assertEqual(operation, {
            "type": "TrimClip",
            "clip_id": A1,
            "edge": "Head",
            "delta": {"value": -50, "rate": 25},
            "exact_clip": None,
        })

        shorten = planner.plan(
            self.timeline, "Coupe les 10 premières images de A1."
        ).operation
        self.assertEqual(shorten["edge"], "Head")
        self.assertEqual(shorten["delta"], {"value": 10, "rate": 25})

    def test_explicit_timeline_frame_is_absolute(self) -> None:
        operation = {
            "type": "InsertClip",
            "track_id": self.timeline["tracks"][0]["id"],
            "source_id": self.timeline["sources"][1]["id"],
            "source_in": {"value": 50, "rate": 25},
            "duration": {"value": 10, "rate": 25},
            "timeline_in": {"value": 75, "rate": 25},
        }
        planner = OllamaPlanner(opener=RecordingOpener({
            "message": {"content": json.dumps({
                "operation": operation, "refusal": None})},
        }))
        normalized = planner.plan(
            self.timeline,
            "Dans le trou après A1, à l'image 50, place 10 images.",
        ).operation
        self.assertEqual(normalized["timeline_in"], {"value": 50, "rate": 25})

    def test_ordinal_entities_override_model_ulids(self) -> None:
        wrong_clip = remove_plan()
        wrong_clip["operation"]["clip_id"] = next(
            item["id"] for item in self.timeline["tracks"][1]["items"]
            if item["type"] == "clip")
        remove = OllamaPlanner(opener=RecordingOpener({
            "message": {"content": json.dumps(wrong_clip)},
        })).plan(
            self.timeline, "Enlève le deuxième clip de la piste vidéo 1."
        ).operation
        primary_clips = [
            item for item in self.timeline["tracks"][0]["items"]
            if item["type"] == "clip"]
        self.assertEqual(remove["clip_id"], primary_clips[1]["id"])

        insert = {
            "type": "InsertClip",
            "track_id": self.timeline["tracks"][1]["id"],
            "source_id": self.timeline["sources"][1]["id"],
            "source_in": {"value": 300, "rate": 25},
            "duration": {"value": 20, "rate": 25},
            "timeline_in": {"value": 1, "rate": 25},
        }
        normalized = OllamaPlanner(opener=RecordingOpener({
            "message": {"content": json.dumps({
                "operation": insert, "refusal": None})},
        })).plan(
            self.timeline,
            "Ajoute à la fin de la piste vidéo 1 vingt images de "
            "interview.mov depuis l'image source 300.",
        ).operation
        self.assertEqual(normalized["track_id"], self.timeline["tracks"][0]["id"])
        self.assertEqual(normalized["source_id"], self.timeline["sources"][0]["id"])
        self.assertEqual(normalized["timeline_in"], {"value": 150, "rate": 25})


class BinaryIntegrationTests(unittest.TestCase):
    def setUp(self) -> None:
        if not BINARY.exists():
            self.skipTest("build/cutmachine is not available")
        self.directory = tempfile.TemporaryDirectory()
        self.document = create_project_package(
            Path(self.directory.name), "Integration")
        self.binary = CutmachineBinary(BINARY)

    def tearDown(self) -> None:
        self.directory.cleanup()

    def test_describe_and_named_refusal(self) -> None:
        timeline = self.binary.describe(self.document)
        self.assertEqual(timeline["tracks"][0]["items"][0]["alias"], "A1")
        before = self.document.read_bytes()
        result = self.binary.apply_operation(self.document, {
            "type": "RemoveClip",
            "clip_id": "01K49999999999999999999999",
            "exact_timeline": [],
        })
        self.assertFalse(result.ok)
        self.assertEqual(result.error, "UnknownClip")
        self.assertEqual(self.document.read_bytes(), before)

    def test_reordered_operation_is_serialized_canonically(self) -> None:
        # JSON object order has no semantics, while the existing C++ operation
        # reader consumes its canonical serializer order.
        operation = {
            "exact_clip": None,
            "delta": {"rate": 25, "value": -1},
            "edge": "Tail",
            "clip_id": A1,
            "type": "TrimClip",
        }
        result = self.binary.apply_operation(self.document, operation)
        self.assertTrue(result.ok, f"{result.error}: {result.detail}")

    def test_markers_are_addressable_through_describe_and_apply_op(self) -> None:
        add = self.binary.apply_operation(self.document, {
            "type": "AddMarker",
            "marker": {
                "id": "",
                "name": "Choix interview",
                "time": {"value": 15, "rate": 25},
                "color": "#33AAFF",
                "category": "Montage",
            },
            "insertion_index": -1,
        })
        self.assertTrue(add.ok, f"{add.error}: {add.detail}")
        timeline = self.binary.describe(self.document)
        self.assertEqual(timeline["markers"][0]["alias"], "K1")
        marker_id = timeline["markers"][0]["id"]

        update = self.binary.apply_operation(self.document, {
            "type": "UpdateMarker",
            "marker_id": marker_id,
            "name": "Choix final",
            "time": {"value": 20, "rate": 25},
            "color": "#FFAA33",
            "category": "Validation",
        })
        self.assertTrue(update.ok, f"{update.error}: {update.detail}")
        self.assertEqual(
            self.binary.describe(self.document)["markers"][0]["name"],
            "Choix final",
        )

        remove = self.binary.apply_operation(self.document, {
            "type": "RemoveMarker", "marker_id": marker_id,
        })
        self.assertTrue(remove.ok, f"{remove.error}: {remove.detail}")
        self.assertEqual(self.binary.describe(self.document)["markers"], [])


class FakePlanner(Planner):
    def __init__(self) -> None:
        self.errors: list[dict[str, Any] | None] = []

    @property
    def backend_name(self) -> str:
        return "fake"

    def plan(self, timeline: dict[str, Any], instruction: str,
             previous_error: dict[str, Any] | None = None) -> Plan:
        self.errors.append(previous_error)
        return Plan(operation=remove_plan()["operation"])


class FakeBinary:
    def __init__(self) -> None:
        self.apply_count = 0

    def describe(self, document: Path) -> dict[str, Any]:
        return {"sources": [], "tracks": [], "duration": {"frames": 0,
                                                               "seconds": 0.0}}

    def apply_operation(self, document: Path,
                        operation: dict[str, Any]) -> ApplyResult:
        self.apply_count += 1
        if self.apply_count == 1:
            return ApplyResult(False, error="Overlap", detail="test overlap")
        return ApplyResult(True, doc_hash="abc123")


class ReplTests(unittest.TestCase):
    def test_one_retry_includes_named_binary_error(self) -> None:
        planner = FakePlanner()
        binary = FakeBinary()
        answers = iter(("o", "o"))
        output: list[str] = []
        run_turn(planner, binary, Path("unused.json"), "supprime A1",
                 input_fn=lambda _: next(answers), print_fn=output.append)
        self.assertEqual(binary.apply_count, 2)
        self.assertIsNone(planner.errors[0])
        self.assertEqual(planner.errors[1]["error"], "Overlap")
        self.assertIn("abc123", output[-1])


class EvaluationTests(unittest.TestCase):
    def test_corpus_has_exactly_fifteen_cases(self) -> None:
        self.assertEqual(len(CASES), 15)

    def test_semantic_comparison_accepts_equivalent_timebase(self) -> None:
        expected = CASES[4].expected
        actual = json.loads(json.dumps(expected))
        actual["delta"] = {"value": -20, "rate": 50}
        self.assertTrue(operations_equal(actual, expected))

    def test_all_expected_operations_are_accepted_on_fresh_documents(self) -> None:
        if not BINARY.exists():
            self.skipTest("build/cutmachine is not available")
        binary = CutmachineBinary(BINARY)
        with tempfile.TemporaryDirectory() as directory:
            for index, case in enumerate(CASES):
                document = create_project_package(
                    Path(directory), f"Case-{index}")
                result = binary.apply_operation(document, case.expected)
                self.assertTrue(
                    result.ok,
                    f"invalid oracle for case {index + 1}: "
                    f"{result.error}: {result.detail}",
                )


class FakeResolveClip:
    """Duck type of a Resolve MediaPoolItem, property bag included."""

    def __init__(self, **properties: Any) -> None:
        self.properties = properties

    def GetClipProperty(self, key: str | None = None):
        # The real API answers the whole property bag with no argument and a
        # single value with a key. collect() uses the first form, the send
        # path the second, so the fake has to do both.
        if key is None:
            return self.properties
        return self.properties.get(key, "")

    def GetUniqueId(self) -> str:
        return self.properties.get("uid", "")


class FakeResolveFolder:
    def __init__(self, name: str, clips=None, folders=None) -> None:
        self.name = name
        self.clips = clips or []
        self.folders = folders or []

    def GetName(self) -> str:
        return self.name

    def GetClipList(self):
        return self.clips

    def GetSubFolderList(self):
        return self.folders


class ResolveBridgeTests(unittest.TestCase):
    def media_pool(self) -> FakeResolveFolder:
        rush = FakeResolveClip(**{
            "File Path": "/rushes/C8015.MP4",
            "Clip Name": "C8015.MP4",
            "uid": "5d45",
        })
        loose = FakeResolveClip(**{
            "File Path": "/rushes/C8035.MP4",
            "File Name": "C8035.MP4",
        })
        timeline = FakeResolveClip(**{
            "File Path": "",
            "Clip Name": "DERUSH",
            "Type": "Timeline",
        })
        rosie = FakeResolveFolder("Rosie", clips=[rush])
        rushes = FakeResolveFolder("1_RUSHES", folders=[rosie])
        tl = FakeResolveFolder("TL", clips=[timeline])
        return FakeResolveFolder("Master", clips=[loose],
                                 folders=[rushes, tl])

    def test_walks_the_tree_into_flat_lists(self) -> None:
        collected = collect(self.media_pool())
        names = {b["name"]: b for b in collected["bins"]}
        self.assertEqual(set(names), {"1_RUSHES", "Rosie", "TL"})
        self.assertEqual(names["1_RUSHES"]["parent_key"], "")
        self.assertEqual(names["Rosie"]["parent_key"],
                         names["1_RUSHES"]["key"])
        self.assertEqual(len(set(b["key"] for b in collected["bins"])), 3)

    def test_root_clips_carry_the_empty_bin_key(self) -> None:
        collected = collect(self.media_pool())
        loose = [c for c in collected["clips"] if c["name"] == "C8035.MP4"]
        self.assertEqual(len(loose), 1)
        self.assertEqual(loose[0]["bin_key"], "")
        self.assertNotIn("resolve_uid", loose[0])

    def test_nested_clip_keeps_its_bin_and_identity(self) -> None:
        collected = collect(self.media_pool())
        names = {b["name"]: b for b in collected["bins"]}
        nested = [c for c in collected["clips"] if c["name"] == "C8015.MP4"]
        self.assertEqual(len(nested), 1)
        self.assertEqual(nested[0]["bin_key"], names["Rosie"]["key"])
        self.assertEqual(nested[0]["resolve_uid"], "5d45")
        self.assertEqual(nested[0]["path"], "/rushes/C8015.MP4")

    def test_entries_without_a_file_are_reported_not_dropped(self) -> None:
        collected = collect(self.media_pool())
        self.assertEqual([c["name"] for c in collected["clips"]].count(
            "DERUSH"), 0)
        skipped = collected["skipped"]
        self.assertEqual(len(skipped), 1)
        self.assertEqual(skipped[0]["name"], "DERUSH")
        self.assertEqual(skipped[0]["reason"], "Timeline")

    def test_sibling_bins_sharing_a_name_get_distinct_keys(self) -> None:
        pool = FakeResolveFolder("Master", folders=[
            FakeResolveFolder("Jour 01"), FakeResolveFolder("Jour 01")])
        collected = collect(pool)
        keys = [b["key"] for b in collected["bins"]]
        self.assertEqual(len(keys), 2)
        self.assertEqual(len(set(keys)), 2)

    def test_schema_matches_the_importer(self) -> None:
        header = (ROOT / "src" / "ResolveImport.h").read_text(
            encoding="utf-8")
        self.assertIn(f'"{SCHEMA}"', header)


class FakeResolveTimeline:
    """Resolve compte les images depuis le timecode de départ de la séquence.

    90000 = 01:00:00:00 à 25 i/s, ce que renvoie un vrai projet. Le pont doit
    décaler chaque `recordFrame` d'autant, sinon les plans repartent à zéro et
    se réempilent bout à bout -- exactement le défaut que le schéma v2 corrige.
    """

    def __init__(self, name: str, start_frame: int = 90000) -> None:
        self.name = name
        self.start_frame = start_frame
        self.video_tracks = 1

    def GetTrackCount(self, kind: str) -> int:
        return self.video_tracks if kind == "video" else 1

    def AddTrack(self, kind: str) -> bool:
        if kind != "video":
            return False
        self.video_tracks += 1
        return True

    def GetStartFrame(self) -> int:
        return self.start_frame


class FakeMediaPool:
    def __init__(self, root, timelines=None) -> None:
        self.root = root
        self.created = None
        self.timeline = None
        self.appended = None

    def GetRootFolder(self):
        return self.root

    def CreateEmptyTimeline(self, name):
        self.created = name
        self.timeline = FakeResolveTimeline(name)
        return self.timeline

    def AppendToTimeline(self, infos):
        self.appended = infos
        return True


class FakeResolveProject:
    def __init__(self, pool) -> None:
        self.pool = pool

    def GetMediaPool(self):
        return self.pool

    def GetName(self):
        return "projet"


class FakeResolveApp:
    def __init__(self, project) -> None:
        self.project = project

    def GetProjectManager(self):
        return self

    def GetCurrentProject(self):
        return self.project

    def GetVersionString(self):
        return "20.3.1.6"


class ResolveSendTests(unittest.TestCase):
    ACCENTED = "/Volumes/LaCie/Anthropoc\u00e8ne/C7429.MP4"

    def app(self, stored_path: str) -> tuple[FakeResolveApp, FakeMediaPool]:
        clip = FakeResolveClip(**{
            "File Path": stored_path, "File Name": "C7429.MP4"})
        pool = FakeMediaPool(FakeResolveFolder("Master", clips=[clip]))
        return FakeResolveApp(FakeResolveProject(pool)), pool

    def timeline(self, path: str) -> dict[str, Any]:
        return {"schema": TIMELINE_SCHEMA, "name": "MONTAGE",
                "clips": [{"path": path, "filename": "C7429.MP4",
                           "start_frame": 0, "end_frame": 100,
                           "video_layer": 0, "record_frame": 0,
                           "with_audio": True}]}

    def overlay(self, path: str) -> dict[str, Any]:
        """Un plan de coupe posé par-dessus le plan parlant."""
        timeline = self.timeline(path)
        timeline["clips"].append({
            "path": path, "filename": "C7429.MP4",
            "start_frame": 300, "end_frame": 350,
            "video_layer": 1, "record_frame": 40, "with_audio": False})
        return timeline

    def test_rebuilds_the_cut_in_resolve(self) -> None:
        app, pool = self.app(self.ACCENTED)
        message = send_timeline(app, self.timeline(self.ACCENTED))
        self.assertEqual(pool.created, "MONTAGE")
        self.assertEqual(len(pool.appended), 1)
        self.assertEqual(pool.appended[0]["startFrame"], 0)
        self.assertEqual(pool.appended[0]["endFrame"], 100)
        self.assertIn("1 plan", message)

    def test_positions_each_plan_from_the_timeline_start(self) -> None:
        # Sans le décalage, recordFrame vaudrait 0 et Resolve replacerait le
        # plan au tout début au lieu de l'endroit que le montage lui donne.
        app, pool = self.app(self.ACCENTED)
        send_timeline(app, self.overlay(self.ACCENTED))
        self.assertEqual(pool.appended[0]["recordFrame"], 90000)
        self.assertEqual(pool.appended[1]["recordFrame"], 90040)

    def test_creates_a_track_per_layer_and_addresses_it(self) -> None:
        app, pool = self.app(self.ACCENTED)
        message = send_timeline(app, self.overlay(self.ACCENTED))
        self.assertEqual(pool.timeline.video_tracks, 2)
        self.assertEqual(pool.appended[0]["trackIndex"], 1)
        self.assertEqual(pool.appended[1]["trackIndex"], 2)
        self.assertIn("2 pistes", message)

    def test_an_overlay_travels_without_its_sound(self) -> None:
        # Resolve rapporte le son de chaque plan : laisser passer celui d'un
        # plan de coupe poserait son ambiance sous l'interview recouverte.
        app, pool = self.app(self.ACCENTED)
        send_timeline(app, self.overlay(self.ACCENTED))
        self.assertNotIn("mediaType", pool.appended[0])
        self.assertEqual(pool.appended[1]["mediaType"], 1)

    def test_an_explicit_name_overrides_the_montage_name(self) -> None:
        # Resolve refuse un nom déjà pris : sans surcharge, une révision du
        # même montage ne pourrait jamais être renvoyée.
        app, pool = self.app(self.ACCENTED)
        send_timeline(app, self.timeline(self.ACCENTED), "MONTAGE v3")
        self.assertEqual(pool.created, "MONTAGE v3")

    def test_without_an_override_the_montage_name_is_kept(self) -> None:
        app, pool = self.app(self.ACCENTED)
        send_timeline(app, self.timeline(self.ACCENTED), None)
        self.assertEqual(pool.created, "MONTAGE")

    def test_matches_across_unicode_normalizations(self) -> None:
        # CUTMACHINE resolves paths through the filesystem and gets NFD;
        # Resolve reports what it stored. Raw string comparison would fail on
        # every accented folder name.
        decomposed = unicodedata.normalize("NFD", self.ACCENTED)
        self.assertNotEqual(decomposed, self.ACCENTED)
        app, pool = self.app(self.ACCENTED)
        send_timeline(app, self.timeline(decomposed))
        self.assertEqual(len(pool.appended), 1)

    def test_falls_back_to_the_filename(self) -> None:
        app, pool = self.app("/ailleurs/C7429.MP4")
        send_timeline(app, self.timeline("/Volumes/autre/C7429.MP4"))
        self.assertEqual(len(pool.appended), 1)

    def test_refuses_a_rush_absent_from_the_media_pool(self) -> None:
        app, pool = self.app(self.ACCENTED)
        timeline = self.timeline(self.ACCENTED)
        timeline["clips"][0]["filename"] = "INCONNU.MP4"
        timeline["clips"][0]["path"] = "/ailleurs/INCONNU.MP4"
        with self.assertRaises(ResolveBridgeError) as raised:
            send_timeline(app, timeline)
        self.assertIn("INCONNU.MP4", str(raised.exception))
        self.assertIsNone(pool.appended)

    def test_refuses_an_unexpected_schema(self) -> None:
        app, _ = self.app(self.ACCENTED)
        timeline = self.timeline(self.ACCENTED)
        timeline["schema"] = "autre.v1"
        with self.assertRaises(ResolveBridgeError):
            send_timeline(app, timeline)

    def test_indexes_every_nested_folder(self) -> None:
        clip = FakeResolveClip(**{"File Path": "/a/B.MP4",
                                  "File Name": "B.MP4"})
        nested = FakeResolveFolder("Master", folders=[
            FakeResolveFolder("1_RUSHES", clips=[clip])])
        index = index_media_pool(nested)
        self.assertIn(normalized("/a/B.MP4"), index["by_path"])

    def test_timeline_schema_matches_the_exporter(self) -> None:
        header = (ROOT / "src" / "ResolveExport.cc").read_text(
            encoding="utf-8")
        self.assertIn(TIMELINE_SCHEMA, header)


if __name__ == "__main__":
    unittest.main()
