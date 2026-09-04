#include "Cli.h"

#include "Document.h"
#include "DocumentDelta.h"
#include "EditLog.h"
#include "Export.h"
#include "InterviewShort.h"
#include "Json.h"
#include "Operations.h"
#include "PauseTightening.h"
#include "Project.h"
#include "ProjectStorage.h"
#include "SequenceFormat.h"
#include "ShotQuality.h"
#include "SourceAddress.h"
#include "SpeechOnset.h"
#include "Subtitles.h"
#include "Timeline.h"
#include "TimelineStats.h"
#include "TranscriptAlignment.h"
#include "Transcription.h"
#include "Ulid.h"

#include <fcntl.h>
#include <unistd.h>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

int FailCliCommand(const std::string& errorCode, const std::string& detail,
                   std::string& output, int exitStatus) {
    output = "{\"ok\":false,\"error\":\"" +
             mcp_json::EscapeJsonString(errorCode) + "\",\"detail\":\"" +
             mcp_json::EscapeJsonString(detail) + "\"}\n";
    return exitStatus;
}

namespace {

std::string EscapeJson(const std::string& input) {
    std::ostringstream output;
    for (const unsigned char character : input) {
        switch (character) {
            case '"':
                output << "\\\"";
                break;
            case '\\':
                output << "\\\\";
                break;
            case '\b':
                output << "\\b";
                break;
            case '\f':
                output << "\\f";
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
                if (character < 0x20) {
                    const char digits[] = "0123456789abcdef";
                    output << "\\u00" << digits[character >> 4]
                           << digits[character & 0xf];
                } else {
                    output << static_cast<char>(character);
                }
        }
    }
    return output.str();
}

std::string ErrorJson(EditError error, const std::string& detail) {
    std::string output;
    FailCliCommand(EditErrorName(error), detail, output);
    return output;
}

bool ReadFile(const std::string& path, std::string& contents,
              std::string& message) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        message = "unable to open '" + path + "'";
        return false;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    if (!input.good() && !input.eof()) {
        message = "unable to read '" + path + "'";
        return false;
    }
    contents = buffer.str();
    return true;
}

bool WriteFile(const std::filesystem::path& path, const std::string& contents,
               std::string& message) {
    const int descriptor =
        open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (descriptor < 0) {
        message = "unable to create '" + path.string() + "'";
        return false;
    }
    size_t written = 0;
    while (written < contents.size()) {
        const ssize_t count = write(descriptor, contents.data() + written,
                                    contents.size() - written);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            const int savedError = errno;
            close(descriptor);
            message = "unable to write '" + path.string() +
                      "': " + std::strerror(savedError);
            return false;
        }
        written += static_cast<size_t>(count);
    }
    // fsync is the portable baseline. macOS may still defer drive-cache
    // flushing, so request F_FULLFSYNC there when the filesystem supports it.
#ifdef F_FULLFSYNC
    if (fcntl(descriptor, F_FULLFSYNC) != 0 && fsync(descriptor) != 0) {
#else
    if (fsync(descriptor) != 0) {
#endif
        const int savedError = errno;
        close(descriptor);
        message = "unable to synchronize '" + path.string() +
                  "': " + std::strerror(savedError);
        return false;
    }
    if (close(descriptor) != 0) {
        message =
            "unable to close '" + path.string() + "': " + std::strerror(errno);
        return false;
    }
    return true;
}

bool SyncDirectory(const std::filesystem::path& path, std::string& message) {
    const int descriptor = open(path.c_str(), O_RDONLY);
    if (descriptor < 0) {
        message = "unable to open directory '" + path.string() +
                  "': " + std::strerror(errno);
        return false;
    }
    if (fsync(descriptor) != 0) {
        const int savedError = errno;
        close(descriptor);
        message = "unable to synchronize directory '" + path.string() +
                  "': " + std::strerror(savedError);
        return false;
    }
    if (close(descriptor) != 0) {
        message = "unable to close directory '" + path.string() +
                  "': " + std::strerror(errno);
        return false;
    }
    return true;
}

