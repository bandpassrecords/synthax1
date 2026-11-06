#include "LegacyPatchMapper.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string_view>

namespace synth::legacy
{
namespace
{
using F = Field;

float clamp01(float value) noexcept { return std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 0.0f; }
float bipolar(float normalized) noexcept { return clamp01(normalized) * 2.0f - 1.0f; }
float exponential(float normalized, float low, float high) noexcept
{
    return low * std::pow(high / low, clamp01(normalized));
}

// ------------------------------------------------------------------ knob curves
// MEASURED curves come from Per Ivar Nerseth's measurements of the reference synth
// (github.com/perivar/AudioVSTToolbox, PresetConverter, the preset class and Zebra2 adapter), reimplemented here; the formulas are fitted to values
// read off the reference synth's own display. ESTIMATED curves have no measurement yet and use the
// template's display range with a conventional shape. docs/LEGACY_IMPORT.md lists both.
namespace curves
{
// MEASURED from the reference synth 3.0.7.3 (research/parity, sweeps/env): stage time against knob
// value 0, 0.05 .. 1. Attack = time to full level; decay and release = time to fall 40 dB,
// which is how Synthax1's envelope (src/dsp/Envelope.cpp, same measured shapes) reads them.
// Measured with the analytic-signal envelope (a short RMS window ripples with the carrier and
// reads crossings early). The last points extend the measured x1.42-per-step trend where the
// sweep's 15 s notes ended before the stage did.
constexpr std::array<float, 21> attackTable { 1.0f, 12.0f, 29.0f, 53.0f, 87.0f, 135.0f, 202.0f, 298.0f, 433.0f, 624.0f, 894.0f, 1275.0f, 1818.0f, 2583.0f, 3660.0f, 5197.0f, 7329.0f, 10424.0f, 14825.0f, 21000.0f, 29800.0f };
constexpr std::array<float, 21> decayTable { 1.0f, 10.0f, 21.0f, 40.0f, 66.0f, 102.0f, 152.0f, 228.0f, 329.0f, 474.0f, 680.0f, 971.0f, 1384.0f, 1967.0f, 2790.0f, 3959.0f, 5590.0f, 7947.0f, 11272.0f, 16000.0f, 22700.0f };
constexpr std::array<float, 21> releaseTable { 1.0f, 10.0f, 22.0f, 40.0f, 66.0f, 103.0f, 155.0f, 228.0f, 331.0f, 478.0f, 684.0f, 977.0f, 1390.0f, 1974.0f, 2799.0f, 3973.0f, 5610.0f, 7975.0f, 11311.0f, 16023.0f, 22715.0f };

// Log-linear between the 21 measured points (the times grow exponentially with the knob).
float tableMs(const std::array<float, 21>& table, float n) noexcept
{
    const auto position = clamp01(n) * 20.0f;
    const auto index = std::min<std::size_t>(19, static_cast<std::size_t>(position));
    const auto fraction = position - static_cast<float>(index);
    return std::exp(std::log(table[index]) + (std::log(table[index + 1]) - std::log(table[index])) * fraction);
}

float attackMs(float n) noexcept { return tableMs(attackTable, n); }
float decayMs(float n) noexcept { return tableMs(decayTable, n); }
float releaseMs(float n) noexcept { return tableMs(releaseTable, n); }

// MEASURED. A part filter on its own displays 1.0011 * e^(9.9673 x), 1 Hz .. ~21.3 kHz.
// MEASURED. The Filter Control cutoff adds to a part's cutoff in the normalized domain:
// f = 0.064749 * e^(10.152717 * (part + control)), limited to 22028.47 Hz.
float combinedFilterHz(float part, float control) noexcept
{
    const auto hz = 0.064749088 * std::exp(10.152717 * (static_cast<double>(clamp01(part)) + clamp01(control)));
    return static_cast<float>(std::min(hz, 22028.47));
}

float lfoHz(float n) noexcept { return 0.0417f * std::exp(8.4352f * clamp01(n)); }       // MEASURED, free-running
float delayLowCutHz(float n) noexcept { return 3.2328f * std::exp(9.0109f * clamp01(n)); } // MEASURED
float delayTimeMs(float n) noexcept { return 1000.0f * std::exp(3.49f * (clamp01(n) - 1.0f)); } // MEASURED, Sync off; below 0.05 extrapolated
float delayHighCutHz(float n) noexcept { return 21120.0f * std::exp(-5.545f * clamp01(n)); } // MEASURED, falls as n rises
float eqBassHz(float n) noexcept { return 13.75f * std::exp(4.1589f * clamp01(n)); }      // MEASURED
float eqTrebleHz(float n) noexcept { return 440.0f * std::exp(4.1589f * clamp01(n)); }    // MEASURED

// MEASURED. Compressor ratio sampled from the 190-step display table; log-linear between
// knots, within 2% of every measured step.
float compRatio(float n) noexcept
{
    static constexpr std::array<std::pair<float, float>, 37> knots { {
        { 0.0f, 1.00f }, { 0.1119f, 1.12f }, { 0.1976f, 1.24f }, { 0.2690f, 1.36f }, { 0.3310f, 1.48f },
        { 0.3881f, 1.62f }, { 0.4452f, 1.79f }, { 0.5024f, 1.98f }, { 0.5595f, 2.23f }, { 0.6167f, 2.55f },
        { 0.6738f, 2.98f }, { 0.7310f, 3.59f }, { 0.7881f, 4.50f }, { 0.8452f, 6.04f }, { 0.8976f, 8.79f },
        { 0.9024f, 9.17f }, { 0.9071f, 9.59f }, { 0.9119f, 10.04f }, { 0.9167f, 10.54f }, { 0.9214f, 11.09f },
        { 0.9262f, 11.71f }, { 0.9310f, 12.39f }, { 0.9357f, 13.16f }, { 0.9405f, 14.03f }, { 0.9452f, 15.02f },
        { 0.9500f, 16.17f }, { 0.9548f, 17.50f }, { 0.9595f, 19.07f }, { 0.9643f, 20.96f }, { 0.9690f, 23.26f },
        { 0.9738f, 26.12f }, { 0.9786f, 29.79f }, { 0.9833f, 34.65f }, { 0.9881f, 41.42f }, { 0.9929f, 51.47f },
        { 0.9976f, 67.96f }, { 1.0f, 100.00f },
    } };
    const auto x = clamp01(n);
    std::size_t upper = 1;
    while (upper < knots.size() - 1 && knots[upper].first < x)
        ++upper;
    const auto [x0, r0] = knots[upper - 1];
    const auto [x1, r1] = knots[upper];
    const auto u = std::clamp((x - x0) / (x1 - x0), 0.0f, 1.0f);
    return std::exp(std::log(r0) + u * (std::log(r1) - std::log(r0)));
}

// MEASURED. Pitch modulation: stored 0..1 -> -120..+120 semitones (display +-1 = one octave).
constexpr float pitchModSemitonesAtFullScale = 120.0f;

// ESTIMATED: no measurement yet.
float glideMs(float n) noexcept { return 2000.0f * std::pow(clamp01(n), 3.0f); }
float chorusHz(float n) noexcept { return exponential(n, 0.01f, 27.5f); }
float phaserHz(float n) noexcept { return exponential(n, 20.0f, 20000.0f); }
float compAttackMs(float n) noexcept { return exponential(n, 0.1f, 300.0f); }
float compReleaseMs(float n) noexcept { return exponential(n, 1.0f, 500.0f); }
// Mixer levels: display 0..10, 5 assumed to be unity gain.
float levelDb(float n) noexcept
{
    const auto gain = clamp01(n) * 2.0f;
    return gain <= 0.0f ? -48.0f : std::max(-48.0f, 20.0f * std::log10(gain));
}
} // namespace curves

float hzToCutoffSemitones(float hz) noexcept
{
    constexpr float midiNoteZeroHz = 8.1757989f;
    return 12.0f * std::log2(std::max(hz, 1.0f) / midiNoteZeroHz);
}

// ------------------------------------------------------------------ tempo divisions
// MEASURED selector tables. Each entry is the lowest normalized value showing that label.
struct Division { float from; const char* label; };

constexpr std::array<Division, 19> arpDivisions { {
    { 0.0f, "1/1" }, { 0.0405f, "1/2D" }, { 0.0929f, "1/1T" }, { 0.15f, "1/2" }, { 0.2024f, "1/4D" },
    { 0.2595f, "1/2T" }, { 0.3119f, "1/4" }, { 0.369f, "1/8D" }, { 0.4214f, "1/4T" }, { 0.4786f, "1/8" },
    { 0.531f, "1/16D" }, { 0.5881f, "1/8T" }, { 0.6452f, "1/16" }, { 0.6976f, "1/32D" }, { 0.7548f, "1/16T" },
    { 0.8071f, "1/32" }, { 0.8643f, "1/64D" }, { 0.9167f, "1/32T" }, { 0.9738f, "1/64" },
} };

constexpr std::array<Division, 16> delayDivisions { {
    { 0.0f, "1/64" }, { 0.0143f, "1/32T" }, { 0.081f, "1/64D" }, { 0.1524f, "1/32" }, { 0.219f, "1/16T" },
    { 0.2905f, "1/32D" }, { 0.3571f, "1/16" }, { 0.4286f, "1/8T" }, { 0.4952f, "1/16D" }, { 0.5667f, "1/8" },
    { 0.6333f, "1/4T" }, { 0.7048f, "1/8D" }, { 0.7714f, "1/4" }, { 0.8429f, "1/2T" }, { 0.9095f, "1/4D" },
    { 0.981f, "1/2" },
} };

// LFO (and phaser LFO) synced rates: 36 evenly spaced steps, index = round(n * 35).
constexpr std::array<const char*, 36> lfoDivisions {
    "8/1D", "8/1", "4/1D", "8/1T", "4/1", "2/1D", "4/1T", "2/1", "1/1D", "2/1T", "1/1", "1/2D",
    "1/1T", "1/2", "1/4D", "1/2T", "1/4", "1/8D", "1/4T", "1/8", "1/16D", "1/8T", "1/16", "1/32D",
    "1/16T", "1/32", "1/64D", "1/32T", "1/64", "1/128D", "1/64T", "1/128", "1/256D", "1/128T", "1/256", "1/256T",
};

template <std::size_t N>
const char* divisionAt(const std::array<Division, N>& table, float n) noexcept
{
    const auto x = clamp01(n);
    const char* label = table.front().label;
    for (const auto& entry : table)
        if (x >= entry.from)
            label = entry.label;
    return label;
}

const char* lfoDivisionAt(float n) noexcept
{
    return lfoDivisions[static_cast<std::size_t>(std::lround(clamp01(n) * 35.0f))];
}

// ------------------------------------------------------------------ selector tables
const std::vector<std::string> oscWaves { "Sine", "Saw", "Triangle", "Pulse", "HPulse", "QPulse", "TriSaw", "Noise" };
const std::vector<std::string> oscWavesSynthax1 { "Sine", "Saw", "Triangle", "Pulse", "HalfPulse", "QuarterPulse", "SawTriangle", "Noise" };

const std::vector<std::string> lfoWaves { "Sine", "Saw", "Ramp", "Triangle", "Pulse", "HPulse", "QPulse", "TriSaw", "Lorenz", "SmpHold", "Random" };
struct LfoShape { const char* synthax1; bool exact; };
const std::array<LfoShape, 11> lfoWaveToSynthax1 {
    LfoShape { "Sine", true },     { "SawDown", true },  { "SawUp", true },  { "Triangle", true },
    LfoShape { "Square", true },   { "Square", false },  { "Square", false }, { "Triangle", false },
    LfoShape { "Noise", false },   { "SampleHold", true }, { "Noise", true }
};

const std::vector<std::string> modSources { "None", "Velocity", "ModWheel", "KeyTrack", "AmpEnv A", "AmpEnv B",
                                            "ModEnv 1", "ModEnv 2", "LFO 1", "LFO 2", "Aftertouch", "StepVelocity" };
const std::array<const char*, 12> modSourceToSynthax1 { "None", "Velocity", "ModWheel", "Keytrack", "AmpEnv", "AmpEnv2",
                                                       "ModEnv", "ModEnv2", "LFO", "LFO2", "Aftertouch", "StepVelocity" };

// Stored as index / 32. Indices 28..32 are not described by the template.
const std::vector<std::string> modDestinations {
    "None", "Volume A", "Volume B", "Volume AB", "Pitch A", "Pitch B", "Pitch AB", "Phase A", "Phase B",
    "Phase AB", "Pan A", "Pan B", "Pan AB", "Cutoff A", "Cutoff B", "Cutoff AB", "Reso A", "Reso B",
    "Reso AB", "Phaser Freq", "Mix A", "Mix B", "Mix AB", "LFO1 Rate", "LFO1 Gain", "LFO2 Rate", "LFO2 Gain",
    "Dist Amount", "Unknown 28", "Unknown 29", "Unknown 30", "Unknown 31", "Unknown 32"
};
constexpr int modDestinationSteps = 33;

const std::vector<std::string> arpModes { "Up", "Down", "Up/Down", "Up/Down 2", "Down/Up", "Down/Up 2", "Random", "Ordered", "Step", "Chord" };
const std::array<const char*, 10> arpModeToSynthax1 { "Up", "Down", "UpDown", "UpDownRepeat", "DownUp", "DownUpRepeat", "Random", "AsPlayed", "StepSequence", "StepChord" };

// Synthax1's sync choices (fx.delay_sync_division, lfo.sync_division and arp.rate share the labels).
const std::array<const char*, 18> syncLabels { "1/16", "1/8", "1/8D", "1/4", "1/2", "1/32", "1/32T", "1/32D", "1/16T",
                                               "1/16D", "1/8T", "1/4T", "1/4D", "1/2T", "1/2D", "1 bar", "1 barT", "1 barD" };

// Length in beats of "a/b", "a/bT", "a/bD" (the reference synth) or "1 bar", "1 barT", "1 barD" (Synthax1).
float beatsOf(std::string_view label) noexcept
{
    float beats = 4.0f;
    if (const auto slash = label.find('/'); slash != std::string_view::npos)
    {
        int numerator = 0;
        int denominator = 0;
        for (const auto ch : label.substr(0, slash))
            numerator = numerator * 10 + (ch - '0');
        for (const auto ch : label.substr(slash + 1))
            if (ch >= '0' && ch <= '9')
                denominator = denominator * 10 + (ch - '0');
        beats = denominator > 0 ? 4.0f * static_cast<float>(numerator) / static_cast<float>(denominator) : 4.0f;
    }
    if (label.back() == 'T')
        beats *= 2.0f / 3.0f;
    else if (label.back() == 'D')
        beats *= 1.5f;
    return beats;
}

const char* nearestSyncLabel(float beats) noexcept
{
    const char* best = syncLabels.front();
    float bestDistance = 1.0e9f;
    for (const auto* label : syncLabels)
    {
        const auto distance = std::abs(std::log2(beatsOf(label) / beats));
        if (distance < bestDistance)
        {
            bestDistance = distance;
            best = label;
        }
    }
    return best;
}

bool sameLength(std::string_view a, std::string_view b) noexcept
{
    return std::abs(beatsOf(a) - beatsOf(b)) < 1.0e-4f;
}

// ------------------------------------------------------------------ builder
class Builder
{
public:
    explicit Builder(const Patch& source) : patch(source) { mapped.name = source.name.empty() ? "Untitled" : source.name; }

