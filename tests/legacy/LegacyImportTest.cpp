// The reference synth import against the live parameter registry (needs JUCE).
//
//  1. Every parameter ID the mapper emits must be registered and every choice label must be
//     one of that parameter's choices, over many pseudo-random patches.
//  2. With a folder argument: every real .fxp/.fxb in it is imported end to end (JUCE zlib,
//     mapper, registry, state) with no rejected assignments, and the unedited programs must
//     export back to exactly the values they were imported from.

#include "../../src/plugin/ParameterHistory.h"
#include "../../src/plugin/ParameterRegistry.h"
#include "../../src/presets/PresetLibrary.h"
#include "../../src/legacy/LegacyImport.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <set>
#include <string>

using namespace synth;

namespace
{
int failures = 0;

void fail(const std::string& what)
{
    ++failures;
    std::printf("FAIL: %s\n", what.c_str());
}

// Owns a parameter tree laid out exactly like the plugin's.
class HostlessProcessor final : public juce::AudioProcessor
{
public:
    HostlessProcessor() : parameters(*this, nullptr, "SYNTHAX1_STATE", createParameterLayout()) {}

    const juce::String getName() const override { return "LegacyImportTest"; }
    void prepareToPlay(double, int) override {}
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}
    double getTailLengthSeconds() const override { return 0.0; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}

    juce::AudioProcessorValueTreeState parameters;
};

void checkRegistryCoverage()
{
    std::set<std::string> problems;
    std::mt19937 rng(1234);
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    for (int round = 0; round < 2000; ++round)
    {
        legacy::Patch patch;
        for (auto& value : patch.values)
            value = round == 0 ? 0.0f : round == 1 ? 1.0f : unit(rng);
        patch.extraFields = { unit(rng), unit(rng) };

        for (const auto& assignment : legacy::mapPatch(patch).assignments)
        {
            if (assignment.parameterId == "performance.mod_wheel")
                continue; // carried as a program property, not a registry parameter
            const auto* spec = findParameterSpec(assignment.parameterId);
            if (spec == nullptr)
                problems.insert("unregistered parameter " + assignment.parameterId);
            else if (spec->kind == ParameterKind::Choice
                     && std::find(spec->choices.begin(), spec->choices.end(), assignment.choice) == spec->choices.end())
                problems.insert(assignment.parameterId + " has no choice '" + assignment.choice + "'");
            else if (spec->kind != ParameterKind::Choice && !assignment.choice.empty())
                problems.insert(assignment.parameterId + " is not a choice parameter");
        }
    }
    for (const auto& problem : problems)
        fail(problem);
}

// Undo and redo of edited gestures, without limit, and a program load clears them.
void checkParameterHistory(HostlessProcessor& processor)
{
    synth::ParameterHistory history;
    history.attach(processor);
    auto* parameter = processor.parameters.getParameter("layer.1.filter.cutoff_semitones");
    if (parameter == nullptr)
    {
        fail("history: parameter missing");
        return;
    }
    const auto start = parameter->getValue();
    constexpr int edits = 500;
    for (int i = 1; i <= edits; ++i)
    {
        parameter->beginChangeGesture();
        parameter->setValueNotifyingHost(start * 0.5f + static_cast<float>(i) / (edits * 4.0f));
        parameter->endChangeGesture();
    }
    const auto last = parameter->getValue();
    for (int i = 0; i < edits; ++i)
        if (!history.undo())
        {
            fail("history: undo ran out early");
            return;
        }
    if (history.undo() || std::abs(parameter->getValue() - start) > 1.0e-5f)
        fail("history: undoing every edit did not return to the start value");
    for (int i = 0; i < edits; ++i)
        history.redo();
    if (std::abs(parameter->getValue() - last) > 1.0e-5f)
        fail("history: redoing every edit did not return to the last value");
    history.undo();
    parameter->beginChangeGesture();
    parameter->setValueNotifyingHost(0.123f);
    parameter->endChangeGesture();
    if (history.canRedo())
        fail("history: a new edit must clear redo");
    history.clear();
    if (history.canUndo())
        fail("history: clear left an undo entry");
}