void RemoveIfPresent(const std::filesystem::path& path) {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

bool Rename(const std::filesystem::path& from, const std::filesystem::path& to,
            std::string& message) {
    std::error_code error;
    std::filesystem::rename(from, to, error);
    if (!error) return true;
    message = "unable to rename '" + from.string() + "' to '" + to.string() +
              "': " + error.message();
    return false;
}

struct CommitArtifact {
    std::filesystem::path path;
    std::string contents;
    std::filesystem::path temporary;
    std::filesystem::path backup;
    bool existed = false;
    bool backed_up = false;
    bool committed = false;
    bool remove = false;
};

std::string HexEncode(const std::string& input) {
    constexpr char kDigits[] = "0123456789abcdef";
    std::string output;
    output.reserve(input.size() * 2);
    for (const unsigned char byte : input) {
        output.push_back(kDigits[byte >> 4]);
        output.push_back(kDigits[byte & 0xf]);
    }
    return output;
}

bool HexDecode(const std::string& input, std::string& output) {
    if (input.size() % 2 != 0) return false;
    const auto digit = [](char value) -> int {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        return -1;
    };
    output.clear();
    output.reserve(input.size() / 2);
    for (size_t index = 0; index < input.size(); index += 2) {
        const int high = digit(input[index]);
        const int low = digit(input[index + 1]);
        if (high < 0 || low < 0) return false;
        output.push_back(static_cast<char>((high << 4) | low));
    }
    return true;
}

std::filesystem::path CommonDirectory(
    const std::vector<CommitArtifact>& artifacts) {
    std::filesystem::path common =
        std::filesystem::absolute(artifacts.front().path).parent_path();
    for (size_t index = 1; index < artifacts.size(); ++index) {
        const std::filesystem::path parent =
            std::filesystem::absolute(artifacts[index].path).parent_path();
        while (!common.empty()) {
            const std::filesystem::path relative =
                parent.lexically_relative(common);
            if (!relative.empty() && *relative.begin() != "..") break;
            if (parent == common) break;
            common = common.parent_path();
        }
    }
    return common;
}

bool IsWithinDirectory(const std::filesystem::path& directory,
                       const std::filesystem::path& path) {
    std::error_code error;
    const std::filesystem::path canonicalDirectory =
        std::filesystem::weakly_canonical(directory, error);
    if (error) return false;
    const std::filesystem::path canonicalParent =
        std::filesystem::weakly_canonical(path.parent_path(), error);
    if (error) return false;
    const std::filesystem::path relative =
        canonicalParent.lexically_relative(canonicalDirectory);
    return canonicalParent == canonicalDirectory ||
           (!relative.empty() && *relative.begin() != "..");
}

bool IsTransactionLeftover(const std::filesystem::path& path) {
    const std::string filename = path.filename().string();
    const size_t marker = filename.rfind(".cutmachine-");
    if (marker == std::string::npos) return false;
    const std::string suffix = filename.substr(marker + 12);
    if (suffix.size() != 30 || suffix[26] != '.') return false;
    for (size_t index = 0; index < 26; ++index)
        if (!std::isalnum(static_cast<unsigned char>(suffix[index])))
            return false;
    return suffix.substr(27) == "tmp" || suffix.substr(27) == "bak";
}

bool RecoverTransaction(const std::filesystem::path& directory,
                        std::string& message) {
    const std::filesystem::path marker = directory / ".cutmachine-transaction";
    std::error_code existsError;
    if (!std::filesystem::exists(marker, existsError)) {
        if (existsError) {
            message = "unable to inspect transaction marker: " +
                      existsError.message();
            return false;
        }
        return true;
    }
    std::string journal;
    if (!ReadFile(marker.string(), journal, message)) return false;
    std::istringstream input(journal);
    std::string version;
    std::string nonce;
    if (!std::getline(input, version) ||
        version != "CUTMACHINE_TRANSACTION_V1" || !std::getline(input, nonce) ||
        nonce.size() != 38 || nonce.rfind(".cutmachine-", 0) != 0) {
        message = "invalid interrupted transaction marker";
        return false;
    }
    for (size_t index = 12; index < nonce.size(); ++index)
        if (!std::isalnum(static_cast<unsigned char>(nonce[index]))) {
            message = "invalid interrupted transaction marker";
            return false;
        }
    struct RecoveryEntry {
        std::filesystem::path path;
        bool existed = false;
    };
    std::vector<RecoveryEntry> entries;
    std::string line;
    while (std::getline(input, line)) {
        if (line.size() < 3 || line[1] != ' ') {
            message = "invalid interrupted transaction entry";
            return false;
        }
        std::string decoded;
        if ((line[0] != '0' && line[0] != '1') ||
            !HexDecode(line.substr(2), decoded)) {
            message = "invalid interrupted transaction entry";
            return false;
        }
        const std::filesystem::path relative(decoded);
        if (relative.empty() || relative.is_absolute()) {
            message = "unsafe interrupted transaction entry";
            return false;
        }
        for (const auto& component : relative)
            if (component == "..") {
                message = "unsafe interrupted transaction entry";
                return false;
            }
        const std::filesystem::path path = directory / relative;
        if (!IsWithinDirectory(directory, path)) {
            message = "unsafe interrupted transaction entry";
            return false;
        }
        entries.push_back({path, line[0] == '1'});
    }
    for (auto item = entries.rbegin(); item != entries.rend(); ++item) {
        const std::filesystem::path backup =
            item->path.string() + nonce + ".bak";
        const std::filesystem::path temporary =
            item->path.string() + nonce + ".tmp";
        std::error_code backupError;
        if (std::filesystem::exists(backup, backupError)) {
            RemoveIfPresent(item->path);
            if (!Rename(backup, item->path, message)) return false;
        } else if (backupError) {
            message = "unable to inspect transaction backup: " +
                      backupError.message();
            return false;
        } else if (!item->existed) {
            RemoveIfPresent(item->path);
        }
        RemoveIfPresent(temporary);
        if (!SyncDirectory(item->path.parent_path(), message)) return false;
    }
    if (!std::filesystem::remove(marker, existsError) || existsError) {
        message = "unable to remove recovered transaction marker: " +
                  existsError.message();
        return false;
    }
    return SyncDirectory(directory, message);
}

void RemoveTransactionLeftovers(const std::filesystem::path& directory) {
    std::error_code error;
    for (const std::filesystem::path& scan :
         {directory, directory / "Timelines"}) {
        if (!std::filesystem::is_directory(scan, error)) {
            error.clear();
            continue;
        }
        for (std::filesystem::directory_iterator item(scan, error), end;
             !error && item != end; item.increment(error)) {
            if (item->is_regular_file(error) &&
                IsTransactionLeftover(item->path()))
                RemoveIfPresent(item->path());
            error.clear();
        }
    }
}

// S2 -- SAVING_ROADMAP.md. Every byte and the rollback marker reach durable
// storage before an existing canonical destination is moved.
bool CommitArtifacts(std::vector<CommitArtifact> artifacts,
                     std::string& message) {
    if (artifacts.empty()) return true;
    const std::filesystem::path directory = CommonDirectory(artifacts);
    if (directory.empty() || directory == directory.root_path()) {
        message = "transaction artifacts do not share a safe directory";
        return false;
    }
    for (const CommitArtifact& artifact : artifacts)
        if (!IsWithinDirectory(directory,
                               std::filesystem::absolute(artifact.path))) {
            message = "transaction artifact escapes its package directory";
            return false;
        }
    if (!RecoverTransaction(directory, message)) return false;
    RemoveTransactionLeftovers(directory);
    const std::string nonce = ".cutmachine-" + GenerateUlid();
    const std::filesystem::path marker = directory / ".cutmachine-transaction";
    const std::filesystem::path markerTemporary =
        marker.string() + nonce + ".tmp";
    const auto cleanupTemporary = [&] {
        for (const CommitArtifact& artifact : artifacts)
            RemoveIfPresent(artifact.temporary);
    };
    const auto rollback = [&] {
        std::string ignored;
        for (auto item = artifacts.rbegin(); item != artifacts.rend(); ++item) {
            if (item->committed) RemoveIfPresent(item->path);
            if (item->backed_up) Rename(item->backup, item->path, ignored);
        }
        cleanupTemporary();
    };
    for (CommitArtifact& artifact : artifacts) {
        artifact.temporary = artifact.path.string() + nonce + ".tmp";
        artifact.backup = artifact.path.string() + nonce + ".bak";
        std::error_code existsError;
        artifact.existed = std::filesystem::exists(artifact.path, existsError);
        if (existsError) {
            message = "unable to inspect '" + artifact.path.string() +
                      "': " + existsError.message();
            cleanupTemporary();
            return false;
        }
        if (!artifact.remove &&
            !WriteFile(artifact.temporary, artifact.contents, message)) {
            cleanupTemporary();
            return false;
        }
    }
    std::ostringstream journal;
    journal << "CUTMACHINE_TRANSACTION_V1\n" << nonce << '\n';
    for (const CommitArtifact& artifact : artifacts) {
        const std::filesystem::path relative =
            std::filesystem::absolute(artifact.path)
                .lexically_relative(directory);
        journal << (artifact.existed ? '1' : '0') << ' '
                << HexEncode(relative.generic_string()) << '\n';
    }
    if (!WriteFile(markerTemporary, journal.str(), message) ||
        !Rename(markerTemporary, marker, message) ||
        !SyncDirectory(directory, message)) {
        cleanupTemporary();
        RemoveIfPresent(markerTemporary);
        RemoveIfPresent(marker);
        return false;
    }
    for (CommitArtifact& artifact : artifacts) {
        if (artifact.existed) {
            if (!Rename(artifact.path, artifact.backup, message)) {
                rollback();
                return false;
            }
            artifact.backed_up = true;
            if (!SyncDirectory(artifact.path.parent_path(), message)) {
                rollback();
                return false;
            }
        }
    }
    for (CommitArtifact& artifact : artifacts) {
        if (artifact.remove) continue;
        if (!Rename(artifact.temporary, artifact.path, message)) {
            rollback();
            return false;
        }
        artifact.committed = true;
        if (!SyncDirectory(artifact.path.parent_path(), message)) {
            rollback();
            return false;
        }
    }
    std::error_code markerError;
    if (!std::filesystem::remove(marker, markerError) || markerError) {
        message =
            "unable to remove transaction marker: " + markerError.message();
        rollback();
        return false;
    }
    if (!SyncDirectory(directory, message)) {
        std::string ignored;
        if (WriteFile(markerTemporary, journal.str(), ignored) &&
            Rename(markerTemporary, marker, ignored))
            SyncDirectory(directory, ignored);
        rollback();
        return false;
    }
    for (const CommitArtifact& artifact : artifacts)
        if (artifact.backed_up) RemoveIfPresent(artifact.backup);
    RemoveTransactionLeftovers(directory);
    return true;
}

std::string DecimalSeconds(const RationalTime& time) {
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::fixed << std::setprecision(9)
           << (static_cast<long double>(time.value) /
               static_cast<long double>(time.rate));
    std::string text = output.str();
    while (text.size() > 2 && text.back() == '0') text.pop_back();
    if (!text.empty() && text.back() == '.') text.push_back('0');
    return text;
}

void WriteTime(std::ostringstream& output, const RationalTime& time,
               const MediaRate& frameRate) {
    output << "{\"frames\":" << time.to_frames(frameRate.num, frameRate.den)
           << ",\"seconds\":" << DecimalSeconds(time) << '}';
}

std::string AliasPrefix(size_t ordinal) {
    std::string prefix;
    do {
        prefix.insert(prefix.begin(),
                      static_cast<char>('A' + static_cast<int>(ordinal % 26)));
        ordinal = ordinal / 26;
        if (ordinal == 0) break;
        --ordinal;
    } while (true);
    return prefix;
}

MediaRate PresentationRate(const Document& document) {
    return document.sequence.frame_rate;
}

std::string Describe(const Document& document) {
    const MediaRate timelineRate = PresentationRate(document);
    Timeline timeline(document);
    const RationalTime duration = timeline.Duration();
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << "{\"sequence\":{\"id\":\"" << EscapeJson(document.sequence.id)
           << "\",\"name\":\"" << EscapeJson(document.sequence.name)
           << "\",\"width\":" << document.sequence.width
           << ",\"height\":" << document.sequence.height << ",\"frame_rate\":\""
           << document.sequence.frame_rate.num << '/'
           << document.sequence.frame_rate.den
           << "\"},\"timeline\":{\"sources\":[";
    for (size_t index = 0; index < document.sources.size(); ++index) {
        if (index) output << ',';
        const DocumentSource& source = document.sources[index];
        output << "{\"id\":\"" << EscapeJson(source.id) << "\",\"file\":\""
               << EscapeJson(
                      std::filesystem::path(source.path).filename().string())
               << "\",\"frame_rate\":\"" << source.rate.num << '/'
               << source.rate.den << "\",\"duration\":";
        WriteTime(output, source.duration, source.rate);
        output << '}';
    }
    output << "],\"tracks\":[";

    std::vector<const DocumentTrack*> tracks;
    for (const DocumentTrack& track : document.sequence.tracks)
        tracks.push_back(&track);
    std::stable_sort(tracks.begin(), tracks.end(),
                     [](const DocumentTrack* left, const DocumentTrack* right) {
                         return left->index < right->index;
                     });
    for (size_t trackOrdinal = 0; trackOrdinal < tracks.size();
         ++trackOrdinal) {
        if (trackOrdinal) output << ',';
        const DocumentTrack& track = *tracks[trackOrdinal];
        output << "{\"id\":\"" << EscapeJson(track.id) << "\",\"kind\":\""
               << EscapeJson(track.kind) << "\",\"index\":" << track.index
               << ",\"locked\":" << (track.locked ? "true" : "false")
               << ",\"visible\":" << (track.visible ? "true" : "false")
               << ",\"muted\":" << (track.muted ? "true" : "false")
               << ",\"solo\":" << (track.solo ? "true" : "false")
               << ",\"items\":[";
        RationalTime cursor{0, 1};
        bool firstItem = true;
        for (size_t clipIndex = 0; clipIndex < track.clips.size();
             ++clipIndex) {
            const DocumentClip& clip = track.clips[clipIndex];
            const DocumentSource* source = document.FindSource(clip.source_id);
            if (cursor < clip.timeline_in) {
                if (!firstItem) output << ',';
                output << "{\"type\":\"gap\",\"timeline_in\":";
                WriteTime(output, cursor, timelineRate);
                output << ",\"duration\":";
                WriteTime(output, clip.timeline_in.sub(cursor), timelineRate);
                output << '}';
                firstItem = false;
            }
            if (!firstItem) output << ',';
            output << "{\"type\":\""
                   << (track.kind == "caption" ? "caption" : "clip")
                   << "\",\"alias\":\"" << AliasPrefix(trackOrdinal)
                   << (clipIndex + 1) << "\",\"id\":\"" << EscapeJson(clip.id)
                   << '"';
            if (track.kind != "caption") {
                output << ",\"source_id\":\"" << EscapeJson(clip.source_id)
                       << "\",\"source_in\":";
                WriteTime(output, clip.source_in, source->rate);
            }
            output << ",\"timeline_in\":";
            WriteTime(output, clip.timeline_in, timelineRate);
            output << ",\"duration\":";
            WriteTime(output, clip.duration, timelineRate);
            if (track.kind == "caption")
                output << ",\"text\":\"" << EscapeJson(clip.caption_text)
                       << '"';
            else
                output << ",\"include_audio\":"
                       << (clip.include_audio ? "true" : "false");
            if (!clip.link_group_id.empty())
                output << ",\"link_group_id\":\""
                       << EscapeJson(clip.link_group_id) << "\"";
            if (!clip.sync_anchor_clip_id.empty()) {
                output << ",\"sync_anchor_clip_id\":\""
                       << EscapeJson(clip.sync_anchor_clip_id)
                       << "\",\"sync_reference_delta\":";
                WriteTime(output, clip.sync_reference_delta, timelineRate);
            }
            output << '}';
            firstItem = false;
            cursor = clip.timeline_in.add(clip.duration);
        }
        if (cursor < duration) {
            if (!firstItem) output << ',';
            output << "{\"type\":\"gap\",\"timeline_in\":";
            WriteTime(output, cursor, timelineRate);
            output << ",\"duration\":";
            WriteTime(output, duration.sub(cursor), timelineRate);
            output << '}';
        }
        output << "]}";
    }
    output << "],\"duration\":";
    WriteTime(output, duration, timelineRate);
    output << "},\"library\":[";
    std::set<Ulid> usedMedia;
    for (const DocumentTrack& track : document.sequence.tracks) {
        for (const DocumentClip& clip : track.clips) {
            usedMedia.insert(clip.source_id);
        }
    }
    for (size_t index = 0; index < document.library.size(); ++index) {
        if (index) output << ',';
        const LibraryMedia& media = document.library[index];
        output << "{\"alias\":\"M" << (index + 1) << "\",\"id\":\""
               << EscapeJson(media.id) << "\",\"path\":\""
               << EscapeJson(media.path) << "\",\"filename\":\""
               << EscapeJson(media.filename) << "\"";
        if (media.metadata_complete) {
            output << ",\"codec\":\"" << EscapeJson(media.codec)
                   << "\",\"width\":" << media.width
                   << ",\"height\":" << media.height
                   << ",\"rotation_degrees\":" << media.rotation_degrees
                   << ",\"pixel_format\":\"" << EscapeJson(media.pixel_format)
                   << "\",\"color_range\":\"" << EscapeJson(media.color_range)
                   << "\",\"color_space\":\"" << EscapeJson(media.color_space)
                   << "\",\"color_transfer\":\""
                   << EscapeJson(media.color_transfer)
                   << "\",\"color_primaries\":\""
                   << EscapeJson(media.color_primaries) << "\"";
        }
        output << ",\"rate\":{\"num\":" << media.rate.num
               << ",\"den\":" << media.rate.den
               << "},\"duration\":{\"value\":" << media.duration.value
               << ",\"rate\":" << media.duration.rate << "}";
        if (media.metadata_complete) {
            output << ",\"orientation\":\"" << EscapeJson(media.orientation)
                   << "\",\"has_audio\":"
                   << (media.has_audio ? "true" : "false");
            if (media.has_audio) {
                output << ",\"audio_rate\":" << media.audio_rate
                       << ",\"audio_channels\":" << media.audio_channels;
            }
        }
        if (!media.bin_id.empty())
            output << ",\"bin_id\":\"" << EscapeJson(media.bin_id) << "\"";
        output << ",\"in_use\":"
               << (usedMedia.count(media.id) ? "true" : "false") << '}';
    }
    output << "],\"bins\":[";
    for (size_t index = 0; index < document.bins.size(); ++index) {
        if (index) output << ',';
        output << "{\"id\":\"" << EscapeJson(document.bins[index].id)
               << "\",\"name\":\"" << EscapeJson(document.bins[index].name)
               << "\"";
        if (!document.bins[index].parent_id.empty())
            output << ",\"parent_id\":\""
                   << EscapeJson(document.bins[index].parent_id) << "\"";
        output << "}";
    }
    output << "],\"markers\":[";
    for (size_t index = 0; index < document.sequence.markers.size(); ++index) {
        if (index) output << ',';
        const DocumentMarker& marker = document.sequence.markers[index];
        output << "{\"alias\":\"K" << (index + 1) << "\",\"id\":\""
               << EscapeJson(marker.id) << "\",\"name\":\""
               << EscapeJson(marker.name) << "\",\"time\":";
        WriteTime(output, marker.time, timelineRate);
        output << ",\"color\":\"" << EscapeJson(marker.color)
               << "\",\"category\":\"" << EscapeJson(marker.category) << "\"}";
    }
    output << "]}\n";
    return output.str();
}

// B8 -- ROADMAP.md. Keep project-level metadata structured while composing
// it with the existing document description; relying on a trailing `}\n`
// would make an unrelated formatting change silently corrupt describe.
mcp_json::Value DescribeTimelines(const Project& project) {
    mcp_json::Value timelines = mcp_json::Value::MakeArray();
    for (const DocumentSequence& sequence : project.timelines) {
        const RationalTime duration =
            Timeline(project.MakeDocument(sequence.id)).Duration();
        mcp_json::Value item = mcp_json::Value::MakeObject();
        item.Set("id", mcp_json::Value::MakeString(sequence.id));
        item.Set("name", mcp_json::Value::MakeString(sequence.name));
        item.Set("width", mcp_json::Value::MakeInt(sequence.width));
        item.Set("height", mcp_json::Value::MakeInt(sequence.height));
        mcp_json::Value frameRate = mcp_json::Value::MakeObject();
        frameRate.Set("num", mcp_json::Value::MakeInt(sequence.frame_rate.num));
        frameRate.Set("den", mcp_json::Value::MakeInt(sequence.frame_rate.den));
        item.Set("frame_rate", std::move(frameRate));
        mcp_json::Value durationValue = mcp_json::Value::MakeObject();
        durationValue.Set(
            "frames", mcp_json::Value::MakeInt(duration.to_frames(
                          sequence.frame_rate.num, sequence.frame_rate.den)));
        durationValue.Set("seconds",
                          mcp_json::Value::MakeRawNumber(DecimalSeconds(duration)));
        item.Set("duration", std::move(durationValue));
        item.Set("active", mcp_json::Value::MakeBool(
                               sequence.id == project.active_timeline_id));
        timelines.Push(std::move(item));
    }
    return timelines;
}

std::string CanonicalHash(const std::string& json) {
    uint64_t hash = UINT64_C(1469598103934665603);
    for (const unsigned char byte : json) {
        hash ^= byte;
        hash *= UINT64_C(1099511628211);
    }
    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(16) << hash;
    return output.str();
}

}  // namespace

