#include "ResolveImport.h"

#include "Cli.h"
#include "EditLog.h"
#include "Ingest.h"
#include "Json.h"
#include "ProjectStorage.h"
#include "Ulid.h"

#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <vector>
#include <system_error>
#include <utility>

namespace {

std::string EscapeJson(const std::string& input) {
    return mcp_json::EscapeJsonString(input);
}

const mcp_json::Value* RequiredString(const mcp_json::Value& object,
                                      const std::string& key,
                                      const std::string& context,
                                      std::string& error) {
    const mcp_json::Value* value = object.Find(key);
    if (value == nullptr || !value->IsString()) {
        error = context + ": champ '" + key + "' absent ou non textuel";
        return nullptr;
    }
    return value;
}

std::string OptionalString(const mcp_json::Value& object,
                           const std::string& key) {
    const mcp_json::Value* value = object.Find(key);
    if (value == nullptr || !value->IsString()) return std::string();
    return value->AsString();
}

// Depth-first descent used only to order bin creation: a child's parent must
// already own a ULID when its AddBinOperation is emitted. Doubles as the cycle
// detector, since a key still on the stack when reached again closes a loop.
bool OrderBins(const std::vector<ResolveManifestBin>& bins,
               std::vector<size_t>& order, std::string& error) {
    std::map<std::string, size_t> byKey;
    for (size_t index = 0; index < bins.size(); ++index) {
        if (bins[index].key.empty()) {
            error = "clé de chutier vide";
            return false;
        }
        if (!byKey.emplace(bins[index].key, index).second) {
            error = "clé de chutier dupliquée : " + bins[index].key;
            return false;
        }
    }
    enum class Mark { None, Open, Done };
    std::map<std::string, Mark> marks;
    order.clear();
    order.reserve(bins.size());
    for (const ResolveManifestBin& bin : bins) {
        std::vector<std::string> stack{bin.key};
        while (!stack.empty()) {
            const std::string key = stack.back();
            Mark& mark = marks[key];
            if (mark == Mark::Done) {
                stack.pop_back();
                continue;
            }
            const size_t index = byKey.at(key);
            const std::string& parent = bins[index].parent_key;
            if (mark == Mark::Open) {
                mark = Mark::Done;
                order.push_back(index);
                stack.pop_back();
                continue;
            }
            mark = Mark::Open;
            if (parent.empty()) continue;
            const auto parentEntry = byKey.find(parent);
            if (parentEntry == byKey.end()) {
                error = "chutier '" + bins[index].name +
                        "' rattaché à un parent inconnu : " + parent;
                return false;
            }
            const auto parentMark = marks.find(parent);
            if (parentMark != marks.end() && parentMark->second == Mark::Open) {
                error = "cycle de chutiers sur la clé " + parent;
                return false;
            }
            stack.push_back(parent);
        }
    }
    return true;
}

struct SkippedClip {
    std::string file;
    std::string reason;
};

// A count alone is not a report: "skipped: 3" out of 400 rushes leaves the
// user to diff the Media Pool by hand. Same errors array as IngestCommand --
// and, as there, a rush already in the library is counted, never listed: it
// is the expected outcome of re-importing, not a failure.
std::string ResultJson(bool ok, size_t addedMedia, size_t knownMedia,
                       size_t addedBins, size_t reusedBins,
                       const std::string& detail,
                       const std::vector<SkippedClip>& skipped) {
    std::ostringstream output;
    output << "{\"ok\":" << (ok ? "true" : "false")
           << ",\"added\":" << addedMedia
           << ",\"skipped\":" << (knownMedia + skipped.size())
           << ",\"bins_created\":" << addedBins
           << ",\"bins_reused\":" << reusedBins << ",\"detail\":\""
           << EscapeJson(detail) << "\",\"errors\":[";
    for (size_t index = 0; index < skipped.size(); ++index) {
        if (index) output << ',';
        output << "{\"file\":\"" << EscapeJson(skipped[index].file)
               << "\",\"reason\":\"" << EscapeJson(skipped[index].reason)
               << "\"}";
    }
    output << "]}\n";
    return output.str();
}

// CommitStoredProjectAndLogs rewrites *every* log it is handed a project
// for, substituting an empty one for any timeline absent from the map. So the
// commit below has to carry the whole set, not just the log it changed --
// same shape as MutateProjectLogCommand in Cli.cc.
bool LoadOptionalLogs(const std::string& projectPath, const Project& project,
                      std::map<std::string, EditLog>& timelineLogs,
                      ProjectEditLog& projectLog, std::string& error) {
    EditError editError = EditError::None;
    for (const DocumentSequence& timeline : project.timelines) {
        EditLog log;
        const std::string path =
            TimelineEditLogPathForProject(projectPath, timeline.id);
        std::error_code existsError;
        if (std::filesystem::exists(path, existsError) &&
            !EditLog::Load(path, log, editError, error))
            return false;
        if (existsError) {
            error = "historique de timeline illisible : " +
                    existsError.message();
            return false;
        }
        timelineLogs.emplace(timeline.id, std::move(log));
    }
    const std::string projectLogPath =
        ProjectEditLogPathForProject(projectPath);
    std::error_code existsError;
    if (std::filesystem::exists(projectLogPath, existsError) &&
        !ProjectEditLog::Load(projectLogPath, projectLog, editError, error))
        return false;
    if (existsError) {
        error = "historique de projet illisible : " + existsError.message();
        return false;
    }
    return true;
}

bool ReadWholeFile(const std::filesystem::path& path, std::string& text,
                   std::string& error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "manifeste illisible : " + path.string();
        return false;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    text = buffer.str();
    return true;
}

}  // namespace

