#include "Subtitles.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <string_view>

namespace {

std::string Trim(std::string value) {
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.front())))
        value.erase(value.begin());
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.back())))
        value.pop_back();
    return value;
}

bool ParseTimestamp(std::string value, RationalTime& output) {
    value = Trim(std::move(value));
    if (value.size() != 12 || value[2] != ':' || value[5] != ':' ||
        (value[8] != ',' && value[8] != '.'))
        return false;
    constexpr size_t kDigitIndices[] = {0, 1, 3, 4, 6, 7, 9, 10, 11};
    for (size_t index : kDigitIndices)
        if (!std::isdigit(static_cast<unsigned char>(value[index])))
            return false;
    const int64_t hours = std::stoll(value.substr(0, 2));
    const int64_t minutes = std::stoll(value.substr(3, 2));
    const int64_t seconds = std::stoll(value.substr(6, 2));
    const int64_t milliseconds = std::stoll(value.substr(9, 3));
    if (minutes >= 60 || seconds >= 60) return false;
    output = {((hours * 60 + minutes) * 60 + seconds) * 1000 + milliseconds,
              1000};
    return true;
}

bool ParseTiming(const std::string& line, RationalTime& start,
                 RationalTime& end) {
    const size_t arrow = line.find("-->");
    if (arrow == std::string::npos) return false;
    return ParseTimestamp(line.substr(0, arrow), start) &&
           ParseTimestamp(line.substr(arrow + 3), end) && end > start;
}

std::string FormatTimestamp(RationalTime time) {
    const RationalTime milliseconds = time.rescale(1000);
    int64_t value = milliseconds.value;
    const int64_t hours = value / 3600000;
    value %= 3600000;
    const int64_t minutes = value / 60000;
    value %= 60000;
    const int64_t seconds = value / 1000;
    value %= 1000;
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::setfill('0') << std::setw(2) << hours << ':' << std::setw(2)
           << minutes << ':' << std::setw(2) << seconds << ',' << std::setw(3)
           << value;
    return output.str();
}

bool EndsSentence(const std::string& text) {
    if (text.empty()) return false;
    const char last = text.back();
    return last == '.' || last == '?' || last == '!';
}

bool JoinsWithoutSpace(const std::string& text) {
    if (text.empty()) return false;
    constexpr std::string_view kPunctuation = ".,!?;:%)]}";
    return kPunctuation.find(text.front()) != std::string_view::npos;
}

}  // namespace

bool ParseSrt(const std::string& contents, std::vector<SubtitleCue>& cues,
              std::string& error) {
    cues.clear();
    error.clear();
    std::string normalized;
    normalized.reserve(contents.size());
    for (char character : contents)
        if (character != '\r') normalized.push_back(character);
    if (normalized.compare(0, 3, "\xEF\xBB\xBF") == 0) normalized.erase(0, 3);

    std::istringstream input(normalized);
    std::string line;
    size_t lineNumber = 0;
    while (std::getline(input, line)) {
        ++lineNumber;
        if (Trim(line).empty()) continue;
        std::string timing = line;
        if (line.find("-->") == std::string::npos) {
            if (!std::all_of(line.begin(), line.end(),
                             [](unsigned char c) {
                                 return std::isdigit(c) || std::isspace(c);
                             }) ||
                !std::getline(input, timing)) {
                error = "invalid SRT cue index at line " +
                        std::to_string(lineNumber);
                return false;
            }
            ++lineNumber;
        }
        RationalTime start;
        RationalTime end;
        if (!ParseTiming(timing, start, end)) {
            error = "invalid SRT timing at line " + std::to_string(lineNumber);
            return false;
        }
        std::string text;
        while (std::getline(input, line)) {
            ++lineNumber;
            if (Trim(line).empty()) break;
            if (!text.empty()) text.push_back('\n');
            text += line;
        }
        if (text.empty()) {
            error = "empty SRT cue at line " + std::to_string(lineNumber);
            return false;
        }
        if (!cues.empty() &&
            start < cues.back().timeline_in.add(cues.back().duration)) {
            error = "overlapping SRT cues require separate subtitle tracks";
            return false;
        }
        cues.push_back({start, end.sub(start), std::move(text)});
    }
    if (cues.empty()) {
        error = "SRT file contains no cues";
        return false;
    }
    return true;
}

bool LoadSrt(const std::string& path, std::vector<SubtitleCue>& cues,
             std::string& error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "unable to open SRT file";
        return false;
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    if (!input.good() && !input.eof()) {
        error = "unable to read SRT file";
        return false;
    }
    return ParseSrt(contents.str(), cues, error);
}

