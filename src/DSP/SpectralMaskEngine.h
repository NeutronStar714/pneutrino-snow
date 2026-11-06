#pragma once

#include <array>
#include <atomic>
#include <complex>
#include <cstdint>
#include <memory>
#include <vector>

#include "signalsmith-fft.h" // vendored clone, MIT (attribution: Signalsmith Audio, https://signalsmith-audio.co.uk/code/fft)

// ==============================================================================
// SpectralMaskEngine — stochastic spectral masking via streaming STFT/WOLA
// (phase-vocoder backbone), inspired by the additive re-synthesis engines of
// synths like Image-Line Harmor (Resonance -> Noise mode).
//
// Contract: plan.md (root) — "Core DSP Logic & Overlap-Add Execution",
// "Spectral Masking Kernel", "Smoothing & Parameter Mapping".
//
// Geometry (plan.md §3):
//   N = 2048 window, hop = 512 (75% overlap / 4x overlap-add), WOLA
//   normalization 2/3 (1 / sum of Hann^2 over the four overlapping frames).
//   Input ring: N. Output overlap-add ring: N + 3*hop = 3584 (the live window
//   spans the placements of the four frames that can still contribute to
//   undrained positions). Drain positioned at "input time - (N - hop)":
//   aligned-path wet latency = 1536 samples; blocks that do not end on the hop
//   grid are emitted one extra hop behind (2048) — see process(). The plugin
//   reports ZERO latency — the processor blends this delayed wet against a
//   zero-latency dry tap with a fade-through crossfade (see PluginProcessor.h).
//
// VISUALIZATION: the engine publishes the POST-MASK spectrum magnitudes
// (per channel, bins 1..1023) once per hop tick into an optional
// SpectrumSnapshot — a lock-free double buffer the audio thread fills and the
// UI thread reads (see the struct below). The editor's scrolling frequency
// display consumes it; null snapshot = no publishing (headless/tests).
//
// BIN MODEL — signalsmith half-spectrum packing: spectrum[0] = {X[0], X[N/2]}
// (DC real, Nyquist imag, both signed reals), indices 1..N/2-1 hold the TRUE
// positive-frequency bins (no conjugate mirrors — verified for this library
// in the sibling Snowfall project), so the spectrum is a bijection R^N <->
// C^(N/2) and no mirror bookkeeping is ever needed. numBins = 1025
// (0 = DC, 1..1023 = true positive bins, 1024 = Nyquist).
//
// MASK MODEL (plan.md §3 steps 3-5):
//   - Every mask duration ("Width") a fresh target mask is drawn for both
//     channels from independent xorshift32 streams: random controls are drawn
//     on a coarse grid ("Grid Size" bins apart, default 8) and cosine-
//     interpolated per bin, so the target mask is a smooth random spectral
//     envelope — neighbouring bins move together (band-correlated decisions)
//     instead of being gated independently. (Grid = 1 reproduces the raw
//     per-bin dice.) Each control is selected with probability "Amount"; the
//     right channel's controls reuse the left's with probability
//     (1 - "Stereo"), otherwise it keeps its own independent draws (stereo
//     decoupling).
//   - "Cutoff" bounds the randomized band with the same logarithmic scale
//     and INVERTS the old Depth behavior: the region at or above the cutoff
//     is hard-muted (silence), not preserved. cutoff 0 -> ONLY the lowest
//     16 bins (1..16 @ N=2048) carry the random mask and every bin above is
//     cancelled to silence; cutoff 0.5 -> lowest 128 bins randomized, the
//     rest muted; cutoff 1 -> all 1023 bins randomized, nothing muted.
//     Formula cutoffEnd = 1 + min(1023, 16*8^(2*cutoff)) (exclusive).
//   - The mask values slew frame-by-frame toward the targets (per-frame
//     exponential smoothing with a ~10 ms time constant, rate-matched to the
//     sample rate: alpha = 1 - exp(-hop/(fs*tau))) so state refreshes never
//     click. With the smooth (grid-interpolated) envelope a 10 ms slew is
//     fast enough to read as noise-like texture rather than a slow morph.
//   - The mask NEVER freezes between refreshes: every hop tick each control
//     performs a small clamped random walk and the per-bin envelope is
//     re-interpolated, so the spectral envelope stays in continuous motion.
//     (A frozen random amplitude comb — deep notches held still for the
//     whole Width window — reads as metallic static flange; this was the
//     phase=0 artefact that the phase-preservation fix could not touch: at
//     phi = 0 the old and new formulas are the identical real scale 1 - g.)
//     The walk steps scale with amount*(1-amount)*driftStep, so amount 0
//     and 1 freeze exactly (clean pass-through / exact full-mask silence)
//     and the motion concentrates where the comb is richest (mid amounts).
//     Width still sets the coarse refresh cadence; the walk is the fine
//     texture between refreshes.
//   - Selected bins are AMPLITUDE-modulated with the analysis phase
//     PRESERVED: Z' = |1 - g*e^{j*phi}| * Z, where g is the smoothed mask
//     gain and phi the "Phase" offset (0° -> pure cancellation |1-g| = 1-g,
//     90° -> orthogonal mask |1-j*g| = sqrt(1+g^2), a spectral boost up to
//     +3 dB, 180° -> maximum boost 1+g, +6 dB (the plugin's PHASE knob maps
//     0..90 exponentially and tops out at the 90° boost). The mask is therefore a
//     purely linear-phase spectral envelope (a smooth random EQ), not a
//     phase distorter. The original complex form Z' = (1 - g*e^{j*phi}) * Z
//     rotated every bin by arg(1 - g*e^{j*phi}) — a mask-dependent STATIC
//     per-bin phase offset; a static frequency-dependent phase response is
//     dispersive (smears transients, freezes partials at random absolute
//     phases) and sounded like a metallic static flange ("the bins out of
//     phase"), worst near phi = 90° (up to -45° per bin). Bins at or above
//     the Cutoff band are hard-muted instead: pure amplitude cancellation
//     (1 - g), converging to EXACT silence at g = 1 regardless of the Phase
//     knob — the filter side of the control. DC and Nyquist always pass.
//   - Make-up gain, Dry and Wet are applied by the processor on the
//     time-domain tail (plan.md §3 step 7 / §4 step 5).
//
// Threading: audio-thread only; all buffers are members sized in prepare() —
// zero allocations on the audio thread. In-place operation (input == output
// pointers) is supported: output writes always land at or behind the current
// input-append position.
// ==============================================================================
namespace Snow::DSP
{

// ==============================================================================
// Visualization sink: the engine publishes the post-mask spectrum magnitudes
// (per channel) once per hop tick. Lock-free double buffer — the
// audio thread fills the buffer it is NOT currently publishing, then releases
// a new publishCount; the UI thread reads the buffer of the latest publication
// with an acquire load and can detect a racing republish by re-checking the
// count. Sizes are fixed in SpectralMaskEngine::prepare — zero allocation on
// the audio thread.
struct SpectrumSnapshot
{
    std::array<std::array<std::vector<float>, 2>, 2> frames;  // [buffer][channel][bin]
    std::atomic<std::int64_t> publishCount { 0 };             // monotonic, release-stored by the audio thread
    int numBins = 0;                                          // bins per channel (fixed in prepare)
    double sampleRate = 0.0;                                  // fixed in prepare (bin frequency scale)
};

class SpectralMaskEngine
{
public:
    // --- Geometry (plan.md contract) ---
    static constexpr int fftSize        = 2048;
    static constexpr int hopSize        = 512;
    static constexpr int numBins        = fftSize / 2 + 1;   // 1025 true bins (DC + 1023 + Nyquist)
    static constexpr int spectrumSize   = fftSize / 2;       // 1024 complex values
    static constexpr int olaSize        = fftSize + 3 * hopSize; // 3584
    static constexpr int latencySamples = fftSize - hopSize; // 1536 (aligned-path wet delay)
    static constexpr float wolaNormalization = 2.0f / 3.0f;  // 1 / sum of Hann^2 @ 75% overlap

