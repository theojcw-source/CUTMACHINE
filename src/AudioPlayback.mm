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
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
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

bool DecodeSource(const std::string& path, PCMSource& output,
                  std::string& error) {
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
};

struct MixPlan {
    std::vector<MixClip> clips;
};

}  // namespace

struct AudioPlayback::Impl {
    AVAudioEngine* engine = nil;
    AVAudioSourceNode* sourceNode = nil;
    std::map<Ulid, std::shared_ptr<const PCMSource>> sources;
    std::shared_ptr<const MixPlan> plan = std::make_shared<MixPlan>();
    std::atomic<int64_t> cursor{0};
    std::atomic<int> direction{0};
    std::atomic<int32_t> scrubRemaining{0};
    std::atomic<uint64_t> generation{0};
    std::atomic<int64_t> lastScrubSample{std::numeric_limits<int64_t>::min()};
    std::atomic<uint64_t> scrubTriggerCount{0};
};

AudioPlayback::AudioPlayback() : impl_(new Impl()) {}

AudioPlayback::~AudioPlayback() {
    Stop();
    if (impl_->engine && impl_->sourceNode)
        [impl_->engine detachNode:impl_->sourceNode];
    delete impl_;
}

bool AudioPlayback::Open(const Document& document,
                         const std::string& baseDirectory, std::string& error) {
    impl_->sources.clear();
    for (const DocumentSource& source : document.sources) {
        std::filesystem::path path(source.path);
        if (path.is_relative())
            path = std::filesystem::path(baseDirectory) / path;
        auto decoded = std::make_shared<PCMSource>();
        std::string decodeError;
        if (DecodeSource(path.lexically_normal().string(), *decoded,
                         decodeError)) {
            impl_->sources[source.id] = std::move(decoded);
        } else if (decodeError != "no audio stream") {
            std::fprintf(stderr, "Audio disabled for %s: %s\n",
                         path.string().c_str(), decodeError.c_str());
        }
    }
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
                         left[index] += clip.source->left[sourceSample];
                         right[index] += clip.source->right[sourceSample];
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

void AudioPlayback::RebuildTimeline(const Document& document) {
    auto plan = std::make_shared<MixPlan>();
    for (const DocumentTrack& track : document.sequence.tracks) {
        for (const DocumentClip& clip : track.clips) {
            if (track.kind != "audio") continue;
            const auto source = impl_->sources.find(clip.source_id);
            if (source == impl_->sources.end()) continue;
            MixClip mixed;
            mixed.source = source->second;
            mixed.timelineStart = clip.timeline_in.to_frames(kMixRate);
            mixed.sourceStart = clip.source_in.to_frames(kMixRate);
            mixed.length = clip.duration.to_frames(kMixRate);
            if (mixed.length > 0) plan->clips.push_back(std::move(mixed));
        }
    }
    std::atomic_store(&impl_->plan,
                      std::static_pointer_cast<const MixPlan>(plan));
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
    impl_->direction.store(direction < 0 ? -1 : 1);
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
    return impl_->sources.size();
}

size_t AudioPlayback::PlannedClipCount() const {
    return std::atomic_load(&impl_->plan)->clips.size();
}

uint64_t AudioPlayback::ScrubTriggerCount() const {
    return impl_->scrubTriggerCount.load();
}
