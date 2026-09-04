#pragma once

// Backend seam between the transport (McpServer/HttpServer) and tool
// dispatch (McpTools) on one side, and the actual edit path on the other.
//
// There are two implementations:
//  - McpProjectBackend (McpProjectBackend.h): the production backend. It
//    reuses ApplyOperationCommand/UndoOperationCommand/RedoOperationCommand/
//    DescribeCommand from Cli.cc verbatim -- the exact same load/apply/
//    commit path a `--apply-op` CLI invocation takes. It depends on
//    ProjectStorage, so (like Cli.cc's project-facing commands) it only
//    links where CommonCrypto is available (macOS).
//  - A pure in-memory backend used by tests (tests/mcp_tools_tests.cc),
//    which calls EditLog::Apply/Undo/Redo directly against a Document held
//    in memory. That is still the exact same apply path -- EditLog::Apply
//    is what ApplyOperationCommand calls -- just without the project-file
//    round trip, so it builds and runs without FFmpeg/CommonCrypto/AppKit
//    for local iteration and CI on non-macOS hosts.
//
// Either way, McpTools.cc never mutates a Document itself: every tool
// dispatch function ends by constructing an Operation and handing it to
// this interface. Errors are reported by name (the same strings
// EditErrorName() in Operations.h produces) rather than as an EditError
// enum, so both implementations -- one working from a JSON string returned
// by a Cli.cc command, the other from a live EditError -- report through
// one shape without a reverse enum<->string mapping.

#include "Document.h"
#include "FrameCapture.h"
#include "InterviewShort.h"
#include "Operations.h"
#include "ShotQuality.h"
#include "SpeechOnset.h"
#include "Transcription.h"

#include <string>
#include <vector>

class McpBackend {
public:
    virtual ~McpBackend() = default;

    // Read-only snapshot of the current document, used only to resolve
    // short IDs (IdResolver) before an operation is constructed.
    virtual bool SnapshotDocument(Document& document, std::string& message) = 0;

    // B9 -- scopes one timeline-editing tool call. The default backend only
    // owns one document, so it accepts that document's id and rejects any
    // other explicit timeline. Project/live backends override this to select
    // another timeline without changing persisted active-project state.
    virtual bool SelectTimelineForEdit(const std::string& timelineId,
                                       std::string& errorName,
                                       std::string& message) {
        if (timelineId.empty()) return true;
        Document document;
        if (!SnapshotDocument(document, message)) {
            errorName = "IoError";
            return false;
        }
        if (document.sequence.id == timelineId) return true;
        errorName = "UnknownSequence";
        message = "unknown timeline_id '" + timelineId + "'";
        return false;
    }
    virtual void EndTimelineEdit() {}

    // Applies a freshly constructed (not-yet-applied) operation through the
    // same path as `--apply-op`. On success, resultJson holds the tool
    // result payload to report back over MCP. On failure, errorName is one
    // of the EditErrorName() strings from Operations.h.
    virtual bool ApplyOperation(Operation operation, std::string& resultJson,
                                std::string& errorName,
                                std::string& message) = 0;

    // Project-level edit used by agent-created timelines. Backends that only
    // expose a standalone Document may leave this unsupported.
    virtual bool ApplyProjectEdit(ProjectOperation, std::string&,
                                  std::string& errorName,
                                  std::string& message) {
        errorName = "UnsupportedOperation";
        message = "this backend cannot create project timelines";
        return false;
    }

    // Read-only cached transcript view. Kept behind the backend because the
    // cache directory belongs to the open project, not the Document JSON.
    virtual bool ReadTimelineTranscript(std::string&, std::string& message) {
        message = "this backend has no project transcript cache";
        return false;
    }

    // One mounted source's cached transcript, keyed on the media identity
    // DocumentSource::id and LibraryMedia::id share. Separate from
    // ReadTimelineTranscript because word-level editing addresses a single
    // clip's own source, not the flattened view of the audible timeline.
    virtual bool ReadSourceTranscript(const Ulid&, Transcript&,
                                      std::string& message) {
        message = "this backend has no project transcript cache";
        return false;
    }

