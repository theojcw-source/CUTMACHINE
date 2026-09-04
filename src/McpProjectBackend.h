#pragma once

// Production McpBackend: a thin adapter over Cli.cc's headless commands, the
// same entry points `--apply-op`/`--describe` use. Depends on ProjectStorage
// (CommonCrypto), so -- like the rest of the project-facing CLI surface --
// this only builds where that is available (macOS). Keep this file free of
// HTTP/JSON-RPC concerns; McpServer.cc/McpTools.cc own that layer and only
// see it through McpBackend.

#include "McpBackend.h"

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
    std::string project_path_;
    std::string selected_timeline_id_;
    bool require_explicit_timeline_ = false;
};
