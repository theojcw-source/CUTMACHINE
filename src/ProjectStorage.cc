#include "ProjectStorage.h"

#include "Cli.h"
#include "EditLog.h"
#include "Project.h"
#include "Ulid.h"

#include <CommonCrypto/CommonDigest.h>

#include <unistd.h>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <system_error>

namespace {

namespace fs = std::filesystem;

class StagingDirectory {
public:
    explicit StagingDirectory(fs::path path) : path_(std::move(path)) {}
    ~StagingDirectory() {
        if (!committed_) {
            std::error_code ignored;
            fs::remove_all(path_, ignored);
        }
    }
    const fs::path& path() const { return path_; }
    void Commit() { committed_ = true; }

private:
    fs::path path_;
    bool committed_ = false;
};

std::string EscapeJson(const std::string& value) {
    std::ostringstream output;
    for (unsigned char character : value) {
        switch (character) {
            case '\\':
                output << "\\\\";
                break;
            case '"':
                output << "\\\"";
                break;
            case '\n':
                output << "\\n";
                break;
            case '\r':
                output << "\\r";
                break;
            case '\t':
                output << "\\t";
                break;
            default:
                if (character < 0x20)
                    output << "\\u" << std::hex << std::setw(4)
                           << std::setfill('0') << static_cast<int>(character)
                           << std::dec;
                else
                    output << static_cast<char>(character);
        }
    }
    return output.str();
}

bool Sha256(const fs::path& path, std::string& digest, std::string& error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "unable to hash media '" + path.string() + "'";
        return false;
    }
    CC_SHA256_CTX context;
    CC_SHA256_Init(&context);
    std::array<char, 1024 * 1024> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = input.gcount();
        if (count > 0)
            CC_SHA256_Update(&context, buffer.data(),
                             static_cast<CC_LONG>(count));
    }
    if (!input.eof()) {
        error = "unable to read media while hashing '" + path.string() + "'";
        return false;
    }
    std::array<unsigned char, CC_SHA256_DIGEST_LENGTH> bytes{};
    CC_SHA256_Final(bytes.data(), &context);
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (unsigned char byte : bytes) output << std::setw(2) << int(byte);
    digest = output.str();
    return true;
}

bool HasParentTraversal(const fs::path& path) {
    for (const fs::path& component : path)
        if (component == "..") return true;
    return false;
}

bool LoadTimelineLogs(const std::string& projectPath, const Project& project,
                      std::map<std::string, EditLog>& logs,
                      std::string& error) {
    EditError editError = EditError::None;
    for (const DocumentSequence& timeline : project.timelines) {
        EditLog log;
        const fs::path path =
            TimelineEditLogPathForProject(projectPath, timeline.id);
        std::error_code existsError;
        if (fs::exists(path, existsError)) {
            if (!EditLog::Load(path.string(), log, editError, error))
                return false;
        }
        if (existsError) {
            error =
                "unable to inspect timeline history: " + existsError.message();
            return false;
        }
        logs.emplace(timeline.id, std::move(log));
    }
    return true;
}

std::string Hostname() {
    std::array<char, 256> value{};
    if (gethostname(value.data(), value.size() - 1) != 0) return "unknown";
    return value.data();
}

std::string Username() {
    const char* value = std::getenv("USER");
    return value && *value ? value : "unknown";
}

bool ReadLockOwner(const fs::path& directory, std::string& token,
                   std::string& host, int64_t& pid, std::string& owner) {
    std::ifstream input(directory / "owner");
    if (!input) return false;
    std::string user;
    std::getline(input, token);
    std::getline(input, host);
    std::string pidText;
    std::getline(input, pidText);
    std::getline(input, user);
    try {
        pid = std::stoll(pidText);
    } catch (...) {
        return false;
    }
    owner = user + " sur " + host + " (PID " + pidText + ")";
    return !token.empty() && !host.empty() && pid > 0;
}

