#pragma once

namespace synth
{
enum class EnvelopeStage
{
    Idle,
    Attack,
    Decay,
    Sustain,
    Release
};

// The value is the envelope as the reference synth computes it; a part's volume is its square (see Envelope.cpp).
// Times follow the reference synth's definitions (measured): attack = time until the squared value reaches 0.99;
// decay and release = time until the squared value has fallen 40 dB, the stage ending at 1.33 x that time.
struct EnvelopeSettings
{
    float attackMs = 2.0f;
    float decayMs = 300.0f;
    float sustain = 0.8f;
    float releaseMs = 200.0f;
};

class Envelope
{
public:
    void prepare(double newSampleRate) noexcept;
    void setSettings(EnvelopeSettings newSettings) noexcept;
    void reset() noexcept;
    void noteOn() noexcept;
    void noteOff() noexcept;
    float process() noexcept;

    float getValue() const noexcept { return value; }
    EnvelopeStage getStage() const noexcept { return stage; }
    bool isActive() const noexcept { return stage != EnvelopeStage::Idle; }

private:
    float samplesForMs(float ms) const noexcept;
    void updateCachedRates() noexcept;

    double sampleRate = 44100.0;
    EnvelopeSettings settings;
    EnvelopeStage stage = EnvelopeStage::Idle;
    float value = 0.0f;
    // Per-sample factors of the three exponential stages (see Envelope.cpp).
    float attackStep = 0.0f;
    float decayStep = 0.0f;
    float releaseStep = 0.0f;
    static constexpr float attackTarget = 1.1545f; // 1 + overshoot
};
} // namespace synth
