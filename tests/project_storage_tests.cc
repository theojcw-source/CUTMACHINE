#include "Cli.h"
#include "Document.h"
#include "EditLog.h"
#include "Operations.h"
#include "Project.h"
#include "ProjectStorage.h"
#include "Ulid.h"

#include <sys/stat.h>

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

std::string Hex(const std::string& input) {
    constexpr char kDigits[] = "0123456789abcdef";
    std::string output;
    for (const unsigned char byte : input) {
        output.push_back(kDigits[byte >> 4]);
        output.push_back(kDigits[byte & 0xf]);
    }
    return output;
}

std::string MediaManifestSection(const std::string& manifest) {
    const size_t begin = manifest.find("\"media\":");
    const size_t end = manifest.rfind(']');
    if (begin == std::string::npos || end == std::string::npos || end < begin)
        return {};
    return manifest.substr(begin, end - begin + 1);
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
    CollectionIntegrityReport sourceIntegrity;
    Check(VerifyPortableProject(sourceProject, sourceIntegrity, error) &&
              sourceIntegrity.verified_media == 0 &&
              sourceIntegrity.missing_media.empty() &&
              sourceIntegrity.modified_media.empty(),
          "external media absent from the collection manifest is not "
          "reported as modified: " +
              error);

    // PERF-2026-09. Every commit is handed the whole package, but only the
    // artifacts whose bytes actually change may be rewritten: each rewrite
    // costs an F_FULLFSYNC plus two directory syncs, and on an external
    // exFAT volume that is what makes a single edit visibly stall the
    // editor. A rewrite renames the destination aside and moves a fresh file
    // into place, so the inode is the exact observable: it survives a skip
    // and never survives a rewrite.
    const auto inodeOf = [](const fs::path& path) -> ino_t {
        struct stat status = {};
        return ::stat(path.c_str(), &status) == 0 ? status.st_ino : 0;
    };
    const fs::path package = fs::path(sourceProject).parent_path();
    const std::string editedTimeline = project.timelines.front().id;
    const std::string untouchedTimeline = project.timelines.back().id;
    const fs::path manifestPath = package / "manifest.json";
    const fs::path editedPath =
        package / "Timelines" / (editedTimeline + ".json");
    const fs::path untouchedPath =
        package / "Timelines" / (untouchedTimeline + ".json");
    const fs::path editedLogPath =
        TimelineEditLogPathForProject(sourceProject, editedTimeline);
    const fs::path untouchedLogPath =
        TimelineEditLogPathForProject(sourceProject, untouchedTimeline);
    const fs::path projectLogPath =
        ProjectEditLogPathForProject(sourceProject);

    const ino_t manifestInode = inodeOf(manifestPath);
    const ino_t untouchedInode = inodeOf(untouchedPath);
    const ino_t untouchedLogInode = inodeOf(untouchedLogPath);
    const ino_t projectLogInode = inodeOf(projectLogPath);
    const ino_t editedInode = inodeOf(editedPath);
    const ino_t editedLogInode = inodeOf(editedLogPath);
    Check(manifestInode != 0 && untouchedInode != 0 && editedInode != 0 &&
              untouchedLogInode != 0 && projectLogInode != 0 &&
              editedLogInode != 0,
          "every package artifact exists before the incremental commit");

    Check(CommitStoredProjectAndLogs(sourceProject, project, logs, projectLog,
                                     error),
          "re-committing an unchanged project succeeds: " + error);
    Check(inodeOf(fs::path(sourceProject)) != 0 &&
              inodeOf(manifestPath) == manifestInode &&
              inodeOf(editedPath) == editedInode &&
              inodeOf(untouchedPath) == untouchedInode &&
              inodeOf(editedLogPath) == editedLogInode &&
              inodeOf(untouchedLogPath) == untouchedLogInode &&
              inodeOf(projectLogPath) == projectLogInode,
          "a commit that changes nothing rewrites nothing");

    // Edited on copies, and reverted below: everything after this block
    // reads the package back from disk and expects the pristine fixture.
    Project incrementalProject = project;
    std::map<std::string, EditLog> incrementalLogs = logs;
    Document editedDocument = incrementalProject.MakeDocument(editedTimeline);
    EditError incrementalError = EditError::None;
    std::string incrementalDetail;
    DocumentMarker incrementalMarker;
    incrementalMarker.name = "Incremental";
    incrementalMarker.time = {50, 25};
    Check(incrementalLogs.at(editedTimeline)
              .Apply(editedDocument, AddMarkerOperation{incrementalMarker},
                     incrementalError, incrementalDetail),
          "incremental fixture edit applies: " + incrementalDetail);
    Check(incrementalProject.CommitDocument(editedTimeline, editedDocument,
                                            incrementalDetail),
          "incremental fixture edit stages: " + incrementalDetail);
    Check(CommitStoredProjectAndLogs(sourceProject, incrementalProject,
                                     incrementalLogs, projectLog, error),
          "committing one edited timeline succeeds: " + error);
    Check(inodeOf(editedPath) != editedInode &&
              inodeOf(editedLogPath) != editedLogInode,
          "the edited timeline and its journal are rewritten");
    Check(inodeOf(manifestPath) == manifestInode &&
              inodeOf(untouchedPath) == untouchedInode &&
              inodeOf(untouchedLogPath) == untouchedLogInode &&
              inodeOf(projectLogPath) == projectLogInode,
          "the manifest, the other timeline, its journal and the project "
          "journal are left untouched");

    Project reloadedIncremental;
    Check(LoadStoredProject(sourceProject, reloadedIncremental, error) &&
              reloadedIncremental.MakeDocument(editedTimeline)
                      .sequence.markers.size() ==
                  editedDocument.sequence.markers.size(),
          "the incrementally committed generation reloads with its edit: " +
              error);
    Check(CommitStoredProjectAndLogs(sourceProject, project, logs, projectLog,
                                     error),
          "the pristine fixture is restored for the checks below: " + error);

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
    const std::string collectedMediaManifest =
        MediaManifestSection(Read(destination / "manifest.json"));
    Project withTransientTimeline = collected;
    DocumentSequence transientTimeline;
    transientTimeline.id = "01K50000000000000000000004";
    transientTimeline.name = "Temporaire";
    withTransientTimeline.timelines.push_back(transientTimeline);
    Check(
        CommitStoredProject(result.project_path, withTransientTimeline, error),
        "adding a timeline commits package metadata: " + error);
    Check(Read(destination / "manifest.json").find(transientTimeline.id) !=
                  std::string::npos &&
              fs::is_regular_file(destination / "Timelines" /
                                  (transientTimeline.id + ".json")) &&
              fs::is_regular_file(TimelineEditLogPathForProject(
                  result.project_path, transientTimeline.id)),
          "manifest, physical timeline, and history advance together");
    Check(CommitStoredProject(result.project_path, collected, error),
          "removing a timeline commits package metadata: " + error);
    Check(Read(destination / "manifest.json").find(transientTimeline.id) ==
                  std::string::npos &&
              !fs::exists(destination / "Timelines" /
                          (transientTimeline.id + ".json")) &&
              !fs::exists(TimelineEditLogPathForProject(result.project_path,
                                                        transientTimeline.id)),
          "obsolete timeline state and history are removed atomically");

    // macOS writes an AppleDouble "._<name>" sidecar next to every file on an
    // exFAT or FAT volume -- the format of most shared media drives. Those
    // sidecars cannot be renamed on their own, so treating one as an obsolete
    // timeline made every save to a project stored on such a drive fail.
    const fs::path sidecar = destination / "Timelines" /
                             ("._" + collected.timelines.front().id + ".json");
    const fs::path stranger = destination / "Timelines" / "notes.json";
    Write(sidecar, "apple-double");
    Write(stranger, "{}");
    Check(CommitStoredProject(result.project_path, collected, error),
          "a package holding AppleDouble sidecars still commits: " + error);
    Check(fs::exists(sidecar) && Read(sidecar) == "apple-double" &&
              fs::exists(stranger),
          "the transaction never touches files it did not write");
    fs::remove(sidecar);
    fs::remove(stranger);
    Check(MediaManifestSection(Read(destination / "manifest.json")) ==
              collectedMediaManifest,
          "timeline commits preserve collected-media paths and fingerprints");
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

    // A path with no manifest.json beside it has two very different causes,
    // and the message has to say which: a single-file project from before
    // the package format (the file is there, the package is not) versus a
    // path that simply is not there. Reporting "collection manifest is
    // missing" for both describes our bookkeeping instead of the mistake.
    const fs::path legacyProject = root / "example-timeline.json";
    Write(legacyProject, "{\"version\":3,\"sequence\":{}}\n");
    CollectionIntegrityReport legacyReport;
    std::string legacyError;
    Check(!VerifyPortableProject(legacyProject.string(), legacyReport,
                                 legacyError) &&
              legacyError.find("cutmachine-project") != std::string::npos,
          "a single-file project is refused by naming the package "
          "requirement, not the missing manifest: " +
              legacyError);
    CollectionIntegrityReport absentReport;
    std::string absentError;
    Check(!VerifyPortableProject((root / "nope.cutmachine.json").string(),
                                 absentReport, absentError) &&
              absentError.find("no project file at") != std::string::npos,
          "a path that does not exist says so rather than blaming the "
          "manifest: " +
              absentError);
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

    const fs::path interrupted = root / "Interrupted.cutmachine-project";
    fs::create_directories(interrupted / "Timelines");
    const std::string nonce = ".cutmachine-01K50000000000000000000005";
    const fs::path replaced = interrupted / "project.cutmachine.json";
    const fs::path created = interrupted / "new.editlog.json";
    const fs::path removed = interrupted / "Timelines/removed.json";
    Write(replaced, "partially-published-new");
    Write(replaced.string() + nonce + ".bak", "previous-project");
    Write(created, "partially-published-new-log");
    Write(removed.string() + nonce + ".bak", "previous-timeline");
    Write(created.string() + nonce + ".tmp", "unused-temporary");
    std::ostringstream transaction;
    transaction << "CUTMACHINE_TRANSACTION_V1\n"
                << nonce << "\n1 " << Hex("project.cutmachine.json") << "\n0 "
                << Hex("new.editlog.json") << "\n1 "
                << Hex("Timelines/removed.json") << '\n';
    Write(interrupted / ".cutmachine-transaction", transaction.str());
    Check(RecoverTextArtifactTransaction(interrupted.string(), error),
          "an interrupted multi-file commit is recoverable: " + error);
    Check(Read(replaced) == "previous-project" && !fs::exists(created) &&
              Read(removed) == "previous-timeline" &&
              !fs::exists(interrupted / ".cutmachine-transaction") &&
              !fs::exists(created.string() + nonce + ".tmp"),
          "recovery restores the complete previous generation and cleans "
          "transaction files");
    Write(replaced.string() + nonce + ".bak", "stale-backup");
    Check(RecoverTextArtifactTransaction(interrupted.string(), error) &&
              !fs::exists(replaced.string() + nonce + ".bak"),
          "leftovers from an already committed generation are cleaned");

    fs::remove_all(root);
    if (failures) {
        std::cerr << failures << " assertion(s) failed\n";
        return 1;
    }
    std::cout << "project storage tests passed\n";
    return 0;
}
