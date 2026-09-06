#include "TimelineTranscription.h"

#include "AudioMixFilters.h"
#include "Timeline.h"

#include <algorithm>
#include <filesystem>
#include <iomanip>
#include <locale>
#include <sstream>
#include <stdexcept>

namespace {

constexpr int kWhisperSampleRate = 16000;

std::string Decimal(const RationalTime& time) {
    return audio_mix::Seconds(time);
}

std::filesystem::path ResolveMediaPath(const std::filesystem::path& base,
                                       const DocumentSource& source) {
    std::filesystem::path result(source.path);
    if (result.is_relative()) result = base / result;
    return std::filesystem::absolute(result).lexically_normal();
}

void HashByte(uint64_t& state, unsigned char byte) {
    state ^= byte;
    state *= 1099511628211ULL;
}

void HashText(uint64_t& state, const std::string& text) {
    for (const unsigned char byte : text) HashByte(state, byte);
    HashByte(state, 0xff);
}

void HashTime(uint64_t& state, const RationalTime& time) {
    HashText(state, std::to_string(time.value));
    HashText(state, std::to_string(time.rate));
}

struct AudioInput {
    const DocumentTrack* track = nullptr;
    const DocumentClip* clip = nullptr;
    const DocumentSource* source = nullptr;
};

}  // namespace

