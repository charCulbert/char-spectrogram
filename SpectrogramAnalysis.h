#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
#include "chardsp_AnalysisBuffer.h"
#include "chardsp_ConstantQSpectrogram.h"
#include "chardsp_Spectrum.h"

// The spectrogram's analysis, made of chardsp's parts: an AnalysisBuffer
// brings both channels from the audio thread, and each column is a windowed
// FFT (Spectrum) or a constant-Q transform (ConstantQSpectrogram) of the
// newest block of the channel chosen (left, right, or their mix), in dB
// relative to a full-scale sine, with time smoothing.
//
// The audio thread only push()es. The configure and next calls run on
// another thread (the plugin's main thread), which sets the transform up and
// reads the buffer; configuring allocates when the transform changes.
struct SpectrogramAnalysis
{
    enum class Channel
    {
        mix,
        left,
        right
    };

    // The largest block either transform takes: the CQT's with 36 bands an
    // octave from 27.5 Hz at 96 kHz, 131072 samples. The buffer holds two,
    // 2 MB for both channels.
    static constexpr int largestBlock = 131072;

    struct Fft
    {
        int size = 2048, hop = 512; // samples: a power of two from 256 to 16384, and 1 to size
        chardsp::SpectrumWindow window = chardsp::SpectrumWindow::hann;
        Channel channel = Channel::mix;
        float smoothing = 0; // 0 (off) to 0.9: how much of each bin's last value stays (in dB)
        bool operator== (const Fft&) const = default;
    };

    struct Cqt
    {
        chardsp::ConstantQDesign design;
        int hop = 1600;
        chardsp::ConstantQReading reading = chardsp::ConstantQReading::mean;
        Channel channel = Channel::mix;
        float smoothing = 0;
        bool operator== (const Cqt& other) const
        {
            return design == other.design && hop == other.hop && reading == other.reading && channel == other.channel
                   && smoothing == other.smoothing;
        }
    };

    // While nothing pushes.
    void prepare()
    {
        input.prepare (2, 2 * largestBlock);
    }

    // Audio thread: never blocks or allocates.
    void push (const float* left, const float* right, uint32_t frames) noexcept
    {
        const float* channels[] = { left, right };
        input.push (channels, 2, int (frames));
    }

    // The FFT's columns from now on: size / 2 bins, bin k at k · sampleRate /
    // size Hz. Anything changed starts afresh from the newest samples.
    void configure (const Fft& settings)
    {
        Fft s = settings;
        s.size = std::clamp (s.size, 256, 16384);
        s.hop = std::clamp (s.hop, 1, s.size);
        s.smoothing = std::clamp (s.smoothing, 0.0f, 0.9f);
        if (usingFft && s == fft)
            return;
        if (!usingFft || s.size != fft.size || s.window != fft.window)
            spectrum.prepare ({ .size = s.size, .window = s.window }); // symmetric
        fft = s, usingFft = true, started = true;
        startAfresh (s.hop);
    }

    // The CQT's columns from now on, a value per band. False, and no columns,
    // when the design makes no bands.
    bool configure (const Cqt& settings)
    {
        if (started && !usingFft && settings == cqt)
            return cqtReady;
        cqtReady = transform.configure (settings.design, settings.hop, settings.reading);
        cqt = settings, usingFft = false, started = true;
        startAfresh (settings.hop);
        return cqtReady;
    }

    const chardsp::ConstantQ<float>& cqtTransform() const
    {
        return transform.getTransform();
    }

    // The next column into `decibels`, if a hop has arrived since the last,
    // and in `advanced` the samples since the last column. With more than
    // `keep` hops waiting it skips to the newest, so a short hop with a large
    // block can't take more time than there is.
    bool next (std::vector<float>& decibels, uint32_t& advanced, int keep = 8)
    {
        if (!started || (!usingFft && !cqtReady))
            return false;
        const int size = usingFft ? fft.size : transform.getBlockSize();
        advanced = 0;
        for (;;)
        {
            if (!input.nextHop (size, keep))
                return false;
            advanced += input.getAdvanced();
            if (read (usingFft ? fft.channel : cqt.channel, size))
                break; // else the writer overtook the read: the next hop
        }
        if (usingFft)
        {
            spectrum.process (block.data());
            decibels.resize (size_t (size / 2));
            for (int k = 1; k < size / 2; ++k)
                decibels[size_t (k)] = std::max (-160.0f, 20 * std::log10 (spectrum.getMagnitude (k) + 1e-12f));
            decibels[0] = decibels[1]; // DC shows as its neighbour
        }
        else
        {
            decibels.resize (size_t (transform.getTransform().getNumBands()));
            transform.process (block.data(), decibels.data());
        }
        // Time smoothing: each value keeps that much of its last (in dB).
        const float smoothing = usingFft ? fft.smoothing : cqt.smoothing;
        if (smoothing > 0)
        {
            if (smoothed.size() != decibels.size())
                smoothed = decibels;
            for (size_t k = 0; k < decibels.size(); ++k)
                decibels[k] = smoothed[k] = smoothing * smoothed[k] + (1 - smoothing) * decibels[k];
        }
        return true;
    }

private:
    void startAfresh (int hop)
    {
        smoothed.clear();
        input.restart (hop);
    }

    // The block ending at the hop: one channel, or the mix of both. False when torn.
    bool read (Channel channel, int size)
    {
        block.resize (size_t (size));
        if (channel != Channel::mix)
            return input.read (channel == Channel::left ? 0 : 1, size, block.data());
        other.resize (size_t (size));
        const bool whole = input.read (0, size, block.data()) && input.read (1, size, other.data());
        for (int i = 0; i < size; ++i)
            block[size_t (i)] = 0.5f * (block[size_t (i)] + other[size_t (i)]);
        return whole;
    }

    chardsp::AnalysisBuffer<float> input;
    chardsp::Spectrum<float> spectrum;
    chardsp::ConstantQSpectrogram<float> transform;
    Fft fft;
    Cqt cqt;
    bool started = false, usingFft = false, cqtReady = false;
    std::vector<float> block, other, smoothed;
};
