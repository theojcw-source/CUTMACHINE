#include "Cli.h"
#include "Document.h"
#include "EditLog.h"
#include "Operations.h"
#include "Project.h"
#include "ProjectStorage.h"
#include "Ulid.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>

namespace {

int failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

void Write(const std::filesystem::path& path, const std::string& contents) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << contents;
}

std::string Read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream output;
    output << input.rdbuf();
    return output.str();
}

}  // namespace

int main() {
    namespace fs = std::filesystem;
    const fs::path root =
        fs::temp_directory_path() / (GenerateUlid() + "-project-storage-tests");
    const fs::path sourceDirectory = root / "Film.cutmachine-project";
    Write(root / "rushes/a/shared.mov", "first-media");
    Write(root / "rushes/b/shared.mov", "second-media");

    Document document;
    LibraryMedia first;
    first.id = "01K50000000000000000000001";
    first.path = (root / "rushes/a/shared.mov").string();
    first.filename = "shared.mov";
    first.rate = {25, 1};
    first.duration = {100, 25};
    first.metadata_complete = false;
    LibraryMedia second = first;
    second.id = "01K50000000000000000000002";
    second.path = (root / "rushes/b/shared.mov").string();
    document.library = {first, second};
    document.sources = {
        {first.id, first.path, first.rate, first.duration},
        {second.id, second.path, second.rate, second.duration},
    };
    Project project = Project::FromDocument(document, "Portable Film");
    DocumentSequence secondTimeline;
    secondTimeline.id = "01K50000000000000000000003";
    secondTimeline.name = "Vertical";
    secondTimeline.width = 1080;
    secondTimeline.height = 1920;
    project.timelines.push_back(secondTimeline);

    std::map<std::string, EditLog> logs;
    for (const DocumentSequence& timeline : project.timelines) {
        Document timelineDocument = project.MakeDocument(timeline.id);
        EditLog log;
        EditError editError = EditError::None;
        std::string detail;
        DocumentMarker marker;
        marker.name = timeline.name;
        Check(log.Apply(timelineDocument, AddMarkerOperation{marker}, editError,
                        detail),
              "timeline history fixture applies: " + detail);
        logs.emplace(timeline.id, std::move(log));
    }
    ProjectEditLog projectLog;
    std::string error;
    std::string sourceProject;
    Check(CreatePortableProject(sourceDirectory.string(), project,
                                sourceProject, error),
          "source project package is created: " + error);
    Check(CommitStoredProjectAndLogs(sourceProject, project, logs, projectLog,
                                     error),
          "source project and histories save: " + error);

    const fs::path destination = root / "Portable Film.cutmachine-project";
    PortableProjectResult result;
    Check(CollectPortableProject(sourceProject, destination.string(), result,
                                 error),
          "portable collection succeeds: " + error);
    Check(result.media_count == 2 && result.media_bytes == 23,
          "collection reports copied media count and bytes");
    Check(result.project_path ==
              (destination / "project.cutmachine.json").string(),
          "collection reports canonical inner project");

    Project collected;
    Check(LoadStoredProject(result.project_path, collected, error),
          "collected project loads: " + error);
    Check(IsPortableProjectV2(result.project_path),
          "collection publishes the separated-timeline v2 format");
    for (const DocumentSequence& timeline : collected.timelines)
        Check(fs::is_regular_file(destination / "Timelines" /
                                  (timeline.id + ".json")),
              "each v2 timeline has its own physical file");
    Check(collected.rushes.size() == 2,
          "collected project retains every media identity");
    for (const LibraryMedia& media : collected.rushes) {
        Check(!fs::path(media.path).is_absolute() &&
                  media.path.rfind("Media/", 0) == 0,
              "collected media path is internal and relative");
        Check(fs::is_regular_file(destination / media.path),
              "collected media exists at rewritten path");
        Check(media.proxy_path.empty(), "collection clears derived proxy path");
    }
    Check(collected.rushes[0].path != collected.rushes[1].path,
          "same-name originals cannot collide in the collection");
    Check(Read(destination / "manifest.json").find("\"sha256\"") !=
              std::string::npos,
          "collection manifest fingerprints originals");
    const fs::path rejectedPackage = root / "Rejected.cutmachine-project";
    fs::copy(destination, rejectedPackage, fs::copy_options::recursive);
    std::string rejectedManifest = Read(rejectedPackage / "manifest.json");
    const size_t v2 = rejectedManifest.find("\"version\":2");
    Check(v2 != std::string::npos, "v2 manifest fixture is recognizable");
    rejectedManifest.replace(v2, std::string("\"version\":2").size(),
                             "\"version\":1");
    Write(rejectedPackage / "manifest.json", rejectedManifest);
    Project rejected;
    Check(!LoadStoredProject(
              (rejectedPackage / "project.cutmachine.json").string(), rejected,
              error),
          "v1 packages are rejected instead of migrated");
    CollectionIntegrityReport integrity;
    Check(VerifyPortableProject(result.project_path, integrity, error) &&
              integrity.verified_media == 2 &&
              integrity.missing_media.empty() &&
              integrity.modified_media.empty(),
          "fresh collection passes full integrity verification: " + error);
    for (const DocumentSequence& timeline : collected.timelines) {
        EditLog log;
        EditError editError = EditError::None;
        std::string detail;
        Check(EditLog::Load(TimelineEditLogPathForProject(result.project_path,
                                                          timeline.id),
                            log, editError, detail) &&
                  log.AppliedCount() == 1,
              "each timeline history survives collection: " + detail);
    }

    Document authoritative;
    authoritative.sequence = collected.timelines.front();
    authoritative.sequence.name = "Nom physique autoritaire";
    authoritative.color_management = collected.settings.color_management;
    Write(destination / "Timelines" / (authoritative.sequence.id + ".json"),
          authoritative.SaveToString());
    Project reloadedAuthority;
    Check(
        LoadStoredProject(result.project_path, reloadedAuthority, error) &&
            reloadedAuthority.timelines.front().name ==
                "Nom physique autoritaire",
        "v2 physical timeline overrides the embedded compatibility snapshot: " +
            error);

    PortableProjectResult duplicate;
    Check(!CollectPortableProject(sourceProject, destination.string(),
                                  duplicate, error) &&
              error == "destination already exists",
          "collection never overwrites an existing destination");

    Write(destination / collected.rushes.front().path, "altered-media");
    Check(VerifyPortableProject(result.project_path, integrity, error) &&
              integrity.modified_media.size() == 1,
          "same-sized or changed media is detected by size or SHA-256");

    ProjectSessionLock firstLock;
    ProjectSessionLock secondLock;
    std::string owner;
    Check(firstLock.Acquire(sourceProject, owner, error) && firstLock.Held(),
          "first writer acquires the project lock: " + error);
    Check(!secondLock.Acquire(sourceProject, owner, error) && !owner.empty(),
          "second writer is rejected with lock owner information");
    firstLock.Release();
    Check(secondLock.Acquire(sourceProject, owner, error),
          "lock can be reacquired after clean release: " + error);
    secondLock.Release();

    fs::remove_all(root);
    if (failures) {
        std::cerr << failures << " assertion(s) failed\n";
        return 1;
    }
    std::cout << "project storage tests passed\n";
    return 0;
}
