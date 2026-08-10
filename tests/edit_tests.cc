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
    document.tracks = {
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
    Test("insert/remove ripple positions", [] {
        Document document = EditDocument();
        EditLog log;
        InsertClipOperation insert{document.tracks[0].id,
                                   document.sources[1].id,
                                   {50, 25},
                                   {5, 25},
                                   {10, 25},
                                   {},
                                   {}};
        Check(Apply(log, document, insert, "insert"), "insert applies");
        const auto& stored =
            std::get<InsertClipOperation>(log.AppliedEntries().back().op);
        Check(document.tracks[0].clips.size() == 4, "insert adds one clip");
        Check(document.FindClip(stored.clip_id) != nullptr,
              "inserted clip is addressed by generated ULID");
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

    Test("mixed-rate ripple undo restores exact representation", [] {
        Document document;
        document.sources = {
            {"01K40000000000000000000001", "25.MP4", {25, 1}, {250, 25}},
            {"01K40000000000000000000002",
             "2997.MP4",
             {30000, 1001},
             {300300, 30000}},
        };
        document.tracks = {
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
                    InsertClipOperation{document.tracks[0].id,
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
            isolated.tracks[0].clips.erase(isolated.tracks[0].clips.begin() + 1,
                                           isolated.tracks[0].clips.end());
            EditLog log;
            Check(Apply(log, isolated,
                        TrimClipOperation{isolated.tracks[0].clips[0].id,
                                          TrimEdge::Head,
                                          {2, 25},
                                          std::nullopt},
                        "isolated head trim"),
                  "isolated head trim applies");
            const DocumentClip& clip = isolated.tracks[0].clips[0];
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
            Check(isolated.tracks[0].clips[0].duration == RationalTime{6, 25},
                  "tail trim changes duration");
        }
        {
            Document framed = EditDocument();
            const RationalTime beforePrevious =
                framed.tracks[0].clips[0].timeline_in;
            const RationalTime beforeNext =
                framed.tracks[0].clips[2].timeline_in;
            EditLog log;
            const Ulid middle = framed.tracks[0].clips[1].id;
            Check(Apply(log, framed,
                        TrimClipOperation{
                            middle, TrimEdge::Head, {2, 25}, std::nullopt},
                        "framed head trim"),
                  "framed head trim applies");
            Check(framed.tracks[0].clips[0].timeline_in == beforePrevious &&
                      framed.tracks[0].clips[2].timeline_in == beforeNext,
                  "head trim does not move surrounding clips");
            Check(Apply(log, framed,
                        TrimClipOperation{
                            middle, TrimEdge::Tail, {2, 25}, std::nullopt},
                        "framed tail trim"),
                  "framed tail trim applies inside the gap");
            Check(framed.tracks[0].clips[2].timeline_in == beforeNext,
                  "tail trim does not move following clip");
        }
    });

    Test("invalid preconditions are atomic and named", [] {
        const Document base = EditDocument();
        const InsertClipOperation validInsert{base.tracks[0].id,
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
                       TrimClipOperation{base.tracks[0].clips[0].id,
                                         TrimEdge::Tail,
                                         {-10, 25},
                                         std::nullopt},
                       EditError::InvalidDuration, "zero trim duration");
        ExpectRejected(base,
                       TrimClipOperation{base.tracks[0].clips[0].id,
                                         TrimEdge::Head,
                                         {-101, 25},
                                         std::nullopt},
                       EditError::InvalidTimelineIn,
                       "head trim before timeline zero");

        Document sourceBound = base;
        sourceBound.tracks[0].clips[0].timeline_in = {200, 25};
        sourceBound.tracks[0].clips[1].timeline_in = {220, 25};
        sourceBound.tracks[0].clips[2].timeline_in = {240, 25};
        ExpectRejected(sourceBound,
                       TrimClipOperation{sourceBound.tracks[0].clips[0].id,
                                         TrimEdge::Head,
                                         {-101, 25},
                                         std::nullopt},
                       EditError::SourceOutOfBounds,
                       "head trim before source start");

        Document tailOverlap = base;
        tailOverlap.tracks[0].clips[1].timeline_in = {11, 25};
        tailOverlap.tracks[0].clips[2].timeline_in = {40, 25};
        ExpectRejected(tailOverlap,
                       TrimClipOperation{tailOverlap.tracks[0].clips[0].id,
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
                    InsertClipOperation{document.tracks[0].id,
                                        document.sources[1].id,
                                        {10, 25},
                                        {3, 25},
                                        {10, 25},
                                        {},
                                        {}},
                    "serialized insert"),
              "insert applies");
        Check(Apply(log, document,
                    TrimClipOperation{document.tracks[0].clips[0].id,
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
                    TrimClipOperation{document.tracks[0].clips[0].id,
                                      TrimEdge::Tail,
                                      {-1, 25},
                                      std::nullopt},
                    "trim before undo"),
              "trim applies");
        Check(log.Undo(document, error, message), "undo succeeds");
        Check(log.UndoneCount() == 1, "undo populates redo stack");
        Check(Apply(log, document,
                    TrimClipOperation{document.tracks[0].clips[1].id,
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
        document.tracks[0].clips.erase(document.tracks[0].clips.begin() + 1,
                                       document.tracks[0].clips.end());
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
                        InsertClipOperation{document.tracks[0].id,
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
                    TrimClipOperation{document.tracks[0].clips[0].id,
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

    if (failures) {
        std::cerr << failures << " assertion(s) failed\n";
        return 1;
    }
    std::cout << "All edit tests passed\n";
    return 0;
}