    static constexpr int maxChannels = 2;                    // mono/stereo (plugin bus contract)

    // Mask slew time constant (plan.md §3 step 4 / §4 step 5: alpha is derived
    // from the sample rate and the frame rate so the same physical transition
    // happens at 44.1/48/96 kHz).
    static constexpr float slewTimeSeconds = 0.01f;
    // Gains below this after smoothing are snapped to zero (kills the
    // denormal tail; -80 dB, inaudible).
    static constexpr float snapThreshold = 1e-4f;

    SpectralMaskEngine() = default;

    // Sizes all buffers, builds the Hann table, configures the FFT, derives
    // the slew alpha, seeds the RNG streams, resets state.
    void prepare (double sampleRate, int numChannels);

    // Clears all signal state (rings, OLA, masks, counters) and re-seeds the
    // RNG streams (fresh texture on re-engage). Buffers, the window table and
    // parameters are retained. Called by prepare() and by the plugin when the
    // wet path re-engages after WET = 0.
    void reset() noexcept;

    // Streaming process. Consumes numSamples from inputChannels, advances the
    // hop pipeline, and writes the wet output (the input delayed by
    // latencySamples — or one extra hop on the lagged path, see process())
    // into outputChannels. Channels is clamped to maxChannels; input/output
    // pointers may alias (in-place processing).
    void process (const float* const* inputChannels, float* const* outputChannels,
                  int numChannels, int numSamples) noexcept;

