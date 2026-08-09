#include "MediaSource.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/pixdesc.h>
}

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>

namespace {

void LogAvError(const char* operation, int error) {
    char text[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(error, text, sizeof(text));
    std::fprintf(stderr, "%s failed: %s\n", operation, text);
}

}  // namespace

struct MediaSource::Impl {
    AVFormatContext* format = nullptr;
    AVCodecContext* decoder = nullptr;
    AVStream* stream = nullptr;
    int streamIndex = -1;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    AVRational frameRate = {0, 1};
    int64_t frameCount = 0;
    bool draining = false;
};

MediaSource::MediaSource() : impl_(new Impl()) {}

MediaSource::~MediaSource() {
    av_frame_free(&impl_->frame);
    av_packet_free(&impl_->packet);
    avcodec_free_context(&impl_->decoder);
    avformat_close_input(&impl_->format);
    delete impl_;
}

bool MediaSource::Open(const std::string& path, int threadCount, int threadType) {
    int result = avformat_open_input(&impl_->format, path.c_str(), nullptr, nullptr);
    if (result < 0) {
        LogAvError("avformat_open_input", result);
        return false;
    }

    result = avformat_find_stream_info(impl_->format, nullptr);
    if (result < 0) {
        LogAvError("avformat_find_stream_info", result);
        return false;
    }

    result = av_find_best_stream(impl_->format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (result < 0) {
        LogAvError("av_find_best_stream(video)", result);
        return false;
    }
    impl_->streamIndex = result;
    impl_->stream = impl_->format->streams[impl_->streamIndex];

    const AVCodec* codec = avcodec_find_decoder(impl_->stream->codecpar->codec_id);
    if (!codec) {
        std::fprintf(stderr, "No FFmpeg decoder for codec id %d\n",
                     impl_->stream->codecpar->codec_id);
        return false;
    }

    impl_->decoder = avcodec_alloc_context3(codec);
    if (!impl_->decoder) {
        std::fprintf(stderr, "avcodec_alloc_context3 failed\n");
        return false;
    }
    result = avcodec_parameters_to_context(impl_->decoder, impl_->stream->codecpar);
    if (result < 0) {
        LogAvError("avcodec_parameters_to_context", result);
        return false;
    }

    // Required for software decoding performance. These must be set before open2.
    impl_->decoder->thread_count = threadCount;
    impl_->decoder->thread_type = threadType < 0
        ? FF_THREAD_SLICE | FF_THREAD_FRAME
        : threadType;

    result = avcodec_open2(impl_->decoder, codec, nullptr);
    if (result < 0) {
        LogAvError("avcodec_open2", result);
        return false;
    }
    std::fprintf(stderr,
                 "Decoder threading: count=%d requested=0x%x active=0x%x\n",
                 impl_->decoder->thread_count, impl_->decoder->thread_type,
                 impl_->decoder->active_thread_type);

    impl_->packet = av_packet_alloc();
    impl_->frame = av_frame_alloc();
    if (!impl_->packet || !impl_->frame) {
        std::fprintf(stderr, "Unable to allocate FFmpeg packet/frame\n");
        return false;
    }

    impl_->frameRate = av_guess_frame_rate(impl_->format, impl_->stream, nullptr);
    if (impl_->frameRate.num <= 0 || impl_->frameRate.den <= 0) {
        std::fprintf(stderr, "Unable to determine video frame rate\n");
        return false;
    }

    if (impl_->stream->nb_frames > 0) {
        impl_->frameCount = impl_->stream->nb_frames;
    } else {
        double duration = 0.0;
        if (impl_->stream->duration != AV_NOPTS_VALUE) {
            duration = impl_->stream->duration * av_q2d(impl_->stream->time_base);
        } else if (impl_->format->duration != AV_NOPTS_VALUE) {
            duration = static_cast<double>(impl_->format->duration) / AV_TIME_BASE;
        }
        impl_->frameCount = std::max<int64_t>(1, std::llround(duration * FrameRate()));
    }

    std::fprintf(stderr, "Opened %dx%d, %.3f fps, %lld frames (software decode)\n",
                 Width(), Height(), FrameRate(), static_cast<long long>(impl_->frameCount));
    return true;
}

bool MediaSource::DecodeFrame(int64_t frameIndex, const AVFrame*& outFrame, int64_t& outPts) {
    outFrame = nullptr;
    outPts = AV_NOPTS_VALUE;
    if (!impl_->format || !impl_->decoder || !impl_->stream || !impl_->packet || !impl_->frame) {
        return false;
    }

    frameIndex = std::clamp<int64_t>(frameIndex, 0, std::max<int64_t>(0, impl_->frameCount - 1));
    int64_t targetPts = av_rescale_q(frameIndex, av_inv_q(impl_->frameRate),
                                     impl_->stream->time_base);
    if (impl_->stream->start_time != AV_NOPTS_VALUE) {
        targetPts += impl_->stream->start_time;
    }

    int result = av_seek_frame(impl_->format, impl_->streamIndex, targetPts,
                               AVSEEK_FLAG_BACKWARD);
    if (result < 0) {
        LogAvError("av_seek_frame", result);
        return false;
    }
    avcodec_flush_buffers(impl_->decoder);
    av_frame_unref(impl_->frame);
    impl_->draining = false;

    while (DecodeNextFrame(outFrame, outPts)) {
        if (outPts == AV_NOPTS_VALUE || outPts >= targetPts) {
            return true;
        }
    }
    return false;
}

bool MediaSource::DecodeNextFrame(const AVFrame*& outFrame, int64_t& outPts) {
    outFrame = nullptr;
    outPts = AV_NOPTS_VALUE;
    if (!impl_->format || !impl_->decoder || !impl_->packet || !impl_->frame) {
        return false;
    }

    av_frame_unref(impl_->frame);
    while (true) {
        int result = avcodec_receive_frame(impl_->decoder, impl_->frame);
        if (result >= 0) {
            if (impl_->frame->format != AV_PIX_FMT_YUV422P10LE) {
                const char* actual = av_get_pix_fmt_name(
                    static_cast<AVPixelFormat>(impl_->frame->format));
                std::fprintf(stderr,
                             "FATAL: expected yuv422p10le, decoder returned %s (%d)\n",
                             actual ? actual : "unknown", impl_->frame->format);
                assert(impl_->frame->format == AV_PIX_FMT_YUV422P10LE);
                return false;
            }
            outPts = impl_->frame->best_effort_timestamp;
            if (outPts == AV_NOPTS_VALUE) {
                outPts = impl_->frame->pts;
            }
            outFrame = impl_->frame;
            return true;
        }
        if (result == AVERROR_EOF) {
            return false;
        }
        if (result != AVERROR(EAGAIN)) {
            LogAvError("avcodec_receive_frame", result);
            return false;
        }

        if (impl_->draining) {
            return false;
        }

        while (true) {
            result = av_read_frame(impl_->format, impl_->packet);
            if (result < 0) {
                impl_->draining = true;
                result = avcodec_send_packet(impl_->decoder, nullptr);
                if (result < 0 && result != AVERROR_EOF) {
                    LogAvError("avcodec_send_packet(drain)", result);
                    return false;
                }
                break;
            }
            if (impl_->packet->stream_index != impl_->streamIndex) {
                av_packet_unref(impl_->packet);
                continue;
            }
            result = avcodec_send_packet(impl_->decoder, impl_->packet);
            av_packet_unref(impl_->packet);
            if (result < 0) {
                LogAvError("avcodec_send_packet", result);
                return false;
            }
            break;
        }
    }
}

int MediaSource::Width() const { return impl_->decoder ? impl_->decoder->width : 0; }
int MediaSource::Height() const { return impl_->decoder ? impl_->decoder->height : 0; }
int64_t MediaSource::FrameCount() const { return impl_->frameCount; }
double MediaSource::FrameRate() const { return av_q2d(impl_->frameRate); }
int64_t MediaSource::FrameIndexForPts(int64_t pts) const {
    if (!impl_->stream || pts == AV_NOPTS_VALUE) {
        return -1;
    }
    if (impl_->stream->start_time != AV_NOPTS_VALUE) {
        pts -= impl_->stream->start_time;
    }
    return av_rescale_q_rnd(pts, impl_->stream->time_base, av_inv_q(impl_->frameRate),
                            static_cast<AVRounding>(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
}
