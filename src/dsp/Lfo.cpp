#include "Lfo.h"

#include <algorithm>
#include <cmath>

namespace synth
{
namespace
{
constexpr float twoPi = 6.28318530717958647692f;
constexpr float pi = 3.14159265358979323846f;
}

void Lfo::prepare(double newSampleRate) noexcept
{
    sampleRate = std::isfinite(newSampleRate) && newSampleRate > 0.0 ? newSampleRate : 44100.0;
    updatePhaseIncrement();
    resetPhase();
}

void Lfo::setRateHz(float newRateHz) noexcept
{
    const auto clampedRate = std::isfinite(newRateHz) ? std::clamp(newRateHz, 0.01f, 200.0f) : 0.01f;
    if (std::abs(clampedRate - rateHz) <= 0.000001f)
        return;

    rateHz = clampedRate;
    updatePhaseIncrement();
}

void Lfo::setShape(LfoShape newShape) noexcept
{
    if (shape == newShape)
        return;

    shape = newShape;
}

void Lfo::setSteps(const float* stepValues, int count, float smooth) noexcept
{
    stepCount = std::clamp(count, 1, lfoMaxSteps);
    stepSmooth = std::isfinite(smooth) ? std::clamp(smooth, 0.0f, 1.0f) : 0.0f;
    if (stepValues == nullptr)
        return;

    for (int i = 0; i < stepCount; ++i)
    {
        const auto raw = stepValues[i];
        steps[static_cast<std::size_t>(i)] = std::isfinite(raw) ? std::clamp(raw, -1.0f, 1.0f) : 0.0f;
    }
}

void Lfo::setPhaseDegrees(float degrees) noexcept
{
    degrees = std::isfinite(degrees) ? std::clamp(degrees, 0.0f, 360.0f) : 0.0f;
    if (cachedPhaseDegrees == degrees) return;
    cachedPhaseDegrees = degrees;
    const auto newPhaseOffset = degrees >= 360.0f ? 0.0f : degrees / 360.0f;
    if (std::abs(newPhaseOffset - phaseOffset) <= 0.000001f)
        return;

    phaseOffset = newPhaseOffset;
}

void Lfo::resetPhase() noexcept
{
    phase = phaseOffset;
    value = valueForPhase(phase);
}

void Lfo::setNormalizedPhase(float normalizedPhase) noexcept
{
    phase = std::isfinite(normalizedPhase) ? normalizedPhase - std::floor(normalizedPhase) : 0.0f;
    value = valueForPhase(phase);
}

float Lfo::process() noexcept
{
    const auto previousPhase = phase;
    phase += phaseIncrement;
    if (phase >= 1.0f)
        phase -= std::floor(phase);

    // MEASURED (the reference synth 3.0.7.3, sweeps/lfoshape): its sample-and-hold and random LFOs draw a new value
    // twice per cycle, on both square-wave edges. Random glides to each new value (cosine) instead of
    // stepping; it is not audio-rate noise.
    const auto crossed = phase < previousPhase || (previousPhase < 0.5f && phase >= 0.5f);
    if ((shape == LfoShape::SampleHold || shape == LfoShape::Noise) && crossed)
    {
        previousRandom = heldRandom;
        randomState = randomState * 1664525u + 1013904223u;
        heldRandom = (static_cast<float>((randomState >> 8) & 0x00ffffffu) / 8388607.5f) - 1.0f;
    }
    value = valueForPhase(phase);
    return value;
}

void Lfo::advance() noexcept
{
    const auto previousPhase = phase;
    phase += phaseIncrement;
    if (phase >= 1.0f)
        phase -= std::floor(phase);
    const auto crossed = phase < previousPhase || (previousPhase < 0.5f && phase >= 0.5f);
    if ((shape == LfoShape::SampleHold || shape == LfoShape::Noise) && crossed)
    {
        previousRandom = heldRandom;
        randomState = randomState * 1664525u + 1013904223u;
        heldRandom = (static_cast<float>((randomState >> 8) & 0x00ffffffu) / 8388607.5f) - 1.0f;
    }
}

float Lfo::refreshValue() noexcept
{
    value = valueForPhase(phase);
    return value;
}

float Lfo::valueForPhase(float phaseValue) noexcept
{
    phaseValue -= std::floor(phaseValue);

    switch (shape)
    {
        case LfoShape::Sine:
            return std::sin(twoPi * phaseValue);
        case LfoShape::Triangle:
            return 1.0f - 4.0f * std::abs(phaseValue - 0.5f);
        case LfoShape::SawUp:
            return 2.0f * phaseValue - 1.0f;
        case LfoShape::SawDown:
            return 1.0f - 2.0f * phaseValue;
        case LfoShape::Square:
            return phaseValue < 0.5f ? 1.0f : -1.0f;
        case LfoShape::SampleHold:
            return heldRandom;
        case LfoShape::Noise: {
            const auto along = phaseValue < 0.5f ? phaseValue * 2.0f : phaseValue * 2.0f - 1.0f;
            const auto glide = 0.5f - 0.5f * std::cos(pi * along);
            return previousRandom + (heldRandom - previousRandom) * glide;
        }
        case LfoShape::Step: {
            const auto count = static_cast<float>(stepCount);
            const auto position = phaseValue * count;
            const auto index = std::min(static_cast<int>(position), stepCount - 1);
            const auto current = steps[static_cast<std::size_t>(index)];
            if (stepSmooth <= 0.0001f)
                return current;

            // Crossfade only inside the trailing `stepSmooth` fraction of the
            // step so low smooth values keep the gated stepper character.
            const auto fraction = position - static_cast<float>(index);
            const auto blendStart = 1.0f - stepSmooth;
            if (fraction <= blendStart)
                return current;

            const auto nextIndex = index + 1 < stepCount ? index + 1 : 0;
            const auto next = steps[static_cast<std::size_t>(nextIndex)];
            const auto blend = (fraction - blendStart) / stepSmooth;
            return current + (next - current) * blend;
        }
    }

    return 0.0f;
}

void Lfo::updatePhaseIncrement() noexcept
{
    phaseIncrement = rateHz / static_cast<float>(sampleRate);
}
} // namespace synth
