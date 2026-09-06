#pragma once

// IA1 -- ROADMAP.md. The agent's transcription must observe the exported mix:
// sharing the clip envelopes and final limiter prevents the two FFmpeg paths
// from silently disagreeing after an audio edit. Times stay rational until
// the existing decimal serialization boundary required by FFmpeg options.

#include "Document.h"

#include <cstddef>
#include <iomanip>
#include <locale>
#include <ostream>
#include <sstream>
#include <string>

namespace audio_mix {

inline constexpr int kSampleRate = 48000;

inline std::string Seconds(const RationalTime& time) {
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::fixed << std::setprecision(12)
           << static_cast<long double>(time.value) / time.rate;
    std::string value = output.str();
    while (value.size() > 2 && value.back() == '0') value.pop_back();
    if (!value.empty() && value.back() == '.') value.push_back('0');
    return value;
}

inline void AppendClipEnvelope(std::ostream& graph, const DocumentClip& clip) {
    if (clip.audio_gain_db.num != 0)
        graph << ",volume=pow(10\\,(" << clip.audio_gain_db.num << '/'
              << clip.audio_gain_db.den << ")/20)";
    if (clip.audio_fade_in.value != 0)
        graph << ",afade=t=in:st=0:d=" << Seconds(clip.audio_fade_in);
    if (clip.audio_fade_out.value != 0)
        graph << ",afade=t=out:st="
              << Seconds(clip.duration.sub(clip.audio_fade_out))
              << ":d=" << Seconds(clip.audio_fade_out);
}

inline void AppendMixedOutput(std::ostream& graph, size_t inputs,
                              const RationalTime& duration) {
    graph << "amix=inputs=" << inputs
          << ":duration=longest:normalize=0,"
             "alimiter=limit=0.668344:level=0:latency=1,"
             "apad,atrim=duration="
          << Seconds(duration) << "[audio]";
}

}  // namespace audio_mix
