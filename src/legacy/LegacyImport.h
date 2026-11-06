#pragma once

// Bridges the reference synth files into Synthax1 processor state (control thread only).
//
// Each imported program keeps its original the reference synth bytes in the state property
// `legacy_source` (base64 FPCh program) plus the fingerprint it had on import. While the
// program is unedited, exporting it reproduces the original patch exactly, so banks
// assembled from imported programs load unchanged in the reference synth.

#include "LegacyFormat.h"
#include "LegacyPatchMapper.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <optional>
#include <string>
#include <vector>

namespace synth::legacy
{
struct ImportedProgram
{
    juce::String name;
    juce::ValueTree state;               // SYNTHAX1_STATE with every registered parameter
    std::vector<ImportNote> notes;       // approximations and dropped settings
    std::vector<std::string> rejected;   // mapper assignments the registry did not accept
};

struct ImportResult
{
    bool ok = false;
    juce::String message;
    std::vector<ImportedProgram> programs;
    std::vector<std::string> fileWarnings;
};

// zlib backed by JUCE, for the reference synth 3.045 files.
const Zlib& juceZlib();

ImportResult importFile(juce::AudioProcessorValueTreeState& parameters, const juce::File& file);
ImportResult importBytes(juce::AudioProcessorValueTreeState& parameters, std::span<const std::uint8_t> bytes,
                         const juce::String& fallbackName = {});
// fallbackName names a lone program still called "Init" (common in vendor .fxp files).
ImportResult importDocument(juce::AudioProcessorValueTreeState& parameters, const Document& document,
                            const juce::String& fallbackName = {});

ImportedProgram buildProgram(juce::AudioProcessorValueTreeState& parameters, const Patch& patch, const ContainerInfo& info);

struct SourceProgram
{
    Patch patch;
    ContainerInfo info; // version words of the file it came from
};

// Original patch recorded on import, if the state still carries it.
std::optional<SourceProgram> sourceProgram(const juce::ValueTree& state);

// Builds a the reference synth bank (.fxb bytes) from unedited imported programs, filled to 512 slots
// with the reference synth's Init. Fails, naming the offending entries, if any state has no recorded
// source or was edited after import.
bool buildBank(const std::vector<juce::ValueTree>& states, const std::vector<juce::String>& names,
               std::vector<std::uint8_t>& bytes, juce::String& error);

// True when the state still sounds exactly like the patch it was imported from.
bool isUneditedImport(const juce::ValueTree& state);

// One-line summary for the status bar, e.g. "Imported 128 the reference synth programs (37 approximations)".
juce::String summarize(const ImportResult& result);
} // namespace synth::legacy