bool ParseResolveManifest(const std::string& json, ResolveManifest& manifest,
                          std::string& error) {
    mcp_json::Value root;
    if (!mcp_json::Value::Parse(json, root, error)) return false;
    if (!root.IsObject()) {
        error = "le manifeste doit être un objet JSON";
        return false;
    }
    const mcp_json::Value* schema =
        RequiredString(root, "schema", "manifeste", error);
    if (schema == nullptr) return false;
    if (schema->AsString() != kResolveManifestSchema) {
        error = "schéma inattendu : " + schema->AsString() + " (attendu " +
                std::string(kResolveManifestSchema) + ")";
        return false;
    }

    ResolveManifest parsed;
    parsed.project = OptionalString(root, "project");
    parsed.resolve_version = OptionalString(root, "resolve_version");

    const mcp_json::Value* bins = root.Find("bins");
    if (bins != nullptr && !bins->IsArray()) {
        error = "manifeste : 'bins' doit être un tableau";
        return false;
    }
    if (bins != nullptr) {
        for (const mcp_json::Value& entry : bins->AsArray()) {
            if (!entry.IsObject()) {
                error = "manifeste : un chutier n'est pas un objet";
                return false;
            }
            const mcp_json::Value* key =
                RequiredString(entry, "key", "chutier", error);
            if (key == nullptr) return false;
            const mcp_json::Value* name =
                RequiredString(entry, "name", "chutier " + key->AsString(),
                               error);
            if (name == nullptr) return false;
            if (name->AsString().empty()) {
                error = "chutier " + key->AsString() + " : nom vide";
                return false;
            }
            ResolveManifestBin bin;
            bin.key = key->AsString();
            bin.name = name->AsString();
            bin.parent_key = OptionalString(entry, "parent_key");
            parsed.bins.push_back(std::move(bin));
        }
    }

    std::vector<size_t> order;
    if (!OrderBins(parsed.bins, order, error)) return false;

    std::set<std::string> knownKeys;
    for (const ResolveManifestBin& bin : parsed.bins) knownKeys.insert(bin.key);

    const mcp_json::Value* clips = root.Find("clips");
    if (clips != nullptr && !clips->IsArray()) {
        error = "manifeste : 'clips' doit être un tableau";
        return false;
    }
    if (clips != nullptr) {
        for (const mcp_json::Value& entry : clips->AsArray()) {
            if (!entry.IsObject()) {
                error = "manifeste : un rush n'est pas un objet";
                return false;
            }
            const mcp_json::Value* path =
                RequiredString(entry, "path", "rush", error);
            if (path == nullptr) return false;
            if (path->AsString().empty()) {
                error = "rush sans chemin de fichier";
                return false;
            }
            ResolveManifestClip clip;
            clip.path = path->AsString();
            clip.name = OptionalString(entry, "name");
            clip.bin_key = OptionalString(entry, "bin_key");
            clip.resolve_uid = OptionalString(entry, "resolve_uid");
            if (!clip.bin_key.empty() && knownKeys.count(clip.bin_key) == 0) {
                error = "rush '" + clip.path +
                        "' rattaché à un chutier inconnu : " + clip.bin_key;
                return false;
            }
            parsed.clips.push_back(std::move(clip));
        }
    }

    manifest = std::move(parsed);
    return true;
}

bool PlanResolveImport(const Document& document,
                       const ResolveManifest& manifest,
                       ResolveImportPlan& plan, std::string& error) {
    std::vector<size_t> order;
    if (!OrderBins(manifest.bins, order, error)) return false;

    ResolveImportPlan built;
    built.bin_ids.emplace(std::string(), Ulid());
    for (const size_t index : order) {
        const ResolveManifestBin& bin = manifest.bins[index];
        const auto parent = built.bin_ids.find(bin.parent_key);
        if (parent == built.bin_ids.end()) {
            error = "chutier '" + bin.name + "' : parent non résolu";
            return false;
        }
        const DocumentBin* existing = nullptr;
        for (const DocumentBin& candidate : document.bins) {
            if (candidate.name == bin.name &&
                candidate.parent_id == parent->second) {
                existing = &candidate;
                break;
            }
        }
        if (existing != nullptr) {
            built.bin_ids.emplace(bin.key, existing->id);
            ++built.reused_bins;
            continue;
        }
        AddBinOperation operation;
        operation.bin_id = GenerateUlid();
        operation.name = bin.name;
        operation.parent_id = parent->second;
        built.bin_ids.emplace(bin.key, operation.bin_id);
        built.new_bins.push_back(std::move(operation));
    }
    plan = std::move(built);
    return true;
}

