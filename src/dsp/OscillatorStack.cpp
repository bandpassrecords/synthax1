#include "OscillatorStack.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <type_traits>

namespace synth
{
namespace
{
constexpr float pi = 3.14159265358979323846f;

#if defined(__clang__) || defined(__GNUC__)
#define SYNTHAX1_ALWAYS_INLINE inline __attribute__((always_inline))
#elif defined(_MSC_VER)
#define SYNTHAX1_ALWAYS_INLINE __forceinline
#else
#define SYNTHAX1_ALWAYS_INLINE inline
#endif

float wrapUnit(float value) noexcept
{
    value -= std::floor(value);
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

// Only for values proven finite by construction (e.g. derived from an
// already-clamped cached base frequency); skips the isfinite guard.
SYNTHAX1_ALWAYS_INLINE float clampKnownFinite(float value, float minimum, float maximum) noexcept
{
    if (value < minimum)
        return minimum;

    if (value > maximum)
        return maximum;

    return value;
}

float clampUnitFast(float value) noexcept
{
    return clampFast(value, 0.0f, 1.0f);
}

int clampIntFast(int value, int minimum, int maximum) noexcept
{
    if (value < minimum)
        return minimum;

    if (value > maximum)
        return maximum;

    return value;
}
} // namespace

void OscillatorStack::prepare(double newSampleRate) noexcept
{
    sampleRate = std::isfinite(newSampleRate) && newSampleRate > 0.0 ? newSampleRate : 44100.0;
    cachedBaseFrequencyValid = false;
    cachedNativeNote = -1.0e9f;
    reset(0.0f);
}

void OscillatorStack::reset(float randomOnNote) noexcept
{
    const auto basePhase = wrapUnit((randomOnNote + 1.0f) * 0.5f);
    resetState(basePhase);
}

void OscillatorStack::resetToPhase(float normalizedPhase) noexcept
{
    resetState(wrapUnit(normalizedPhase));
}

void OscillatorStack::resetState(float basePhase) noexcept
{
    for (int i = 0; i < maxStackCount; ++i)
        phases[static_cast<std::size_t>(i)] = wrapUnit(basePhase + 0.173f * static_cast<float>(i));

    subPhase = wrapUnit(basePhase * 0.5f);
    syncMasterPhase = basePhase;
    noiseState = 0x76543210u ^ static_cast<unsigned int>((basePhase * 16777215.0f) + 1.0f);
    cachedBaseFrequencyValid = false;
    cachedDetunedIncrementStackCount = -1;
    cachedDetunedMasterIncrement = -1.0f;
}

void OscillatorStack::resetNativePhase(float normalizedPhase) noexcept
{
    resetState(wrapUnit(std::isfinite(normalizedPhase) ? normalizedPhase : 0.0f));
    phases.fill(wrapUnit(std::isfinite(normalizedPhase) ? normalizedPhase : 0.0f));
}

namespace
{
// The native triangle: a rising and falling ramp with its two corners smoothed over one sample
// period, so the edges alias less.
SYNTHAX1_ALWAYS_INLINE float nativeTriangle(float position, float increment) noexcept
{
    const auto correction = [increment](float value) noexcept {
        if (value >= 1.0f) value -= 1.0f;
        const auto distance = std::min(value, 1.0f - value);
        if (distance >= increment)
            return 0.0f;
        const auto remainder = 1.0f - distance / increment;
        return increment * remainder * remainder * remainder / 3.0f;
    };
    return 1.0f - 4.0f * std::abs(position - 0.5f)
        + 4.0f * correction(position) - 4.0f * correction(position + 0.5f);
}

// MEASURED (the reference synth 3.0.7.3, sweeps/unison): n coherent voices add up to an amplitude of
// 0.8 + 0.2 n, so each voice carries (0.8 + 0.2 n) / n.
SYNTHAX1_ALWAYS_INLINE float nativeVoiceGain(float level, int count) noexcept
{
    return 0.7f * clampFast(level, 0.0f, 4.0f)
         * (0.8f + 0.2f * static_cast<float>(count)) / static_cast<float>(count);
}
} // namespace

// One native waveform value at `phase`, before the waveform's start shift.
template <OscillatorSlotWaveform waveform>
SYNTHAX1_ALWAYS_INLINE float OscillatorStack::nativeWave(float phase, float increment) noexcept
{
    // MEASURED (the reference synth 3.0.7.3, sweeps/startwave): where each waveform starts when a voice retriggers
    // at Phase 0, as a shift of the cycle (saw half a cycle, triangle a quarter, trisaw 0.09). The
    // pulse waves start inverted. Only matters when oscillators are layered with Retrig on.
    if constexpr (waveform == OscillatorSlotWaveform::Saw)
    {
        phase = wrapUnit(phase + 0.5f);
        return renderSaw(phase, increment);
    }
    else if constexpr (waveform == OscillatorSlotWaveform::Pulse)
        return -renderPulse(phase, increment, 0.5f);
    else if constexpr (waveform == OscillatorSlotWaveform::HalfPulse)
        return -(renderPulse(phase, increment, 0.25f) + 0.5f);
    else if constexpr (waveform == OscillatorSlotWaveform::QuarterPulse)
        return -(renderPulse(phase, increment, 0.125f) + 0.75f);
    // MEASURED (the reference synth 3.0.7.3): its noise oscillator is 10.2 dB above the same-level saw,
    // white and identical in both channels.
    else if constexpr (waveform == OscillatorSlotWaveform::Noise)
        return renderNoise() * 3.24f;
    else if constexpr (waveform == OscillatorSlotWaveform::Sine)
        return std::sin(2.0f * pi * phase);
    else if constexpr (waveform == OscillatorSlotWaveform::Triangle)
        return nativeTriangle(wrapUnit(phase + 0.25f), increment);
    else
    {
        // MEASURED (the reference synth 3.0.7.3, sweeps/wavespec): odd harmonics only, falling as 0.85/n after a
        // stronger fundamental, which is a square wave plus a little sine; level matched to the
        // fundamental of the reference synth's render.
        phase = wrapUnit(phase + 0.09f);
        return 0.496f * renderPulse(phase, increment, 0.5f) + 0.110f * std::sin(2.0f * pi * phase);
    }
}

float OscillatorStack::nativeWaveValue(OscillatorSlotWaveform waveform, float phase, float increment) noexcept
{
    using Waveform = OscillatorSlotWaveform;
    switch (waveform)
    {
        case Waveform::Saw: return nativeWave<Waveform::Saw>(phase, increment);
        case Waveform::Pulse: return nativeWave<Waveform::Pulse>(phase, increment);
        case Waveform::HalfPulse: return nativeWave<Waveform::HalfPulse>(phase, increment);
        case Waveform::QuarterPulse: return nativeWave<Waveform::QuarterPulse>(phase, increment);
        case Waveform::Noise: return nativeWave<Waveform::Noise>(phase, increment);
        case Waveform::Sine: return nativeWave<Waveform::Sine>(phase, increment);
        case Waveform::Triangle: return nativeWave<Waveform::Triangle>(phase, increment);
        case Waveform::SawTriangle: return nativeWave<Waveform::SawTriangle>(phase, increment);
    }
    return 0.0f;
}

void OscillatorStack::updateNativePan(int count, float pan, float spread) noexcept
{
    if (cachedNativePanCount == count && cachedNativePan == pan && cachedNativeStereo == spread)
        return;

    cachedNativePanCount = count;
    cachedNativePan = pan;
    cachedNativeStereo = spread;
    // MEASURED (the reference synth 3.0.7.3, sweeps/unipan, voice counts 2-8): each unison voice is panned hard to
    // one side by the amount of the Stereo knob, not spread smoothly. Mirror voices (about the
    // centre) take opposite sides; the outside-in side sequence is L R R L (Thue-Morse) for an even
    // count and L R L for an odd one, and the middle voice of an odd count stays centred. The gains
    // are linear: left a(1 - p), right a(1 + p) for a voice at pan p, with a the centred level.
    for (int index = 0; index < count; ++index)
    {
        const auto mirrored = count - 1 - index;
        const auto outside = std::min(index, mirrored);
        const auto onLeft = (count % 2 == 0 ? (std::popcount(static_cast<unsigned>(outside)) & 1) == 0
                                            : (outside & 1) == 0)
                                == (index <= mirrored);
        const auto voicePan = index == mirrored ? 0.0f : (onLeft ? -spread : spread);
        // MEASURED (sweeps/panlaw): the oscillator Pan knob is linear too, a hard pan sitting 6 dB above the centre
        // (a constant-power law gives 3 dB).
        const auto clampedPan = clampFast(pan, -1.0f, 1.0f);
        const auto baseLeft = 0.70710678f * (1.0f - clampedPan);
        const auto baseRight = 0.70710678f * (1.0f + clampedPan);
        nativePanLeft[static_cast<std::size_t>(index)] = baseLeft * (1.0f - voicePan);
        nativePanRight[static_cast<std::size_t>(index)] = baseRight * (1.0f + voicePan);
    }
}

OscillatorStack::StereoSample OscillatorStack::renderNative(float midiNote,
                                                            const LayerOscillatorParameters& parameters, float pitchModSemitones, float phaseModDegrees) noexcept
{
    StereoSample output;
    const auto count = clampIntFast(parameters.voices, 0, maxStackCount);
    if (count == 0)
        return output;

    const auto rate = static_cast<float>(sampleRate);
    const auto note = midiNote + static_cast<float>(parameters.octave * 12 + parameters.note)
        + parameters.fineCents * 0.01f + pitchModSemitones;
    if (note != cachedNativeNote)
    {
        cachedNativeNote = note;
        cachedNativeFrequency = clampFast(midiNoteToHz(note), 1.0f, rate * 0.45f);
    }
    const auto frequency = cachedNativeFrequency;
    updateDetuneRatios(count, clampUnitFast(parameters.detune));
    const auto gain = nativeVoiceGain(parameters.level, count);
    const auto phaseOffset = clampFast(phaseModDegrees, -720.0f, 720.0f) / 360.0f;
    updateNativePan(count, parameters.pan, clampUnitFast(parameters.stereo));
    for (int index = 0; index < count; ++index)
    {
        auto& storedPhase = phases[static_cast<std::size_t>(index)];
        const auto phase = phaseOffset == 0.0f ? storedPhase : wrapUnit(storedPhase + phaseOffset);
        const auto increment = clampFast(frequency / rate * detuneRatios[static_cast<std::size_t>(index)],
                                         1.0f / rate, 0.45f);
        const auto value = nativeWaveValue(parameters.waveform, phase, increment);
        advancePhase(storedPhase, increment);
        const auto sample = value * gain * (parameters.invert ? -1.0f : 1.0f);
        output.left += sample * nativePanLeft[static_cast<std::size_t>(index)];
        output.right += sample * nativePanRight[static_cast<std::size_t>(index)];
    }
    output.left = std::isfinite(output.left) ? output.left : 0.0f;
    output.right = std::isfinite(output.right) ? output.right : 0.0f;
    return output;
}

void OscillatorStack::renderNativeBlock(const float* midiNote, const LayerOscillatorParameters& parameters,
                                        const float* pitchModSemitones, const float* level, const float* pan,
                                        const float* detune, const float* phaseModDegrees,
                                        bool panAndDetuneConstant, int numSamples, float* left,
                                        float* right) noexcept
{
    numSamples = clampIntFast(numSamples, 0, renderBlockMaxSamples);
    const auto count = clampIntFast(parameters.voices, 0, maxStackCount);
    if (count == 0)
    {
        std::fill(left, left + numSamples, 0.0f);
        std::fill(right, right + numSamples, 0.0f);
        return;
    }

    // A moving pan or detune rebuilds the per-voice tables, so it keeps the sample-by-sample order.
    if (!panAndDetuneConstant)
    {
        auto slot = parameters;
        for (int i = 0; i < numSamples; ++i)
        {
            slot.level = level[i];
            slot.pan = pan[i];
            slot.detune = detune[i];
            const auto frame = renderNative(midiNote[i], slot, pitchModSemitones[i], phaseModDegrees[i]);
            left[i] = frame.left;
            right[i] = frame.right;
        }
        return;
    }

    const auto rate = static_cast<float>(sampleRate);
    const auto pitchOffset = static_cast<float>(parameters.octave * 12 + parameters.note);
    const auto fine = parameters.fineCents * 0.01f;
    std::array<float, renderBlockMaxSamples> frequencyOverRate;
    std::array<float, renderBlockMaxSamples> gain;
    std::array<float, renderBlockMaxSamples> phaseOffset;
    auto anyPhaseOffset = false;
    for (int i = 0; i < numSamples; ++i)
    {
        const auto index = static_cast<std::size_t>(i);
        const auto note = midiNote[i] + pitchOffset + fine + pitchModSemitones[i];
        if (note != cachedNativeNote)
        {
            cachedNativeNote = note;
            cachedNativeFrequency = clampFast(midiNoteToHz(note), 1.0f, rate * 0.45f);
        }
        frequencyOverRate[index] = cachedNativeFrequency / rate;
        gain[index] = nativeVoiceGain(level[i], count);
        phaseOffset[index] = clampFast(phaseModDegrees[i], -720.0f, 720.0f) / 360.0f;
        anyPhaseOffset = anyPhaseOffset || phaseOffset[index] != 0.0f;
    }
    updateDetuneRatios(count, clampUnitFast(detune[0]));
    updateNativePan(count, pan[0], clampUnitFast(parameters.stereo));

    // Noise comes from one generator that the voices share, drawn sample by sample in voice order.
    std::array<float, renderBlockMaxSamples * maxStackCount> noiseValues;
    if (parameters.waveform == OscillatorSlotWaveform::Noise)
        for (int i = 0; i < numSamples * count; ++i)
            noiseValues[static_cast<std::size_t>(i)] = nativeWave<OscillatorSlotWaveform::Noise>(0.0f, 0.0f);

    std::fill(left, left + numSamples, 0.0f);
    std::fill(right, right + numSamples, 0.0f);
    const auto sign = parameters.invert ? -1.0f : 1.0f;
    const auto minimumIncrement = 1.0f / rate;
    // Each unison voice runs through the block in turn; per sample the voices still add in voice
    // order, so the sum is the same as renderNative's.
    const auto runVoices = [&](auto waveformTag) noexcept {
        constexpr auto waveform = decltype(waveformTag)::value;
        for (int voice = 0; voice < count; ++voice)
        {
            const auto voiceIndex = static_cast<std::size_t>(voice);
            auto storedPhase = phases[voiceIndex];
            const auto ratio = detuneRatios[voiceIndex];
            const auto panLeft = nativePanLeft[voiceIndex];
            const auto panRight = nativePanRight[voiceIndex];
            for (int i = 0; i < numSamples; ++i)
            {
                const auto index = static_cast<std::size_t>(i);
                const auto phase = !anyPhaseOffset || phaseOffset[index] == 0.0f
                    ? storedPhase
                    : wrapUnit(storedPhase + phaseOffset[index]);
                const auto increment = clampFast(frequencyOverRate[index] * ratio, minimumIncrement, 0.45f);
                auto value = 0.0f;
                if constexpr (waveform == OscillatorSlotWaveform::Noise)
                    value = noiseValues[index * static_cast<std::size_t>(count) + voiceIndex];
                else
                    value = nativeWave<waveform>(phase, increment);
                advancePhase(storedPhase, increment);
                const auto sample = value * gain[index] * sign;
                left[i] += sample * panLeft;
                right[i] += sample * panRight;
            }
            phases[voiceIndex] = storedPhase;
        }
    };
    using Waveform = OscillatorSlotWaveform;
    switch (parameters.waveform)
    {
        case Waveform::Saw: runVoices(std::integral_constant<Waveform, Waveform::Saw> {}); break;
        case Waveform::Pulse: runVoices(std::integral_constant<Waveform, Waveform::Pulse> {}); break;
        case Waveform::HalfPulse: runVoices(std::integral_constant<Waveform, Waveform::HalfPulse> {}); break;
        case Waveform::QuarterPulse: runVoices(std::integral_constant<Waveform, Waveform::QuarterPulse> {}); break;
        case Waveform::Sine: runVoices(std::integral_constant<Waveform, Waveform::Sine> {}); break;
        case Waveform::Triangle: runVoices(std::integral_constant<Waveform, Waveform::Triangle> {}); break;
        case Waveform::SawTriangle: runVoices(std::integral_constant<Waveform, Waveform::SawTriangle> {}); break;
        case Waveform::Noise: runVoices(std::integral_constant<Waveform, Waveform::Noise> {}); break;
    }
    for (int i = 0; i < numSamples; ++i)
    {
        left[i] = std::isfinite(left[i]) ? left[i] : 0.0f;
        right[i] = std::isfinite(right[i]) ? right[i] : 0.0f;
    }
}

float OscillatorStack::renderSample(float midiNote, const SynthParameters& parameters,
                                    float pitchModSemitones, float pulseWidthMod) noexcept
{
    return renderSample(midiNote, parameters.osc, pitchModSemitones, pulseWidthMod);
}

float OscillatorStack::renderSample(float midiNote, const OscillatorParameters& osc,
                                    float pitchModSemitones, float pulseWidthMod) noexcept
{
    const auto stackCount = clampIntFast(osc.stackCount, 1, maxStackCount);
    const auto sampleRateFloat = static_cast<float>(sampleRate);
    const auto maxOscFrequency = sampleRateFloat * 0.45f;
    const auto minOscIncrement = 1.0f / sampleRateFloat;
    const auto maxOscIncrement = maxOscFrequency / sampleRateFloat;
    const auto basePitch = midiNote
        + osc.pitchSemitones
        + osc.fineCents / 100.0f
        + pitchModSemitones;
    auto baseFrequency = cachedBaseFrequency;
    if (!cachedBaseFrequencyValid
        || !std::isfinite(basePitch)
        || std::abs(cachedBasePitch - basePitch) > 0.000001f
        || std::abs(cachedMaxFrequency - maxOscFrequency) > 0.001f)
    {
        baseFrequency = clampFast(midiNoteToHz(basePitch), 1.0f, maxOscFrequency);
        cachedBaseFrequency = baseFrequency;
        cachedBasePitch = basePitch;
        cachedMaxFrequency = maxOscFrequency;
        cachedBaseFrequencyValid = std::isfinite(basePitch);
    }
    const auto syncAmount = clampUnitFast(osc.syncAmount);
    const auto masterIncrement = clampKnownFinite(baseFrequency / sampleRateFloat, minOscIncrement, maxOscIncrement);
    const auto previousMaster = syncMasterPhase;
    advancePhase(syncMasterPhase, masterIncrement);
    const auto syncWrapped = syncAmount > 0.001f && syncMasterPhase < previousMaster;

    const auto stackDetune = clampUnitFast(osc.stackDetune);
    const auto sawLevel = clampUnitFast(osc.sawLevel);
    const auto pulseLevel = clampUnitFast(osc.pulseLevel);
    const auto subLevel = clampUnitFast(osc.subLevel);
    const auto noiseLevel = clampUnitFast(osc.noiseLevel);
    if (cachedDetuneStackCount != stackCount || std::abs(cachedDetuneAmount - stackDetune) > 0.000001f)
        updateDetuneRatios(stackCount, stackDetune);

    if (sawLevel > 0.0f && pulseLevel <= 0.0f && subLevel <= 0.0f
        && noiseLevel <= 0.0f && syncAmount <= 0.001f)
    {
        auto stackedSaw = 0.0f;
        for (int i = 0; i < stackCount; ++i)
        {
            const auto increment = clampFast(masterIncrement * detuneRatios[static_cast<std::size_t>(i)],
                                             minOscIncrement, maxOscIncrement);
            auto& phase = phases[static_cast<std::size_t>(i)];
            stackedSaw += renderSaw(phase, increment);
            advancePhase(phase, increment);
        }

        const auto compensation = 0.7f / std::max(1.0f, sawLevel);
        return clampFast(stackedSaw * inverseSqrtForCount(stackCount) * sawLevel * compensation,
                         -1.2f, 1.2f);
    }

    auto stacked = 0.0f;
    const auto syncSlaveRatio = 1.0f + syncAmount * 4.0f;
    const auto pulseWidth = clampFast(osc.pulseWidth + pulseWidthMod * 0.45f, 0.05f, 0.95f);

    for (int i = 0; i < stackCount; ++i)
    {
        if (syncWrapped)
            phases[static_cast<std::size_t>(i)] = syncMasterPhase;

        const auto increment = clampFast(masterIncrement * detuneRatios[static_cast<std::size_t>(i)] * syncSlaveRatio,
                                         minOscIncrement, maxOscIncrement);

        auto& phase = phases[static_cast<std::size_t>(i)];
        auto voice = 0.0f;
        if (sawLevel > 0.0f)
            voice += sawLevel * renderSaw(phase, increment);

        if (pulseLevel > 0.0f)
        {
            auto pulsePhase = phase;
            voice += pulseLevel * renderPulse(pulsePhase, increment, pulseWidth);
        }

        advancePhase(phase, increment);
        stacked += voice;
    }

    stacked *= inverseSqrtForCount(stackCount);

    auto sub = 0.0f;
    if (subLevel > 0.0f)
    {
        const auto octave = clampIntFast(osc.subOctave, 1, 3);
        const auto subIncrement = clampFast((baseFrequency / static_cast<float>(1 << octave)) / sampleRateFloat,
                                            0.0f, 0.49f);
        sub = subLevel * renderSub(subPhase, osc.subWave, osc.subPulseWidth);
        advancePhase(subPhase, subIncrement);
    }

    const auto noise = noiseLevel > 0.0f ? noiseLevel * renderNoise() : 0.0f;
    const auto levelSum = sawLevel + pulseLevel + subLevel + noiseLevel;
    const auto compensation = 0.7f / std::max(1.0f, levelSum);
    return clampFast((stacked + sub + noise) * compensation, -1.2f, 1.2f);
}

float OscillatorStack::renderSawStack(float midiNote, const OscillatorParameters& osc,
                                      float pitchModSemitones, float outputGain) noexcept
{
    const auto stackCount = clampIntFast(osc.stackCount, 1, maxStackCount);
    const auto sampleRateFloat = static_cast<float>(sampleRate);
    const auto maxOscFrequency = sampleRateFloat * 0.45f;
    const auto minOscIncrement = 1.0f / sampleRateFloat;
    const auto maxOscIncrement = maxOscFrequency / sampleRateFloat;
    const auto basePitch = midiNote
        + osc.pitchSemitones
        + osc.fineCents / 100.0f
        + pitchModSemitones;
    auto baseFrequency = cachedBaseFrequency;
    if (!cachedBaseFrequencyValid
        || !std::isfinite(basePitch)
        || std::abs(cachedBasePitch - basePitch) > 0.000001f
        || std::abs(cachedMaxFrequency - maxOscFrequency) > 0.001f)
    {
        baseFrequency = clampFast(midiNoteToHz(basePitch), 1.0f, maxOscFrequency);
        cachedBaseFrequency = baseFrequency;
        cachedBasePitch = basePitch;
        cachedMaxFrequency = maxOscFrequency;
        cachedBaseFrequencyValid = std::isfinite(basePitch);
    }

    const auto masterIncrement = clampKnownFinite(baseFrequency / sampleRateFloat, minOscIncrement, maxOscIncrement);
    advancePhase(syncMasterPhase, masterIncrement);

    const auto stackDetune = clampUnitFast(osc.stackDetune);
    if (cachedDetuneStackCount != stackCount || std::abs(cachedDetuneAmount - stackDetune) > 0.000001f)
        updateDetuneRatios(stackCount, stackDetune);
    if (cachedDetunedIncrementStackCount != stackCount
        || std::abs(cachedDetunedMasterIncrement - masterIncrement) > 0.000000001f)
    {
        updateDetunedIncrements(stackCount, masterIncrement, minOscIncrement, maxOscIncrement);
    }

    const auto renderDetunedSaw = [this](int index) noexcept {
        const auto arrayIndex = static_cast<std::size_t>(index);
        const auto increment = detunedIncrements[arrayIndex];
        auto& phase = phases[arrayIndex];
        const auto sample = renderSaw(phase, increment);
        advancePhase(phase, increment);
        return sample;
    };

    auto stackedSaw = 0.0f;
    switch (stackCount)
    {
        case 1:
            stackedSaw = renderDetunedSaw(0);
            break;
        case 2:
            stackedSaw = renderDetunedSaw(0) + renderDetunedSaw(1);
            break;
        case 3:
            stackedSaw = renderDetunedSaw(0) + renderDetunedSaw(1) + renderDetunedSaw(2);
            break;
        case 4:
            stackedSaw = renderDetunedSaw(0) + renderDetunedSaw(1) + renderDetunedSaw(2) + renderDetunedSaw(3);
            break;
        default:
            for (int i = 0; i < stackCount; ++i)
                stackedSaw += renderDetunedSaw(i);
            break;
    }

    return clampFast(stackedSaw * outputGain, -1.2f, 1.2f);
}

// Block variant of renderSawStack: identical per-sample math and cache logic so
// results stay bit-exact with the scalar entry; only the block-constant detune
// ratio check is hoisted, and phases stay register-resident across the loop.
void OscillatorStack::renderSawStackBlock(const float* midiNote, const float* pitchModSemitones,
                                          float analogPitchMod, const OscillatorParameters& osc,
                                          float outputGain, float* out, int numSamples) noexcept
{
    const auto stackCount = clampIntFast(osc.stackCount, 1, maxStackCount);
    const auto sampleRateFloat = static_cast<float>(sampleRate);
    const auto maxOscFrequency = sampleRateFloat * 0.45f;
    const auto minOscIncrement = 1.0f / sampleRateFloat;
    const auto maxOscIncrement = maxOscFrequency / sampleRateFloat;

    const auto stackDetune = clampUnitFast(osc.stackDetune);
    if (cachedDetuneStackCount != stackCount || std::abs(cachedDetuneAmount - stackDetune) > 0.000001f)
        updateDetuneRatios(stackCount, stackDetune);

    for (int sampleIndex = 0; sampleIndex < numSamples; ++sampleIndex)
    {
        const auto effectivePitchMod = pitchModSemitones[sampleIndex] + analogPitchMod;
        const auto basePitch = midiNote[sampleIndex]
            + osc.pitchSemitones
            + osc.fineCents / 100.0f
            + effectivePitchMod;
        auto baseFrequency = cachedBaseFrequency;
        if (!cachedBaseFrequencyValid
            || !std::isfinite(basePitch)
            || std::abs(cachedBasePitch - basePitch) > 0.000001f
            || std::abs(cachedMaxFrequency - maxOscFrequency) > 0.001f)
        {
            baseFrequency = clampFast(midiNoteToHz(basePitch), 1.0f, maxOscFrequency);
            cachedBaseFrequency = baseFrequency;
            cachedBasePitch = basePitch;
            cachedMaxFrequency = maxOscFrequency;
            cachedBaseFrequencyValid = std::isfinite(basePitch);
        }

        const auto masterIncrement = clampKnownFinite(baseFrequency / sampleRateFloat,
                                                      minOscIncrement, maxOscIncrement);
        advancePhase(syncMasterPhase, masterIncrement);

        if (cachedDetunedIncrementStackCount != stackCount
            || std::abs(cachedDetunedMasterIncrement - masterIncrement) > 0.000000001f)
        {
            updateDetunedIncrements(stackCount, masterIncrement, minOscIncrement, maxOscIncrement);
        }

        const auto renderDetunedSaw = [this](int index) noexcept {
            const auto arrayIndex = static_cast<std::size_t>(index);
            const auto increment = detunedIncrements[arrayIndex];
            auto& phase = phases[arrayIndex];
            const auto sample = renderSaw(phase, increment);
            advancePhase(phase, increment);
            return sample;
        };

        auto stackedSaw = 0.0f;
        switch (stackCount)
        {
            case 1:
                stackedSaw = renderDetunedSaw(0);
                break;
            case 2:
                stackedSaw = renderDetunedSaw(0) + renderDetunedSaw(1);
                break;
            case 3:
                stackedSaw = renderDetunedSaw(0) + renderDetunedSaw(1) + renderDetunedSaw(2);
                break;
            case 4:
                stackedSaw = renderDetunedSaw(0) + renderDetunedSaw(1) + renderDetunedSaw(2) + renderDetunedSaw(3);
                break;
            default:
                for (int i = 0; i < stackCount; ++i)
                    stackedSaw += renderDetunedSaw(i);
                break;
        }

        out[sampleIndex] = clampFast(stackedSaw * outputGain, -1.2f, 1.2f);
    }
}

SYNTHAX1_ALWAYS_INLINE float OscillatorStack::renderSaw(float& phase, float phaseIncrement) noexcept
{
    return 2.0f * phase - 1.0f - polyBlep(phase, phaseIncrement);
}

SYNTHAX1_ALWAYS_INLINE float OscillatorStack::renderPulse(float& phase, float phaseIncrement, float pulseWidth) noexcept
{
    auto value = phase < pulseWidth ? 1.0f : -1.0f;
    value += polyBlep(phase, phaseIncrement);
    auto fallingEdgePhase = phase - pulseWidth;
    if (fallingEdgePhase < 0.0f)
        fallingEdgePhase += 1.0f;
    value -= polyBlep(fallingEdgePhase, phaseIncrement);
    return value;
}

SYNTHAX1_ALWAYS_INLINE float OscillatorStack::renderSub(float phase, SubWave wave, float pulseWidth) const noexcept
{
    phase = wrapUnit(phase);
    switch (wave)
    {
        case SubWave::Sine:
            return std::sin(2.0f * pi * phase);
        case SubWave::Triangle:
            return 1.0f - 4.0f * std::abs(phase - 0.5f);
        case SubWave::Saw:
            return 2.0f * phase - 1.0f;
        case SubWave::Pulse:
            return phase < clampFast(pulseWidth, 0.05f, 0.95f) ? 1.0f : -1.0f;
    }

    return 0.0f;
}

float OscillatorStack::renderNoise() noexcept
{
    noiseState = noiseState * 1664525u + 1013904223u;
    return (static_cast<float>((noiseState >> 8) & 0x00ffffffu) / 8388607.5f) - 1.0f;
}

SYNTHAX1_ALWAYS_INLINE float OscillatorStack::advancePhase(float& phase, float increment) noexcept
{
    phase += increment;
    if (phase >= 1.0f)
        phase -= 1.0f;

    return phase;
}

SYNTHAX1_ALWAYS_INLINE float OscillatorStack::polyBlep(float phase, float phaseIncrement) noexcept
{
    // Four-sample band-limited step residual (the integral of a cubic B-spline minus the ideal step), twice
    // its height for the unit jump of 2 that a saw or pulse edge has. Two samples either side of the edge
    // are corrected, which keeps aliasing far lower than the two-sample correction used before.
    if (phaseIncrement <= 0.0f)
        return 0.0f;

    if (phase < 2.0f * phaseIncrement)
    {
        const auto x = phase / phaseIncrement;
        if (x < 1.0f)
            return 2.0f * (-0.5f + x * (2.0f / 3.0f - x * x * (1.0f / 3.0f - x * (1.0f / 8.0f))));
        const auto u = 2.0f - x;
        return -(u * u * u * u) * (2.0f / 24.0f);
    }

    if (phase > 1.0f - 2.0f * phaseIncrement)
    {
        const auto x = (phase - 1.0f) / phaseIncrement; // -2 .. 0
        if (x > -1.0f)
            return 2.0f * (0.5f + x * (2.0f / 3.0f - x * x * (1.0f / 3.0f + x * (1.0f / 8.0f))));
        const auto u = x + 2.0f;
        return (u * u * u * u) * (2.0f / 24.0f);
    }

    return 0.0f;
}

float OscillatorStack::detuneOffsetCents(int index, int count, float detune) noexcept
{
    if (count <= 1)
        return 0.0f;

    const auto normalizedIndex = (2.0f * static_cast<float>(index) / static_cast<float>(count - 1)) - 1.0f;
    const auto shapedDetune = clampUnitFast(detune);
    // MEASURED (the reference synth 3.0.7.3, sweeps/univoices): the outer voice's offset at full detune depends on the
    // voice count, and falls with detune as d^2.9 (about 51 cents at d 0.5 with eight voices). The voices
    // in between are spread evenly.
    constexpr std::array<float, 9> outerCentsAtFullDetune { 0.0f, 0.0f, 100.0f, 148.0f, 195.0f, 240.0f, 284.0f, 327.0f, 370.0f };
    const auto outer = outerCentsAtFullDetune[static_cast<std::size_t>(std::min(count, 8))];
    const auto cents = outer * std::pow(shapedDetune, 2.9f);
    // The inner voices sit slightly further out than an even spread: |t|^0.93 fits the eight-voice
    // measurement (0.736, 0.456, 0.159 of the outer offset against 0.714, 0.429, 0.143).
    return std::copysign(std::pow(std::abs(normalizedIndex), 0.93f), normalizedIndex) * cents;
}

void OscillatorStack::updateDetuneRatios(int stackCount, float stackDetune) noexcept
{
    if (cachedDetuneStackCount == stackCount && std::abs(cachedDetuneAmount - stackDetune) <= 0.000001f)
        return;

    cachedDetuneStackCount = stackCount;
    cachedDetuneAmount = stackDetune;
    cachedDetunedIncrementStackCount = -1;
    cachedDetunedMasterIncrement = -1.0f;
    for (int i = 0; i < stackCount; ++i)
    {
        const auto cents = detuneOffsetCents(i, stackCount, stackDetune);
        detuneRatios[static_cast<std::size_t>(i)] = std::pow(2.0f, cents / 1200.0f);
    }
}

void OscillatorStack::updateDetunedIncrements(int stackCount, float masterIncrement,
                                              float minIncrement, float maxIncrement) noexcept
{
    cachedDetunedIncrementStackCount = stackCount;
    cachedDetunedMasterIncrement = masterIncrement;
    for (int i = 0; i < stackCount; ++i)
    {
        const auto arrayIndex = static_cast<std::size_t>(i);
        detunedIncrements[arrayIndex] = clampFast(masterIncrement * detuneRatios[arrayIndex],
                                                  minIncrement, maxIncrement);
    }
}
} // namespace synth
