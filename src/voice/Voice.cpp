#include "Voice.h"

#include <algorithm>
#include <cmath>

namespace synth
{
namespace
{
constexpr float halfPi = 1.57079632679489661923f;

LfoShape toLfoShape(LfoShapeChoice choice) noexcept
{
    switch (choice)
    {
        case LfoShapeChoice::Sine: return LfoShape::Sine;
        case LfoShapeChoice::Triangle: return LfoShape::Triangle;
        case LfoShapeChoice::SawUp: return LfoShape::SawUp;
        case LfoShapeChoice::SawDown: return LfoShape::SawDown;
        case LfoShapeChoice::Square: return LfoShape::Square;
        case LfoShapeChoice::SampleHold:
            return LfoShape::SampleHold;
        case LfoShapeChoice::Noise:
            return LfoShape::SampleHold;
        case LfoShapeChoice::Step: return LfoShape::Step;
    }

    return LfoShape::SawDown;
}

EnvelopeSettings toEnvelopeSettings(EnvelopeParameters parameters) noexcept
{
    return { parameters.attackMs, parameters.decayMs, parameters.sustain, parameters.releaseMs };
}

float syncDivisionBeats(int division) noexcept
{
    switch (division)
    {
        case 0: return 0.25f;
        case 1: return 0.5f;
        case 2: return 0.75f;
        case 3: return 1.0f;
        case 4: return 2.0f;
        case 5: return 4.0f;
        case 6: return 1.0f / 12.0f;
        case 7: return 0.1875f;
        case 8: return 1.0f / 6.0f;
        case 9: return 0.375f;
        case 10: return 1.0f / 3.0f;
        case 11: return 2.0f / 3.0f;
        case 12: return 1.5f;
        case 13: return 4.0f / 3.0f;
        case 14: return 3.0f;
        case 15: return 0.125f;
        case 16: return 8.0f / 3.0f;
        case 17: return 6.0f;
        default: return 1.0f;
    }
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

float bipolarIndex(int index, int count) noexcept
{
    if (count <= 1)
        return 0.0f;

    return (2.0f * static_cast<float>(index) / static_cast<float>(count - 1)) - 1.0f;
}

float unipolarIndex(int index, int count) noexcept
{
    if (count <= 1)
        return 0.0f;

    return clampUnitFast(static_cast<float>(index) / static_cast<float>(count - 1));
}

float sanitize(float value) noexcept
{
    return std::isfinite(value) ? value : 0.0f;
}

int layerOscillatorIndex(int layerIndex, int oscillatorIndex) noexcept
{
    return layerIndex * oscillatorSlotsPerLayer + oscillatorIndex;
}

} // namespace

void Voice::prepare(double newSampleRate)
{
    sampleRate = std::isfinite(newSampleRate) && newSampleRate > 0.0 ? newSampleRate : 44100.0;
    for (auto& envelope : partAmpEnvelopes) envelope.prepare(sampleRate);
    for (auto& part : partFilters)
        for (auto& channel : part) channel.prepare(sampleRate);
    modEnvelope.prepare(sampleRate);
    modEnvelope2.prepare(sampleRate);
    lfo.prepare(sampleRate);
    lfo2.prepare(sampleRate);
    for (auto& stack : layerOscillators) stack.prepare(sampleRate);
    ramp.prepare(sampleRate);
}

void Voice::setVoiceIndex(int index, int total) noexcept
{
    setAllocationIndices(index, total, unisonIndex, unisonCount);
}

void Voice::setAllocationIndices(int index, int total, int newUnisonIndex, int newUnisonCount) noexcept
{
    voiceIndex = std::max(0, index);
    voiceCount = std::max(1, total);
    unisonIndex = std::max(0, newUnisonIndex);
    unisonCount = std::max(1, newUnisonCount);
    cachedVoiceUni = unipolarIndex(voiceIndex, voiceCount);
    cachedVoiceBi = bipolarIndex(voiceIndex, voiceCount);
    cachedUnisonUni = unipolarIndex(unisonIndex, unisonCount);
    cachedUnisonBi = bipolarIndex(unisonIndex, unisonCount);
}

void Voice::noteOn(int note, float normalizedVelocity, float randomValue,
                   int newUnisonIndex, int newUnisonCount, const SynthParameters& parameters,
                   bool allowGlide, bool retriggerModulators, float glideFromNote) noexcept
{
    normalizedVelocity = clampUnitFast(normalizedVelocity);
    const auto wasActive = state != VoiceState::Idle && midiNote >= 0;
    const auto shouldGlide = allowGlide && (wasActive || glideFromNote >= 0.0f);
    const auto shouldRetrigger = !wasActive || retriggerModulators;
    const auto previousNote = wasActive ? currentMidiNote : (glideFromNote >= 0.0f ? glideFromNote : static_cast<float>(note));
    const auto previousVelocity = wasActive ? currentVelocity : normalizedVelocity;

    state = VoiceState::Held;
    midiNote = note;
    velocity = std::clamp(normalizedVelocity, 0.0f, 1.0f);
    startVelocity = previousVelocity;
    currentVelocity = previousVelocity;
    const auto velocityGlideMs = std::clamp(parameters.velocityGlideMs, 0.0f, 2000.0f);
    velocityGlideTotalSamples = shouldGlide && velocityGlideMs > 0.0f
        ? std::max(1, static_cast<int>(std::round(velocityGlideMs * static_cast<float>(sampleRate) * 0.001f)))
        : 0;
    velocityGlideSamples = 0;
    if (velocityGlideTotalSamples == 0)
        currentVelocity = velocity;

    startMidiNote = previousNote;
    targetMidiNote = static_cast<float>(note);
    currentMidiNote = previousNote;
    const auto glideMs = std::clamp(parameters.glideMs, 0.0f, 2000.0f);
    glideTotalSamples = shouldGlide && glideMs > 0.0f
        ? std::max(1, static_cast<int>(std::round(glideMs * static_cast<float>(sampleRate) * 0.001f)))
        : 0;
    glideSamples = 0;
    if (glideTotalSamples == 0)
        currentMidiNote = targetMidiNote;

    randomOnNote = clampFast(randomValue, -1.0f, 1.0f);
    stopFadeSamples = 0;
    stopFadeTotalSamples = 0;
    setAllocationIndices(voiceIndex, voiceCount, newUnisonIndex, newUnisonCount);
    syncModulatorConfig(parameters);
    if (shouldRetrigger)
    {
        {
            for (auto& envelope : partAmpEnvelopes) envelope.noteOn();
            modEnvelope2.noteOn();
            if (!parameters.lfo.free) lfo.resetPhase();
            if (!parameters.lfo2.free) lfo2.resetPhase();
            for (auto& part : partFilters)
                for (auto& channel : part) channel.reset();
        }
        controlCountdown = 0;
        controlPrimed = false;
        modEnvelope.noteOn();
        if (!parameters.lfo.free && (parameters.lfo.gateMode == LfoGateMode::Poly || parameters.lfo.gateMode == LfoGateMode::PolyOn))
            lfo.resetPhase();
        resetLayerOscillators(parameters, randomOnNote);
        ramp.noteOn();
    }
}

void Voice::noteOff() noexcept
{
    if (state == VoiceState::Idle)
        return;

    if (stopFadeSamples > 0)
        return;

    stopFadeSamples = 0;
    stopFadeTotalSamples = 0;
    state = VoiceState::Releasing;
    modEnvelope.noteOff();
    modEnvelope2.noteOff();
    for (auto& envelope : partAmpEnvelopes) envelope.noteOff();
}

void Voice::stopWithFade(int fadeSamples) noexcept
{
    if (state == VoiceState::Idle)
        return;

    if (stopFadeSamples > 0)
        return;

    stopFadeTotalSamples = std::max(1, fadeSamples);
    stopFadeSamples = stopFadeTotalSamples;
    state = VoiceState::Releasing;
}

float Voice::normalizationPowerWeight() const noexcept
{
    if (state == VoiceState::Idle)
        return 0.0f;

    if (stopFadeSamples <= 0 || stopFadeTotalSamples <= 0)
        return 1.0f;

    const auto fade = static_cast<float>(stopFadeSamples)
        / static_cast<float>(std::max(1, stopFadeTotalSamples));
    return fade * fade;
}

void Voice::reset() noexcept
{
    state = VoiceState::Idle;
    midiNote = -1;
    velocity = 0.0f;
    currentVelocity = 0.0f;
    startVelocity = 0.0f;
    velocityGlideSamples = 0;
    velocityGlideTotalSamples = 0;
    currentMidiNote = 0.0f;
    startMidiNote = 0.0f;
    targetMidiNote = 0.0f;
    glideSamples = 0;
    glideTotalSamples = 0;
    stopFadeSamples = 0;
    stopFadeTotalSamples = 0;
    randomOnNote = 0.0f;
    modEnvelope.reset();
    lfo.resetPhase();
    ramp.reset();
    for (auto& envelope : partAmpEnvelopes) envelope.reset();
    for (auto& part : partFilters)
        for (auto& channel : part) channel.reset();
    modEnvelope2.reset();
    lfo2.resetPhase();
    nativeModulation.fill(0.0f);
    nativeModEnv2Value = nativeAmpEnv2Value = nativeLfo2Value = 0.0f;
    nativeLfo1PhaseOffset = nativeLfo2PhaseOffset = 0.0f;
    lastDirectSums = {};
    lastTransModSums = {};
    lastRampValue = 0.0f;
    controlCountdown = 0;
    controlPosition = 0;
    controlPrimed = false;
    modulatorConfigInitialized = false;
}

void Voice::process(int numSamples) noexcept
{
    for (int index = 0; index < numSamples; ++index)
    {
        modEnvelope.process();
        lfo.process();
        {
            for (auto& envelope : partAmpEnvelopes) envelope.process();
            modEnvelope2.process();
            lfo2.process();
        }
    }
    const auto envelopeActive = partAmpEnvelopes[0].isActive() || partAmpEnvelopes[1].isActive();
    if (state == VoiceState::Releasing && !envelopeActive && stopFadeSamples <= 0) reset();
}

void Voice::syncModulatorConfig(const SynthParameters& parameters) noexcept
{
    for (std::size_t part = 0; part < partAmpEnvelopes.size(); ++part)
    {
        partAmpEnvelopes[part].setSettings(toEnvelopeSettings(parameters.layers[part].ampEnv));
        auto settings = parameters.layers[part].filter;
        settings.warmDrive = parameters.filterControl.warmDrive;
        settings.selfOscillation = true;
        settings.nativeTopology = true;
        settings.cutoffSemitones += parameters.filterControl.cutoffSemitones;
        settings.keytrack = clampFast(settings.keytrack + parameters.filterControl.keytrack, -1.0f, 2.0f);
        settings.resonance = clampUnitFast(settings.resonance + parameters.filterControl.resonance);
        settings.drive = clampUnitFast(settings.drive + parameters.filterControl.drive);
        preparedPartFilters[part] = settings;
        for (auto& channel : partFilters[part]) channel.prepareBlock(settings);
    }
    modEnvelope.setSettings(toEnvelopeSettings(parameters.modEnv));
    lfo.setPhaseDegrees(parameters.lfo.phaseDegrees);
    lfo.setSteps(parameters.lfo.steps.data(), parameters.lfo.stepCount, parameters.lfo.stepSmooth);
    modEnvelope2.setSettings(toEnvelopeSettings(parameters.modEnv2));
    const auto rate = [&parameters](const LfoParameters& settings, float tempo) noexcept {
        return settings.free || !parameters.sync ? settings.rateHz
                                                 : tempo / (60.0f * syncDivisionBeats(settings.syncDivision));
    };
    lfo.setShape(parameters.lfo.shape == LfoShapeChoice::Noise ? LfoShape::Noise : toLfoShape(parameters.lfo.shape));
    lfo.setRateHz(rate(parameters.lfo, parameters.tempoBpm));
    lfo2.setRateHz(rate(parameters.lfo2, parameters.tempoBpm));
    lfo2.setShape(parameters.lfo2.shape == LfoShapeChoice::Noise ? LfoShape::Noise : toLfoShape(parameters.lfo2.shape));
    lfo2.setPhaseDegrees(parameters.lfo2.phaseDegrees);
    lfo2.setSteps(parameters.lfo2.steps.data(), parameters.lfo2.stepCount, parameters.lfo2.stepSmooth);
    modulatorConfigInitialized = true;
}

void VoiceModPlan::build(const TransModParameters& transMod) noexcept
{
    routeCount = 0;
    anyLegacyDepth = false;
    live.fill(false);
    for (int slotIndex = 0; slotIndex < transModSlotCount; ++slotIndex)
    {
        const auto& slot = transMod.slots[static_cast<std::size_t>(slotIndex)];
        if (!slot.enabled || slot.source == ModSource::None)
            continue;

        auto& route = routes[static_cast<std::size_t>(routeCount++)];
        route.slot = slotIndex;
        route.legacyDepth = slot.oscPitchSemitones != 0.0f || slot.pulseWidth != 0.0f || slot.filterCutoffSemitones != 0.0f
            || slot.depth != 0.0f || slot.ampLevelDb != 0.0f || slot.pan != 0.0f;
        anyLegacyDepth = anyLegacyDepth || route.legacyDepth;
        route.destinationCount = 0;
        // A zero depth adds nothing to a destination's sum, so leaving it out keeps the sum exact.
        for (int destination = 0; destination < nativeModDestinationCount; ++destination)
        {
            const auto depth = slot.nativeDepths[static_cast<std::size_t>(destination)];
            if (depth == 0.0f)
                continue;
            const auto entry = static_cast<std::size_t>(route.destinationCount++);
            route.destinations[entry] = static_cast<std::uint8_t>(destination);
            route.depths[entry] = depth;
            live[static_cast<std::size_t>(destination)] = true;
        }
    }
    liveCount = 0;
    for (int destination = 0; destination < nativeModDestinationCount; ++destination)
        if (live[static_cast<std::size_t>(destination)])
            liveDestinations[static_cast<std::size_t>(liveCount++)] = static_cast<std::uint8_t>(destination);
}

int Voice::renderBlock(const SynthParameters& parameters, const VoiceModPlan& plan, const LfoFrame* monoLfoValues,
                       const LfoFrame* monoLfo2Values, int numSamples, const VoiceBlockOutput& output) noexcept
{
    numSamples = clampIntFast(numSamples, 0, renderBlockMaxSamples);
    if (!isActive() || numSamples == 0)
        return 0;
    if (!modulatorConfigInitialized)
        syncModulatorConfig(parameters);

    using Lane = std::array<float, renderBlockMaxSamples>;
    static const Lane zeroLane {};
    Lane noteLane, pitchLane, cutoffLane, fadeLane, transModLevelDbLane, transModPanLane;
    std::array<Lane, layerCount> ampLane;
    std::array<Lane, layerCount> coefficientLane, leakLane; // control rate only
    std::array<Lane, nativeModDestinationCount> modLanes; // filled only for live destinations
    const auto lane = [&plan, &modLanes](int destination) noexcept -> const float* {
        const auto index = static_cast<std::size_t>(destination);
        return plan.live[index] ? modLanes[index].data() : zeroLane.data();
    };
    const auto destination = [this](NativeModDestination id) noexcept {
        return nativeModulation[static_cast<std::size_t>(id)];
    };
    const auto rate = [&parameters](const LfoParameters& settings, float tempo) noexcept {
        return settings.free || !parameters.sync ? settings.rateHz
                                                 : tempo / (60.0f * syncDivisionBeats(settings.syncDivision));
    };
    const auto baseRate1 = clampFast(rate(parameters.lfo, parameters.tempoBpm), 0.01f, 100.0f);
    const auto baseRate2 = clampFast(rate(parameters.lfo2, parameters.tempoBpm), 0.01f, 100.0f);
    const auto interval = clampIntFast(parameters.modulationIntervalSamples, 1, renderBlockMaxSamples);
    const auto exactPath = interval == 1;

    // Modulation, one sample at a time: the LFO rate, gain and offset destinations take the previous
    // sample's modulation, so this part cannot run as a block.
    auto endSample = numSamples;  // the sample at which the voice ends
    auto renderedSamples = numSamples; // the samples the oscillators and filters render
    auto silentEnd = false;       // the voice ended before rendering endSample
    for (int i = 0; i < numSamples; ++i)
    {
        const auto sample = static_cast<std::size_t>(i);
        output.weight[i] = isStopFading() ? normalizationPowerWeight() : 1.0f;
        const auto note = processGlide(parameters);
        const auto glidedVelocity = processVelocityGlide(parameters);
        std::array<float, layerCount> ampValues {};
        auto anyEnvelopeActive = false;
        for (std::size_t part = 0; part < partAmpEnvelopes.size(); ++part)
        {
            ampValues[part] = partAmpEnvelopes[part].process();
            anyEnvelopeActive = anyEnvelopeActive || partAmpEnvelopes[part].isActive();
        }
        if (state == VoiceState::Releasing && !anyEnvelopeActive && stopFadeSamples <= 0)
        {
            endSample = renderedSamples = i;
            silentEnd = true;
            break;
        }
        const auto mod = modEnvelope.process();
        nativeModEnv2Value = modEnvelope2.process();
        nativeAmpEnv2Value = ampValues[1];
        if (exactPath)
        {
            const auto effectiveRate1 = clampFast(baseRate1 + destination(NativeModDestination::Lfo1Rate), 0.01f, 100.0f);
            const auto effectiveRate2 = clampFast(baseRate2 + destination(NativeModDestination::Lfo2Rate), 0.01f, 100.0f);
            lfo.setRateHz(effectiveRate1);
            lfo2.setRateHz(effectiveRate2);
            const auto* monoLfoValue = monoLfoValues != nullptr ? &monoLfoValues[i] : nullptr;
            const auto* monoLfo2Value = monoLfo2Values != nullptr ? &monoLfo2Values[i] : nullptr;
            if (monoLfoValue != nullptr)
            {
                nativeLfo1PhaseOffset += (effectiveRate1 - baseRate1) / static_cast<float>(sampleRate);
                nativeLfo1PhaseOffset -= std::floor(nativeLfo1PhaseOffset);
                lfo.setNormalizedPhase(monoLfoValue->phase + nativeLfo1PhaseOffset);
            }
            if (monoLfo2Value != nullptr)
            {
                nativeLfo2PhaseOffset += (effectiveRate2 - baseRate2) / static_cast<float>(sampleRate);
                nativeLfo2PhaseOffset -= std::floor(nativeLfo2PhaseOffset);
                lfo2.setNormalizedPhase(monoLfo2Value->phase + nativeLfo2PhaseOffset);
            }
            const auto lfoValue = clampFast((monoLfoValue == nullptr ? lfo.process() : (nativeLfo1PhaseOffset == 0.0f ? monoLfoValue->value : lfo.getValue()))
                                                    * clampFast(parameters.lfo.gain + destination(NativeModDestination::Lfo1Gain), 0.0f, 2.0f)
                                                + parameters.lfo.offset + destination(NativeModDestination::Lfo1Offset),
                                            -2.0f, 2.0f);
            nativeLfo2Value = clampFast((monoLfo2Value == nullptr ? lfo2.process() : (nativeLfo2PhaseOffset == 0.0f ? monoLfo2Value->value : lfo2.getValue()))
                                                * clampFast(parameters.lfo2.gain + destination(NativeModDestination::Lfo2Gain), 0.0f, 2.0f)
                                            + parameters.lfo2.offset + destination(NativeModDestination::Lfo2Offset),
                                        -2.0f, 2.0f);
            const auto rampValue = ramp.process(parameters.ramp, parameters.tempoBpm);
            lastRampValue = rampValue;

            std::array<float, transModSlotCount> amounts {};
            std::array<bool, transModSlotCount> amountsFinite {};
            for (int routeIndex = 0; routeIndex < plan.routeCount; ++routeIndex)
            {
                const auto& slot = parameters.transMod.slots[static_cast<std::size_t>(plan.routes[static_cast<std::size_t>(routeIndex)].slot)];
                const auto source = evalModSource(parameters, slot.source, lfoValue, rampValue, mod, ampValues[0], note, glidedVelocity);
                const auto scaler = slot.scaler == ModSource::None ? 1.0f
                                                                   : evalModSource(parameters, slot.scaler, lfoValue, rampValue, mod, ampValues[0], note, glidedVelocity);
                amounts[static_cast<std::size_t>(routeIndex)] = source * scaler;
                amountsFinite[static_cast<std::size_t>(routeIndex)] = std::isfinite(source * scaler);
            }
            lastTransModSums = evaluateTransMod(parameters, plan, amounts.data(), amountsFinite.data());
            nativeModulation.fill(0.0f);
            for (int routeIndex = 0; routeIndex < plan.routeCount; ++routeIndex)
            {
                if (!amountsFinite[static_cast<std::size_t>(routeIndex)])
                    continue;
                const auto& route = plan.routes[static_cast<std::size_t>(routeIndex)];
                const auto amount = amounts[static_cast<std::size_t>(routeIndex)];
                for (int entry = 0; entry < route.destinationCount; ++entry)
                    nativeModulation[route.destinations[static_cast<std::size_t>(entry)]] += amount * route.depths[static_cast<std::size_t>(entry)];
            }
            const auto keytrack = (note - 60.0f) / 12.0f;
            const auto pitch = keytrack * parameters.direct.oscKeytrackSemitones
                + lfoValue * parameters.direct.oscLfoSemitones + mod * parameters.direct.oscModEnvSemitones
                + lastTransModSums.oscPitchSemitones
                + parameters.performance.pitchBend * parameters.pitchBendRange;
            const auto cutoff = (note - 60.0f) * parameters.direct.filterKeytrack
                + lfoValue * parameters.direct.filterLfoSemitones + mod * parameters.direct.filterModEnvSemitones
                + lastTransModSums.filterCutoffSemitones;
            lastDirectSums.oscPitchSemitones = pitch - lastTransModSums.oscPitchSemitones;
            lastDirectSums.filterCutoffSemitones = cutoff - lastTransModSums.filterCutoffSemitones;

            noteLane[sample] = note;
            pitchLane[sample] = pitch;
            cutoffLane[sample] = cutoff;
            for (std::size_t part = 0; part < ampLane.size(); ++part)
                ampLane[part][sample] = ampValues[part];
            transModLevelDbLane[sample] = lastTransModSums.ampLevelDb;
            transModPanLane[sample] = lastTransModSums.pan;
            for (int entry = 0; entry < plan.liveCount; ++entry)
            {
                const auto id = plan.liveDestinations[static_cast<std::size_t>(entry)];
                modLanes[id][sample] = nativeModulation[id];
            }
            output.phaserCenterOffsetHz[i] = clampFast(destination(NativeModDestination::PhaserCenterFrequency), -20000.0f, 20000.0f);
        }
        else
        {
            // Control rate: a new modulation frame every `interval` samples, from the modulators'
            // state at that sample; in between, each modulated value ramps from the previous frame
            // to the new one. The envelopes, glide and fade above stay exact per sample.
            const auto* monoLfoValue = monoLfoValues != nullptr ? &monoLfoValues[i] : nullptr;
            const auto* monoLfo2Value = monoLfo2Values != nullptr ? &monoLfo2Values[i] : nullptr;
            if (controlCountdown <= 0)
            {
                const auto elapsed = static_cast<float>(controlPrimed ? interval : 1);
                const auto effectiveRate1 = clampFast(baseRate1 + destination(NativeModDestination::Lfo1Rate), 0.01f, 100.0f);
                const auto effectiveRate2 = clampFast(baseRate2 + destination(NativeModDestination::Lfo2Rate), 0.01f, 100.0f);
                lfo.setRateHz(effectiveRate1);
                lfo2.setRateHz(effectiveRate2);
                if (monoLfoValue != nullptr)
                {
                    nativeLfo1PhaseOffset += elapsed * (effectiveRate1 - baseRate1) / static_cast<float>(sampleRate);
                    nativeLfo1PhaseOffset -= std::floor(nativeLfo1PhaseOffset);
                    lfo.setNormalizedPhase(monoLfoValue->phase + nativeLfo1PhaseOffset);
                }
                if (monoLfo2Value != nullptr)
                {
                    nativeLfo2PhaseOffset += elapsed * (effectiveRate2 - baseRate2) / static_cast<float>(sampleRate);
                    nativeLfo2PhaseOffset -= std::floor(nativeLfo2PhaseOffset);
                    lfo2.setNormalizedPhase(monoLfo2Value->phase + nativeLfo2PhaseOffset);
                }
                const auto lfoValue = clampFast((monoLfoValue == nullptr ? lfo.process() : (nativeLfo1PhaseOffset == 0.0f ? monoLfoValue->value : lfo.getValue()))
                                                    * clampFast(parameters.lfo.gain + destination(NativeModDestination::Lfo1Gain), 0.0f, 2.0f)
                                                + parameters.lfo.offset + destination(NativeModDestination::Lfo1Offset),
                                            -2.0f, 2.0f);
                nativeLfo2Value = clampFast((monoLfo2Value == nullptr ? lfo2.process() : (nativeLfo2PhaseOffset == 0.0f ? monoLfo2Value->value : lfo2.getValue()))
                                                * clampFast(parameters.lfo2.gain + destination(NativeModDestination::Lfo2Gain), 0.0f, 2.0f)
                                            + parameters.lfo2.offset + destination(NativeModDestination::Lfo2Offset),
                                        -2.0f, 2.0f);
                const auto rampValue = ramp.process(parameters.ramp, parameters.tempoBpm);
                lastRampValue = rampValue;

                std::array<float, transModSlotCount> amounts {};
                std::array<bool, transModSlotCount> amountsFinite {};
                for (int routeIndex = 0; routeIndex < plan.routeCount; ++routeIndex)
                {
                    const auto& slot = parameters.transMod.slots[static_cast<std::size_t>(plan.routes[static_cast<std::size_t>(routeIndex)].slot)];
                    const auto source = evalModSource(parameters, slot.source, lfoValue, rampValue, mod, ampValues[0], note, glidedVelocity);
                    const auto scaler = slot.scaler == ModSource::None ? 1.0f
                                                                       : evalModSource(parameters, slot.scaler, lfoValue, rampValue, mod, ampValues[0], note, glidedVelocity);
                    amounts[static_cast<std::size_t>(routeIndex)] = source * scaler;
                    amountsFinite[static_cast<std::size_t>(routeIndex)] = std::isfinite(source * scaler);
                }
                lastTransModSums = evaluateTransMod(parameters, plan, amounts.data(), amountsFinite.data());
                nativeModulation.fill(0.0f);
                for (int routeIndex = 0; routeIndex < plan.routeCount; ++routeIndex)
                {
                    if (!amountsFinite[static_cast<std::size_t>(routeIndex)])
                        continue;
                    const auto& route = plan.routes[static_cast<std::size_t>(routeIndex)];
                    const auto amount = amounts[static_cast<std::size_t>(routeIndex)];
                    for (int entry = 0; entry < route.destinationCount; ++entry)
                        nativeModulation[route.destinations[static_cast<std::size_t>(entry)]] += amount * route.depths[static_cast<std::size_t>(entry)];
                }
                ControlFrame frame;
                frame.pitch = lfoValue * parameters.direct.oscLfoSemitones + mod * parameters.direct.oscModEnvSemitones
                    + lastTransModSums.oscPitchSemitones + parameters.performance.pitchBend * parameters.pitchBendRange;
                frame.cutoff = lfoValue * parameters.direct.filterLfoSemitones + mod * parameters.direct.filterModEnvSemitones
                    + lastTransModSums.filterCutoffSemitones;
                frame.levelDb = lastTransModSums.ampLevelDb;
                frame.pan = lastTransModSums.pan;
                frame.phaserCenterOffsetHz = clampFast(destination(NativeModDestination::PhaserCenterFrequency), -20000.0f, 20000.0f);
                frame.modulation = nativeModulation;
                for (int partIndex = 0; partIndex < layerCount; ++partIndex)
                {
                    const auto part = static_cast<std::size_t>(partIndex);
                    if (!preparedPartFilters[part].enabled)
                        continue;
                    const auto cutoffModulation = (note - 60.0f) * parameters.direct.filterKeytrack + frame.cutoff
                        + clampFast(nativeModulation[static_cast<std::size_t>(20 + partIndex * 3)], -136.0f, 136.0f);
                    const auto& filter = partFilters[part][0];
                    filter.preparedCoefficients(filter.preparedCutoffSemitones(note, preparedPartFilters[part], cutoffModulation),
                                                frame.filterCoefficient[part], frame.filterHighpassLeak[part]);
                }
                lastDirectSums.oscPitchSemitones = (note - 60.0f) / 12.0f * parameters.direct.oscKeytrackSemitones
                    + frame.pitch - lastTransModSums.oscPitchSemitones;
                lastDirectSums.filterCutoffSemitones = (note - 60.0f) * parameters.direct.filterKeytrack
                    + frame.cutoff - lastTransModSums.filterCutoffSemitones;
                controlFrom = controlPrimed ? controlTo : frame;
                controlTo = frame;
                controlPrimed = true;
                controlCountdown = interval;
                controlPosition = 0;
            }
            else
            {
                if (monoLfoValue == nullptr) lfo.advance();
                if (monoLfo2Value == nullptr) lfo2.advance();
                ramp.advance();
            }
            --controlCountdown;
            ++controlPosition;
            const auto t = static_cast<float>(controlPosition) / static_cast<float>(interval);
            const auto blend = [t](float from, float to) noexcept { return from + (to - from) * t; };
            noteLane[sample] = note;
            pitchLane[sample] = (note - 60.0f) / 12.0f * parameters.direct.oscKeytrackSemitones
                + blend(controlFrom.pitch, controlTo.pitch);
            cutoffLane[sample] = (note - 60.0f) * parameters.direct.filterKeytrack
                + blend(controlFrom.cutoff, controlTo.cutoff);
            for (std::size_t part = 0; part < ampLane.size(); ++part)
                ampLane[part][sample] = ampValues[part];
            transModLevelDbLane[sample] = blend(controlFrom.levelDb, controlTo.levelDb);
            transModPanLane[sample] = blend(controlFrom.pan, controlTo.pan);
            for (int entry = 0; entry < plan.liveCount; ++entry)
            {
                const auto id = plan.liveDestinations[static_cast<std::size_t>(entry)];
                modLanes[id][sample] = blend(controlFrom.modulation[id], controlTo.modulation[id]);
            }
            output.phaserCenterOffsetHz[i] = blend(controlFrom.phaserCenterOffsetHz, controlTo.phaserCenterOffsetHz);
            for (std::size_t part = 0; part < coefficientLane.size(); ++part)
            {
                coefficientLane[part][sample] = blend(controlFrom.filterCoefficient[part], controlTo.filterCoefficient[part]);
                leakLane[part][sample] = blend(controlFrom.filterHighpassLeak[part], controlTo.filterHighpassLeak[part]);
            }
        }
        fadeLane[sample] = 1.0f;
        if (stopFadeSamples > 0)
        {
            fadeLane[sample] = static_cast<float>(stopFadeSamples) / static_cast<float>(std::max(1, stopFadeTotalSamples));
            if (--stopFadeSamples <= 0)
            {
                endSample = i;
                renderedSamples = i + 1;
                break;
            }
        }
    }

    // Audio, one module at a time over the samples the modulation produced.
    const auto count = renderedSamples;
    const auto isConstant = [count](const float* values) noexcept {
        for (int i = 1; i < count; ++i)
            if (values[i] != values[0])
                return false;
        return true;
    };
    std::array<Lane, layerCount> sourceLeft {}, sourceRight {};
    Lane oscillatorLeft, oscillatorRight, oscillatorPitch, oscillatorLevel, oscillatorPan, oscillatorDetune;
    for (int partIndex = 0; partIndex < layerCount && count > 0; ++partIndex)
    {
        const auto part = static_cast<std::size_t>(partIndex);
        for (int oscillatorIndex = 0; oscillatorIndex < oscillatorSlotsPerLayer; ++oscillatorIndex)
        {
            const auto& slot = parameters.layers[part].oscillators[static_cast<std::size_t>(oscillatorIndex)];
            if (slot.voices <= 0)
                continue; // renders silence and leaves the oscillator untouched
            const auto index = partIndex * oscillatorSlotsPerLayer + oscillatorIndex;
            const auto offset = index * 5;
            const auto* pitchMod = lane(offset);
            const auto* levelMod = lane(offset + 1);
            const auto* panMod = lane(offset + 2);
            const auto* detuneMod = lane(offset + 3);
            const auto* phaseMod = lane(offset + 4);
            // Volume modulation adds to the oscillator's linear level (renderNative clamps it to 0..4).
            const auto levelLive = plan.live[static_cast<std::size_t>(offset + 1)];
            for (int i = 0; i < count; ++i)
            {
                const auto sample = static_cast<std::size_t>(i);
                oscillatorPitch[sample] = pitchLane[sample] + clampFast(pitchMod[i], -120.0f, 120.0f);
                oscillatorLevel[sample] = levelLive ? slot.level + clampFast(levelMod[i], -2.0f, 2.0f) : slot.level;
                oscillatorPan[sample] = clampFast(slot.pan + panMod[i], -1.0f, 1.0f);
                oscillatorDetune[sample] = clampUnitFast(slot.detune + detuneMod[i]);
            }
            const auto steady = isConstant(oscillatorPan.data()) && isConstant(oscillatorDetune.data());
            layerOscillators[static_cast<std::size_t>(index)].renderNativeBlock(
                noteLane.data(), slot, oscillatorPitch.data(), oscillatorLevel.data(), oscillatorPan.data(),
                oscillatorDetune.data(), phaseMod, steady, count, oscillatorLeft.data(), oscillatorRight.data());
            for (int i = 0; i < count; ++i)
            {
                sourceLeft[part][static_cast<std::size_t>(i)] += oscillatorLeft[static_cast<std::size_t>(i)];
                sourceRight[part][static_cast<std::size_t>(i)] += oscillatorRight[static_cast<std::size_t>(i)];
            }
        }
    }

    Lane voiceLeft {}, voiceRight {}, inputLeft, inputRight;
    const auto solo = parameters.layers[0].solo || parameters.layers[1].solo;
    for (int partIndex = 0; partIndex < layerCount && count > 0; ++partIndex)
    {
        const auto part = static_cast<std::size_t>(partIndex);
        const auto& layer = parameters.layers[part];
        if (layer.mute || (solo && !layer.solo)) continue;
        const auto fromA = layer.input == FilterInput::A || layer.input == FilterInput::AB;
        const auto fromB = layer.input == FilterInput::B || layer.input == FilterInput::AB;
        for (int i = 0; i < count; ++i)
        {
            const auto sample = static_cast<std::size_t>(i);
            inputLeft[sample] = fromA ? sourceLeft[0][sample] : 0.0f;
            inputRight[sample] = fromA ? sourceRight[0][sample] : 0.0f;
            if (fromB)
            {
                inputLeft[sample] += sourceLeft[1][sample];
                inputRight[sample] += sourceRight[1][sample];
            }
        }

        const auto filterOffset = 20 + partIndex * 3;
        auto settings = preparedPartFilters[part];
        auto& leftFilter = partFilters[part][0];
        auto& rightFilter = partFilters[part][1];
        if (!settings.enabled)
        {
            for (auto& channel : partFilters[part]) channel.prepareBlock(settings); // passes the input through
        }
        else
        {
            const auto* cutoffMod = lane(filterOffset);
            const auto* resonanceMod = lane(filterOffset + 1);
            const auto* driveMod = lane(filterOffset + 2);
            const auto* resonanceModAB = lane(static_cast<int>(NativeModDestination::FilterResonanceAB));
            const auto baseResonance = settings.resonance;
            const auto baseDrive = settings.drive;
            for (int i = 0; i < count; ++i)
            {
                const auto sample = static_cast<std::size_t>(i);
                const auto resonance = clampUnitFast(baseResonance + resonanceMod[i] + resonanceModAB[i]);
                const auto drive = clampUnitFast(baseDrive + driveMod[i]);
                // Preparing again with the same values changes nothing, so only a change is passed on.
                if (i == 0 || resonance != settings.resonance || drive != settings.drive)
                {
                    settings.resonance = resonance;
                    settings.drive = drive;
                    leftFilter.prepareBlock(settings);
                    rightFilter.prepareBlock(settings);
                }
                if (!exactPath)
                {
                    Filter::processRampedPair(leftFilter, rightFilter, inputLeft[sample], inputRight[sample],
                                              coefficientLane[part][sample], leakLane[part][sample]);
                    continue;
                }
                const auto cutoffModulation = cutoffLane[sample] + clampFast(cutoffMod[i], -136.0f, 136.0f);
                Filter::processPreparedPair(leftFilter, rightFilter, inputLeft[sample], inputRight[sample],
                                            noteLane[sample], settings, cutoffModulation);
            }
        }

        // Part level modulation is added to the part's linear gain, as the reference synth adds it to the Mix knob
        // (the gate recipe in its manual sets Mix to 0 and lets Step Velocity supply the whole level).
        // MEASURED (the reference synth 3.0.7.3, sweeps/lfodepth, sweeps/velocity): +-0.5 of gain at full scale for
        // Volume, 1.33 for Mix; note velocity itself does not change the level.
        const auto layerOffset = 26 + partIndex * 2;
        const auto* levelMod = lane(layerOffset);
        const auto* panMod = lane(layerOffset + 1);
        auto lastLevelDb = 0.0f, baseGain = 0.0f, lastPan = 0.0f, panLeft = 1.0f, panRight = 1.0f;
        for (int i = 0; i < count; ++i)
        {
            const auto sample = static_cast<std::size_t>(i);
            const auto levelDb = layer.levelDb + transModLevelDbLane[sample];
            if (i == 0 || levelDb != lastLevelDb)
            {
                lastLevelDb = levelDb;
                baseGain = levelDb <= -48.0f ? 0.0f : decibelsToGain(levelDb);
            }
            const auto gain = ampLane[part][sample] * std::max(0.0f, baseGain + clampFast(levelMod[i], -2.0f, 2.0f));
            const auto pan = clampFast(layer.pan + panMod[i] + transModPanLane[sample], -1.0f, 1.0f);
            if (i == 0 || pan != lastPan)
            {
                // A stereo balance preserves the independently rendered channels at center.
                lastPan = pan;
                panLeft = pan >= 1.0f ? 0.0f : (pan > 0.0f ? std::cos(pan * halfPi) : 1.0f);
                panRight = pan <= -1.0f ? 0.0f : (pan < 0.0f ? std::cos(-pan * halfPi) : 1.0f);
            }
            voiceLeft[sample] += inputLeft[sample] * gain * panLeft;
            voiceRight[sample] += inputRight[sample] * gain * panRight;
        }
    }

    for (int i = 0; i < count; ++i)
    {
        const auto sample = static_cast<std::size_t>(i);
        output.left[i] = sanitize(voiceLeft[sample]) * fadeLane[sample];
        output.right[i] = sanitize(voiceRight[sample]) * fadeLane[sample];
    }
    if (silentEnd)
    {
        output.left[endSample] = 0.0f;
        output.right[endSample] = 0.0f;
        output.phaserCenterOffsetHz[endSample] = 0.0f;
    }
    if (endSample < numSamples)
        reset();
    return endSample;
}

void Voice::syncModulators(const SynthParameters& parameters) noexcept
{
    if (state != VoiceState::Idle)
        syncModulatorConfig(parameters);
}

void Voice::resetLayerOscillators(const SynthParameters& parameters, float fallbackPhase) noexcept
{
    for (int part = 0; part < layerCount; ++part)
    {
        for (int index = 0; index < oscillatorSlotsPerLayer; ++index)
        {
            const auto& slot = parameters.layers[static_cast<std::size_t>(part)].oscillators[static_cast<std::size_t>(index)];
            if (!slot.retrigger) continue;
            const auto phase = std::isfinite(slot.phaseDegrees) ? slot.phaseDegrees / 360.0f : fallbackPhase;
            layerOscillators[static_cast<std::size_t>(layerOscillatorIndex(part, index))].resetNativePhase(phase);
        }
    }
}

float Voice::processGlide(const SynthParameters& parameters) noexcept
{
    if (glideTotalSamples <= 0)
    {
        currentMidiNote = targetMidiNote;
        return currentMidiNote;
    }

    const auto phase = glideTotalSamples <= 1
        ? 1.0f
        : clampUnitFast(static_cast<float>(glideSamples) / static_cast<float>(glideTotalSamples - 1));
    currentMidiNote = startMidiNote + (targetMidiNote - startMidiNote) * phase;
    if (glideSamples < glideTotalSamples - 1)
        ++glideSamples;
    else
        currentMidiNote = targetMidiNote;

    if (!std::isfinite(currentMidiNote))
        currentMidiNote = static_cast<float>(midiNote);

    (void)parameters;
    return currentMidiNote;
}

float Voice::processVelocityGlide(const SynthParameters& parameters) noexcept
{
    if (velocityGlideTotalSamples <= 0)
    {
        currentVelocity = velocity;
        return currentVelocity;
    }

    const auto phase = velocityGlideTotalSamples <= 1
        ? 1.0f
        : clampUnitFast(static_cast<float>(velocityGlideSamples) / static_cast<float>(velocityGlideTotalSamples - 1));
    currentVelocity = startVelocity + (velocity - startVelocity) * phase;
    if (velocityGlideSamples < velocityGlideTotalSamples - 1)
        ++velocityGlideSamples;
    else
        currentVelocity = velocity;

    if (!std::isfinite(currentVelocity))
        currentVelocity = velocity;

    currentVelocity = clampUnitFast(currentVelocity);
    (void)parameters;
    return currentVelocity;
}

float Voice::evalModSource(const SynthParameters& parameters, ModSource source, float lfoValue, float rampValue,
                           float modEnvValue, float ampEnvValue, float effectiveNote,
                           float velocityGlideValue) const noexcept
{
    switch (source)
    {
        case ModSource::None:
            return 0.0f;
        case ModSource::Lfo:
            return clampFast(lfoValue, -2.0f, 2.0f);
        case ModSource::Lfo2:
            return nativeLfo2Value;
        case ModSource::ModEnv2:
            return nativeModEnv2Value;
        case ModSource::AmpEnv2:
            return nativeAmpEnv2Value;
        case ModSource::StepVelocity:
            return parameters.performance.stepVelocity;
        case ModSource::Ramp:
            return clampUnitFast(rampValue);
        case ModSource::ModEnv:
            return clampUnitFast(modEnvValue);
        case ModSource::AmpEnv:
            return clampUnitFast(ampEnvValue);
        case ModSource::Keytrack:
            return clampFast((effectiveNote - 60.0f) / 36.0f, -1.0f, 1.0f);
        case ModSource::Velocity:
            return clampUnitFast(velocity);
        case ModSource::VelocityGlide:
            return clampUnitFast(velocityGlideValue);
        case ModSource::PitchBend:
            return clampFast(parameters.performance.pitchBend, -1.0f, 1.0f);
        case ModSource::ModWheel:
            return clampUnitFast(parameters.performance.modWheel);
        case ModSource::Aftertouch:
            return clampUnitFast(parameters.performance.aftertouch);
        case ModSource::VoiceUni:
            return cachedVoiceUni;
        case ModSource::VoiceBi:
            return cachedVoiceBi;
        case ModSource::UnisonUni:
            return cachedUnisonUni;
        case ModSource::UnisonBi:
            return cachedUnisonBi;
        case ModSource::RandomOnNote:
            return clampFast(randomOnNote, -1.0f, 1.0f);
        case ModSource::Macro1:
            return clampUnitFast(parameters.macro.motion);
        case ModSource::Macro2:
            return clampUnitFast(parameters.macro.width);
        case ModSource::Macro3:
            return clampUnitFast(parameters.macro.drive);
        case ModSource::Macro4:
            return clampUnitFast(parameters.macro.space);
    }

    return 0.0f;
}

Voice::ModulationSums Voice::evaluateTransMod(const SynthParameters& parameters, const VoiceModPlan& plan,
                                              const float* amounts, const bool* amountsFinite) const noexcept
{
    // A slot with no legacy depth adds zero to every sum, so it can be left out exactly.
    if (!plan.anyLegacyDepth)
        return {};

    ModulationSums sums;
    if (parameters.transMod.activeSlotCacheValid)
    {
        // The engine builds activeSlots from the same enabled slots, in the same order, as the plan.
        const auto activeSlotCount = std::min(clampIntFast(parameters.transMod.activeSlotCount, 0, transModSlotCount),
                                              plan.routeCount);
        if (activeSlotCount <= 0)
            return {};

        for (int routeIndex = 0; routeIndex < activeSlotCount; ++routeIndex)
        {
            if (!amountsFinite[routeIndex] || !plan.routes[static_cast<std::size_t>(routeIndex)].legacyDepth)
                continue;
            const auto& slot = parameters.transMod.activeSlots[static_cast<std::size_t>(routeIndex)];
            const auto amount = amounts[routeIndex];
            sums.oscPitchSemitones += amount * slot.oscPitchSemitones;
            sums.pulseWidth += amount * slot.pulseWidth;
            sums.filterCutoffSemitones += amount * (slot.filterCutoffSemitones + slot.depth * 72.0f);
            sums.ampLevelDb += amount * slot.ampLevelDb;
            sums.pan += amount * slot.pan;
        }
    }
    else
    {
        for (int routeIndex = 0; routeIndex < plan.routeCount; ++routeIndex)
        {
            if (!amountsFinite[routeIndex] || !plan.routes[static_cast<std::size_t>(routeIndex)].legacyDepth)
                continue;
            const auto& slot = parameters.transMod.slots[static_cast<std::size_t>(plan.routes[static_cast<std::size_t>(routeIndex)].slot)];
            const auto amount = amounts[routeIndex];
            sums.oscPitchSemitones += amount * clampFast(slot.oscPitchSemitones, -48.0f, 48.0f);
            sums.pulseWidth += amount * clampFast(slot.pulseWidth, -1.0f, 1.0f);
            sums.filterCutoffSemitones += amount
                * (clampFast(slot.filterCutoffSemitones, -72.0f, 72.0f) + clampFast(slot.depth, -1.0f, 1.0f) * 72.0f);
            sums.ampLevelDb += amount * clampFast(slot.ampLevelDb, -48.0f, 48.0f);
            sums.pan += amount * clampFast(slot.pan, -1.0f, 1.0f);
        }
    }

    sums.oscPitchSemitones = clampFast(sanitize(sums.oscPitchSemitones), -96.0f, 96.0f);
    sums.pulseWidth = clampFast(sanitize(sums.pulseWidth), -1.0f, 1.0f);
    sums.filterCutoffSemitones = clampFast(sanitize(sums.filterCutoffSemitones), -136.0f, 136.0f);
    sums.ampLevelDb = clampFast(sanitize(sums.ampLevelDb), -24.0f, 24.0f);
    sums.pan = clampFast(sanitize(sums.pan), -2.0f, 2.0f);
    return sums;
}

VoiceSnapshot Voice::snapshot() const noexcept
{
    return {
        state,
        midiNote,
        velocity,
        partAmpEnvelopes[0].getValue(),
        modEnvelope.getValue(),
        lfo.getValue(),
        lastRampValue,
        randomOnNote,
        currentMidiNote,
        currentVelocity,
        cachedVoiceUni,
        cachedVoiceBi,
        cachedUnisonUni,
        cachedUnisonBi,
        lastDirectSums.oscPitchSemitones,
        lastDirectSums.pulseWidth,
        lastDirectSums.filterCutoffSemitones,
        lastTransModSums.oscPitchSemitones,
        lastTransModSums.pulseWidth,
        lastTransModSums.filterCutoffSemitones,
        lastTransModSums.ampLevelDb,
        lastTransModSums.pan,
        lfo.getPhase()
    };
}
} // namespace synth
