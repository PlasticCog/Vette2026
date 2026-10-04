#pragma once

#include "platform/sdl_util.h"

#include <cstdint>
#include <vector>

namespace vette {

// Mono 16-bit output on the default playback device. The emulator produces samples in step with
// wall-clock time, so the queue is kept small: primed with a little silence when it runs dry, and
// cleared if it ever falls far behind. Requires SDL_INIT_AUDIO; throws std::runtime_error on failure.
class AudioOut {
public:
    explicit AudioOut(int sample_rate);

    void push(const std::vector<std::int16_t>& samples);

private:
    SdlPtr<SDL_AudioStream> stream_;
    int min_queued_bytes_ = 0;  // below this, prime with silence (avoids crackle from underruns)
    int max_queued_bytes_ = 0;  // above this, drop the backlog (keeps latency bounded)
};

}  // namespace vette
