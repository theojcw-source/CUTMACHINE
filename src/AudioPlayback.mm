#include "AudioPlayback.h"

#import <AVFAudio/AVFAudio.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr int32_t kMixRate = 48000;
constexpr int32_t kScrubSamples = 2880;     // 60 ms
constexpr int32_t kScrubFadeSamples = 240;  // 5 ms

std::string AvError(int value) {
    char text[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(value, text, sizeof(text));
    return text;
}

struct PCMSource {
    std::vector<float> left;
    std::vector<float> right;
};

// `cancel` lets the decode thread abandon a rush in progress. Without it,
// closing a project waited for the whole file: a 30-minute interview is
// several seconds of decoding, and quitting looked like a hang.
bool DecodeSource(const std::string& path, PCMSource& output,
                  std::string& error, const std::atomic_bool* cancel) {
    AVFormatContext* format = nullptr;
    int result = avformat_open_input(&format, path.c_str(), nullptr, nullptr);
    if (result < 0) {
        error = "audio open failed: " + AvError(result);
        return false;
    }
    const auto closeFormat = [](AVFormatContext* value) {
        avformat_close_input(&value);
    };
    std::unique_ptr<AVFormatContext, decltype(closeFormat)> formatOwner(
        format, closeFormat);
    result = avformat_find_stream_info(format, nullptr);
    if (result < 0) {
        error = "audio stream headers failed: " + AvError(result);
        return false;
    }
    const int streamIndex =
        av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (streamIndex < 0) {
        error = "no audio stream";
        return false;
    }
    AVStream* stream = format->streams[streamIndex];
    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) {
        error = "no audio decoder";
        return false;
    }
    AVCodecContext* rawDecoder = avcodec_alloc_context3(codec);
    const auto freeDecoder = [](AVCodecContext* value) {
        avcodec_free_context(&value);
    };
    std::unique_ptr<AVCodecContext, decltype(freeDecoder)> decoder(rawDecoder,
                                                                   freeDecoder);
    if (!decoder ||
        avcodec_parameters_to_context(decoder.get(), stream->codecpar) < 0 ||
        avcodec_open2(decoder.get(), codec, nullptr) < 0) {
        error = "unable to initialize audio decoder";
        return false;
    }

    AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
    SwrContext* rawResampler = nullptr;
    result =
        swr_alloc_set_opts2(&rawResampler, &stereo, AV_SAMPLE_FMT_FLTP,
                            kMixRate, &decoder->ch_layout, decoder->sample_fmt,
                            decoder->sample_rate, 0, nullptr);
    av_channel_layout_uninit(&stereo);
    const auto freeResampler = [](SwrContext* value) { swr_free(&value); };
    std::unique_ptr<SwrContext, decltype(freeResampler)> resampler(
        rawResampler, freeResampler);
    if (result < 0 || !resampler || swr_init(resampler.get()) < 0) {
        error = "unable to initialize audio resampler";
        return false;
    }

    AVPacket* rawPacket = av_packet_alloc();
    AVFrame* rawFrame = av_frame_alloc();
    const auto freePacket = [](AVPacket* value) { av_packet_free(&value); };
    const auto freeFrame = [](AVFrame* value) { av_frame_free(&value); };
    std::unique_ptr<AVPacket, decltype(freePacket)> packet(rawPacket,
                                                           freePacket);
    std::unique_ptr<AVFrame, decltype(freeFrame)> frame(rawFrame, freeFrame);
    if (!packet || !frame) {
        error = "unable to allocate audio packet/frame";
        return false;
    }

    const auto convert = [&](const AVFrame* input) {
        const int inputSamples = input ? input->nb_samples : 0;
        const int capacity =
            std::max(1, swr_get_out_samples(resampler.get(), inputSamples));
        std::vector<float> left(static_cast<size_t>(capacity));
        std::vector<float> right(static_cast<size_t>(capacity));
        uint8_t* planes[2] = {
            reinterpret_cast<uint8_t*>(left.data()),
            reinterpret_cast<uint8_t*>(right.data()),
        };
        const uint8_t* const* inputData =
            input ? const_cast<const uint8_t* const*>(input->extended_data)
                  : nullptr;
        const int count =
            swr_convert(resampler.get(), planes, capacity,
                        const_cast<const uint8_t**>(inputData), inputSamples);
        if (count < 0) return false;
        output.left.insert(output.left.end(), left.begin(),
                           left.begin() + count);
        output.right.insert(output.right.end(), right.begin(),
                            right.begin() + count);
        return true;
    };
    const auto receive = [&]() {
        while (true) {
            const int status =
                avcodec_receive_frame(decoder.get(), frame.get());
            if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) return true;
            if (status < 0 || !convert(frame.get())) return false;
            av_frame_unref(frame.get());
        }
    };

    while (av_read_frame(format, packet.get()) >= 0) {
        if (cancel && cancel->load()) {
            av_packet_unref(packet.get());
            error = "audio decode cancelled";
            return false;
        }
        if (packet->stream_index == streamIndex) {
            result = avcodec_send_packet(decoder.get(), packet.get());
            if (result < 0 && result != AVERROR(EAGAIN)) {
                error = "audio packet decode failed: " + AvError(result);
                return false;
            }
            if (!receive()) {
                error = "audio frame decode/resample failed";
                return false;
            }
        }
        av_packet_unref(packet.get());
    }
    avcodec_send_packet(decoder.get(), nullptr);
    if (!receive()) {
        error = "audio decoder drain failed";
        return false;
    }
    while (swr_get_delay(resampler.get(), kMixRate) > 0) {
        const size_t before = output.left.size();
        if (!convert(nullptr) || output.left.size() == before) break;
    }
    return !output.left.empty();
}

