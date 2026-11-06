#include "Filter.h"

#include <algorithm>
#include <cmath>

namespace synth
{
namespace
{
#if defined(__clang__) || defined(__GNUC__)
#define SYNTHAX1_ALWAYS_INLINE inline __attribute__((always_inline))
#else
#define SYNTHAX1_ALWAYS_INLINE inline
#endif

SYNTHAX1_ALWAYS_INLINE float softClip(float value) noexcept
{
    if (value < -64.0f)
        value = -64.0f;
    else if (value > 64.0f)
        value = 64.0f;

    return value >= 0.0f ? value / (1.0f + value)
                         : value / (1.0f - value);
}

// softClip(softClip(x)) collapses algebraically to clamp(x)/(1 + 2*|clamp(x)|):
// softClip is sign-preserving and maps into (-1, 1), so the outer clamp never
// engages and the two divisions fuse into one.
SYNTHAX1_ALWAYS_INLINE float doubleSoftClip(float value) noexcept
{
    if (value < -64.0f)
        value = -64.0f;
    else if (value > 64.0f)
        value = 64.0f;

    return value >= 0.0f ? value / (1.0f + 2.0f * value)
                         : value / (1.0f - 2.0f * value);
}

SYNTHAX1_ALWAYS_INLINE float clampKnownFinite(float value, float minimum, float maximum) noexcept
{
    if (value < minimum)
        return minimum;

    if (value > maximum)
        return maximum;

    return value;
}

float clampFast(float value, float minimum, float maximum) noexcept
{
    if (!std::isfinite(value))
        return minimum;

    if (value < minimum)
        return minimum;

    if (value > maximum)
        return maximum;

    return value;
}

SYNTHAX1_ALWAYS_INLINE float flushTiny(float value) noexcept
{
    return value > -1.0e-20f && value < 1.0e-20f ? 0.0f : value;
}

SYNTHAX1_ALWAYS_INLINE bool isTiny(float value) noexcept
{
    return value > -1.0e-15f && value < 1.0e-15f;
}

// Coefficient of one topology-preserving (zero-delay-feedback) one-pole stage:
// G = g / (1 + g) with the bilinear-prewarped g = tan(pi fc / fs), so the stage's -3 dB
// point and the ladder's resonance sit exactly at the requested cutoff up to Nyquist.
// G depends only on fc / fs, so it is tabulated once over [0, 0.5) and interpolated: a
// modulated cutoff then needs no tan per sample. The table error is below 3e-6 relative above
// fs / 1000 and 6e-5 at the 8 Hz floor, at most 0.001 semitone of cutoff.
constexpr int ladderTableSize = 8192; // steps over fc / fs in [0, 0.5]

const std::array<float, ladderTableSize + 2> ladderTable = [] {
    std::array<float, ladderTableSize + 2> table {};
    for (int i = 0; i < ladderTableSize + 2; ++i)
    {
        const auto ratio = std::min(0.4999, 0.5 * static_cast<double>(i) / ladderTableSize);
        const auto g = std::tan(3.14159265358979323846 * ratio);
        table[static_cast<std::size_t>(i)] = static_cast<float>(g / (1.0 + g));
    }
    return table;
}();

float ladderCoefficient(float cutoffHz, float sampleRate) noexcept
{
    const auto position = std::clamp(cutoffHz / sampleRate, 0.0f, 0.4999f) * (2.0f * ladderTableSize);
    const auto index = static_cast<std::size_t>(position);
    const auto fraction = position - static_cast<float>(index);
    return ladderTable[index] + (ladderTable[index + 1] - ladderTable[index]) * fraction;
}

// MEASURED (the reference synth 3.0.7.3, sweeps/drive, 44 programs): drive is a pre-gain followed by a limiter that
// closes as drive rises. Fitted from two input levels at each of 11 drive settings, modelling the
// limiter as A tanh(G x / A). Gains in dB; limits in units of a full-scale saw's peak (the filter
// input of a 1-voice oscillator at full level is 0.7 in Synthax1, hence the scaling at use).
// MEASURED (the reference synth 3.0.7.3, sweeps/keytrack): the slope is one semitone per semitone at full, but the
// reference note is low: at middle C the cutoff already sits 50.6 semitones above the knob's zero value
// (fits within 1.5 semitones at every knob position), so the reference is MIDI note 9.4, not 60.
constexpr float keytrackReferenceNote = 9.4f;

constexpr std::array<float, 11> driveGainDb { 0.0f, 5.9f, 8.9f, 10.3f, 11.1f, 11.6f, 12.0f, 12.4f, 12.8f, 13.2f, 13.6f };
constexpr std::array<float, 11> driveLimit { 1.0e4f, 1.0e4f, 1.0e4f, 24.4f, 17.6f, 11.1f, 5.85f, 3.62f, 2.69f, 2.14f, 1.79f };
constexpr float filterInputPeak = 0.7f;

float tableAt(const std::array<float, 11>& table, float amount) noexcept
{
    const auto position = amount * 10.0f;
    const auto index = std::min<std::size_t>(9, static_cast<std::size_t>(position));
    return table[index] + (table[index + 1] - table[index]) * (position - static_cast<float>(index));
}

float driveGainFor(float amount) noexcept { return std::pow(10.0f, tableAt(driveGainDb, amount) * 0.05f); }

// MEASURED (the reference synth 3.0.7.3, sweeps/drive2, sine waveforms): the drive stage is not a saturator. The output
// follows the input up to a knee and then falls again (a sine at full drive and level comes out with a dip
// at its crest), the knee moving to lower levels as drive rises. Modelled as x / (1 + (x/K)^4); K in the
// filter input's units after the drive gain, interpolated on the log between these drive settings.
float driveLimitFor(float amount) noexcept
{
    constexpr std::array<float, 5> kneeAt { 60.0f, 6.5f, 3.9f, 2.74f, 2.24f };
    const auto position = std::clamp(amount, 0.0f, 1.0f) * 4.0f;
    const auto index = std::min<std::size_t>(3, static_cast<std::size_t>(position));
    const auto fraction = position - static_cast<float>(index);
    constexpr float kneeScale = 0.78f;
    return kneeScale * std::exp(std::log(kneeAt[index]) + (std::log(kneeAt[index + 1]) - std::log(kneeAt[index])) * fraction);
}

// MEASURED (the reference synth 3.0.7.3, sweeps/hpresp): the highpass keeps a flat bass floor instead of rolling off
// to nothing, about -25 dB under the passband and rising to -17 dB (24 dB slope) or -12 dB (12 dB slope) at
// the highest cutoffs. Gain against cutoff in semitones.
float highpassLeakFor(float cutoffSemitones, bool steep) noexcept
{
    struct Node { float semitones; float db; };
    constexpr std::array<Node, 7> steepNodes { { { 40.0f, -60.0f }, { 55.0f, -27.0f }, { 72.6f, -25.0f }, { 89.8f, -24.0f }, { 107.2f, -20.4f }, { 125.6f, -17.3f }, { 134.6f, -22.5f } } };
    constexpr std::array<Node, 7> gentleNodes { { { 40.0f, -60.0f }, { 55.0f, -32.0f }, { 72.6f, -26.0f }, { 89.8f, -25.0f }, { 107.2f, -25.0f }, { 125.6f, -20.4f }, { 134.6f, -12.4f } } };
    const auto& nodes = steep ? steepNodes : gentleNodes;
    if (cutoffSemitones <= nodes.front().semitones)
        return 0.0f;
    for (std::size_t i = 1; i < nodes.size(); ++i)
        if (cutoffSemitones <= nodes[i].semitones)
        {
            const auto t = (cutoffSemitones - nodes[i - 1].semitones) / (nodes[i].semitones - nodes[i - 1].semitones);
            return std::pow(10.0f, (nodes[i - 1].db + (nodes[i].db - nodes[i - 1].db) * t) * 0.05f);
        }
    return std::pow(10.0f, nodes.back().db * 0.05f);
}

// tanh via a rational approximation, exact enough for a saturator (error < 2% to |x| = 3, clamped beyond).
SYNTHAX1_ALWAYS_INLINE float fastTanh(float x) noexcept
{
    x = x < -3.0f ? -3.0f : (x > 3.0f ? 3.0f : x);
    const auto squared = x * x;
    return x * (27.0f + squared) / (27.0f + 9.0f * squared);
}
} // namespace

void Filter::prepare(double newSampleRate) noexcept
{
    sampleRate = std::isfinite(newSampleRate) && newSampleRate > 0.0 ? newSampleRate : 44100.0;
    reset();
}

void Filter::reset() noexcept
{
    stage.fill(0.0f);
    clippedStage.fill(0.0f);
    ladderInput = 0.0f;
    previousInput = 0.0f;
    cachedCutoffSemitones = -1.0f;
    cachedCoefficientMode = -1;
    cachedEffectiveSampleRate = 0.0f;
    cachedCoefficient = 0.0f;
    cachedDriveAmount = -1.0f;
    cachedResonance = -1.0f;
    cachedDriveGain = 1.0f;
    cachedDriveLimit = 1.0e4f;
    cachedFeedback = 0.0f;
}

float Filter::process(float input, float midiNote, const SynthParameters& parameters,
                      float cutoffModSemitones) noexcept
{
    const auto& filter = parameters.filter;
    if (!filter.enabled)
        return input;

    const auto keytrack = clampKnownFinite(filter.keytrack, -1.0f, 2.0f);
    const auto keytrackSemitones = (midiNote - keytrackReferenceNote) * keytrack;
    const auto cutoffSemitones = clampKnownFinite(filter.cutoffSemitones + keytrackSemitones + cutoffModSemitones,
                                                  0.0f, 136.0f);
    const auto oversampling = oversamplingFactor(filter.oversampling);
    const auto effectiveSampleRate = static_cast<float>(sampleRate) * static_cast<float>(oversampling);
    const auto driveAmount = clampKnownFinite(filter.drive, 0.0f, 1.0f);
    const auto resonance = clampKnownFinite(filter.resonance, 0.0f, 1.0f);
    const auto useL4FastPath = filter.mode == FilterMode::L4;

    if (std::abs(cachedCutoffSemitones - cutoffSemitones) > 0.000001f
        || std::abs(cachedEffectiveSampleRate - effectiveSampleRate) > 0.001f
        || cachedCoefficientMode != static_cast<int>(filter.mode))
    {
        // MEASURED (sweeps/hpresp): the reference synth's highpass corner stops rising near 6 kHz however far the knob goes.
        const auto highpass = filter.mode == FilterMode::H2 || filter.mode == FilterMode::H4;
        const auto cutoffHz = clampFast(cutoffSemitonesToHz(highpass ? std::min(cutoffSemitones, 113.0f) : cutoffSemitones), 8.0f, 0.45f * effectiveSampleRate);
        const auto safeSampleRate = effectiveSampleRate > 1000.0f ? effectiveSampleRate : 1000.0f;
        cachedCoefficient = clampFast(ladderCoefficient(cutoffHz, safeSampleRate), 0.00001f, 0.99f);
        cachedCutoffSemitones = cutoffSemitones;
        cachedHighpassLeak = highpass ? highpassLeakFor(cutoffSemitones, filter.mode == FilterMode::H4) : 0.0f;
        cachedEffectiveSampleRate = effectiveSampleRate;
        cachedCoefficientMode = static_cast<int>(filter.mode);
    }

    if (std::abs(cachedDriveAmount - driveAmount) > 0.000001f
        || std::abs(cachedResonance - resonance) > 0.000001f)
    {
        cachedDriveGain = driveGainFor(driveAmount);
        cachedDriveLimit = driveLimitFor(driveAmount);
        cachedFeedback = resonance * (3.85f / (1.0f + driveAmount * 1.35f));
        cachedDriveAmount = driveAmount;
        cachedResonance = resonance;
    }

    if (oversampling <= 1)
    {
        previousInput = input;
        const auto value = useL4FastPath
            ? processCoreL4(input, cachedCoefficient, cachedFeedback, cachedDriveGain)
            : processCore(input, cachedCoefficient, cachedFeedback, cachedDriveGain, filter.mode);
        return std::isfinite(value) ? value : 0.0f;
    }

    auto value = 0.0f;
    for (int i = 0; i < oversampling; ++i)
    {
        const auto t = static_cast<float>(i + 1) / static_cast<float>(oversampling);
        const auto subInput = previousInput + (input - previousInput) * t;
        value += useL4FastPath
            ? processCoreL4(subInput, cachedCoefficient, cachedFeedback, cachedDriveGain)
            : processCore(subInput, cachedCoefficient, cachedFeedback, cachedDriveGain, filter.mode);
    }
    previousInput = input;
    value /= static_cast<float>(oversampling);

    return std::isfinite(value) ? value : 0.0f;
}

// Per-block snapshot of every parameter-derived value that is constant within a
// host block. processPrepared() then only re-derives the cutoff-dependent
// coefficient, which can change per sample under modulation.
void Filter::prepareBlock(const SynthParameters& parameters) noexcept
{
    prepareBlock(parameters.filter);
}

void Filter::prepareBlock(const FilterParameters& filter) noexcept
{
    preparedEnabled = filter.enabled;
    preparedWarmDrive = filter.warmDrive;
    preparedNativeTopology = filter.nativeTopology;
    if (!preparedEnabled)
        return;

    preparedKeytrack = clampKnownFinite(filter.keytrack, -1.0f, 2.0f);
    preparedOversampling = oversamplingFactor(filter.oversampling);
    preparedEffectiveSampleRate = static_cast<float>(sampleRate) * static_cast<float>(preparedOversampling);
    preparedMode = filter.mode;
    preparedUseL4 = filter.mode == FilterMode::L4;

    const auto driveAmount = clampKnownFinite(filter.drive, 0.0f, 1.0f);
    const auto resonance = clampKnownFinite(filter.resonance, 0.0f, 1.0f);
    if (std::abs(cachedDriveAmount - driveAmount) > 0.000001f
        || std::abs(cachedResonance - resonance) > 0.000001f
        || cachedSelfOscillation != filter.selfOscillation)
    {
        cachedDriveGain = driveGainFor(driveAmount);
        cachedDriveLimit = driveLimitFor(driveAmount);
        cachedFeedback = resonance * ((filter.selfOscillation ? 4.35f : 3.85f) / (1.0f + driveAmount * 1.35f));
        cachedDriveAmount = driveAmount;
        cachedResonance = resonance;
        cachedSelfOscillation = filter.selfOscillation;
        if (filter.selfOscillation && resonance > 0.95f && isTiny(stage[0]) && isTiny(stage[3]))
            stage[0] = 0.00001f;
    }
}

float Filter::processPrepared(float input, float midiNote, const SynthParameters& parameters,
                              float cutoffModSemitones) noexcept
{
    return processPrepared(input, midiNote, parameters.filter, cutoffModSemitones);
}

float Filter::processPrepared(float input, float midiNote, const FilterParameters& parameters,
                              float cutoffModSemitones) noexcept
{
    if (!preparedEnabled)
        return input;

    const auto keytrackSemitones = (midiNote - keytrackReferenceNote) * preparedKeytrack;
    const auto cutoffSemitones = clampKnownFinite(parameters.cutoffSemitones + keytrackSemitones
                                                      + cutoffModSemitones,
                                                  0.0f, 136.0f);
    if (std::abs(cachedCutoffSemitones - cutoffSemitones) > 0.000001f
        || std::abs(cachedEffectiveSampleRate - preparedEffectiveSampleRate) > 0.001f
        || cachedCoefficientMode != static_cast<int>(preparedMode))
    {
        const auto highpass = preparedMode == FilterMode::H2 || preparedMode == FilterMode::H4;
        const auto cutoffHz = clampFast(cutoffSemitonesToHz(highpass ? std::min(cutoffSemitones, 113.0f) : cutoffSemitones), 8.0f,
                                        0.45f * preparedEffectiveSampleRate);
        const auto safeSampleRate = preparedEffectiveSampleRate > 1000.0f ? preparedEffectiveSampleRate : 1000.0f;
        cachedCoefficient = clampFast(ladderCoefficient(cutoffHz, safeSampleRate), 0.00001f, 0.99f);
        cachedCutoffSemitones = cutoffSemitones;
        // The leak only feeds the highpass outputs, and its pow is skipped for every other mode.
        cachedHighpassLeak = highpass ? highpassLeakFor(cutoffSemitones, preparedMode == FilterMode::H4) : 0.0f;
        cachedEffectiveSampleRate = preparedEffectiveSampleRate;
        cachedCoefficientMode = static_cast<int>(preparedMode);
    }

    if (preparedOversampling <= 1)
    {
        previousInput = input;
        const auto value = preparedUseL4
            ? processCoreL4(input, cachedCoefficient, cachedFeedback, cachedDriveGain)
            : processCore(input, cachedCoefficient, cachedFeedback, cachedDriveGain, preparedMode);
        return std::isfinite(value) ? value : 0.0f;
    }

    auto value = 0.0f;
    for (int i = 0; i < preparedOversampling; ++i)
    {
        const auto t = static_cast<float>(i + 1) / static_cast<float>(preparedOversampling);
        const auto subInput = previousInput + (input - previousInput) * t;
        value += preparedUseL4
            ? processCoreL4(subInput, cachedCoefficient, cachedFeedback, cachedDriveGain)
            : processCore(subInput, cachedCoefficient, cachedFeedback, cachedDriveGain, preparedMode);
    }
    previousInput = input;
    value /= static_cast<float>(preparedOversampling);

    return std::isfinite(value) ? value : 0.0f;
}

// Block variant: identical per-sample calls so results stay bit-exact with the
// scalar prepared entry; batching keeps filter state hot across the loop.
void Filter::processPreparedBlock(float* samples, const float* midiNote, const SynthParameters& parameters,
                                  const float* cutoffModSemitones, int numSamples) noexcept
{
    if (!preparedEnabled)
        return;

    for (int i = 0; i < numSamples; ++i)
        samples[i] = processPrepared(samples[i], midiNote[i], parameters, cutoffModSemitones[i]);
}

float Filter::cutoffSemitonesToHz(float semitones) noexcept
{
    return semitonesToHz(clampFast(semitones, 0.0f, 136.0f));
}

// Four cascaded topology-preserving one-pole lowpass stages with global feedback, solved
// without a unit delay (Zavalishin, "The Art of VA Filter Design", ch. 5). Each stage is
//   v = (x - s) G;  y = v + s;  s' = y + v,   so  y = G x + s (1 - G).
// The ladder output is then y4 = G^4 u + sigma, with sigma from the current states, and the
// feedback equation u = in - k y4 is solved for u before the stages run. The input
// saturation keeps self-oscillation bounded.
void Filter::processStages(float input, float coefficient, float feedback, float driveGain) noexcept
{
    const auto G = coefficient;
    const auto hold = 1.0f - G; // 1 / (1 + g)
    const auto sigma = G * G * G * stage[0] * hold + G * G * stage[1] * hold + G * stage[2] * hold + stage[3] * hold;
    const auto g4 = G * G * G * G;
    const auto linear = (input * driveGain - feedback * sigma) / (1.0f + feedback * g4);
    // The limiter closes with drive; at low drive it is far above the signal and this is linear.
    // A wide safety limit keeps self-oscillation bounded; the drive knee shapes the rest.
    const auto relative = linear / cachedDriveLimit;
    const auto relativeSquared = relative * relative;
    ladderInput = 5.0f * fastTanh(linear * 0.2f) / (1.0f + relativeSquared * relativeSquared);

    auto x = ladderInput;
    for (int i = 0; i < 4; ++i)
    {
        const auto v = (x - stage[i]) * G;
        const auto y = v + stage[i];
        stage[i] = flushTiny(y + v);
        clippedStage[i] = y;
        x = y;
    }
}

SYNTHAX1_ALWAYS_INLINE float Filter::processCoreL4(float input, float coefficient, float feedback, float driveGain) noexcept
{
    processStages(input, coefficient, feedback, driveGain);
    return clippedStage[3];
}

// The reference synth's bandpass measured 3-4 dB quieter at its centre than this ladder tap (docs/SOUND_PARITY.md).
constexpr float bandpassTrim = 0.66f;

SYNTHAX1_ALWAYS_INLINE float Filter::processCore(float input, float coefficient, float feedback, float driveGain,
                                                FilterMode mode) noexcept
{
    processStages(input, coefficient, feedback, driveGain);
    const auto& y = clippedStage;
    const auto u = ladderInput;

    switch (mode)
    {
        case FilterMode::L2:
            return y[1];
        case FilterMode::L4:
            return y[3];
        case FilterMode::B2:
            return (y[0] - y[1]) * (preparedNativeTopology ? 2.0f * bandpassTrim : 2.0f);
        case FilterMode::B4:
            return preparedNativeTopology ? (y[1] - 2.0f * y[2] + y[3]) * (4.0f * bandpassTrim) : (y[2] - y[3]) * 3.0f;
        case FilterMode::H2:
            return preparedNativeTopology ? u - 2.0f * y[0] + y[1] + cachedHighpassLeak * u : input - y[1];
        case FilterMode::H4:
            return preparedNativeTopology ? u - 4.0f * y[0] + 6.0f * y[1] - 4.0f * y[2] + y[3] + cachedHighpassLeak * u : input - y[3];
        case FilterMode::Peak2:
            return y[1] - (input - y[1]);
        case FilterMode::Notch2:
            return y[1] + (input - y[1]);
        case FilterMode::Notch4:
            return y[3] + (input - y[3]);
    }

    return y[3];
}
} // namespace synth
