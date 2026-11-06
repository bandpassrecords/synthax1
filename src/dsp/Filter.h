#pragma once

#include "SynthParameters.h"

#include <array>

namespace synth
{
class Filter
{
public:
    void prepare(double newSampleRate) noexcept;
    void reset() noexcept;
    float process(float input, float midiNote, const SynthParameters& parameters,
                  float cutoffModSemitones) noexcept;
    void prepareBlock(const SynthParameters& parameters) noexcept;
    float processPrepared(float input, float midiNote, const SynthParameters& parameters,
                          float cutoffModSemitones) noexcept;
    void processPreparedBlock(float* samples, const float* midiNote, const SynthParameters& parameters,
                              const float* cutoffModSemitones, int numSamples) noexcept;

    void prepareBlock(const FilterParameters& parameters) noexcept;
    float processPrepared(float input, float midiNote, const FilterParameters& parameters,
                          float cutoffModSemitones) noexcept;

    static float cutoffSemitonesToHz(float semitones) noexcept;

    // Reuses the cutoff-dependent coefficient another filter just computed for the same cutoff
    // and sample rate (the left and right filters of a voice), so the tan runs once, not twice.
    void shareCutoffFrom(const Filter& other) noexcept
    {
        cachedCutoffSemitones = other.cachedCutoffSemitones;
        cachedEffectiveSampleRate = other.cachedEffectiveSampleRate;
        cachedCoefficient = other.cachedCoefficient;
        cachedHighpassLeak = other.cachedHighpassLeak;
        cachedCoefficientMode = other.cachedCoefficientMode;
    }

private:
    void processStages(float input, float coefficient, float feedback, float driveGain) noexcept;
    float processCoreL4(float input, float coefficient, float feedback, float driveGain) noexcept;
    float processCore(float input, float coefficient, float feedback, float driveGain,
                      FilterMode mode) noexcept;

    double sampleRate = 44100.0;
    std::array<float, 4> stage {};
    std::array<float, 4> clippedStage {}; // stage outputs (the ladder taps)
    float ladderInput = 0.0f;             // saturated ladder input u, for the high-pass taps
    float previousInput = 0.0f;
    float cachedCutoffSemitones = -1.0f;
    float cachedEffectiveSampleRate = 0.0f;
    float cachedCoefficient = 0.0f;
    float cachedDriveAmount = -1.0f;
    float cachedResonance = -1.0f;
    float cachedDriveGain = 1.0f;
    float cachedDriveLimit = 1.0e4f;
    float cachedHighpassLeak = 0.0f;
    int cachedCoefficientMode = -1; // the mode the cached coefficient and leak were computed for
    float cachedFeedback = 0.0f;
    bool preparedEnabled = false;
    bool preparedWarmDrive = false;
    bool preparedNativeTopology = false;
    bool cachedSelfOscillation = false;
    bool preparedUseL4 = false;
    FilterMode preparedMode = FilterMode::L4;
    int preparedOversampling = 1;
    float preparedKeytrack = 0.0f;
    float preparedEffectiveSampleRate = 0.0f;
};
} // namespace synth