bool BuildTimelineAudioPlan(const Document& document,
                            const std::filesystem::path& projectPath,
                            TimelineAudioPlan& plan, std::string& error) {
    plan = {};
    error.clear();
    try {
        plan.duration = Timeline(document).Duration();
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
    if (plan.duration.value <= 0) {
        error = "cannot transcribe an empty timeline";
        return false;
    }

    std::vector<const DocumentTrack*> tracks;
    for (const DocumentTrack& track : document.sequence.tracks)
        if (track.kind == "audio") tracks.push_back(&track);
    std::stable_sort(tracks.begin(), tracks.end(),
                     [](const DocumentTrack* left, const DocumentTrack* right) {
                         return left->index < right->index;
                     });
    const bool hasSolo =
        std::any_of(tracks.begin(), tracks.end(),
                    [](const DocumentTrack* track) { return track->solo; });

    uint64_t fingerprint = 1469598103934665603ULL;
    // IA1 -- invalidate transcripts made before the clip envelopes and the
    // export limiter were included, even when every current knob is neutral.
    HashText(fingerprint, "timeline-audio-v2");
    HashText(fingerprint, document.sequence.id);
    HashTime(fingerprint, plan.duration);
    std::vector<AudioInput> inputs;
    for (const DocumentTrack* track : tracks) {
        HashText(fingerprint, track->id);
        HashText(fingerprint, track->muted ? "muted" : "audible");
        HashText(fingerprint, track->solo ? "solo" : "not-solo");
        if (track->muted || (hasSolo && !track->solo)) continue;
        std::vector<const DocumentClip*> clips;
        for (const DocumentClip& clip : track->clips) clips.push_back(&clip);
        std::stable_sort(
            clips.begin(), clips.end(),
            [](const DocumentClip* left, const DocumentClip* right) {
                if (left->timeline_in != right->timeline_in)
                    return left->timeline_in < right->timeline_in;
                return left->id < right->id;
            });
        for (const DocumentClip* clip : clips) {
            const DocumentSource* source = document.FindSource(clip->source_id);
            if (source == nullptr) {
                error = "audio clip references an unknown source";
                return false;
            }
            const LibraryMedia* media = document.FindLibraryMedia(source->id);
            if (media != nullptr && media->metadata_complete &&
                !media->has_audio) {
                continue;
            }
            HashText(fingerprint, clip->id);
            HashText(fingerprint, clip->source_id);
            HashTime(fingerprint, clip->source_in);
            HashTime(fingerprint, clip->duration);
            HashTime(fingerprint, clip->timeline_in);
            HashText(fingerprint, std::to_string(clip->audio_gain_db.num));
            HashText(fingerprint, std::to_string(clip->audio_gain_db.den));
            HashTime(fingerprint, clip->audio_fade_in);
            HashTime(fingerprint, clip->audio_fade_out);
            inputs.push_back({track, clip, source});
        }
    }
    if (inputs.empty()) {
        error = "timeline contains no audible audio clips";
        return false;
    }

    std::ostringstream identity;
    identity << "timeline:" << document.sequence.id << ':' << std::hex
             << std::setw(16) << std::setfill('0') << fingerprint;
    plan.cache_identity = identity.str();
    plan.audio_clips = inputs.size();

    std::vector<std::string> arguments = {"-hide_banner", "-loglevel", "error",
                                          "-nostdin"};
    const std::filesystem::path base =
        std::filesystem::absolute(projectPath).parent_path();
    for (const AudioInput& input : inputs) {
        arguments.push_back("-ss");
        arguments.push_back(Decimal(input.clip->source_in));
        arguments.push_back("-t");
        arguments.push_back(Decimal(input.clip->duration));
        arguments.push_back("-i");
        arguments.push_back(ResolveMediaPath(base, *input.source).string());
    }

    std::ostringstream graph;
    graph.imbue(std::locale::classic());
    for (size_t index = 0; index < inputs.size(); ++index) {
        const DocumentClip& clip = *inputs[index].clip;
        const __int128 numerator =
            static_cast<__int128>(clip.timeline_in.value) *
            audio_mix::kSampleRate;
        const __int128 denominator = clip.timeline_in.rate;
        const int64_t delay =
            static_cast<int64_t>((numerator + denominator / 2) / denominator);
        graph << '[' << index
              << ":a:0]atrim=duration=" << Decimal(clip.duration)
              << ",asetpts=PTS-STARTPTS,aresample=" << audio_mix::kSampleRate
              << ",aformat=sample_fmts=fltp:channel_layouts=stereo";
        audio_mix::AppendClipEnvelope(graph, clip);
        graph << ",adelay=" << delay << "S:all=1[a" << index << "];";
    }
    for (size_t index = 0; index < inputs.size(); ++index)
        graph << "[a" << index << ']';
    // Match the default export's stereo mix and limiting before the final
    // mono/16 kHz conversion Whisper needs; limiting after downmixing would
    // change the balance of a loud channel against a quiet one.
    audio_mix::AppendMixedOutput(graph, inputs.size(), plan.duration);
    arguments.insert(
        arguments.end(),
        {"-filter_complex", graph.str(), "-map", "[audio]", "-ac", "1", "-ar",
         std::to_string(kWhisperSampleRate), "-f", "f32le", "pipe:1"});
    plan.ffmpeg_arguments = std::move(arguments);
    return true;
}

bool TranscribeTimelineAudio(const Document& document,
                             const std::filesystem::path& projectPath,
                             const std::filesystem::path& cachePath,
                             const WhisperSettings& settings,
                             MediaTaskContext& context, Transcript& transcript,
                             bool& cacheHit, std::string& error) {
    TimelineAudioPlan plan;
    if (!BuildTimelineAudioPlan(document, projectPath, plan, error))
        return false;
    Transcript cached;
    std::string cacheError;
    if (LoadAudioTranscript(cachePath.string(), cached, cacheError) &&
        TranscriptCacheMatches(cached, plan.cache_identity, settings)) {
        transcript = std::move(cached);
        cacheHit = true;
        return true;
    }

    std::vector<float> samples;
    if (!DecodeFfmpegAudioToPcm16k(settings.ffmpeg_path, plan.ffmpeg_arguments,
                                   context, samples, error)) {
        return false;
    }
    if (!GenerateAudioTranscriptFromPcm(
            samples, cachePath.string(), plan.cache_identity,
            document.sequence.frame_rate, settings, context, error)) {
        return false;
    }
    if (!LoadAudioTranscript(cachePath.string(), transcript, error))
        return false;
    cacheHit = false;
    return true;
}
