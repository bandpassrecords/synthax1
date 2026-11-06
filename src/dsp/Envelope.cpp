#include "Envelope.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace synth
{
namespace
{
// Measured on the reference synth 3.0.7.3 (research/parity, sweeps/env, envshape, sustain). The envelope is the
// analogue kind: every stage is an exponential aimed past its destination, so it arrives in finite time.
//
//   attack   aims at 1 + c and stops on reaching 1
//   decay    aims at sustain - c (1 - sustain) and stops on reaching sustain
//   release  aims at -c and stops on reaching 0
//
// with c = 0.1545. Modulation reads this value as it is (the pitch of a sine under Mod Env 1 follows it linearly);
// the part volume is its square (sustain 0.5 sits 12 dB down, sustain 0.1 at -40 dB). The old table-based shapes
// were the squared curves read off in decibels: the fit of 1 - e^(-t/s) squared to the attack table is within
// 0.0003, and (1 + c) e^(-t/tau) - c, squared, within 0.06 dB of the decay table.
constexpr float overshoot = 0.1545f;

// Time constants as fractions of the stage time: attack time = level 0.99 (v^2) reached, decay and release time =
// fall to -40 dB (v^2), i.e. to v = 0.1.
constexpr float attackSigma = 0.5052f; // (1 + c) (1 - e^(-1/sigma)) = sqrt(0.99)
constexpr float fallTau = 0.6613f;     // (1 + c) e^(-1/tau) - c = 0.1
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
}

void Envelope::noteOn() noexcept
{
    if (settings.attackMs <= 0.0f)
    {
        value = 1.0f;
        stage = EnvelopeStage::Decay;
        return;
    }

    // Continue from the current level (legato or a retrigger during release): no click.
    stage = EnvelopeStage::Attack;
}

void Envelope::noteOff() noexcept
{
    if (stage == EnvelopeStage::Idle)
        return;

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
            value = attackTarget + (value - attackTarget) * attackStep;
            if (value >= 1.0f)
            {
                value = 1.0f;
                stage = EnvelopeStage::Decay;
            }
            break;

        case EnvelopeStage::Decay: {
            if (settings.decayMs <= 0.0f)
            {
                value = settings.sustain;
                stage = EnvelopeStage::Sustain;
                break;
            }
            const auto target = settings.sustain - overshoot * (1.0f - settings.sustain);
            value = target + (value - target) * decayStep;
            if (value <= settings.sustain)
            {
                value = settings.sustain;
                stage = EnvelopeStage::Sustain;
            }
            break;
        }

        case EnvelopeStage::Sustain:
            value = settings.sustain;
            break;

        case EnvelopeStage::Release:
            value = -overshoot + (value + overshoot) * releaseStep;
            if (value <= 0.0f)
            {
                value = 0.0f;
                stage = EnvelopeStage::Idle;
            }
            break;
    }

    return value;
}

float Envelope::samplesForMs(float ms) const noexcept
{
    return std::max(1.0f, static_cast<float>(sampleRate) * ms / 1000.0f);
}

void Envelope::updateCachedRates() noexcept
{
    attackStep = std::exp(-1.0f / (attackSigma * samplesForMs(settings.attackMs)));
    decayStep = std::exp(-1.0f / (fallTau * samplesForMs(settings.decayMs)));
    releaseStep = std::exp(-1.0f / (fallTau * samplesForMs(settings.releaseMs)));
}
} // namespace synth
