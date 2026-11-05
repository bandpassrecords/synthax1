// Offline reference renderer: hosts the installed Sylenth1 VST3, plays the sound-parity
// reference set (docs/SOUND_PARITY.md) and writes WAV files, with no DAW involved.
//
//   SylenthRenderHost --plugin <Sylenth1.vst3> --bank <Synthax1Reference.fxb>
//                     --manifest <Synthax1Reference.json> --out <dir> [--rate 48000] [--block 256]
//
// For each program in the manifest the host builds a bank holding that program in slot 1
// (selected), hands it to the plug-in as VST3 component state in the form Cubase stores
// ("VstW" header + .fxb), checks the plug-in now reports the program's parameter values,
// then renders the program's notes offline. It writes one WAV per program and the whole
// timeline as SylenthReference.wav (matching the manifest, like a Cubase export would).

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include "../../src/sylenth/SylenthFormat.h"

#include <cmath>
#include <cstdio>
#include <optional>

using namespace synth;

namespace
{
// ---------------------------------------------------------------- zlib through JUCE
bool inflate(std::span<const std::uint8_t> in, std::vector<std::uint8_t>& out, std::size_t limit)
{
    juce::MemoryInputStream source(in.data(), in.size(), false);
    juce::GZIPDecompressorInputStream inflater(&source, false, juce::GZIPDecompressorInputStream::zlibFormat);
    std::array<std::uint8_t, 65536> buffer {};
    out.clear();
    for (;;)
    {
        const auto count = inflater.read(buffer.data(), static_cast<int>(buffer.size()));
        if (count <= 0)
            break;
        if (out.size() + static_cast<std::size_t>(count) > limit)
            return false;
        out.insert(out.end(), buffer.begin(), buffer.begin() + count);
    }
    return !out.empty();
}

std::vector<std::uint8_t> deflate(std::span<const std::uint8_t> in)
{
    juce::MemoryOutputStream compressed;
    {
        juce::GZIPCompressorOutputStream deflater(compressed, 6);
        deflater.write(in.data(), in.size());
    }
    const auto* data = static_cast<const std::uint8_t*>(compressed.getData());
    return { data, data + compressed.getDataSize() };
}

const sylenth::Zlib zlib { inflate, deflate };

// ---------------------------------------------------------------- transport
class FixedTempoPlayHead final : public juce::AudioPlayHead
{
public:
    FixedTempoPlayHead(double bpm, double rate) : tempo(bpm), sampleRate(rate) {}

    void setPosition(juce::int64 samples) noexcept { position = samples; }

    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo info;
        info.setBpm(tempo);
        info.setTimeSignature(juce::AudioPlayHead::TimeSignature { 4, 4 });
        info.setTimeInSamples(position);
        info.setTimeInSeconds(static_cast<double>(position) / sampleRate);
        info.setPpqPosition(static_cast<double>(position) / sampleRate * tempo / 60.0);
        info.setIsPlaying(true);
        return info;
    }

private:
    double tempo;
    double sampleRate;
    juce::int64 position = 0;
};

// ---------------------------------------------------------------- helpers
juce::String argument(const juce::StringArray& args, const juce::String& name, const juce::String& fallback = {})
{
    const auto index = args.indexOf(name);
    return index >= 0 && index + 1 < args.size() ? args[index + 1] : fallback;
}

// Plug-in state as JUCE's VST3 host expects it, carrying what Cubase stores in
// 'audioComponent': "VstW" (size 8, version 1, not bypassed) followed by the bank file.
juce::MemoryBlock componentState(const std::vector<std::uint8_t>& fxb)
{
    juce::MemoryOutputStream blob;
    blob.write("VstW", 4);
    blob.writeIntBigEndian(8);
    blob.writeIntBigEndian(1);
    blob.writeIntBigEndian(0);
    blob.write(fxb.data(), fxb.size());

    juce::XmlElement state("VST3PluginState");
    juce::MemoryBlock component(blob.getData(), blob.getDataSize());
    state.createNewChildElement("IComponent")->addTextElement(component.toBase64Encoding());
    juce::MemoryBlock data;
    juce::AudioProcessor::copyXmlToBinary(state, data);
    return data;
}

juce::String normalisedName(const juce::String& name)
{
    return name.retainCharacters("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789").toLowerCase();
}

// Counts the plug-in parameters whose name matches a Sylenth1 field and whose value differs
// from the patch: a non-zero result means the state did not take.
struct StateCheck
{
    int matched = 0;
    int different = 0;
    juce::String firstDifference;
};

