#include "Operations.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace {

void Fail(EditError code, const std::string& text, EditError& error,
          std::string& message) {
    error = code;
    message = text;
}

ExactClipTimes TimesOf(const DocumentClip& clip) {
    return {clip.source_in, clip.duration, clip.timeline_in};
}

std::vector<ExactTimelinePosition> PositionsAfter(const DocumentTrack& track,
                                                  size_t first) {
    std::vector<ExactTimelinePosition> positions;
    for (size_t index = first; index < track.clips.size(); ++index) {
        positions.push_back(
            {track.clips[index].id, track.clips[index].timeline_in});
    }
    return positions;
}

bool ApplyExactPositions(Document& document,
                         const std::vector<ExactTimelinePosition>& positions,
                         EditError& error, std::string& message) {
    for (const ExactTimelinePosition& position : positions) {
        DocumentClip* clip = document.FindClip(position.clip_id);
        if (!clip) {
            Fail(EditError::UnknownClip,
                 "exact ripple state references unknown clip_id '" +
                     position.clip_id + "'",
                 error, message);
            return false;
        }
        if (position.timeline_in.rate <= 0 || position.timeline_in.value < 0) {
            Fail(EditError::InvalidTimelineIn,
                 "exact ripple state has invalid timeline_in", error, message);
            return false;
        }
        clip->timeline_in = position.timeline_in;
    }
    return true;
}

bool ValidateResult(const Document& candidate, EditError& error,
                    std::string& message) {
    std::string validation;
    if (candidate.Validate(validation)) return true;
    const EditError code = validation.find("overlap") != std::string::npos
                               ? EditError::Overlap
                               : EditError::ValidationFailed;
    Fail(code, validation, error, message);
    return false;
}

bool ValidateSourceRange(const DocumentSource& source,
                         const RationalTime& sourceIn,
                         const RationalTime& duration, EditError& error,
                         std::string& message) {
    if (sourceIn.rate <= 0 || duration.rate <= 0) {
        Fail(EditError::ArithmeticError, "time rate must be positive", error,
             message);
        return false;
    }
    if (duration.value <= 0) {
        Fail(EditError::InvalidDuration, "duration must be positive", error,
             message);
        return false;
    }
    if (sourceIn.value < 0 || sourceIn.add(duration) > source.duration) {
        Fail(EditError::SourceOutOfBounds,
             "source range is outside source_id '" + source.id + "'", error,
             message);
        return false;
    }
    return true;
}

bool ApplyInsert(Document& candidate, InsertClipOperation& operation,
                 Operation& inverse, EditError& error, std::string& message) {
    DocumentTrack* track = candidate.FindTrack(operation.track_id);
    if (!track) {
        Fail(EditError::UnknownTrack,
             "unknown track_id '" + operation.track_id + "'", error, message);
        return false;
    }
    const DocumentSource* source = candidate.FindSource(operation.source_id);
    if (!source) {
        Fail(EditError::UnknownSource,
             "unknown source_id '" + operation.source_id + "'", error, message);
        return false;
    }
    if (operation.timeline_in.rate <= 0 || operation.timeline_in.value < 0) {
        Fail(EditError::InvalidTimelineIn,
             "timeline_in must be non-negative with a positive rate", error,
             message);
        return false;
    }
    if (!ValidateSourceRange(*source, operation.source_in, operation.duration,
                             error, message)) {
        return false;
    }

    if (operation.clip_id.empty()) operation.clip_id = GenerateUlid();
    if (!IsValidUlid(operation.clip_id) ||
        candidate.FindClip(operation.clip_id) ||
        candidate.FindSource(operation.clip_id) ||
        candidate.FindTrack(operation.clip_id)) {
        Fail(EditError::DuplicateId,
             "insert clip_id is invalid or already exists: '" +
                 operation.clip_id + "'",
             error, message);
        return false;
    }

    auto insertion = std::lower_bound(
        track->clips.begin(), track->clips.end(), operation.timeline_in,
        [](const DocumentClip& clip, const RationalTime& position) {
            return clip.timeline_in < position;
        });
    const size_t insertionIndex =
        static_cast<size_t>(std::distance(track->clips.begin(), insertion));
    if (insertion != track->clips.begin()) {
        const DocumentClip& previous = *std::prev(insertion);
        if (operation.timeline_in <
            previous.timeline_in.add(previous.duration)) {
            Fail(EditError::Overlap,
                 "insertion timeline_in overlaps clip_id '" + previous.id + "'",
                 error, message);
            return false;
        }
    }

    const std::vector<ExactTimelinePosition> before =
        PositionsAfter(*track, insertionIndex);
    for (size_t index = insertionIndex; index < track->clips.size(); ++index) {
        track->clips[index].timeline_in =
            track->clips[index].timeline_in.add(operation.duration);
    }
    track->clips.insert(
        track->clips.begin() + static_cast<std::ptrdiff_t>(insertionIndex),
        DocumentClip{operation.clip_id, operation.source_id,
                     operation.source_in, operation.duration,
                     operation.timeline_in});
    if (!operation.exact_timeline_result.empty() &&
        !ApplyExactPositions(candidate, operation.exact_timeline_result, error,
                             message)) {
        return false;
    }
    if (!ValidateResult(candidate, error, message)) return false;

    if (operation.exact_timeline_result.empty()) {
        DocumentTrack* updated = candidate.FindTrack(operation.track_id);
        operation.exact_timeline_result =
            PositionsAfter(*updated, insertionIndex + 1);
    }
    inverse = RemoveClipOperation{operation.clip_id, before};
    return true;
}