    float raw(F field) const noexcept { return clamp01(patch[field]); }
    bool on(F field) const noexcept { return patch[field] >= 0.5f; }
    int index(F field, int count) const noexcept { return selectorIndex(patch[field], count); }

    void set(std::string id, float value) { mapped.assignments.push_back({ std::move(id), value, {} }); }
    void choose(std::string id, std::string label) { mapped.assignments.push_back({ std::move(id), 0.0f, std::move(label) }); }
    void flag(std::string id, bool value) { set(std::move(id), value ? 1.0f : 0.0f); }

    // Picks Synthax1's closest tempo division to a the reference synth one, noting a mismatch.
    void chooseDivision(std::string id, const char* legacyLabel, const std::string& what)
    {
        const auto* synthax1Label = nearestSyncLabel(beatsOf(legacyLabel));
        if (!sameLength(synthax1Label, legacyLabel))
            approximated(what + " " + legacyLabel + " played as " + synthax1Label);
        choose(std::move(id), synthax1Label);
    }

    // Sets a frequency, noting when it falls outside Synthax1's range.
    void setHz(std::string id, float hz, float minimum, float maximum, const std::string& what)
    {
        if (hz < minimum || hz > maximum)
        {
            char text[96];
            std::snprintf(text, sizeof text, " %.2f Hz is outside Synthax1's %.2f..%.0f Hz and was clamped", hz, minimum, maximum);
            approximated(what + text);
        }
        set(std::move(id), std::clamp(hz, minimum, maximum));
    }

