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

    // Filters the left and right channels of a part through two filters prepared with the same
    // settings: exactly processPrepared on each, with the right filter taking the left's cutoff.
    static void processPreparedPair(Filter& left, Filter& right, float& leftSample, float& rightSample,
                                    float midiNote, const FilterParameters& parameters,
                                    float cutoffModSemitones) noexcept;

    // Control-rate path (modulationIntervalSamples > 1). The caller computes the cutoff coefficient
    // and high-pass leak at its modulation frames and ramps them; the pair then runs a ladder with
    // one division per channel instead of four, which differs from processPrepared by rounding only.
    float preparedCutoffSemitones(float midiNote, const FilterParameters& parameters,
                                  float cutoffModSemitones) const noexcept;
    void preparedCoefficients(float cutoffSemitones, float& coefficient, float& highpassLeak) const noexcept;
    static void processRampedPair(Filter& left, Filter& right, float& leftSample, float& rightSample,
                                  float coefficient, float highpassLeak) noexcept;

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
    void updatePreparedCutoff(float midiNote, const FilterParameters& parameters, float cutoffModSemitones) noexcept;
    float ladderInputFor(float input, float coefficient, float feedback, float driveGain) const noexcept;
    void runStage(int index, float& x, float coefficient) noexcept;
    void processStages(float input, float coefficient, float feedback, float driveGain) noexcept;
    static void processStagesPair(Filter& left, Filter& right, float leftInput, float rightInput) noexcept;
    float tapOutput(float input, FilterMode mode) const noexcept;
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
    float cachedInverseDriveLimit = 1.0e-4f;
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