bool ApplyRemove(Document& candidate, RemoveClipOperation& operation,
                 Operation& inverse, EditError& error, std::string& message) {
    DocumentTrack* track = candidate.FindTrackForClip(operation.clip_id);
    if (!track) {
        Fail(EditError::UnknownClip,
             "unknown clip_id '" + operation.clip_id + "'", error, message);
        return false;
    }
    const auto found = std::find_if(
        track->clips.begin(), track->clips.end(),
        [&](const DocumentClip& clip) { return clip.id == operation.clip_id; });
    const size_t index =
        static_cast<size_t>(std::distance(track->clips.begin(), found));
    const DocumentClip removed = *found;
    const Ulid trackId = track->id;
    const std::vector<ExactTimelinePosition> before =
        PositionsAfter(*track, index + 1);
    for (size_t next = index + 1; next < track->clips.size(); ++next) {
        track->clips[next].timeline_in =
            track->clips[next].timeline_in.sub(removed.duration);
    }
    track->clips.erase(track->clips.begin() +
                       static_cast<std::ptrdiff_t>(index));
    if (!operation.exact_timeline_result.empty() &&
        !ApplyExactPositions(candidate, operation.exact_timeline_result, error,
                             message)) {
        return false;
    }
    if (!ValidateResult(candidate, error, message)) return false;
    if (operation.exact_timeline_result.empty()) {
        DocumentTrack* updated = candidate.FindTrack(trackId);
        operation.exact_timeline_result = PositionsAfter(*updated, index);
    }
    inverse = InsertClipOperation{trackId,
                                  removed.source_id,
                                  removed.source_in,
                                  removed.duration,
                                  removed.timeline_in,
                                  removed.id,
                                  before};
    return true;
}

bool Negate(const RationalTime& value, RationalTime& output) {
    if (value.value == std::numeric_limits<int64_t>::min()) return false;
    output = {-value.value, value.rate};
    return true;
}