// F2.4 -- ROADMAP.md chat panel. Exposes the same JSON view DescribeCommand
// serializes from a project file path, directly on an in-memory Document,
// for a backend (McpLiveBackend.h) that already holds the app's live
// document instead of a path to reload from disk. Same serialization, same
// aliasing scheme (A1/A2.../K1... the chat and MCP tool catalog both rely
// on) -- just skipping the project-file round trip DescribeCommand does.
std::string DescribeDocument(const Document& document) {
    return Describe(document);
}

std::string DescribeProject(const Project& project) {
    mcp_json::Value description;
    std::string error;
    if (!mcp_json::Value::Parse(Describe(project.MakeActiveDocument()),
                                description, error)) {
        return "{\"ok\":false,\"error\":\"ParseError\",\"detail\":\"" +
               EscapeJson(error) + "\"}\n";
    }
    description.Set("timelines", DescribeTimelines(project));
    return description.Dump() + "\n";
}

std::string TimelineEditLogPathForProject(const std::string& projectPath,
                                          const std::string& timelineId) {
    return projectPath + ".timeline-" + timelineId + ".editlog.json";
}

std::string ProjectEditLogPathForProject(const std::string& projectPath) {
    return projectPath + ".project-editlog.json";
}

bool CommitTextArtifacts(
    const std::vector<std::pair<std::string, std::string>>& artifacts,
    std::string& message) {
    std::vector<CommitArtifact> prepared;
    prepared.reserve(artifacts.size());
    for (const auto& artifact : artifacts)
        prepared.push_back({artifact.first, artifact.second});
    return CommitArtifacts(std::move(prepared), message);
}

