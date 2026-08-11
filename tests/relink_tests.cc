#include "EditLog.h"
#include "Relink.h"

#include <iostream>

namespace {

int failures = 0;

void Check(bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

Document Fixture() {
    Document document;
    LibraryMedia media;
    media.id = "01K00000000000000000000001";
    media.path = "offline.mov";
    media.filename = "offline.mov";
    media.codec = "h264";
    media.width = 1920;
    media.height = 1080;
    media.rate = {25, 1};
    media.duration = {100, 25};
    media.orientation = "landscape";
    media.has_audio = true;
    media.audio_rate = 48000;
    media.audio_channels = 2;
    media.proxy_path = ".cutmachine/proxies/old.mov";
    media.metadata_complete = true;
    document.library.push_back(media);
    document.sources.push_back(
        {media.id, media.path, media.rate, media.duration});
    DocumentTrack audio;
    audio.id = "01K00000000000000000000002";
    audio.kind = "audio";
    audio.clips.push_back({"01K00000000000000000000003",
                           media.id,
                           {0, 25},
                           {50, 25},
                           {0, 25},
                           false});
    document.sequence.tracks.push_back(audio);
    return document;
}

}  // namespace

int main() {
    Project project = Project::FromDocument(Fixture());
    LibraryMedia replacement = project.rushes[0];
    replacement.path = "/new/camera.mov";
    replacement.filename = "camera.mov";
    replacement.codec = "prores";
    replacement.width = 3840;
    replacement.height = 2160;
    replacement.duration = {125, 25};
    replacement.proxy_path = "must-not-survive";
    std::string error;
    EditError editError = EditError::None;
    ProjectEditLog projectLog;
    const std::string beforeRelink = project.SaveToString();
    Check(projectLog.Apply(
              project,
              ProjectOperation{RelinkProjectMediaOperation{
                  {{replacement.id, replacement, "media/camera.mov"}}}},
              editError, error),
          "compatible replacement applies");
    const std::string afterRelink = project.SaveToString();
    Check(
        project.sources[0].id == replacement.id &&
            project.sources[0].path == "media/camera.mov" &&
            project.rushes[0].path == "media/camera.mov" &&
            project.rushes[0].proxy_path.empty() &&
            project.timelines[0].tracks[0].clips[0].source_id == replacement.id,
        "relink preserves identity and clips while invalidating proxy");
    Check(projectLog.Undo(project, editError, error), "relink undo applies");
    Check(project.SaveToString() == beforeRelink,
          "relink undo restores byte-identical project JSON");
    Check(projectLog.Redo(project, editError, error), "relink redo applies");
    Check(project.SaveToString() == afterRelink,
          "relink redo is deterministic");

    ProjectOperation serializedRelink;
    Check(DeserializeProjectOperation(
              SerializeProjectOperation(projectLog.AppliedEntries().back().op),
              serializedRelink, editError, error),
          "relink operation serializes");

    Project shorter = Project::FromDocument(Fixture());
    LibraryMedia shortMedia = shorter.rushes[0];
    shortMedia.duration = {99, 25};
    ProjectEditLog shorterLog;
    Check(!shorterLog.Apply(shorter,
                            ProjectOperation{RelinkProjectMediaOperation{
                                {{shortMedia.id, shortMedia, "short.mov"}}}},
                            editError, error) &&
              shorter.sources[0].path == "offline.mov",
          "short replacement is rejected transactionally");

    Document silent = Fixture();
    LibraryMedia silentMedia = silent.library[0];
    silentMedia.has_audio = false;
    silentMedia.audio_rate = 0;
    silentMedia.audio_channels = 0;
    Check(!ValidateRelinkCandidate(silent, silentMedia.id, silentMedia, error),
          "silent replacement cannot orphan existing audio clips");

    Document cadence = Fixture();
    LibraryMedia differentRate = cadence.library[0];
    differentRate.rate = {24, 1};
    Check(!ValidateRelinkCandidate(cadence, differentRate.id, differentRate,
                                   error),
          "different frame rate is rejected");

    Project batch = Project::FromDocument(Fixture());
    LibraryMedia batchValid = batch.rushes[0];
    batchValid.duration = {125, 25};
    LibraryMedia batchInvalid = batchValid;
    batchInvalid.rate = {24, 1};
    const std::vector<ProjectRelinkItem> replacements = {
        {batchValid.id, batchValid, "valid.mov"},
        {batchInvalid.id, batchInvalid, "invalid.mov"},
    };
    ProjectEditLog batchLog;
    Check(
        !batchLog.Apply(
            batch, ProjectOperation{RelinkProjectMediaOperation{replacements}},
            editError, error) &&
            batch.sources[0].path == "offline.mov",
        "batch relink is atomic when a replacement becomes invalid");

    if (failures) return 1;
    std::cout << "PASS: relink compatibility and transactional identity\n";
    return 0;
}