    // --- Parameter push (audio thread, called from processBlock before process) ---
    //   amount 0..1   -> per-control selection probability per mask refresh
    //   widthSeconds  -> mask dwell time (seconds) between refreshes
    //   cutoff 0..1   -> randomized band grows exponentially from the bottom
    //                    (16 bins at 0 -> all bins at 1); everything at or
    //                    above the cutoff is hard-muted (silence)
    //   phaseDegrees  -> masking-character angle (0..90 from the plugin's
    //                    exponential knob; 0 = cancellation, 90 = +3 dB boost)
    //   stereo 0..1   -> left/right mask decoupling (0 = shared, 1 = independent)
    void setMaskParameters (float amount, float widthSeconds,
                            float cutoff, float phaseDegrees, float stereo) noexcept;

    // Mask smoothing grid (1..16 bins, default 8): targets are drawn on a
    // coarse grid this many bins apart over the randomized band and cosine-
    // interpolated per bin. Grid 1 = raw per-bin dice (binary targets);
    // larger grids = smooth band-correlated spectral envelope.
    void setGridSize (int gridSize) noexcept;

    // Mask motion (test/tuning hook): per-tick random-walk step of the mask
    // controls (default 0.4). The engine scales it by amount*(1-amount), so
    // amount 0 and 1 freeze exactly; 0 here disables the walk entirely (used
    // by the deterministic tests).
    void setMaskDrift (float stepPerTick) noexcept;

    // Visualization hook (optional): route the per-hop post-mask spectrum
    // magnitudes into a snapshot the UI can read (lock-free double buffer —
    // see SpectrumSnapshot). Attach before prepare() so the buffers are sized;
    // null (the default) disables publishing entirely.
    void setSpectrumSnapshot (std::shared_ptr<SpectrumSnapshot> snapshot) noexcept
    {
        spectrumSnapshot = std::move (snapshot);
    }

    // Test hooks:
    int currentCutoffEnd() const noexcept { return cutoffEnd; }
    const float* targetMaskData (int channel) const noexcept { return targetMask[channel].data(); }

private:
    // One hop tick: mask refresh (timer check) -> per-channel analysis
    // (window, fft, mask, ifft, WOLA add) -> drain the completed chunk into
    // the output ring. Runs when the ring holds a full window.
    void runTick() noexcept;

    // One channel's frame: window -> fft -> applyMaskToChannel -> ifft ->
    // WOLA add at olaWritePos.
    void analyseChannel (int channelIndex) noexcept;

    // Slew-rate smoothing + complex-plane modulation for one channel's bins.
    void applyMaskToChannel (int channelIndex, std::complex<float>* spectrumData) noexcept;

    // Draws fresh target masks for both channels (left draw, right draw, then
    // the stereo share decision — frozen draw order for determinism). The
    // drawn controls are stored live in controlTarget and interpolated.
    void refreshTargets() noexcept;

