#include "netease_music_lyrics.h"

#include <cassert>
#include <string>

using namespace netease_music;

LyricsPlayback MakePlayback() {
    return {.playback_id = "0123456789abcdef0123456789abcdef",
            .track_id = "33894312",
            .title = "Track",
            .artists = "Artist",
            .available = true,
            .lines = {{100, "first"}, {200, "same-a"}, {200, "same-b"}, {400, "last"}}};
}

int main() {
    LyricsTimeline timeline;
    const auto generation = timeline.Start(MakePlayback());
    auto initial = timeline.CurrentWindow();
    assert(initial.previous.empty());
    assert(initial.current.empty());
    assert(initial.next == "first");

    // Unrelated queued speech and obsolete playback generations never advance lyrics.
    assert(!timeline.OnPcmRendered(0, 4800, 24000).has_value());
    assert(!timeline.OnPcmRendered(generation + 1, 4800, 24000).has_value());

    auto at_100 = timeline.OnPcmRendered(generation, 2400, 24000);
    assert(at_100.has_value());
    assert(at_100->current == "first");
    assert(at_100->next == "same-a");

    // Equal timestamps select the final line while preserving server order around it.
    auto at_200 = timeline.OnPcmRendered(generation, 2400, 24000);
    assert(at_200.has_value());
    assert(at_200->previous == "same-a");
    assert(at_200->current == "same-b");
    assert(at_200->next == "last");
    assert(!timeline.OnPcmRendered(generation, 1200, 24000).has_value());

    LyricsTimeline dma_timeline;
    const auto dma_generation = dma_timeline.Start(MakePlayback());
    // A full 60 ms DMA queue is not yet audible and must not advance to the first line.
    assert(!dma_timeline.OnPcmRendered(dma_generation, 2400, 24000, 1440).has_value());
    auto dma_at_100 = dma_timeline.OnPcmRendered(dma_generation, 1440, 24000, 1440);
    assert(dma_at_100.has_value());
    assert(dma_at_100->current == "first");

    const auto replacement = timeline.Start(MakePlayback());
    assert(replacement != generation);
    assert(!timeline.OnPcmRendered(generation, 9600, 24000).has_value());
    assert(timeline.CurrentWindow().next == "first");

    timeline.Clear();
    assert(!timeline.IsActive());
    assert(!timeline.OnPcmRendered(replacement, 2400, 24000).has_value());

    auto unavailable = MakePlayback();
    unavailable.available = false;
    unavailable.lines.clear();
    const auto no_lyrics_generation = timeline.Start(std::move(unavailable));
    assert(timeline.CurrentWindow().current == "暂无歌词");
    assert(!timeline.OnPcmRendered(no_lyrics_generation, 2400, 24000).has_value());
    return 0;
}