bool ExtractManifestValue(const std::string& json, const std::string& mediaId,
                          uint64_t& bytes, std::string& sha256) {
    const std::string marker = "{\"id\":\"" + mediaId + "\"";
    const size_t begin = json.find(marker);
    if (begin == std::string::npos) return false;
    const size_t end = json.find('}', begin);
    if (end == std::string::npos) return false;
    const size_t bytesKey = json.find("\"bytes\":", begin);
    const size_t shaKey = json.find("\"sha256\":\"", begin);
    if (bytesKey == std::string::npos || bytesKey > end ||
        shaKey == std::string::npos || shaKey > end)
        return false;
    const size_t bytesStart = bytesKey + std::strlen("\"bytes\":");
    const size_t bytesEnd = json.find(',', bytesStart);
    const size_t shaStart = shaKey + std::strlen("\"sha256\":\"");
    const size_t shaEnd = json.find('"', shaStart);
    if (bytesEnd == std::string::npos || bytesEnd > end ||
        shaEnd == std::string::npos || shaEnd > end)
        return false;
    try {
        bytes = std::stoull(json.substr(bytesStart, bytesEnd - bytesStart));
    } catch (...) {
        return false;
    }
    sha256 = json.substr(shaStart, shaEnd - shaStart);
    return sha256.size() == CC_SHA256_DIGEST_LENGTH * 2;
}

bool ReadFile(const fs::path& path, std::string& contents, std::string& error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "unable to open '" + path.string() + "'";
        return false;
    }
    std::ostringstream output;
    output << input.rdbuf();
    if (!input.good() && !input.eof()) {
        error = "unable to read '" + path.string() + "'";
        return false;
    }
    contents = output.str();
    return true;
}

Document StandaloneTimelineDocument(const Project& project,
                                    const DocumentSequence& timeline) {
    Document document;
    document.sequence = timeline;
    document.color_management = project.settings.color_management;
    return document;
}

}  // namespace

bool IsPortableProjectV2(const std::string& projectPath) {
    const fs::path manifest =
        fs::path(projectPath).parent_path() / "manifest.json";
    std::string contents;
    std::string error;
    return ReadFile(manifest, contents, error) &&
           contents.find("\"format\":\"cutmachine-collection\"") !=
               std::string::npos &&
           contents.find("\"version\":2") != std::string::npos;
}

bool CreatePortableProject(const std::string& destinationDirectory,
                           const Project& project, std::string& projectPath,
                           std::string& error) {
    projectPath.clear();
    if (!project.Validate(error)) return false;
    const fs::path destination = fs::absolute(destinationDirectory);
    std::error_code pathError;
    if (fs::exists(destination, pathError) || pathError) {
        error = pathError
                    ? "unable to inspect destination: " + pathError.message()
                    : "destination already exists";
        return false;
    }
    if (!fs::is_directory(destination.parent_path(), pathError) || pathError) {
        error = "destination parent is not accessible";
        return false;
    }
    const fs::path staging =
        destination.parent_path() / (destination.filename().string() +
                                     ".cutmachine-" + GenerateUlid() + ".tmp");
    StagingDirectory cleanup(staging);
    if (!fs::create_directories(staging / "Timelines", pathError) ||
        pathError) {
        error = "unable to create project package: " + pathError.message();
        return false;
    }
    std::ostringstream manifest;
    manifest << "{\n  \"format\":\"cutmachine-collection\",\n"
             << "  \"version\":2,\n"
             << "  \"project\":\"project.cutmachine.json\",\n"
             << "  \"timelines\":[";
    for (size_t index = 0; index < project.timelines.size(); ++index) {
        const DocumentSequence& timeline = project.timelines[index];
        manifest << (index == 0 ? "\n" : ",\n") << "    {\"id\":\""
                 << EscapeJson(timeline.id) << "\",\"path\":\"Timelines/"
                 << EscapeJson(timeline.id) << ".json\"}";
    }
    if (!project.timelines.empty()) manifest << '\n';
    manifest << "  ],\n  \"media\":[]\n}\n";
    if (!CommitTextArtifacts(
            {{(staging / "manifest.json").string(), manifest.str()}}, error))
        return false;

    std::map<std::string, EditLog> logs;
    for (const DocumentSequence& timeline : project.timelines)
        logs.emplace(timeline.id, EditLog{});
    const fs::path inner = staging / "project.cutmachine.json";
    if (!CommitStoredProjectAndLogs(inner.string(), project, logs,
                                    ProjectEditLog{}, error))
        return false;
    fs::rename(staging, destination, pathError);
    if (pathError) {
        error = "unable to publish project package: " + pathError.message();
        return false;
    }
    cleanup.Commit();
    projectPath = (destination / "project.cutmachine.json").string();
    error.clear();
    return true;
}