struct MixClip {
    std::shared_ptr<const PCMSource> source;
    int64_t timelineStart = 0;
    int64_t sourceStart = 0;
    int64_t length = 0;
    // The conversion to an amplitude is deliberately deferred to this
    // realtime boundary. The document retains exact dB and exact durations;
    // libm merely turns them into samples for AVAudioEngine.
    float gain = 1.0f;
    int64_t fadeInSamples = 0;
    int64_t fadeOutSamples = 0;
};

struct MixPlan {
    std::vector<MixClip> clips;
};

// PERF-2026-09. A MixClip minus the one thing that has to wait: the decoded
// samples. RebuildTimeline resolves everything a clip contributes to the mix
// from the Document alone and keeps it here, so the plan the realtime thread
// reads can be rebuilt from the sources decoded so far -- as many times as
// decoding finishes a new one -- without the Document being consulted again,
// let alone retained.
struct PlannedClip {
    Ulid source_id;
    int64_t timelineStart = 0;
    int64_t sourceStart = 0;
    int64_t length = 0;
    float gain = 1.0f;
    int64_t fadeInSamples = 0;
    int64_t fadeOutSamples = 0;
};

size_t PcmBytes(const PCMSource& source) {
    return (source.left.size() + source.right.size()) * sizeof(float);
}

// Retention on top of what is audible, not a cap on it: a source the active
// timeline plays is never dropped, exactly as before. This budget only says
// how much *already decoded* audio is worth keeping around once it stops
// being audible, so that switching back to the timeline it belongs to is
// silent-free and costs no disk. Roughly 45 minutes at 48 kHz stereo float.
constexpr size_t kRetainedAudioBudgetBytes = 1024u * 1024u * 1024u;

}  // namespace

struct AudioPlayback::Impl {
    AVAudioEngine* engine = nil;
    AVAudioSourceNode* sourceNode = nil;
    std::filesystem::path baseDirectory;
    std::shared_ptr<const MixPlan> plan = std::make_shared<MixPlan>();
    std::atomic<int64_t> cursor{0};
    std::atomic<int> direction{0};
    std::atomic<int32_t> scrubRemaining{0};
    std::atomic<uint64_t> generation{0};
    std::atomic<int64_t> lastScrubSample{std::numeric_limits<int64_t>::min()};
    std::atomic<uint64_t> scrubTriggerCount{0};