bool RecoverTextArtifactTransaction(const std::string& directory,
                                    std::string& message) {
    if (!RecoverTransaction(std::filesystem::absolute(directory), message))
        return false;
    RemoveTransactionLeftovers(std::filesystem::absolute(directory));
    message.clear();
    return true;
}

bool CommitTextArtifactsAndRemove(
    const std::vector<std::pair<std::string, std::string>>& artifacts,
    const std::vector<std::string>& removals, std::string& message) {
    std::vector<CommitArtifact> prepared;
    prepared.reserve(artifacts.size() + removals.size());
    for (const auto& artifact : artifacts)
        prepared.push_back({artifact.first, artifact.second});
    for (const std::string& removal : removals) {
        CommitArtifact artifact;
        artifact.path = removal;
        artifact.remove = true;
        prepared.push_back(std::move(artifact));
    }
    return CommitArtifacts(std::move(prepared), message);
}

int ExportSrtCommand(const std::string& projectPath,
                     const std::string& outputPath, std::string& output) {
    Project project;
    std::string error;
    if (!LoadStoredProject(projectPath, project, error)) {
        return FailCliCommand("InvalidDocument", error, output);
    }
    Document document = project.MakeActiveDocument();
    const std::filesystem::path transcriptDirectory =
        std::filesystem::absolute(projectPath).parent_path() / ".cutmachine" /
        "transcripts";
    std::vector<TimelineTranscriptSpan> spans;
    if (!BuildTimelineTranscriptSpans(document, transcriptDirectory, spans,
                                      error)) {
        return FailCliCommand("InvalidTranscript", error, output);
    }
    // The spans are already the readable groupings SubtitleCuesForClip cuts
    // -- about 42 characters, a sentence end, or a pause -- and they already
    // carry timeline positions, so nothing here recomputes a boundary.
    std::vector<SubtitleCue> cues;
    cues.reserve(spans.size());
    for (const TimelineTranscriptSpan& span : spans)
        cues.push_back({span.timeline_in, span.duration, span.text});
    if (!WriteSrt(cues, outputPath, error)) {
        return FailCliCommand("InvalidSubtitles", error, output);
    }
    output = "{\"ok\":true,\"cues\":" + std::to_string(cues.size()) +
             ",\"path\":\"" + EscapeJson(outputPath) + "\"}\n";
    return 0;
}

int ExportCommand(const std::string& documentPath,
                  const ExportSettings& settings,
                  const ExportProgressCallback& progress,
                  const std::atomic_bool* cancel, std::string& output) {
    Project project;
    std::string error;
    if (!LoadStoredProject(documentPath, project, error)) {
        return FailCliCommand("InvalidDocument", error, output);
    }
    Document document = project.MakeActiveDocument();
    ExportSettings resolvedSettings = settings;
    if (settings.width == 0 && settings.height == 0) {
        resolvedSettings = Exporter::SettingsForPreset(
            ExportPresetId::HevcHighQuality, settings.output_path,
            document.sequence.width, document.sequence.height,
            document.sequence.frame_rate);
        resolvedSettings.encoder = settings.encoder;
        resolvedSettings.overwrite = settings.overwrite;
        resolvedSettings.ffmpeg_path = settings.ffmpeg_path;
    }
    ExportPlan plan;
    if (!Exporter::BuildPlan(document, documentPath, resolvedSettings, plan,
                             error)) {
        return FailCliCommand("InvalidExport", error, output);
    }
    if (!Exporter::Run(plan, progress, cancel, error)) {
        return FailCliCommand("ExportFailed", error, output);
    }
    output = "{\"ok\":true,\"codec\":\"hevc\",\"profile\":\"" +
             std::string(plan.settings.main10 ? "main10" : "main") +
             "\",\"width\":" + std::to_string(plan.settings.width) +
             ",\"height\":" + std::to_string(plan.settings.height) +
             ",\"frames\":" + std::to_string(plan.total_frames) +
             ",\"path\":\"" + EscapeJson(plan.settings.output_path) + "\"}\n";
    return 0;
}

int CreateProjectCommand(const std::string& packagePath,
                         const std::string& projectName, std::string& output) {
    if (projectName.empty()) {
        output = ErrorJson(EditError::InvalidOperation,
                           "project name must not be empty");
        return 1;
    }
    Project project(projectName);
    std::string projectPath;
    std::string error;
    if (!CreatePortableProject(packagePath, project, projectPath, error)) {
        output = ErrorJson(EditError::IoError, error);
        return 1;
    }
    output = "{\"ok\":true,\"path\":\"" + EscapeJson(projectPath) + "\"}\n";
    return 0;
}