    // One tick of the mask random walk: every control steps by +-driftStep
    // (scaled by amount*(1-amount)), clamped to [0,1], then the per-bin
    // envelope is re-interpolated — the mask never freezes between refreshes.
    void driftTargets (int channelIndex) noexcept;

    // Cosine-interpolates controlTarget into targetMask for one channel
    // (band bins only; the hard-muted region keeps its fixed full mask).
    void interpolateTargets (int channelIndex) noexcept;

    // Publishes the captured input magnitudes into the snapshot's back buffer
    // and advances the publication counter (release) — called once per hop tick.
    void publishSnapshot() noexcept;

    std::uint32_t nextRandom (int channelIndex) noexcept;   // xorshift32, per channel
    float unitRandom (int channelIndex) noexcept;           // [0,1) with 24-bit mantissa

    // The signalsmith RealFFT constructor requires the size up front; prepare()
    // re-asserts it via setSize (a no-op for the same size).
    signalsmith::RealFFT<float> fft { (size_t) fftSize };

    std::vector<float> window;                            // 2048 Hann table (shared across channels)
    std::array<std::vector<float>, maxChannels> inputRing;      // 2048 each
    std::array<std::vector<float>, maxChannels> ola;            // 3584 each
    std::array<std::vector<float>, maxChannels> timeBuffer;     // 2048 each
    std::array<std::vector<std::complex<float>>, maxChannels> spectrum; // 1024 each
    std::array<std::vector<float>, maxChannels> outputRing;     // 2048 each (4 hop-sized chunks)

    // --- Mask state (per channel, per bin) ---
    std::array<std::vector<float>, maxChannels> targetMask;     // 1025 each, {0..1} (randomized band; hard-muted region fixed at 1)
    std::array<std::vector<float>, maxChannels> currentGain;    // 1025 each, slew-smoothed
    std::array<std::vector<float>, maxChannels> controlTarget;  // <= 1024 each — live control values (the random-walk state)

    // --- Visualization (post-mask spectrum; optional snapshot sink) ---
    std::array<std::vector<float>, maxChannels> visMagnitude;  // 1025 each — captured in analyseChannel, bins 1..1023
    std::shared_ptr<SpectrumSnapshot> spectrumSnapshot;        // UI sink (null = no publishing)
    std::int64_t snapshotCount = 0;                            // publish counter (monotonic, never reset)

    // --- RNG streams (independent per channel) ---
    std::array<std::uint32_t, maxChannels> rngState { 0x9E3779B9u, 0x41C64E6Du };

    // --- Parameters (audio thread, pushed per block) ---
    float amount = 0.5f;          // selection probability 0..1
    float widthSeconds = 0.09f;   // mask dwell time
    float cutoff = 1.0f;          // randomized band size (0 = 16 bins + rest muted, 1 = all bins)
    float phaseRadians = 0.0f;    // masking-character angle
    float stereo = 0.7f;          // channel decoupling 0..1
    float driftStep = 0.4f;       // per-tick control walk step (scaled by amount*(1-amount))

    int gridSize = 8;             // control-grid spacing (bins); 1 = per-bin dice
    int cutoffEnd = 1024;         // randomized band end + 1 (from cutoff, recomputed on push); at/above = hard-muted
    int numControls = 0;          // control slots in use: (cutoffEnd - 1 + gridSize - 1) / gridSize
    float alpha = 0.1f;           // per-frame slew coefficient (sample-rate matched)

    double frameTime = 0.0;       // seconds since the last mask refresh
    double framePeriod = 0.0;     // hopSize / sampleRate

    int ringWrite = 0;            // next input-ring write index
    int samplesInRing = 0;        // unconsumed samples in the input ring
    int olaWritePos = 0;          // next OLA add/drain position (advances by hop per frame)
    std::int64_t frameIndex = 0;  // number of frames analyzed (hop ticks)
    std::int64_t outPos = 0;      // absolute output-stream position of the next block
    int numChannels = 0;
    double sampleRate = 0.0;
};

} // namespace Snow::DSP
