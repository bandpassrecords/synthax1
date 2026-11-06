// JUCE-free tests for the the reference synth .fxp/.fxb container layer.
// Optional argument: a directory of real .fxp/.fxb files; each must parse, and writing it
// back must reproduce it (byte for byte when uncompressed, value for value when compressed).

#include "../../src/legacy/LegacyFormat.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace synth::legacy;

namespace
{
int failures = 0;
bool sawHuffmanStream = false; // a real deflate stream this JUCE-free test cannot decode

void check(bool condition, const std::string& what)
{
    if (!condition)
    {
        ++failures;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

// Inflates zlib streams made of stored blocks only (what storedZlib writes).
Zlib storedOnlyZlib()
{
    Zlib zlib;
    zlib.inflate = [](std::span<const std::uint8_t> in, std::vector<std::uint8_t>& out, std::size_t limit) {
        std::size_t at = 2;
        for (;;)
        {
            if (at + 5 > in.size() || (in[at] & 0x06) != 0)
            {
                sawHuffmanStream = true; // not a stored block
                return false;
            }
            const bool last = (in[at] & 1) != 0;
            const std::size_t length = in[at + 1] | (std::size_t(in[at + 2]) << 8);
            at += 5;
            if (at + length > in.size() || out.size() + length > limit)
                return false;
            out.insert(out.end(), in.begin() + static_cast<std::ptrdiff_t>(at), in.begin() + static_cast<std::ptrdiff_t>(at + length));
            at += length;
            if (last)
                return at + 4 == in.size();
        }
    };
    return zlib;
}

Patch makePatch(const std::string& name, int seed, std::size_t extras)
{
    Patch patch;
    patch.name = name;
    for (int index = 0; index < fieldCount; ++index)
        patch.values[static_cast<std::size_t>(index)] = static_cast<float>((index * 37 + seed * 11) % 101) / 100.0f;
    for (std::size_t extra = 0; extra < extras; ++extra)
        patch.extraFields.push_back(extra == 0 ? 1.0f : 0.25f);
    return patch;
}

ContainerInfo layout(std::int32_t version, std::int32_t fields, bool compressed)
{
    ContainerInfo info;
    info.chunkVersion = version;
    info.fieldsPerProgram = fields;
    info.compressed = compressed;
    info.chunkCurrentProgram = 2;
    return info;
}

std::uint32_t be32(const std::vector<std::uint8_t>& b, std::size_t at)
{
    return (std::uint32_t(b[at]) << 24) | (std::uint32_t(b[at + 1]) << 16) | (std::uint32_t(b[at + 2]) << 8) | b[at + 3];
}

std::int32_t le32(const std::vector<std::uint8_t>& b, std::size_t at)
{
    return static_cast<std::int32_t>(b[at] | (std::uint32_t(b[at + 1]) << 8) | (std::uint32_t(b[at + 2]) << 16) | (std::uint32_t(b[at + 3]) << 24));
}

bool samePatches(const std::vector<Patch>& a, const std::vector<Patch>& b)
{
    if (a.size() != b.size())
        return false;
    for (std::size_t index = 0; index < a.size(); ++index)
        if (a[index].name != b[index].name || a[index].values != b[index].values || a[index].extraFields != b[index].extraFields)
            return false;
    return true;
}

void testFieldTable()
{
    check(fieldName(Field::AmpEnvAAttack) == "AmpEnvAAttack", "first field name");
    check(fieldName(Field::YPartSelect) == "YPartSelect", "last field name");
    check(static_cast<int>(Field::OscA1Wave) == (388 - chunkHeaderBytes) / 4, "OscA1Wave sits at template offset 388");
    check(static_cast<int>(Field::YPartSelect) == (984 - chunkHeaderBytes) / 4, "YPartSelect sits at template offset 984");
    check(fieldFromName("FilterACutoff") == Field::FilterACutoff, "lookup by name");
    check(fieldKind(Field::OscA1Wave) == "OSCWAVE", "enumerated kind");
}

void testUncompressedBank(std::int32_t version, std::int32_t fields)
{
    const auto tag = std::to_string(version) + "/" + std::to_string(fields);
    const auto extras = static_cast<std::size_t>(fields - fieldCount);
    const std::vector<Patch> patches { makePatch("Lead One", 1, extras),
                                       makePatch("A name that is far too long for thirty-six bytes", 2, extras),
                                       makePatch("Pad", 3, extras) };
    const auto bytes = writeBank(patches, layout(version, fields, false));
    const auto chunkSize = static_cast<std::size_t>(chunkHeaderBytes) + patches.size() * (static_cast<std::size_t>(fields) * 4 + 36);

    check(bytes.size() == 160 + chunkSize, "bank size " + tag);
    check(std::memcmp(bytes.data(), "CcnK", 4) == 0 && std::memcmp(bytes.data() + 8, "FBCh", 4) == 0, "bank magic " + tag);
    check(be32(bytes, 4) == bytes.size() - 8, "byteSize " + tag);
    check(be32(bytes, 156) == chunkSize, "chunkSize " + tag);
    check(le32(bytes, 164) == version && le32(bytes, 168) == 3 && le32(bytes, 172) == fields && le32(bytes, 176) == 2,
          "chunk header is version, programs, fields, current " + tag);

    const auto result = read(bytes);
    check(result.ok() && result.warnings.empty(), "bank reads cleanly " + tag + " " + result.error);
    if (!result.ok())
        return;
    const auto& doc = *result.document;
    check(!doc.info.compressed && doc.info.fieldsPerProgram == fields && doc.info.chunkCurrentProgram == 2, "layout kept " + tag);
    check(doc.patches[1].name.size() == 35, "long name truncated to 35 bytes");
    auto stale = doc;
    stale.patches[0].rawName->at(20) = 'X'; // a stale byte after the terminator survives a rewrite
    check(write(stale)[160 + 20 + static_cast<std::size_t>(fields) * 4 + 20] == 'X', "stale name bytes kept");
    stale.patches[0].name = "Renamed";
    const auto renamed = read(write(stale));
    check(renamed.ok() && renamed.document->patches[0].name == "Renamed", "rename replaces the raw buffer");
    check(doc.patches[2].values == patches[2].values && doc.patches[2].extraFields == patches[2].extraFields, "values survive " + tag);
    check(doc.patches[0].zeqMode().has_value() == (fields > fieldCount), "zeq presence " + tag);
    check(write(doc) == bytes, "bank round-trips byte for byte " + tag);
}

void testCompressedBank()
{
    const std::vector<Patch> patches { makePatch("One", 1, 2), makePatch("Two", 2, 2) };
    const auto bytes = writeBank(patches, layout(3045, 244, true));
    check(std::memcmp(bytes.data() + 160, "1lys", 4) == 0 && le32(bytes, 164) == 3045, "compressed chunk prefix");
    check(bytes[168] == 0x78, "zlib stream follows the version");

    check(!read(bytes).ok() && read(bytes).error.find("zlib") != std::string::npos, "compressed bank needs a decoder");
    const auto result = read(bytes, storedOnlyZlib());
    check(result.ok(), "compressed bank reads " + result.error);
    if (result.ok())
    {
        check(result.document->info.compressed && result.document->info.fieldsPerProgram == 244, "compressed layout kept");
        check(samePatches(result.document->patches, patches), "compressed values survive");
        check(write(*result.document) == bytes, "compressed bank round-trips");
    }
}

void testMixedFieldCounts()
{
    // A 242-field patch written into a 244-field bank is completed from Init (ZEQ 0, extra 0).
    const std::vector<Patch> patches { makePatch("Old", 1, 0) };
    const auto result = read(writeBank(patches, layout(3031, 244, false)));
    check(result.ok() && result.document->patches[0].extraFields == std::vector<float>({ 0.0f, 0.0f }), "missing fields come from Init");

    const auto info = infoForSources({ layout(2202, 243, false), layout(3045, 244, true) }, ContainerKind::ChunkBank);
    check(info.chunkVersion == 3045 && info.fieldsPerProgram == 244 && info.compressed, "newest source layout wins");
}

void testInitAndPadding()
{
    const auto init = initPatch();
    check(init.name == "Init" && init[Field::AmpEnvASustain] == 1.0f && init[Field::OscA1Voices] == 0.125f, "init values");
    std::vector<Patch> patches { makePatch("Mine", 1, 2) };
    padWithInit(patches);
    check(patches.size() == programsPerBank && patches.back().name == "Init", "bank padded to 512 with Init");
}

void testProgramAndRawChunk()
{
    auto info = layout(2211, 243, false);
    info.kind = ContainerKind::ChunkProgram;
    const auto patch = makePatch("Pluck", 4, 1);
    const auto bytes = writeProgram(patch, info);
    check(std::memcmp(bytes.data() + 8, "FPCh", 4) == 0 && be32(bytes, 24) == 243, "program header counts fields");
    const auto result = read(bytes);
    check(result.ok() && result.document->patches.size() == 1 && result.document->patches[0].name == "Pluck", "program reads");
    check(result.ok() && write(*result.document) == bytes, "program round-trips");

    const std::span<const std::uint8_t> chunk(bytes.data() + 60, bytes.size() - 60);
    check(looksLikeChunk(chunk), "program chunk sniffed");
    const auto raw = readChunk(chunk);
    check(raw.ok() && raw.document->info.kind == ContainerKind::ChunkProgram, "raw chunk reads as program");
    check(!looksLikeChunk(std::span<const std::uint8_t>(bytes)), "container is not a raw chunk");
}

void testParameterContainers()
{
    Document doc;
    doc.info.kind = ContainerKind::ParameterBank;
    doc.patches = { makePatch("P1", 5, 1), makePatch("P2", 6, 1) };
    const auto bytes = write(doc);
    const auto result = read(bytes);
    check(result.ok() && samePatches(result.document->patches, doc.patches), "FxBk reads " + result.error);
    check(result.ok() && write(*result.document) == bytes, "FxBk round-trips");
}

void testRejections()
{
    check(!read({}).ok(), "empty rejected");
    auto bytes = writeBank({ makePatch("X", 1, 1) }, layout(2211, 243, false));
    auto otherPlugin = bytes;
    std::memcpy(otherPlugin.data() + 16, "LINE", 4);
    check(read(otherPlugin).error.find("another plugin") != std::string::npos, "other plugin rejected");

    auto truncated = bytes;
    truncated.resize(truncated.size() - 100);
    check(!read(truncated).ok(), "truncated bank rejected");

    auto hugeCount = bytes;
    hugeCount[168] = 0xff;
    hugeCount[169] = 0xff;
    check(!read(hugeCount).ok(), "implausible program count rejected");
}

int skippedCompressed = 0;

int checkDirectory(const std::filesystem::path& directory)
{
    int files = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(directory))
    {
        auto ext = entry.path().extension().string();
        for (auto& ch : ext)
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (ext != ".fxp" && ext != ".fxb")
            continue;
        ++files;
        const auto utf8 = entry.path().u8string();
        sawHuffmanStream = false;
        const auto result = readFile(std::string(utf8.begin(), utf8.end()), storedOnlyZlib());
        const auto label = entry.path().filename().string();
        if (!result.ok() && sawHuffmanStream)
        {
            ++skippedCompressed; // decoded by LegacyImportTest with JUCE's zlib
            continue;
        }
        if (!result.ok())
        {
            const auto otherPlugin = result.error.find("another plugin") != std::string::npos;
            std::printf("  %s: %s %s\n", label.c_str(), otherPlugin ? "skipped," : "FAILED:", result.error.c_str());
            failures += otherPlugin ? 0 : 1;
            continue;
        }

        std::ifstream in(entry.path(), std::ios::binary);
        const std::vector<std::uint8_t> original((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const auto rewritten = write(*result.document);
        const auto again = read(rewritten, storedOnlyZlib());
        auto comparable = original;
        if (!result.warnings.empty() && comparable.size() >= 8 && rewritten.size() >= 8)
            std::copy(rewritten.begin() + 4, rewritten.begin() + 8, comparable.begin() + 4); // corrected byteSize
        const auto exact = rewritten == comparable;
        const auto equivalent = again.ok() && samePatches(again.document->patches, result.document->patches);
        check(exact || (result.document->info.compressed && equivalent), label + ": rewrite differs");
        if (!result.warnings.empty())
            std::printf("  %s: %s\n", label.c_str(), result.warnings.front().c_str());
    }
    return files;
}
} // namespace

int main(int argc, char** argv)
{
    testFieldTable();
    testUncompressedBank(2011, 242);
    testUncompressedBank(2202, 243);
    testUncompressedBank(3031, 243);
    testUncompressedBank(3045, 244);
    testCompressedBank();
    testMixedFieldCounts();
    testInitAndPadding();
    testProgramAndRawChunk();
    testParameterContainers();
    testRejections();
    if (argc > 1)
    {
        const auto files = checkDirectory(argv[1]);
        std::printf("checked %d real file(s); %d zlib-compressed one(s) left to LegacyImportTest\n",
                    files - skippedCompressed, skippedCompressed);
    }

    std::printf(failures == 0 ? "LegacyFormatTest: all passed\n" : "LegacyFormatTest: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
