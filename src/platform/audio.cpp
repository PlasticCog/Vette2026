#include "platform/audio.h"

namespace vette {

AudioOut::AudioOut(int sample_rate) {
    const SDL_AudioSpec spec{SDL_AUDIO_S16, 1, sample_rate};
    stream_.reset(SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr));
    if (!stream_)
        throw_sdl_error("SDL_OpenAudioDeviceStream");
    const int bytes_per_ms = sample_rate * 2 / 1000;
    min_queued_bytes_ = 10 * bytes_per_ms;
    max_queued_bytes_ = 150 * bytes_per_ms;
    SDL_ResumeAudioStreamDevice(stream_.get());
}

void AudioOut::push(const std::vector<std::int16_t>& samples) {
    if (samples.empty())
        return;
    SDL_AudioStream* stream = stream_.get();
    const int queued = SDL_GetAudioStreamQueued(stream);
    if (queued > max_queued_bytes_) {
        SDL_ClearAudioStream(stream);
    } else if (queued < min_queued_bytes_) {
        const std::vector<std::int16_t> silence(static_cast<std::size_t>(min_queued_bytes_ * 4 / 2), 0);
        SDL_PutAudioStreamData(stream, silence.data(), static_cast<int>(silence.size() * 2));
    }
    SDL_PutAudioStreamData(stream, samples.data(), static_cast<int>(samples.size() * 2));
}

}  // namespace vette
