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
    explicit McpProjectBackend(std::string projectPath);

    bool SnapshotDocument(Document& document, std::string& message) override;
    bool ApplyOperation(Operation operation, std::string& resultJson,
                        std::string& errorName, std::string& message) override;
    bool ApplyProjectEdit(ProjectOperation operation, std::string& resultJson,
                          std::string& errorName,
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
};