int TranscribeMediaCommand(const std::string& projectPath,
                           const std::vector<std::string>& mediaIds,
                           const std::string& whisperModelPath,
                           const std::string& language, bool verbatim,
                           bool includeSilent, std::string& output) {
    Project project;
    std::string error;
    if (!LoadStoredProject(projectPath, project, error)) {
        output = ErrorJson(EditError::ParseError, error);
        return 1;
    }
    if (mediaIds.empty()) {
        output = ErrorJson(EditError::InvalidOperation,
                           "at least one media_id is required");
        return 1;
    }
    const std::filesystem::path base =
        std::filesystem::absolute(projectPath).parent_path();
    struct Skipped {
        std::string media_id;
        std::string reason;
    };
    std::vector<TranscriptionJob> jobs;
    std::vector<Skipped> skipped;
    for (const std::string& mediaId : mediaIds) {
        const auto media = std::find_if(
            project.rushes.begin(), project.rushes.end(),
            [&](const LibraryMedia& item) { return item.id == mediaId; });
        const auto source = std::find_if(
            project.sources.begin(), project.sources.end(),
            [&](const DocumentSource& item) { return item.id == mediaId; });
        if (media == project.rushes.end() || source == project.sources.end()) {
            output = ErrorJson(EditError::UnknownMedia,
                               "unknown media_id '" + mediaId + "'");
            return 1;
        }
        if (!media->has_audio) {
            output = ErrorJson(EditError::InvalidOperation,
                               "media '" + mediaId + "' has no audio stream");
            return 1;
        }
        // QC-2026-09 A3 -- a mute cutaway costs eleven times its own runtime
        // to produce nothing. 29 of one project's 71 rushes were exactly
        // that, at -74 dBFS. Skipping them is not a guess: the level was
        // measured at ingest and is a document fact. `includeSilent` exists
        // because a threshold is still a threshold, and a caller who
        // disagrees must be able to say so.
        if (!includeSilent && media->audio_level_measured &&
            media->audio_level < kSilentMediaAudioLevel) {
            skipped.push_back(
                {mediaId, "measured audio level " +
                              std::to_string(media->audio_level) +
                              " is below the silence threshold "
                              "(" +
                              std::to_string(kSilentMediaAudioLevel) + ")"});
            continue;
        }
        std::filesystem::path input(media->path);
        if (input.is_relative()) input = base / input;
        input = input.lexically_normal();
        jobs.push_back(
            {mediaId, input.string(),
             (base / ".cutmachine" / "transcripts" / (mediaId + ".json"))
                 .string(),
             source->rate});
    }

    // The model is resolved only once there is something to run it on: a
    // batch that turned out to be all mute cutaways needs no model, and
    // failing it for a missing one would be a wrong answer. Resolved after
    // the media loop for the same reason it used to come after the single
    // media lookup -- "you named a media that is not in this project" is a
    // more useful first answer than "the ggml file is missing".
    std::string modelPath = whisperModelPath;
    if (!jobs.empty()) {
        // An empty path means "use the configured model" -- the only form the
        // agent can use, since it has no way to know where a human keeps a
        // ggml file. An explicit path still wins, so the CLI stays scriptable.
        if (modelPath.empty() &&
            !ResolveConfiguredWhisperModel(modelPath, error)) {
            output = ErrorJson(EditError::IoError, error);
            return 1;
        }
        if (!std::filesystem::is_regular_file(modelPath)) {
            output = ErrorJson(
                EditError::IoError,
                "Whisper model is not a regular file: '" + modelPath + "'");
            return 1;
        }
    }

    std::vector<TranscriptionOutcome> outcomes;
    if (!jobs.empty()) {
        WhisperSettings settings;
        settings.whisper_model_path = modelPath;
        settings.language = language.empty() ? "auto" : language;
        settings.verbatim = verbatim;
        MediaTaskManager tasks(1);
        std::string batchError;
        const Ulid taskId = tasks.Enqueue(
            MediaTaskKind::Transcription,
            "Whisper " + std::to_string(jobs.size()) + " media",
            [&jobs, &outcomes, &batchError, settings](MediaTaskContext& context,
                                                      std::string& taskError) {
                const bool started = GenerateAudioTranscripts(
                    jobs, settings, context, outcomes, batchError);
                if (!started) taskError = batchError;
                return started;
            });
        if (!tasks.WaitForIdle(24 * 60 * 60 * 1000)) {
            tasks.Cancel(taskId);
            output = ErrorJson(EditError::IoError, "transcription timed out");
            return 1;
        }
        const std::vector<MediaTaskSnapshot> snapshots = tasks.Snapshot();
        const auto snapshot = std::find_if(
            snapshots.begin(), snapshots.end(),
            [&](const MediaTaskSnapshot& item) { return item.id == taskId; });
        if (snapshot == snapshots.end() ||
            snapshot->state != MediaTaskState::Succeeded) {
            const std::string detail = snapshot == snapshots.end()
                                           ? "transcription task disappeared"
                                           : snapshot->error;
            output = ErrorJson(EditError::IoError, detail);
            return 1;
        }
    }

    size_t succeeded = 0;
    std::ostringstream json;
    json << "{\"ok\":true,\"verbatim\":" << (verbatim ? "true" : "false")
         << ",\"results\":[";
    bool first = true;
    for (const TranscriptionOutcome& outcome : outcomes) {
        if (!first) json << ',';
        first = false;
        if (outcome.ok) ++succeeded;
        json << "{\"media_id\":\"" << EscapeJson(outcome.media_id)
             << "\",\"ok\":" << (outcome.ok ? "true" : "false")
             << ",\"words\":" << outcome.words;
        if (!outcome.ok)
            json << ",\"error\":\"" << EscapeJson(outcome.error) << "\"";
        else
            json << ",\"path\":\""
                 << EscapeJson((base / ".cutmachine" / "transcripts" /
                                (outcome.media_id + ".json"))
                                   .string())
                 << "\"";
        json << "}";
    }
    json << "],\"skipped\":[";
    for (size_t index = 0; index < skipped.size(); ++index) {
        if (index) json << ',';
        json << "{\"media_id\":\"" << EscapeJson(skipped[index].media_id)
             << "\",\"reason\":\"" << EscapeJson(skipped[index].reason)
             << "\"}";
    }
    json << "],\"transcribed\":" << succeeded
         << ",\"skipped_count\":" << skipped.size() << "}\n";
    output = json.str();
    // A batch in which every named media failed is a failed command: a caller
    // that only checks the exit status must not read it as done.
    if (!outcomes.empty() && succeeded == 0) {
        const std::string detail = outcomes.size() == 1
                                       ? outcomes.front().error
                                       : "all transcription jobs failed";
        return FailCliCommand("IoError", detail, output);
    }
    return 0;
}

int AnalyzeShotQualityCommand(const std::string& projectPath,
                              const std::string& mediaId, std::string& output) {
    Project project;
    std::string error;
    if (!LoadStoredProject(projectPath, project, error)) {
        output = ErrorJson(EditError::ParseError, error);
        return 1;
    }
    const auto media = std::find_if(
        project.rushes.begin(), project.rushes.end(),
        [&](const LibraryMedia& item) { return item.id == mediaId; });
    const auto source = std::find_if(
        project.sources.begin(), project.sources.end(),
        [&](const DocumentSource& item) { return item.id == mediaId; });
    if (media == project.rushes.end() || source == project.sources.end()) {
        output = ErrorJson(EditError::UnknownMedia,
                           "unknown media_id '" + mediaId + "'");
        return 1;
    }

    const std::filesystem::path base =
        std::filesystem::absolute(projectPath).parent_path();
    std::filesystem::path input(media->path);
    if (input.is_relative()) input = base / input;
    input = input.lexically_normal();
    const std::filesystem::path reportPath =
        base / ".cutmachine" / "shotquality" / (mediaId + ".json");
    MediaTaskManager tasks(1);
    const Ulid taskId = tasks.Enqueue(
        MediaTaskKind::ShotQuality, "Analyse image " + media->filename,
        [input, reportPath, mediaId, duration = source->duration](
            MediaTaskContext& context, std::string& taskError) {
            return GenerateShotQuality(input.string(), reportPath.string(),
                                       mediaId, duration, ShotQualitySettings{},
                                       context, taskError);
        });
    if (!tasks.WaitForIdle(24 * 60 * 60 * 1000)) {
        tasks.Cancel(taskId);
        output =
            ErrorJson(EditError::IoError, "shot quality analysis timed out");
        return 1;
    }
    const std::vector<MediaTaskSnapshot> snapshots = tasks.Snapshot();
    const auto snapshot = std::find_if(
        snapshots.begin(), snapshots.end(),
        [&](const MediaTaskSnapshot& item) { return item.id == taskId; });
    if (snapshot == snapshots.end() ||
        snapshot->state != MediaTaskState::Succeeded) {
        const std::string detail = snapshot == snapshots.end()
                                       ? "shot quality task disappeared"
                                       : snapshot->error;
        output = ErrorJson(EditError::IoError, detail);
        return 1;
    }
    ShotQualityReport report;
    std::string loadError;
    const bool loaded = LoadShotQuality(reportPath.string(), report, loadError);
    output =
        "{\"ok\":true,\"media_id\":\"" + EscapeJson(mediaId) +
        "\",\"path\":\"" + EscapeJson(reportPath.string()) +
        "\",\"samples\":" + std::to_string(loaded ? report.samples.size() : 0) +
        ",\"median_sharpness\":" +
        std::to_string(loaded ? report.median_sharpness : 0) + "}\n";
    return 0;
}

int ShotQualityReportCommand(const std::string& projectPath,
                             std::string& output) {
    Project project;
    std::string error;
    if (!LoadStoredProject(projectPath, project, error)) {
        output = ErrorJson(EditError::ParseError, error);
        return 1;
    }
    const Document document = project.MakeActiveDocument();
    const std::filesystem::path base =
        std::filesystem::absolute(projectPath).parent_path();
    std::map<Ulid, ShotQualityReport> reports;
    for (const DocumentTrack& track : document.sequence.tracks) {
        if (track.kind != "video") continue;
        for (const DocumentClip& clip : track.clips) {
            if (reports.count(clip.source_id)) continue;
            ShotQualityReport report;
            std::string loadError;
            const std::filesystem::path path = base / ".cutmachine" /
                                               "shotquality" /
                                               (clip.source_id + ".json");
            if (LoadShotQuality(path.string(), report, loadError))
                reports.emplace(clip.source_id, std::move(report));
        }
    }
    output =
        DescribeShotQualityForAgent(document, reports, ShotQualityThresholds{},
                                    ShotSegmentationSettings{}) +
        "\n";
    return 0;
}

