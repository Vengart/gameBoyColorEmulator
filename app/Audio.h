#pragma once
#include <SDL.h>
#include <cstddef>
#include <cstdint>

// Вывод звука через очередь SDL. Эмулятор каждый кадр отдаёт готовые отсчёты,
// а suggestedRate() слегка подстраивает частоту генерации, чтобы очередь не росла и не пустела
// (часы звуковой карты и таймер кадров никогда не совпадают идеально).
class AudioOut {
public:
    bool open();
    void close();
    bool ok() const { return dev_ != 0; }
    int freq() const { return freq_; }

    void push(const int16_t* stereo, size_t count);  // count = число значений int16 (L и R вместе)
    void start();   // очищает очередь, подкладывает немного тишины и включает воспроизведение
    void stop();    // пауза и очистка очереди

    double suggestedRate() const;  // частота, с которой эмулятору стоит генерировать отсчёты

private:
    double queuedSeconds() const;
    SDL_AudioDeviceID dev_ = 0;
    int freq_ = 48000;
};
