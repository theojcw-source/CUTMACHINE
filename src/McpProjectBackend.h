#pragma once

// Production McpBackend: a thin adapter over Cli.cc's headless commands, the
// same entry points `--apply-op`/`--describe` use. Depends on ProjectStorage
// (CommonCrypto), so -- like the rest of the project-facing CLI surface --
// this only builds where that is available (macOS). Keep this file free of
// HTTP/JSON-RPC concerns; McpServer.cc/McpTools.cc own that layer and only
// see it through McpBackend.

#include "McpBackend.h"
#include "Project.h"

#include <filesystem>
#include <string>

class McpProjectBackend : public McpBackend {
public:
    explicit McpProjectBackend(std::string projectPath,
                               bool requireExplicitTimeline = false);

    bool SelectTimelineForEdit(const std::string& timelineId,
                               std::string& errorName,
                               std::string& message) override;
    void EndTimelineEdit() override { selected_timeline_id_.clear(); }
    bool SnapshotDocument(Document& document, std::string& message) override;
    bool ApplyOperation(Operation operation, std::string& resultJson,
                        std::string& errorName, std::string& message) override;
    bool ApplyProjectEdit(ProjectOperation operation, std::string& resultJson,
                          std::string& errorName,
                          std::string& message) override;
    bool ReadSourceTranscript(const Ulid& sourceId, Transcript& transcript,
                              std::string& message) override;
    bool ReadSourceShotQuality(const Ulid& sourceId, ShotQualityReport& report,
                               std::string& message) override;
    bool AnalyzeSourceShotQuality(const Ulid& sourceId, std::string& resultJson,
                                  std::string& message) override;
    bool ReadSourceSpeechOnset(const Ulid& sourceId, SpeechOnsetReport& report,
                               std::string& message) override;
    bool AnalyzeSourceSpeechOnset(const Ulid& sourceId,
                                  const SpeechOnsetSettings& settings,
                                  std::string& resultJson,
                                  std::string& message) override;
    bool AlignSourceTranscripts(bool apply, std::string& resultJson,
                                std::string& message) override;
    bool TranscribeSources(const std::vector<Ulid>& sourceIds,
                           const std::string& language, bool verbatim,
                           bool includeSilent, std::string& resultJson,
                           std::string& message) override;
    bool TranscribeTimeline(const std::string& timelineId,
                            const std::string& language, bool verbatim,
                            std::string& resultJson,
                            std::string& message) override;
    bool CaptureSourceFrame(const Ulid& sourceId, const RationalTime& time,
                            std::string& jpegBytes,
                            std::string& message) override;
    bool CaptureTimelineSheet(const TimelineSheetPlan& plan,
                              const TimelineSheetSettings& settings,
                              std::string& jpegBytes,
                              std::string& message) override;
    bool ReadTimelineTranscript(std::string& json,
                                std::string& message) override;
    bool Undo(std::string& resultJson, std::string& errorName,
              std::string& message) override;
    bool Redo(std::string& resultJson, std::string& errorName,
              std::string& message) override;
    bool Describe(std::string& json, std::string& message) override;

private:
    // PERF-2026-09. One MCP tool call used to reload the whole package from
    // disk three or four times over: once to resolve the timeline, once for
    // the snapshot the dispatcher resolves aliases against, usually once
    // more inside the dispatcher itself, and once more inside the CLI
    // command that applies the edit. Each of those reads the project file
    // plus every Timelines/*.json beside it, parses them and revalidates the
    // result. --mcp-serve holds the project's session lock for its whole
    // lifetime, so nothing outside this process can move those bytes: the
    // reloads were repetition, not re-reads of something that had changed.
    bool LoadProject(Project& project, std::string& message);
    void InvalidateCache();

    // Any CLI command this backend runs may commit a new generation of the
    // files the cache mirrors, so the cache stops being the truth when one
    // returns. The rule is drawn at "runs a command" and not at "writes"
    // deliberately: whether a given command commits is Cli.cc's business,
    // not this adapter's, and a command that becomes a writer later must not
    // quietly invalidate the reasoning here. Declared as a scope rather than
    // a call before each `return` so a method added later cannot forget it,
    // and released on the way out rather than on the way in because a
    // dispatcher may still snapshot between the two.
    class CommandScope {
    public:
        explicit CommandScope(McpProjectBackend& backend) : backend_(backend) {}
        ~CommandScope() { backend_.InvalidateCache(); }
        CommandScope(const CommandScope&) = delete;
        CommandScope& operator=(const CommandScope&) = delete;

    private:
        McpProjectBackend& backend_;
    };

    std::string project_path_;
    std::string selected_timeline_id_;
    bool require_explicit_timeline_ = false;
    Project cached_project_;
    bool cache_valid_ = false;
    std::uintmax_t cached_size_ = 0;
    std::filesystem::file_time_type cached_write_time_{};
};