int TimelineStatsCommand(const std::string& projectPath, std::string& output) {
    Project project;
    std::string error;
    if (!LoadStoredProject(projectPath, project, error)) {
        output = ErrorJson(EditError::ParseError, error);
        return 1;
    }
    TimelineStats stats;
    if (!CalculateTimelineStats(project.MakeActiveDocument(), stats, error)) {
        output = ErrorJson(EditError::ValidationFailed, error);
        return 1;
    }
    output = "{\"ok\":true,\"stats\":" +
             SerializeTimelineStats(stats) + "}\n";
    return 0;
}

int AlignTranscriptsCommand(const std::string& projectPath, bool apply,
                            std::string& output) {
    Project project;
    std::string error;
    if (!LoadStoredProject(projectPath, project, error)) {
        output = ErrorJson(EditError::ParseError, error);
        return 1;
    }
    const std::filesystem::path base =
        std::filesystem::absolute(projectPath).parent_path();
    std::ostringstream out;
    out << "{\"ok\":true,\"applied\":" << (apply ? "true" : "false")
        << ",\"sources\":[";
    bool first = true;
    for (const LibraryMedia& media : project.rushes) {
        Transcript transcript;
        SpeechOnsetReport envelope;
        std::string message;
        const std::filesystem::path transcriptPath =
            base / ".cutmachine" / "transcripts" / (media.id + ".json");
        const std::filesystem::path envelopePath =
            base / ".cutmachine" / "speechonset" / (media.id + ".json");
        if (!LoadAudioTranscript(transcriptPath.string(), transcript, message))
            continue;
        if (!LoadSpeechOnset(envelopePath.string(), envelope, message))
            continue;
        Transcript aligned;
        TranscriptAlignmentReport report;
        if (!AlignTranscriptToSpeech(transcript, envelope,
                                     TranscriptAlignmentSettings{}, aligned,
                                     report, message))
            continue;
        // QC-2026-09 (A1) -- the write is what removes the probing loop. A
        // report alone leaves every downstream reader (remove_words,
        // clean_disfluencies, the interview short) still cutting on the
        // boundaries the signal contradicts, so the correction has to reach
        // the cache the rest of the toolchain actually reads. It stays
        // opt-in, and a failed write is reported per source rather than
        // aborting the pass: the remaining sources are still worth aligning.
        std::string writeError;
        bool written = false;
        if (apply) {
            aligned.speech_aligned = true;
            written = SaveAudioTranscript(transcriptPath.string(), aligned,
                                          writeError);
        }
        if (!first) out << ',';
        first = false;
        out << "{\"media_id\":\"" << EscapeJson(media.id)
            << "\",\"filename\":\"" << EscapeJson(media.filename)
            << "\",\"written\":" << (written ? "true" : "false");
        if (apply && !written)
            out << ",\"write_error\":\"" << EscapeJson(writeError) << "\"";
        out << ",\"report\":" << SerializeTranscriptAlignmentReport(report)
            << "}";
    }
    out << "]}\n";
    output = out.str();
    return 0;
}

int AnalyzeSpeechOnsetCommand(const std::string& projectPath,
                              const std::string& mediaId, std::string& output) {
    Project project;
    std::string error;
    if (!LoadStoredProject(projectPath, project, error)) {
        output = ErrorJson(EditError::ParseError, error);
        return 1;
    }
    const auto media = std::find_if(
        project.rushes.begin(), project.rushes.end(),
        [&](const LibraryMedia& item) { return item.id == mediaId; });
    if (media == project.rushes.end()) {
        output = ErrorJson(EditError::UnknownMedia,
                           "unknown media_id '" + mediaId + "'");
        return 1;
    }
    if (!media->has_audio) {
        output = ErrorJson(EditError::ValidationFailed,
                           "media '" + media->filename + "' has no audio");
        return 1;
    }
    const std::filesystem::path base =
        std::filesystem::absolute(projectPath).parent_path();
    std::filesystem::path input(media->path);
    if (input.is_relative()) input = base / input;
    input = input.lexically_normal();
    const std::filesystem::path reportPath =
        base / ".cutmachine" / "speechonset" / (mediaId + ".json");
    MediaTaskManager tasks(1);
    const Ulid taskId = tasks.Enqueue(
        MediaTaskKind::SpeechOnset, "Analyse parole " + media->filename,
        [input, reportPath, mediaId](MediaTaskContext& context,
                                     std::string& taskError) {
            return GenerateSpeechOnset(input.string(), reportPath.string(),
                                       mediaId, SpeechOnsetSettings{}, context,
                                       taskError);
        });
    if (!tasks.WaitForIdle(24 * 60 * 60 * 1000)) {
        tasks.Cancel(taskId);
        output =
            ErrorJson(EditError::IoError, "speech onset analysis timed out");
        return 1;
    }
    const std::vector<MediaTaskSnapshot> snapshots = tasks.Snapshot();
    const auto snapshot = std::find_if(
        snapshots.begin(), snapshots.end(),
        [&](const MediaTaskSnapshot& item) { return item.id == taskId; });
    if (snapshot == snapshots.end() ||
        snapshot->state != MediaTaskState::Succeeded) {
        const std::string detail = snapshot == snapshots.end()
                                       ? "speech onset task disappeared"
                                       : snapshot->error;
        output = ErrorJson(EditError::IoError, detail);
        return 1;
    }
    SpeechOnsetReport report;
    std::string loadError;
    if (!LoadSpeechOnset(reportPath.string(), report, loadError)) {
        output = ErrorJson(EditError::IoError, loadError);
        return 1;
    }
    output = "{\"ok\":true,\"media_id\":\"" + EscapeJson(mediaId) +
             "\",\"path\":\"" + EscapeJson(reportPath.string()) +
             "\",\"windows\":" + std::to_string(report.levels.size()) +
             ",\"speech_level\":" + std::to_string(report.speech_level) +
             ",\"noise_floor\":" + std::to_string(report.noise_floor) + "}\n";
    return 0;
}

int SpeechOnsetReportCommand(const std::string& projectPath,
                             std::string& output) {
    Project project;
    std::string error;
    if (!LoadStoredProject(projectPath, project, error)) {
        output = ErrorJson(EditError::ParseError, error);
        return 1;
    }
    const Document document = project.MakeActiveDocument();
    const std::filesystem::path base =
        std::filesystem::absolute(projectPath).parent_path();
    std::map<Ulid, SpeechOnsetReport> reports;
    for (const DocumentTrack& track : document.sequence.tracks) {
        if (track.kind != "audio") continue;
        for (const DocumentClip& clip : track.clips) {
            if (reports.count(clip.source_id)) continue;
            SpeechOnsetReport report;
            std::string loadError;
            const std::filesystem::path path = base / ".cutmachine" /
                                               "speechonset" /
                                               (clip.source_id + ".json");
            if (LoadSpeechOnset(path.string(), report, loadError))
                reports.emplace(clip.source_id, std::move(report));
        }
    }
    output = DescribeSpeechOnsetForAgent(document, reports,
                                         SpeechOnsetThresholds{}) +
             "\n";
    return 0;
}

namespace {

// Both word commands need the same three things resolved from a clip id, and
// getting any of them wrong is the difference between cutting the right
// frames and cutting someone else's. Kept in one place so neither command
// can drift from the other.
bool LoadClipTranscript(const std::string& projectPath,
                        const std::string& clipId, Project& project,
                        Document& document, const DocumentClip*& clip,
                        Transcript& transcript, std::string& output) {
    std::string message;
    if (!LoadStoredProject(projectPath, project, message)) {
        output = ErrorJson(EditError::ParseError, message);
        return false;
    }
    document = project.MakeActiveDocument();
    clip = document.FindClip(clipId);
    if (clip == nullptr) {
        output =
            ErrorJson(EditError::UnknownClip, "unknown clip_id '" + clipId +
                                                  "' on the active timeline");
        return false;
    }
    const std::filesystem::path base =
        std::filesystem::absolute(projectPath).parent_path();
    const std::filesystem::path path =
        base / ".cutmachine" / "transcripts" / (clip->source_id + ".json");
    if (!LoadAudioTranscript(path.string(), transcript, message)) {
        output = ErrorJson(EditError::IoError,
                           "no usable transcript for source '" +
                               clip->source_id + "': " + message);
        return false;
    }
    return true;
}

std::string DisfluencyKindName(DisfluencyKind kind) {
    return kind == DisfluencyKind::Filler ? "Filler" : "Repetition";
}

}  // namespace