    // The same view, structured. Editorial selection addresses spans by id
    // and never by time, so the tool that builds a short needs the spans as
    // objects rather than as the rendered JSON a model reads. The default
    // goes through ReadTimelineTranscript and parses its own output, so a
    // backend that can only produce the rendered view still answers this
    // correctly instead of having to grow a second cache path.
    virtual bool ReadTimelineTranscriptSpans(
        std::vector<TimelineTranscriptSpan>& spans, std::string& message) {
        std::string json;
        if (!ReadTimelineTranscript(json, message)) return false;
        return ParseTimelineTranscriptSpans(json, spans, message);
    }

    // Renders one frame of a mounted source, as JPEG bytes, at an exact
    // position in that source's own time domain. Behind the backend because
    // resolving a media id to a file on disk is the open project's job.
    // See FrameCapture.h for why looking is a capability at all.
    virtual bool CaptureSourceFrame(const Ulid&, const RationalTime&,
                                    std::string&, std::string& message) {
        message = "this backend cannot render a frame";
        return false;
    }

    // Produces the picture-quality analysis this backend can otherwise only
    // read. Without it the tool catalog could report grades and never make
    // one, so an agent depended on a human running the CLI first -- the
    // engine-first rule broken at the last step (PHILOSOPHY.md principle 3).
    // Long-running by nature: it decodes the whole source.
    virtual bool AnalyzeSourceShotQuality(const Ulid&, std::string&,
                                          std::string& message) {
        message = "this backend cannot run a shot quality analysis";
        return false;
    }

    // Produces the transcript this backend can otherwise only read. Same
    // asymmetry AnalyzeSourceShotQuality closed for picture quality, and the
    // more costly one: every word-level tool in this catalog
    // (list_disfluencies, remove_words, the interview short) needs a
    // transcript, so without this the agent's whole editorial path depended
    // on a human running `--transcribe` first. The Whisper model is not an
    // argument -- it is a local setting the engine resolves
    // (Transcription.h) -- because an agent has no way to know where a human
    // keeps a ggml file. Long-running by nature: it decodes and infers over
    // the whole audio.
    //
    // QC-2026-09 A3 -- takes a list because loading the model costs about 8 s
    // and used to be paid once per media. `includeSilent` overrides the skip
    // of media the document records as mute.
    virtual bool TranscribeSources(const std::vector<Ulid>&, const std::string&,
                                   bool, bool, std::string&,
                                   std::string& message) {
        message = "this backend cannot run a transcription";
        return false;
    }

    // Read-only cached picture-quality report for one mounted source, keyed
    // on the same media identity DocumentSource::id and LibraryMedia::id
    // share. Kept behind the backend for the same reason the transcript
    // readers are: the analysis is a cache artifact belonging to the open
    // project, never document state.
    virtual bool ReadSourceShotQuality(const Ulid&, ShotQualityReport&,
                                       std::string& message) {
        message = "this backend has no project shot quality cache";
        return false;
    }

    // ONSET-2026-08 -- the same pair for the speech envelope. It exists
    // because the transcript is not a reliable answer to "where does this
    // clip start speaking": Whisper puts the first words of a segment on
    // silence, so an agent trusting them opens clips on dead air. Reading a
    // measurement it cannot produce would leave the same half-workflow
    // AnalyzeSourceShotQuality was added to close.
    virtual bool ReadSourceSpeechOnset(const Ulid&, SpeechOnsetReport&,
                                       std::string& message) {
        message = "this backend has no project speech onset cache";
        return false;
    }

    virtual bool AnalyzeSourceSpeechOnset(const Ulid&,
                                          const SpeechOnsetSettings&,
                                          std::string&, std::string& message) {
        message = "this backend cannot run a speech onset analysis";
        return false;
    }

    // QC-2026-09 (A1) -- pulls every cached transcript's word boundaries onto
    // the speech envelope, and optionally writes the corrections back. Not a
    // per-source call like the pair above: the point of the pass is that the
    // agent stops choosing which rush to doubt, because it cannot tell from
    // the words alone. Behind the backend because both artifacts are project
    // cache files, and the write installs one.
    virtual bool AlignSourceTranscripts(bool, std::string&,
                                        std::string& message) {
        message = "this backend has no project transcript cache";
        return false;
    }

    virtual bool Undo(std::string& resultJson, std::string& errorName,
                      std::string& message) = 0;
    virtual bool Redo(std::string& resultJson, std::string& errorName,
                      std::string& message) = 0;

    // Same JSON shape as `--describe`.
    virtual bool Describe(std::string& json, std::string& message) = 0;
};