    // PERF-2026-09. Everything below belongs to the decode thread and the
    // editor thread jointly; the realtime render block touches none of it and
    // reads `plan` alone, so it never waits on this mutex.
    mutable std::mutex mutex;
    std::condition_variable wakeup;
    std::condition_variable idle;
    std::thread decoder;
    // Read by the decode thread inside DecodeSource, so it cannot live under
    // `mutex`: the point is to reach a decode that is already running.
    std::atomic_bool cancel{false};
    bool stopping = false;
    bool decoding = false;
    std::map<Ulid, std::shared_ptr<const PCMSource>> sources;
    std::vector<PlannedClip> planned;
    std::set<Ulid> audible;
    // A source that cannot be decoded is remembered as such: retrying it on
    // every rebuild would re-open an offline rush over and over.
    std::set<Ulid> failed;
    std::deque<std::pair<Ulid, std::filesystem::path>> queue;
    std::map<Ulid, uint64_t> lastUsed;
    uint64_t useTick = 0;

    // Both callers already hold `mutex`.
    void PublishPlanLocked() {
        auto next = std::make_shared<MixPlan>();
        next->clips.reserve(planned.size());
        for (const PlannedClip& clip : planned) {
            const auto source = sources.find(clip.source_id);
            if (source == sources.end()) continue;
            MixClip mixed;
            mixed.source = source->second;
            mixed.timelineStart = clip.timelineStart;
            mixed.sourceStart = clip.sourceStart;
            mixed.length = clip.length;
            mixed.gain = clip.gain;
            mixed.fadeInSamples = clip.fadeInSamples;
            mixed.fadeOutSamples = clip.fadeOutSamples;
            next->clips.push_back(std::move(mixed));
        }
        std::atomic_store(&plan, std::static_pointer_cast<const MixPlan>(next));
    }

    // Evicting a source the plan still names is harmless -- every MixClip
    // holds its own shared_ptr, so the realtime thread keeps reading valid
    // samples until the plan is replaced -- but it would be re-decoded on the
    // next rebuild, so only sources the active timeline does not play are
    // considered.
    void EvictLocked() {
        size_t total = 0;
        for (const auto& source : sources) total += PcmBytes(*source.second);
        while (total > kRetainedAudioBudgetBytes) {
            auto oldest = sources.end();
            uint64_t oldestTick = std::numeric_limits<uint64_t>::max();
            for (auto item = sources.begin(); item != sources.end(); ++item) {
                if (audible.count(item->first) != 0) continue;
                const auto used = lastUsed.find(item->first);
                const uint64_t tick = used == lastUsed.end() ? 0 : used->second;
                if (tick < oldestTick) {
                    oldestTick = tick;
                    oldest = item;
                }
            }
            if (oldest == sources.end()) break;
            total -= PcmBytes(*oldest->second);
            lastUsed.erase(oldest->first);
            sources.erase(oldest);
        }
    }
};

AudioPlayback::AudioPlayback() : impl_(new Impl()) {}

AudioPlayback::~AudioPlayback() {
    Stop();
    impl_->cancel.store(true);
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->stopping = true;
    }
    impl_->wakeup.notify_all();
    if (impl_->decoder.joinable()) impl_->decoder.join();
    if (impl_->engine && impl_->sourceNode)
        [impl_->engine detachNode:impl_->sourceNode];
    delete impl_;
}