int ListDisfluenciesCommand(const std::string& projectPath,
                            const std::string& clipId, std::string& output) {
    Project project;
    Document document;
    const DocumentClip* clip = nullptr;
    Transcript transcript;
    if (!LoadClipTranscript(projectPath, clipId, project, document, clip,
                            transcript, output))
        return 1;

    const std::vector<Disfluency> found =
        FindDisfluenciesInClip(*clip, transcript);
    std::ostringstream json;
    json << "{\"ok\":true,\"clip_id\":\"" << EscapeJson(clip->id)
         << "\",\"source_id\":\"" << EscapeJson(clip->source_id)
         << "\",\"whisper_model\":\""
         << EscapeJson(transcript.whisper_model)
         // Reported because it decides what this list can possibly contain:
         // Whisper's default decoding drops most fillers before they are ever
         // written down, so an empty result on a non-verbatim transcript says
         // nothing about how the take was actually spoken.
         << "\",\"verbatim\":" << (transcript.verbatim ? "true" : "false")
         << ",\"words\":" << transcript.words.size() << ",\"disfluencies\":[";
    for (size_t index = 0; index < found.size(); ++index) {
        const Disfluency& item = found[index];
        if (index != 0) json << ',';
        json << "{\"start_word_index\":" << item.range.start_word_index
             << ",\"end_word_index\":" << item.range.end_word_index
             << ",\"kind\":\"" << DisfluencyKindName(item.kind)
             << "\",\"text\":\"" << EscapeJson(item.text) << "\"}";
    }
    json << "]}\n";
    output = json.str();
    return 0;
}

int RemoveWordsCommand(const std::string& projectPath,
                       const std::string& clipId, const std::string& rangesJson,
                       std::string& output) {
    Project project;
    Document document;
    const DocumentClip* clip = nullptr;
    Transcript transcript;
    if (!LoadClipTranscript(projectPath, clipId, project, document, clip,
                            transcript, output))
        return 1;

    mcp_json::Value parsed;
    std::string message;
    if (!mcp_json::Value::Parse(rangesJson, parsed, message) ||
        !parsed.IsArray()) {
        output = ErrorJson(EditError::ParseError,
                           "word ranges must be a JSON array: " + message);
        return 1;
    }
    std::vector<WordRange> ranges;
    for (const mcp_json::Value& value : parsed.AsArray()) {
        const mcp_json::Value* start = value.Find("start_word_index");
        const mcp_json::Value* end = value.Find("end_word_index");
        int64_t startIndex = 0;
        int64_t endIndex = 0;
        if (start == nullptr || end == nullptr || !start->AsInt64(startIndex) ||
            !end->AsInt64(endIndex) || startIndex < 0 || endIndex < 0) {
            output = ErrorJson(
                EditError::InvalidOperation,
                "each word range needs integer 'start_word_index' and "
                "'end_word_index'");
            return 1;
        }
        ranges.push_back(
            {static_cast<size_t>(startIndex), static_cast<size_t>(endIndex)});
    }

    // The caller only ever names words. Turning those into frames is
    // ResolveWordRemoval's job and nobody else's -- see Transcription.h,
    // where it is documented as the only intended producer of a
    // WordRemovalRange.
    RemoveWordsOperation operation;
    if (!ResolveWordRemoval(*clip, transcript, ranges, RationalTime{0, 1}, {},
                            operation, message)) {
        output = ErrorJson(EditError::InvalidOperation, message);
        return 1;
    }
    // Handing the serialized operation back to ApplyOperationCommand keeps
    // this on the one reversible path: same edit log, same undo, same
    // round-trip guarantee as any other edit.
    return ApplyOperationCommand(projectPath, SerializeOperation(operation),
                                 output);
}

int TightenPausesCommand(const std::string& projectPath,
                         const std::string& clipId, int64_t minimumGapMs,
                         int64_t keepFrames, std::string& output) {
    Project project;
    std::string message;
    if (!LoadStoredProject(projectPath, project, message)) {
        output = ErrorJson(EditError::ParseError, message);
        return 1;
    }
    const Document document = project.MakeActiveDocument();
    const DocumentClip* clip = document.FindClip(clipId);
    if (clip == nullptr) {
        output =
            ErrorJson(EditError::UnknownClip, "unknown clip_id '" + clipId +
                                                  "' on the active timeline");
        return 1;
    }
    const DocumentSource* source = document.FindSource(clip->source_id);
    if (source == nullptr) {
        output =
            ErrorJson(EditError::UnknownSource, "clip '" + clipId +
                                                    "' reads from an unmounted "
                                                    "source");
        return 1;
    }
    const std::filesystem::path envelopePath =
        std::filesystem::absolute(projectPath).parent_path() / ".cutmachine" /
        "speechonset" / (clip->source_id + ".json");
    SpeechOnsetReport envelope;
    if (!LoadSpeechOnset(envelopePath.string(), envelope, message)) {
        output = ErrorJson(EditError::IoError,
                           "no speech envelope for source '" + clip->source_id +
                               "': " + message +
                               " (run --speech-onset on it first)");
        return 1;
    }

    PauseTighteningSettings settings;
    settings.minimum_gap_milliseconds = minimumGapMs;
    settings.keep_frames = keepFrames;
    RemoveWordsOperation operation;
    PauseTighteningReport report;
    if (!ResolvePauseTightening(*clip, envelope, settings, source->rate, {},
                                operation, report, message)) {
        output = ErrorJson(EditError::InvalidOperation, message);
        return 1;
    }
    if (operation.ranges.empty()) {
        // Nothing to close is a success: "tighten this clip" on an already
        // tight clip has been honoured. Applying an empty RemoveWords would
        // only be refused by the engine.
        output = "{\"ok\":true,\"clip_id\":\"" + EscapeJson(clip->id) +
                 "\",\"applied\":false,\"report\":" +
                 SerializePauseTighteningReport(report) + "}\n";
        return 0;
    }
    operation.linked_clip_ids =
        LinkedClipIdsCoveringRanges(document, *clip, operation.ranges);
    // Same hand-off RemoveWordsCommand makes: the serialized operation goes
    // through ApplyOperationCommand, so this lands on the one reversible
    // path with the same edit log and the same undo as any other edit.
    const int result = ApplyOperationCommand(
        projectPath, SerializeOperation(operation), output);
    if (result != 0) return result;
    // The apply result is the document-facing half; the report is what says
    // which pauses were closed and what was deliberately left standing. Both
    // are carried back, so one call answers "what changed" and "why" --
    // parsed and re-emitted rather than spliced, so a change to the shape of
    // either payload cannot silently produce invalid JSON.
    mcp_json::Value applied;
    mcp_json::Value reportView;
    std::string parseError;
    if (mcp_json::Value::Parse(output, applied, parseError) &&
        applied.IsObject() &&
        mcp_json::Value::Parse(SerializePauseTighteningReport(report),
                               reportView, parseError)) {
        applied.Set("report", std::move(reportView));
        output = applied.Dump() + "\n";
    }
    return 0;
}

int LocateSourceFrameCommand(const std::string& projectPath,
                             const std::string& mediaId, int64_t sourceFrame,
                             std::string& output) {
    Project project;
    std::string message;
    if (!LoadStoredProject(projectPath, project, message)) {
        output = ErrorJson(EditError::ParseError, message);
        return 1;
    }
    const Document document = project.MakeActiveDocument();
    std::vector<SourceFrameMatch> matches;
    if (!ResolveSourceFrame(document, mediaId, sourceFrame, matches, message)) {
        output = ErrorJson(EditError::UnknownMedia, message);
        return 1;
    }
    output = DescribeSourceFrameMatches(mediaId, sourceFrame, matches) + "\n";
    return 0;
}

int DescribeCommand(const std::string& documentPath, std::string& output) {
    std::string message;
    Project project;
    if (!LoadStoredProject(documentPath, project, message)) {
        output = ErrorJson(EditError::ParseError, message);
        return 1;
    }
    try {
        output = DescribeProject(project);
        return 0;
    } catch (const std::exception& exception) {
        output = ErrorJson(EditError::ArithmeticError, exception.what());
        return 1;
    }
}