AddTrackOperation BuildSubtitleTrackEdit(const std::vector<SubtitleCue>& cues,
                                         int32_t trackIndex,
                                         const Ulid& trackId) {
    AddTrackOperation operation;
    operation.track_id = trackId.empty() ? GenerateUlid() : trackId;
    operation.kind = "caption";
    operation.index = trackIndex;
    operation.sync_lock = false;
    for (const SubtitleCue& cue : cues) {
        DocumentClip clip;
        clip.source_id.clear();
        clip.source_in = {0, cue.timeline_in.rate};
        clip.timeline_in = cue.timeline_in;
        clip.duration = cue.duration;
        clip.include_audio = false;
        clip.caption_text = cue.text;
        operation.clips.push_back(std::move(clip));
    }
    return operation;
}

bool WriteSrt(const std::vector<SubtitleCue>& cues, const std::string& path,
              std::string& error) {
    error.clear();
    if (cues.empty()) {
        error = "timeline contains no visible subtitles";
        return false;
    }
    try {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output) {
            error = "unable to create SRT file";
            return false;
        }
        for (size_t index = 0; index < cues.size(); ++index) {
            output << index + 1 << '\n'
                   << FormatTimestamp(cues[index].timeline_in) << " --> "
                   << FormatTimestamp(
                          cues[index].timeline_in.add(cues[index].duration))
                   << '\n'
                   << cues[index].text << "\n\n";
        }
        if (!output) {
            error = "unable to write SRT file";
            return false;
        }
    } catch (const std::exception& exception) {
        error =
            std::string("subtitle time is not exactly representable in SRT: ") +
            exception.what();
        return false;
    }
    return true;
}

bool SaveSrt(const Document& document, const std::string& path,
             std::string& error) {
    error.clear();
    std::vector<const DocumentClip*> ordered;
    for (const DocumentTrack& track : document.sequence.tracks)
        if (track.kind == "caption" && track.visible)
            for (const DocumentClip& clip : track.clips)
                ordered.push_back(&clip);
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const DocumentClip* left, const DocumentClip* right) {
                         return left->timeline_in < right->timeline_in;
                     });
    std::vector<SubtitleCue> cues;
    cues.reserve(ordered.size());
    for (const DocumentClip* clip : ordered)
        cues.push_back({clip->timeline_in, clip->duration, clip->caption_text});
    return WriteSrt(cues, path, error);
}

std::vector<const DocumentClip*> ActiveSubtitles(const Document& document,
                                                 RationalTime position) {
    std::vector<const DocumentClip*> result;
    for (const DocumentTrack& track : document.sequence.tracks) {
        if (track.kind != "caption" || !track.visible) continue;
        for (const DocumentClip& clip : track.clips)
            if (position >= clip.timeline_in &&
                position < clip.timeline_in.add(clip.duration))
                result.push_back(&clip);
    }
    return result;
}

bool SubtitleCuesForClip(const Transcript& transcript, const DocumentClip& clip,
                         std::vector<SubtitleCue>& cues, std::string& error) {
    cues.clear();
    error.clear();
    if (clip.source_id.empty() || transcript.media_id != clip.source_id) {
        error = "transcript does not match clip source";
        return false;
    }

    const RationalTime sourceEnd = clip.source_in.add(clip.duration);
    std::string cueText;
    RationalTime cueStart;
    RationalTime cueEnd;
    std::string previousWord;
    bool hasCue = false;
    auto finishCue = [&]() {
        if (!hasCue) return;
        cues.push_back({cueStart, cueEnd.sub(cueStart), cueText});
        cueText.clear();
        previousWord.clear();
        hasCue = false;
    };

    constexpr size_t kMaximumCharacters = 42;
    const RationalTime kMaximumGap = kSubtitleCueMaximumGap;
    const RationalTime kMaximumDuration{5, 1};
    for (const TranscriptWord& word : transcript.words) {
        if (word.end <= clip.source_in || word.start >= sourceEnd) continue;
        const RationalTime sourceStart =
            word.start < clip.source_in ? clip.source_in : word.start;
        const RationalTime clippedEnd =
            word.end > sourceEnd ? sourceEnd : word.end;
        const RationalTime timelineStart =
            clip.timeline_in.add(sourceStart.sub(clip.source_in));
        const RationalTime timelineEnd =
            clip.timeline_in.add(clippedEnd.sub(clip.source_in));
        std::string text = Trim(word.text);
        if (text.empty() || timelineEnd <= timelineStart) continue;

        const bool joins = JoinsWithoutSpace(text);
        const size_t joinedLength =
            cueText.size() + (joins ? 0 : 1) + text.size();
        const bool breakBefore =
            hasCue &&
            (timelineStart.sub(cueEnd) >= kMaximumGap ||
             EndsSentence(previousWord) || joinedLength > kMaximumCharacters ||
             timelineEnd.sub(cueStart) > kMaximumDuration);
        if (breakBefore) finishCue();

        if (!hasCue) {
            cueStart = timelineStart;
            cueText = text;
            hasCue = true;
        } else {
            if (!joins) cueText.push_back(' ');
            cueText += text;
        }
        cueEnd = timelineEnd;
        previousWord = std::move(text);
    }
    finishCue();
    if (cues.empty()) {
        error = "transcript has no words inside clip source range";
        return false;
    }
    return true;
}
