// The spectrogram's analysis (SpectrogramAnalysis.h): a full-scale sine at a
// bin's centre reads 0 dB whatever the FFT size and window, the channel setting
// picks what's analysed, a short hop skips columns it can't keep up with,
// smoothing holds on, and the CQT reads a sine on a band's centre at 0 dB.
#include "../SpectrogramAnalysis.h"
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

using Analysis = SpectrogramAnalysis;
using Window = chardsp::SpectrumWindow;

static int failures = 0;
#define CHECK(condition)                                                                      \
    do                                                                                        \
    {                                                                                         \
        if (!(condition))                                                                     \
            std::fprintf (stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition), ++failures; \
    } while (0)

const double pi = 3.14159265358979;

// Pushes `frames` of a sine in each channel (amplitude 0 for silence) from sample `start`, in 512-sample blocks.
static void feed (Analysis& stft, uint32_t start, uint32_t frames, double leftCycles, float leftAmplitude,
    double rightCycles, float rightAmplitude)
{
    std::vector<float> left (512), right (512);
    for (uint32_t n = 0; n < frames; n += 512)
    {
        for (uint32_t i = 0; i < 512; ++i)
        {
            left[i] = leftAmplitude * float (std::sin (2 * pi * leftCycles * (start + n + i)));
            right[i] = rightAmplitude * float (std::sin (2 * pi * rightCycles * (start + n + i)));
        }
        stft.push (left.data(), right.data(), 512);
    }
}

int main()
{
    // Every size and window: a sine on bin size / 8 reads 0 dB there.
    for (int size = 256; size <= 16384; size *= 2)
        for (Window window : { Window::rectangular, Window::hann, Window::hamming, Window::blackman })
        {
            auto stft = std::make_unique<Analysis>();
            stft->prepare();
            stft->configure (Analysis::Fft { .size = size, .hop = size, .window = window });
            const int bin = size / 8;
            feed (*stft, 0, uint32_t (size) * 2, double (bin) / size, 1, double (bin) / size, 1);
            std::vector<float> db;
            uint32_t advanced = 0;
            bool any = false;
            while (stft->next (db, advanced))
                any = true;
            CHECK (any && int (db.size()) == size / 2);
            if (any && std::abs (db[size_t (bin)]) > 0.05f)
                std::fprintf (stderr, "size %d window %d: %.3f dB\n", size, int (window), db[size_t (bin)]), ++failures;
        }

    // The channel: a 1 kHz sine on the left, 3 kHz on the right (bins 43 and 128 of 2048 at 48 kHz, nearly).
    {
        const int size = 2048;
        auto level = [&] (Analysis::Channel channel, int bin) {
            auto stft = std::make_unique<Analysis>();
            stft->prepare();
            stft->configure (Analysis::Fft { .size = size, .hop = 512, .channel = channel });
            feed (*stft, 0, 8192, 43.0 / size, 1, 128.0 / size, 1);
            std::vector<float> db;
            uint32_t advanced = 0;
            while (stft->next (db, advanced, 100))
            {
            }
            return db[size_t (bin)];
        };
        CHECK (std::abs (level (Analysis::Channel::left, 43)) < 0.1f && level (Analysis::Channel::left, 128) < -100);
        CHECK (std::abs (level (Analysis::Channel::right, 128)) < 0.1f && level (Analysis::Channel::right, 43) < -100);
        // The mix halves each: -6 dB.
        CHECK (std::abs (level (Analysis::Channel::mix, 43) + 6.02f) < 0.1f
               && std::abs (level (Analysis::Channel::mix, 128) + 6.02f) < 0.1f);
    }

    // A column every hop, each saying how far it moved; more waiting than
    // `keep` skips to the newest, and the skipped audio is in `advanced`.
    {
        auto stft = std::make_unique<Analysis>();
        stft->prepare();
        stft->configure (Analysis::Fft { .size = 1024, .hop = 256 });
        feed (*stft, 0, 1024, 0.1, 1, 0.1, 1);
        std::vector<float> db;
        uint32_t advanced = 0, total = 0;
        int columns = 0;
        while (stft->next (db, advanced, 100))
            ++columns, total += advanced;
        CHECK (columns == 4 && total == 1024);
        feed (*stft, 1024, 256 * 20, 0.1, 1, 0.1, 1);
        columns = 0, total = 0;
        while (stft->next (db, advanced, 4))
            ++columns, total += advanced;
        CHECK (columns == 4 && total == 256 * 20);
    }

    // Smoothing 0.5: a sine that stops falls from 0 dB halfway (in dB) each column.
    {
        auto stft = std::make_unique<Analysis>();
        stft->prepare();
        stft->configure (Analysis::Fft { .size = 256, .hop = 256, .window = Window::rectangular, .smoothing = 0.5f });
        feed (*stft, 0, 512, 32.0 / 256, 1, 32.0 / 256, 1);
        std::vector<float> db;
        uint32_t advanced = 0;
        while (stft->next (db, advanced))
        {
        }
        CHECK (std::abs (db[32]) < 0.05f);
        feed (*stft, 512, 512, 0, 0, 0, 0); // silence: -160 dB raw
        CHECK (stft->next (db, advanced) && std::abs (db[32] - (-80)) < 0.1f);
        CHECK (stft->next (db, advanced) && std::abs (db[32] - (-120)) < 0.1f);
    }

    // The CQT: a full-scale sine on A4 (a band's centre) reads 0 dB in its band.
    {
        auto cqt = std::make_unique<Analysis>();
        cqt->prepare();
        Analysis::Cqt settings;
        settings.design.sampleRate = 48000, settings.design.minFrequency = 55, settings.design.maxFrequency = 16000;
        settings.hop = 1600;
        CHECK (cqt->configure (settings));
        feed (*cqt, 0, 65536, 440.0 / 48000, 1, 440.0 / 48000, 1);
        std::vector<float> db;
        uint32_t advanced = 0;
        bool any = false;
        while (cqt->next (db, advanced, 100))
            any = true;
        const auto& hz = cqt->cqtTransform().getFrequencies();
        CHECK (any && db.size() == hz.size());
        for (size_t b = 0; b < hz.size(); ++b)
            if (std::abs (hz[b] - 440) < 0.01)
                CHECK (std::abs (db[b]) < 0.1f);
        // A design with no bands makes no columns.
        settings.design.minFrequency = 30000;
        CHECK (!cqt->configure (settings) && !cqt->next (db, advanced));
    }

    if (failures)
        std::fprintf (stderr, "%d failed\n", failures);
    return failures ? 1 : 0;
}