bool ApplyTrim(Document& candidate, TrimClipOperation& operation,
               Operation& inverse, EditError& error, std::string& message) {
    DocumentClip* clip = candidate.FindClip(operation.clip_id);
    if (!clip) {
        Fail(EditError::UnknownClip,
             "unknown clip_id '" + operation.clip_id + "'", error, message);
        return false;
    }
    if (operation.delta.rate <= 0) {
        Fail(EditError::ArithmeticError, "trim delta rate must be positive",
             error, message);
        return false;
    }
    const DocumentSource* source = candidate.FindSource(clip->source_id);
    if (!source) {
        Fail(EditError::UnknownSource,
             "clip references unknown source_id '" + clip->source_id + "'",
             error, message);
        return false;
    }
    const ExactClipTimes before = TimesOf(*clip);
    if (operation.edge == TrimEdge::Head) {
        clip->source_in = clip->source_in.add(operation.delta);
        clip->duration = clip->duration.sub(operation.delta);
        clip->timeline_in = clip->timeline_in.add(operation.delta);
    } else {
        clip->duration = clip->duration.add(operation.delta);
    }
    if (clip->duration.value <= 0) {
        Fail(EditError::InvalidDuration,
             "trim would make duration zero or negative", error, message);
        return false;
    }
    if (clip->timeline_in.value < 0) {
        Fail(EditError::InvalidTimelineIn,
             "trim would make timeline_in negative", error, message);
        return false;
    }
    if (!ValidateSourceRange(*source, clip->source_in, clip->duration, error,
                             message)) {
        return false;
    }
    if (operation.exact_clip_result) {
        clip->source_in = operation.exact_clip_result->source_in;
        clip->duration = operation.exact_clip_result->duration;
        clip->timeline_in = operation.exact_clip_result->timeline_in;
    }
    if (!ValidateResult(candidate, error, message)) return false;
    if (!operation.exact_clip_result)
        operation.exact_clip_result = TimesOf(*clip);

    RationalTime inverseDelta;
    if (!Negate(operation.delta, inverseDelta)) {
        Fail(EditError::ArithmeticError, "trim delta cannot be negated", error,
             message);
        return false;
    }
    inverse = TrimClipOperation{operation.clip_id, operation.edge, inverseDelta,
                                before};
    return true;
}

void WriteTime(std::ostringstream& output, const RationalTime& time) {
    output << "{\"value\":" << time.value << ",\"rate\":" << time.rate << "}";
}

void WriteExactPositions(std::ostringstream& output,
                         const std::vector<ExactTimelinePosition>& positions) {
    output << '[';
    for (size_t index = 0; index < positions.size(); ++index) {
        if (index) output << ',';
        output << "{\"clip_id\":\"" << positions[index].clip_id
               << "\",\"timeline_in\":";
        WriteTime(output, positions[index].timeline_in);
        output << '}';
    }
    output << ']';
}

class Reader {
public:
    explicit Reader(const std::string& input) : input_(input) {}

    void Expect(const std::string& text) {
        Skip();
        if (input_.compare(position_, text.size(), text) != 0) {
            throw std::runtime_error("expected '" + text + "' at byte " +
                                     std::to_string(position_));
        }
        position_ += text.size();
    }

    bool Consume(const std::string& text) {
        Skip();
        if (input_.compare(position_, text.size(), text) != 0) return false;
        position_ += text.size();
        return true;
    }

    std::string String() {
        Skip();
        if (position_ >= input_.size() || input_[position_++] != '"') {
            throw std::runtime_error("expected string at byte " +
                                     std::to_string(position_));
        }
        std::string output;
        while (position_ < input_.size()) {
            const char character = input_[position_++];
            if (character == '"') return output;
            if (character == '\\') {
                if (position_ >= input_.size())
                    throw std::runtime_error("unterminated string escape");
                const char escaped = input_[position_++];
                if (escaped != '"' && escaped != '\\')
                    throw std::runtime_error("unsupported string escape");
                output.push_back(escaped);
            } else {
                output.push_back(character);
            }
        }
        throw std::runtime_error("unterminated string");
    }

    int64_t Integer() {
        Skip();
        const size_t start = position_;
        if (position_ < input_.size() && input_[position_] == '-') ++position_;
        while (position_ < input_.size() &&
               std::isdigit(static_cast<unsigned char>(input_[position_]))) {
            ++position_;
        }
        if (position_ == start ||
            (position_ == start + 1 && input_[start] == '-')) {
            throw std::runtime_error("expected integer at byte " +
                                     std::to_string(start));
        }
        char* end = nullptr;
        const std::string text = input_.substr(start, position_ - start);
        const long long value = std::strtoll(text.c_str(), &end, 10);
        if (!end || *end != '\0') throw std::runtime_error("invalid integer");
        return static_cast<int64_t>(value);
    }

