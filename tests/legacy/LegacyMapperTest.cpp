// JUCE-free tests for LegacyPatchMapper: selector decoding and route expansion.
// Registry validity of every emitted ID/label is checked by LegacyImportTest (needs JUCE).

#include "../../src/legacy/LegacyPatchMapper.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

using namespace synth::legacy;

namespace
{
int failures = 0;

void check(bool condition, const std::string& what)
{
    if (!condition)
    {
        ++failures;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

const Assignment* find(const MappedPatch& mapped, const std::string& id)
{
    const Assignment* found = nullptr;
    for (const auto& assignment : mapped.assignments)
        if (assignment.parameterId == id)
            found = &assignment; // last write wins, like state application
    return found;
}

std::string choiceOf(const MappedPatch& mapped, const std::string& id)
{
    const auto* a = find(mapped, id);
    return a == nullptr ? "<missing>" : a->choice;
}

float valueOf(const MappedPatch& mapped, const std::string& id)
{
    const auto* a = find(mapped, id);
    return a == nullptr ? NAN : a->value;
}

bool near(float a, float b, float tolerance = 1.0e-3f) { return std::abs(a - b) <= tolerance; }

// Bit patterns quoted in the public field template.
float bits(std::uint32_t pattern)
{
    float value;
    std::memcpy(&value, &pattern, sizeof value);
    return value;
}

void testTemplateSelectors()
{
    check(selectorIndex(bits(0x3E124925), 8) == 1, "OSC_Saw is wave index 1");
    check(selectorIndex(bits(0x3F5B6DB7), 8) == 6, "OSC_TriSaw is wave index 6");
    check(selectorIndex(bits(0x3F800000), 8) == 7, "OSC_Noise is wave index 7");
    check(selectorIndex(bits(0x3E4CCCCD), 11) == 2, "LFO Ramp (byte-swapped in the template) is index 2");
    check(selectorIndex(bits(0x3F666666), 11) == 9, "LFO SmpHold is index 9");
    check(selectorIndex(bits(0x3F580000), 33) == 27, "DistAmnt is destination 27 of 0..32");
    check(selectorIndex(bits(0x3F68BA2F), 12) == 10, "Aftertouch is source 10");
    check(selectorIndex(bits(0x3F471C72), 10) == 7, "Arp Ordered is mode 7");
    check(selectorIndex(bits(0x3EC00000), 9) == 3, "VOICES_3");
    check(selectorIndex(-1.0f, 4) == 0 && selectorIndex(2.0f, 4) == 3, "selector clamps");
}

void testOscillatorsAndFilters()
{
    Patch patch;
    patch.name = "Test";
    patch[Field::OscA1Wave] = bits(0x3E124925);  // Saw
    patch[Field::OscB2Wave] = bits(0x3F124925);  // HPulse
    patch[Field::OscA1Voices] = bits(0x3F000000); // 4 voices
    patch[Field::OscA1Octave] = 1.0f;             // +3
    patch[Field::OscA1Note] = 0.0f;               // -7
    patch[Field::OscA1Fine] = 0.75f;              // +0.5 semitone
    patch[Field::OscA1Pan] = 0.0f;                // hard left
    patch[Field::FilterAType] = bits(0x3EAAAAAB); // Lowpass
    patch[Field::FilterADB] = 0.0f;               // stored 0 = 24 dB/octave
    patch[Field::FilterAInput] = 0.5f;            // A+B
    patch[Field::FilterBType] = 0.0f;             // Bypass
    patch[Field::FilterBInput] = 1.0f;            // None
    patch[Field::FilterACutoff] = 1.0f;

    const auto mapped = mapPatch(patch);
    check(mapped.name == "Test", "name kept");
    check(choiceOf(mapped, "layer.1.osc.1.waveform") == "Saw", "A1 saw");
    check(choiceOf(mapped, "layer.2.osc.2.waveform") == "HalfPulse", "B2 half pulse");
    check(valueOf(mapped, "layer.1.osc.1.voices") == 4.0f, "A1 voices");
    check(valueOf(mapped, "layer.1.osc.1.octave") == 3.0f, "A1 octave");
    check(valueOf(mapped, "layer.1.osc.1.note") == -7.0f, "A1 note");
    check(near(valueOf(mapped, "layer.1.osc.1.fine_cents"), 50.0f), "A1 fine cents");
    check(near(valueOf(mapped, "layer.1.osc.1.pan"), -1.0f), "A1 pan");
    check(choiceOf(mapped, "layer.1.filter.mode") == "L4", "filter A L4");
    check(valueOf(mapped, "layer.1.filter.enabled") == 1.0f, "filter A enabled");
    check(choiceOf(mapped, "layer.1.filter.input") == "A+B", "filter A input");
    check(valueOf(mapped, "layer.2.filter.enabled") == 0.0f, "filter B bypassed");
    check(choiceOf(mapped, "layer.2.filter.input") == "None", "filter B input");
    check(valueOf(mapped, "layer.1.filter.cutoff_semitones") > 150.0f, "a wide-open cutoff sits near the top of the 176 semitone range");
}

void testModulation()
{
    Patch patch;
    patch[Field::YModEnv1Dest1] = bits(0x3ED00000); // Cutoff_A
    patch[Field::XModEnv1Dest1Am] = 1.0f;           // +10
    patch[Field::YModLFO1Dest2] = bits(0x3E000000); // Pitch_A
    patch[Field::XModLFO1Dest2Am] = 0.45f;          // display -1 = -12 semitones
    patch[Field::XModMisc1ASource] = bits(0x3DBA2E8C); // Velocity
    patch[Field::YModMisc2BDest1] = bits(0x3F580000);  // DistAmnt
    patch[Field::XModMisc2BDest1Am] = 0.9f;
    for (auto field : { Field::XModEnv1Dest2Am, Field::XModEnv2Dest1Am, Field::XModEnv2Dest2Am, Field::XModLFO1Dest1Am,
                        Field::XModLFO2Dest1Am, Field::XModLFO2Dest2Am, Field::XModMisc1ADest1Am, Field::XModMisc1ADest2Am,
                        Field::XModMisc1BDest1Am, Field::XModMisc1BDest2Am, Field::XModMisc2ADest1Am, Field::XModMisc2ADest2Am,
                        Field::XModMisc2BDest2Am })
        patch[field] = 0.5f; // zero amount

    const auto mapped = mapPatch(patch);
    check(choiceOf(mapped, "transmod.1.source") == "ModEnv", "slot 1 source");
    check(choiceOf(mapped, "transmod.1.route.1.destination") == "Filter A Cutoff", "env1 -> cutoff A");
    check(near(valueOf(mapped, "transmod.1.route.1.amount"), 1.0f), "env1 amount");
    check(choiceOf(mapped, "transmod.3.route.2.destination") == "A1 Pitch", "lfo1 -> pitch A shown on A1");
    check(near(valueOf(mapped, "transmod.3.route.2.amount"), -0.05f), "pitch amount: an LFO moves pitch half as far, -6 of 120");
    check(near(valueOf(mapped, "transmod.3.native.layer.1.osc.2.note"), -6.0f), "A2 gets the hidden depth in semitones");
    check(valueOf(mapped, "transmod.3.enabled") == 1.0f, "slot with hidden depth enabled");
    check(choiceOf(mapped, "transmod.5.source") == "Velocity", "misc source");
    check(choiceOf(mapped, "transmod.8.route.1.destination") == "None", "unsupported destination cleared");

    bool droppedDistortion = false;
    for (const auto& note : mapped.notes)
        droppedDistortion = droppedDistortion || (note.kind == ImportNote::Kind::Dropped && note.text.find("Dist Amount") != std::string::npos);
    check(droppedDistortion, "dropped destination reported");
}

void testArpAndVoice()
{
    Patch patch;
    patch[Field::ArpMode] = bits(0x3F471C72);     // Ordered
    patch[Field::ArpVelo] = bits(0x3F400000);     // StepKey
    patch[Field::XSwArpOnOff] = 1.0f;
    patch[Field::XArpTransp03] = 1.0f;            // +24
    patch[Field::XArpHold16] = 1.0f;
    patch[Field::Polyphony] = 0.0f;               // mono
    patch[Field::MonoLegato] = 1.0f;
    patch[Field::PortaMode] = bits(0x3F800000);   // Normal

    const auto mapped = mapPatch(patch);
    check(choiceOf(mapped, "arp.mode") == "AsPlayed", "arp ordered");
    check(choiceOf(mapped, "arp.velocity_mode") == "StepKey", "arp velocity");
    check(valueOf(mapped, "arp.enabled") == 1.0f, "arp on");
    check(valueOf(mapped, "arp.step.3.pitch_semitones") == 24.0f, "step transpose");
    check(valueOf(mapped, "arp.step.16.tie") == 1.0f, "step hold");
    check(choiceOf(mapped, "voice.mode") == "MonoLegato", "mono legato");
    check(choiceOf(mapped, "voice.portamento_mode") == "Normal", "porta normal");
}
bool hasNote(const MappedPatch& mapped, const std::string& fragment)
{
    for (const auto& note : mapped.notes)
        if (note.text.find(fragment) != std::string::npos)
            return true;
    return false;
}

float semitones(float hz) { return 12.0f * std::log2(hz / 8.1757989f); }

void testMeasuredCurves()
{
    Patch patch;
    for (auto& value : patch.values)
        value = 0.5f;
    patch[Field::AmpEnvAAttack] = 0.0f;
    patch[Field::AmpEnvARelease] = 1.0f;       // ~30 s in the reference synth
    patch[Field::ModEnv1Decay] = 0.6f;         // 28.187 * e^(6.9709 * 0.6) = 1847 ms
    patch[Field::FilterACutoff] = 0.64f;       // the reference's worked example:
    patch[Field::FilterCtlCutoff] = 0.32f;     //   590 Hz + 24.3 Hz control -> 1100 Hz
    patch[Field::DelayHighCut] = 1.0f;         // high cut falls as the value rises: 82.5 Hz
    patch[Field::CompRatio] = 0.5f;            // ~1.98:1
    patch[Field::Sync] = 1.0f;
    patch[Field::LFO1Free] = 0.0f;
    patch[Field::LFO1Rate] = 0.5f;             // step 18 of 36: 1/4T
    patch[Field::LFO2Free] = 1.0f;             // free LFOs ignore Sync
    patch[Field::LFO2Rate] = 0.5f;             // 0.0417 * e^(8.4352 * 0.5) = 2.83 Hz
    patch[Field::ArpTime] = 0.5f;              // 1/8
    patch[Field::DelayTimeLeft] = 0.8f;        // 1/4
    patch[Field::DelayTimeRight] = 1.0f;       // 1/2

    const auto mapped = mapPatch(patch);
    check(near(valueOf(mapped, "layer.1.amp_env.attack_ms"), 1.0f, 0.01f), "envelope minimum (measured table)");
    check(near(valueOf(mapped, "layer.1.amp_env.release_ms"), 22715.0f, 1.0f), "release at 1.0 is ~22.7 s, within Synthax1's 30 s");
    check(!hasNote(mapped, "longer than"), "no envelope clamping any more");
    check(near(valueOf(mapped, "mod_env.decay_ms"), 1384.0f, 2.0f), "decay table at 0.6");

    const auto combined = valueOf(mapped, "layer.1.filter.cutoff_semitones") + valueOf(mapped, "filter_control.cutoff_semitones");
    // 1100 Hz nominal, minus the measured 1.79 semitone offset of the reference synth's resonance peak.
    check(near(combined, semitones(1100.0f) - 1.79f, 0.3f), "part + control cutoff reproduce 1100 Hz (with measured offset)");

    check(valueOf(mapped, "fx.delay_high_cut_hz") == 200.0f, "delay high cut at 1.0 is its lowest value");
    check(near(valueOf(mapped, "fx.compressor_ratio"), 1.98f, 0.03f), "compressor ratio from the measured table");
    check(near(valueOf(mapped, "fx.compressor_makeup_db"), 1.3f, 0.05f), "compressor makeup depends on Ratio and Threshold (0.5, 0.5 -> 1.3 dB)");
    check(near(valueOf(mapped, "fx.chorus_feedback"), 0.96f * std::pow(0.5f, 1.8f), 0.001f), "chorus feedback curve");
    check(choiceOf(mapped, "lfo.sync_division") == "1/4T", "synced LFO division");
    check(near(valueOf(mapped, "lfo.2.rate_hz"), 2.83f, 0.02f), "free LFO keeps Hz under Sync");
    check(choiceOf(mapped, "arp.rate") == "1/8", "arp division");
    check(choiceOf(mapped, "fx.delay_sync_division") == "1/4", "delay left division");
    check(choiceOf(mapped, "fx.delay_right_sync_division") == "1/2", "delay right division");
    check(near(valueOf(mapped, "fx.delay_time_left_ms"), 498.0f, 3.0f), "free delay time is exponential in the knob (0.8 -> 498 ms)");
    check(near(valueOf(mapped, "fx.delay_time_right_ms"), 1000.0f, 1.0f), "free delay time at 1.0 is 1 s");
    check(near(valueOf(mapped, "fx.delay_feedback"), 0.4875f, 0.001f), "delay feedback is knob * 0.975");
}

void testLevelRouteDepthBySource()
{
    // MEASURED: on Volume an LFO moves the level half as far as Velocity does; Step Velocity equals Velocity on Mix.
    auto routed = [](Field source, float sourceValue, Field destination, float destinationValue) {
        Patch patch;
        patch[source] = sourceValue;
        patch[destination] = destinationValue;
        patch[Field::XModMisc1ADest1Am] = 1.0f;
        patch[Field::XModLFO1Dest1Am] = 1.0f;
        return mapPatch(patch);
    };
    const auto velocity = routed(Field::XModMisc1ASource, 1.0f / 11.0f, Field::YModMisc1ADest1, 1.0f / 32.0f);
    check(near(valueOf(velocity, "transmod.5.route.1.amount"), 0.5f, 0.001f), "velocity into Volume A: 1.0 per unit");
    const auto lfo = routed(Field::XModMisc1ASource, 0.0f, Field::YModLFO1Dest1, 1.0f / 32.0f);
    check(near(valueOf(lfo, "transmod.3.route.1.amount"), 0.25f, 0.001f), "LFO into Volume A: 0.5 per unit");
    const auto stepMix = routed(Field::XModMisc1ASource, 1.0f, Field::YModMisc1ADest1, 22.0f / 32.0f);
    const auto velocityMix = routed(Field::XModMisc1ASource, 1.0f / 11.0f, Field::YModMisc1ADest1, 22.0f / 32.0f);
    check(near(valueOf(stepMix, "transmod.5.route.1.amount"), valueOf(velocityMix, "transmod.5.route.1.amount"), 0.0001f),
          "step velocity into Mix AB equals velocity");
}

void testUnavailableDivisionIsReported()
{
    Patch patch;
    patch[Field::ArpTime] = 1.0f; // 1/64
    const auto silent = mapPatch(patch);
    check(choiceOf(silent, "arp.rate") == "1/64", "1/64 arp is available");
    check(!hasNote(silent, "Arp time"), "a disabled arp is not reported");
    patch[Field::XSwArpOnOff] = 1.0f;
    check(!hasNote(mapPatch(patch), "Arp time"), "an available arp division is not reported as approximated");
}
} // namespace

int main()
{
    testMeasuredCurves();
    testLevelRouteDepthBySource();
    testUnavailableDivisionIsReported();
    testTemplateSelectors();
    testOscillatorsAndFilters();
    testModulation();
    testArpAndVoice();
    std::printf(failures == 0 ? "LegacyMapperTest: all passed\n" : "LegacyMapperTest: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
