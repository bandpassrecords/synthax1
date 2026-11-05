#pragma once

// Reader/writer for Sylenth1 program (.fxp) and bank (.fxb) files.
//
// Free of JUCE and of the Synthax1 registry: it knows the container layout and the raw
// Sylenth1 chunk only. Values stay in Sylenth1's normalized 0..1 domain, so a read followed
// by a write keeps every value and banks assembled here load in Sylenth1 itself.
// SylenthPatchMapper translates values into Synthax1 parameters.
//
// Container (big-endian):
//   'CcnK' byteSize magic version 'syl1' fxVersion count ...
//     FPCh : name[28] chunkSize chunk          program (what Sylenth1 writes)
//     FBCh : future[128] chunkSize chunk       bank    (what Sylenth1 writes)
//     FxCk : name[28] float params[count]      parameter-list program
//     FxBk : future[128] FxCk programs[count]  parameter-list bank
//
// Sylenth1 chunk (little-endian), established from 264 real files:
//   '1lys' version programCount fieldsPerProgram currentProgram      (20 bytes)
//   programCount x { float fields[fieldsPerProgram]; char name[36]; }
// fieldsPerProgram is 242 (2.0x), 243 (2.2x and 3.0x: + ZEQ mode) or 244 (3.045: + one more).
// Version 3045 stores '1lys' version followed by a zlib stream holding the chunk above;
// 3031 does not, so compression is detected from the data, not the version. Banks hold
// 512 programs (4 x 128); unused slots are Sylenth1's Init program.

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace synth::sylenth
{
enum class Field : int
{
#define X(name, kind) name,
#include "SylenthFields.inc"
#undef X
    Count
};

inline constexpr int fieldCount = static_cast<int>(Field::Count);
static_assert(fieldCount == 242, "Sylenth1 programs carry at least 242 fields");

inline constexpr int nameBytesInChunk = 36;
inline constexpr int nameBytesInFxHeader = 28;
inline constexpr int chunkHeaderBytes = 20;
inline constexpr int programsPerBank = 512;

std::string_view fieldName(Field field) noexcept;
std::string_view fieldKind(Field field) noexcept; // "Continuous" or the template enum name
std::optional<Field> fieldFromName(std::string_view name) noexcept;

struct Patch
{
    std::string name;
    std::array<float, fieldCount> values {}; // normalized 0..1, file order
    std::vector<float> extraFields;          // fields after the 242 base ones, verbatim; [0] is ZEQ mode
    // The stored 36-byte name buffer. Sylenth1 leaves stale bytes after the terminator; they
    // are written back unchanged while `name` still matches, so files round-trip exactly.
    std::optional<std::array<char, nameBytesInChunk>> rawName;

    float operator[](Field field) const noexcept { return values[static_cast<std::size_t>(field)]; }
    float& operator[](Field field) noexcept { return values[static_cast<std::size_t>(field)]; }
    std::optional<float> zeqMode() const noexcept
    {
        return extraFields.empty() ? std::nullopt : std::optional<float>(extraFields.front());
    }
};

// Sylenth1's Init program (the filler of unused bank slots), with 244 fields.
Patch initPatch();

enum class ContainerKind
{
    ChunkProgram,     // FPCh
    ChunkBank,        // FBCh
    ParameterProgram, // FxCk
    ParameterBank,    // FxBk
};

// Everything needed to write a file back the way it was read.
struct ContainerInfo
{
    ContainerKind kind = ContainerKind::ChunkBank;
    std::int32_t fxpVersion = 1;            // outer container format version
    std::int32_t fxVersion = 1;             // plugin version in the outer header (Sylenth1 writes 1)
    std::int32_t outerCurrentProgram = 0;   // fxb v2 keeps it at the start of 'future'
    std::vector<std::uint8_t> future;       // the bank header's 128 reserved bytes, verbatim when read
    std::int32_t chunkVersion = 3045;
    std::int32_t fieldsPerProgram = 244;
    std::int32_t chunkCurrentProgram = 0;   // selected program, last chunk header word
    bool compressed = true;                 // zlib body (3045)
    std::vector<std::uint8_t> chunkTail;    // unrecognised bytes after the last program, kept verbatim
};

struct Document
{
    ContainerInfo info;
    std::vector<Patch> patches; // one entry for a program file
};

// zlib (RFC 1950) streams. inflate must stop with failure past `limit` output bytes.
// Without a deflate function, writing uses uncompressed (stored) zlib blocks, which every
// zlib reader accepts.
struct Zlib
{
    std::function<bool(std::span<const std::uint8_t> in, std::vector<std::uint8_t>& out, std::size_t limit)> inflate;
    std::function<std::vector<std::uint8_t>(std::span<const std::uint8_t> in)> deflate;
};

struct ReadResult
{
    std::optional<Document> document;
    std::string error;
    std::vector<std::string> warnings;

    bool ok() const noexcept { return document.has_value(); }
};

// Sniffs the container; never throws, never reads past the span.
ReadResult read(std::span<const std::uint8_t> bytes, const Zlib& zlib = {});
ReadResult readFile(const std::string& utf8Path, const Zlib& zlib = {});

// Parses a bare chunk (starting with '1lys'), as a host hands it to a plugin.
ReadResult readChunk(std::span<const std::uint8_t> chunk, const Zlib& zlib = {});
bool looksLikeChunk(std::span<const std::uint8_t> bytes) noexcept;

// Writers. Patches with fewer fields than info.fieldsPerProgram are completed from Init.
std::vector<std::uint8_t> writeBank(const std::vector<Patch>& patches, const ContainerInfo& info, const Zlib& zlib = {});
std::vector<std::uint8_t> writeProgram(const Patch& patch, const ContainerInfo& info, const Zlib& zlib = {});
std::vector<std::uint8_t> write(const Document& document, const Zlib& zlib = {});

// Fills a bank to `count` programs with Init, the way Sylenth1 leaves unused slots.
void padWithInit(std::vector<Patch>& patches, std::size_t count = programsPerBank);

// Container settings for a new file holding these sources: the newest chunk layout among
// them, compressed if any source was.
ContainerInfo infoForSources(const std::vector<ContainerInfo>& sources, ContainerKind kind);

bool writeFile(const std::string& utf8Path, std::span<const std::uint8_t> bytes, std::string& error);

// Uncompressed zlib stream (stored blocks + Adler-32).
std::vector<std::uint8_t> storedZlib(std::span<const std::uint8_t> data);
} // namespace synth::sylenth