    void Finish() {
        Skip();
        if (position_ != input_.size())
            throw std::runtime_error("unexpected trailing operation JSON");
    }

private:
    void Skip() {
        while (position_ < input_.size() &&
               std::isspace(static_cast<unsigned char>(input_[position_]))) {
            ++position_;
        }
    }
    const std::string& input_;
    size_t position_ = 0;
};

RationalTime ReadTime(Reader& reader) {
    reader.Expect("{\"value\":");
    const int64_t value = reader.Integer();
    reader.Expect(",\"rate\":");
    const int64_t rate = reader.Integer();
    reader.Expect("}");
    if (rate < std::numeric_limits<int32_t>::min() ||
        rate > std::numeric_limits<int32_t>::max()) {
        throw std::runtime_error("RationalTime rate outside int32_t range");
    }
    return {value, static_cast<int32_t>(rate)};
}

std::vector<ExactTimelinePosition> ReadExactPositions(Reader& reader) {
    std::vector<ExactTimelinePosition> positions;
    reader.Expect("[");
    if (reader.Consume("]")) return positions;
    while (true) {
        reader.Expect("{\"clip_id\":");
        const Ulid id = reader.String();
        reader.Expect(",\"timeline_in\":");
        const RationalTime timelineIn = ReadTime(reader);
        reader.Expect("}");
        positions.push_back({id, timelineIn});
        if (reader.Consume("]")) return positions;
        reader.Expect(",");
    }
}

}  // namespace

const char* EditErrorName(EditError error) {
    switch (error) {
        case EditError::None:
            return "None";
        case EditError::UnknownTrack:
            return "UnknownTrack";
        case EditError::UnknownClip:
            return "UnknownClip";
        case EditError::UnknownSource:
            return "UnknownSource";
        case EditError::InvalidDuration:
            return "InvalidDuration";
        case EditError::InvalidTimelineIn:
            return "InvalidTimelineIn";
        case EditError::SourceOutOfBounds:
            return "SourceOutOfBounds";
        case EditError::Overlap:
            return "Overlap";
        case EditError::DuplicateId:
            return "DuplicateId";
        case EditError::ArithmeticError:
            return "ArithmeticError";
        case EditError::InvalidOperation:
            return "InvalidOperation";
        case EditError::ValidationFailed:
            return "ValidationFailed";
        case EditError::EmptyUndo:
            return "EmptyUndo";
        case EditError::EmptyRedo:
            return "EmptyRedo";
        case EditError::IoError:
            return "IoError";
        case EditError::ParseError:
            return "ParseError";
    }
    return "InvalidOperation";
}

bool ApplyOperation(Document& document, Operation& operation,
                    Operation& inverse, EditError& error,
                    std::string& message) {
    Document candidate = document;
    Operation normalized = operation;
    Operation generatedInverse = RemoveClipOperation{};
    try {
        bool applied = false;
        if (auto* insert = std::get_if<InsertClipOperation>(&normalized)) {
            applied = ApplyInsert(candidate, *insert, generatedInverse, error,
                                  message);
        } else if (auto* remove =
                       std::get_if<RemoveClipOperation>(&normalized)) {
            applied = ApplyRemove(candidate, *remove, generatedInverse, error,
                                  message);
        } else if (auto* trim = std::get_if<TrimClipOperation>(&normalized)) {
            applied =
                ApplyTrim(candidate, *trim, generatedInverse, error, message);
        }
        if (!applied) return false;
    } catch (const std::exception& exception) {
        Fail(EditError::ArithmeticError, exception.what(), error, message);
        return false;
    }
    document = std::move(candidate);
    operation = std::move(normalized);
    inverse = std::move(generatedInverse);
    error = EditError::None;
    message.clear();
    return true;
}

