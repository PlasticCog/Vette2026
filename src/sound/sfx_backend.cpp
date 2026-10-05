#include "sound/sfx_backend.h"

#include <cstddef>

namespace vette::sound {

void AdlibBackend::render(float* out, int frames) {
    buf_.resize(static_cast<size_t>(frames));
    adlib_.render(buf_.data(), frames);
    for (int i = 0; i < frames; ++i)
        out[i] += buf_[static_cast<size_t>(i)];
}

}  // namespace vette::sound