bool AudioPlayback::Open(const Document& document,
                         const std::string& baseDirectory, std::string& error) {
    // Open re-initialises. Anything the previous document left decoding has
    // to be abandoned before the cache is cleared, or its result would land
    // in the new one after the fact.
    impl_->cancel.store(true);
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->queue.clear();
    }
    WaitForDecodes(2000);
    impl_->cancel.store(false);
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->sources.clear();
        impl_->planned.clear();
        impl_->audible.clear();
        impl_->failed.clear();
        impl_->queue.clear();
        impl_->lastUsed.clear();
    }
    impl_->baseDirectory = baseDirectory;
    EnsureDecoderStarted();
    RebuildTimeline(document);

    impl_->engine = [[AVAudioEngine alloc] init];
    AVAudioFormat* format =
        [[AVAudioFormat alloc] initWithCommonFormat:AVAudioPCMFormatFloat32
                                         sampleRate:kMixRate
                                           channels:2
                                        interleaved:NO];
    Impl* state = impl_;
    impl_->sourceNode = [[AVAudioSourceNode alloc]
        initWithFormat:format
           renderBlock:^OSStatus(
               BOOL* isSilence, const AudioTimeStamp* timestamp,
               AVAudioFrameCount frameCount, AudioBufferList* outputData) {
             (void)timestamp;
             const auto plan = std::atomic_load(&state->plan);
             const uint64_t generation = state->generation.load();
             const int direction = state->direction.load();
             const int64_t cursor = state->cursor.load();
             const int32_t scrubRemaining = state->scrubRemaining.load();
             const bool scrubbing = scrubRemaining > 0;
             float* left = static_cast<float*>(outputData->mBuffers[0].mData);
             float* right = static_cast<float*>(outputData->mBuffers[1].mData);
             std::fill(left, left + frameCount, 0.0f);
             std::fill(right, right + frameCount, 0.0f);
             bool audible = false;
             if (direction != 0) {
                 for (AVAudioFrameCount index = 0; index < frameCount;
                      ++index) {
                     if (scrubbing && index >= static_cast<AVAudioFrameCount>(
                                                   scrubRemaining))
                         break;
                     const int64_t timelineSample =
                         cursor + static_cast<int64_t>(index) * direction;
                     for (const MixClip& clip : plan->clips) {
                         const int64_t offset =
                             timelineSample - clip.timelineStart;
                         if (offset < 0 || offset >= clip.length) continue;
                         const int64_t sourceSample = clip.sourceStart + offset;
                         if (sourceSample < 0 ||
                             sourceSample >=
                                 static_cast<int64_t>(clip.source->left.size()))
                             continue;
                         float envelope = clip.gain;
                         if (clip.fadeInSamples > 0)
                             envelope *= std::min(
                                 1.0f,
                                 static_cast<float>(offset) /
                                     static_cast<float>(clip.fadeInSamples));
                         if (clip.fadeOutSamples > 0) {
                             const int64_t remaining = clip.length - offset;
                             envelope *= std::min(
                                 1.0f,
                                 static_cast<float>(remaining) /
                                     static_cast<float>(clip.fadeOutSamples));
                         }
                         left[index] +=
                             clip.source->left[sourceSample] * envelope;
                         right[index] +=
                             clip.source->right[sourceSample] * envelope;
                         audible = true;
                     }
                     left[index] = std::clamp(left[index], -1.0f, 1.0f);
                     right[index] = std::clamp(right[index], -1.0f, 1.0f);
                     if (scrubbing) {
                         const int32_t elapsed =
                             kScrubSamples - scrubRemaining + index;
                         const int32_t remaining = scrubRemaining - index;
                         const float attack = std::clamp(
                             static_cast<float>(elapsed) / kScrubFadeSamples,
                             0.0f, 1.0f);
                         const float release = std::clamp(
                             static_cast<float>(remaining) / kScrubFadeSamples,
                             0.0f, 1.0f);
                         const float gain = std::min(attack, release);
                         left[index] *= gain;
                         right[index] *= gain;
                     }
                 }
                 if (state->generation.load() == generation) {
                     const int32_t consumed =
                         scrubbing
                             ? std::min<int32_t>(scrubRemaining, frameCount)
                             : static_cast<int32_t>(frameCount);
                     state->cursor.store(
                         cursor + static_cast<int64_t>(consumed) * direction);
                     if (scrubbing) {
                         const int32_t next = scrubRemaining - consumed;
                         state->scrubRemaining.store(next);
                         if (next == 0) state->direction.store(0);
                     }
                 }
             }
             *isSilence = !audible;
             return noErr;
           }];
    [impl_->engine attachNode:impl_->sourceNode];
    [impl_->engine connect:impl_->sourceNode
                        to:impl_->engine.mainMixerNode
                    format:format];
    error.clear();
    return true;
}

