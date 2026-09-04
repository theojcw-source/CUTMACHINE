#pragma once

#include "Export.h"

#include <map>
#include <string>
#include <utility>
#include <vector>

class Document;
class EditLog;
class ProjectEditLog;
class Project;

// B2 -- ROADMAP.md. Every headless surface uses this serializer for a
// refusal, including commands implemented outside Cli.cc. Keeping the exit
// status beside the JSON construction prevents a caller from receiving a
// non-zero status with a success-shaped or free-form payload.
int FailCliCommand(const std::string& errorCode, const std::string& detail,
                   std::string& output, int exitStatus = 1);

// Headless command entry points. These functions depend only on the model
// library and never initialize AppKit, Metal, media decoding, or rendering.
int CreateProjectCommand(const std::string& packagePath,
                         const std::string& projectName, std::string& output);
// ALPHA-2026-08 -- automation must be able to produce the same local
// transcript cache the AppKit action consumes; otherwise an agent can read a
// transcript but cannot complete the workflow that creates it.
// An empty `whisperModelPath` resolves the model configured locally
// (Transcription.h's ResolveConfiguredWhisperModel), which is how every
// caller that is not a human typing a path reaches this.
// QC-2026-09 A3 -- takes several media because the Whisper model load costs
// about 8 s and used to be paid once per call. Media whose measured audio
// level (Document.h, recorded at ingest) says they are mute are skipped and
// reported rather than sent to the model; `includeSilent` overrides that.
int TranscribeMediaCommand(const std::string& projectPath,
                           const std::vector<std::string>& mediaIds,
                           const std::string& whisperModelPath,
                           const std::string& language, bool verbatim,
                           bool includeSilent, std::string& output);
// ALPHA-2026-08 -- the reason these take word indices and never a timecode
// is PHILOSOPHY.md principle 7: the caller (agent or human) names *which
// words*, CUTMACHINE resolves *which frames*. A caller that could pass a
// time would be a caller that could invent one.
// QC-2026-08 -- picture quality is measured, never judged, so the analysis
// has to be reachable without the app: an agent that can read a grade but
// cannot produce one has half a workflow. Mirrors TranscribeMediaCommand.
int AnalyzeShotQualityCommand(const std::string& projectPath,
                              const std::string& mediaId, std::string& output);
int ShotQualityReportCommand(const std::string& projectPath,
                             std::string& output);
// QC-2026-09 (A8) -- read-only arithmetic over the active composited
// timeline. The command publishes exact time and ratio values so callers do
// not have to count clips or convert frame rates themselves.
int TimelineStatsCommand(const std::string& projectPath, std::string& output);
// ONSET-2026-08 -- same shape, and for the same reason: where the voice
// starts is a measurement, so it has to be reachable without the app. The
// report publishes the trim in whole frames precisely so a caller never
// computes one (PHILOSOPHY.md principle 7).
int AnalyzeSpeechOnsetCommand(const std::string& projectPath,
                              const std::string& mediaId, std::string& output);
int SpeechOnsetReportCommand(const std::string& projectPath,
                             std::string& output);
// ALIGN-2026-08 -- reports which cached word boundaries the speech envelope
// contradicts, and where each one belongs. Read-only unless `apply`: a
// transcript is a cache artifact several tools read, and rewriting it under
// them is a decision for the caller, not for a report.
//
// QC-2026-09 (A1) -- `apply` is that decision. Without it the correction
// exists only in a report, and every tool that cuts on words keeps cutting on
// the boundaries the signal contradicts; the measured cost of that was some
// forty probe round trips in one session. The document is never touched
// either way -- this rewrites a cache file, which a re-transcription
// regenerates.
int AlignTranscriptsCommand(const std::string& projectPath, bool apply,
                            std::string& output);
int ListDisfluenciesCommand(const std::string& projectPath,
                            const std::string& clipId, std::string& output);
int RemoveWordsCommand(const std::string& projectPath,
                       const std::string& clipId, const std::string& rangesJson,
                       std::string& output);
