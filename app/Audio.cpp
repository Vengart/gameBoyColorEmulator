#include "Audio.h"
#include <vector>

namespace {
constexpr double kTarget = 0.050;   // желаемая задержка звука, с
constexpr double kMaxQueue = 0.200; // больше этого не копим: лишнее отбрасывается
constexpr double kMaxAdjust = 0.005;
}  // namespace

bool AudioOut::open() {
    if (dev_) return true;
    if (SDL_WasInit(SDL_INIT_AUDIO) == 0 && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) return false;
    SDL_AudioSpec want{}, have{};
    want.freq = 48000;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = nullptr;  // режим очереди (SDL_QueueAudio)
    dev_ = SDL_OpenAudioDevice(nullptr, 0, &want, &have, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (!dev_) return false;
    freq_ = have.freq > 0 ? have.freq : 48000;
    return true;
}

void AudioOut::close() {
    if (dev_) SDL_CloseAudioDevice(dev_);
    dev_ = 0;
}

double AudioOut::queuedSeconds() const {
    if (!dev_) return 0.0;
    return static_cast<double>(SDL_GetQueuedAudioSize(dev_)) / (4.0 * freq_);  // 4 байта на стереопару
}

void AudioOut::push(const int16_t* stereo, size_t count) {
    if (!dev_ || count == 0) return;
    if (queuedSeconds() > kMaxQueue) return;
    SDL_QueueAudio(dev_, stereo, static_cast<Uint32>(count * sizeof(int16_t)));
}

void AudioOut::start() {
    if (!dev_) return;
    SDL_ClearQueuedAudio(dev_);
    std::vector<int16_t> silence(static_cast<size_t>(kTarget * freq_) * 2, 0);
    SDL_QueueAudio(dev_, silence.data(), static_cast<Uint32>(silence.size() * sizeof(int16_t)));
    SDL_PauseAudioDevice(dev_, 0);
}

void AudioOut::stop() {
    if (!dev_) return;
    SDL_PauseAudioDevice(dev_, 1);
    SDL_ClearQueuedAudio(dev_);
}

double AudioOut::suggestedRate() const {
    // Очередь длиннее нужной -> генерируем чуть меньше отсчётов, короче -> чуть больше
    double err = (queuedSeconds() - kTarget) / kTarget;
    if (err > 1.0) err = 1.0;
    if (err < -1.0) err = -1.0;
    return freq_ * (1.0 - err * kMaxAdjust);
}