bool LoadStoredProject(const std::string& projectPath, Project& project,
                       std::string& error) {
    if (!IsPortableProjectV2(projectPath)) {
        error = "unsupported project package (CUTMACHINE package v2 required)";
        return false;
    }
    Project loaded;
    if (!Project::Load(projectPath, loaded, error)) return false;
    const fs::path timelineDirectory =
        fs::path(projectPath).parent_path() / "Timelines";
    for (DocumentSequence& timeline : loaded.timelines) {
        const fs::path path = timelineDirectory / (timeline.id + ".json");
        std::string json;
        if (!ReadFile(path, json, error)) {
            error =
                "portable timeline is missing: " + timeline.id + ": " + error;
            return false;
        }
        Document document;
        if (!Document::LoadFromString(json, document, error, false)) {
            error = "invalid portable timeline '" + timeline.id + "': " + error;
            return false;
        }
        if (document.sequence.id != timeline.id) {
            error = "portable timeline ID does not match its filename: " +
                    timeline.id;
            return false;
        }
        timeline = std::move(document.sequence);
    }
    if (!loaded.Validate(error)) return false;
    project = std::move(loaded);
    error.clear();
    return true;
}

bool CommitStoredProject(const std::string& projectPath, const Project& project,
                         std::string& error) {
    std::map<std::string, EditLog> timelineLogs;
    if (!LoadTimelineLogs(projectPath, project, timelineLogs, error))
        return false;
    ProjectEditLog projectLog;
    const fs::path projectLogPath = ProjectEditLogPathForProject(projectPath);
    std::error_code existsError;
    if (fs::exists(projectLogPath, existsError)) {
        EditError editError = EditError::None;
        if (!ProjectEditLog::Load(projectLogPath.string(), projectLog,
                                  editError, error))
            return false;
    }
    if (existsError) {
        error = "unable to inspect project history: " + existsError.message();
        return false;
    }
    return CommitStoredProjectAndLogs(projectPath, project, timelineLogs,
                                      projectLog, error);
}

bool CommitStoredProjectAndLogs(
    const std::string& projectPath, const Project& project,
    const std::map<std::string, EditLog>& timelineLogs,
    const ProjectEditLog& projectLog, std::string& error) {
    if (!IsPortableProjectV2(projectPath)) {
        error = "unsupported project package (CUTMACHINE package v2 required)";
        return false;
    }
    if (!project.Validate(error)) return false;
    std::vector<std::pair<std::string, std::string>> artifacts;
    artifacts.push_back({projectPath, project.SaveToString()});
    const fs::path base = fs::path(projectPath).parent_path();
    const fs::path timelines = base / "Timelines";
    std::error_code directoryError;
    fs::create_directories(timelines, directoryError);
    if (directoryError) {
        error =
            "unable to create timeline directory: " + directoryError.message();
        return false;
    }
    for (const DocumentSequence& timeline : project.timelines) {
        artifacts.push_back(
            {(timelines / (timeline.id + ".json")).string(),
             StandaloneTimelineDocument(project, timeline).SaveToString()});
        const auto log = timelineLogs.find(timeline.id);
        artifacts.push_back(
            {TimelineEditLogPathForProject(projectPath, timeline.id),
             (log == timelineLogs.end() ? EditLog{} : log->second)
                 .Serialize()});
    }
    artifacts.push_back(
        {ProjectEditLogPathForProject(projectPath), projectLog.Serialize()});
    return CommitTextArtifacts(artifacts, error);
}

