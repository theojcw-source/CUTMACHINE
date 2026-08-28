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
int TranscribeMediaCommand(const std::string& projectPath,
                           const std::string& mediaId,
                           const std::string& whisperModelPath,
                           const std::string& language, bool verbatim,
                           std::string& output);
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
int ListDisfluenciesCommand(const std::string& projectPath,
                            const std::string& clipId, std::string& output);
int RemoveWordsCommand(const std::string& projectPath,
                       const std::string& clipId, const std::string& rangesJson,
                       std::string& output);
int DescribeCommand(const std::string& documentPath, std::string& output);

// The same JSON view DescribeCommand produces (sequence/tracks/library/bins/
// markers, with the A1/A2.../K1... aliases the MCP tool catalog's ID
// resolver and the chat panel both key off), computed directly from an
// in-memory Document instead of a project file path. See McpLiveBackend.h.
std::string DescribeDocument(const Document& document);
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
