#pragma once

// Third McpBackend implementation (ROADMAP.md F2.4), alongside
// McpProjectBackend (McpProjectBackend.h, production MCP server -- a
// project *file path*, reloaded per call) and the in-memory test backend
// (tests/mcp_tools_tests.cc, a Document the test itself owns). This one
// wraps the app's already-open, in-memory Document and EditLog *by
// reference* -- the exact objects main.mm's mouse-driven edit handlers
// call `self.state->editLog.Apply(self.state->document, ...)` against.
//
// That reference-not-copy choice is the whole point: the chat panel and a
// mouse gesture must be editing the *same* document instance, sharing one
// undo/redo stack, never a stale reload of what was last saved to disk.
// Routing the chat panel through McpProjectBackend instead (reload from the
// project file, apply, save back) would silently fork the in-memory
// document from whatever the chat tool call just wrote to disk -- exactly
// the kind of hidden second mutation path PHILOSOPHY.md principle 2 and
// this ticket's hard constraints rule out.
//
// PHILOSOPHY.md principle 3 stays satisfied the same way McpProjectBackend
// already satisfies it: ApplyOperation below calls EditLog::Apply, the
// identical function every other edit path in this project calls, and nothing
// here constructs an Operation from scratch -- McpTools.cc's dispatch
// functions still own that.
//
// AppKit-only in practice (only ChatPanelView.mm constructs one), but the
// class itself is plain C++ -- Document.h/EditLog.h/Operations.h only, no
// AppKit -- so nothing stops a future test from linking it directly.
// Describe() does pull in Cli.h's DescribeDocument, whose translation unit
// (Cli.cc) also defines ProjectStorage-dependent commands this class never
// calls; that only matters at *link* time and only for whoever links this
// class in, which today is only the macOS app target.

#include "Document.h"
#include "EditLog.h"
#include "McpBackend.h"

#include <functional>
#include <string>

class McpLiveBackend : public McpBackend {
public:
    using ProjectApplyCallback = std::function<bool(
        ProjectOperation, std::string&, std::string&, std::string&)>;
    using ProjectDescribeCallback =
        std::function<bool(std::string&, std::string&)>;
    using TranscriptCallback = std::function<bool(std::string&, std::string&)>;
    // Word-level editing needs one clip's own source transcript, which the
    // window resolves against the open project's cache directory -- the same
    // place the transcribe action writes it.
    using SourceTranscriptCallback =
        std::function<bool(const Ulid&, Transcript&, std::string&)>;
    // Picture-quality reports live beside the transcripts, in the open
    // project's cache directory, and are resolved by the window for the same
    // reason: this backend holds a Document, not a project path.
    using SourceShotQualityCallback =
        std::function<bool(const Ulid&, ShotQualityReport&, std::string&)>;
    // Producing the analysis, as opposed to reading it back. Held as a
    // callback for the same reason: this backend has a Document, and the
    // media path and cache directory belong to the open project.
    using AnalyzeShotQualityCallback =
        std::function<bool(const Ulid&, std::string&, std::string&)>;
    using CaptureFrameCallback = std::function<bool(
        const Ulid&, const RationalTime&, std::string&, std::string&)>;
    // Neither reference is owned; both must outlive this backend. `onApplied`
    // (optional) is invoked after every successful ApplyOperation/Undo/Redo,
    // before the call returns -- main.mm wires it to the same
    // -persistEdits/-refreshTimelineAfterEditFromPosition/-rebuildMediaList
    // sequence every other in-app edit handler already runs after
    // `self.state->editLog.Apply(...)` succeeds, so a chat-driven edit is
    // saved and reflected on screen exactly like a mouse-driven one. Runs on
    // whatever thread calls ApplyOperation/Undo/Redo -- see ChatPanelView.mm
    // for why that is always the main thread.
    McpLiveBackend(Document& document, EditLog& editLog,
                   std::function<void()> onApplied = nullptr,
                   ProjectApplyCallback applyProject = nullptr,
                   ProjectDescribeCallback describeProject = nullptr,
                   TranscriptCallback readTranscript = nullptr,
                   SourceTranscriptCallback readSourceTranscript = nullptr,
                   SourceShotQualityCallback readSourceShotQuality = nullptr,
                   AnalyzeShotQualityCallback analyzeShotQuality = nullptr,
                   CaptureFrameCallback captureFrame = nullptr);

    bool SnapshotDocument(Document& document, std::string& message) override;
    bool ApplyOperation(Operation operation, std::string& resultJson,
                        std::string& errorName, std::string& message) override;
    bool ApplyProjectEdit(ProjectOperation operation, std::string& resultJson,
                          std::string& errorName,
                          std::string& message) override;
    bool ReadTimelineTranscript(std::string& json,
                                std::string& message) override;
    bool ReadSourceTranscript(const Ulid& sourceId, Transcript& transcript,
                              std::string& message) override;
    bool ReadSourceShotQuality(const Ulid& sourceId, ShotQualityReport& report,
                               std::string& message) override;
    bool AnalyzeSourceShotQuality(const Ulid& sourceId, std::string& resultJson,
                                  std::string& message) override;
    bool CaptureSourceFrame(const Ulid& sourceId, const RationalTime& time,
                            std::string& jpegBytes,
                            std::string& message) override;
    bool Undo(std::string& resultJson, std::string& errorName,
              std::string& message) override;
    bool Redo(std::string& resultJson, std::string& errorName,
              std::string& message) override;
    bool Describe(std::string& json, std::string& message) override;

private:
    Document& document_;
    EditLog& edit_log_;
    std::function<void()> on_applied_;
    ProjectApplyCallback apply_project_;
    ProjectDescribeCallback describe_project_;
    TranscriptCallback read_transcript_;
    SourceTranscriptCallback read_source_transcript_;
    SourceShotQualityCallback read_source_shot_quality_;
    AnalyzeShotQualityCallback analyze_shot_quality_;
    CaptureFrameCallback capture_frame_;
};
