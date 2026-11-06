#include "SpectralMaskEngine.h"

#include <algorithm>
#include <cmath>
#include <random>

namespace Snow::DSP
{

namespace
{
constexpr double pi = 3.14159265358979323846;

// Periodic Hann with a half-sample phase shift:
//   w[k] = 0.5 - 0.5 * cos(2*pi*(k + 0.5) / N)
// This is the plan's "Hanning" window; the sum of squares over the four
// overlapping frames at 75% overlap is still exactly 1.5 (the 2/3 WOLA
// normalization holds), so interior samples reconstruct at unity gain. The
// +0.5 shift keeps w[0] non-zero (a strict periodic Hann would give exactly
// zero), so a sample at the very first window edge is not lost outright — it
// survives heavily attenuated, as only one of the four overlapping frames
// covers it (verified by the impulse tests: interior exact, edge nonzero).
void buildHannWindow (std::vector<float>& w, int size)
{
    w.resize ((size_t) size);
    for (int k = 0; k < size; ++k)
        w[(size_t) k] = 0.5f - 0.5f * (float) std::cos (2.0 * pi * (k + 0.5) / size);
}

} // namespace

// ==============================================================================
void SpectralMaskEngine::prepare (double newSampleRate, int channels)
{
    sampleRate = newSampleRate;
    numChannels = std::min (std::max (channels, 1), maxChannels);

    buildHannWindow (window, fftSize);

    for (auto& r : inputRing)  r.assign ((size_t) fftSize, 0.0f);
    for (auto& o : ola)        o.assign ((size_t) olaSize, 0.0f);
    for (auto& t : timeBuffer) t.assign ((size_t) fftSize, 0.0f);
    for (auto& s : spectrum)   s.assign ((size_t) spectrumSize, std::complex<float> (0.0f, 0.0f));
    for (auto& r : outputRing) r.assign ((size_t) fftSize, 0.0f);
    for (auto& m : targetMask) m.assign ((size_t) numBins, 0.0f);
    for (auto& g : currentGain) g.assign ((size_t) numBins, 0.0f);
    for (auto& c : controlTarget) c.assign ((size_t) numBins - 1, 0.0f);   // control slots (<= 1023)
    for (auto& v : visMagnitude) v.assign ((size_t) numBins, 0.0f);

    // Size the UI snapshot to the engine's geometry (idempotent — the assign
    // only reallocates when the size actually changed, e.g. a sample-rate
    // change).
    if (spectrumSnapshot != nullptr)
    {
        for (auto& buf : spectrumSnapshot->frames)
            for (auto& ch : buf)
                if ((int) ch.size() != numBins)
                    ch.assign ((size_t) numBins, 0.0f);
        spectrumSnapshot->numBins = numBins;
        spectrumSnapshot->sampleRate = sampleRate;
    }

    fft.setSize ((size_t) fftSize);

    // Slew coefficient matched to the sample rate and the frame rate
    // (plan.md §4 step 5): alpha = 1 - exp(-hop / (fs * tau)) — the same
    // physical ~10 ms transition at 44.1/48/96 kHz.
    framePeriod = (double) hopSize / sampleRate;
    alpha = 1.0f - (float) std::exp (-framePeriod / slewTimeSeconds);

    reset();
}

void SpectralMaskEngine::reset() noexcept
{
    for (auto& r : inputRing)  std::fill (r.begin(), r.end(), 0.0f);
    for (auto& o : ola)        std::fill (o.begin(), o.end(), 0.0f);
    for (auto& t : timeBuffer) std::fill (t.begin(), t.end(), 0.0f);
    for (auto& s : spectrum)   std::fill (s.begin(), s.end(), std::complex<float> (0.0f, 0.0f));
    for (auto& r : outputRing) std::fill (r.begin(), r.end(), 0.0f);
    for (auto& m : targetMask) std::fill (m.begin(), m.end(), 0.0f);
    for (auto& g : currentGain) std::fill (g.begin(), g.end(), 0.0f);
    for (auto& c : controlTarget) std::fill (c.begin(), c.end(), 0.0f);

    ringWrite = 0;
    samplesInRing = 0;
    olaWritePos = 0;
    frameIndex = 0;
    outPos = 0;
    frameTime = 0.0;

    // Re-seed the per-channel RNG streams (fresh texture on re-engage). Both
    // streams are seeded independently so stereo=1 decorrelation is real.
    std::random_device rd;
    for (auto& s : rngState)
    {
        auto value = rd();
        s = value == 0 ? 0x9E3779B9u : value;
    }
}

// ==============================================================================
void SpectralMaskEngine::setMaskParameters (float newAmount, float newWidthSeconds,
                                            float newCutoff, float newPhaseDegrees,
                                            float newStereo) noexcept
{
    amount        = std::clamp (newAmount, 0.0f, 1.0f);
    widthSeconds  = std::clamp (newWidthSeconds, 0.001f, 10.0f);
    cutoff        = std::clamp (newCutoff, 0.0f, 1.0f);
    phaseRadians  = newPhaseDegrees * (float) (pi / 180.0);
    stereo        = std::clamp (newStereo, 0.0f, 1.0f);

    // Randomized band from cutoff (plan.md §2, logarithmic): the band grows
    // EXPONENTIALLY upward from the bottom — cutoff 0 -> lowest 16 bins
    // (1..16), cutoff 0.5 -> lowest 128 bins, cutoff 1 -> all 1023 bins.
    // cutoffEnd = 1 + min(1023, 16*8^(2*cutoff)), exclusive — computed as
    // 16*exp2(6*cutoff). Everything at or above cutoffEnd is hard-muted.
    const float bandCount = std::min (1023.0f, 16.0f * std::exp2 (6.0f * cutoff));
    cutoffEnd = 1 + (int) bandCount;   // exclusive
    numControls = (cutoffEnd - 1 + gridSize - 1) / gridSize;   // controls over bins 1..cutoffEnd-1

    // The hard-muted region is a FIXED full mask (target 1): set it here on
    // every parameter push so the mute is active from the first frame (not
    // only after the first mask refresh) and follows cutoff changes
    // immediately — the per-frame slew makes the band edge move click-free.
    for (int ch = 0; ch < numChannels; ++ch)
        for (int b = cutoffEnd; b < numBins - 1; ++b)
            targetMask[ch][(size_t) b] = 1.0f;
}

// ==============================================================================
void SpectralMaskEngine::setGridSize (int newGridSize) noexcept
{
    gridSize = std::clamp (newGridSize, 1, 16);
    numControls = (cutoffEnd - 1 + gridSize - 1) / gridSize;
}

// ==============================================================================
void SpectralMaskEngine::setMaskDrift (float stepPerTick) noexcept
{
    driftStep = std::clamp (stepPerTick, 0.0f, 1.0f);
}

// ==============================================================================
std::uint32_t SpectralMaskEngine::nextRandom (int channelIndex) noexcept
{
    std::uint32_t x = rngState[(size_t) channelIndex];
    if (x == 0)
        x = 0x9E3779B9u;                    // zero-state guard
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rngState[(size_t) channelIndex] = x;
    return x;
}

float SpectralMaskEngine::unitRandom (int channelIndex) noexcept
{
    return (float) (nextRandom (channelIndex) >> 8) * (1.0f / 16777216.0f);   // [0,1)
}

// ==============================================================================
void SpectralMaskEngine::refreshTargets() noexcept
{
    // Draw order per control is FROZEN (deterministic stream): (1) left draw,
    // (2) right draw, (3) the stereo share coin. Controls are spaced gridSize
    // bins apart over the randomized band and cosine-interpolated per bin, so
    // the target mask is a smooth random spectral envelope: neighbouring bins
    // move together (band-correlated decisions) instead of being gated
    // independently — no isolated windowed-sine pings and no sharp per-bin
    // notches (the two mechanisms that made the mask sound metallic). grid=1
    // reduces to the original per-bin binary draw. Bin 0 (DC) and numBins-1
    // (Nyquist) pass through; the hard-muted region at/above the cutoff is
    // not drawn here — its targets are a fixed 1 (set in setMaskParameters).
    // The drawn values are stored live in controlTarget: between refreshes
    // driftTargets() walks them, so the envelope never freezes (a frozen
    // random amplitude comb reads as metallic static flange).
    for (int j = 0; j < numControls; ++j)
    {
        const float drawL = unitRandom (0) < amount ? 1.0f : 0.0f;
        const float drawR = unitRandom (1) < amount ? 1.0f : 0.0f;
        // Stereo decoupling: with probability (1 - stereo) the right channel
        // reuses the left's control (identical masks -> coherent mono
        // cancellation); otherwise it keeps its own independent draw (extreme
        // width).
        controlTarget[0][(size_t) j] = drawL;
        controlTarget[1][(size_t) j] = (unitRandom (1) < 1.0f - stereo) ? drawL : drawR;
    }

    interpolateTargets (0);
    interpolateTargets (1);
}

// ==============================================================================
void SpectralMaskEngine::interpolateTargets (int channelIndex) noexcept
{
    // Bins at or above bandEnd are never written here — their targets are the
    // fixed full mask (1) of the hard-muted region.
    const int bandStart = 1;                     // lowest randomized bin (always bin 1)
    const int bandEnd   = cutoffEnd;             // exclusive — cutoff 0: 17 (16 bins), cutoff 1: 1024 (all bins)
    const float* ctrl = controlTarget[channelIndex].data();
    float* tgt = targetMask[channelIndex].data();

    for (int b = bandStart; b < bandEnd; ++b)
    {
        const int rel = b - bandStart;
        const int j = rel / gridSize;
        const float frac = float (rel % gridSize) / float (gridSize);
        // Cosine taper between the flanking controls — C1-continuous envelope
        // (max slope pi/2 per grid cell, so adjacent bins differ by at most
        // pi/(2*gridSize)).
        const float w = 0.5f - 0.5f * (float) std::cos (pi * (double) frac);
        const int jn = (j + 1 < numControls) ? j + 1 : j;

        tgt[(size_t) b] = ctrl[(size_t) j] + (ctrl[(size_t) jn] - ctrl[(size_t) j]) * w;
    }
}

// ==============================================================================
void SpectralMaskEngine::driftTargets (int channelIndex) noexcept
{
    // The mask must never freeze: a frozen random amplitude comb — deep
    // notches (1 - g -> 0 at selected controls) held still for the whole
    // Width window — reads as metallic static flange ("the bins out of
    // phase"), the phase=0 artefact that the phase-preservation fix could
    // not touch (at phi = 0 the old and new formulas are the identical real
    // scale 1 - g). Every tick each control performs a small clamped random
    // walk and the envelope is re-interpolated, so the notches wander and
    // breathe: continuous motion reads as organic noise/phaser texture. The
    // step scales with amount*(1-amount), so amount 0 and 1 freeze exactly
    // (clean pass-through / exact full-mask silence) and the motion
    // concentrates where the comb is richest (mid amounts). Width still sets
    // the coarse refresh cadence; the walk is the fine texture between
    // refreshes.
    const float scale = driftStep * amount * (1.0f - amount);
    if (scale <= 0.0f)
        return;

    float* ctrl = controlTarget[channelIndex].data();
    for (int j = 0; j < numControls; ++j)
    {
        const float step = (unitRandom (channelIndex) - 0.5f) * 2.0f * scale;
        ctrl[(size_t) j] = std::clamp (ctrl[(size_t) j] + step, 0.0f, 1.0f);
    }

    interpolateTargets (channelIndex);
}

// ==============================================================================
void SpectralMaskEngine::applyMaskToChannel (int channelIndex,
                                             std::complex<float>* spectrumData) noexcept
{
    const float a = alpha;
    float* cur = currentGain[channelIndex].data();
    const float* tgt = targetMask[channelIndex].data();

    // Per-frame mask-character term (global across bins; identical for both
    // channels — the stereo texture comes from the mask decoupling, not the
    // phase). plan.md §3 step 5: Z' = Z * |1 - g * e^{j*phi}| — the mask
    // scales the bin amplitude and PRESERVES the analysis phase.
    const float cosPhi = std::cos (phaseRadians);

    const int end = numBins - 1;   // DC (0) and Nyquist (numBins-1) pass through
    for (int b = 1; b < end; ++b)
    {
        // Slew-rate smoothing toward the current target (plan.md §3 step 4):
        //   current[b] += alpha * (target[b] - current[b])
        float g = cur[b] + a * (tgt[b] - cur[b]);
        if (g < snapThreshold)
            g = 0.0f;              // snap the denormal tail; targets are never negative
        cur[b] = g;

        if (g <= 0.0f)
            continue;

        const float re = spectrumData[b].real();
        const float im = spectrumData[b].imag();

        if (b >= cutoffEnd)
        {
            // Hard-muted region (the Cutoff filter side): pure amplitude
            // cancellation (1 - g), converging to EXACT silence at g = 1
            // regardless of the Phase knob — the region above the cutoff is
            // silent, not phase-rotated.
            const float m = 1.0f - g;
            spectrumData[b] = { m * re, m * im };
            continue;
        }

        // Randomized band — phase-coherent amplitude mask (plan.md §3 step 5):
        // m = |1 - g*e^{j*phi}| = sqrt(1 - 2g*cos(phi) + g^2), a real
        // non-negative scale that keeps the ANALYSIS PHASE of every bin.
        // (The previous complex form Z' = (1 - g*e^{j*phi}) * Z rotated each
        // bin by arg(1 - g*e^{j*phi}) — a mask-dependent static phase offset
        // per bin, a dispersive response that sounded like a metallic static
        // flange: "the bins out of phase".)
        const float m = std::sqrt (1.0f - 2.0f * g * cosPhi + g * g);
        spectrumData[b] = { m * re, m * im };
    }
}

// ==============================================================================
void SpectralMaskEngine::analyseChannel (int channelIndex) noexcept
{
    // The frame is the OLDEST fftSize samples in the ring: the window ends at
    // the current input position (plan.md §3 step 1).
    float* timeData = timeBuffer[channelIndex].data();
    const float* ringData = inputRing[channelIndex].data();

    for (int k = 0; k < fftSize; ++k)
    {
        int idx = ringWrite + k;
        if (idx >= fftSize) idx -= fftSize;
        timeData[k] = window[(size_t) k] * ringData[idx];
    }

    std::complex<float>* specData = spectrum[channelIndex].data();
    fft.fft (timeData, specData);

    applyMaskToChannel (channelIndex, specData);

    // Visualization: capture the POST-mask spectrum magnitudes for the UI
    // display — the 1023 true harmonics, bins 1..numBins-2 (the DC/Nyquist
    // pair in bin 0 is not displayed). Captured AFTER the masking step, so the
    // display mirrors what the mask actually lets through.
    {
        float* vis = visMagnitude[(size_t) channelIndex].data();
        for (int b = 1; b < numBins - 1; ++b)
            vis[(size_t) b] = std::abs (specData[(size_t) b]);
    }

    fft.ifft (specData, timeData);

    // WOLA synthesis: same Hann window on the synthesis side. Two normalizing
    // factors compose here:
    //   - 1/N: the signalsmith forward+inverse round trip is N*x (both
    //     transforms are unnormalized), so the ifft output carries an extra
    //     factor of the transform size;
    //   - 2/3: 1 / sum of w^2 over the four overlapping frames at 75% overlap.
    // The frame is placed at its start position in the overlap-add ring.
    float* olaData = ola[channelIndex].data();
    const float norm = wolaNormalization / fftSize;
    for (int k = 0; k < fftSize; ++k)
    {
        int idx = olaWritePos + k;
        if (idx >= olaSize) idx -= olaSize;
        olaData[idx] += norm * window[(size_t) k] * timeData[k];
    }
}

// ==============================================================================
void SpectralMaskEngine::publishSnapshot() noexcept
{
    if (spectrumSnapshot == nullptr || numChannels == 0)
        return;

    // Write the frame into the buffer that is NOT the currently published one,
    // then advance the counter (release) — the UI reads the published buffer,
    // so it never sees a torn frame (a racing republish is detected by the UI
    // via the counter, which is monotonic and never reset).
    auto& dest = spectrumSnapshot->frames[(size_t) (snapshotCount & 1)];
    for (int c = 0; c < numChannels; ++c)
        std::copy (visMagnitude[(size_t) c].begin(),
                   visMagnitude[(size_t) c].end(),
                   dest[(size_t) c].begin());
    spectrumSnapshot->publishCount.store (snapshotCount + 1, std::memory_order_release);
    ++snapshotCount;
}

// ==============================================================================
void SpectralMaskEngine::runTick() noexcept
{
    // Timer check (plan.md §3 step 3): when the current mask's age exceeds
    // Width, draw a fresh target mask for both channels. Drift-free by
    // subtracting the dwell time instead of zeroing the accumulator.
    frameTime += framePeriod;
    if (frameTime >= widthSeconds)
    {
        frameTime -= widthSeconds;
        refreshTargets();
    }

    // Per-tick mask motion: the controls walk so the envelope never freezes
    // between refreshes (a frozen amplitude comb reads as metallic static
    // flange); the refresh above recenters the walk at the Width cadence.
    for (int c = 0; c < numChannels; ++c)
        driftTargets (c);

    // Per-channel frame: window -> fft -> mask (slew + amplitude scale) ->
    // ifft -> WOLA add. The refresh above feeds every channel of this frame.
    for (int c = 0; c < numChannels; ++c)
        analyseChannel (c);

    // Publish the post-mask spectrum for the UI display (no-op without a
    // snapshot sink).
    publishSnapshot();

    // Drain the completed WOLA chunk into the output ring, then zero it. The
    // output ring holds 4 hop-sized chunks; the chunk for out position n is
    // emitted three hops later (see process()), giving the constant
    // N - hop = 1536-sample output delay ("drain positioned at input time -
    // 1536").
    const int slotBase = (int) (frameIndex % 4) * hopSize;
    for (int c = 0; c < numChannels; ++c)
    {
        float* ringOut = outputRing[c].data();
        float* olaData = ola[c].data();
        for (int r = 0; r < hopSize; ++r)
        {
            int idx = olaWritePos + r;
            if (idx >= olaSize) idx -= olaSize;
            ringOut[slotBase + r] = olaData[idx];
            olaData[idx] = 0.0f;
        }
    }

    olaWritePos = (olaWritePos + hopSize) % olaSize;
    samplesInRing -= hopSize;
    ++frameIndex;
}

// ==============================================================================
void SpectralMaskEngine::process (const float* const* inputChannels, float* const* outputChannels,
                                  int channels, int numSamples) noexcept
{
    if (numSamples <= 0)
        return;

    channels = std::min (std::max (channels, 1), maxChannels);
    const std::int64_t blockStart = outPos;   // absolute out-stream position of this block
    outPos += numSamples;

    // Emission modes. Chunk f covers out positions [f*hop, (f+1)*hop) and
    // holds the drained WOLA chunk of hop tick f-3, which runs when the input
    // reaches (f+1)*hop (the 1536-sample delay: out[f*hop+r] = in[f*hop+r-1536]).
    //
    // STREAM-ALIGNED emission (blocks ending on the hop grid, i.e. block size
    // a multiple of 512): every chunk is finalized by its block's end, and the
    // emission reads the ring slot exactly once per block — this is the 1536
    // latency path.
    //
    // LAGGED emission (any other block size — 32/64/128/300...): the block's
    // own buffer positions [P-bs, P) are not final at block end (their chunks
    // drain four hops later), so the block delivers the stream one extra hop
    // behind: buffer position j carries stream position j-512, whose chunk is
    // always final by block end (tick f-3 ran at input 512(f+1) <= j <= P).
    // The delivered stream is therefore the aligned stream delayed by one hop
    // (total latency 2048); positions n < 0 are the pre-stream silence.
    const bool lagged = (numSamples % hopSize) != 0;

    for (int segment = 0; segment * hopSize < numSamples; ++segment)
    {
        const int segStart = segment * hopSize;
        const int segLen = std::min (hopSize, numSamples - segStart);

        // --- 1. append + hop pipeline ---
        for (int s = 0; s < segLen; ++s)
        {
            for (int c = 0; c < channels; ++c)
                inputRing[c][ringWrite] = inputChannels[c][segStart + s];

            ringWrite = (ringWrite + 1) % fftSize;
            ++samplesInRing;

            while (samplesInRing >= fftSize)
                runTick();
        }

        // --- 2. stream-aligned emission of the chunks finalized here ---
        if (! lagged)
        {
            // A chunk is final when tick f-3 drained it — i.e. the input has
            // reached (f+1)*hop, the current segment's end. Chunks 0..2 hold
            // the initial zeros (their ticks never run) and are final from the
            // start. (For hop-aligned blocks the check is always satisfied;
            // it exists to make the aligned path provably gap-free.)
            const std::int64_t segEnd = blockStart + segStart + segLen;
            const std::int64_t f0 = (blockStart + segStart) / hopSize;
            for (std::int64_t f = f0 - 3; f <= f0; ++f)
            {
                const std::int64_t chunkStart = f * hopSize;
                if (chunkStart >= blockStart + numSamples || chunkStart + hopSize <= blockStart)
                    continue;   // chunk does not overlap this block
                if (f > 2 && (f + 1) * hopSize > segEnd)
                    continue;   // tick f-3 has not drained yet

                const int rStart = (int) std::max<std::int64_t> (0, blockStart - chunkStart);
                const int rEnd   = (int) std::min<std::int64_t> (hopSize, blockStart + numSamples - chunkStart);
                if (rEnd <= rStart)
                    continue;

                // Slot of chunk f: tick f-3 stored it (slot (f-3) mod 4 == (f+1) mod 4).
                const int slot = (int) ((f % 4 + 4 + 1) % 4);
                const int outStart = (int) (chunkStart - blockStart) + rStart;
                const int count = rEnd - rStart;

                for (int c = 0; c < channels; ++c)
                {
                    const float* src = outputRing[c].data() + slot * hopSize + rStart;
                    float* dst = outputChannels[c] + outStart;
                    std::copy (src, src + count, dst);
                }
            }
        }
    }

    // --- 3. lagged emission (blocks that do not end on the hop grid) ---
    if (lagged)
    {
        const std::int64_t P = blockStart + numSamples;
        for (int p = 0; p < numSamples; ++p)
        {
            const std::int64_t n = blockStart + p - hopSize;   // out-stream position of buffer slot p
            for (int c = 0; c < channels; ++c)
            {
                float value = 0.0f;                   // n < 0: pre-stream silence
                if (n >= 0)
                {
                    const int f = (int) (n / hopSize);
                    const int r = (int) (n % hopSize);
                    const int slot = (f % 4 + 4 + 1) % 4;
                    value = outputRing[c][(size_t) (slot * hopSize) + (size_t) r];
                }
                outputChannels[c][p] = value;
            }
        }
    }
}

} // namespace Snow::DSP
