#include "SylenthImport.h"

#include "../plugin/ParameterRegistry.h"
#include "../presets/PresetManager.h"

#include <algorithm>
#include <array>
#include <unordered_map>

namespace synth::sylenth
{
namespace
{
const juce::Identifier sourceProperty { "sylenth_source" };
const juce::Identifier fingerprintProperty { "sylenth_fingerprint" };
const juce::Identifier modWheelProperty { "sylenth_mod_wheel" };

juce::String fingerprintText(const juce::ValueTree& state)
{
    return juce::String::toHexString(static_cast<juce::int64>(fingerprintPresetState(state).hash));
}

bool inflateWithJuce(std::span<const std::uint8_t> in, std::vector<std::uint8_t>& out, std::size_t limit)
{
    juce::MemoryInputStream source(in.data(), in.size(), false);
    juce::GZIPDecompressorInputStream inflater(&source, false, juce::GZIPDecompressorInputStream::zlibFormat);
    std::array<std::uint8_t, 65536> buffer {};
    out.clear();
    for (;;)
    {
        const auto count = inflater.read(buffer.data(), static_cast<int>(buffer.size()));
        if (count < 0)
            return false;
        if (count == 0)
            break;
        if (out.size() + static_cast<std::size_t>(count) > limit)
            return false;
        out.insert(out.end(), buffer.begin(), buffer.begin() + count);
    }
    return !out.empty(); // the format layer validates the inflated chunk's structure
}

std::vector<std::uint8_t> deflateWithJuce(std::span<const std::uint8_t> in)
{
    juce::MemoryOutputStream compressed;
    {
        juce::GZIPCompressorOutputStream deflater(compressed, 6); // default window: zlib-wrapped stream
        deflater.write(in.data(), in.size());
    }
    const auto* data = static_cast<const std::uint8_t*>(compressed.getData());
    return { data, data + compressed.getDataSize() };
}

juce::String nameFromBytes(const std::string& raw)
{
    // Sylenth1 names are 8-bit; accept UTF-8 when valid, otherwise treat as Latin-1.
    if (juce::CharPointer_UTF8::isValidString(raw.c_str(), static_cast<int>(raw.size())))
        return juce::String::fromUTF8(raw.c_str(), static_cast<int>(raw.size())).trim();
    juce::String latin1;
    for (const auto ch : raw)
        latin1 += juce::String::charToString(static_cast<juce::juce_wchar>(static_cast<unsigned char>(ch)));
    return latin1.trim();
}
} // namespace

const Zlib& juceZlib()
{
    static const Zlib zlib { inflateWithJuce, deflateWithJuce };
    return zlib;
}

ImportedProgram buildProgram(juce::AudioProcessorValueTreeState& parameters, const Patch& patch, const ContainerInfo& info)
{
    ImportedProgram program;
    program.name = nameFromBytes(patch.name);
    if (program.name.isEmpty())
        program.name = "Untitled";

    auto state = prepareInitPresetState(parameters).state;
    std::unordered_map<std::string, juce::ValueTree> byId;
    for (auto child : state)
        if (child.hasType("PARAM"))
            byId.emplace(child.getProperty("id").toString().toStdString(), child);

    auto mapped = mapPatch(patch);
    for (const auto& assignment : mapped.assignments)
    {
        if (assignment.parameterId == "performance.mod_wheel")
        {
            state.setProperty(modWheelProperty, assignment.value, nullptr);
            continue;
        }
        const auto* spec = findParameterSpec(assignment.parameterId);
        const auto found = byId.find(assignment.parameterId);
        if (spec == nullptr || found == byId.end())
        {
            program.rejected.push_back(assignment.parameterId + " (unregistered)");
            continue;
        }

        float value = 0.0f;
        if (spec->kind == ParameterKind::Choice)
        {
            const auto choice = std::find(spec->choices.begin(), spec->choices.end(), assignment.choice);
            if (choice == spec->choices.end())
            {
                program.rejected.push_back(assignment.parameterId + " = '" + assignment.choice + "' (not a choice)");
                continue;
            }
            value = static_cast<float>(std::distance(spec->choices.begin(), choice));
        }
        else
        {
            value = clampPhysicalParameterValue(*spec, assignment.value);
        }
        found->second.setProperty("value", value, nullptr);
    }

    auto programInfo = info;
    programInfo.kind = ContainerKind::ChunkProgram;
    programInfo.chunkTail.clear();
    programInfo.future.clear();
    programInfo.chunkCurrentProgram = 0;
    const auto bytes = writeProgram(patch, programInfo, juceZlib());
    state.setProperty("current_preset", program.name, nullptr);
    state.setProperty(sourceProperty, juce::Base64::toBase64(bytes.data(), bytes.size()), nullptr);
    state.setProperty(fingerprintProperty, fingerprintText(state), nullptr);

    program.state = state;
    program.notes = std::move(mapped.notes);
    return program;
}

ImportResult importDocument(juce::AudioProcessorValueTreeState& parameters, const Document& document,
                            const juce::String& fallbackName)
{
    ImportResult result;
    for (const auto& patch : document.patches)
        result.programs.push_back(buildProgram(parameters, patch, document.info));
    const auto unnamed = result.programs.size() == 1
                      && (result.programs.front().name == "Init" || result.programs.front().name == "Untitled");
    if (unnamed && fallbackName.isNotEmpty())
    {
        auto& program = result.programs.front();
        program.name = fallbackName;
        program.state.setProperty("current_preset", fallbackName, nullptr);
    }
    result.ok = !result.programs.empty();
    result.message = result.ok ? summarize(result) : juce::String("Sylenth1 file contains no programs");
    return result;
}

ImportResult importBytes(juce::AudioProcessorValueTreeState& parameters, std::span<const std::uint8_t> bytes,
                         const juce::String& fallbackName)
{
    const auto parsed = looksLikeChunk(bytes) ? readChunk(bytes, juceZlib()) : sylenth::read(bytes, juceZlib());
    if (!parsed.ok())
    {
        ImportResult failed;
        failed.message = "Sylenth1 import failed: " + juce::String(parsed.error);
        return failed;
    }

    auto result = importDocument(parameters, *parsed.document, fallbackName);
    result.fileWarnings = parsed.warnings;
    return result;
}

ImportResult importFile(juce::AudioProcessorValueTreeState& parameters, const juce::File& file)
{
    if (file.getSize() > 64 * 1024 * 1024)
    {
        ImportResult failed;
        failed.message = "Sylenth1 file exceeds the 64 MB limit";
        return failed;
    }

    juce::MemoryBlock data;
    if (!file.loadFileAsData(data))
    {
        ImportResult failed;
        failed.message = "Cannot read " + file.getFileName();
        return failed;
    }
    return importBytes(parameters, { static_cast<const std::uint8_t*>(data.getData()), data.getSize() },
                       file.getFileNameWithoutExtension());
}

std::optional<SourceProgram> sourceProgram(const juce::ValueTree& state)
{
    const auto encoded = state.getProperty(sourceProperty).toString();
    if (encoded.isEmpty())
        return std::nullopt;

    juce::MemoryOutputStream decoded;
    if (!juce::Base64::convertFromBase64(decoded, encoded))
        return std::nullopt;

    const auto parsed = sylenth::read({ static_cast<const std::uint8_t*>(decoded.getData()), decoded.getDataSize() }, juceZlib());
    if (!parsed.ok() || parsed.document->patches.size() != 1)
        return std::nullopt;
    return SourceProgram { parsed.document->patches.front(), parsed.document->info };
}

bool buildBank(const std::vector<juce::ValueTree>& states, const std::vector<juce::String>& names,
               std::vector<std::uint8_t>& bytes, juce::String& error)
{
    std::vector<Patch> patches;
    std::vector<ContainerInfo> sources;
    juce::StringArray problems;
    for (std::size_t index = 0; index < states.size(); ++index)
    {
        const auto label = index < names.size() ? names[index] : juce::String(static_cast<int>(index + 1));
        auto source = sourceProgram(states[index]);
        if (!source)
        {
            problems.add(label + " (not imported from Sylenth1)");
            continue;
        }
        if (!isUneditedImport(states[index]))
        {
            problems.add(label + " (edited since import)");
            continue;
        }
        sources.push_back(source->info);
        if (index < names.size() && names[index].isNotEmpty())
            source->patch.name = names[index].toStdString(); // keep renames made in the bank builder
        patches.push_back(std::move(source->patch));
    }

    if (!problems.isEmpty())
    {
        error = "Cannot export as Sylenth1 bank: " + problems.joinIntoString(", ");
        return false;
    }
    if (patches.empty())
    {
        error = "Nothing selected to export";
        return false;
    }

    if (patches.size() > static_cast<std::size_t>(programsPerBank))
    {
        error = "A Sylenth1 bank holds " + juce::String(programsPerBank) + " programs";
        return false;
    }
    padWithInit(patches);
    bytes = writeBank(patches, infoForSources(sources, ContainerKind::ChunkBank), juceZlib());
    return true;
}

bool isUneditedImport(const juce::ValueTree& state)
{
    const auto recorded = state.getProperty(fingerprintProperty).toString();
    return recorded.isNotEmpty() && recorded == fingerprintText(state);
}

juce::String summarize(const ImportResult& result)
{
    int approximations = 0;
    int dropped = 0;
    int rejected = 0;
    for (const auto& program : result.programs)
    {
        for (const auto& note : program.notes)
            (note.kind == ImportNote::Kind::Dropped ? dropped : approximations) += 1;
        rejected += static_cast<int>(program.rejected.size());
    }

    auto text = "Imported " + juce::String(static_cast<int>(result.programs.size())) + " Sylenth1 program"
              + (result.programs.size() == 1 ? "" : "s");
    juce::StringArray details;
    if (approximations > 0)
        details.add(juce::String(approximations) + " approximated");
    if (dropped > 0)
        details.add(juce::String(dropped) + " dropped");
    if (rejected > 0)
        details.add(juce::String(rejected) + " rejected");
    return details.isEmpty() ? text : text + " (" + details.joinIntoString(", ") + ")";
}
} // namespace synth::sylenth