StateCheck checkState(juce::AudioPluginInstance& plugin, const sylenth::Patch& patch)
{
    StateCheck check;
    for (auto* parameter : plugin.getParameters())
    {
        const auto name = normalisedName(parameter->getName(128));
        for (int index = 0; index < sylenth::fieldCount; ++index)
        {
            const auto field = static_cast<sylenth::Field>(index);
            if (normalisedName(juce::String(std::string(sylenth::fieldName(field)))) != name)
                continue;
            ++check.matched;
            const auto expected = patch[field];
            if (std::abs(parameter->getValue() - expected) > 0.002f)
            {
                ++check.different;
                if (check.firstDifference.isEmpty())
                    check.firstDifference = parameter->getName(64) + " = " + juce::String(parameter->getValue(), 4)
                                          + " (expected " + juce::String(expected, 4) + ")";
            }
        }
    }
    return check;
}

bool writeWav(const juce::File& file, const juce::AudioBuffer<float>& audio, double rate)
{
    file.deleteFile();
    auto stream = std::unique_ptr<juce::OutputStream>(file.createOutputStream().release());
    if (stream == nullptr)
        return false;
    juce::WavAudioFormat format;
    const auto options = juce::AudioFormatWriterOptions {}
                             .withSampleRate(rate)
                             .withNumChannels(audio.getNumChannels())
                             .withBitsPerSample(32)
                             .withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);
    auto writer = format.createWriterFor(stream, options);
    return writer != nullptr && writer->writeFromAudioSampleBuffer(audio, 0, audio.getNumSamples());
}