ProjectSessionLock::ProjectSessionLock() = default;
ProjectSessionLock::~ProjectSessionLock() { Release(); }

bool ProjectSessionLock::Acquire(const std::string& projectPath,
                                 std::string& owner, std::string& error) {
    Release();
    owner.clear();
    error.clear();
    if (projectPath.empty()) {
        error = "project path must not be empty";
        return false;
    }
    const fs::path directory(projectPath + ".lock");
    for (int attempt = 0; attempt < 2; ++attempt) {
        std::error_code createError;
        if (fs::create_directory(directory, createError)) {
            token_ = GenerateUlid();
            std::ofstream output(directory / "owner", std::ios::trunc);
            if (!output) {
                std::error_code ignored;
                fs::remove_all(directory, ignored);
                token_.clear();
                error = "unable to write project lock metadata";
                return false;
            }
            output << token_ << '\n'
                   << Hostname() << '\n'
                   << getpid() << '\n'
                   << Username() << '\n';
            output.close();
            if (!output) {
                std::error_code ignored;
                fs::remove_all(directory, ignored);
                token_.clear();
                error = "unable to commit project lock metadata";
                return false;
            }
            lock_directory_ = directory.string();
            return true;
        }
        if (createError && createError != std::errc::file_exists) {
            error = "unable to create project lock: " + createError.message();
            return false;
        }
        std::string existingToken;
        std::string host;
        int64_t pid = 0;
        if (!ReadLockOwner(directory, existingToken, host, pid, owner)) {
            error = "project lock exists but its owner metadata is invalid";
            return false;
        }
        if (host == Hostname() && kill(static_cast<pid_t>(pid), 0) != 0 &&
            errno == ESRCH) {
            std::error_code removeError;
            fs::remove_all(directory, removeError);
            if (removeError) {
                error = "unable to remove stale project lock: " +
                        removeError.message();
                return false;
            }
            continue;
        }
        error = "project is already open by " + owner;
        return false;
    }
    error = "unable to acquire project lock";
    return false;
}

void ProjectSessionLock::Release() {
    if (lock_directory_.empty() || token_.empty()) return;
    const fs::path directory(lock_directory_);
    std::string existingToken;
    std::string host;
    std::string owner;
    int64_t pid = 0;
    if (ReadLockOwner(directory, existingToken, host, pid, owner) &&
        existingToken == token_) {
        std::error_code ignored;
        fs::remove_all(directory, ignored);
    }
    lock_directory_.clear();
    token_.clear();
}

bool ProjectSessionLock::Held() const { return !lock_directory_.empty(); }

