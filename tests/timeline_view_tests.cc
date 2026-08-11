#include "TimelineView.h"

#include <algorithm>
#include <functional>
#include <iostream>
#include <string>
#include <variant>

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
        if (before == failures) std::cout << "PASS: " << name << '\n';
    } catch (const std::exception& exception) {
        ++failures;
        std::cerr << "FAIL: " << name << ": " << exception.what() << '\n';
    }
}

Document Fixture() {
    Document document;
    document.sources = {
        {"01KT0000000000000000000001", "source.mov", {10, 1}, {100, 10}},
    };
    document.sequence.tracks = {
        {"01KT0000000000000000000002",
         "video",
         0,
         {{"01KT0000000000000000000003",
           "01KT0000000000000000000001",
           {10, 10},
           {20, 10},
           {10, 10}},
          {"01KT0000000000000000000004",
           "01KT0000000000000000000001",
           {40, 10},
           {10, 10},
           {40, 10}}}},
    };
    return document;
}

TimelineViewport Viewport() {
    TimelineViewport viewport;
    viewport.view_start = {0, 10};
    viewport.pixels_per_second = 100.0;
    viewport.track_height = 40.0;
    viewport.header_width = 50.0;
    return viewport;
}

uint64_t Hash(const std::string& bytes) {
    uint64_t hash = UINT64_C(1469598103934665603);
    for (const unsigned char byte : bytes) {
        hash ^= byte;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

}  // namespace

int main() {
    Test("In/Out clear splits boundaries and leaves a gap", [] {
        Document document = Fixture();
        const std::string original = document.SaveToString();
        auto operation = TimelineRangeDeleteOperation(
            document, RationalTime{15, 10}, RationalTime{25, 10}, false);
        Check(operation.has_value(), "clear range produces one exact edit");
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        Check(operation &&
                  log.Apply(document, Operation{*operation}, error, message),
              "clear range applies: " + message);
        const auto& clips = document.sequence.tracks[0].clips;
        Check(clips.size() == 3 &&
                  clips[0].timeline_in == RationalTime{10, 10} &&
                  clips[0].duration == RationalTime{5, 10} &&
                  clips[1].timeline_in == RationalTime{25, 10} &&
                  clips[1].source_in == RationalTime{25, 10} &&
                  clips[1].duration == RationalTime{5, 10} &&
                  clips[2].timeline_in == RationalTime{40, 10},
              "clear retains exact left/right survivors around the gap");
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == original,
              "clear range undoes byte-exactly");
    });

    Test("In/Out ripple delete closes the range on unlocked tracks", [] {
        Document document = Fixture();
        DocumentTrack locked = document.sequence.tracks[0];
        locked.id = "01KT0000000000000000000008";
        locked.index = 1;
        locked.locked = true;
        for (DocumentClip& clip : locked.clips) clip.id = GenerateUlid();
        document.sequence.tracks.push_back(locked);
        auto operation = TimelineRangeDeleteOperation(
            document, RationalTime{15, 10}, RationalTime{25, 10}, true);
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        Check(operation &&
                  log.Apply(document, Operation{*operation}, error, message),
              "ripple range applies: " + message);
        const auto& clips = document.sequence.tracks[0].clips;
        Check(clips.size() == 3 &&
                  clips[0].timeline_in == RationalTime{10, 10} &&
                  clips[0].duration == RationalTime{5, 10} &&
                  clips[1].timeline_in == RationalTime{15, 10} &&
                  clips[1].duration == RationalTime{5, 10} &&
                  clips[2].timeline_in == RationalTime{30, 10},
              "ripple removes ten ticks and closes both boundaries");
        Check(document.sequence.tracks[1].clips[0].timeline_in ==
                      RationalTime{10, 10} &&
                  document.sequence.tracks[1].clips[0].duration ==
                      RationalTime{20, 10} &&
                  document.sequence.tracks[1].clips[1].timeline_in ==
                      RationalTime{40, 10},
              "locked track remains byte-for-byte timed as before");
    });

    Test("range edits obey targeting and independent sync lock", [] {
        Document document = Fixture();
        DocumentTrack follower = document.sequence.tracks[0];
        follower.id = "01KT0000000000000000000008";
        follower.kind = "audio";
        follower.index = 1;
        follower.sync_lock = false;
        for (DocumentClip& clip : follower.clips) clip.id = GenerateUlid();
        document.sequence.tracks.push_back(follower);
        const Ulid target = document.sequence.tracks[0].id;

        auto clear =
            TimelineRangeDeleteOperation(document, RationalTime{15, 10},
                                         RationalTime{25, 10}, false, {target});
        Check(clear && clear->exact_track_result.size() == 1 &&
                  clear->exact_track_result.front().track_id == target,
              "clear edits only the explicitly targeted track");

        auto unsyncedRipple =
            TimelineRangeDeleteOperation(document, RationalTime{15, 10},
                                         RationalTime{25, 10}, true, {target});
        Check(unsyncedRipple && unsyncedRipple->exact_track_result.size() == 1,
              "ripple leaves an untargeted sync-unlocked track untouched");

        document.sequence.tracks[1].sync_lock = true;
        auto syncedRipple =
            TimelineRangeDeleteOperation(document, RationalTime{15, 10},
                                         RationalTime{25, 10}, true, {target});
        Check(syncedRipple && syncedRipple->exact_track_result.size() == 2,
              "ripple carries an untargeted sync-locked track");
        Check(!TimelineRangeDeleteOperation(document, RationalTime{15, 10},
                                            RationalTime{25, 10}, true, {}),
              "ripple without an editable target is a no-op");
    });

    Test("source insert opens sync-locked tracks atomically", [] {
        Document document = Fixture();
        DocumentTrack follower = document.sequence.tracks[0];
        follower.id = "01KT0000000000000000000008";
        follower.kind = "audio";
        follower.index = 1;
        for (DocumentClip& clip : follower.clips) clip.id = GenerateUlid();
        document.sequence.tracks.push_back(follower);
        const std::string original = document.SaveToString();
        auto operation = TimelineSourceEditOperation(
            document, document.sources[0].id, RationalTime{0, 10},
            RationalTime{10, 10}, RationalTime{20, 10},
            document.sequence.tracks[0].id, true);
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        Check(operation && operation->exact_track_result.size() == 2 &&
                  log.Apply(document, Operation{*operation}, error, message),
              "source insert applies to target and sync follower: " + message);
        const auto& target = document.sequence.tracks[0].clips;
        Check(target.size() == 4 &&
                  target[0].duration == RationalTime{10, 10} &&
                  target[1].source_in == RationalTime{0, 10} &&
                  target[1].timeline_in == RationalTime{20, 10} &&
                  target[2].timeline_in == RationalTime{30, 10} &&
                  target[3].timeline_in == RationalTime{50, 10},
              "insert splits the edit-point clip and opens exact duration");
        Check(document.sequence.tracks[1].clips.back().timeline_in ==
                  RationalTime{50, 10},
              "sync follower opens the same duration");
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == original,
              "source insert undoes byte-exactly");
    });

    Test("source overwrite replaces only the destination interval", [] {
        Document document = Fixture();
        auto operation = TimelineSourceEditOperation(
            document, document.sources[0].id, RationalTime{0, 10},
            RationalTime{10, 10}, RationalTime{20, 10},
            document.sequence.tracks[0].id, false);
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        Check(operation &&
                  log.Apply(document, Operation{*operation}, error, message),
              "source overwrite applies: " + message);
        const auto& clips = document.sequence.tracks[0].clips;
        Check(clips.size() == 3 && clips[0].duration == RationalTime{10, 10} &&
                  clips[1].source_in == RationalTime{0, 10} &&
                  clips[1].timeline_in == RationalTime{20, 10} &&
                  clips[2].timeline_in == RationalTime{40, 10},
              "overwrite preserves timeline duration and downstream clips");
    });

    Test("source edit patches linked audio to its explicit target", [] {
        Document document = Fixture();
        const Ulid audioTrackId = "01KT0000000000000000000008";
        document.sequence.tracks.push_back(
            {audioTrackId, "audio", 1, {}, false, false});
        const std::string original = document.SaveToString();
        auto operation = TimelineSourceEditOperation(
            document, document.sources[0].id, RationalTime{0, 10},
            RationalTime{10, 10}, RationalTime{20, 10},
            document.sequence.tracks[0].id, false, {audioTrackId});
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        Check(operation && operation->exact_track_result.size() == 2 &&
                  log.Apply(document, Operation{*operation}, error, message),
              "video/audio source overwrite applies atomically: " + message);
        const DocumentClip* video = nullptr;
        for (const DocumentClip& clip : document.sequence.tracks[0].clips)
            if (clip.timeline_in == RationalTime{20, 10}) video = &clip;
        const auto& audio = document.sequence.tracks[1].clips;
        Check(video && !video->include_audio && audio.size() == 1 &&
                  !video->link_group_id.empty() &&
                  audio[0].link_group_id == video->link_group_id &&
                  video->sync_anchor_clip_id == video->id &&
                  audio[0].sync_anchor_clip_id == video->id &&
                  audio[0].source_in == video->source_in &&
                  audio[0].duration == video->duration &&
                  audio[0].timeline_in == video->timeline_in,
              "patched audio is separate, sample-aligned, linked and synced");
        Check(video &&
                  ExpandLinkedClipSelection(document, {video->id}).size() == 2,
              "linked selection expands across the patched pair");
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == original,
              "patched source edit undoes byte-exactly");
    });

    Test("display order follows professional video/audio stacking", [] {
        Document document = Fixture();
        document.sequence.tracks.push_back(
            {"01KT0000000000000000000005", "video", 2, {}});
        document.sequence.tracks.push_back(
            {"01KT0000000000000000000006", "audio", 3, {}});
        document.sequence.tracks.push_back(
            {"01KT0000000000000000000007", "audio", 1, {}});
        const auto tracks = TimelineTracksInDisplayOrder(document);
        Check(tracks.size() == 4 && tracks[0]->kind == "video" &&
                  tracks[0]->index == 2 && tracks[1]->kind == "video" &&
                  tracks[1]->index == 0 && tracks[2]->kind == "audio" &&
                  tracks[2]->index == 1 && tracks[3]->index == 3,
              "higher video layers appear above V1 and audio starts at A1");
    });

    Test("time and pixels invert at frame boundaries", [] {
        const struct Case {
            int32_t rate;
            int64_t frameStep;
        } cases[] = {{24, 1}, {25, 1}, {30000, 1001}};
        const double zooms[] = {4.0, 37.5, 100.0, 1333.0, 4000.0};
        for (const Case value : cases) {
            for (double zoom : zooms) {
                TimelineViewport viewport;
                viewport.view_start = {value.frameStep * 7, value.rate};
                viewport.pixels_per_second = zoom;
                viewport.header_width = 71.0;
                for (int64_t frame = -5; frame <= 50; ++frame) {
                    const RationalTime time{frame * value.frameStep,
                                            value.rate};
                    const RationalTime roundTrip =
                        viewport.XToTime(viewport.TimeToX(time), value.rate);
                    Check(roundTrip == time,
                          "round trip must preserve frame at every zoom");
                }
            }
        }
    });

    Test("playhead quantizes exactly to frames or audio samples", [] {
        const MediaRate ntsc{30000, 1001};
        Check(QuantizePlayheadPosition({1500, 30000}, PlayheadResolution::Frame,
                                       ntsc) == RationalTime{1001, 30000},
              "NTSC position below half-frame rounds to the first frame");
        Check(QuantizePlayheadPosition({1502, 30000}, PlayheadResolution::Frame,
                                       ntsc) == RationalTime{2002, 30000},
              "NTSC position above half-frame rounds to the second frame");
        Check(QuantizePlayheadPosition({1, 96000}, PlayheadResolution::Sample,
                                       ntsc) == RationalTime{1, 48000},
              "half an audio sample rounds explicitly away from zero");
        Check(QuantizePlayheadPosition({1, 192000}, PlayheadResolution::Sample,
                                       ntsc) == RationalTime{0, 48000},
              "sub-half-sample position rounds to sample zero");
    });

    Test("hit test resolves clips, holes and six-point edges", [] {
        const Document document = Fixture();
        const TimelineViewport viewport = Viewport();
        const double trackY = kTimelineRulerHeight + 20.0;
        const auto inside =
            HitTestTimeline(document, viewport, 200.0, trackY, 700.0);
        Check(inside &&
                  inside->clip_id == document.sequence.tracks[0].clips[0].id &&
                  inside->edge == TimelineHitEdge::None,
              "clip body returns its ULID");
        Check(!HitTestTimeline(document, viewport, 375.0, trackY, 700.0),
              "gap returns no clip");
        const auto head =
            HitTestTimeline(document, viewport, 154.0, trackY, 700.0);
        Check(head && head->edge == TimelineHitEdge::Head,
              "point within six points of head returns head edge");
        const auto tail =
            HitTestTimeline(document, viewport, 346.0, trackY, 700.0);
        Check(tail && tail->edge == TimelineHitEdge::Tail,
              "point within six points of tail returns tail edge");
    });

    Test("bounded gaps are selectable but trailing space is not", [] {
        Document document = Fixture();
        TimelineViewport viewport = Viewport();
        const double trackY = kTimelineRulerHeight + 20.0;
        const auto gap =
            HitTestTimelineGap(document, viewport, 375.0, trackY, 700.0, 10);
        Check(gap && gap->track_id == document.sequence.tracks[0].id &&
                  gap->start == RationalTime{30, 10} &&
                  gap->duration == RationalTime{10, 10},
              "clicking the bounded hole resolves its exact range");
        EditLog log;
        TimelineInteraction interaction(document, log, viewport);
        interaction.PointerDown(375.0, trackY, 700.0, 10);
        Check(interaction.SelectedGap() && interaction.SelectedClipId().empty(),
              "pointer interaction selects the gap instead of a clip");
        interaction.PointerDown(600.0, trackY, 700.0, 10);
        Check(!interaction.SelectedGap(),
              "unbounded space after the last clip is not a deletable gap");
    });

    Test("lasso selects every intersecting clip rectangle", [] {
        Document document = Fixture();
        TimelineViewport viewport = Viewport();
        const double top = kTimelineRulerHeight + 5.0;
        const double bottom =
            kTimelineRulerHeight + viewport.track_height - 5.0;
        const auto selected = LassoHitTestTimeline(document, viewport, 140.0,
                                                   top, 460.0, bottom, 700.0);
        Check(selected.size() == 2 &&
                  selected[0] == document.sequence.tracks[0].clips[0].id &&
                  selected[1] == document.sequence.tracks[0].clips[1].id,
              "lasso intersection returns both clip ULIDs in display order");
        Check(LassoHitTestTimeline(document, viewport, 360.0, top, 440.0,
                                   bottom, 700.0)
                  .empty(),
              "lasso confined to a gap selects no clip");
        EditLog log;
        TimelineInteraction interaction(document, log, viewport);
        interaction.SelectClips(selected);
        Check(interaction.SelectedClipIds().size() == 2 &&
                  interaction.SelectedClipId().empty(),
              "multi-selection has no accidental single edit target");
        const auto rectangles =
            VisibleTimelineClips(document, viewport, 700.0,
                                 interaction.SelectedClipIds(), std::nullopt);
        Check(std::count_if(rectangles.begin(), rectangles.end(),
                            [](const TimelineClipRect& clip) {
                                return clip.selected;
                            }) == 2,
              "every lasso member receives selected rendering");
    });

    Test("deleting a gap closes one track atomically and undo restores bytes",
         [] {
             Document document = Fixture();
             const std::string initial = document.SaveToString();
             EditLog log;
             EditError error = EditError::None;
             std::string message;
             Operation operation = DeleteGapOperation{
                 document.sequence.tracks[0].id, {30, 10}, {10, 10}, {}};
             Check(log.Apply(document, std::move(operation), error, message),
                   "gap deletion applies: " + message);
             Check(log.AppliedCount() == 1 &&
                       document.sequence.tracks[0].clips[0].timeline_in ==
                           RationalTime{10, 10} &&
                       document.sequence.tracks[0].clips[1].timeline_in ==
                           RationalTime{30, 10},
                   "all following clips shift left by the exact gap duration");
             const std::string json =
                 SerializeOperation(log.AppliedEntries().back().op);
             Operation parsed = RemoveClipOperation{};
             Check(DeserializeOperation(json, parsed, error, message) &&
                       SerializeOperation(parsed) == json,
                   "DeleteGap JSON round-trips canonically");
             Check(log.Undo(document, error, message),
                   "gap deletion undo succeeds: " + message);
             Check(document.SaveToString() == initial,
                   "undo restores the canonical document byte for byte");
             Check(log.Redo(document, error, message) &&
                       document.sequence.tracks[0].clips[1].timeline_in ==
                           RationalTime{30, 10},
                   "redo closes the same gap exactly");
         });

    Test("linked gap deletion ripples video and audio together", [] {
        Document document = Fixture();
        document.sequence.tracks.push_back(
            {"01KT0000000000000000000005", "audio", 1, {}});
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        const Ulid videoId = document.sequence.tracks[0].clips[1].id;
        Operation detach = DetachAudioOperation{videoId,
                                                document.sequence.tracks[1].id,
                                                "01KT0000000000000000000006",
                                                {}};
        Check(log.Apply(document, std::move(detach), error, message),
              "linked gap fixture detaches the following audio");
        const Ulid audioId = document.sequence.tracks[1].clips.front().id;
        const std::string before = document.SaveToString();
        Operation close = DeleteGapOperation{document.sequence.tracks[0].id,
                                             {30, 10},
                                             {10, 10},
                                             {},
                                             {document.sequence.tracks[1].id}};
        Check(log.Apply(document, std::move(close), error, message),
              "linked gap deletion applies: " + message);
        Check(document.FindClip(videoId)->timeline_in == RationalTime{30, 10} &&
                  document.FindClip(audioId)->timeline_in ==
                      RationalTime{30, 10} &&
                  ClipSyncDrift(document, audioId) == RationalTime{0, 1},
              "linked image and audio ripple by the same exact duration");
        const std::string json =
            SerializeOperation(log.AppliedEntries().back().op);
        Operation parsed = RemoveClipOperation{};
        Check(DeserializeOperation(json, parsed, error, message) &&
                  SerializeOperation(parsed) == json,
              "linked DeleteGap JSON round-trips canonically");
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == before,
              "linked gap undo restores both tracks byte-exactly");
    });

    Test("a gap deletion crossing a clip is rejected without mutation", [] {
        Document document = Fixture();
        const std::string initial = document.SaveToString();
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        Operation operation = DeleteGapOperation{
            document.sequence.tracks[0].id, {20, 10}, {10, 10}, {}};
        Check(!log.Apply(document, std::move(operation), error, message) &&
                  error == EditError::InvalidOperation,
              "range intersecting a clip is rejected");
        Check(log.AppliedCount() == 0 && document.SaveToString() == initial,
              "rejected deletion leaves document bytes and event log intact");
    });

    Test("adding a video track is atomic, stable and exactly reversible", [] {
        Document document = Fixture();
        const std::string initial = document.SaveToString();
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        Operation operation = AddTrackOperation{"", "video", 1};
        Check(log.Apply(document, std::move(operation), error, message),
              "track creation applies: " + message);
        Check(document.sequence.tracks.size() == 2 &&
                  document.sequence.tracks[1].kind == "video" &&
                  document.sequence.tracks[1].index == 1 &&
                  IsValidUlid(document.sequence.tracks[1].id),
              "new video layer has a stable generated identity");
        const Ulid addedId = document.sequence.tracks[1].id;
        const std::string json =
            SerializeOperation(log.AppliedEntries().back().op);
        Operation parsed = RemoveClipOperation{};
        Check(DeserializeOperation(json, parsed, error, message) &&
                  SerializeOperation(parsed) == json,
              "AddTrack JSON round-trips canonically");
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == initial,
              "track creation undo restores exact document bytes");
        Check(log.Redo(document, error, message) &&
                  document.sequence.tracks.back().id == addedId,
              "redo retains the generated track ULID");
    });

    Test("detached audio becomes an independent exact timeline clip", [] {
        Document document = Fixture();
        document.sequence.tracks.push_back(
            {"01KT0000000000000000000005", "audio", 1, {}});
        const std::string initial = document.SaveToString();
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        const Ulid videoId = document.sequence.tracks[0].clips[0].id;
        Operation detach = DetachAudioOperation{
            videoId, document.sequence.tracks[1].id, "", {}};
        Check(log.Apply(document, std::move(detach), error, message),
              "audio detach applies: " + message);
        Check(!document.sequence.tracks[0].clips[0].include_audio &&
                  document.sequence.tracks[1].clips.size() == 1,
              "video contribution is muted and audio rectangle is created");
        const DocumentClip audio = document.sequence.tracks[1].clips[0];
        Check(IsValidUlid(audio.id) && audio.id != videoId &&
                  audio.source_id ==
                      document.sequence.tracks[0].clips[0].source_id &&
                  audio.source_in ==
                      document.sequence.tracks[0].clips[0].source_in &&
                  audio.duration ==
                      document.sequence.tracks[0].clips[0].duration &&
                  audio.timeline_in ==
                      document.sequence.tracks[0].clips[0].timeline_in,
              "detached clip retains exact source and timeline timing");
        Check(
            audio.link_group_id == audio.id &&
                document.sequence.tracks[0].clips[0].link_group_id == audio.id,
            "detached image and audio share a stable link group");
        const std::vector<Ulid> linked =
            ExpandLinkedClipSelection(document, {videoId});
        Check(linked.size() == 2 && std::find(linked.begin(), linked.end(),
                                              audio.id) != linked.end(),
              "linked selection expands from video to its audio clip");
        const auto rectangles = VisibleTimelineClips(
            document, Viewport(), 700.0, linked, std::nullopt);
        Check(std::count_if(rectangles.begin(), rectangles.end(),
                            [](const TimelineClipRect& rectangle) {
                                return rectangle.audio;
                            }) == 1,
              "audio rectangles carry their distinct semantic render role");
        const std::string json =
            SerializeOperation(log.AppliedEntries().back().op);
        Operation parsed = RemoveClipOperation{};
        Check(DeserializeOperation(json, parsed, error, message) &&
                  SerializeOperation(parsed) == json,
              "DetachAudio JSON round-trips canonically");

        Operation sampleOffset =
            MoveClipOperation{audio.id,
                              document.sequence.tracks[1].id,
                              audio.timeline_in.add({1, 48000}),
                              {}};
        Check(log.Apply(document, std::move(sampleOffset), error, message) &&
                  ClipSyncDrift(document, audio.id) == RationalTime{1, 48000},
              "sub-frame drift remains exact at audio-sample resolution");
        Check(log.Undo(document, error, message) &&
                  ClipSyncDrift(document, audio.id) == RationalTime{0, 1},
              "sample offset undo restores zero drift");
        Operation headTrim =
            TrimClipOperation{audio.id, TrimEdge::Head, {1, 10}, std::nullopt};
        Check(log.Apply(document, std::move(headTrim), error, message) &&
                  ClipSyncDrift(document, audio.id) == RationalTime{0, 1},
              "head trim changes source and timeline equally without false "
              "drift");
        Check(log.Undo(document, error, message),
              "head trim sync test undo succeeds");

        Operation move = MoveClipOperation{
            audio.id, document.sequence.tracks[1].id, {50, 10}, {}};
        Check(log.Apply(document, std::move(move), error, message),
              "detached audio moves independently: " + message);
        Check(document.sequence.tracks[1].clips[0].timeline_in ==
                      RationalTime{50, 10} &&
                  document.sequence.tracks[0].clips[0].timeline_in ==
                      RationalTime{10, 10},
              "moving audio leaves video timing unchanged");
        Check(log.Undo(document, error, message), "audio move undo succeeds");
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == initial,
              "detach undo restores exact document bytes");
        Check(log.Redo(document, error, message) &&
                  document.sequence.tracks[1].clips[0].id == audio.id,
              "detach redo retains the generated audio ULID");
        Operation unlink = SetClipLinkOperation{videoId, audio.id, "", ""};
        Check(
            log.Apply(document, std::move(unlink), error, message) &&
                document.FindClip(videoId)->link_group_id.empty() &&
                document.FindClip(audio.id)->link_group_id.empty(),
            "linked selection can be atomically removed without editing times");
        const std::string unlinkJson =
            SerializeOperation(log.AppliedEntries().back().op);
        Check(DeserializeOperation(unlinkJson, parsed, error, message) &&
                  SerializeOperation(parsed) == unlinkJson,
              "SetClipLink JSON round-trips canonically");
        Check(log.Undo(document, error, message) &&
                  document.FindClip(videoId)->link_group_id == audio.id &&
                  document.FindClip(audio.id)->link_group_id == audio.id,
              "link toggle undo restores the exact shared group");
    });

    Test("linked selection moves video and audio atomically", [] {
        Document document = Fixture();
        document.sequence.tracks.push_back(
            {"01KT0000000000000000000005", "audio", 1, {}});
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        const Ulid videoId = document.sequence.tracks[0].clips[0].id;
        Operation detach = DetachAudioOperation{videoId,
                                                document.sequence.tracks[1].id,
                                                "01KT0000000000000000000006",
                                                {}};
        Check(log.Apply(document, std::move(detach), error, message),
              "linked fixture detach applies: " + message);
        const Ulid audioId = document.sequence.tracks[1].clips[0].id;
        const std::string beforeMove = document.SaveToString();
        TimelineViewport viewport = Viewport();
        TimelineInteraction interaction(document, log, viewport);
        const double y = kTimelineRulerHeight + 20.0;
        interaction.PointerDown(200.0, y, 700.0, 10);
        interaction.PointerDrag(250.0, y, 700.0);
        Check(interaction.SelectedClipIds().size() == 2 &&
                  interaction.MovePreview() &&
                  interaction.MovePreview()->linked_moves.size() == 2 &&
                  interaction.MovePreview()->valid,
              "linked drag previews both members as one valid candidate");
        const auto pending = interaction.PendingOperation();
        Check(pending &&
                  std::holds_alternative<MoveLinkedClipsOperation>(*pending),
              "linked drag emits the dedicated atomic operation");
        Check(interaction.PointerUp(error, message),
              "linked pointer-up applies: " + message);
        Check(document.FindClip(videoId)->timeline_in == RationalTime{15, 10} &&
                  document.FindClip(audioId)->timeline_in ==
                      RationalTime{15, 10} &&
                  ClipSyncDrift(document, audioId) == RationalTime{0, 1},
              "video and audio receive the same exact delta without drift");
        const std::string operationJson =
            SerializeOperation(log.AppliedEntries().back().op);
        Operation parsed = RemoveClipOperation{};
        Check(DeserializeOperation(operationJson, parsed, error, message) &&
                  SerializeOperation(parsed) == operationJson,
              "MoveLinkedClips JSON round-trips canonically");
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == beforeMove,
              "one undo restores the pre-move document byte-exactly");
    });

    Test("linked cross-track move maps V1/A1 to V2/A2", [] {
        Document document = Fixture();
        document.sequence.tracks.push_back(
            {"01KT0000000000000000000010", "video", 1, {}});
        document.sequence.tracks.push_back(
            {"01KT0000000000000000000011", "audio", 2, {}});
        document.sequence.tracks.push_back(
            {"01KT0000000000000000000012", "audio", 3, {}});
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        const Ulid videoId = document.sequence.tracks[0].clips[0].id;
        Check(log.Apply(
                  document,
                  Operation{DetachAudioOperation{videoId,
                                                 document.sequence.tracks[2].id,
                                                 "01KT0000000000000000000013",
                                                 {}}},
                  error, message),
              "four-track linked fixture detaches audio: " + message);
        const Ulid audioId = document.sequence.tracks[2].clips[0].id;
        const Ulid videoTwo = document.sequence.tracks[1].id;
        const Ulid audioTwo = document.sequence.tracks[3].id;
        TimelineViewport viewport = Viewport();
        TimelineInteraction interaction(document, log, viewport);
        // Display order is V2, V1, A1, A2.
        const double videoOneY =
            kTimelineRulerHeight + viewport.track_height + 20.0;
        const double videoTwoY = kTimelineRulerHeight + 20.0;
        interaction.PointerDown(200.0, videoOneY, 700.0, 10);
        interaction.PointerDrag(250.0, videoTwoY, 700.0);
        const auto pending = interaction.PendingOperation();
        Check(pending &&
                  std::holds_alternative<MoveLinkedClipsOperation>(*pending),
              "V1 to V2 emits one linked move");
        if (pending) {
            const auto& move = std::get<MoveLinkedClipsOperation>(*pending);
            const auto video =
                std::find_if(move.moves.begin(), move.moves.end(),
                             [&](const LinkedClipMove& item) {
                                 return item.clip_id == videoId;
                             });
            const auto audio =
                std::find_if(move.moves.begin(), move.moves.end(),
                             [&](const LinkedClipMove& item) {
                                 return item.clip_id == audioId;
                             });
            Check(video != move.moves.end() && video->track_id == videoTwo &&
                      audio != move.moves.end() && audio->track_id == audioTwo,
                  "the same +1 ordinal maps video to V2 and audio to A2");
        }
        Check(interaction.PointerUp(error, message),
              "paired V2/A2 move applies: " + message);
        Check(document.FindTrackForClip(videoId)->id == videoTwo &&
                  document.FindTrackForClip(audioId)->id == audioTwo &&
                  ClipSyncDrift(document, audioId) == RationalTime{0, 1},
              "linked clips land on V2/A2 without sync drift");
    });

    Test("invalid linked move emits nothing and sync drift is exact", [] {
        Document document = Fixture();
        document.sequence.tracks.push_back(
            {"01KT0000000000000000000005", "audio", 1, {}});
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        const Ulid videoId = document.sequence.tracks[0].clips[0].id;
        Operation detach = DetachAudioOperation{videoId,
                                                document.sequence.tracks[1].id,
                                                "01KT0000000000000000000006",
                                                {}};
        Check(log.Apply(document, std::move(detach), error, message),
              "invalid fixture detach applies");
        const Ulid audioId = document.sequence.tracks[1].clips[0].id;
        Operation offset = MoveClipOperation{
            audioId, document.sequence.tracks[1].id, {0, 10}, {}};
        Check(log.Apply(document, std::move(offset), error, message),
              "independent audio offset applies");
        Check(ClipSyncDrift(document, audioId) == RationalTime{-10, 10},
              "audio drift reports its exact signed phase difference");
        TimelineViewport viewport = Viewport();
        viewport.pixels_per_second = 50.0;
        TimelineInteraction independent(document, log, viewport);
        independent.SetLinkedSelectionEnabled(false);
        const double audioY =
            kTimelineRulerHeight + viewport.track_height + 20.0;
        independent.PointerDown(75.0, audioY, 700.0, 10);
        independent.PointerDrag(120.0, audioY, 700.0);
        Check(independent.MovePreview() &&
                  independent.MovePreview()->timeline_in ==
                      RationalTime{10, 10} &&
                  independent.SnapGuideTime() == RationalTime{10, 10},
              "independent audio snaps exactly back to its sync reference");
        independent.CancelDrag();
        const std::string beforeInvalid = document.SaveToString();
        const size_t logCount = log.AppliedCount();
        TimelineInteraction interaction(document, log, viewport);
        const double y = kTimelineRulerHeight + 20.0;
        interaction.PointerDown(125.0, y, 700.0, 10);
        interaction.PointerDrag(50.0, y, 700.0);
        Check(interaction.MovePreview() && !interaction.MovePreview()->valid,
              "partner crossing timeline zero invalidates the whole preview");
        Check(!interaction.PointerUp(error, message) &&
                  document.SaveToString() == beforeInvalid &&
                  log.AppliedCount() == logCount,
              "invalid linked drop emits nothing and preserves exact bytes");
    });

    Test("linked trim previews and commits one atomic operation", [] {
        Document document = Fixture();
        document.sequence.tracks.push_back(
            {"01KT0000000000000000000005", "audio", 1, {}});
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        const Ulid videoId = document.sequence.tracks[0].clips[0].id;
        Operation detach = DetachAudioOperation{videoId,
                                                document.sequence.tracks[1].id,
                                                "01KT0000000000000000000006",
                                                {}};
        Check(log.Apply(document, std::move(detach), error, message),
              "linked trim fixture detaches audio");
        const Ulid audioId = document.sequence.tracks[1].clips[0].id;
        const std::string beforeTrim = document.SaveToString();
        TimelineViewport viewport = Viewport();
        TimelineInteraction interaction(document, log, viewport);
        const double videoY = kTimelineRulerHeight + 20.0;
        interaction.PointerDown(150.0, videoY, 700.0, 10);
        interaction.PointerDrag(200.0, videoY, 700.0);
        const auto pending = interaction.PendingOperation();
        Check(interaction.TrimPreview() &&
                  interaction.TrimPreview()->linked_times.size() == 2 &&
                  interaction.TrimPreview()->valid,
              "linked trim previews both A/V rectangles");
        Check(pending &&
                  std::holds_alternative<TrimLinkedClipsOperation>(*pending),
              "linked edge drag emits TrimLinkedClips");
        Check(interaction.PointerUp(error, message),
              "linked trim commits: " + message);
        Check(document.FindClip(videoId)->timeline_in == RationalTime{15, 10} &&
                  document.FindClip(audioId)->timeline_in ==
                      RationalTime{15, 10} &&
                  ClipSyncDrift(document, audioId) == RationalTime{0, 1},
              "linked head trim applies the same exact delta without drift");
        const std::string json =
            SerializeOperation(log.AppliedEntries().back().op);
        Operation parsed = RemoveClipOperation{};
        Check(DeserializeOperation(json, parsed, error, message) &&
                  SerializeOperation(parsed) == json,
              "TrimLinkedClips JSON round-trips canonically");
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == beforeTrim,
              "one undo restores the pre-trim document bytes");
    });

    Test("invalid linked trim emits nothing byte-identically", [] {
        Document document = Fixture();
        document.sequence.tracks.push_back(
            {"01KT0000000000000000000005", "audio", 1, {}});
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        Operation detach =
            DetachAudioOperation{document.sequence.tracks[0].clips[0].id,
                                 document.sequence.tracks[1].id,
                                 "01KT0000000000000000000006",
                                 {}};
        Check(log.Apply(document, std::move(detach), error, message),
              "invalid linked trim fixture detaches audio");
        document.sequence.tracks[1].clips[0].duration = {2, 10};
        const std::string before = document.SaveToString();
        const size_t logCount = log.AppliedCount();
        TimelineViewport viewport = Viewport();
        TimelineInteraction interaction(document, log, viewport);
        const double videoY = kTimelineRulerHeight + 20.0;
        interaction.PointerDown(150.0, videoY, 700.0, 10);
        interaction.PointerDrag(180.0, videoY, 700.0);
        Check(interaction.TrimPreview() && !interaction.TrimPreview()->valid,
              "one constrained linked member marks the whole preview invalid");
        Check(!interaction.PointerUp(error, message) &&
                  document.SaveToString() == before &&
                  log.AppliedCount() == logCount,
              "invalid linked trim emits no event and changes no bytes");
    });

    Test("linked delete is atomic and reversible", [] {
        Document document = Fixture();
        document.sequence.tracks.push_back(
            {"01KT0000000000000000000005", "audio", 1, {}});
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        const Ulid videoId = document.sequence.tracks[0].clips[0].id;
        Operation detach = DetachAudioOperation{videoId,
                                                document.sequence.tracks[1].id,
                                                "01KT0000000000000000000006",
                                                {}};
        Check(log.Apply(document, std::move(detach), error, message),
              "linked delete fixture detaches audio");
        const Ulid audioId = document.sequence.tracks[1].clips[0].id;
        const Ulid group = document.FindClip(videoId)->link_group_id;
        const std::string before = document.SaveToString();
        Operation remove =
            RemoveLinkedClipsOperation{group, {videoId, audioId}, {}};
        Check(log.Apply(document, std::move(remove), error, message) &&
                  !document.FindClip(videoId) && !document.FindClip(audioId),
              "linked delete removes both members in one event");
        const std::string json =
            SerializeOperation(log.AppliedEntries().back().op);
        Operation parsed = RemoveClipOperation{};
        Check(DeserializeOperation(json, parsed, error, message) &&
                  SerializeOperation(parsed) == json,
              "RemoveLinkedClips JSON round-trips canonically");
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == before,
              "linked delete undo restores exact document bytes");
        const Ulid followingVideoId = document.sequence.tracks[0].clips[1].id;
        const RationalTime followingPosition =
            document.sequence.tracks[0].clips[1].timeline_in;
        Operation clear =
            ClearLinkedClipsOperation{group, {videoId, audioId}, {}};
        Check(log.Apply(document, std::move(clear), error, message) &&
                  !document.FindClip(videoId) && !document.FindClip(audioId) &&
                  document.FindClip(followingVideoId)->timeline_in ==
                      followingPosition,
              "linked clear removes A/V and leaves following clips in place");
        const std::string clearJson =
            SerializeOperation(log.AppliedEntries().back().op);
        Check(DeserializeOperation(clearJson, parsed, error, message) &&
                  std::holds_alternative<ClearLinkedClipsOperation>(parsed) &&
                  SerializeOperation(parsed) == clearJson,
              "ClearLinkedClips JSON round-trips as its own operation");
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == before,
              "linked clear undo restores both tracks exactly");
    });

    Test("pan, cursor-centered zoom and fit preserve navigation invariants",
         [] {
             TimelineViewport viewport = Viewport();
             const double anchorX = 425.0;
             const RationalTime before = viewport.XToTime(anchorX, 1000);
             viewport.ZoomAroundX(anchorX, 2.0, 1000);
             Check(viewport.XToTime(anchorX, 1000) == before,
                   "zoom keeps the cursor time anchored");
             const RationalTime start = viewport.view_start;
             viewport.ScrollByPixels(100.0, 1000);
             Check(viewport.view_start > start,
                   "positive pan advances the visible start");
             viewport.FitDuration({200, 10}, 1000.0);
             Check(viewport.view_start == RationalTime{0, 10},
                   "fit starts at timeline zero");
             Check(viewport.TimeToX({200, 10}) <= 1000.0,
                   "fit keeps the timeline end visible");
         });

    Test("simulated trim emits the canonical operation and undo restores hash",
         [] {
             Document document = Fixture();
             const uint64_t initialHash = Hash(document.SaveToString());
             TimelineViewport viewport = Viewport();
             EditLog log;
             TimelineInteraction interaction(document, log, viewport);
             const double trackY = kTimelineRulerHeight + 20.0;
             interaction.PointerDown(350.0, trackY, 700.0, 10);
             interaction.PointerDrag(300.0, trackY, 700.0);
             const auto pending = interaction.PendingOperation();
             Check(
                 pending && std::holds_alternative<TrimClipOperation>(*pending),
                 "drag creates a TrimClip operation");
             if (pending) {
                 const auto& trim = std::get<TrimClipOperation>(*pending);
                 Check(
                     trim.clip_id == document.sequence.tracks[0].clips[0].id &&
                         trim.edge == TrimEdge::Tail &&
                         trim.delta == RationalTime{-5, 10},
                     "tail drag has the expected exact delta");
             }
             EditError error = EditError::None;
             std::string message;
             Check(interaction.PointerUp(error, message),
                   "valid drag applies through the edit log: " + message);
             Check(log.AppliedCount() == 1,
                   "one mouse gesture emits exactly one operation");
             Check(document.sequence.tracks[0].clips[0].duration ==
                       RationalTime{15, 10},
                   "document contains the trim result");
             Check(log.Undo(document, error, message),
                   "trim undo succeeds: " + message);
             Check(Hash(document.SaveToString()) == initialHash,
                   "undo restores the initial canonical document hash");
         });

    Test("ripple trim gesture shifts downstream sync-locked clips", [] {
        Document document = Fixture();
        document.sequence.tracks.push_back({"01KT0000000000000000000005",
                                            "audio",
                                            1,
                                            {{"01KT0000000000000000000006",
                                              "01KT0000000000000000000001",
                                              {40, 10},
                                              {10, 10},
                                              {40, 10}}}});
        const std::string initial = document.SaveToString();
        TimelineViewport viewport = Viewport();
        EditLog log;
        TimelineInteraction interaction(document, log, viewport);
        interaction.SetTrimMode(TimelineTrimMode::Ripple);
        const double videoY = kTimelineRulerHeight + 20.0;
        interaction.PointerDown(350.0, videoY, 700.0, 10);
        interaction.PointerDrag(400.0, videoY, 700.0);
        const auto pending = interaction.PendingOperation();
        Check(interaction.TrimPreview() && interaction.TrimPreview()->valid &&
                  interaction.TrimPreview()->linked_times.size() == 2,
              "ripple preview includes primary and sync followers");
        Check(pending && std::holds_alternative<RippleTrimOperation>(*pending),
              "ripple gesture emits its dedicated atomic operation");
        EditError error = EditError::None;
        std::string message;
        Check(interaction.PointerUp(error, message),
              "ripple trim commits: " + message);
        Check(document.sequence.tracks[0].clips[0].duration ==
                      RationalTime{25, 10} &&
                  document.sequence.tracks[0].clips[1].timeline_in ==
                      RationalTime{45, 10} &&
                  document.sequence.tracks[1].clips[0].timeline_in ==
                      RationalTime{45, 10},
              "ripple extends the edit and moves every sync follower exactly");
        Check(log.AppliedCount() == 1 && log.Undo(document, error, message) &&
                  document.SaveToString() == initial,
              "one undo restores the ripple gesture byte-exactly");
    });

    Test("roll gesture moves one cut while preserving program duration", [] {
        Document document = Fixture();
        document.sequence.tracks[0].clips[1].timeline_in = {30, 10};
        const std::string initial = document.SaveToString();
        TimelineViewport viewport = Viewport();
        EditLog log;
        TimelineInteraction interaction(document, log, viewport);
        interaction.SetTrimMode(TimelineTrimMode::Roll);
        const double videoY = kTimelineRulerHeight + 20.0;
        interaction.PointerDown(350.0, videoY, 700.0, 10);
        interaction.PointerDrag(400.0, videoY, 700.0);
        const auto pending = interaction.PendingOperation();
        Check(interaction.TrimPreview() && interaction.TrimPreview()->valid &&
                  interaction.TrimPreview()->linked_times.size() == 1,
              "roll preview updates both sides of the cut");
        Check(pending && std::holds_alternative<RollEditOperation>(*pending),
              "roll gesture emits its dedicated atomic operation");
        EditError error = EditError::None;
        std::string message;
        Check(interaction.PointerUp(error, message),
              "roll edit commits: " + message);
        const auto& clips = document.sequence.tracks[0].clips;
        Check(clips[0].duration == RationalTime{25, 10} &&
                  clips[1].source_in == RationalTime{45, 10} &&
                  clips[1].duration == RationalTime{5, 10} &&
                  clips[1].timeline_in == RationalTime{35, 10} &&
                  clips[1].timeline_in.add(clips[1].duration) ==
                      RationalTime{40, 10},
              "roll moves the cut without changing the sequence end");
        Check(log.AppliedCount() == 1 && log.Undo(document, error, message) &&
                  document.SaveToString() == initial,
              "one undo restores the roll gesture byte-exactly");
    });

    Test("slip gesture keeps linked A/V rectangles fixed", [] {
        Document document = Fixture();
        document.sequence.tracks.push_back(
            {"01KT0000000000000000000005", "audio", 1, {}});
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        const Ulid videoId = document.sequence.tracks[0].clips[0].id;
        Check(log.Apply(document,
                        DetachAudioOperation{videoId,
                                             document.sequence.tracks[1].id,
                                             "01KT0000000000000000000006",
                                             {}},
                        error, message),
              "slip fixture detaches audio");
        const Ulid audioId = document.sequence.tracks[1].clips[0].id;
        const std::string before = document.SaveToString();
        TimelineViewport viewport = Viewport();
        TimelineInteraction interaction(document, log, viewport);
        interaction.SetTrimMode(TimelineTrimMode::Slip);
        const double videoY = kTimelineRulerHeight + 20.0;
        interaction.PointerDown(200.0, videoY, 700.0, 10);
        interaction.PointerDrag(250.0, videoY, 700.0);
        const auto pending = interaction.PendingOperation();
        Check(interaction.TrimPreview() && interaction.TrimPreview()->valid &&
                  interaction.TrimPreview()->linked_times.size() == 2,
              "slip previews both separate linked rectangles");
        Check(pending && std::holds_alternative<SlipEditOperation>(*pending),
              "slip gesture emits one dedicated operation");
        Check(interaction.PointerUp(error, message),
              "linked slip commits: " + message);
        Check(
            document.FindClip(videoId)->source_in == RationalTime{15, 10} &&
                document.FindClip(audioId)->source_in == RationalTime{15, 10} &&
                document.FindClip(videoId)->timeline_in ==
                    RationalTime{10, 10} &&
                document.FindClip(audioId)->timeline_in ==
                    RationalTime{10, 10} &&
                document.FindClip(videoId)->duration == RationalTime{20, 10} &&
                document.FindClip(audioId)->duration == RationalTime{20, 10} &&
                ClipSyncDrift(document, audioId) == RationalTime{0, 1},
            "slip changes only both source windows and preserves A/V sync");
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == before,
              "one undo restores the linked slip byte-exactly");
    });

    Test("clip body drag moves atomically across tracks and undo restores hash",
         [] {
             Document document = Fixture();
             document.sequence.tracks.push_back(
                 {"01KT0000000000000000000005", "video", 1, {}});
             const std::string initial = document.SaveToString();
             TimelineViewport viewport = Viewport();
             EditLog log;
             TimelineInteraction interaction(document, log, viewport);
             const double firstTrackY = kTimelineRulerHeight + 20.0;
             const double secondTrackY =
                 kTimelineRulerHeight + viewport.track_height + 20.0;
             // V2 is displayed above V1, matching professional NLE stacking.
             interaction.PointerDown(200.0, secondTrackY, 700.0, 10);
             interaction.PointerDrag(250.0, firstTrackY, 700.0);
             Check(
                 interaction.MovePreview() && interaction.MovePreview()->valid,
                 "cross-track move preview is valid");
             const auto pending = interaction.PendingOperation();
             Check(
                 pending && std::holds_alternative<MoveClipOperation>(*pending),
                 "body drag emits MoveClip");
             if (pending) {
                 const auto& move = std::get<MoveClipOperation>(*pending);
                 Check(move.track_id == document.sequence.tracks[1].id &&
                           move.timeline_in == RationalTime{15, 10},
                       "move carries exact destination track and time");
                 const std::string json = SerializeOperation(*pending);
                 Operation parsed = RemoveClipOperation{};
                 EditError parseError = EditError::None;
                 std::string parseMessage;
                 Check(DeserializeOperation(json, parsed, parseError,
                                            parseMessage) &&
                           SerializeOperation(parsed) == json,
                       "MoveClip JSON round-trips canonically");
             }
             EditError error = EditError::None;
             std::string message;
             Check(interaction.PointerUp(error, message),
                   "move applies through one edit-log entry: " + message);
             Check(log.AppliedCount() == 1 &&
                       document.sequence.tracks[0].clips.size() == 1 &&
                       document.sequence.tracks[1].clips.size() == 1,
                   "one event transfers the clip between tracks");
             Check(log.Undo(document, error, message),
                   "move undo succeeds: " + message);
             Check(document.SaveToString() == initial,
                   "move undo restores canonical document bytes");
         });

    Test("overlapping clip move overwrites covered clips atomically", [] {
        Document document = Fixture();
        const std::string initial = document.SaveToString();
        TimelineViewport viewport = Viewport();
        EditLog log;
        TimelineInteraction interaction(document, log, viewport);
        const double trackY = kTimelineRulerHeight + 20.0;
        interaction.PointerDown(200.0, trackY, 700.0, 10);
        interaction.PointerDrag(500.0, trackY, 700.0);
        Check(interaction.MovePreview() && interaction.MovePreview()->valid,
              "overlap is accepted as an overwrite preview");
        EditError error = EditError::None;
        std::string message;
        Check(interaction.PointerUp(error, message) && log.AppliedCount() == 1,
              "overwrite emits one atomic move event");
        Check(document.sequence.tracks[0].clips.size() == 1 &&
                  document.sequence.tracks[0].clips[0].id ==
                      "01KT0000000000000000000003" &&
                  document.sequence.tracks[0].clips[0].timeline_in ==
                      RationalTime{40, 10},
              "fully covered destination clip is removed");
        Check(log.Undo(document, error, message), "overwrite undo succeeds");
        Check(document.SaveToString() == initial,
              "overwrite undo restores document bytes");
    });

    Test("overwrite through a longer clip preserves both survivors", [] {
        Document document = Fixture();
        document.sequence.tracks.push_back({"01KT0000000000000000000005",
                                            "video",
                                            1,
                                            {{"01KT0000000000000000000006",
                                              "01KT0000000000000000000001",
                                              {40, 10},
                                              {50, 10},
                                              {30, 10}}}});
        const std::string initial = document.SaveToString();
        TimelineViewport viewport = Viewport();
        EditLog log;
        TimelineInteraction interaction(document, log, viewport);
        const double firstTrackY = kTimelineRulerHeight + 20.0;
        const double secondTrackY =
            kTimelineRulerHeight + viewport.track_height + 20.0;
        interaction.PointerDown(200.0, secondTrackY, 900.0, 10);
        interaction.PointerDrag(600.0, firstTrackY, 900.0);
        EditError error = EditError::None;
        std::string message;
        Check(interaction.PointerUp(error, message),
              "spanning overwrite applies: " + message);
        const auto& clips = document.sequence.tracks[1].clips;
        Check(clips.size() == 3,
              "spanning destination becomes left, moved and right clips");
        Check(clips[0].id == "01KT0000000000000000000006" &&
                  clips[0].timeline_in == RationalTime{30, 10} &&
                  clips[0].duration == RationalTime{20, 10},
              "left survivor retains its ULID");
        Check(clips[1].id == "01KT0000000000000000000003" &&
                  clips[1].timeline_in == RationalTime{50, 10},
              "moved clip occupies the overwrite interval");
        const Ulid rightSurvivorId = clips[2].id;
        Check(rightSurvivorId != clips[0].id && IsValidUlid(rightSurvivorId) &&
                  clips[2].source_in == RationalTime{80, 10} &&
                  clips[2].timeline_in == RationalTime{70, 10} &&
                  clips[2].duration == RationalTime{10, 10},
              "right survivor gets an exact stable identity and source range");
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == initial,
              "spanning overwrite undo restores exact bytes");
        Check(log.Redo(document, error, message),
              "spanning overwrite redo succeeds");
        Check(document.sequence.tracks[1].clips[2].id == rightSurvivorId,
              "redo retains the right survivor ULID");
    });

    Test("split clip is atomic, stable and exactly reversible", [] {
        Document document = Fixture();
        const std::string initial = document.SaveToString();
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        Check(log.Apply(
                  document,
                  SplitClipOperation{
                      document.sequence.tracks[0].clips[0].id, {20, 10}, {}},
                  error, message),
              "split applies: " + message);
        const Ulid rightId =
            std::get<SplitClipOperation>(log.AppliedEntries().back().op)
                .right_clip_id;
        Check(IsValidUlid(rightId), "split generates a stable right ULID");
        Check(document.sequence.tracks[0].clips.size() == 3,
              "split adds one clip without ripple");
        const DocumentClip& left = document.sequence.tracks[0].clips[0];
        const DocumentClip& right = document.sequence.tracks[0].clips[1];
        Check(left.duration == RationalTime{10, 10} && right.id == rightId &&
                  right.source_in == RationalTime{20, 10} &&
                  right.duration == RationalTime{10, 10} &&
                  right.timeline_in == RationalTime{20, 10},
              "split preserves exact source and timeline continuity");
        const std::string operationJson =
            SerializeOperation(log.AppliedEntries().back().op);
        Operation parsed = RemoveClipOperation{};
        Check(DeserializeOperation(operationJson, parsed, error, message) &&
                  SerializeOperation(parsed) == operationJson,
              "SplitClip JSON round-trips canonically");
        Check(log.Undo(document, error, message), "split undo succeeds");
        Check(document.SaveToString() == initial,
              "split undo restores document bytes");
        Check(log.Redo(document, error, message), "split redo succeeds");
        Check(document.sequence.tracks[0].clips[1].id == rightId,
              "split redo retains the generated right ULID");
    });

    Test("linked cut creates independently selectable A/V segments", [] {
        Document document = Fixture();
        document.sequence.tracks.push_back(
            {"01KT0000000000000000000005", "audio", 1, {}});
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        const Ulid videoId = document.sequence.tracks[0].clips[0].id;
        Check(log.Apply(document,
                        DetachAudioOperation{videoId,
                                             document.sequence.tracks[1].id,
                                             "01KT0000000000000000000006",
                                             {}},
                        error, message),
              "linked cut fixture detaches audio: " + message);
        const Ulid audioId = document.sequence.tracks[1].clips[0].id;
        const std::string beforeCut = document.SaveToString();
        Operation cut = TimelineCutOperationForClip(
            document, *document.FindClip(videoId), {20, 10}, true);
        Check(std::holds_alternative<SplitLinkedClipsOperation>(cut),
              "linked selection builds one linked blade operation");
        Check(log.Apply(document, std::move(cut), error, message),
              "linked cut applies atomically: " + message);
        const auto& applied =
            std::get<SplitLinkedClipsOperation>(log.AppliedEntries().back().op);
        Check(applied.right_clip_ids.size() == 2,
              "linked cut creates one stable right ID per A/V member");
        const Ulid rightVideoId = applied.right_clip_ids[0];
        const Ulid rightAudioId = applied.right_clip_ids[1];
        const std::vector<Ulid> leftSelection =
            ExpandLinkedClipSelection(document, {videoId});
        const std::vector<Ulid> rightSelection =
            ExpandLinkedClipSelection(document, {rightVideoId});
        Check(leftSelection.size() == 2 &&
                  std::find(leftSelection.begin(), leftSelection.end(),
                            audioId) != leftSelection.end() &&
                  std::find(leftSelection.begin(), leftSelection.end(),
                            rightVideoId) == leftSelection.end(),
              "left selection contains only its linked image and sound");
        Check(rightSelection.size() == 2 &&
                  std::find(rightSelection.begin(), rightSelection.end(),
                            rightAudioId) != rightSelection.end() &&
                  std::find(rightSelection.begin(), rightSelection.end(),
                            videoId) == rightSelection.end(),
              "right selection contains only its linked image and sound");
        Check(document.FindClip(videoId)->link_group_id !=
                  document.FindClip(rightVideoId)->link_group_id,
              "the two sides receive distinct link groups");
        const std::string json = SerializeOperation(applied);
        Operation parsed = RemoveClipOperation{};
        Check(DeserializeOperation(json, parsed, error, message) &&
                  SerializeOperation(parsed) == json,
              "SplitLinkedClips JSON round-trips canonically");
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == beforeCut,
              "one undo restores the complete linked cut");
        Check(log.Redo(document, error, message) &&
                  document.FindClip(rightVideoId) &&
                  document.FindClip(rightAudioId),
              "linked cut redo retains both generated identities");
    });

    Test("split at a clip boundary is rejected atomically", [] {
        Document document = Fixture();
        const std::string initial = document.SaveToString();
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        Check(!log.Apply(
                  document,
                  SplitClipOperation{
                      document.sequence.tracks[0].clips[0].id, {10, 10}, {}},
                  error, message) &&
                  error == EditError::InvalidTimelineIn,
              "cut on head is rejected");
        Check(log.AppliedCount() == 0 && document.SaveToString() == initial,
              "rejected cut leaves bytes and event log unchanged");
    });

    Test("trim drag is clamped before duration can cross zero", [] {
        Document document = Fixture();
        const std::string initial = document.SaveToString();
        TimelineViewport viewport = Viewport();
        EditLog log;
        TimelineInteraction interaction(document, log, viewport);
        const double trackY = kTimelineRulerHeight + 20.0;
        interaction.PointerDown(150.0, trackY, 700.0, 10);
        interaction.PointerDrag(400.0, trackY, 700.0);
        Check(interaction.TrimPreview() && interaction.TrimPreview()->valid &&
                  interaction.TrimPreview()->times.duration ==
                      RationalTime{1, 10},
              "preview stops at one timebase tick of duration");
        EditError error = EditError::None;
        std::string message;
        Check(interaction.PointerUp(error, message),
              "release applies the constrained trim");
        Check(log.AppliedCount() == 1 &&
                  document.sequence.tracks[0].clips[0].duration ==
                      RationalTime{1, 10},
              "constrained trim emits one valid event");
        Check(log.Undo(document, error, message), "constrained trim undoes");
        Check(document.SaveToString() == initial,
              "undo restores bytes after constrained trim");
    });

    Test("magnetism snaps a trim to the neighboring cut", [] {
        Document document = Fixture();
        TimelineViewport viewport = Viewport();
        viewport.pixels_per_second = 50.0;
        EditLog log;
        TimelineInteraction interaction(document, log, viewport);
        const double trackY = kTimelineRulerHeight + 20.0;
        interaction.PointerDown(200.0, trackY, 700.0, 10);
        interaction.PointerDrag(243.0, trackY, 700.0);
        Check(interaction.TrimPreview() && interaction.TrimPreview()->valid &&
                  interaction.TrimPreview()->times.duration ==
                      RationalTime{30, 10},
              "tail snaps exactly to the next clip start");
        Check(interaction.SnapGuideTime() &&
                  *interaction.SnapGuideTime() == RationalTime{40, 10},
              "snap exposes an exact guide time for rendering");
        EditError error = EditError::None;
        std::string message;
        Check(interaction.PointerUp(error, message), "snapped trim applies");
        Check(document.sequence.tracks[0].clips[0].duration ==
                  RationalTime{30, 10},
              "document receives the snapped duration");
    });

    Test("tail trim cannot pass the source media end", [] {
        Document document = Fixture();
        document.sequence.tracks[0].clips.erase(
            document.sequence.tracks[0].clips.begin() + 1);
        TimelineViewport viewport = Viewport();
        EditLog log;
        TimelineInteraction interaction(document, log, viewport);
        const double trackY = kTimelineRulerHeight + 20.0;
        interaction.PointerDown(350.0, trackY, 2200.0, 10);
        interaction.PointerDrag(2000.0, trackY, 2200.0);
        Check(interaction.TrimPreview() && interaction.TrimPreview()->valid &&
                  interaction.TrimPreview()->times.duration ==
                      RationalTime{90, 10},
              "preview stops exactly at source duration");
        Check(interaction.TrimPreview()->times.source_in.add(
                  interaction.TrimPreview()->times.duration) ==
                  document.sources[0].duration,
              "preview source range ends at the media boundary");
    });

    return failures == 0 ? 0 : 1;
}