int ImportResolveCommand(const std::string& projectPath,
                         const std::string& manifestPath,
                         std::string& output) {
    std::string reason;
    std::string manifestText;
    const std::vector<SkippedClip> none;
    if (!ReadWholeFile(std::filesystem::path(manifestPath), manifestText,
                       reason)) {
        output = ResultJson(false, 0, 0, 0, 0, reason, none);
        return 1;
    }
    ResolveManifest manifest;
    if (!ParseResolveManifest(manifestText, manifest, reason)) {
        output = ResultJson(false, 0, 0, 0, 0, reason, none);
        return 1;
    }

    Project project;
    if (!LoadStoredProject(projectPath, project, reason)) {
        output = ResultJson(false, 0, 0, 0, 0, reason, none);
        return 1;
    }
    Document document = project.MakeActiveDocument();

    ResolveImportPlan plan;
    if (!PlanResolveImport(document, manifest, plan, reason)) {
        output = ResultJson(false, 0, 0, 0, 0, reason, none);
        return 1;
    }

    std::map<std::string, EditLog> logs;
    ProjectEditLog projectLog;
    if (!LoadOptionalLogs(projectPath, project, logs, projectLog, reason)) {
        output = ResultJson(false, 0, 0, 0, 0, reason, none);
        return 1;
    }
    EditLog& log = logs[project.active_timeline_id];

    EditError error = EditError::None;
    for (const AddBinOperation& operation : plan.new_bins) {
        if (!log.Apply(document, operation, error, reason)) {
            output = ResultJson(false, 0, 0, 0, plan.reused_bins, reason,
                                none);
            return 1;
        }
    }

    const std::filesystem::path projectRoot =
        std::filesystem::path(projectPath).parent_path();
    std::map<std::string, size_t> knownPaths;
    for (size_t index = 0; index < document.library.size(); ++index) {
        std::filesystem::path stored(document.library[index].path);
        if (stored.is_relative()) stored = projectRoot / stored;
        std::error_code pathError;
        const std::filesystem::path resolved =
            std::filesystem::weakly_canonical(stored, pathError);
        if (!pathError) knownPaths.emplace(resolved.string(), index);
    }

    size_t added = 0;
    size_t alreadyPresent = 0;
    std::vector<SkippedClip> skipped;
    for (const ResolveManifestClip& clip : manifest.clips) {
        std::error_code pathError;
        const std::filesystem::path absolute =
            std::filesystem::weakly_canonical(std::filesystem::path(clip.path),
                                              pathError);
        if (pathError) {
            skipped.push_back({clip.path, pathError.message()});
            continue;
        }
        const Ulid& binId = plan.bin_ids[clip.bin_key];
        const auto known = knownPaths.find(absolute.string());
        if (known != knownPaths.end()) {
            ++alreadyPresent;
            SetMediaBinOperation filing;
            filing.media_id = document.library[known->second].id;
            filing.bin_id = binId;
            if (document.library[known->second].bin_id != binId &&
                !log.Apply(document, filing, error, reason)) {
                output =
                    ResultJson(false, added, alreadyPresent,
                               plan.new_bins.size(), plan.reused_bins, reason,
                               skipped);
                return 1;
            }
            continue;
        }
        LibraryMedia media;
        media.id = GenerateUlid();
        media.filename = absolute.filename().string();
        std::error_code relativeError;
        media.path = std::filesystem::relative(absolute, projectRoot,
                                               relativeError)
                         .lexically_normal()
                         .string();
        if (relativeError || media.path.empty()) media.path = absolute.string();
        if (!ProbeMediaMetadata(absolute.string(), media, reason)) {
            skipped.push_back({media.filename, reason});
            continue;
        }
        document.library.push_back(media);
        knownPaths.emplace(absolute.string(), document.library.size() - 1);
        if (!document.FindSource(media.id)) {
            document.sources.push_back(
                {media.id, media.path, media.rate, media.duration});
        }
        ++added;
        if (!binId.empty()) {
            SetMediaBinOperation filing;
            filing.media_id = media.id;
            filing.bin_id = binId;
            if (!log.Apply(document, filing, error, reason)) {
                output =
                    ResultJson(false, added, alreadyPresent,
                               plan.new_bins.size(), plan.reused_bins, reason,
                               skipped);
                return 1;
            }
        }
    }

    if (!document.Validate(reason)) {
        output = ResultJson(false, added, alreadyPresent, plan.new_bins.size(),
                            plan.reused_bins, reason, skipped);
        return 1;
    }
    if (!project.CommitActiveDocument(document, reason) ||
        !CommitStoredProjectAndLogs(projectPath, project, logs, projectLog,
                                    reason)) {
        output = ResultJson(false, added, alreadyPresent, plan.new_bins.size(),
                            plan.reused_bins, reason, skipped);
        return 1;
    }
    output = ResultJson(true, added, alreadyPresent, plan.new_bins.size(),
                        plan.reused_bins, manifest.project, skipped);
    return 0;
}
