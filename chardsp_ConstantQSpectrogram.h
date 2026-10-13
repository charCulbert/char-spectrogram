// From github.com/charCulbert/chardsp at 87bfc1e (MIT).
//
// A spectrogram from chardsp_ConstantQ.h, whose transform follows CiCueTea
// (https://github.com/jdsierral/CiCueTea, MIT); its notice is in that header.

#pragma once

#include <algorithm>
#include <cmath>
#include <type_traits>
#include <vector>

#include "chardsp_ConstantQ.h"

namespace chardsp
{

// A ConstantQ's spectrogram: a column of levels in dB per band (-160 at least)
// for every hop of a signal, from the newest block, each band read over the
// hop centred on the block's middle. It takes the block, one channel at a
// time; an AnalysisBuffer (chardsp_AnalysisBuffer.h) can bring it from the
// audio thread.
//
// ConstantQReading::centre reads only the middle, as most spectrograms do:
// one coefficient per band per hop. A high band's coefficients change far
// faster than that, so a click between two middles goes unseen and close
// tones in one band alias to a slow flicker. peak and mean read every
// coefficient in the hop instead. mean, the default, is the band's power over
// the hop: steady tones read as in centre, noise reads the same in every band
// whatever the hop, and a click reads its energy spread over the hop. peak
// shows a click at its own level, but reads noise higher in the bands with
// more coefficients per hop (the high ones).
//
// For peak and mean, the bands' coefficients over the hop only depend on the
// samples within about six of their atom's spreads (with a few bands per
// octave or more; see ConstantQ::getReach()), so bands are grouped by how many
// samples that is, rounded up to a power of two, and each group analyses only
// those: the same bands, sampled on the coarser bins of a shorter block.
//
// configure() allocates when anything changes; process() does not.
template <typename SampleType>
class ConstantQSpectrogram
{
public:
    static_assert (std::is_same_v<SampleType, float>
                   || std::is_same_v<SampleType, double>,
                   "ConstantQSpectrogram supports float or double");

    // False, analysing nothing, when the design isn't valid. Rebuilds only
    // what changed.
    bool configure (const ConstantQDesign& newDesign, int newHop,
                    ConstantQReading newReading = ConstantQReading::mean)
    {
        newHop = std::max (1, newHop);
        const bool rebuild = newDesign != design || ! transform.isValid();

        if (rebuild)
        {
            design = newDesign;
            transform.prepare (design);
            magnitudes.assign (static_cast<size_t> (transform.getNumBands()),
                               SampleType {});
        }

        if (rebuild || newHop != hop)
        {
            hop = newHop;
            group();
        }

        reading = newReading;
        return transform.isValid();
    }

    // The bands, with the whole block.
    const ConstantQ<SampleType>& getTransform() const noexcept
    {
        return transform;
    }

    int getBlockSize() const noexcept { return transform.getBlockSize(); }
    int getHop() const noexcept { return hop; }

    // The column for `block` (getBlockSize() samples) into `decibels` (a value
    // per band). It is centred on the block's middle, getBlockSize() / 2
    // samples in, so for a block ending at position P its time is
    // P - getBlockSize() / 2: centre reads there, and peak and mean read the
    // hop around it, from getHop() / 2 before to getHop() / 2 after.
    void process (const SampleType* block, SampleType* decibels) noexcept
    {
        if (! transform.isValid())
            return;

        const int size = transform.getBlockSize();

        if (reading == ConstantQReading::centre)
        {
            transform.process (block);

            for (int b = 0; b < transform.getNumBands(); ++b)
                magnitudes[static_cast<size_t> (b)] =
                    transform.getCentreMagnitude (b);
        }
        else
        {
            const double middle = 0.5 * size;
            bool whole = false;

            for (auto& g : groups)
            {
                if (g.transform.isValid())
                {
                    const int length = g.transform.getBlockSize();
                    g.transform.process (block + (size - length) / 2);
                    g.transform.getMagnitudes (
                        reading, 0.5 * length - 0.5 * hop,
                        0.5 * length + 0.5 * hop, magnitudes.data() + g.first);
                    continue;
                }

                if (! whole)
                    transform.process (block), whole = true;

                for (int b = g.first; b < g.end; ++b)
                    magnitudes[static_cast<size_t> (b)] = transform.getMagnitude (
                        b, reading, middle - 0.5 * hop, middle + 0.5 * hop);
            }
        }

        for (size_t b = 0; b < magnitudes.size(); ++b)
            decibels[b] = static_cast<SampleType> (std::max (-160.0,
                20.0 * std::log10 (std::max (
                    1.0e-8, static_cast<double> (magnitudes[b])))));
    }

private:
    struct Group
    {
        int first = 0, end = 0; // bands
        int length = 0; // samples
        ConstantQ<SampleType> transform; // when not valid, the whole block's
    };

    // Each band's coefficients across the hop need the samples within six of
    // its atom's spreads either side.
    void group()
    {
        groups.clear();
        const int size = transform.getBlockSize();
        int previous = -1;

        for (int b = 0; b < transform.getNumBands(); ++b)
        {
            int length = 4;

            while (length < hop + 12.0 * transform.getSpread (b) && length < size)
                length *= 2;

            if (length != previous)
            {
                groups.emplace_back();
                groups.back().first = b;
                groups.back().length = previous = length;
            }

            groups.back().end = b + 1;
        }

        const auto& frequencies = transform.getFrequencies();

        for (auto& g : groups)
        {
            auto part = design;
            part.minFrequency = frequencies[static_cast<size_t> (g.first)];
            part.maxFrequency = frequencies[static_cast<size_t> (g.end - 1)];
            part.blockSize = g.length;

            // Too long, or bands it can't resolve: the whole block's.
            if (part.blockSize >= size || ! g.transform.prepare (part)
                || g.transform.getNumBands() != g.end - g.first)
                g.transform = ConstantQ<SampleType>();
        }
    }

    ConstantQ<SampleType> transform;
    std::vector<Group> groups;
    ConstantQDesign design;
    int hop = 0;
    ConstantQReading reading = ConstantQReading::mean;
    std::vector<SampleType> magnitudes;
};

} // namespace chardsp
