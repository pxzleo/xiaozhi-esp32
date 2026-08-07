#ifndef NETEASE_MUSIC_LYRICS_H
#define NETEASE_MUSIC_LYRICS_H

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace netease_music {

struct LyricLine {
    uint32_t start_ms = 0;
    std::string text;
};

struct LyricsPlayback {
    std::string playback_id;
    std::string track_id;
    std::string title;
    std::string artists;
    bool available = false;
    std::vector<LyricLine> lines;
};

struct LyricsWindow {
    std::string previous;
    std::string current;
    std::string next;
    int current_index = -1;
};

// Owns only the in-RAM timeline and the sample-driven playback position. Audio packets are
// tagged with Generation() when accepted so queued speech and obsolete tracks cannot advance it.
class LyricsTimeline {
public:
    uint32_t Start(LyricsPlayback playback);
    void Clear();
    uint32_t Generation() const;
    bool IsActive() const;
    LyricsPlayback Playback() const;
    LyricsWindow CurrentWindow() const;
    std::optional<LyricsWindow> OnPcmRendered(uint32_t generation, size_t samples,
                                               uint32_t sample_rate,
                                               size_t buffered_samples = 0);

private:
    LyricsWindow WindowAtLocked(uint64_t position_ms) const;

    mutable std::mutex mutex_;
    LyricsPlayback playback_;
    uint32_t generation_ = 0;
    uint64_t rendered_samples_ = 0;
    uint64_t submitted_samples_ = 0;
    uint32_t sample_rate_ = 0;
    int rendered_index_ = -2;
    bool active_ = false;
};

}  // namespace netease_music

#endif  // NETEASE_MUSIC_LYRICS_H