bool VerifyPortableProject(const std::string& projectPath,
                           CollectionIntegrityReport& report,
                           std::string& error) {
    report = {};
    const fs::path project(projectPath);
    const fs::path manifestPath = project.parent_path() / "manifest.json";
    std::ifstream input(manifestPath, std::ios::binary);
    if (!input) {
        // "manifest is missing" describes our own bookkeeping, not what the
        // caller did wrong. The two ways to get here are worth telling
        // apart, because they need opposite things from the user.
        std::error_code projectError;
        if (!fs::exists(project, projectError) || projectError) {
            error = "no project file at " + project.string();
        } else {
            error = project.filename().string() + " is not inside a " +
                    ".cutmachine-project package (no manifest.json beside "
                    "it). Single-file projects predate the package format "
                    "and cannot be opened directly; create a new project "
                    "and re-ingest, or point at the project.cutmachine.json "
                    "inside an existing package";
        }
        return false;
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    if (!input.good() && !input.eof()) {
        error = "unable to read collection manifest";
        return false;
    }
    const std::string json = contents.str();
    if (json.find("\"format\":\"cutmachine-collection\"") ==
        std::string::npos) {
        error = "unsupported collection manifest";
        return false;
    }
    Project loaded;
    if (!LoadStoredProject(projectPath, loaded, error)) return false;
    for (const LibraryMedia& media : loaded.rushes) {
        uint64_t expectedBytes = 0;
        std::string expectedSha;
        if (!ExtractManifestValue(json, media.id, expectedBytes, expectedSha)) {
            report.modified_media.push_back(media.filename);
            continue;
        }
        const fs::path path = project.parent_path() / media.path;
        std::error_code sizeError;
        const uint64_t actualBytes = fs::file_size(path, sizeError);
        if (sizeError) {
            report.missing_media.push_back(media.filename);
            continue;
        }
        if (actualBytes != expectedBytes) {
            report.modified_media.push_back(media.filename);
            continue;
        }
        std::string actualSha;
        if (!Sha256(path, actualSha, error)) return false;
        if (actualSha != expectedSha)
            report.modified_media.push_back(media.filename);
        else
            report.verified_media++;
    }
    error.clear();
    return true;
}

bool CollectPortableProject(const std::string& sourceProjectPath,
                            const std::string& destinationDirectory,
                            PortableProjectResult& result, std::string& error) {
    result = {};
    if (sourceProjectPath.empty() || destinationDirectory.empty()) {
        error = "source project and destination must not be empty";
        return false;
    }

    std::error_code pathError;
    const fs::path sourceProject =
        fs::weakly_canonical(sourceProjectPath, pathError);
    if (pathError || !fs::is_regular_file(sourceProject, pathError)) {
        error = "source project is not a readable regular file";
        return false;
    }
    const fs::path destination = fs::absolute(destinationDirectory);
    if (fs::exists(destination, pathError) || pathError) {
        error = pathError
                    ? "unable to inspect destination: " + pathError.message()
                    : "destination already exists";
        return false;
    }
    const fs::path parent = destination.parent_path();
    if (!fs::is_directory(parent, pathError) || pathError) {
        error = "destination parent is not accessible";
        return false;
    }

    Project collected;
    if (!LoadStoredProject(sourceProject.string(), collected, error))
        return false;
    std::map<std::string, EditLog> timelineLogs;
    if (!LoadTimelineLogs(sourceProject.string(), collected, timelineLogs,
                          error))
        return false;
    ProjectEditLog projectLog;
    const fs::path sourceProjectLog =
        ProjectEditLogPathForProject(sourceProject.string());
    pathError.clear();
    if (fs::exists(sourceProjectLog, pathError)) {
        EditError editError = EditError::None;
        if (!ProjectEditLog::Load(sourceProjectLog.string(), projectLog,
                                  editError, error))
            return false;
    }
    if (pathError) {
        error = "unable to inspect project history: " + pathError.message();
        return false;
    }

    const fs::path staging =
        parent / (destination.filename().string() + ".cutmachine-" +
                  GenerateUlid() + ".tmp");
    StagingDirectory cleanup(staging);
    const fs::path mediaDirectory = staging / "Media";
    if (!fs::create_directories(mediaDirectory, pathError) || pathError) {
        error = "unable to create collection staging directory: " +
                pathError.message();
        return false;
    }

    struct ManifestMedia {
        std::string id;
        std::string path;
        uint64_t bytes = 0;
        std::string sha256;
    };
    std::vector<ManifestMedia> manifest;
    const fs::path sourceBase = sourceProject.parent_path();
    for (LibraryMedia& media : collected.rushes) {
        pathError.clear();
        fs::path original(media.path);
        if (original.is_relative()) original = sourceBase / original;
        original = fs::weakly_canonical(original, pathError);
        if (pathError || !fs::is_regular_file(original, pathError)) {
            error = "original media is missing or unreadable: " + media.path;
            return false;
        }
        std::string filename = original.filename().string();
        if (filename.empty()) filename = "media";
        const fs::path relative =
            fs::path("Media") / (media.id + "-" + filename);
        const fs::path copied = staging / relative;
        fs::copy_file(original, copied, fs::copy_options::none, pathError);
        if (pathError) {
            error = "unable to copy media '" + original.string() +
                    "': " + pathError.message();
            return false;
        }
        const uint64_t bytes = fs::file_size(copied, pathError);
        if (pathError) {
            error = "unable to inspect copied media: " + pathError.message();
            return false;
        }
        std::string digest;
        if (!Sha256(copied, digest, error)) return false;
        media.path = relative.generic_string();
        media.proxy_path.clear();
        if (DocumentSource* source = [&]() -> DocumentSource* {
                for (DocumentSource& candidate : collected.sources)
                    if (candidate.id == media.id) return &candidate;
                return nullptr;
            }())
            source->path = media.path;
        manifest.push_back({media.id, media.path, bytes, std::move(digest)});
        result.media_count++;
        result.media_bytes += bytes;
    }

    std::ofstream manifestOutput(staging / "manifest.json",
                                 std::ios::binary | std::ios::trunc);
    if (!manifestOutput) {
        error = "unable to create collection manifest";
        return false;
    }
    manifestOutput
        << "{\n  \"format\":\"cutmachine-collection\",\n"
        << "  \"version\":2,\n  \"project\":\"project.cutmachine.json\",\n"
        << "  \"timelines\":[";
    for (size_t index = 0; index < collected.timelines.size(); ++index) {
        const DocumentSequence& timeline = collected.timelines[index];
        manifestOutput << (index == 0 ? "\n" : ",\n") << "    {\"id\":\""
                       << EscapeJson(timeline.id) << "\",\"path\":\"Timelines/"
                       << EscapeJson(timeline.id) << ".json\"}";
    }
    if (!collected.timelines.empty()) manifestOutput << '\n';
    manifestOutput << "  ],\n"
                   << "  \"media\":[";
    for (size_t index = 0; index < manifest.size(); ++index) {
        const ManifestMedia& item = manifest[index];
        manifestOutput << (index == 0 ? "\n" : ",\n") << "    {\"id\":\""
                       << EscapeJson(item.id) << "\",\"path\":\""
                       << EscapeJson(item.path) << "\",\"bytes\":" << item.bytes
                       << ",\"sha256\":\"" << item.sha256 << "\"}";
    }
    if (!manifest.empty()) manifestOutput << '\n';
    manifestOutput << "  ]\n}\n";
    manifestOutput.close();
    if (!manifestOutput) {
        error = "unable to write collection manifest";
        return false;
    }

    const fs::path collectedProject = staging / "project.cutmachine.json";
    if (!CommitStoredProjectAndLogs(collectedProject.string(), collected,
                                    timelineLogs, projectLog, error))
        return false;

    Project verified;
    if (!LoadStoredProject(collectedProject.string(), verified, error)) {
        error = "collected project failed validation: " + error;
        return false;
    }
    for (const LibraryMedia& media : verified.rushes) {
        const fs::path relative(media.path);
        pathError.clear();
        if (relative.is_absolute() || HasParentTraversal(relative) ||
            !fs::is_regular_file(staging / relative, pathError) || pathError) {
            error =
                "collected project contains an unsafe or missing media path";
            return false;
        }
    }

    fs::rename(staging, destination, pathError);
    if (pathError) {
        error = "unable to publish collected project: " + pathError.message();
        return false;
    }
    cleanup.Commit();
    result.directory_path = destination.string();
    result.project_path = (destination / "project.cutmachine.json").string();
    error.clear();
    return true;
}