int run(const juce::StringArray& args)
{
    const juce::File pluginFile(argument(args, "--plugin", "C:/Program Files/Common Files/VST3/LennarDigital/Sylenth1.vst3"));
    const juce::File bankFile(argument(args, "--bank"));
    const juce::File manifestFile(argument(args, "--manifest"));
    const juce::File outDir(argument(args, "--out"));
    const auto rate = argument(args, "--rate", "48000").getDoubleValue();
    const auto block = argument(args, "--block", "256").getIntValue();
    if (!bankFile.existsAsFile() || !manifestFile.existsAsFile() || outDir == juce::File())
    {
        std::puts("usage: SylenthRenderHost --plugin <vst3> --bank <fxb> --manifest <json> --out <dir> [--rate 48000] [--block 256]");
        return 2;
    }

    // Reference bank.
    const auto read = sylenth::readFile(bankFile.getFullPathName().toStdString(), zlib);
    if (!read.ok())
    {
        std::printf("cannot read bank: %s\n", read.error.c_str());
        return 1;
    }
    const auto& sourceInfo = read.document->info;

    // Manifest.
    const auto manifest = juce::JSON::parse(manifestFile);
    const auto* programs = manifest["programs"].getArray();
    if (programs == nullptr || programs->isEmpty())
    {
        std::puts("manifest has no programs");
        return 1;
    }
    const auto tempo = static_cast<double>(manifest["tempo_bpm"]);
    const auto totalSeconds = static_cast<double>(manifest["total_s"]);

    // Plug-in.
    juce::VST3PluginFormat format;
    juce::OwnedArray<juce::PluginDescription> types;
    format.findAllTypesForFile(types, pluginFile.getFullPathName());
    if (types.isEmpty())
    {
        std::printf("no VST3 plug-in found in %s\n", pluginFile.getFullPathName().toRawUTF8());
        return 1;
    }
    juce::String error;
    auto plugin = format.createInstanceFromDescription(*types[0], rate, block, error);
    if (plugin == nullptr)
    {
        std::printf("cannot load %s: %s\n", types[0]->name.toRawUTF8(), error.toRawUTF8());
        return 1;
    }
    std::printf("loaded %s %s by %s, %d parameters\n", types[0]->name.toRawUTF8(), types[0]->version.toRawUTF8(),
                types[0]->manufacturerName.toRawUTF8(), plugin->getParameters().size());

    plugin->disableNonMainBuses();
    plugin->setPlayConfigDetails(0, 2, rate, block);
    plugin->setNonRealtime(true);
    FixedTempoPlayHead playHead(tempo, rate);
    plugin->setPlayHead(&playHead);
    plugin->prepareToPlay(rate, block);

    outDir.createDirectory();
    const auto totalSamples = static_cast<int>(std::ceil(totalSeconds * rate));
    juce::AudioBuffer<float> timeline(2, totalSamples);
    timeline.clear();
    int failures = 0;

    for (const auto& entry : *programs)
    {
        const auto index = static_cast<int>(entry["program"]) - 1;
        const auto name = entry["name"].toString();
        const auto startSeconds = static_cast<double>(entry["start_s"]);
        const auto lengthSeconds = static_cast<double>(entry["length_s"]);
        if (index < 0 || index >= static_cast<int>(read.document->patches.size()))
        {
            std::printf("program %d is not in the bank\n", index + 1);
            ++failures;
            continue;
        }

        // A bank with this program in slot 1, selected; the rest Init.
        std::vector<sylenth::Patch> bank { read.document->patches[static_cast<std::size_t>(index)] };
        sylenth::padWithInit(bank);
        auto info = sourceInfo;
        info.chunkCurrentProgram = 0;
        info.future.clear();
        plugin->suspendProcessing(true);
        const auto state = componentState(sylenth::writeBank(bank, info, zlib));
        plugin->setStateInformation(state.getData(), static_cast<int>(state.getSize()));
        juce::MessageManager::getInstance()->runDispatchLoopUntil(30); // let the plug-in finish applying it
        plugin->reset();
        plugin->suspendProcessing(false);

        const auto check = checkState(*plugin, bank.front());
        if (index == 0 || check.matched == 0)
            std::printf("  state check: %d parameters matched to Sylenth1 fields\n", check.matched);
        if (check.matched > 0 && check.different > 0)
        {
            std::printf("  %-28s state did not take: %d of %d parameters differ, e.g. %s\n", name.toRawUTF8(),
                        check.different, check.matched, check.firstDifference.toRawUTF8());
            ++failures;
        }

        // Render the program's notes, shifted so the program starts at time 0.
        const auto samples = static_cast<int>(std::ceil(lengthSeconds * rate));
        juce::AudioBuffer<float> audio(2, samples);
        audio.clear();
        juce::MidiBuffer events;
        for (const auto& note : *entry["notes"].getArray())
        {
            const auto pitch = static_cast<int>(note["pitch"]);
            const auto on = static_cast<int>((static_cast<double>(note["on_s"]) - startSeconds) * rate);
            const auto off = static_cast<int>((static_cast<double>(note["off_s"]) - startSeconds) * rate);
            const auto velocity = note.hasProperty("velocity") ? static_cast<int>(note["velocity"]) : 100;
            events.addEvent(juce::MidiMessage::noteOn(1, pitch, static_cast<juce::uint8>(velocity)), on);
            events.addEvent(juce::MidiMessage::noteOff(1, pitch), off);
        }

        juce::AudioBuffer<float> chunk(std::max(2, plugin->getTotalNumOutputChannels()), block);
        for (int at = 0; at < samples; at += block)
        {
            const auto count = std::min(block, samples - at);
            chunk.setSize(chunk.getNumChannels(), count, false, false, true);
            chunk.clear();
            juce::MidiBuffer slice;
            for (const auto metadata : events)
                if (metadata.samplePosition >= at && metadata.samplePosition < at + count)
                    slice.addEvent(metadata.getMessage(), metadata.samplePosition - at);
            playHead.setPosition(static_cast<juce::int64>(startSeconds * rate) + at);
            plugin->processBlock(chunk, slice);
            for (int channel = 0; channel < 2; ++channel)
                audio.copyFrom(channel, at, chunk, std::min(channel, chunk.getNumChannels() - 1), 0, count);
        }

        const auto peak = audio.getMagnitude(0, samples);
        const auto safe = juce::File::createLegalFileName(name);
        writeWav(outDir.getChildFile(juce::String(index + 1).paddedLeft('0', 2) + " " + safe + ".wav"), audio, rate);
        const auto offset = static_cast<int>(startSeconds * rate);
        for (int channel = 0; channel < 2; ++channel)
            timeline.copyFrom(channel, offset, audio, channel, 0, std::min(samples, totalSamples - offset));
        std::printf("  %2d %-28s peak %6.1f dBFS%s\n", index + 1, name.toRawUTF8(), juce::Decibels::gainToDecibels(peak, -120.0f),
                    peak < 1.0e-5f ? "  SILENT" : "");
        failures += peak < 1.0e-5f ? 1 : 0;
    }

    plugin->releaseResources();
    writeWav(outDir.getChildFile("SylenthReference.wav"), timeline, rate);
    std::printf("wrote %s (%.1f s)%s\n", outDir.getChildFile("SylenthReference.wav").getFullPathName().toRawUTF8(),
                totalSeconds, failures > 0 ? (", " + juce::String(failures) + " problem(s)").toRawUTF8() : "");
    return failures > 0 ? 1 : 0;
}
} // namespace

int main(int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI juce;
    juce::StringArray args;
    for (int index = 1; index < argc; ++index)
        args.add(juce::String::fromUTF8(argv[index]));
    return run(args);
}
