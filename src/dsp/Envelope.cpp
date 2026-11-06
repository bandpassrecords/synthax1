#include "Envelope.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace synth
{
namespace
{
// Stage shapes measured from the reference synth 3.0.7.3 (research/parity, sweeps/env). Each stage has
// one fixed shape stretched over its time; times of all lengths trace the same curve.
//
// Attack: level at t/T for T = the attack time (reaching 99%), 33 points. Slow start,
// near-linear middle, gentle arrival, unlike a linear ramp.
constexpr std::array<float, 33> attackShape {
    0.0000f, 0.0048f, 0.0180f, 0.0380f, 0.0637f, 0.0941f, 0.1275f, 0.1639f, 0.2024f, 0.2421f, 0.2830f,
    0.3242f, 0.3654f, 0.4066f, 0.4472f, 0.4873f, 0.5262f, 0.5641f, 0.6011f, 0.6366f, 0.6712f, 0.7045f,
    0.7364f, 0.7673f, 0.7968f, 0.8251f, 0.8521f, 0.8779f, 0.9026f, 0.9260f, 0.9485f, 0.9697f, 0.9900f,
};

// Decay and release (the same shape): level in dB at t/T for T = the time to reach -40 dB,
// in steps of 1/32; the stage ends at the last point (-90 dB). Faster than exponential at
// first in level terms, close to linear in dB until about -30 dB, then steeper.
constexpr std::array<float, 43> fallShapeDb {
    0.00f,   -1.02f,  -1.98f,  -2.94f,  -3.91f,  -4.89f,  -5.88f,  -6.88f,  -7.89f,  -8.92f,  -9.95f,
    -11.00f, -12.06f, -13.15f, -14.24f, -15.36f, -16.50f, -17.65f, -18.83f, -20.04f, -21.28f, -22.55f,
    -23.85f, -25.18f, -26.56f, -27.98f, -29.47f, -31.01f, -32.63f, -34.31f, -36.07f, -37.95f, -40.02f,
    -42.18f, -44.50f, -47.09f, -49.98f, -53.25f, -57.15f, -62.01f, -68.23f, -76.79f, -90.14f,
};

constexpr float fallEnd = static_cast<float>(fallShapeDb.size() - 1) / 32.0f;

float attackLevel(float progress) noexcept
{
    if (progress >= 1.0f)
        return 1.0f;
    const auto position = std::max(0.0f, progress) * 32.0f;
    const auto index = static_cast<std::size_t>(position);
    const auto fraction = position - static_cast<float>(index);
    const auto level = attackShape[index] + (attackShape[index + 1] - attackShape[index]) * fraction;
    return level / attackShape.back(); // reach exactly 1 at the end of the attack time
}

// Where on the attack curve a given level sits, so a retrigger continues from it.
float attackProgressFor(float level) noexcept
{
    const auto target = std::clamp(level, 0.0f, 1.0f) * attackShape.back();
    for (std::size_t i = 1; i < attackShape.size(); ++i)
        if (attackShape[i] >= target)
        {
            const auto span = attackShape[i] - attackShape[i - 1];
            const auto fraction = span > 0.0f ? (target - attackShape[i - 1]) / span : 0.0f;
            return (static_cast<float>(i - 1) + fraction) / 32.0f;
        }
    return 1.0f;
}

// Remaining level (1 -> 0) at t/T along the decay/release curve; 0 past its end.
float fallLevel(float progress) noexcept
{
    if (progress >= fallEnd)
        return 0.0f;
    const auto position = std::max(0.0f, progress) * 32.0f;
    const auto index = static_cast<std::size_t>(position);
    const auto fraction = position - static_cast<float>(index);
    const auto db = fallShapeDb[index] + (fallShapeDb[index + 1] - fallShapeDb[index]) * fraction;
    return std::pow(10.0f, db * 0.05f);
}
} // namespace

void Envelope::prepare(double newSampleRate) noexcept
{
    sampleRate = std::isfinite(newSampleRate) && newSampleRate > 0.0 ? newSampleRate : 44100.0;
    updateCachedRates();
    reset();
}

void Envelope::setSettings(EnvelopeSettings newSettings) noexcept
{
    newSettings.attackMs = std::max(0.0f, newSettings.attackMs);
    newSettings.decayMs = std::max(0.0f, newSettings.decayMs);
    newSettings.sustain = std::clamp(newSettings.sustain, 0.0f, 1.0f);
    newSettings.releaseMs = std::max(0.0f, newSettings.releaseMs);
    if (std::abs(newSettings.attackMs - settings.attackMs) <= 0.000001f
        && std::abs(newSettings.decayMs - settings.decayMs) <= 0.000001f
        && std::abs(newSettings.sustain - settings.sustain) <= 0.000001f
        && std::abs(newSettings.releaseMs - settings.releaseMs) <= 0.000001f)
    {
        return;
    }

    settings = newSettings;
    updateCachedRates();
}

void Envelope::reset() noexcept
{
    stage = EnvelopeStage::Idle;
    value = 0.0f;
    releaseStart = 0.0f;
    stageElapsed = 0.0f;
}

void Envelope::noteOn() noexcept
{
    if (settings.attackMs <= 0.0f)
    {
        value = 1.0f;
        stage = EnvelopeStage::Decay;
        stageElapsed = 0.0f;
        return;
    }

    // Continue from the current level (legato or a retrigger during release): no click.
    stage = EnvelopeStage::Attack;
    stageElapsed = attackProgressFor(value) * attackSamples;
}

void Envelope::noteOff() noexcept
{
    if (stage == EnvelopeStage::Idle)
        return;

    releaseStart = value;
    stageElapsed = 0.0f;
    stage = settings.releaseMs <= 0.0f ? EnvelopeStage::Idle : EnvelopeStage::Release;
    if (stage == EnvelopeStage::Idle)
        value = 0.0f;
}

float Envelope::process() noexcept
{
    switch (stage)
    {
        case EnvelopeStage::Idle:
            break;

        case EnvelopeStage::Attack:
            stageElapsed += 1.0f;
            value = attackLevel(stageElapsed / attackSamples);
            if (stageElapsed >= attackSamples)
            {
                value = 1.0f;
                stage = EnvelopeStage::Decay;
                stageElapsed = 0.0f;
            }
            break;

        case EnvelopeStage::Decay: {
            if (settings.decayMs <= 0.0f)
            {
                value = settings.sustain;
                stage = EnvelopeStage::Sustain;
                break;
            }
            stageElapsed += 1.0f;
            const auto remaining = fallLevel(stageElapsed / decaySamples);
            value = settings.sustain + (1.0f - settings.sustain) * remaining;
            if (remaining <= 0.0f)
            {
                value = settings.sustain;
                stage = EnvelopeStage::Sustain;
            }
            break;
        }

        case EnvelopeStage::Sustain:
            value = settings.sustain;
            break;

        case EnvelopeStage::Release: {
            stageElapsed += 1.0f;
            const auto remaining = fallLevel(stageElapsed / releaseSamples);
            value = releaseStart * remaining;
            if (remaining <= 0.0f)
            {
                value = 0.0f;
                stage = EnvelopeStage::Idle;
            }
            break;
        }
    }

    return value;
}

float Envelope::samplesForMs(float ms) const noexcept
{
    return std::max(1.0f, static_cast<float>(sampleRate) * ms / 1000.0f);
}

void Envelope::updateCachedRates() noexcept
{
    attackSamples = samplesForMs(settings.attackMs);
    decaySamples = samplesForMs(settings.decayMs);
    releaseSamples = samplesForMs(settings.releaseMs);
}
} // namespace synth
