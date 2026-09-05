#include "Document.h"
#include "EditLog.h"
#include "Operations.h"
#include "Timeline.h"

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

template <typename Function>
void Test(const std::string& name, Function function) {
    const int before = failures;
    try {
        function();
        if (failures == before) std::cout << "PASS: " << name << '\n';
    } catch (const std::exception& exception) {
        ++failures;
        std::cerr << "FAIL: " << name << ": " << exception.what() << '\n';
    }
}

Document EditDocument() {
    Document document;
    document.sources = {
        {"01K20000000000000000000001", "A.MP4", {25, 1}, {1000, 25}},
        {"01K20000000000000000000002", "B.MP4", {25, 1}, {1000, 25}},
    };
    document.sequence.tracks = {
        {"01K20000000000000000000003",
         "video",
         0,
         {
             {"01K20000000000000000000004",
              "01K20000000000000000000001",
              {100, 25},
              {10, 25},
              {0, 25}},
             {"01K20000000000000000000005",
              "01K20000000000000000000001",
              {200, 25},
              {10, 25},
              {20, 25}},
             {"01K20000000000000000000006",
              "01K20000000000000000000002",
              {300, 25},
              {10, 25},
              {40, 25}},
         }},
    };
    return document;
}

uint64_t CanonicalHash(const Document& document) {
    uint64_t hash = UINT64_C(1469598103934665603);
    for (unsigned char byte : document.SaveToString()) {
        hash ^= byte;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

bool Apply(EditLog& log, Document& document, Operation operation,
           const std::string& label) {
    EditError error = EditError::None;
    std::string message;
    const bool result =
        log.Apply(document, std::move(operation), error, message);
    Check(result,
          label + " failed with " + EditErrorName(error) + ": " + message);
    return result;
}

void ExpectRejected(Document document, Operation operation, EditError expected,
                    const std::string& label) {
    const std::string before = document.SaveToString();
    EditLog log;
    EditError error = EditError::None;
    std::string message;
    Check(!log.Apply(document, std::move(operation), error, message),
          label + " must be rejected");
    Check(error == expected, label + " expected " + EditErrorName(expected) +
                                 ", got " + EditErrorName(error) + ": " +
                                 message);
    Check(document.SaveToString() == before,
          label + " must leave the document byte-identical");
    Check(log.AppliedCount() == 0 && log.UndoneCount() == 0,
          label + " must leave the log unchanged");
}

}  // namespace

int main() {
    Test("color management is serializable and byte-exact on undo", [] {
        Document document = EditDocument();
        const std::string before = document.SaveToString();
        ColorManagementSettings settings;
        settings.enabled = true;
        settings.input_gamut = "sony_sgamut3_cine";
        settings.input_transfer = "sony_slog3";
        settings.input_ycbcr_matrix = "bt709";
        settings.input_range = "full";
        settings.working_gamut = "acescct";
        settings.output_gamut = "rec2020";
        settings.output_transfer = "hlg";
        EditLog log;
        Check(Apply(log, document, SetColorManagementOperation{settings},
                    "set color management"),
              "color operation applies");
        const std::string operationJson =
            SerializeOperation(log.AppliedEntries().back().op);
        Operation parsed = RemoveClipOperation{};
        EditError error = EditError::None;
        std::string message;
        Check(DeserializeOperation(operationJson, parsed, error, message) &&
                  std::holds_alternative<SetColorManagementOperation>(parsed) &&
                  SerializeOperation(parsed) == operationJson,
              "SetColorManagement JSON is canonical");
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == before,
              "color operation undo restores byte-identical document");
    });

    Test("transition operations are exact, serializable and undoable", [] {
        Document document = EditDocument();
        document.sequence.tracks[0].clips[1].timeline_in = {10, 25};
        DocumentTransition transition{
            "01K20000000000000000000030",
            document.sequence.tracks[0].id,
            document.sequence.tracks[0].clips[0].id,
            document.sequence.tracks[0].clips[1].id,
            "cross_dissolve",
            {4, 25},
            TransitionAlignment::Center,
        };
        const std::string before = document.SaveToString();
        EditLog log;
        Check(Apply(log, document, AddTransitionOperation{transition},
                    "add transition"),
              "transition add applies");
        Check(document.FindTransition(transition.id) != nullptr,
              "added transition is addressable");
        const std::string added = document.SaveToString();
        const std::string addJson =
            SerializeOperation(log.AppliedEntries().back().op);
        Operation parsed = RemoveClipOperation{};
        EditError error = EditError::None;
        std::string message;
        Check(DeserializeOperation(addJson, parsed, error, message) &&
                  std::holds_alternative<AddTransitionOperation>(parsed) &&
                  SerializeOperation(parsed) == addJson,
              "AddTransition JSON round-trips exactly");

        Check(Apply(log, document,
                    UpdateTransitionOperation{transition.id,
                                              "cross_dissolve",
                                              {2, 25},
                                              TransitionAlignment::EndAtCut},
                    "update transition"),
              "transition update applies");
        Check(document.FindTransition(transition.id)->duration ==
                      RationalTime{2, 25} &&
                  document.FindTransition(transition.id)->alignment ==
                      TransitionAlignment::EndAtCut,
              "transition update changes only transition state");
        Check(Apply(log, document, RemoveTransitionOperation{transition.id},
                    "remove transition"),
              "transition remove applies");
        Check(!document.FindTransition(transition.id),
              "transition remove leaves both clips intact");
        Check(log.Undo(document, error, message) &&
                  document.FindTransition(transition.id),
              "remove undo restores transition");
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == added,
              "update undo restores exact added state");
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == before,
              "add undo restores byte-identical document");

        document.sequence.tracks[0].locked = true;
        ExpectRejected(document, AddTransitionOperation{transition},
                       EditError::LockedTrack,
                       "transition cannot be added to a locked track");
    });

    Test("insert/remove ripple positions", [] {
        Document document = EditDocument();
        EditLog log;
        InsertClipOperation insert{document.sequence.tracks[0].id,
                                   document.sources[1].id,
                                   {50, 25},
                                   {5, 25},
                                   {10, 25},
                                   {},
                                   {}};
        Check(Apply(log, document, insert, "insert"), "insert applies");
        const auto& stored =
            std::get<InsertClipOperation>(log.AppliedEntries().back().op);
        Check(document.sequence.tracks[0].clips.size() == 4,
              "insert adds one clip");
        Check(document.FindClip(stored.clip_id) != nullptr,
              "inserted clip is addressed by generated ULID");
        Check(!document.FindClip(stored.clip_id)->include_audio,
              "new video inserts never embed source audio");
        Check(document.FindClip("01K20000000000000000000005")->timeline_in ==
                  RationalTime{25, 25},
              "second original clip ripples right by insertion duration");
        Check(document.FindClip("01K20000000000000000000006")->timeline_in ==
                  RationalTime{45, 25},
              "all following clips ripple right");

        Check(Apply(log, document, RemoveClipOperation{stored.clip_id, {}},
                    "remove"),
              "remove applies");
        Check(document.FindClip(stored.clip_id) == nullptr,
              "remove deletes clip");
        Check(document.FindClip("01K20000000000000000000005")->timeline_in ==
                  RationalTime{20, 25},
              "remove ripples following clip left");
        Check(document.FindClip("01K20000000000000000000006")->timeline_in ==
                  RationalTime{40, 25},
              "remove restores every following position");
    });

    Test("arbitrary multi-clip move is atomic, serializable and reversible",
         [] {
             Document document = EditDocument();
             const std::string before = document.SaveToString();
             const Ulid trackId = document.sequence.tracks[0].id;
             const Ulid firstId = document.sequence.tracks[0].clips[0].id;
             const Ulid secondId = document.sequence.tracks[0].clips[1].id;
             EditLog log;
             MoveClipsOperation move{
                 {{firstId, trackId, {50, 25}}, {secondId, trackId, {70, 25}}},
                 {}};
             Check(Apply(log, document, move, "multi-clip move"),
                   "multi-clip move applies as one entry");
             Check(document.FindClip(firstId)->timeline_in ==
                           RationalTime{50, 25} &&
                       document.FindClip(secondId)->timeline_in ==
                           RationalTime{70, 25} &&
                       log.AppliedCount() == 1,
                   "every selected clip keeps its ID and relative spacing");
             const std::string json =
                 SerializeOperation(log.AppliedEntries().back().op);
             Operation parsed = RemoveClipOperation{};
             EditError error = EditError::None;
             std::string message;
             Check(DeserializeOperation(json, parsed, error, message) &&
                       std::holds_alternative<MoveClipsOperation>(parsed) &&
                       SerializeOperation(parsed) == json,
                   "MoveClips canonical JSON round-trips");
             Check(log.Undo(document, error, message) &&
                       document.SaveToString() == before,
                   "multi-clip move undo restores byte-identical state");
         });

    Test("clear and ripple delete are distinct operations", [] {
        Document document = EditDocument();
        const std::string original = document.SaveToString();
        const Ulid clearedId = "01K20000000000000000000005";
        const Ulid followingId = "01K20000000000000000000006";
        EditLog log;
        EditError error = EditError::None;
        std::string message;

        Check(Apply(log, document, ClearClipOperation{clearedId, {}}, "clear"),
              "clear applies");
        Check(!document.FindClip(clearedId), "clear removes the selected clip");
        Check(
            document.FindClip(followingId)->timeline_in == RationalTime{40, 25},
            "clear leaves the following clip in place and creates a gap");
        const std::string cleared = document.SaveToString();
        const std::string json =
            SerializeOperation(log.AppliedEntries().back().op);
        Operation parsed = RemoveClipOperation{};
        Check(DeserializeOperation(json, parsed, error, message) &&
                  std::holds_alternative<ClearClipOperation>(parsed) &&
                  SerializeOperation(parsed) == json,
              "ClearClip JSON round-trips as its own operation");
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == original,
              "clear undo restores exact document bytes");
        Check(log.Redo(document, error, message) &&
                  document.SaveToString() == cleared,
              "clear redo restores the gap without ripple");

        Document rippleDocument = EditDocument();
        EditLog rippleLog;
        Check(Apply(rippleLog, rippleDocument,
                    RemoveClipOperation{clearedId, {}}, "ripple delete"),
              "ripple delete applies");
        Check(rippleDocument.FindClip(followingId)->timeline_in ==
                  RationalTime{30, 25},
              "ripple delete closes the removed clip interval");
    });

    Test("clearing an arbitrary multi-selection is one exact operation", [] {
        Document document = EditDocument();
        const std::string original = document.SaveToString();
        const Ulid firstId = "01K20000000000000000000004";
        const Ulid lastId = "01K20000000000000000000006";
        const Ulid untouchedId = "01K20000000000000000000005";
        EditLog log;
        EditError error = EditError::None;
        std::string message;

        Check(Apply(log, document, ClearClipsOperation{{firstId, lastId}, {}},
                    "multi-clear"),
              "multi-clear applies");
        Check(!document.FindClip(firstId) && !document.FindClip(lastId) &&
                  document.FindClip(untouchedId),
              "multi-clear removes every selected clip and no other clip");
        const std::string cleared = document.SaveToString();
        const std::string json =
            SerializeOperation(log.AppliedEntries().back().op);
        Operation parsed = RemoveClipOperation{};
        Check(DeserializeOperation(json, parsed, error, message) &&
                  std::holds_alternative<ClearClipsOperation>(parsed) &&
                  SerializeOperation(parsed) == json,
              "ClearClips JSON round-trips canonically");
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == original,
              "multi-clear undo restores exact document bytes");
        Check(log.Redo(document, error, message) &&
                  document.SaveToString() == cleared,
              "multi-clear redo restores every gap exactly");

        Document locked = EditDocument();
        locked.sequence.tracks.front().locked = true;
        ExpectRejected(locked, ClearClipsOperation{{firstId, lastId}, {}},
                       EditError::LockedTrack,
                       "multi-clear touching a locked track");
    });

    Test("paste duplicates linked clips as one exact operation", [] {
        Document document = EditDocument();
        DocumentClip& video = document.sequence.tracks[0].clips[0];
        video.include_audio = false;
        video.link_group_id = "01K20000000000000000000020";
        video.sync_anchor_clip_id = video.id;
        video.sync_reference_delta = {0, 1};
        const DocumentClip copiedVideo = video;
        const Ulid videoTrackId = document.sequence.tracks[0].id;
        DocumentTrack audio;
        audio.id = "01K20000000000000000000021";
        audio.kind = "audio";
        audio.index = 1;
        DocumentClip audioClip{"01K20000000000000000000022", video.source_id,
                               video.source_in, video.duration,
                               video.timeline_in};
        audioClip.include_audio = false;
        audioClip.link_group_id = video.link_group_id;
        audioClip.sync_anchor_clip_id = video.id;
        audioClip.sync_reference_delta = {0, 1};
        audio.clips.push_back(audioClip);
        document.sequence.tracks.push_back(audio);
        const std::string before = document.SaveToString();

        PasteClipsOperation paste{{
            {copiedVideo.id,
             videoTrackId,
             copiedVideo.source_id,
             copiedVideo.source_in,
             copiedVideo.duration,
             {60, 25},
             {},
             copiedVideo.link_group_id,
             {},
             copiedVideo.sync_anchor_clip_id,
             {},
             copiedVideo.sync_reference_delta},
            {audioClip.id,
             audio.id,
             audioClip.source_id,
             audioClip.source_in,
             audioClip.duration,
             {60, 25},
             {},
             audioClip.link_group_id,
             {},
             audioClip.sync_anchor_clip_id,
             {},
             audioClip.sync_reference_delta},
        }};
        EditLog log;
        Check(Apply(log, document, paste, "paste linked clips"),
              "paste applies");
        const auto& stored =
            std::get<PasteClipsOperation>(log.AppliedEntries().back().op);
        Check(stored.clips.size() == 2 && !stored.clips[0].clip_id.empty() &&
                  !stored.clips[1].clip_id.empty() &&
                  stored.clips[0].link_group_id ==
                      stored.clips[1].link_group_id &&
                  stored.clips[0].link_group_id != copiedVideo.link_group_id,
              "paste generates stable clip IDs and a fresh link group");
        const DocumentClip* pastedVideo =
            document.FindClip(stored.clips[0].clip_id);
        const DocumentClip* pastedAudio =
            document.FindClip(stored.clips[1].clip_id);
        Check(pastedVideo && pastedAudio && !pastedVideo->include_audio &&
                  !pastedAudio->include_audio &&
                  pastedVideo->sync_anchor_clip_id == pastedVideo->id &&
                  pastedAudio->sync_anchor_clip_id == pastedVideo->id,
              "pasted A/V remains separate and linked to the new video clip");
        const std::string after = document.SaveToString();
        const std::string serialized =
            SerializeOperation(log.AppliedEntries().back().op);
        Operation parsed = RemoveClipOperation{};
        EditError error = EditError::None;
        std::string message;
        Check(DeserializeOperation(serialized, parsed, error, message) &&
                  SerializeOperation(parsed) == serialized,
              "PasteClips JSON round-trips exactly");
        std::string legacyPaste = serialized;
        const std::string appendedField = ",\"overwrite\":false";
        const size_t appendedAt = legacyPaste.rfind(appendedField);
        Check(appendedAt != std::string::npos,
              "canonical paste contains its overwrite field");
        if (appendedAt != std::string::npos)
            legacyPaste.erase(appendedAt, appendedField.size());
        const std::string opacityField = ",\"opacity\":{\"num\":1,\"den\":1}";
        size_t opacityAt = 0;
        while ((opacityAt = legacyPaste.find(opacityField, opacityAt)) !=
               std::string::npos) {
            legacyPaste.erase(opacityAt, opacityField.size());
        }
        Operation parsedLegacy = RemoveClipOperation{};
        Check(DeserializeOperation(legacyPaste, parsedLegacy, error, message) &&
                  std::holds_alternative<PasteClipsOperation>(parsedLegacy) &&
                  !std::get<PasteClipsOperation>(parsedLegacy).overwrite &&
                  SerializeOperation(parsedLegacy).find(opacityField) !=
                      std::string::npos,
              "legacy PasteClips logs without overwrite or opacity remain "
              "readable");
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == before,
              "paste undo restores byte-identical document JSON");
        Check(log.Redo(document, error, message) &&
                  document.SaveToString() == after,
              "paste redo retains generated clip and link IDs");

        PasteClipsOperation overlap = paste;
        overlap.exact_track_result.clear();
        for (PastedClip& item : overlap.clips) {
            item.timeline_in = {0, 25};
            item.clip_id.clear();
            item.link_group_id.clear();
            item.sync_anchor_clip_id.clear();
        }
        ExpectRejected(document, overlap, EditError::Overlap,
                       "paste cannot overwrite existing clips");

        PasteClipsOperation overwrite = paste;
        overwrite.overwrite = true;
        for (PastedClip& item : overwrite.clips) item.timeline_in = {5, 25};
        const std::string beforeOverwrite = document.SaveToString();
        EditLog overwriteLog;
        Check(Apply(overwriteLog, document, overwrite, "overwrite paste"),
              "overwrite paste applies at the playhead");
        Check(document.sequence.tracks[0].clips.front().duration ==
                      RationalTime{5, 25} &&
                  document.sequence.tracks[1].clips.front().duration ==
                      RationalTime{5, 25},
              "overwrite paste trims covered clips on video and audio tracks");
        EditError overwriteError = EditError::None;
        std::string overwriteMessage;
        Check(overwriteLog.Undo(document, overwriteError, overwriteMessage) &&
                  document.SaveToString() == beforeOverwrite,
              "overwrite paste undo restores every covered track exactly");

        PasteClipsOperation locked = paste;
        for (PastedClip& item : locked.clips) item.timeline_in = {80, 25};
        document.sequence.tracks[1].locked = true;
        ExpectRejected(document, locked, EditError::LockedTrack,
                       "paste cannot modify a locked destination track");
    });

    Test("mixed-rate ripple undo restores exact representation", [] {
        Document document;
        document.sources = {
            {"01K40000000000000000000001", "25.MP4", {25, 1}, {250, 25}},
            {"01K40000000000000000000002",
             "2997.MP4",
             {30000, 1001},
             {300300, 30000}},
        };
        document.sequence.tracks = {
            {"01K40000000000000000000003",
             "video",
             0,
             {
                 {"01K40000000000000000000004",
                  "01K40000000000000000000001",
                  {0, 25},
                  {25, 25},
                  {0, 25}},
                 {"01K40000000000000000000005",
                  "01K40000000000000000000002",
                  {100100, 30000},
                  {1001, 30000},
                  {30000, 30000}},
             }},
        };
        const std::string original = document.SaveToString();
        EditLog log;
        Check(Apply(log, document,
                    InsertClipOperation{document.sequence.tracks[0].id,
                                        document.sources[1].id,
                                        {0, 30000},
                                        {1001, 30000},
                                        {25, 25},
                                        "01K40000000000000000000006",
                                        {}},
                    "mixed-rate insert"),
              "mixed-rate insertion applies");
        Check(
            document.FindClip("01K40000000000000000000005")->timeline_in.rate ==
                30000,
            "ripple uses an exact common timebase");
        EditError error = EditError::None;
        std::string message;
        Check(log.Undo(document, error, message),
              "mixed-rate insertion undo succeeds: " + message);
        Check(document.SaveToString() == original,
              "mixed-rate undo restores original RationalTime representation");
    });

    Test("head and tail trim stay local", [] {
        {
            Document isolated = EditDocument();
            isolated.sequence.tracks[0].clips.erase(
                isolated.sequence.tracks[0].clips.begin() + 1,
                isolated.sequence.tracks[0].clips.end());
            EditLog log;
            Check(
                Apply(log, isolated,
                      TrimClipOperation{isolated.sequence.tracks[0].clips[0].id,
                                        TrimEdge::Head,
                                        {2, 25},
                                        std::nullopt},
                      "isolated head trim"),
                "isolated head trim applies");
            const DocumentClip& clip = isolated.sequence.tracks[0].clips[0];
            Check(clip.source_in == RationalTime{102, 25} &&
                      clip.duration == RationalTime{8, 25} &&
                      clip.timeline_in == RationalTime{2, 25},
                  "head trim changes exactly source_in, duration and "
                  "timeline_in");
            Check(Apply(log, isolated,
                        TrimClipOperation{
                            clip.id, TrimEdge::Tail, {-2, 25}, std::nullopt},
                        "isolated tail trim"),
                  "isolated tail trim applies");
            Check(isolated.sequence.tracks[0].clips[0].duration ==
                      RationalTime{6, 25},
                  "tail trim changes duration");
        }
        {
            Document framed = EditDocument();
            const RationalTime beforePrevious =
                framed.sequence.tracks[0].clips[0].timeline_in;
            const RationalTime beforeNext =
                framed.sequence.tracks[0].clips[2].timeline_in;
            EditLog log;
            const Ulid middle = framed.sequence.tracks[0].clips[1].id;
            Check(Apply(log, framed,
                        TrimClipOperation{
                            middle, TrimEdge::Head, {2, 25}, std::nullopt},
                        "framed head trim"),
                  "framed head trim applies");
            Check(framed.sequence.tracks[0].clips[0].timeline_in ==
                          beforePrevious &&
                      framed.sequence.tracks[0].clips[2].timeline_in ==
                          beforeNext,
                  "head trim does not move surrounding clips");
            Check(Apply(log, framed,
                        TrimClipOperation{
                            middle, TrimEdge::Tail, {2, 25}, std::nullopt},
                        "framed tail trim"),
                  "framed tail trim applies inside the gap");
            Check(framed.sequence.tracks[0].clips[2].timeline_in == beforeNext,
                  "tail trim does not move following clip");
        }
    });

    Test("invalid preconditions are atomic and named", [] {
        const Document base = EditDocument();
        const InsertClipOperation validInsert{base.sequence.tracks[0].id,
                                              base.sources[0].id,
                                              {0, 25},
                                              {2, 25},
                                              {10, 25},
                                              {},
                                              {}};
        InsertClipOperation unknownTrack = validInsert;
        unknownTrack.track_id = "01K29999999999999999999991";
        ExpectRejected(base, unknownTrack, EditError::UnknownTrack,
                       "unknown track_id");
        InsertClipOperation unknownSource = validInsert;
        unknownSource.source_id = "01K29999999999999999999992";
        ExpectRejected(base, unknownSource, EditError::UnknownSource,
                       "unknown source_id");
        InsertClipOperation zeroDuration = validInsert;
        zeroDuration.duration = {0, 25};
        ExpectRejected(base, zeroDuration, EditError::InvalidDuration,
                       "zero insert duration");
        InsertClipOperation negativeDuration = validInsert;
        negativeDuration.duration = {-1, 25};
        ExpectRejected(base, negativeDuration, EditError::InvalidDuration,
                       "negative insert duration");
        InsertClipOperation negativeTimeline = validInsert;
        negativeTimeline.timeline_in = {-1, 25};
        ExpectRejected(base, negativeTimeline, EditError::InvalidTimelineIn,
                       "negative insertion timeline_in");
        InsertClipOperation overlap = validInsert;
        overlap.timeline_in = {5, 25};
        ExpectRejected(base, overlap, EditError::Overlap, "insertion overlap");
        InsertClipOperation outside = validInsert;
        outside.source_in = {999, 25};
        ExpectRejected(base, outside, EditError::SourceOutOfBounds,
                       "insert source bounds");
        ExpectRejected(base,
                       RemoveClipOperation{"01K29999999999999999999993", {}},
                       EditError::UnknownClip, "unknown remove clip_id");
        ExpectRejected(base,
                       TrimClipOperation{"01K29999999999999999999994",
                                         TrimEdge::Tail,
                                         {1, 25},
                                         std::nullopt},
                       EditError::UnknownClip, "unknown trim clip_id");
        ExpectRejected(base,
                       TrimClipOperation{base.sequence.tracks[0].clips[0].id,
                                         TrimEdge::Tail,
                                         {-10, 25},
                                         std::nullopt},
                       EditError::InvalidDuration, "zero trim duration");
        ExpectRejected(base,
                       TrimClipOperation{base.sequence.tracks[0].clips[0].id,
                                         TrimEdge::Head,
                                         {-101, 25},
                                         std::nullopt},
                       EditError::InvalidTimelineIn,
                       "head trim before timeline zero");

        Document sourceBound = base;
        sourceBound.sequence.tracks[0].clips[0].timeline_in = {200, 25};
        sourceBound.sequence.tracks[0].clips[1].timeline_in = {220, 25};
        sourceBound.sequence.tracks[0].clips[2].timeline_in = {240, 25};
        ExpectRejected(
            sourceBound,
            TrimClipOperation{sourceBound.sequence.tracks[0].clips[0].id,
                              TrimEdge::Head,
                              {-101, 25},
                              std::nullopt},
            EditError::InvalidOperation, "head trim before source start");

        Document tailOverlap = base;
        tailOverlap.sequence.tracks[0].clips[1].timeline_in = {11, 25};
        tailOverlap.sequence.tracks[0].clips[2].timeline_in = {40, 25};
        ExpectRejected(
            tailOverlap,
            TrimClipOperation{tailOverlap.sequence.tracks[0].clips[0].id,
                              TrimEdge::Tail,
                              {2, 25},
                              std::nullopt},
            EditError::Overlap, "tail trim overlap");
    });

    Test("operation and edit-log serialization round trips", [] {
        Document document = EditDocument();
        const std::string originalDocument = document.SaveToString();
        EditLog log;
        Check(Apply(log, document,
                    InsertClipOperation{document.sequence.tracks[0].id,
                                        document.sources[1].id,
                                        {10, 25},
                                        {3, 25},
                                        {10, 25},
                                        {},
                                        {}},
                    "serialized insert"),
              "insert applies");
        Check(Apply(log, document,
                    TrimClipOperation{document.sequence.tracks[0].clips[0].id,
                                      TrimEdge::Tail,
                                      {-1, 25},
                                      std::nullopt},
                    "serialized trim"),
              "trim applies");
        const Operation& original = log.AppliedEntries()[0].op;
        const std::string json = SerializeOperation(original);
        Operation parsed = RemoveClipOperation{};
        EditError error = EditError::None;
        std::string message;
        Check(DeserializeOperation(json, parsed, error, message),
              "operation parses: " + message);
        Check(SerializeOperation(parsed) == json,
              "operation JSON is canonical after round trip");

        const std::string logJson = log.Serialize();
        EditLog parsedLog;
        Check(EditLog::Deserialize(logJson, parsedLog, error, message),
              "edit log parses: " + message);
        Check(parsedLog.Serialize() == logJson,
              "edit log JSON is canonical after round trip");

        const std::filesystem::path path =
            std::filesystem::temp_directory_path() /
            (GenerateUlid() + "-cutmachine-edit-log.json");
        Check(log.Save(path.string(), error, message),
              "edit log saves: " + message);
        EditLog loaded;
        Check(EditLog::Load(path.string(), loaded, error, message),
              "edit log loads: " + message);
        Check(loaded.Serialize() == logJson, "persisted edit log is identical");
        Check(loaded.Undo(document, error, message),
              "loaded log can undo trim: " + message);
        Check(loaded.Undo(document, error, message),
              "loaded log can undo insert: " + message);
        Check(document.SaveToString() == originalDocument,
              "persisted inverses restore the original document bytes");
        std::filesystem::remove(path);
    });

    Test("sequence settings are addressable and reversible", [] {
        Document document = EditDocument();
        const std::string original = document.SaveToString();
        const Ulid sequenceId = document.sequence.id;
        const Ulid firstTrackId = document.sequence.tracks.front().id;
        EditLog log;
        EditError error = EditError::None;
        std::string message;

        Check(
            Apply(log, document,
                  UpdateSequenceOperation{
                      sequenceId, "Vertical master", 1080, 1920, {24000, 1001}},
                  "update sequence"),
            "sequence update succeeds");
        Check(document.sequence.id == sequenceId &&
                  document.sequence.name == "Vertical master" &&
                  document.sequence.width == 1080 &&
                  document.sequence.height == 1920 &&
                  document.sequence.frame_rate.num == 24000 &&
                  document.sequence.frame_rate.den == 1001,
              "sequence settings change without replacing its identity");
        Check(document.sequence.tracks.front().id == firstTrackId,
              "sequence update preserves the owned timeline");

        const Operation& stored = log.AppliedEntries().back().op;
        const std::string json = SerializeOperation(stored);
        Operation parsed = RemoveClipOperation{};
        Check(DeserializeOperation(json, parsed, error, message) &&
                  SerializeOperation(parsed) == json,
              "UpdateSequence JSON round-trips canonically: " + message);
        const std::string updated = document.SaveToString();
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == original,
              "sequence settings undo restores original bytes");
        Check(log.Redo(document, error, message) &&
                  document.SaveToString() == updated,
              "sequence settings redo restores updated bytes");

        ExpectRejected(
            document,
            UpdateSequenceOperation{
                "01K83000000000000000000009", "Other", 1920, 1080, {25, 1}},
            EditError::UnknownSequence, "unknown sequence");
        ExpectRejected(
            document,
            UpdateSequenceOperation{sequenceId, "Invalid", 0, 1080, {25, 1}},
            EditError::ValidationFailed, "invalid sequence dimensions");
    });

    Test("removing a populated track restores its clips on undo", [] {
        Document document = EditDocument();
        EditLog log;
        const Ulid trackId = document.sequence.tracks.front().id;
        const std::string original = document.SaveToString();
        Check(!document.sequence.tracks.front().clips.empty(),
              "fixture track contains clips");
        Check(Apply(log, document, RemoveTrackOperation{trackId},
                    "remove populated track"),
              "populated track removal succeeds");
        Check(!document.FindTrack(trackId), "track and its clips are removed");
        const std::string inverseJson =
            SerializeOperation(log.AppliedEntries().back().inverse);
        Operation parsed = RemoveClipOperation{};
        EditError error = EditError::None;
        std::string message;
        Check(DeserializeOperation(inverseJson, parsed, error, message) &&
                  SerializeOperation(parsed) == inverseJson,
              "track contents round-trip in the inverse operation");
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == original,
              "undo restores the populated track byte-exactly");
    });

    Test("bins create and classify media through reversible operations", [] {
        Document document = EditDocument();
        LibraryMedia media;
        media.id = document.sources[0].id;
        media.path = "A.MP4";
        media.filename = "A.MP4";
        media.codec = "h264";
        media.width = 1920;
        media.height = 1080;
        media.rate = {25, 1};
        media.duration = {1000, 25};
        media.orientation = "landscape";
        document.library.push_back(media);
        const std::string initial = document.SaveToString();
        EditLog log;
        const Ulid binId = "01K80000000000000000000001";
        Check(Apply(log, document, AddBinOperation{binId, "Rushes \"A\""},
                    "add bin"),
              "bin creation succeeds");
        Check(Apply(log, document,
                    SetMediaBinOperation{document.library[0].id, binId},
                    "assign media bin"),
              "media assignment succeeds");
        Check(document.bins.size() == 1 && document.library[0].bin_id == binId,
              "bin and media assignment are stored in the document");
        const std::string serialized = log.Serialize();
        EditLog parsed;
        EditError error = EditError::None;
        std::string message;
        Check(EditLog::Deserialize(serialized, parsed, error, message) &&
                  parsed.Serialize() == serialized,
              "bin operations round-trip in the event log: " + message);
        Check(log.Undo(document, error, message) &&
                  document.library[0].bin_id.empty(),
              "undo restores media to the project root");
        Check(log.Undo(document, error, message) && document.bins.empty() &&
                  document.SaveToString() == initial,
              "second undo removes the empty bin byte-exactly");
        Check(log.Redo(document, error, message) &&
                  log.Redo(document, error, message) &&
                  document.library[0].bin_id == binId,
              "redo recreates the same bin ULID and assignment");
    });

    Test("nested bins preserve hierarchy and reject destructive removal", [] {
        Document document = EditDocument();
        LibraryMedia media;
        media.id = document.sources[0].id;
        media.path = "A.MP4";
        media.filename = "A.MP4";
        media.codec = "h264";
        media.width = 1920;
        media.height = 1080;
        media.rate = {25, 1};
        media.duration = {1000, 25};
        media.orientation = "landscape";
        document.library.push_back(media);
        EditLog log;
        const Ulid parent = "01K81000000000000000000001";
        const Ulid child = "01K81000000000000000000002";
        Check(Apply(log, document, AddBinOperation{parent, "Rushes", ""},
                    "add parent bin"),
              "top-level bin creation succeeds");
        Check(Apply(log, document, AddBinOperation{child, "Interview", parent},
                    "add child bin"),
              "child bin creation succeeds");
        Check(document.FindBin(child) &&
                  document.FindBin(child)->parent_id == parent,
              "child retains its exact parent ULID");

        const std::string childJson =
            SerializeOperation(log.AppliedEntries().back().op);
        Operation parsed = RemoveClipOperation{};
        EditError error = EditError::None;
        std::string message;
        Check(DeserializeOperation(childJson, parsed, error, message) &&
                  SerializeOperation(parsed) == childJson,
              "nested AddBin JSON round-trips canonically");

        const Ulid sibling = "01K81000000000000000000003";
        Check(Apply(log, document, AddBinOperation{sibling, "B-roll", ""},
                    "add sibling"),
              "second top-level bin creation succeeds");
        Check(Apply(log, document, MoveBinOperation{child, sibling},
                    "move child bin"),
              "bin can be moved under another bin");
        Check(document.FindBin(child)->parent_id == sibling,
              "moved bin stores its new parent");
        const std::string moveJson =
            SerializeOperation(log.AppliedEntries().back().op);
        Check(DeserializeOperation(moveJson, parsed, error, message) &&
                  SerializeOperation(parsed) == moveJson,
              "MoveBin JSON round-trips canonically");
        Check(log.Undo(document, error, message) &&
                  document.FindBin(child)->parent_id == parent,
              "move undo restores the previous bin parent");
        Check(log.Redo(document, error, message) &&
                  document.FindBin(child)->parent_id == sibling,
              "move redo restores the new bin parent");
        Check(log.Undo(document, error, message),
              "restore original hierarchy for following checks");
        ExpectRejected(document, MoveBinOperation{parent, child},
                       EditError::InvalidOperation, "cyclic bin move");

        Check(
            Apply(log, document, RenameBinOperation{child, "Interview selects"},
                  "rename child bin"),
            "bin rename succeeds through the event log");
        const std::string renameJson =
            SerializeOperation(log.AppliedEntries().back().op);
        Check(DeserializeOperation(renameJson, parsed, error, message) &&
                  SerializeOperation(parsed) == renameJson,
              "RenameBin JSON round-trips canonically");
        Check(log.Undo(document, error, message) &&
                  document.FindBin(child)->name == "Interview",
              "rename undo restores the original name");

        ExpectRejected(document, RemoveBinOperation{parent, "", ""},
                       EditError::InvalidOperation, "remove bin with child");
        Check(Apply(log, document, SetMediaBinOperation{media.id, child},
                    "classify in child"),
              "media can be placed in a nested bin");
        ExpectRejected(document, RemoveBinOperation{child, "", ""},
                       EditError::InvalidOperation,
                       "remove non-empty child bin");

        Document loaded;
        Check(Document::LoadFromString(document.SaveToString(), loaded,
                                       message) &&
                  loaded.FindBin(child) &&
                  loaded.FindBin(child)->parent_id == parent,
              "document JSON preserves the bin hierarchy");

        Document cyclic = document;
        cyclic.FindBin(parent)->parent_id = child;
        Check(!cyclic.Validate(message), "bin hierarchy cycles are rejected");
    });

    Test(
        "markers are addressable, serializable and byte-exactly reversible",
        [] {
            Document document = EditDocument();
            const Ulid first = "01K82000000000000000000001";
            const Ulid second = "01K82000000000000000000002";
            document.sequence.markers = {
                {first, "Opening", {0, 25}, "#e84a5f", "chapter"},
                {second, "Interview", {125, 25}, "blue", "comment"},
            };
            std::string validation;
            Check(document.Validate(validation),
                  "marker fixture validates: " + validation);
            const std::string original = document.SaveToString();
            EditLog log;
            EditError error = EditError::None;
            std::string message;

            AddMarkerOperation add{
                {{}, "B-roll", {30000, 1001}, "#33aa77", "select"}, -1};
            Check(Apply(log, document, add, "add marker"),
                  "marker addition succeeds");
            const auto& storedAdd =
                std::get<AddMarkerOperation>(log.AppliedEntries().back().op);
            Check(IsValidUlid(storedAdd.marker.id) &&
                      document.FindMarker(storedAdd.marker.id),
                  "AddMarker receives and retains a stable ULID");
            Check(storedAdd.insertion_index == 2,
                  "AddMarker records its exact canonical position");
            const std::string added = document.SaveToString();
            const std::string addJson = SerializeOperation(storedAdd);
            Operation parsed = RemoveClipOperation{};
            Check(DeserializeOperation(addJson, parsed, error, message) &&
                      SerializeOperation(parsed) == addJson,
                  "AddMarker JSON round-trips canonically");
            Check(log.Undo(document, error, message) &&
                      document.SaveToString() == original,
                  "AddMarker undo restores original bytes");
            Check(log.Redo(document, error, message) &&
                      document.SaveToString() == added,
                  "AddMarker redo restores marker ID and bytes");
            Check(log.Undo(document, error, message) &&
                      document.SaveToString() == original,
                  "second AddMarker undo is byte-exact");

            UpdateMarkerOperation update{
                second, "Interview hero", {1001, 24000}, "amber", "approved"};
            Check(Apply(log, document, update, "update marker"),
                  "marker update succeeds");
            const std::string updateJson =
                SerializeOperation(log.AppliedEntries().back().op);
            Check(DeserializeOperation(updateJson, parsed, error, message) &&
                      SerializeOperation(parsed) == updateJson,
                  "UpdateMarker JSON round-trips canonically");
            Check(
                document.FindMarker(second)->time == RationalTime{1001, 24000},
                "UpdateMarker preserves its exact rational time");
            Check(
                log.Undo(document, error, message) &&
                    document.SaveToString() == original,
                "UpdateMarker inverse restores every field and original bytes");

            Check(Apply(log, document, RemoveMarkerOperation{first},
                        "remove marker"),
                  "marker removal succeeds");
            const std::string removeJson =
                SerializeOperation(log.AppliedEntries().back().op);
            Check(DeserializeOperation(removeJson, parsed, error, message) &&
                      SerializeOperation(parsed) == removeJson,
                  "RemoveMarker JSON round-trips canonically");
            Check(log.Undo(document, error, message) &&
                      document.SaveToString() == original &&
                      document.sequence.markers.front().id == first,
                  "RemoveMarker undo restores the original position and bytes");

            ExpectRejected(document,
                           UpdateMarkerOperation{
                               second, "Invalid", {-1, 25}, "blue", "comment"},
                           EditError::ValidationFailed, "negative marker time");
            ExpectRejected(document,
                           RemoveMarkerOperation{"01K82000000000000000000009"},
                           EditError::UnknownMarker, "unknown marker");
        });

    Test("empty undo/redo and redo clearing", [] {
        Document document = EditDocument();
        const std::string original = document.SaveToString();
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        Check(!log.Undo(document, error, message) &&
                  error == EditError::EmptyUndo,
              "undo on empty log returns EmptyUndo");
        Check(!log.Redo(document, error, message) &&
                  error == EditError::EmptyRedo,
              "redo on empty log returns EmptyRedo");
        Check(document.SaveToString() == original,
              "empty undo/redo leave document unchanged");

        Check(Apply(log, document,
                    TrimClipOperation{document.sequence.tracks[0].clips[0].id,
                                      TrimEdge::Tail,
                                      {-1, 25},
                                      std::nullopt},
                    "trim before undo"),
              "trim applies");
        Check(log.Undo(document, error, message), "undo succeeds");
        Check(log.UndoneCount() == 1, "undo populates redo stack");
        Check(Apply(log, document,
                    TrimClipOperation{document.sequence.tracks[0].clips[1].id,
                                      TrimEdge::Tail,
                                      {-1, 25},
                                      std::nullopt},
                    "new trim"),
              "new trim applies");
        Check(log.UndoneCount() == 0, "new operation clears redo stack");
    });

    Test("20 operations full undo/redo canonical hash", [] {
        Document document = EditDocument();
        // Keep one base clip so eight end insertions form a simple, valid
        // chain.
        document.sequence.tracks[0].clips.erase(
            document.sequence.tracks[0].clips.begin() + 1,
            document.sequence.tracks[0].clips.end());
        std::string validation;
        Check(document.Validate(validation),
              "hash fixture validates: " + validation);
        const std::string originalJson = document.SaveToString();
        const uint64_t originalHash = CanonicalHash(document);
        EditLog log;
        std::vector<Ulid> inserted;
        const std::vector<Ulid> insertedIds = {
            "01K30000000000000000000001", "01K30000000000000000000002",
            "01K30000000000000000000003", "01K30000000000000000000004",
            "01K30000000000000000000005", "01K30000000000000000000006",
            "01K30000000000000000000007", "01K30000000000000000000008",
        };

        for (int index = 0; index < 8; ++index) {
            const RationalTime end = Timeline(document).Duration();
            Check(Apply(log, document,
                        InsertClipOperation{document.sequence.tracks[0].id,
                                            document.sources[index % 2].id,
                                            {400 + index * 10, 25},
                                            {5, 25},
                                            end,
                                            insertedIds[index],
                                            {}},
                        "hash insert " + std::to_string(index)),
                  "hash insertion succeeds");
            inserted.push_back(
                std::get<InsertClipOperation>(log.AppliedEntries().back().op)
                    .clip_id);
        }
        for (int index = 4; index < 8; ++index) {
            Check(Apply(log, document,
                        TrimClipOperation{inserted[index],
                                          TrimEdge::Tail,
                                          {-1, 25},
                                          std::nullopt},
                        "hash tail trim " + std::to_string(index)),
                  "hash tail trim succeeds");
        }
        for (int index = 0; index < 4; ++index) {
            Check(
                Apply(
                    log, document,
                    TrimClipOperation{
                        inserted[index], TrimEdge::Head, {1, 25}, std::nullopt},
                    "hash head trim " + std::to_string(index)),
                "hash head trim succeeds");
        }
        Check(Apply(log, document, RemoveClipOperation{inserted[0], {}},
                    "hash remove 0"),
              "first hash remove succeeds");
        Check(Apply(log, document, RemoveClipOperation{inserted[2], {}},
                    "hash remove 2"),
              "second hash remove succeeds");
        Check(Apply(log, document,
                    TrimClipOperation{document.sequence.tracks[0].clips[0].id,
                                      TrimEdge::Tail,
                                      {-1, 25},
                                      std::nullopt},
                    "hash base tail trim"),
              "base tail trim succeeds");
        Check(Apply(log, document,
                    TrimClipOperation{
                        inserted[7], TrimEdge::Head, {1, 25}, std::nullopt},
                    "hash final head trim"),
              "final head trim succeeds");
        Check(log.AppliedCount() == 20, "exactly 20 operations were applied");
        const uint64_t editedHash = CanonicalHash(document);
        Check(editedHash != originalHash,
              "20 operations materially change document");

        EditError error = EditError::None;
        std::string message;
        for (int index = 0; index < 20; ++index) {
            Check(log.Undo(document, error, message),
                  "undo " + std::to_string(index) + ": " + message);
        }
        const uint64_t firstUndoHash = CanonicalHash(document);
        Check(firstUndoHash == originalHash &&
                  document.SaveToString() == originalJson,
              "full undo restores canonical bytes exactly");

        for (int index = 0; index < 20; ++index) {
            Check(log.Redo(document, error, message),
                  "redo " + std::to_string(index) + ": " + message);
        }
        Check(CanonicalHash(document) == editedHash,
              "full redo restores edited canonical hash");
        for (int index = 0; index < 20; ++index) {
            Check(log.Undo(document, error, message),
                  "second undo " + std::to_string(index) + ": " + message);
        }
        const uint64_t secondUndoHash = CanonicalHash(document);
        Check(secondUndoHash == originalHash &&
                  document.SaveToString() == originalJson,
              "redo then full undo restores canonical bytes exactly again");
        std::cout << "HASH original=" << originalHash
                  << " edited=" << editedHash << " undo1=" << firstUndoHash
                  << " undo2=" << secondUndoHash << '\n';
    });

    Test("track locking is undoable and blocks edits", [] {
        Document document = EditDocument();
        EditLog log;
        const Ulid trackId = document.sequence.tracks[0].id;
        const Ulid clipId = document.sequence.tracks[0].clips[0].id;
        Check(Apply(log, document, SetTrackLockOperation{trackId, true},
                    "lock track"),
              "track lock applies");
        Check(document.sequence.tracks[0].locked, "track is marked locked");
        ExpectRejected(
            document,
            TrimClipOperation{clipId, TrimEdge::Tail, {-1, 25}, std::nullopt},
            EditError::LockedTrack, "trim on locked track");
        const std::string serialized =
            SerializeOperation(SetTrackLockOperation{trackId, true});
        Operation parsed = RemoveTrackOperation{};
        EditError error = EditError::None;
        std::string message;
        Check(DeserializeOperation(serialized, parsed, error, message) &&
                  SerializeOperation(parsed) == serialized,
              "SetTrackLock JSON round-trips");
        Check(log.Undo(document, error, message), "track unlock undo");
        Check(!document.sequence.tracks[0].locked,
              "undo restores the unlocked state");
    });

    Test("sync locking is independent, undoable, and serializable", [] {
        Document document = EditDocument();
        EditLog log;
        const Ulid trackId = document.sequence.tracks[0].id;
        Check(document.sequence.tracks[0].sync_lock,
              "new and migrated tracks start sync locked");
        Check(Apply(log, document, SetTrackSyncLockOperation{trackId, false},
                    "disable sync lock"),
              "sync lock operation applies");
        Check(!document.sequence.tracks[0].sync_lock &&
                  !document.sequence.tracks[0].locked,
              "sync lock does not change hard lock");
        const std::string serialized =
            SerializeOperation(SetTrackSyncLockOperation{trackId, false});
        Operation parsed = RemoveTrackOperation{};
        EditError error = EditError::None;
        std::string message;
        Check(DeserializeOperation(serialized, parsed, error, message) &&
                  SerializeOperation(parsed) == serialized,
              "SetTrackSyncLock JSON round-trips");
        Check(log.Undo(document, error, message) &&
                  document.sequence.tracks[0].sync_lock,
              "undo restores sync lock");
    });

    Test("track output state round-trips and is undoable", [] {
        Document document = EditDocument();
        EditLog log;
        const std::string original = document.SaveToString();
        const Ulid trackId = document.sequence.tracks[0].id;
        SetTrackOutputOperation change{trackId, false, true, true};
        Check(Apply(log, document, change, "change track output"),
              "track output operation applies");
        const DocumentTrack* changed = document.FindTrack(trackId);
        Check(changed && !changed->visible && changed->muted && changed->solo,
              "all output flags are stored");
        const std::string serialized = SerializeOperation(change);
        Operation parsed = RemoveTrackOperation{};
        EditError error = EditError::None;
        std::string message;
        Check(DeserializeOperation(serialized, parsed, error, message) &&
                  SerializeOperation(parsed) == serialized,
              "SetTrackOutput JSON round-trips");
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == original,
              "undo restores canonical bytes exactly");
    });

    Test("ripple trim shifts downstream and sync-locked tracks atomically", [] {
        Document document = EditDocument();
        DocumentTrack audio;
        audio.id = "01K20000000000000000000020";
        audio.kind = "audio";
        audio.index = 1;
        audio.clips.push_back({"01K20000000000000000000021",
                               document.sources[0].id,
                               {400, 25},
                               {5, 25},
                               {20, 25},
                               false});
        document.sequence.tracks.push_back(audio);
        const std::string before = document.SaveToString();
        EditLog log;
        RippleTrimOperation ripple{document.sequence.tracks[0].clips[0].id,
                                   TrimEdge::Tail,
                                   {5, 25},
                                   {},
                                   {audio.id},
                                   {}};
        Check(Apply(log, document, ripple, "ripple tail trim"),
              "ripple trim applies");
        Check(document.sequence.tracks[0].clips[0].duration ==
                      RationalTime{15, 25} &&
                  document.sequence.tracks[0].clips[1].timeline_in ==
                      RationalTime{25, 25} &&
                  document.sequence.tracks[1].clips[0].timeline_in ==
                      RationalTime{25, 25},
              "ripple extends the edge and shifts every affected downstream "
              "clip");
        const std::string serialized =
            SerializeOperation(log.AppliedEntries().back().op);
        Operation parsed = RemoveTrackOperation{};
        EditError error = EditError::None;
        std::string message;
        Check(DeserializeOperation(serialized, parsed, error, message) &&
                  SerializeOperation(parsed) == serialized,
              "RippleTrim JSON round-trips exactly");
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == before,
              "one undo restores every ripple-trimmed track");

        document.sequence.tracks[1].locked = true;
        ExpectRejected(document, ripple, EditError::LockedTrack,
                       "ripple cannot shift a locked sync track");
    });

    Test("ripple trim refuses a source range outside the real rush bounds", [] {
        Document document = EditDocument();
        const std::string before = document.SaveToString();
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        const RippleTrimOperation operation{
            document.sequence.tracks[0].clips[0].id,
            TrimEdge::Tail,
            {1000, 25},
            {},
            {},
            {}};
        Check(!log.Apply(document, operation, error, message) &&
                  error == EditError::InvalidOperation,
              "ripple trim beyond the rush is InvalidOperation: " + message);
        Check(message.find("within [0/25, 1000/25)") != std::string::npos &&
                  message.find("requested [100/25, 1110/25)") !=
                      std::string::npos,
              "ripple trim reports the real source bounds and request: " +
                  message);
        Check(log.AppliedCount() == 0 && document.SaveToString() == before,
              "rejected ripple trim is never journaled");
    });

    Test("trim clip refuses a source range outside the real rush bounds", [] {
        Document document = EditDocument();
        const std::string before = document.SaveToString();
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        const TrimClipOperation operation{
            document.sequence.tracks[0].clips[0].id,
            TrimEdge::Tail,
            {1000, 25},
            std::nullopt};
        Check(!log.Apply(document, operation, error, message) &&
                  error == EditError::InvalidOperation,
              "trim beyond the rush is InvalidOperation: " + message);
        Check(message.find("within [0/25, 1000/25)") != std::string::npos &&
                  message.find("requested [100/25, 1110/25)") !=
                      std::string::npos,
              "trim reports the real source bounds and request: " + message);
        Check(log.AppliedCount() == 0 && document.SaveToString() == before,
              "rejected trim is never journaled");
    });

    Test("A5 synchronized ripple and cuts are exact and reversible", [] {
        const Ulid syncTrackId = "01K20000000000000000000020";
        {
            Document document = EditDocument();
            DocumentTrack synced{syncTrackId,
                                 "video",
                                 1,
                                 {{"01K20000000000000000000021",
                                   document.sources[0].id,
                                   {400, 25},
                                   {10, 25},
                                   {20, 25}}}};
            document.sequence.tracks.push_back(synced);
            const std::string before = document.SaveToString();
            EditLog log;
            RemoveClipOperation remove{
                document.sequence.tracks[0].clips[0].id, {syncTrackId}, {}};
            Check(Apply(log, document, remove, "synchronized remove"),
                  "remove_clip accepts a synchronized track");
            Check(document.sequence.tracks[0].clips[0].timeline_in ==
                          RationalTime{10, 25} &&
                      document.sequence.tracks[1].clips[0].timeline_in ==
                          RationalTime{10, 25},
                  "remove_clip ripples every named track by the same exact "
                  "duration");
            const std::string json =
                SerializeOperation(log.AppliedEntries().back().op);
            Operation parsed = RemoveClipOperation{};
            EditError error = EditError::None;
            std::string message;
            Check(DeserializeOperation(json, parsed, error, message) &&
                      std::get<RemoveClipOperation>(parsed).sync_track_ids ==
                          std::vector<Ulid>{syncTrackId} &&
                      SerializeOperation(parsed) == json,
                  "RemoveClip sync_track_ids round-trip canonically");
            Check(log.Undo(document, error, message) &&
                      document.SaveToString() == before,
                  "RemoveClip synchronized undo restores exact bytes");
        }

        const auto linkedDocument = [&](bool crossingSyncClip) {
            Document document;
            document.sources = {
                {"01K20000000000000000000001", "A.MP4", {25, 1}, {1000, 25}}};
            const Ulid group = "01K20000000000000000000030";
            DocumentClip video{"01K20000000000000000000031",
                               document.sources[0].id,
                               {100, 25},
                               {10, 25},
                               {0, 25}};
            video.link_group_id = group;
            video.sync_anchor_clip_id = video.id;
            DocumentClip audio = video;
            audio.id = "01K20000000000000000000032";
            audio.sync_anchor_clip_id = video.id;
            DocumentClip followingVideo{"01K20000000000000000000033",
                                        document.sources[0].id,
                                        {200, 25},
                                        {10, 25},
                                        {10, 25}};
            DocumentClip followingAudio = followingVideo;
            followingAudio.id = "01K20000000000000000000034";
            const RationalTime syncStart =
                crossingSyncClip ? RationalTime{0, 25} : RationalTime{10, 25};
            DocumentClip synced{"01K20000000000000000000035",
                                document.sources[0].id,
                                {300, 25},
                                {10, 25},
                                syncStart};
            document.sequence.tracks = {
                {"01K20000000000000000000036",
                 "video",
                 0,
                 {video, followingVideo}},
                {syncTrackId, "video", 1, {synced}},
                {"01K20000000000000000000037",
                 "audio",
                 2,
                 {audio, followingAudio}},
            };
            return document;
        };

        {
            Document document = linkedDocument(false);
            const std::string before = document.SaveToString();
            const Ulid videoId = document.sequence.tracks[0].clips[0].id;
            const Ulid audioId = document.sequence.tracks[2].clips[0].id;
            const Ulid group = document.FindClip(videoId)->link_group_id;
            EditLog log;
            RemoveLinkedClipsOperation remove{
                group, {videoId, audioId}, {syncTrackId}, {}};
            Check(Apply(log, document, remove, "synchronized linked remove"),
                  "remove_linked_clips accepts a synchronized track");
            Check(document.sequence.tracks[0].clips[0].timeline_in ==
                          RationalTime{0, 25} &&
                      document.sequence.tracks[1].clips[0].timeline_in ==
                          RationalTime{0, 25} &&
                      document.sequence.tracks[2].clips[0].timeline_in ==
                          RationalTime{0, 25},
                  "linked removal leaves all three downstream tracks "
                  "aligned");
            const std::string json =
                SerializeOperation(log.AppliedEntries().back().op);
            Operation parsed = RemoveClipOperation{};
            EditError error = EditError::None;
            std::string message;
            Check(
                DeserializeOperation(json, parsed, error, message) &&
                    std::get<RemoveLinkedClipsOperation>(parsed)
                            .sync_track_ids == std::vector<Ulid>{syncTrackId} &&
                    SerializeOperation(parsed) == json,
                "RemoveLinkedClips sync_track_ids round-trip canonically");
            Check(log.Undo(document, error, message) &&
                      document.SaveToString() == before,
                  "linked synchronized removal undo restores exact bytes");
        }

        {
            Document document = linkedDocument(true);
            const std::string before = document.SaveToString();
            const Ulid videoId = document.sequence.tracks[0].clips[0].id;
            const Ulid audioId = document.sequence.tracks[2].clips[0].id;
            const Ulid group = document.FindClip(videoId)->link_group_id;
            EditLog log;
            SplitLinkedClipsOperation split{
                group, {videoId, audioId}, {5, 25}, {}, {},
                {},    {syncTrackId},      {}};
            Check(Apply(log, document, split, "synchronized linked split"),
                  "split_linked_clips accepts a synchronized track");
            Check(document.sequence.tracks[0].clips.size() == 3 &&
                      document.sequence.tracks[1].clips.size() == 2 &&
                      document.sequence.tracks[2].clips.size() == 3,
                  "linked split cuts the crossing clip on the synchronized "
                  "track too");
            const std::string after = document.SaveToString();
            const std::string json =
                SerializeOperation(log.AppliedEntries().back().op);
            Operation parsed = RemoveClipOperation{};
            EditError error = EditError::None;
            std::string message;
            Check(
                DeserializeOperation(json, parsed, error, message) &&
                    std::get<SplitLinkedClipsOperation>(parsed)
                            .sync_track_ids == std::vector<Ulid>{syncTrackId} &&
                    SerializeOperation(parsed) == json,
                "SplitLinkedClips sync_track_ids round-trip canonically");
            Check(log.Undo(document, error, message) &&
                      document.SaveToString() == before,
                  "linked synchronized split undo restores exact bytes");
            Check(log.Redo(document, error, message) &&
                      document.SaveToString() == after,
                  "linked synchronized split redo restores generated IDs");
        }
    });

    Test("roll edit moves one contiguous cut without changing total duration",
         [] {
             Document document = EditDocument();
             document.sequence.tracks[0].clips[1].timeline_in = {10, 25};
             document.sequence.tracks[0].clips[2].timeline_in = {30, 25};
             const std::string before = document.SaveToString();
             const RationalTime durationBefore = Timeline(document).Duration();
             EditLog log;
             RollEditOperation roll{{{document.sequence.tracks[0].clips[0].id,
                                      document.sequence.tracks[0].clips[1].id}},
                                    {2, 25},
                                    {}};
             Check(Apply(log, document, roll, "roll edit"),
                   "roll edit applies");
             const DocumentClip& left = document.sequence.tracks[0].clips[0];
             const DocumentClip& right = document.sequence.tracks[0].clips[1];
             Check(left.duration == RationalTime{12, 25} &&
                       right.timeline_in == RationalTime{12, 25} &&
                       right.source_in == RationalTime{202, 25} &&
                       right.duration == RationalTime{8, 25} &&
                       Timeline(document).Duration() == durationBefore,
                   "roll preserves the program duration and clip contiguity");
             const std::string serialized =
                 SerializeOperation(log.AppliedEntries().back().op);
             Operation parsed = RemoveTrackOperation{};
             EditError error = EditError::None;
             std::string message;
             Check(DeserializeOperation(serialized, parsed, error, message) &&
                       SerializeOperation(parsed) == serialized,
                   "RollEdit JSON round-trips exactly");
             Check(log.Undo(document, error, message) &&
                       document.SaveToString() == before,
                   "one undo restores both sides of the roll edit");
         });

    Test("roll edit refuses a source range outside the real rush bounds", [] {
        Document document = EditDocument();
        document.sources.push_back(
            {"01K20000000000000000000022", "short.MP4", {25, 1}, {110, 25}});
        document.sequence.tracks[0].clips[0].source_id =
            "01K20000000000000000000022";
        document.sequence.tracks[0].clips[1].timeline_in = {10, 25};
        const std::string before = document.SaveToString();
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        const RollEditOperation operation{
            {{document.sequence.tracks[0].clips[0].id,
              document.sequence.tracks[0].clips[1].id}},
            {1, 25},
            {}};
        Check(!log.Apply(document, operation, error, message) &&
                  error == EditError::InvalidOperation,
              "roll beyond the rush is InvalidOperation: " + message);
        Check(
            message.find("within [0/25, 110/25)") != std::string::npos &&
                message.find("requested [100/25, 111/25)") != std::string::npos,
            "roll reports the real source bounds and request: " + message);
        Check(log.AppliedCount() == 0 && document.SaveToString() == before,
              "rejected roll edit is never journaled");
    });

    Test("slip edit changes only the source window and is reversible", [] {
        Document document = EditDocument();
        const std::string before = document.SaveToString();
        const Ulid clipId = document.sequence.tracks[0].clips[0].id;
        EditLog log;
        SlipEditOperation slip{{clipId}, {5, 25}, {}};
        Check(Apply(log, document, slip, "slip edit"), "slip edit applies");
        const DocumentClip& clip = document.sequence.tracks[0].clips[0];
        Check(clip.source_in == RationalTime{105, 25} &&
                  clip.duration == RationalTime{10, 25} &&
                  clip.timeline_in == RationalTime{0, 25},
              "slip advances source without moving or resizing the clip");
        const std::string serialized =
            SerializeOperation(log.AppliedEntries().back().op);
        Operation parsed = RemoveTrackOperation{};
        EditError error = EditError::None;
        std::string message;
        Check(DeserializeOperation(serialized, parsed, error, message) &&
                  SerializeOperation(parsed) == serialized,
              "SlipEdit JSON round-trips exactly");
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == before,
              "slip undo restores the document byte-exactly");

        document.sequence.tracks[0].locked = true;
        ExpectRejected(document, slip, EditError::LockedTrack,
                       "slip rejects a locked track");
    });

    Test("an interval subdivides a clip in one exact, reversible gesture", [] {
        Document document = EditDocument();
        const Ulid clipId = document.sequence.tracks[0].clips[0].id;
        const DocumentClip* clip = document.FindClip(clipId);
        const RationalTime start = clip->timeline_in;
        const RationalTime end = start.add(clip->duration);
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        const std::string before = document.SaveToString();

        // Stated as a duration, not as a frame count the caller worked out.
        // That is the whole point of the resolver.
        SplitClipAtPositionsOperation op;
        Check(ResolveIntervalSplits(document, clipId, {2, 25}, op, message),
              "resolving an interval succeeds: " + message);
        Check(!op.positions.empty(), "the resolver produced cut positions");
        bool ascending = true;
        for (size_t index = 0; index < op.positions.size(); ++index) {
            if (op.positions[index] <= start || end <= op.positions[index])
                ascending = false;
            if (index && op.positions[index] <= op.positions[index - 1])
                ascending = false;
        }
        Check(ascending,
              "resolved positions are strictly increasing and inside the clip");

        // An interval whose rate differs from the clip's: 1/5s against a
        // 25fps clip. Nobody converted anything by hand, and what comes out
        // is exactly one interval past the clip's start -- this is the
        // off-by-one-per-cut that principle 7 exists to prevent. Checked
        // before the document is subdivided, while clipId still names the
        // whole clip.
        SplitClipAtPositionsOperation crossRate;
        Check(ResolveIntervalSplits(document, clipId, {1, 5}, crossRate,
                                    message) &&
                  crossRate.positions.size() == 1 &&
                  crossRate.positions[0].compare(start.add({1, 5})) == 0,
              "an interval stated in another timebase resolves exactly");

        const size_t clipsBefore = document.sequence.tracks[0].clips.size();
        Check(Apply(log, document, op, "subdivide at one-second interval"),
              "the interval split applies");
        Check(document.sequence.tracks[0].clips.size() ==
                  clipsBefore + op.positions.size(),
              "each resolved position produced exactly one more clip");
        const std::string after = document.SaveToString();

        const std::string json =
            SerializeOperation(log.AppliedEntries().back().op);
        Operation parsed = RemoveClipOperation{};
        Check(DeserializeOperation(json, parsed, error, message) &&
                  SerializeOperation(parsed) == json,
              "SplitClipAtPositions JSON round-trips canonically");

        // The reason this is one operation and not N: one undo puts the
        // whole subdivision back.
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == before,
              "one undo reverses every cut of the gesture, byte-exactly");
        Check(log.Redo(document, error, message) &&
                  document.SaveToString() == after,
              "redo reproduces the subdivision byte-exactly");

        // Rejections are checked against a pristine document: `document` has
        // been subdivided by now, so its clipId names a much shorter first
        // piece and would fail these for the wrong reason.
        Document fresh = EditDocument();
        std::string resolveError;
        SplitClipAtPositionsOperation rejected;
        Check(!ResolveIntervalSplits(fresh, clipId, {0, 1}, rejected,
                                     resolveError),
              "a zero interval is refused rather than looping forever");
        Check(!ResolveIntervalSplits(fresh, clipId, {9999, 1}, rejected,
                                     resolveError),
              "an interval longer than the clip is refused, not a silent "
              "no-op");
        Check(!ResolveIntervalSplits(fresh, "01K20000000000000000000099",
                                     {1, 1}, rejected, resolveError),
              "an unknown clip is refused");

        ExpectRejected(fresh, SplitClipAtPositionsOperation{clipId, {}, {}},
                       EditError::InvalidOperation,
                       "an interval split with no positions");
        ExpectRejected(
            fresh,
            SplitClipAtPositionsOperation{
                clipId, {start.add({4, 25}), start.add({2, 25})}, {}},
            EditError::InvalidOperation,
            "positions that are not strictly increasing");
        ExpectRejected(fresh, SplitClipAtPositionsOperation{clipId, {end}, {}},
                       EditError::InvalidTimelineIn,
                       "a position on the clip's own edge");
    });

    Test("splitting a clip carries its grade and caption onto both halves", [] {
        Document document = EditDocument();
        const Ulid clipId = document.sequence.tracks[0].clips[0].id;
        EditLog log;
        EditError error = EditError::None;
        std::string message;

        Check(Apply(log, document,
                    SetClipEffectsOperation{
                        clipId,
                        {{{}, "color.exposure", {{"amount", {35, 100}}}}}},
                    "grade the clip"),
              "grading the clip to be split succeeds");
        const DocumentClip* before = document.FindClip(clipId);
        const RationalTime cut = before->timeline_in.add(
            {before->duration.value / 2, before->duration.rate});
        const std::string graded = document.SaveToString();

        SplitClipOperation split{clipId, cut};
        Check(Apply(log, document, split, "split the graded clip"),
              "splitting a graded clip succeeds");
        const Ulid rightId =
            std::get<SplitClipOperation>(log.AppliedEntries().back().op)
                .right_clip_id;
        const DocumentClip* left = document.FindClip(clipId);
        const DocumentClip* right = document.FindClip(rightId);
        // The regression this pins: the right half used to be built
        // from an explicit field list, so anything added to
        // DocumentClip later -- effects, captions -- was dropped on
        // every piece after the first cut, silently.
        Check(right && right->effects.size() == 1 &&
                  left->effects.size() == 1 &&
                  right->effects[0].type == "color.exposure" &&
                  right->effects[0].params.at("amount").num == 35 &&
                  right->effects[0].params.at("amount").den == 100,
              "the right half of a split keeps the grade");
        // Same grade, its own effect IDs: a ClipEffect id is unique
        // document-wide, so cloning the stack verbatim would produce a
        // document that fails validation.
        Check(right && right->effects[0].id != left->effects[0].id,
              "the copied grade gets its own effect IDs");
        Check(right && right->include_audio == left->include_audio &&
                  right->link_group_id == left->link_group_id &&
                  right->sync_anchor_clip_id == left->sync_anchor_clip_id,
              "the right half of a split keeps its link and sync identity");
        const std::string afterSplit = document.SaveToString();
        const std::string splitJson =
            SerializeOperation(log.AppliedEntries().back().op);
        Operation reparsed = RemoveClipOperation{};
        Check(DeserializeOperation(splitJson, reparsed, error, message) &&
                  SerializeOperation(reparsed) == splitJson,
              "SplitClip JSON round-trips canonically with its effect IDs");
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == graded,
              "undoing the split restores the graded clip byte-exactly");
        // The reason right_effect_ids is stored rather than generated
        // per application: without it, redo would mint fresh ULIDs and
        // land on different bytes every time.
        Check(log.Redo(document, error, message) &&
                  document.SaveToString() == afterSplit,
              "redoing the split reproduces the same effect IDs");

        // A log written before splits carried the grade has no
        // right_effect_ids at all. Every project with a cut in its
        // history contains these, so refusing them would make undo and
        // redo fail on real projects rather than on a fixture.
        Operation legacy = RemoveClipOperation{};
        const std::string legacyJson =
            "{\"type\":\"SplitClip\",\"clip_id\":\"" + clipId +
            "\",\"timeline_position\":{\"value\":" + std::to_string(cut.value) +
            ",\"rate\":" + std::to_string(cut.rate) +
            "},\"right_clip_id\":\"01K20000000000000000000098\"}";
        Check(DeserializeOperation(legacyJson, legacy, error, message) &&
                  std::get<SplitClipOperation>(legacy).right_effect_ids.empty(),
              "a SplitClip logged without right_effect_ids still parses");
    });

    Test(
        "clip color effects are addressable, serializable and byte-exactly "
        "reversible",
        [] {
            Document document = EditDocument();
            const Ulid clipId = document.sequence.tracks[0].clips[0].id;
            const std::string original = document.SaveToString();
            EditLog log;
            EditError error = EditError::None;
            std::string message;

            SetClipEffectsOperation setEffects{
                clipId,
                {{{}, "color.exposure", {{"amount", {35, 100}}}},
                 {{}, "color.contrast", {{"amount", {10, 100}}}}}};
            Check(Apply(log, document, setEffects, "set clip effects"),
                  "clip effects apply succeeds");
            const auto& stored = std::get<SetClipEffectsOperation>(
                log.AppliedEntries().back().op);
            Check(stored.effects.size() == 2 &&
                      IsValidUlid(stored.effects[0].id) &&
                      IsValidUlid(stored.effects[1].id),
                  "SetClipEffects fills in stable ULIDs for new effects");
            const std::string withEffects = document.SaveToString();
            const std::string effectsJson = SerializeOperation(stored);
            Operation parsed = RemoveClipOperation{};
            Check(DeserializeOperation(effectsJson, parsed, error, message) &&
                      SerializeOperation(parsed) == effectsJson,
                  "SetClipEffects JSON round-trips canonically");
            Check(log.Undo(document, error, message) &&
                      document.SaveToString() == original,
                  "SetClipEffects undo restores original bytes");
            Check(log.Redo(document, error, message) &&
                      document.SaveToString() == withEffects,
                  "SetClipEffects redo restores the effect stack and bytes");
            Check(log.Undo(document, error, message) &&
                      document.SaveToString() == original,
                  "second SetClipEffects undo is byte-exact");

            ExpectRejected(
                document,
                SetClipEffectsOperation{"01K20000000000000000000099", {}},
                EditError::UnknownClip, "unknown clip_id for SetClipEffects");
            ExpectRejected(
                document,
                SetClipEffectsOperation{clipId, {{{}, "brightness", {}}}},
                EditError::ValidationFailed,
                "effect type outside the color.* family");

            document.sequence.tracks[0].locked = true;
            ExpectRejected(document, setEffects, EditError::LockedTrack,
                           "clip effects reject a locked track");
        });

    Test("clip opacity is exact, addressable and byte-exactly reversible", [] {
        Document document = EditDocument();
        const Ulid clipId = document.sequence.tracks[0].clips[0].id;
        const std::string original = document.SaveToString();
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        Check(Apply(log, document, SetClipOpacityOperation{clipId, {425, 1000}},
                    "set clip opacity"),
              "clip opacity applies");
        const std::string changed = document.SaveToString();
        Check(document.FindClip(clipId)->opacity.num == 425,
              "exact opacity is stored on the addressed clip");
        const std::string json =
            SerializeOperation(log.AppliedEntries().back().op);
        Operation parsed = RemoveClipOperation{};
        Check(DeserializeOperation(json, parsed, error, message) &&
                  SerializeOperation(parsed) == json,
              "SetClipOpacity JSON round-trips canonically");
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == original,
              "SetClipOpacity undo restores byte-identical JSON");
        Check(log.Redo(document, error, message) &&
                  document.SaveToString() == changed,
              "SetClipOpacity redo restores byte-identical JSON");
        ExpectRejected(document, SetClipOpacityOperation{clipId, {11, 10}},
                       EditError::ValidationFailed,
                       "opacity above one is rejected");
        document.sequence.tracks[0].locked = true;
        ExpectRejected(document, SetClipOpacityOperation{clipId, {1, 2}},
                       EditError::LockedTrack,
                       "clip opacity rejects a locked track");
    });

    Test(
        "clip audio settings are exact, serializable and byte-exactly "
        "reversible",
        [] {
            Document document = EditDocument();
            document.sequence.tracks.push_back({"01K20000000000000000000090",
                                                "audio",
                                                1,
                                                {{"01K20000000000000000000091",
                                                  "01K20000000000000000000001",
                                                  {100, 25},
                                                  {10, 25},
                                                  {0, 25}}}});
            const Ulid clipId = document.sequence.tracks.back().clips[0].id;
            const std::string original = document.SaveToString();
            EditLog log;
            EditError error = EditError::None;
            std::string message;
            Check(
                Apply(log, document,
                      SetClipAudioOperation{clipId, {10, 1}, {2, 25}, {3, 25}},
                      "set clip audio"),
                "clip audio settings apply");
            const std::string changed = document.SaveToString();
            const DocumentClip* clip = document.FindClip(clipId);
            Check(clip && clip->audio_gain_db.num == 10 &&
                      clip->audio_fade_in == RationalTime{2, 25} &&
                      clip->audio_fade_out == RationalTime{3, 25},
                  "audio gain and exact fade durations are stored");
            const std::string json =
                SerializeOperation(log.AppliedEntries().back().op);
            Operation parsed = RemoveClipOperation{};
            Check(DeserializeOperation(json, parsed, error, message) &&
                      SerializeOperation(parsed) == json,
                  "SetClipAudio JSON round-trips canonically");
            Check(log.Undo(document, error, message) &&
                      document.SaveToString() == original,
                  "SetClipAudio undo restores byte-identical JSON");
            Check(log.Redo(document, error, message) &&
                      document.SaveToString() == changed,
                  "SetClipAudio redo restores byte-identical JSON");
            ExpectRejected(
                document,
                SetClipAudioOperation{clipId, {49, 1}, {0, 1}, {0, 1}},
                EditError::ValidationFailed,
                "gain above the supported range is rejected");
            ExpectRejected(
                document,
                SetClipAudioOperation{clipId, {0, 1}, {11, 25}, {0, 1}},
                EditError::ValidationFailed,
                "fade longer than clip is rejected");
            const Ulid videoClip = document.sequence.tracks[0].clips[0].id;
            ExpectRejected(
                document,
                SetClipAudioOperation{videoClip, {0, 1}, {0, 1}, {0, 1}},
                EditError::InvalidOperation,
                "audio settings reject a video clip");
            document.sequence.tracks.back().locked = true;
            ExpectRejected(
                document, SetClipAudioOperation{clipId, {1, 1}, {0, 1}, {0, 1}},
                EditError::LockedTrack,
                "clip audio settings reject a locked track");
        });

    Test(
        "caption styles and per-clip captions are addressable, "
        "serializable and byte-exactly reversible",
        [] {
            Document document = EditDocument();
            const Ulid clipId = document.sequence.tracks[0].clips[0].id;
            const std::string original = document.SaveToString();
            EditLog log;
            EditError error = EditError::None;
            std::string message;

            AddCaptionStyleOperation addStyle{
                {{}, "Inter", 64, "#ffffff", "bottom"}, -1};
            Check(Apply(log, document, addStyle, "add caption style"),
                  "caption style addition succeeds");
            const auto& storedAdd = std::get<AddCaptionStyleOperation>(
                log.AppliedEntries().back().op);
            const Ulid styleId = storedAdd.style.id;
            Check(IsValidUlid(styleId) && document.FindCaptionStyle(styleId),
                  "AddCaptionStyle receives and retains a stable ULID");
            Check(storedAdd.insertion_index == 0,
                  "AddCaptionStyle records its exact canonical position");
            const std::string withStyle = document.SaveToString();
            const std::string addJson = SerializeOperation(storedAdd);
            Operation parsed = RemoveClipOperation{};
            Check(DeserializeOperation(addJson, parsed, error, message) &&
                      SerializeOperation(parsed) == addJson,
                  "AddCaptionStyle JSON round-trips canonically");

            SetClipCaptionOperation setCaption{clipId, styleId,
                                               "Bonjour le monde"};
            Check(Apply(log, document, setCaption, "set clip caption"),
                  "clip caption assignment succeeds");
            const std::string withCaption = document.SaveToString();
            const std::string setJson =
                SerializeOperation(log.AppliedEntries().back().op);
            Check(DeserializeOperation(setJson, parsed, error, message) &&
                      SerializeOperation(parsed) == setJson,
                  "SetClipCaption JSON round-trips canonically");
            Check(document.FindClip(clipId)->caption_text == "Bonjour le monde",
                  "SetClipCaption keeps the exact per-clip text");

            Check(log.Undo(document, error, message) &&
                      document.SaveToString() == withStyle,
                  "SetClipCaption undo restores the pre-caption bytes");
            Check(log.Redo(document, error, message) &&
                      document.SaveToString() == withCaption,
                  "SetClipCaption redo restores caption bytes");
            Check(log.Undo(document, error, message) &&
                      document.SaveToString() == withStyle,
                  "second SetClipCaption undo is byte-exact");

            ExpectRejected(
                document,
                RemoveCaptionStyleOperation{"01K87000000000000000000099"},
                EditError::UnknownCaptionStyle, "unknown caption style_id");
            ExpectRejected(document,
                           SetClipCaptionOperation{"01K20000000000000000000099",
                                                   styleId, "x"},
                           EditError::UnknownClip,
                           "unknown clip_id for SetClipCaption");

            Check(log.Undo(document, error, message) &&
                      document.SaveToString() == original,
                  "AddCaptionStyle undo restores original bytes");

            document.sequence.tracks[0].locked = true;
            ExpectRejected(document, SetClipCaptionOperation{clipId, "", ""},
                           EditError::LockedTrack,
                           "clip caption rejects a locked track");
        });

    Test(
        "multicam group operations are addressable, serializable and "
        "byte-exactly reversible",
        [] {
            Document document = EditDocument();
            const Ulid clipA = document.sequence.tracks[0].clips[0].id;
            const Ulid clipB = document.sequence.tracks[0].clips[1].id;
            const std::string original = document.SaveToString();
            EditLog log;
            EditError error = EditError::None;
            std::string message;

            AddMulticamGroupOperation addGroup{
                {{},
                 "Interview",
                 {{{}, "Wide", clipA}, {{}, "Close", clipB}},
                 {}},
                -1};
            Check(Apply(log, document, addGroup, "add multicam group"),
                  "multicam group addition succeeds");
            const auto& storedAdd = std::get<AddMulticamGroupOperation>(
                log.AppliedEntries().back().op);
            const Ulid groupId = storedAdd.group.id;
            const Ulid wideAngleId = storedAdd.group.angles[0].id;
            const Ulid closeAngleId = storedAdd.group.angles[1].id;
            Check(IsValidUlid(groupId) && document.FindMulticamGroup(groupId),
                  "AddMulticamGroup receives and retains a stable ULID");
            Check(IsValidUlid(wideAngleId) && IsValidUlid(closeAngleId) &&
                      wideAngleId != closeAngleId,
                  "AddMulticamGroup assigns stable ULIDs to each angle");
            Check(storedAdd.insertion_index == 0,
                  "AddMulticamGroup records its exact canonical position");
            Check(document.FindMulticamGroup(groupId)->angles.size() == 2 &&
                      document.FindMulticamGroup(groupId)
                          ->active_angle_id.empty(),
                  "AddMulticamGroup starts with no active angle selected");
            const std::string withGroup = document.SaveToString();
            const std::string addJson = SerializeOperation(storedAdd);
            Operation parsed = RemoveClipOperation{};
            Check(DeserializeOperation(addJson, parsed, error, message) &&
                      SerializeOperation(parsed) == addJson,
                  "AddMulticamGroup JSON round-trips canonically");

            SetMulticamActiveAngleOperation setActive{groupId, wideAngleId};
            Check(Apply(log, document, setActive, "set active angle"),
                  "active angle change succeeds");
            const std::string withActive = document.SaveToString();
            const std::string setJson =
                SerializeOperation(log.AppliedEntries().back().op);
            Check(DeserializeOperation(setJson, parsed, error, message) &&
                      SerializeOperation(parsed) == setJson,
                  "SetMulticamActiveAngle JSON round-trips canonically");
            Check(document.FindMulticamGroup(groupId)->active_angle_id ==
                      wideAngleId,
                  "SetMulticamActiveAngle keeps the exact selected angle");

            Check(log.Undo(document, error, message) &&
                      document.SaveToString() == withGroup,
                  "SetMulticamActiveAngle undo restores the pre-selection "
                  "bytes");
            Check(log.Redo(document, error, message) &&
                      document.SaveToString() == withActive,
                  "SetMulticamActiveAngle redo restores the selection bytes");
            Check(log.Undo(document, error, message) &&
                      document.SaveToString() == withGroup,
                  "second SetMulticamActiveAngle undo is byte-exact");

            ExpectRejected(
                document,
                SetMulticamActiveAngleOperation{"01K88000000000000000000001",
                                                wideAngleId},
                EditError::UnknownMulticamGroup,
                "unknown multicam group_id for SetMulticamActiveAngle");
            ExpectRejected(document,
                           SetMulticamActiveAngleOperation{
                               groupId, "01K88000000000000000000002"},
                           EditError::UnknownMulticamAngle,
                           "active_angle_id outside the group's own angles");
            ExpectRejected(
                document,
                AddMulticamGroupOperation{
                    {groupId, "Duplicate", {{{}, "Wide", clipA}}, {}}, -1},
                EditError::DuplicateId,
                "AddMulticamGroup rejects a group_id already in use");
            ExpectRejected(
                document,
                AddMulticamGroupOperation{
                    {{},
                     "Bad angle",
                     {{{}, "Wide", "01K20000000000000000000099"}},
                     {}},
                    -1},
                EditError::ValidationFailed,
                "AddMulticamGroup angle clip_id must resolve to a video "
                "clip");
            ExpectRejected(
                document,
                AddMulticamGroupOperation{
                    {{},
                     "Same clip twice",
                     {{{}, "Wide", clipA}, {{}, "Close", clipA}},
                     {}},
                    -1},
                EditError::ValidationFailed,
                "AddMulticamGroup rejects a clip reused within one group");
            ExpectRejected(
                document,
                RemoveMulticamGroupOperation{"01K88000000000000000000099"},
                EditError::UnknownMulticamGroup,
                "unknown multicam group_id for RemoveMulticamGroup");

            Check(log.Undo(document, error, message) &&
                      document.SaveToString() == original,
                  "AddMulticamGroup undo restores original bytes");
            Check(log.Redo(document, error, message) &&
                      document.SaveToString() == withGroup,
                  "AddMulticamGroup redo restores the group bytes");
            Check(log.Redo(document, error, message) &&
                      document.SaveToString() == withActive,
                  "AddMulticamGroup redo chain restores the selection bytes");
            Check(log.Undo(document, error, message) &&
                      log.Undo(document, error, message) &&
                      document.SaveToString() == original,
                  "full multicam undo chain is byte-exact");
        });

    if (failures) {
        std::cerr << failures << " assertion(s) failed\n";
        return 1;
    }
    std::cout << "All edit tests passed\n";
    return 0;
}
