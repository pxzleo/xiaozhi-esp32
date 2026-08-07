#include "netease_music_lyrics.h"

#include <algorithm>
#include <utility>

namespace netease_music {

uint32_t LyricsTimeline::Start(LyricsPlayback playback) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (++generation_ == 0) {
        ++generation_;
    }
    playback_ = std::move(playback);
    rendered_samples_ = 0;
    submitted_samples_ = 0;
    sample_rate_ = 0;
    active_ = true;
    rendered_index_ = WindowAtLocked(0).current_index;
    return generation_;
}

void LyricsTimeline::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (++generation_ == 0) {
        ++generation_;
    }
    playback_ = {};
    rendered_samples_ = 0;
    submitted_samples_ = 0;
    sample_rate_ = 0;
    rendered_index_ = -2;
    active_ = false;
}

uint32_t LyricsTimeline::Generation() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_ ? generation_ : 0;
}

bool LyricsTimeline::IsActive() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_;
}

LyricsPlayback LyricsTimeline::Playback() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return playback_;
}

LyricsWindow LyricsTimeline::CurrentWindow() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return WindowAtLocked(sample_rate_ == 0 ? 0 : rendered_samples_ * 1000 / sample_rate_);
}

std::optional<LyricsWindow> LyricsTimeline::OnPcmRendered(uint32_t generation, size_t samples,
                                                          uint32_t sample_rate,
                                                          size_t buffered_samples) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!active_ || generation == 0 || generation != generation_ || sample_rate == 0) {
        return std::nullopt;
    }
    if (sample_rate_ == 0) {
        sample_rate_ = sample_rate;
        rendered_samples_ = 0;
        submitted_samples_ = 0;
    } else if (sample_rate_ != sample_rate) {
        // Output sample rate should be stable. Preserve elapsed time if the driver changes it.
        rendered_samples_ = rendered_samples_ * sample_rate / sample_rate_;
        submitted_samples_ = submitted_samples_ * sample_rate / sample_rate_;
        sample_rate_ = sample_rate;
    }
    submitted_samples_ += samples;
    rendered_samples_ = submitted_samples_ > buffered_samples
                            ? submitted_samples_ - buffered_samples
                            : 0;
    auto window = WindowAtLocked(rendered_samples_ * 1000 / sample_rate_);
    if (window.current_index == rendered_index_) {
        return std::nullopt;
    }
    rendered_index_ = window.current_index;
    return window;
}

LyricsWindow LyricsTimeline::WindowAtLocked(uint64_t position_ms) const {
    LyricsWindow window;
    if (!playback_.available) {
        window.current = "暂无歌词";
        return window;
    }
    const auto& lines = playback_.lines;
    const auto it = std::upper_bound(lines.begin(), lines.end(), position_ms,
                                     [](uint64_t position, const LyricLine& line) {
                                         return position < line.start_ms;
                                     });
    if (it == lines.begin()) {
        if (!lines.empty()) {
            window.next = lines.front().text;
        }
        return window;
    }
    const size_t index = static_cast<size_t>(std::distance(lines.begin(), it) - 1);
    window.current_index = static_cast<int>(index);
    window.current = lines[index].text;
    if (index > 0) {
        window.previous = lines[index - 1].text;
    }
    if (index + 1 < lines.size()) {
        window.next = lines[index + 1].text;
    }
    return window;
}

}  // namespace netease_music