// PERF-2026-09. This used to decode, on the calling thread, the whole audio
// of every source the timeline plays -- 48 kHz stereo float, so a 30-minute
// interview rush is about 690 MB and several seconds of work. It runs after
// every edit, every timeline switch and once per project open, all on the
// thread AppKit draws from, which is precisely where the editor was seen to
// freeze when a project or a timeline was opened.
//
// The plan the realtime thread reads has always skipped a clip whose source
// is not decoded, so a partially decoded mix was already a state this design
// tolerated -- it just never occurred, because decoding blocked until it
// could not. Making the decode a background job turns that tolerated state
// into the normal one: the mix comes up as sources land, a moment after the
// picture, instead of the whole editor waiting for all of them.
void AudioPlayback::RebuildTimeline(const Document& document) {
    std::set<Ulid> audible;
    for (const DocumentTrack& track : document.sequence.tracks) {
        if (track.kind != "audio") continue;
        for (const DocumentClip& clip : track.clips)
            audible.insert(clip.source_id);
    }

    const bool hasSolo = std::any_of(
        document.sequence.tracks.begin(), document.sequence.tracks.end(),
        [](const DocumentTrack& track) {
            return track.kind == "audio" && track.solo;
        });
    std::vector<PlannedClip> planned;
    for (const DocumentTrack& track : document.sequence.tracks) {
        if (track.kind != "audio" || track.muted || (hasSolo && !track.solo))
            continue;
        for (const DocumentClip& clip : track.clips) {
            PlannedClip mixed;
            mixed.source_id = clip.source_id;
            mixed.timelineStart = clip.timeline_in.to_frames(kMixRate);
            mixed.sourceStart = clip.source_in.to_frames(kMixRate);
            mixed.length = clip.duration.to_frames(kMixRate);
            const float gainDb = static_cast<float>(clip.audio_gain_db.num) /
                                 static_cast<float>(clip.audio_gain_db.den);
            mixed.gain = std::pow(10.0f, gainDb / 20.0f);
            mixed.fadeInSamples = clip.audio_fade_in.to_frames(kMixRate);
            mixed.fadeOutSamples = clip.audio_fade_out.to_frames(kMixRate);
            if (mixed.length > 0) planned.push_back(std::move(mixed));
        }
    }

    // Resolved before taking the lock: a relative source path is answered
    // from the project directory, which the decode thread has no business
    // knowing about.
    std::map<Ulid, std::filesystem::path> paths;
    for (const DocumentSource& source : document.sources) {
        if (audible.count(source.id) == 0) continue;
        std::filesystem::path path(source.path);
        if (path.is_relative()) path = impl_->baseDirectory / path;
        paths.emplace(source.id, path.lexically_normal());
    }

    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->planned = std::move(planned);
        impl_->audible = audible;
        for (const Ulid& id : impl_->audible)
            impl_->lastUsed[id] = ++impl_->useTick;
        for (const auto& item : paths) {
            if (impl_->sources.count(item.first) != 0 ||
                impl_->failed.count(item.first) != 0)
                continue;
            const bool queued = std::any_of(
                impl_->queue.begin(), impl_->queue.end(),
                [&](const auto& entry) { return entry.first == item.first; });
            if (!queued) impl_->queue.push_back(item);
        }
        impl_->EvictLocked();
        impl_->PublishPlanLocked();
    }
    EnsureDecoderStarted();
    impl_->wakeup.notify_one();
}

// Both entry points are called from the editor thread, which is the only
// thread allowed to touch `decoder` itself.
void AudioPlayback::EnsureDecoderStarted() {
    if (!impl_->decoder.joinable())
        impl_->decoder = std::thread(&AudioPlayback::DecodeLoop, this);
}

