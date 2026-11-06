#pragma once

// Translates a raw the reference synth patch (normalized VST values) into Synthax1 registry assignments.
//
// The output is plain data so the mapping can be tested without an audio processor:
// every assignment names a registered parameter ID and either a physical value or, for
// choice parameters, the exact registered choice label. LegacyImport resolves labels and
// builds processor state.
//
// Two kinds of knowledge live here and are kept apart on purpose:
//  * Enumerations (waveforms, filter types, arp modes, mod sources/destinations) are exact:
//    The reference synth stores selector index / (count - 1), which the public field template confirms.
//  * Knob curves (normalized -> seconds, Hz, ...) are *not* published. `curves` in the .cpp
//    holds the current best estimate for each; each one is flagged uncalibrated until it has
//    been checked against the reference synth's own parameter display. The import report says so.

#include "LegacyFormat.h"

#include <string>
#include <vector>

namespace synth::legacy
{
struct Assignment
{
    std::string parameterId;
    float value = 0.0f;  // physical value (bools 0/1); ignored when choice is set
    std::string choice;  // registered choice label for choice parameters
};

struct ImportNote
{
    enum class Kind
    {
        Approximated, // mapped to the nearest Synthax1 equivalent
        Dropped,      // no Synthax1 equivalent; setting ignored
    };

    Kind kind = Kind::Approximated;
    std::string text;
};

struct MappedPatch
{
    std::string name;
    std::vector<Assignment> assignments;
    std::vector<ImportNote> notes;
};

MappedPatch mapPatch(const Patch& patch);

// Selector decoding shared with tools and tests: index = round(normalized * (count - 1)).
int selectorIndex(float normalized, int choiceCount) noexcept;

// Labels for the the reference synth selectors, in stored order.
const std::vector<std::string>& oscillatorWaveLabels();
const std::vector<std::string>& lfoWaveLabels();
const std::vector<std::string>& modSourceLabels();
const std::vector<std::string>& modDestinationLabels();
const std::vector<std::string>& arpModeLabels();
} // namespace synth::legacy