    void approximated(std::string text)
    {
        if (audible)
            mapped.notes.push_back({ ImportNote::Kind::Approximated, std::move(text) });
    }
    void dropped(std::string text)
    {
        if (audible)
            mapped.notes.push_back({ ImportNote::Kind::Dropped, std::move(text) });
    }

    // While false, approximations are applied silently: the section cannot be heard
    // (an effect that is off, an LFO nothing listens to), so reporting it would be noise.
    bool audible = true;

    MappedPatch take() { return std::move(mapped); }

    const Patch& patch;

private:
    MappedPatch mapped;
};

// Scoped "can this section be heard" switch for import notes.
class Audibility
{
public:
    Audibility(Builder& builder, bool audible) : b(builder), saved(builder.audible) { b.audible = saved && audible; }
    ~Audibility() { b.audible = saved; }
    Audibility(const Audibility&) = delete;
    Audibility& operator=(const Audibility&) = delete;

private:
    Builder& b;
    bool saved;
};

struct OscFields { F detune, fine, invert, note, octave, pan, phase, retrig, stereo, voices, volume, wave; };
constexpr std::array<OscFields, 4> oscFields { {
    { F::OscA1Detune, F::OscA1Fine, F::OscA1Invert, F::OscA1Note, F::OscA1Octave, F::OscA1Pan, F::OscA1Phase, F::OscA1Retrig, F::OscA1Stereo, F::OscA1Voices, F::OscA1Volume, F::OscA1Wave },
    { F::OscA2Detune, F::OscA2Fine, F::OscA2Invert, F::OscA2Note, F::OscA2Octave, F::OscA2Pan, F::OscA2Phase, F::OscA2Retrig, F::OscA2Stereo, F::OscA2Voices, F::OscA2Volume, F::OscA2Wave },
    { F::OscB1Detune, F::OscB1Fine, F::OscB1Invert, F::OscB1Note, F::OscB1Octave, F::OscB1Pan, F::OscB1Phase, F::OscB1Retrig, F::OscB1Stereo, F::OscB1Voices, F::OscB1Volume, F::OscB1Wave },
    { F::OscB2Detune, F::OscB2Fine, F::OscB2Invert, F::OscB2Note, F::OscB2Octave, F::OscB2Pan, F::OscB2Phase, F::OscB2Retrig, F::OscB2Stereo, F::OscB2Voices, F::OscB2Volume, F::OscB2Wave },
} };

void mapOscillators(Builder& b)
{
    for (std::size_t slot = 0; slot < oscFields.size(); ++slot)
    {
        const auto& f = oscFields[slot];
        const auto prefix = "layer." + std::to_string(slot / 2 + 1) + ".osc." + std::to_string(slot % 2 + 1) + ".";
        b.set(prefix + "voices", static_cast<float>(b.index(f.voices, 9)));
        b.choose(prefix + "waveform", oscWavesSynthax1[static_cast<std::size_t>(b.index(f.wave, 8))]);
        b.set(prefix + "octave", static_cast<float>(b.index(f.octave, 7) - 3));
        b.set(prefix + "note", static_cast<float>(b.index(f.note, 15) - 7));
        b.set(prefix + "fine_cents", bipolar(b.raw(f.fine)) * 100.0f);
        b.set(prefix + "level", b.raw(f.volume));
        b.set(prefix + "phase_degrees", b.raw(f.phase) * 360.0f);
        b.set(prefix + "detune", b.raw(f.detune));
        b.set(prefix + "stereo", b.raw(f.stereo));
        b.set(prefix + "pan", bipolar(b.raw(f.pan)));
        b.flag(prefix + "retrigger", b.on(f.retrig));
        b.flag(prefix + "invert", b.on(f.invert));
    }
}

// The reference synth sums part and Filter Control cutoffs in the normalized domain (see curves).
// Synthax1 adds a shared semitone offset to each part cutoff, so the shared offset carries
// the control knob and each part cutoff is whatever makes the sum exact.
constexpr float semitonesPerUnit = 12.0f / 0.69314718f * 10.152717f;        // d(semitones)/d(normalized)
// The Filter Control knob adds nothing at its centre (5), as on the reference synth; the part knob alone then spans
// 0 to 176 semitones, which the filter clips at 136 (about 20 kHz), so a part knob above ~7.5 does nothing
// unless the control is turned down, exactly as on the original.
constexpr float neutralControl = 0.5f;
constexpr float partCutoffMaxSemitones = 176.0f;
constexpr float controlCutoffMaxSemitones = 88.0f;

float filterControlOffsetSemitones(float control) noexcept
{
    return std::clamp(semitonesPerUnit * (clamp01(control) - neutralControl), -controlCutoffMaxSemitones, controlCutoffMaxSemitones);
}

// MEASURED (the reference synth 3.0.7.3, sweeps/filter, 24 dB lowpass, resonance 8): the reference synth's resonance
// peak sits 1.79 semitones below the nominal cutoff of the formula above, while Synthax1's
// zero-delay-feedback ladder peaks near its cutoff. Mean over 31 points between 80 Hz and 10 kHz.
constexpr float legacyPeakOffsetSemitones = -1.79f;

float targetSemitones(const Builder& b, F cutoff) noexcept
{
    return hzToCutoffSemitones(curves::combinedFilterHz(b.raw(cutoff), b.raw(F::FilterCtlCutoff)))
         + legacyPeakOffsetSemitones;
}

// The shared offset is free as long as part + offset reproduces each active filter's cutoff,
// so pick the value closest to the control knob's own position that keeps both active part
// cutoffs inside 0..136 semitones.
float sharedOffsetSemitones(const Builder& b) noexcept
{
    auto low = -72.0f;
    auto high = 72.0f;
    for (const auto& [cutoff, type] : { std::pair { F::FilterACutoff, F::FilterAType }, std::pair { F::FilterBCutoff, F::FilterBType } })
    {
        if (b.index(type, 4) == 0)
            continue; // a bypassed filter imposes nothing
        const auto target = targetSemitones(b, cutoff);
        low = std::max(low, target - partCutoffMaxSemitones);
        high = std::min(high, target);
    }
    const auto ideal = filterControlOffsetSemitones(b.raw(F::FilterCtlCutoff));
    return low <= high ? std::clamp(ideal, low, high) : ideal;
}

// MEASURED (the reference synth 3.0.7.3, sweeps/resosweep, noise through LP24 and LP12 at two cutoffs): the
// reference synth's resonance peak is much lower than Synthax1's at the same knob: peak over passband at knob 1.0
// is 16 dB (LP24) and 22 dB (LP12) against 31 and 37 dB here, and 14 against 24 dB at 0.9. Matching
// the peak height at each knob position gives an effective resonance of 0.8 x the knob (0.43 at 0.5, 0.79 at 1).
constexpr float legacyResonanceScale = 0.8f;

void mapPartFilter(Builder& b, int part, F cutoff, F drive, F input, F reso, F type, F slope, float offset)
{
    const auto prefix = "layer." + std::to_string(part) + ".filter.";
    const auto filterType = b.index(type, 4); // Bypass, Lowpass, Bandpass, Highpass
    // MEASURED (the reference synth 3.0.7.3, sweeps): stored 0 = 24 dB/octave, 1 = 12 dB/octave (FILTERDB in the
    // reference code; the template comment has it the other way round).
    const auto steep = !b.on(slope);
    static constexpr std::array<const char*, 3> letters { "L", "B", "H" };
    b.flag(prefix + "enabled", filterType != 0);
    if (filterType != 0)
        b.choose(prefix + "mode", std::string(letters[static_cast<std::size_t>(filterType - 1)]) + (steep ? "4" : "2"));

    const auto target = targetSemitones(b, cutoff);
    const auto partSemitones = std::clamp(target - offset, 0.0f, partCutoffMaxSemitones);
    b.set(prefix + "cutoff_semitones", partSemitones);
    const auto audibleRange = target > 16.0f && target < 135.0f;
    if (filterType != 0 && audibleRange && std::abs(partSemitones + offset - target) > 0.5f)
        b.approximated(std::string("Filter ") + (part == 1 ? "A" : "B") + " cutoff is outside Synthax1's range and was clamped");
    b.set(prefix + "resonance", b.raw(reso) * legacyResonanceScale);
    b.set(prefix + "drive", b.raw(drive));

    // Stored: own part, own+other, none. Synthax1 choices: None, A, B, A+B.
    const auto routing = b.index(input, 3);
    const auto own = part == 1 ? "A" : "B";
    b.choose(prefix + "input", routing == 0 ? own : routing == 1 ? "A+B" : "None");
}

void mapFilters(Builder& b)
{
    const auto offset = sharedOffsetSemitones(b);
    mapPartFilter(b, 1, F::FilterACutoff, F::FilterADrive, F::FilterAInput, F::FilterAReso, F::FilterAType, F::FilterADB, offset);
    mapPartFilter(b, 2, F::FilterBCutoff, F::FilterBDrive, F::FilterBInput, F::FilterBReso, F::FilterBType, F::FilterBDB, offset);
    b.set("filter_control.cutoff_semitones", offset);
    b.set("filter_control.resonance", b.raw(F::FilterCtlReso) * legacyResonanceScale);
    b.set("filter_control.keytrack", b.raw(F::FilterCtlKeyTrk));
    b.flag("filter_control.warm_drive", b.on(F::FilterCtlWarmDrive));
}

void mapEnvelope(Builder& b, const std::string& prefix, F attack, F decay, F release, F sustain)
{
    // The reference synth's stages reach ~20 s; Synthax1's envelope times go to 30 s.
    b.set(prefix + "attack_ms", curves::attackMs(b.raw(attack)));
    b.set(prefix + "decay_ms", curves::decayMs(b.raw(decay)));
    b.set(prefix + "sustain", b.raw(sustain));
    b.set(prefix + "release_ms", curves::releaseMs(b.raw(release)));
}

struct Usage
{
    bool partA = false, partB = false;     // a part with sounding oscillators
    bool modEnv1 = false, modEnv2 = false; // routed somewhere with a nonzero amount
    bool lfo1 = false, lfo2 = false;
};

Usage analyse(const Builder& b)
{
    Usage usage;
    usage.partA = b.index(F::OscA1Voices, 9) > 0 || b.index(F::OscA2Voices, 9) > 0;
    usage.partB = b.index(F::OscB1Voices, 9) > 0 || b.index(F::OscB2Voices, 9) > 0;

    const auto routes = [&b](F destination1, F amount1, F destination2, F amount2) {
        const auto live = [&b](F destination, F amount) {
            return b.index(destination, modDestinationSteps) != 0 && bipolar(b.raw(amount)) != 0.0f;
        };
        return live(destination1, amount1) || live(destination2, amount2);
    };
    usage.modEnv1 = routes(F::YModEnv1Dest1, F::XModEnv1Dest1Am, F::YModEnv1Dest2, F::XModEnv1Dest2Am);
    usage.modEnv2 = routes(F::YModEnv2Dest1, F::XModEnv2Dest1Am, F::YModEnv2Dest2, F::XModEnv2Dest2Am);
    usage.lfo1 = routes(F::YModLFO1Dest1, F::XModLFO1Dest1Am, F::YModLFO1Dest2, F::XModLFO1Dest2Am);
    usage.lfo2 = routes(F::YModLFO2Dest1, F::XModLFO2Dest1Am, F::YModLFO2Dest2, F::XModLFO2Dest2Am);

    struct Misc { F source, destination1, amount1, destination2, amount2; };
    const std::array<Misc, 4> misc { {
        { F::XModMisc1ASource, F::YModMisc1ADest1, F::XModMisc1ADest1Am, F::YModMisc1ADest2, F::XModMisc1ADest2Am },
        { F::XModMisc1BSource, F::YModMisc1BDest1, F::XModMisc1BDest1Am, F::YModMisc1BDest2, F::XModMisc1BDest2Am },
        { F::XModMisc2ASource, F::YModMisc2ADest1, F::XModMisc2ADest1Am, F::YModMisc2ADest2, F::XModMisc2ADest2Am },
        { F::XModMisc2BSource, F::YModMisc2BDest1, F::XModMisc2BDest1Am, F::YModMisc2BDest2, F::XModMisc2BDest2Am },
    } };
    for (const auto& slot : misc)
    {
        if (!routes(slot.destination1, slot.amount1, slot.destination2, slot.amount2))
            continue;
        switch (b.index(slot.source, 12)) // None, Velocity, ModWheel, KeyTrack, AmpEnvA, AmpEnvB, ModEnv1, ModEnv2, LFO1, LFO2, ...
        {
            case 6: usage.modEnv1 = true; break;
            case 7: usage.modEnv2 = true; break;
            case 8: usage.lfo1 = true; break;
            case 9: usage.lfo2 = true; break;
            default: break;
        }
    }
    return usage;
}

void mapEnvelopes(Builder& b, const Usage& usage)
{
    {
        Audibility scope(b, usage.partA);
        mapEnvelope(b, "layer.1.amp_env.", F::AmpEnvAAttack, F::AmpEnvADecay, F::AmpEnvARelease, F::AmpEnvASustain);
    }
    {
        Audibility scope(b, usage.partB);
        mapEnvelope(b, "layer.2.amp_env.", F::AmpEnvBAttack, F::AmpEnvBDecay, F::AmpEnvBRelease, F::AmpEnvBSustain);
    }
    {
        Audibility scope(b, usage.modEnv1);
        mapEnvelope(b, "mod_env.", F::ModEnv1Attack, F::ModEnv1Decay, F::ModEnv1Release, F::ModEnv1Sustain);
    }
    Audibility scope(b, usage.modEnv2);
    mapEnvelope(b, "mod_env.2.", F::ModEnv2Attack, F::ModEnv2Decay, F::ModEnv2Release, F::ModEnv2Sustain);
}

void mapLfo(Builder& b, const std::string& prefix, int number, F free, F gain, F offset, F rate, F wave, bool synced)
{
    const auto shape = static_cast<std::size_t>(b.index(wave, 11));
    b.choose(prefix + "shape", lfoWaveToSynthax1[shape].synthax1);
    if (!lfoWaveToSynthax1[shape].exact)
        b.approximated("LFO " + std::to_string(number) + " waveform " + lfoWaves[shape] + " played as "
                       + lfoWaveToSynthax1[shape].synthax1);
    b.set(prefix + "gain", b.raw(gain));
    // MEASURED (sweeps/lfooffset): the reference synth's LFO output is Gain x (wave + Offset), so Offset moves the centre by
    // Gain times as much (Gain 0 leaves nothing, whatever Offset says); Synthax1 adds its offset after the gain.
    b.set(prefix + "offset", bipolar(b.raw(offset)) * b.raw(gain));
    b.flag(prefix + "free", b.on(free));
    // Free-running LFOs ignore Sync; otherwise Sync selects tempo divisions.
    if (synced && !b.on(free))
        b.chooseDivision(prefix + "sync_division", lfoDivisionAt(b.raw(rate)), "LFO " + std::to_string(number) + " rate");
    else
        b.setHz(prefix + "rate_hz", curves::lfoHz(b.raw(rate)), 0.01f, 200.0f, "LFO " + std::to_string(number) + " rate");
}

void mapVoiceAndGlobal(Builder& b, bool synced)
{
    // Init shows Polyphony 3 and Voices 0/3 for a stored 0.1875 (3/16): the knob reads raw * 16, 1 to 16.
    const auto polyphony = std::clamp(static_cast<int>(b.raw(F::Polyphony) * 16.0f + 1.0e-4f), 1, 16);
    b.set("voice.polyphony", static_cast<float>(polyphony));
    // MEASURED (the reference synth 3.0.7.3, programs with Mono Legato on and Polyphony 3-4): Mono Legato is a mode
    // button that makes the instrument monophonic (last note wins) whatever the polyphony knob says.
    if (b.on(F::MonoLegato))
        b.choose("voice.mode", "MonoLegato");
    else
        b.choose("voice.mode", polyphony == 1 ? "Mono" : "Poly");
    b.choose("voice.portamento_mode", b.on(F::PortaMode) ? "Normal" : "Slide");
    b.set("voice.glide_ms", curves::glideMs(b.raw(F::PortaTime)));
    b.set("voice.pitch_bend_range", static_cast<float>(1 + b.index(F::PitchBendRange, 24)));
    b.flag("global.sync", synced);
    // MEASURED (the reference synth 3.0.7.3, sweeps/wheel): the mod wheel position stored in the preset is a live source
    // value when it loads. Carried by the import as a program property, not a registered parameter.
    b.set("performance.mod_wheel", b.raw(F::ModWheel));
    // MEASURED (the reference synth 3.0.7.3, sweeps/level, 63 points): Main Volume, Mix and oscillator
    // volume are all linear in gain with identical curves in both synths; Synthax1's output is a
    // constant 12.54 dB hotter at the same settings (11.25 plus the 1.29 dB that the old velocity scaling took off at velocity 100), so that is removed here, once.
    b.set("master.level_db", std::max(-48.0f, curves::levelDb(b.raw(F::MainVolume)) - 12.54f));
    b.set("layer.1.level_db", curves::levelDb(b.raw(F::MixA)));
    b.set("layer.2.level_db", curves::levelDb(b.raw(F::MixB)));

    // Solo plays only the part shown by the A/B selector.
    const auto shownPart = b.on(F::YPartSelect) ? 2 : 1;
    b.flag("layer.1.solo", b.on(F::Solo) && shownPart == 1);
    b.flag("layer.2.solo", b.on(F::Solo) && shownPart == 2);
}

void mapArp(Builder& b, bool synced)
{
    Audibility scope(b, b.on(F::XSwArpOnOff));
    b.flag("arp.enabled", b.on(F::XSwArpOnOff));
    b.choose("arp.mode", arpModeToSynthax1[static_cast<std::size_t>(b.index(F::ArpMode, 10))]);
    static constexpr std::array<const char*, 5> velocity { "Key", "Hold", "Step", "StepKey", "StepHold" };
    b.choose("arp.velocity_mode", velocity[static_cast<std::size_t>(b.index(F::ArpVelo, 5))]);
    b.set("arp.gate", std::max(0.02f, b.raw(F::ArpGate)));
    b.set("arp.octaves", static_cast<float>(1 + b.index(F::ArpOctave, 4)));
    b.set("arp.wrap", static_cast<float>(b.index(F::ArpWrap, 17)));
    const auto* division = divisionAt(arpDivisions, b.raw(F::ArpTime));
    b.choose("arp.rate", division); // Synthax1's arp list is the reference synth's: 1/1 to 1/64
    if (!synced) // ESTIMATED: the free-running time is not measured; use the division at 120 BPM
        b.set("arp.time_ms", std::clamp(beatsOf(division) * 500.0f, 1.0f, 6000.0f));

    const auto first = static_cast<int>(F::XArpHold01);
    for (int step = 0; step < 16; ++step)
    {
        const auto prefix = "arp.step." + std::to_string(step + 1) + ".";
        b.flag(prefix + "tie", b.on(static_cast<F>(first + step)));
        b.set(prefix + "pitch_semitones", static_cast<float>(std::min(48, static_cast<int>(clamp01(b.patch[static_cast<F>(static_cast<int>(F::XArpTransp01) + step)]) * 49.0f)) - 24));
        b.set(prefix + "velocity", b.raw(static_cast<F>(static_cast<int>(F::XArpVelo01) + step)));
    }
}

void mapEffects(Builder& b, bool synced)
{
    const auto audible = b.audible;
    const auto section = [&b, audible](F onOff) { b.audible = audible && b.on(onOff); };
    b.flag("fx.saturation_enabled", b.on(F::XSwDistOnOff));
    static constexpr std::array<const char*, 5> distortion { "Overdrive", "Fold", "Clip", "Decimate", "Bitcrush" };
    b.choose("fx.distortion_mode", distortion[static_cast<std::size_t>(b.index(F::DistortType, 5))]);
    b.set("fx.saturation_drive", b.raw(F::DistortAmount));
    b.set("fx.saturation_mix", b.raw(F::DistortDryWet));

    b.flag("fx.phaser_enabled", b.on(F::XSwPhaserOnOff));
    section(F::XSwPhaserOnOff);
    b.set("fx.phaser_mix", b.raw(F::PhaserDry_Wet));
    b.set("fx.phaser_feedback", b.raw(F::PhaserFeedback) * 0.95f);
    b.set("fx.phaser_center_hz", curves::phaserHz(b.raw(F::PhaserCenterFreq)));
    b.set("fx.phaser_depth", b.raw(F::PhaserLFOGain));
    b.set("fx.phaser_spread", b.raw(F::PhaserSpread));
    // The reference synth's L/R Offset knob is bipolar (-10..10) with its centre meaning "none"; its output is
    // fully mono-correlated there (measured L/R correlation 1.00). Synthax1's offset is 0..1, so use the
    // distance from the centre.
    b.set("fx.phaser_lr_offset", std::abs(bipolar(b.raw(F::PhaserLROffset))));
    b.set("fx.phaser_width", b.raw(F::PhaserWidth));
    // ESTIMATED: assumes the phaser LFO uses the same rate scale as the main LFOs.
    if (synced)
        b.chooseDivision("fx.phaser_sync_division", lfoDivisionAt(b.raw(F::PhaserLFORate)), "Phaser rate");
    else
        b.setHz("fx.phaser_rate_hz", curves::lfoHz(b.raw(F::PhaserLFORate)), 0.02f, 8.0f, "Phaser rate");

    b.flag("fx.chorus_enabled", b.on(F::XSwChorusOnOff));
    section(F::XSwChorusOnOff);
    b.set("fx.chorus_mix", b.raw(F::ChorusDry_Wet));
    b.set("fx.chorus_delay_ms", 1.0f + 39.0f * b.raw(F::ChorusDelay));
    b.set("fx.chorus_depth_ms", std::max(0.1f, 24.0f * b.raw(F::ChorusDepth)));
    b.set("fx.chorus_feedback", 0.96f * std::pow(b.raw(F::ChorusFeedback), 1.8f)); // MEASURED (sweeps/fbtail): decay slopes at 0.75, 0.9 and 1.0
    b.set("fx.chorus_width", b.raw(F::ChorusWidth));
    b.flag("fx.chorus_dual_mode", b.on(F::ChorusMode));
    b.setHz("fx.chorus_rate_hz", curves::chorusHz(b.raw(F::ChorusRate)), 0.02f, 8.0f, "Chorus rate");

    b.flag("fx.eq_enabled", b.on(F::XSwEQOnOff));
    section(F::XSwEQOnOff);
    // Boost only, 0..15 dB (template range; the manual: "the Bass knob will turn up the amount
    // of bass", unchanged at 0). In 5,600 real EQ-on programs the commonest bass value is 0.0
    // and the commonest treble value 1.0, i.e. "untouched" and "full boost".
    const auto bassDb = 15.0f * b.raw(F::EQBass);
    const auto trebleDb = 15.0f * b.raw(F::EQTreble);
    b.set("fx.eq_low_gain_db", std::min(bassDb, 12.0f));
    b.set("fx.eq_high_gain_db", std::min(trebleDb, 12.0f));
    if (bassDb > 12.0f || trebleDb > 12.0f)
        b.approximated("EQ boost beyond Synthax1's +12 dB was clamped");
    b.set("fx.eq_low_frequency_hz", std::clamp(curves::eqBassHz(b.raw(F::EQBassFreq)), 20.0f, 2000.0f));
    b.set("fx.eq_high_frequency_hz", std::clamp(curves::eqTrebleHz(b.raw(F::EQTrebleFreq)), 1000.0f, 20000.0f));
    if (b.patch.zeqMode().value_or(0.0f) >= 0.5f)
        b.approximated("EQ 2-pole mode has no Synthax1 equivalent; the EQ uses its single shelf slope");

    b.flag("fx.reverb_enabled", b.on(F::XSwReverbOnOff));
    section(F::XSwReverbOnOff);
    b.set("fx.reverb_mix", b.raw(F::ReverbDry_Wet));
    b.set("fx.reverb_decay", b.raw(F::ReverbSize));
    b.set("fx.reverb_damp", b.raw(F::ReverbDamp));
    b.set("fx.reverb_pre_delay_ms", 200.0f * b.raw(F::ReverbPredelay));
    b.set("fx.reverb_width", b.raw(F::ReverbWidth));

    b.flag("fx.delay_enabled", b.on(F::XSwDelayOnOff));
    section(F::XSwDelayOnOff);
    b.set("fx.delay_mix", b.raw(F::DelayDry_Wet));
    b.set("fx.delay_feedback", b.raw(F::DelayFeedback) * 0.975f); // MEASURED: echo gain = knob * 0.975
    b.flag("fx.delay_ping_pong", b.on(F::DelayPingPong));
    b.set("fx.delay_smear", b.raw(F::DelaySmear));
    b.set("fx.delay_spread", b.raw(F::DelaySpread));
    b.set("fx.delay_width", b.raw(F::DelayWidth));
    // Clamping only removes inaudible extremes (below 20 Hz / above 20 kHz) or rarely used ones.
    b.set("fx.delay_low_cut_hz", std::clamp(curves::delayLowCutHz(b.raw(F::DelayLowCut)), 20.0f, 2000.0f));
    b.set("fx.delay_high_cut_hz", std::clamp(curves::delayHighCutHz(b.raw(F::DelayHighCut)), 200.0f, 20000.0f));
    const auto* left = divisionAt(delayDivisions, b.raw(F::DelayTimeLeft));
    const auto* right = divisionAt(delayDivisions, b.raw(F::DelayTimeRight));
    b.chooseDivision("fx.delay_sync_division", left, "Delay left time");
    b.chooseDivision("fx.delay_right_sync_division", right, "Delay right time");
    b.set("fx.delay_time_left_ms", curves::delayTimeMs(b.raw(F::DelayTimeLeft)));
    b.set("fx.delay_time_right_ms", curves::delayTimeMs(b.raw(F::DelayTimeRight)));

    b.flag("fx.compressor_enabled", b.on(F::XSwCompOnOff));
    section(F::XSwCompOnOff);
    b.set("fx.compressor_mix", 1.0f);
    b.set("fx.compressor_attack_ms", curves::compAttackMs(b.raw(F::CompAttack)));
    b.set("fx.compressor_release_ms", curves::compReleaseMs(b.raw(F::CompRelease)));
    b.set("fx.compressor_ratio", curves::compRatio(b.raw(F::CompRatio)));
    // MEASURED (the reference synth 3.0.7.3, sweeps/comp, 100:1, 120 points): the compressor sits before Main
    // Volume; above T = -5 - 28 k dB (input RMS of a sine, k = the knob) the output follows
    // T + makeup + (in - T)/ratio, below it the signal gains makeup(k). Synthax1 detects peaks (+3 dB
    // for a sine) before a master gain that is 12.54 dB hotter than the output, so its threshold is
    // T + 3 + 12.54.
    // MEASURED (sweeps/compmakeup): the makeup gain also depends on the Ratio knob. Rows are Ratio 0, 0.1 ... 1.0,
    // columns Threshold 0, 0.1, 0.3, 0.5, 0.7, 0.9, 1.0 (dB, quiet sine against compressor off).
    constexpr std::array<float, 7> knobs { 0.0f, 0.1f, 0.3f, 0.5f, 0.7f, 0.9f, 1.0f };
    constexpr std::array<std::array<float, 7>, 11> makeupDb { {
        { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f },
        { 0.0f, 0.0f, 0.0f, 0.1f, 0.1f, 0.2f, 0.2f },
        { 0.0f, 0.0f, 0.1f, 0.2f, 0.4f, 0.7f, 0.9f },
        { 0.0f, 0.0f, 0.2f, 0.5f, 1.0f, 1.5f, 1.9f },
        { 0.0f, 0.0f, 0.3f, 0.9f, 1.6f, 2.6f, 3.1f },
        { 0.0f, 0.1f, 0.5f, 1.3f, 2.4f, 3.7f, 4.4f },
        { 0.0f, 0.1f, 0.7f, 1.9f, 3.3f, 5.0f, 5.8f },
        { 0.0f, 0.1f, 1.0f, 2.4f, 4.3f, 6.2f, 7.2f },
        { 0.0f, 0.1f, 1.2f, 3.1f, 5.2f, 7.5f, 8.6f },
        { 0.0f, 0.2f, 1.5f, 3.7f, 6.2f, 8.8f, 9.9f },
        { 0.0f, 0.2f, 1.9f, 4.4f, 7.2f, 9.9f, 11.2f },
    } };
    const auto knob = b.raw(F::CompThreshold);
    const auto ratioPosition = b.raw(F::CompRatio) * 10.0f;
    const auto row = std::min<std::size_t>(9, static_cast<std::size_t>(ratioPosition));
    const auto rowFraction = ratioPosition - static_cast<float>(row);
    std::size_t column = 1;
    while (column < knobs.size() - 1 && knobs[column] < knob)
        ++column;
    const auto columnFraction = std::clamp((knob - knobs[column - 1]) / (knobs[column] - knobs[column - 1]), 0.0f, 1.0f);
    const auto at = [&](std::size_t r) { return makeupDb[r][column - 1] + (makeupDb[r][column] - makeupDb[r][column - 1]) * columnFraction; };
    b.set("fx.compressor_threshold_db", -5.0f - 28.0f * knob + 3.0f + 12.54f);
    b.set("fx.compressor_makeup_db", at(row) + (at(row + 1) - at(row)) * rowFraction);
    b.audible = audible;
}
// ------------------------------------------------------------------ modulation
// A the reference synth destination becomes one visible Synthax1 route; where it covers several
// Synthax1 targets, the remaining ones get hidden physical depths (transmod.N.native.*),
// which the engine sums with the visible routes.
// maximumDepth: the depth in destination units at the reference synth's +10. catalogMax: the destination's range in
// Synthax1's catalogue when that is larger (0 = the same); the visible route amount is a fraction of it.
struct Target { const char* routeLabel; const char* nativeId; float maximumDepth; float catalogMax = 0.0f; };
struct Destination { std::vector<Target> targets; };

const std::vector<Destination>& destinationTable()
{
    static const std::vector<Destination> table {
        {},                                                                                  // None
        // MEASURED (sweeps/volroute): Volume A/B add to each oscillator's volume knob, +0.5 at +10 for a bipolar source (LFO), measured on the part level before; the route amount is a fraction of the catalogue range 2.0.
        { { { "A1 Volume", "layer.1.osc.1.level", 0.5f, 2.0f }, { nullptr, "layer.1.osc.2.level", 0.5f } } },   // Volume A
        { { { "B1 Volume", "layer.2.osc.1.level", 0.5f, 2.0f }, { nullptr, "layer.2.osc.2.level", 0.5f } } },   // Volume B
        { { { "A1 Volume", "layer.1.osc.1.level", 0.5f, 2.0f }, { nullptr, "layer.1.osc.2.level", 0.5f },
            { nullptr, "layer.2.osc.1.level", 0.5f }, { nullptr, "layer.2.osc.2.level", 0.5f } } },   // Volume AB
        { { { "A1 Pitch", "layer.1.osc.1.note", 120.0f }, { nullptr, "layer.1.osc.2.note", 120.0f } } },
        { { { "B1 Pitch", "layer.2.osc.1.note", 120.0f }, { nullptr, "layer.2.osc.2.note", 120.0f } } },
        { { { "A1 Pitch", "layer.1.osc.1.note", 120.0f }, { nullptr, "layer.1.osc.2.note", 120.0f },
            { nullptr, "layer.2.osc.1.note", 120.0f }, { nullptr, "layer.2.osc.2.note", 120.0f } } }, // Pitch AB
        { { { "A1 Phase", "layer.1.osc.1.phase_degrees", 180.0f }, { nullptr, "layer.1.osc.2.phase_degrees", 180.0f } } },
        { { { "B1 Phase", "layer.2.osc.1.phase_degrees", 180.0f }, { nullptr, "layer.2.osc.2.phase_degrees", 180.0f } } },
        { { { "A1 Phase", "layer.1.osc.1.phase_degrees", 180.0f }, { nullptr, "layer.1.osc.2.phase_degrees", 180.0f },
            { nullptr, "layer.2.osc.1.phase_degrees", 180.0f }, { nullptr, "layer.2.osc.2.phase_degrees", 180.0f } } },
        { { { "Part A Pan", "layer.1.pan", 1.0f } } },                                       // Pan A
        { { { "Part B Pan", "layer.2.pan", 1.0f } } },                                       // Pan B
        { { { "Pan", nullptr, 1.0f } } },                                                    // Pan AB
        { { { "Filter A Cutoff", "layer.1.filter.cutoff_semitones", 122.0f } } },
        { { { "Filter B Cutoff", "layer.2.filter.cutoff_semitones", 122.0f } } },
        { { { "Filter A Cutoff", "layer.1.filter.cutoff_semitones", 122.0f },
            { nullptr, "layer.2.filter.cutoff_semitones", 122.0f } } },                      // Cutoff AB
        { { { "Filter A Resonance", "layer.1.filter.resonance", 1.0f } } },
        { { { "Filter B Resonance", "layer.2.filter.resonance", 1.0f } } },
        { { { "Resonance A+B", "filter_control.resonance", 1.0f } } },
        { { { "Phaser Center Frequency", "fx.phaser_center_hz", 20000.0f } } },
        { { { "Part A Volume", "layer.1.level_db", 1.326f, 1.4f } } },   // Mix A (measured: 2.65x Volume)
        { { { "Part B Volume", "layer.2.level_db", 1.326f, 1.4f } } },   // Mix B
        { { { "Part A Volume", "layer.1.level_db", 1.326f, 1.4f }, { nullptr, "layer.2.level_db", 1.326f } } },   // Mix AB
        { { { "LFO 1 Rate", "lfo.rate_hz", 40.0f } } },
        { { { "LFO 1 Gain", "lfo.gain", 1.0f } } },
        { { { "LFO 2 Rate", "lfo.2.rate_hz", 40.0f } } },
        { { { "LFO 2 Gain", "lfo.2.gain", 1.0f } } },
    };
    return table;
}

struct ModSlot { int number; const char* fixedSource; F source; F amount1, amount2, destination1, destination2; };
constexpr std::array<ModSlot, 8> modSlots { {
    { 1, "ModEnv", F::Count, F::XModEnv1Dest1Am, F::XModEnv1Dest2Am, F::YModEnv1Dest1, F::YModEnv1Dest2 },
    { 2, "ModEnv2", F::Count, F::XModEnv2Dest1Am, F::XModEnv2Dest2Am, F::YModEnv2Dest1, F::YModEnv2Dest2 },
    { 3, "LFO", F::Count, F::XModLFO1Dest1Am, F::XModLFO1Dest2Am, F::YModLFO1Dest1, F::YModLFO1Dest2 },
    { 4, "LFO2", F::Count, F::XModLFO2Dest1Am, F::XModLFO2Dest2Am, F::YModLFO2Dest1, F::YModLFO2Dest2 },
    { 5, nullptr, F::XModMisc1ASource, F::XModMisc1ADest1Am, F::XModMisc1ADest2Am, F::YModMisc1ADest1, F::YModMisc1ADest2 },
    { 6, nullptr, F::XModMisc1BSource, F::XModMisc1BDest1Am, F::XModMisc1BDest2Am, F::YModMisc1BDest1, F::YModMisc1BDest2 },
    { 7, nullptr, F::XModMisc2ASource, F::XModMisc2ADest1Am, F::XModMisc2ADest2Am, F::YModMisc2ADest1, F::YModMisc2ADest2 },
    { 8, nullptr, F::XModMisc2BSource, F::XModMisc2BDest1Am, F::XModMisc2BDest2Am, F::YModMisc2BDest1, F::YModMisc2BDest2 },
} };

void mapModulation(Builder& b)
{
    for (const auto& slot : modSlots)
    {
        const auto prefix = "transmod." + std::to_string(slot.number) + ".";
        const auto source = slot.fixedSource != nullptr
                              ? std::string(slot.fixedSource)
                              : std::string(modSourceToSynthax1[static_cast<std::size_t>(b.index(slot.source, 12))]);
        b.choose(prefix + "source", source);

        const std::array<std::pair<F, F>, 2> routes { { { slot.destination1, slot.amount1 }, { slot.destination2, slot.amount2 } } };
        bool hiddenDepths = false;
        for (std::size_t route = 0; route < routes.size(); ++route)
        {
            const auto routePrefix = prefix + "route." + std::to_string(route + 1) + ".";
            const auto destination = static_cast<std::size_t>(b.index(routes[route].first, modDestinationSteps));
            const auto legacyAmount = bipolar(b.raw(routes[route].second));
            const auto& table = destinationTable();
            if (destination == 0 || destination >= table.size())
            {
                b.choose(routePrefix + "destination", "None");
                if (destination != 0 && legacyAmount != 0.0f)
                    b.dropped("Mod slot " + std::to_string(slot.number) + " destination " + modDestinations[destination]
                              + " has no Synthax1 equivalent");
                continue;
            }

            const auto& targets = table[destination].targets;
            auto amount = legacyAmount;
            if (destination >= 4 && destination <= 6) // Pitch A/B/AB: the reference synth full scale is 120 semitones
            {
                // MEASURED (sweeps/lfoshape): an LFO moves pitch half as far as an envelope at the same amount.
                const auto fromLfo = std::string_view(source).find("LFO") != std::string_view::npos;
                amount = legacyAmount * (fromLfo ? 0.5f : 1.0f) * curves::pitchModSemitonesAtFullScale / targets.front().maximumDepth;
                if (std::abs(amount) > 1.0f)
                    b.approximated("Mod slot " + std::to_string(slot.number) + " " + modDestinations[destination]
                                   + " depth exceeds Synthax1's 120 semitones and was clamped");
                amount = std::clamp(amount, -1.0f, 1.0f);
            }
            // MEASURED (sweeps/lfocut): an LFO swings the cutoff about half as far as an envelope does at the same amount.
            if (destination >= 13 && destination <= 15 && std::string_view(source).find("LFO") != std::string_view::npos)
                amount = legacyAmount * 0.52f;
            // MEASURED (sweeps/lfodepth, volroute, velocity, stepvel): on Volume an LFO moves the level half as far as
            // Velocity does (as for pitch and cutoff, where it moves half as far as an envelope); Step Velocity matches
            // Velocity on Mix. The table holds the LFO depth for Volume (0.5 per unit) and the Velocity depth for Mix
            // (1.326 per unit); an LFO on Mix is assumed to be half as strong too (not measured).
            const auto halfSource = std::string_view(source).find("LFO") != std::string_view::npos;
            auto depthScale = 1.0f;
            if (destination >= 1 && destination <= 3)
                depthScale = halfSource ? 1.0f : 2.0f;
            else if (destination >= 20 && destination <= 22)
                depthScale = halfSource ? 0.5f : 1.0f;
            // Depth in units of each target's maximumDepth (the extra targets use it directly); the visible route of a
            // catalogue destination is that depth as a fraction of the catalogue range.
            if (depthScale != 1.0f)
                amount = legacyAmount * depthScale;
            const auto depthFraction = amount;
            if (targets.front().catalogMax > 0.0f)
                amount = legacyAmount * depthScale * targets.front().maximumDepth / targets.front().catalogMax;
            b.choose(routePrefix + "destination", targets.front().routeLabel);
            b.set(routePrefix + "amount", amount);
            for (std::size_t extra = 1; extra < targets.size(); ++extra)
            {
                b.set(prefix + "native." + targets[extra].nativeId, depthFraction * targets[extra].maximumDepth);
                hiddenDepths = hiddenDepths || depthFraction != 0.0f;
            }
            if (targets.size() > 1 && depthFraction != 0.0f)
                b.approximated("Mod slot " + std::to_string(slot.number) + " " + modDestinations[destination] + " shows as "
                               + targets.front().routeLabel + "; the other oscillators use hidden depths");
        }
        if (hiddenDepths)
            b.flag(prefix + "enabled", true);
    }
}
} // namespace

MappedPatch mapPatch(const Patch& patch)
{
    Builder b(patch);
    const auto synced = b.on(F::Sync);
    const auto usage = analyse(b);
    mapVoiceAndGlobal(b, synced);
    mapOscillators(b);
    mapFilters(b);
    mapEnvelopes(b, usage);
    {
        Audibility scope(b, usage.lfo1);
        mapLfo(b, "lfo.", 1, F::LFO1Free, F::LFO1Gain, F::LFO1Offset, F::LFO1Rate, F::LFO1Wave, synced);
    }
    {
        Audibility scope(b, usage.lfo2);
        mapLfo(b, "lfo.2.", 2, F::LFO2Free, F::LFO2Gain, F::LFO2Offset, F::LFO2Rate, F::LFO2Wave, synced);
    }
    mapModulation(b);
    mapArp(b, synced);
    mapEffects(b, synced);
    return b.take();
}

int selectorIndex(float normalized, int choiceCount) noexcept
{
    if (choiceCount <= 1)
        return 0;
    // MEASURED: the reference synth switches at multiples of 1/count (fine sweeps of octave and arp transpose).
    return std::min(choiceCount - 1, static_cast<int>(clamp01(normalized) * static_cast<float>(choiceCount) + 1e-4f));
}

const std::vector<std::string>& oscillatorWaveLabels() { return oscWaves; }
const std::vector<std::string>& lfoWaveLabels() { return lfoWaves; }
const std::vector<std::string>& modSourceLabels() { return modSources; }
const std::vector<std::string>& modDestinationLabels() { return modDestinations; }
const std::vector<std::string>& arpModeLabels() { return arpModes; }
} // namespace synth::legacy