int ProposeSequenceCommand(const std::string& projectPath,
                           std::string& output) {
    Project project;
    std::string error;
    if (!LoadStoredProject(projectPath, project, error)) {
        output = ErrorJson(EditError::ParseError, error);
        return 1;
    }
    SequenceFormatProposal proposal;
    if (!ResolveSequenceFormat(project.rushes, proposal, error)) {
        output = ErrorJson(EditError::InvalidOperation, error);
        return 1;
    }
    output = SequenceFormatProposalJson(proposal) + "\n";
    return 0;
}

int ApplyOperationCommand(const std::string& documentPath,
                          const std::string& operationJson,
                          std::string& output) {
    Operation operation = RemoveClipOperation{};
    EditError error = EditError::None;
    std::string message;
    if (!DeserializeOperation(operationJson, operation, error, message)) {
        output = ErrorJson(error, message);
        return 1;
    }

    Project project;
    if (!LoadStoredProject(documentPath, project, message)) {
        output = ErrorJson(EditError::ParseError, message);
        return 1;
    }
    Document document = project.MakeActiveDocument();
    // DELTA-2026-08 -- the state the caller already knows about. Kept so the
    // result can say what changed instead of making the caller re-read the
    // whole timeline to find out.
    const Document before = document;

    EditLog log;
    const std::string timelineLogPath =
        TimelineEditLogPathForProject(documentPath, project.active_timeline_id);
    std::string logPath = timelineLogPath;
    std::error_code existsError;
    bool logExists = std::filesystem::exists(logPath, existsError);
    if (existsError) {
        output = ErrorJson(EditError::IoError,
                           "unable to inspect edit log '" + logPath +
                               "': " + existsError.message());
        return 1;
    }
    if (logExists) {
        std::string logJson;
        if (!ReadFile(logPath, logJson, message)) {
            output = ErrorJson(EditError::IoError, message);
            return 1;
        }
        if (!EditLog::Deserialize(logJson, log, error, message)) {
            output = ErrorJson(error, message);
            return 1;
        }
    }

    if (!log.Apply(document, std::move(operation), error, message)) {
        output = ErrorJson(error, message);
        return 1;
    }

    const std::string updatedDocument = document.SaveToString();
    std::map<std::string, EditLog> logs;
    logs[project.active_timeline_id] = log;
    ProjectEditLog projectLog;
    if (!project.CommitActiveDocument(document, message) ||
        !CommitStoredProjectAndLogs(documentPath, project, logs, projectLog,
                                    message)) {
        output = ErrorJson(EditError::IoError, message);
        return 1;
    }
    DocumentDelta delta;
    ComputeDocumentDelta(before, document, delta);
    output = "{\"ok\":true,\"doc_hash\":\"" + CanonicalHash(updatedDocument) +
             "\",\"delta\":" + SerializeDocumentDelta(delta) + "}\n";
    return 0;
}

namespace {

bool LoadOptionalTimelineLog(const std::string& projectPath,
                             const std::string& timelineId, EditLog& log,
                             EditError& error, std::string& message) {
    std::string path = TimelineEditLogPathForProject(projectPath, timelineId);
    std::error_code existsError;
    bool exists = std::filesystem::exists(path, existsError);
    if (existsError) {
        error = EditError::IoError;
        message = "unable to inspect edit log '" + path +
                  "': " + existsError.message();
        return false;
    }
    if (!exists) return true;
    std::string json;
    return ReadFile(path, json, message) &&
           EditLog::Deserialize(json, log, error, message);
}

bool LoadOptionalProjectLog(const std::string& projectPath, ProjectEditLog& log,
                            EditError& error, std::string& message) {
    const std::string path = ProjectEditLogPathForProject(projectPath);
    std::error_code existsError;
    const bool exists = std::filesystem::exists(path, existsError);
    if (existsError) {
        error = EditError::IoError;
        message = "unable to inspect project edit log '" + path +
                  "': " + existsError.message();
        return false;
    }
    if (!exists) return true;
    std::string json;
    return ReadFile(path, json, message) &&
           ProjectEditLog::Deserialize(json, log, error, message);
}

int MutateProjectLogCommand(const std::string& projectPath,
                            const std::optional<ProjectOperation>& operation,
                            bool redo, std::string& output) {
    std::string message;
    EditError error = EditError::None;
    Project project;
    if (!LoadStoredProject(projectPath, project, message)) {
        output = ErrorJson(EditError::ParseError, message);
        return 1;
    }
    std::map<std::string, EditLog> timelineLogs;
    for (const DocumentSequence& timeline : project.timelines) {
        EditLog timelineLog;
        if (!LoadOptionalTimelineLog(projectPath, timeline.id, timelineLog,
                                     error, message)) {
            output = ErrorJson(error, message);
            return 1;
        }
        timelineLogs.emplace(timeline.id, std::move(timelineLog));
    }
    ProjectEditLog projectLog;
    if (!LoadOptionalProjectLog(projectPath, projectLog, error, message)) {
        output = ErrorJson(error, message);
        return 1;
    }
    bool changed = false;
    if (operation)
        changed = projectLog.Apply(project, *operation, error, message);
    else if (redo)
        changed = projectLog.Redo(project, error, message);
    else
        changed = projectLog.Undo(project, error, message);
    if (!changed) {
        output = ErrorJson(error, message);
        return 1;
    }
    for (const DocumentSequence& timeline : project.timelines)
        timelineLogs.try_emplace(timeline.id, EditLog{});
    if (!CommitStoredProjectAndLogs(projectPath, project, timelineLogs,
                                    projectLog, message)) {
        output = ErrorJson(EditError::IoError, message);
        return 1;
    }
    output = "{\"ok\":true,\"project_hash\":\"" +
             CanonicalHash(project.SaveToString()) + "\"}\n";
    return 0;
}

}  // namespace

int ApplyProjectOperationCommand(const std::string& projectPath,
                                 const std::string& operationJson,
                                 std::string& output) {
    ProjectOperation operation = AddProjectTimelineOperation{};
    EditError error = EditError::None;
    std::string message;
    if (!DeserializeProjectOperation(operationJson, operation, error,
                                     message)) {
        output = ErrorJson(error, message);
        return 1;
    }
    return MutateProjectLogCommand(projectPath, operation, false, output);
}

int UndoProjectOperationCommand(const std::string& projectPath,
                                std::string& output) {
    return MutateProjectLogCommand(projectPath, std::nullopt, false, output);
}

int RedoProjectOperationCommand(const std::string& projectPath,
                                std::string& output) {
    return MutateProjectLogCommand(projectPath, std::nullopt, true, output);
}

namespace {

// Shared tail of UndoOperationCommand/RedoOperationCommand: identical to the
// load/mutate/commit shape of ApplyOperationCommand above, but drives the
// active timeline's EditLog::Undo/Redo instead of EditLog::Apply. Kept
// separate from ApplyOperationCommand rather than folded into it so neither
// entry point grows a branch the other doesn't need.
int MutateTimelineLogCommand(const std::string& documentPath, bool redo,
                             std::string& output) {
    std::string message;
    EditError error = EditError::None;
    Project project;
    if (!LoadStoredProject(documentPath, project, message)) {
        output = ErrorJson(EditError::ParseError, message);
        return 1;
    }
    Document document = project.MakeActiveDocument();
    // Undo needs the delta more than anything else does: the caller has no
    // way to guess what a reversal put back.
    const Document before = document;

    EditLog log;
    if (!LoadOptionalTimelineLog(documentPath, project.active_timeline_id, log,
                                 error, message)) {
        output = ErrorJson(error, message);
        return 1;
    }

    const bool changed = redo ? log.Redo(document, error, message)
                              : log.Undo(document, error, message);
    if (!changed) {
        output = ErrorJson(error, message);
        return 1;
    }

    const std::string updatedDocument = document.SaveToString();
    std::map<std::string, EditLog> logs;
    logs[project.active_timeline_id] = log;
    ProjectEditLog projectLog;
    if (!project.CommitActiveDocument(document, message) ||
        !CommitStoredProjectAndLogs(documentPath, project, logs, projectLog,
                                    message)) {
        output = ErrorJson(EditError::IoError, message);
        return 1;
    }
    DocumentDelta delta;
    ComputeDocumentDelta(before, document, delta);
    output = "{\"ok\":true,\"doc_hash\":\"" + CanonicalHash(updatedDocument) +
             "\",\"delta\":" + SerializeDocumentDelta(delta) + "}\n";
    return 0;
}

}  // namespace

int UndoOperationCommand(const std::string& documentPath, std::string& output) {
    return MutateTimelineLogCommand(documentPath, false, output);
}

int RedoOperationCommand(const std::string& documentPath, std::string& output) {
    return MutateTimelineLogCommand(documentPath, true, output);
}