std::string SerializeOperation(const Operation& operation) {
    std::ostringstream output;
    if (const auto* insert = std::get_if<InsertClipOperation>(&operation)) {
        output << "{\"type\":\"InsertClip\",\"track_id\":\"" << insert->track_id
               << "\",\"source_id\":\"" << insert->source_id
               << "\",\"source_in\":";
        WriteTime(output, insert->source_in);
        output << ",\"duration\":";
        WriteTime(output, insert->duration);
        output << ",\"timeline_in\":";
        WriteTime(output, insert->timeline_in);
        output << ",\"clip_id\":\"" << insert->clip_id
               << "\",\"exact_timeline\":";
        WriteExactPositions(output, insert->exact_timeline_result);
        output << '}';
    } else if (const auto* remove =
                   std::get_if<RemoveClipOperation>(&operation)) {
        output << "{\"type\":\"RemoveClip\",\"clip_id\":\"" << remove->clip_id
               << "\",\"exact_timeline\":";
        WriteExactPositions(output, remove->exact_timeline_result);
        output << '}';
    } else {
        const auto& trim = std::get<TrimClipOperation>(operation);
        output << "{\"type\":\"TrimClip\",\"clip_id\":\"" << trim.clip_id
               << "\",\"edge\":\""
               << (trim.edge == TrimEdge::Head ? "Head" : "Tail")
               << "\",\"delta\":";
        WriteTime(output, trim.delta);
        output << ",\"exact_clip\":";
        if (!trim.exact_clip_result) {
            output << "null";
        } else {
            output << "{\"source_in\":";
            WriteTime(output, trim.exact_clip_result->source_in);
            output << ",\"duration\":";
            WriteTime(output, trim.exact_clip_result->duration);
            output << ",\"timeline_in\":";
            WriteTime(output, trim.exact_clip_result->timeline_in);
            output << '}';
        }
        output << '}';
    }
    return output.str();
}

bool DeserializeOperation(const std::string& json, Operation& operation,
                          EditError& error, std::string& message) {
    try {
        Reader reader(json);
        reader.Expect("{\"type\":");
        const std::string type = reader.String();
        if (type == "InsertClip") {
            reader.Expect(",\"track_id\":");
            InsertClipOperation value;
            value.track_id = reader.String();
            reader.Expect(",\"source_id\":");
            value.source_id = reader.String();
            reader.Expect(",\"source_in\":");
            value.source_in = ReadTime(reader);
            reader.Expect(",\"duration\":");
            value.duration = ReadTime(reader);
            reader.Expect(",\"timeline_in\":");
            value.timeline_in = ReadTime(reader);
            reader.Expect(",\"clip_id\":");
            value.clip_id = reader.String();
            reader.Expect(",\"exact_timeline\":");
            value.exact_timeline_result = ReadExactPositions(reader);
            reader.Expect("}");
            operation = std::move(value);
        } else if (type == "RemoveClip") {
            reader.Expect(",\"clip_id\":");
            RemoveClipOperation value;
            value.clip_id = reader.String();
            reader.Expect(",\"exact_timeline\":");
            value.exact_timeline_result = ReadExactPositions(reader);
            reader.Expect("}");
            operation = std::move(value);
        } else if (type == "TrimClip") {
            reader.Expect(",\"clip_id\":");
            TrimClipOperation value;
            value.clip_id = reader.String();
            reader.Expect(",\"edge\":");
            const std::string edge = reader.String();
            if (edge == "Head")
                value.edge = TrimEdge::Head;
            else if (edge == "Tail")
                value.edge = TrimEdge::Tail;
            else
                throw std::runtime_error("unknown trim edge '" + edge + "'");
            reader.Expect(",\"delta\":");
            value.delta = ReadTime(reader);
            reader.Expect(",\"exact_clip\":");
            if (!reader.Consume("null")) {
                reader.Expect("{\"source_in\":");
                ExactClipTimes exact;
                exact.source_in = ReadTime(reader);
                reader.Expect(",\"duration\":");
                exact.duration = ReadTime(reader);
                reader.Expect(",\"timeline_in\":");
                exact.timeline_in = ReadTime(reader);
                reader.Expect("}");
                value.exact_clip_result = exact;
            }
            reader.Expect("}");
            operation = std::move(value);
        } else {
            throw std::runtime_error("unknown operation type '" + type + "'");
        }
        reader.Finish();
        error = EditError::None;
        message.clear();
        return true;
    } catch (const std::exception& exception) {
        error = EditError::ParseError;
        message = exception.what();
        return false;
    }
}