// The library catalogue: names to categories, a scan of a folder with presets, the cache and user data.
void checkLibrary()
{
    if (synth::categoryFromPresetName("BA - Wub Stepper") != "Bass" || synth::categoryFromPresetName("ARP Acid Burn PROTO") != "Arp"
        || synth::categoryFromPresetName("LD_Rolling") != "Lead" || !synth::categoryFromPresetName("Something Else").empty())
        fail("library: category from preset name");

    const auto temp = std::filesystem::temp_directory_path() / "synthax1-library-test";
    std::filesystem::remove_all(temp);
    std::filesystem::create_directories(temp / "Vendor A");
    {
        std::ofstream preset(temp / "Vendor A" / "One.Synthax1Preset");
        preset << R"({ "fileType": "Synthax1Preset", "presetName": "Warm Lead", "presetAuthor": "Me", "category": "Lead",
                       "tags": ["warm", "mono"], "preset": { "display_name": "Warm Lead", "parameters": {} } })";
    }
    const auto cache = temp / "cache.bin";
    const std::vector<synth::LibraryRoot> roots { { temp / "Vendor A", "Mine" } };
    const auto first = synth::scanLibrary(roots, cache, {}, {});
    if (first == nullptr || first->entries.size() != 1)
    {
        fail("library: scan of a Synthax1 preset");
        return;
    }
    const auto& one = first->entries[0];
    if (one.name != "Warm Lead" || first->text(one.category) != "Lead" || first->tagsOf(one).size() != 2
        || first->text(one.location) != "Mine" || first->text(one.author) != "Me")
        fail("library: scan of a Synthax1 preset (fields)");
    const auto cached = synth::loadCachedLibrary(roots, cache); // straight from the cache, no folder scan
    if (cached == nullptr || cached->entries.size() != 1 || cached->entries[0].name != "Warm Lead"
        || cached->idOf(cached->entries[0]) != first->idOf(one) || cached->find(first->idOf(one)) != 0)
        fail("library: cached catalogue differs");
    const auto second = synth::scanLibrary(roots, cache, {}, {}); // answered from the cache
    if (second == nullptr || second->entries.size() != 1 || second->entries[0].name != "Warm Lead")
        fail("library: rescan differs");

    // A large library: 2000 small presets must scan, cache and reload quickly.
    std::filesystem::create_directories(temp / "Big");
    for (int i = 0; i < 2000; ++i)
    {
        std::ofstream preset(temp / "Big" / ("p" + std::to_string(i) + ".Synthax1Preset"));
        preset << R"({ "fileType": "Synthax1Preset", "presetName": "Preset )" << i << R"(", "category": "Pad", "tags": ["a"], "preset": { "parameters": {} } })";
    }
    const std::vector<synth::LibraryRoot> bigRoots { { temp / "Big", "Big" } };
    const auto started = std::chrono::steady_clock::now();
    const auto big = synth::scanLibrary(bigRoots, temp / "big.bin", {}, {});
    const auto scanned = std::chrono::steady_clock::now();
    const auto reloaded = synth::loadCachedLibrary(bigRoots, temp / "big.bin");
    const auto loaded = std::chrono::steady_clock::now();
    if (big == nullptr || big->entries.size() != 2000 || reloaded == nullptr || reloaded->entries.size() != 2000
        || big->searchText.size() != 2000)
        fail("library: large scan");
    std::printf("library: 2000 presets scanned in %lld ms, reloaded from the cache in %lld ms\n",
                static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(scanned - started).count()),
                static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(loaded - scanned).count()));

    synth::LibraryUserStore store(temp / "user.json");
    store.set(first->idOf(one), { true, 4, "keep" });
    const synth::LibraryUserStore reopened(temp / "user.json");
    const auto data = reopened.get(first->idOf(one));
    if (!data.favourite || data.rating != 4 || data.notes != "keep")
        fail("library: user data did not survive a reload");
    std::filesystem::remove_all(temp);
}

void checkRealFiles(HostlessProcessor& processor, const std::filesystem::path& directory)
{
    int files = 0;
    int programs = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(directory))
    {
        auto extension = entry.path().extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (extension != ".fxp" && extension != ".fxb")
            continue;

        const auto utf8 = entry.path().u8string();
        const auto path = std::string(utf8.begin(), utf8.end());
        const auto file = juce::File(juce::String::fromUTF8(path.c_str()));
        const auto label = file.getFileName().toStdString();
        const auto imported = legacy::importFile(processor.parameters, file);
        if (!imported.ok)
        {
            if (!imported.message.contains("another plugin")) // other synths' files may share the folder
                fail(label + ": " + imported.message.toStdString());
            continue;
        }
        ++files;

        std::vector<juce::ValueTree> states;
        std::vector<juce::String> names;
        for (const auto& program : imported.programs)
        {
            ++programs;
            if (!program.rejected.empty())
                fail(label + " / " + program.name.toStdString() + ": registry rejected " + program.rejected.front());
            if (!legacy::isUneditedImport(program.state))
                fail(label + " / " + program.name.toStdString() + ": a fresh import does not count as unedited");
            states.push_back(program.state);
            names.push_back(program.name);
        }

        std::vector<std::uint8_t> bytes;
        juce::String error;
        if (!legacy::buildBank(states, names, bytes, error))
        {
            fail(label + ": export refused: " + error.toStdString());
            continue;
        }
        const auto original = legacy::readFile(path, legacy::juceZlib());
        const auto exported = legacy::read(bytes, legacy::juceZlib());
        if (!original.ok() || !exported.ok())
        {
            fail(label + ": re-read failed: " + original.error + exported.error);
            continue;
        }
        for (std::size_t index = 0; index < original.document->patches.size(); ++index)
            if (original.document->patches[index].values != exported.document->patches[index].values)
            {
                fail(label + ": exported program " + std::to_string(index + 1) + " differs from the original");
                break;
            }
    }
    std::printf("imported %d real file(s), %d program(s)\n", files, programs);
}
} // namespace

int main(int argc, char** argv)
{
    checkRegistryCoverage();
    checkLibrary();
    {
        HostlessProcessor historyProcessor;
        checkParameterHistory(historyProcessor);
    }
    if (argc > 1)
    {
        HostlessProcessor processor;
        checkRealFiles(processor, argv[1]);
    }
    std::printf(failures == 0 ? "LegacyImportTest: all passed\n" : "LegacyImportTest: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