// QC-2026-09 A2 -- closes the silences inside one clip, from the speech
// envelope alone. Applies one reversible RemoveWordsOperation carrying the
// clip's A/V pair, and reports which pauses it closed. Needs a cached
// speech onset analysis for the clip's source; it needs no transcript,
// which is the point -- tightening a take should not wait on Whisper.
int TightenPausesCommand(const std::string& projectPath,
                         const std::string& clipId, int64_t minimumGapMs,
                         int64_t keepFrames, std::string& output);
// QC-2026-09 A4 -- read-only. Reports which clips of the active timeline play
// a given frame of a given rush, and where. The conversion it performs is the
// one every hand-computed timeline position in a measured session got wrong
// at least once.
int LocateSourceFrameCommand(const std::string& projectPath,
                             const std::string& mediaId, int64_t sourceFrame,
                             std::string& output);
int DescribeCommand(const std::string& documentPath, std::string& output);
// SRT-2026-08 -- read-only. Subtitles were reachable only from the app: the
// cue builder and SaveSrt both had their single caller in main.mm, so a
// headless montage could be transcribed but never subtitled, against
// AGENTS.md's rule that what only the mouse can reach does not exist. Cues
// come from the audible audio tracks, because subtitles follow what is
// heard, not what is on screen -- an overlay laid over someone else's words
// must not caption itself.
int ExportSrtCommand(const std::string& projectPath,
                     const std::string& outputPath, std::string& output);
// SEQ-2026-08 -- read-only: reports the sequence format the project's rushes
// imply, without touching the document. Conforming to it is a separate,
// journalized UpdateSequenceOperation.
int ProposeSequenceCommand(const std::string& projectPath, std::string& output);

// The same JSON view DescribeCommand produces (sequence/tracks/library/bins/
// markers, with the A1/A2.../K1... aliases the MCP tool catalog's ID
// resolver and the chat panel both key off), computed directly from an
// in-memory Document instead of a project file path. See McpLiveBackend.h.
std::string DescribeDocument(const Document& document);
std::string DescribeProject(const Project& project);
int ApplyOperationCommand(const std::string& documentPath,
                          const std::string& operationJson,
                          std::string& output);
int ApplyProjectOperationCommand(const std::string& projectPath,
                                 const std::string& operationJson,
                                 std::string& output);
int UndoProjectOperationCommand(const std::string& projectPath,
                                std::string& output);
int RedoProjectOperationCommand(const std::string& projectPath,
                                std::string& output);

// Timeline-level undo/redo counterparts to ApplyOperationCommand. Operate on
// the active timeline's own EditLog, the same log --apply-op appends to, so
// an MCP undo/redo tool call is indistinguishable on disk from a human
// triggering undo/redo in the app. There is no CLI flag for these yet; they
// exist so the MCP server (Operations.h ticket F1.1) can reuse this exact
// path instead of duplicating EditLog::Undo/Redo call sites.
int UndoOperationCommand(const std::string& documentPath, std::string& output);
int RedoOperationCommand(const std::string& documentPath, std::string& output);
int ExportCommand(const std::string& documentPath,
                  const ExportSettings& settings,
                  const ExportProgressCallback& progress,
                  const std::atomic_bool* cancel, std::string& output);

std::string TimelineEditLogPathForProject(const std::string& projectPath,
                                          const std::string& timelineId);
std::string ProjectEditLogPathForProject(const std::string& projectPath);

// Transactional primitive for storage layouts that add package-local
// artifacts. All destinations must be regular files on the same filesystem.
bool CommitTextArtifacts(
    const std::vector<std::pair<std::string, std::string>>& artifacts,
    std::string& message);

// S2 -- SAVING_ROADMAP.md. Recovers an interrupted transaction rooted in
// directory and removes leftovers whose durable commit marker is already gone.
bool RecoverTextArtifactTransaction(const std::string& directory,
                                    std::string& message);

// S4 -- SAVING_ROADMAP.md. Project packages use this variant so removal of
// obsolete canonical files belongs to the generation being published.
bool CommitTextArtifactsAndRemove(
    const std::vector<std::pair<std::string, std::string>>& artifacts,
    const std::vector<std::string>& removals, std::string& message);
