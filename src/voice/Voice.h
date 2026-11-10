#pragma once

#include "../dsp/Envelope.h"
#include "../dsp/Filter.h"
#include "../dsp/Lfo.h"
#include "../dsp/OscillatorStack.h"
#include "../dsp/Ramp.h"
#include "../dsp/SynthParameters.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace synth
{
enum class VoiceState
{
    Idle,
    Held,
    Releasing
};

struct VoiceSnapshot
{
    VoiceState state = VoiceState::Idle;
    int midiNote = -1;
    float velocity = 0.0f;
    float ampEnv = 0.0f;
    float modEnv = 0.0f;
    float lfo = 0.0f;
    float ramp = 0.0f;
    float randomOnNote = 0.0f;
    float effectiveMidiNote = 0.0f;
    float velocityGlide = 0.0f;
    float voiceUni = 0.0f;
    float voiceBi = 0.0f;
    float unisonUni = 0.0f;
    float unisonBi = 0.0f;
    float directOscPitchSemitones = 0.0f;
    float directPulseWidth = 0.0f;
    float directFilterCutoffSemitones = 0.0f;
    float transModOscPitchSemitones = 0.0f;
    float transModPulseWidth = 0.0f;
    float transModFilterCutoffSemitones = 0.0f;
    float transModAmpLevelDb = 0.0f;
    float transModPan = 0.0f;
    float lfoPhase = 0.0f;
    float lfo2Phase = 0.0f;
    std::uint32_t retriggers = 0; // counts the notes that restarted this voice (and its LFOs, unless free)
};

struct LfoFrame
{
    float value = 0.0f;
    float phase = 0.0f;
};

struct StereoFrame
{
    float left = 0.0f;
    float right = 0.0f;
    float phaserCenterOffsetHz = 0.0f;
};

// The modulation slots that are on, and the destinations each one reaches, in slot order. Built
// once per block and shared by every voice, so a voice skips the slots and destinations not in use.
struct VoiceModPlan
{
    struct Route
    {
        int slot = 0;
        bool legacyDepth = false; // the slot sets one of the legacy depths summed by evaluateTransMod
        int destinationCount = 0;
        std::array<std::uint8_t, nativeModDestinationCount> destinations {};
        std::array<float, nativeModDestinationCount> depths {};
    };

    std::array<Route, transModSlotCount> routes {};
    int routeCount = 0;
    bool anyLegacyDepth = false;
    std::array<bool, nativeModDestinationCount> live {}; // reached by at least one route
    std::array<std::uint8_t, nativeModDestinationCount> liveDestinations {};
    int liveCount = 0;

    void build(const TransModParameters& transMod) noexcept;
};

// Where a voice writes one block: its output and its phaser offset, one value per sample it renders.
struct VoiceBlockOutput
{
    float* left = nullptr;
    float* right = nullptr;
    float* phaserCenterOffsetHz = nullptr;
};

class Voice
{
public:
    void prepare(double sampleRate);
    void setVoiceIndex(int index, int total) noexcept;
    void setAllocationIndices(int index, int total, int newUnisonIndex, int newUnisonCount) noexcept;
    void noteOn(int note, float normalizedVelocity, float randomValue,
                int newUnisonIndex, int newUnisonCount, const SynthParameters& parameters,
                bool allowGlide, bool retriggerModulators, float glideFromNote = -1.0f) noexcept;
    void noteOff() noexcept;
    void stopWithFade(int fadeSamples) noexcept;
    void reset() noexcept;
    void process(int numSamples) noexcept;
    void syncModulators(const SynthParameters& parameters) noexcept;
    // Renders up to renderBlockMaxSamples, sample by sample for the modulation and then module by
    // module for the oscillators and filters; the result is the same as rendering one sample at a
    // time. The mono LFO arrays hold one frame per sample, or are null when the LFO is per voice.
    // Returns the sample at which the voice ended (it is written, and the voice is idle after it),
    // or numSamples if the voice is still playing.
    int renderBlock(const SynthParameters& parameters, const VoiceModPlan& plan, const LfoFrame* monoLfoValues,
                    const LfoFrame* monoLfo2Values, int numSamples, const VoiceBlockOutput& output) noexcept;
    bool isActive() const noexcept { return state != VoiceState::Idle; }
    bool isHeld() const noexcept { return state == VoiceState::Held; }
    bool isStopFading() const noexcept { return stopFadeSamples > 0 && stopFadeTotalSamples > 0; }
    int getMidiNote() const noexcept { return midiNote; }
    int getUnisonIndex() const noexcept { return unisonIndex; }
    VoiceSnapshot snapshot() const noexcept;

private:
    struct ModulationSums
    {
        float oscPitchSemitones = 0.0f;
        float pulseWidth = 0.0f;
        float filterCutoffSemitones = 0.0f;
        float ampLevelDb = 0.0f;
        float pan = 0.0f;
    };

    float processGlide(const SynthParameters& parameters) noexcept;
    float processVelocityGlide(const SynthParameters& parameters) noexcept;
    void syncModulatorConfig(const SynthParameters& parameters) noexcept;
    void resetLayerOscillators(const SynthParameters& parameters, float fallbackPhase) noexcept;
    float evalModSource(const SynthParameters& parameters, ModSource source, float lfoValue, float rampValue,
                        float modEnvValue, float ampEnvValue, float effectiveNote,
                        float velocityGlideValue) const noexcept;
    ModulationSums evaluateTransMod(const SynthParameters& parameters, const VoiceModPlan& plan,
                                    const float* amounts, const bool* amountsFinite) const noexcept;
    VoiceState state = VoiceState::Idle;
    int midiNote = -1;
    float velocity = 0.0f;
    float currentVelocity = 0.0f;
    float startVelocity = 0.0f;
    int velocityGlideSamples = 0;
    int velocityGlideTotalSamples = 0;
    float currentMidiNote = 0.0f;
    float startMidiNote = 0.0f;
    float targetMidiNote = 0.0f;
    int glideSamples = 0;
    int glideTotalSamples = 0;
    float randomOnNote = 0.0f;
    int voiceIndex = 0;
    int voiceCount = 1;
    int unisonIndex = 0;
    int unisonCount = 1;
    int stopFadeSamples = 0;
    int stopFadeTotalSamples = 0;
    double sampleRate = 44100.0;
    Envelope modEnvelope;
    Lfo lfo;
    Ramp ramp;
    static constexpr auto layerOscillatorCount =
        static_cast<std::size_t>(layerCount) * static_cast<std::size_t>(oscillatorSlotsPerLayer);
    std::array<OscillatorStack, layerOscillatorCount> layerOscillators;
    std::array<Envelope, layerCount> partAmpEnvelopes;
    std::array<std::array<Filter, 2>, layerCount> partFilters;
    std::array<FilterParameters, layerCount> preparedPartFilters;
    Envelope modEnvelope2;
    Lfo lfo2;
    std::uint32_t retriggers = 0;
    std::array<float, nativeModDestinationCount> nativeModulation {};
    float nativeModEnv2Value = 0.0f;
    float nativeAmpEnv2Value = 0.0f;
    float nativeLfo2Value = 0.0f;
    float nativeLfo1PhaseOffset = 0.0f;
    float nativeLfo2PhaseOffset = 0.0f;
    bool modulatorConfigInitialized = false;
    ModulationSums lastDirectSums;
    ModulationSums lastTransModSums;
    float lastRampValue = 0.0f;
    float cachedVoiceUni = 0.0f;
    float cachedVoiceBi = 0.0f;
    float cachedUnisonUni = 0.0f;
    float cachedUnisonBi = 0.0f;

    // Control-rate modulation (modulationIntervalSamples > 1): the last two modulation frames,
    // between which each modulated value ramps.
    struct ControlFrame
    {
        float pitch = 0.0f;  // pitch modulation without keytracking, semitones
        float cutoff = 0.0f; // cutoff modulation without keytracking, semitones
        float levelDb = 0.0f;
        float pan = 0.0f;
        float phaserCenterOffsetHz = 0.0f;
        std::array<float, nativeModDestinationCount> modulation {};
        std::array<float, layerCount> filterCoefficient {};
        std::array<float, layerCount> filterHighpassLeak {};
    };
    ControlFrame controlFrom;
    ControlFrame controlTo;
    int controlCountdown = 0; // samples until the next frame
    int controlPosition = 0;  // samples since the last frame
    bool controlPrimed = false;
};
} // namespace synth
