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

// Times follow Sylenth1's definitions (measured): attack = time to full level along an
// S-shaped curve; decay and release = time to fall 40 dB along a fixed curve that ends near
// -90 dB at 1.31 x that time.
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
    float releaseStart = 0.0f;
    float stageElapsed = 0.0f;  // samples into the current stage
    float attackSamples = 1.0f; // attack time: reaches full level
    float decaySamples = 1.0f;  // decay time: reaches -40 dB of the way to sustain
    float releaseSamples = 1.0f; // release time: reaches -40 dB
};
} // namespace synth