void AudioPlayback::DecodeLoop() {
    while (true) {
        Ulid sourceId;
        std::filesystem::path path;
        {
            std::unique_lock<std::mutex> lock(impl_->mutex);
            impl_->wakeup.wait(lock, [this] {
                return impl_->stopping || !impl_->queue.empty();
            });
            if (impl_->stopping) return;
            sourceId = impl_->queue.front().first;
            path = impl_->queue.front().second;
            impl_->queue.pop_front();
            impl_->decoding = true;
        }

        auto decoded = std::make_shared<PCMSource>();
        std::string decodeError;
        const bool ok =
            DecodeSource(path.string(), *decoded, decodeError, &impl_->cancel);
        const bool cancelled = impl_->cancel.load();
        if (!ok && !cancelled && decodeError != "no audio stream")
            std::fprintf(stderr, "Audio disabled for %s: %s\n",
                         path.string().c_str(), decodeError.c_str());

        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            impl_->decoding = false;
            if (ok) {
                impl_->sources[sourceId] = std::move(decoded);
                impl_->lastUsed[sourceId] = ++impl_->useTick;
                impl_->EvictLocked();
                impl_->PublishPlanLocked();
            } else if (!cancelled) {
                // A rush abandoned mid-decode is not a rush that cannot be
                // decoded: remembering it as failed would keep it silent for
                // the rest of the session.
                impl_->failed.insert(sourceId);
            }
            if (impl_->queue.empty()) impl_->idle.notify_all();
            if (impl_->stopping) return;
        }
    }
}

bool AudioPlayback::WaitForDecodes(int timeoutMilliseconds) {
    std::unique_lock<std::mutex> lock(impl_->mutex);
    return impl_->idle.wait_for(
        lock, std::chrono::milliseconds(std::max(0, timeoutMilliseconds)),
        [this] { return impl_->queue.empty() && !impl_->decoding; });
}

bool AudioPlayback::PlayFrom(RationalTime position, int direction,
                             std::string& error) {
    if (!impl_->engine || !impl_->sourceNode || direction == 0) {
        error = "audio engine is not initialized";
        return false;
    }
    impl_->generation.fetch_add(1);
    impl_->cursor.store(position.to_frames(kMixRate));
    impl_->scrubRemaining.store(0);
    impl_->lastScrubSample.store(std::numeric_limits<int64_t>::min());
    impl_->direction.store(std::clamp(direction, -4, 4));
    NSError* startError = nil;
    if (!impl_->engine.isRunning &&
        ![impl_->engine startAndReturnError:&startError]) {
        error =
            startError.localizedDescription.UTF8String ?: "audio start failed";
        impl_->direction.store(0);
        return false;
    }
    error.clear();
    return true;
}

bool AudioPlayback::ScrubAt(RationalTime position, std::string& error) {
    if (!impl_->engine || !impl_->sourceNode) {
        error = "audio engine is not initialized";
        return false;
    }
    const int64_t sample = position.to_frames(kMixRate);
    if (impl_->lastScrubSample.exchange(sample) == sample) {
        error.clear();
        return true;
    }
    impl_->scrubTriggerCount.fetch_add(1);
    impl_->generation.fetch_add(1);
    impl_->cursor.store(sample);
    impl_->scrubRemaining.store(kScrubSamples);
    impl_->direction.store(1);
    NSError* startError = nil;
    if (!impl_->engine.isRunning &&
        ![impl_->engine startAndReturnError:&startError]) {
        error =
            startError.localizedDescription.UTF8String ?: "audio scrub failed";
        impl_->direction.store(0);
        impl_->scrubRemaining.store(0);
        impl_->lastScrubSample.store(std::numeric_limits<int64_t>::min());
        return false;
    }
    error.clear();
    return true;
}

void AudioPlayback::Stop() {
    if (!impl_) return;
    impl_->generation.fetch_add(1);
    impl_->direction.store(0);
    impl_->scrubRemaining.store(0);
    impl_->lastScrubSample.store(std::numeric_limits<int64_t>::min());
    if (impl_->engine && impl_->engine.isRunning) [impl_->engine pause];
}

size_t AudioPlayback::DecodedSourceCount() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->sources.size();
}

size_t AudioPlayback::PlannedClipCount() const {
    return std::atomic_load(&impl_->plan)->clips.size();
}

uint64_t AudioPlayback::ScrubTriggerCount() const {
    return impl_->scrubTriggerCount.load();
}

int AudioPlayback::ShuttleSpeed() const { return impl_->direction.load(); }
