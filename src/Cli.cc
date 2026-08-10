#include "Cli.h"

#include "Document.h"
#include "EditLog.h"
#include "Operations.h"
#include "Timeline.h"
#include "Ulid.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>
#include <sstream>
#include <string>
#include <system_error>
#include <set>
#include <vector>

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
    return "{\"ok\":false,\"error\":\"" + std::string(EditErrorName(error)) +
           "\",\"detail\":\"" + EscapeJson(detail) + "\"}\n";
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
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        message = "unable to create '" + path.string() + "'";
        return false;
    }
    output << contents;
    output.close();
    if (!output) {
        message = "unable to write '" + path.string() + "'";
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

// Commits document and edit log as one recoverable pair. All validation and
// serialization happen before this function creates any file.
bool CommitPair(const std::string& documentPath,
                const std::string& documentJson, const std::string& logJson,
                std::string& message) {
    const std::filesystem::path document(documentPath);
    const std::filesystem::path log(EditLogPathForDocument(documentPath));
    const std::string nonce = ".cutmachine-" + GenerateUlid();
    const std::filesystem::path documentTemp =
        document.string() + nonce + ".tmp";
    const std::filesystem::path logTemp = log.string() + nonce + ".tmp";
    const std::filesystem::path documentBackup =
        document.string() + nonce + ".bak";
    const std::filesystem::path logBackup = log.string() + nonce + ".bak";

    if (!WriteFile(documentTemp, documentJson, message)) return false;
    if (!WriteFile(logTemp, logJson, message)) {
        RemoveIfPresent(documentTemp);
        return false;
    }

    std::error_code existsError;
    const bool hadLog = std::filesystem::exists(log, existsError);
    if (existsError) {
        message = "unable to inspect edit log '" + log.string() +
                  "': " + existsError.message();
        RemoveIfPresent(documentTemp);
        RemoveIfPresent(logTemp);
        return false;
    }

    if (!Rename(document, documentBackup, message)) {
        RemoveIfPresent(documentTemp);
        RemoveIfPresent(logTemp);
        return false;
    }
    if (hadLog && !Rename(log, logBackup, message)) {
        std::string ignored;
        Rename(documentBackup, document, ignored);
        RemoveIfPresent(documentTemp);
        RemoveIfPresent(logTemp);
        return false;
    }
    if (!Rename(documentTemp, document, message)) {
        std::string ignored;
        if (hadLog) Rename(logBackup, log, ignored);
        Rename(documentBackup, document, ignored);
        RemoveIfPresent(logTemp);
        return false;
    }
    if (!Rename(logTemp, log, message)) {
        std::string ignored;
        RemoveIfPresent(document);
        Rename(documentBackup, document, ignored);
        if (hadLog) Rename(logBackup, log, ignored);
        return false;
    }

    RemoveIfPresent(documentBackup);
    if (hadLog) RemoveIfPresent(logBackup);
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
    if (!document.sources.empty()) return document.sources.front().rate;
    return {1, 1};
}

std::string Describe(const Document& document) {
    const MediaRate timelineRate = PresentationRate(document);
    Timeline timeline(document);
    const RationalTime duration = timeline.Duration();
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << "{\"timeline\":{\"sources\":[";
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
    for (const DocumentTrack& track : document.tracks) tracks.push_back(&track);
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
            output << "{\"type\":\"clip\",\"alias\":\""
                   << AliasPrefix(trackOrdinal) << (clipIndex + 1)
                   << "\",\"id\":\"" << EscapeJson(clip.id)
                   << "\",\"source_id\":\"" << EscapeJson(clip.source_id)
                   << "\",\"source_in\":";
            WriteTime(output, clip.source_in, source->rate);
            output << ",\"timeline_in\":";
            WriteTime(output, clip.timeline_in, timelineRate);
            output << ",\"duration\":";
            WriteTime(output, clip.duration, timelineRate);
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
    for (const DocumentTrack& track : document.tracks) {
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
                   << ",\"height\":" << media.height;
        }
        output << ",\"rate\":{\"num\":" << media.rate.num
               << ",\"den\":" << media.rate.den
               << "},\"duration\":{\"value\":" << media.duration.value
               << ",\"rate\":" << media.duration.rate << "}";
        if (media.metadata_complete) {
            output << ",\"orientation\":\""
                   << EscapeJson(media.orientation) << "\",\"has_audio\":"
                   << (media.has_audio ? "true" : "false");
            if (media.has_audio) {
                output << ",\"audio_rate\":" << media.audio_rate
                       << ",\"audio_channels\":" << media.audio_channels;
            }
        }
        output << ",\"in_use\":"
               << (usedMedia.count(media.id) ? "true" : "false") << '}';
    }
    output << "]}\n";
    return output.str();
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

std::string EditLogPathForDocument(const std::string& documentPath) {
    return documentPath + ".editlog.json";
}

int DescribeCommand(const std::string& documentPath, std::string& output) {
    std::string json;
    std::string message;
    if (!ReadFile(documentPath, json, message)) {
        output = ErrorJson(EditError::IoError, message);
        return 1;
    }
    Document document;
    if (!Document::LoadFromString(json, document, message)) {
        output = ErrorJson(EditError::ParseError, message);
        return 1;
    }
    try {
        output = Describe(document);
        return 0;
    } catch (const std::exception& exception) {
        output = ErrorJson(EditError::ArithmeticError, exception.what());
        return 1;
    }
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

    std::string documentJson;
    if (!ReadFile(documentPath, documentJson, message)) {
        output = ErrorJson(EditError::IoError, message);
        return 1;
    }
    Document document;
    if (!Document::LoadFromString(documentJson, document, message)) {
        output = ErrorJson(EditError::ParseError, message);
        return 1;
    }

    EditLog log;
    const std::string logPath = EditLogPathForDocument(documentPath);
    std::error_code existsError;
    const bool logExists = std::filesystem::exists(logPath, existsError);
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
    const std::string updatedLog = log.Serialize();
    if (!CommitPair(documentPath, updatedDocument, updatedLog, message)) {
        output = ErrorJson(EditError::IoError, message);
        return 1;
    }
    output = "{\"ok\":true,\"doc_hash\":\"" + CanonicalHash(updatedDocument) +
             "\"}\n";
    return 0;
}
