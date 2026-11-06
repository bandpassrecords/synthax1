#include "LegacyFormat.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace synth::legacy
{
namespace
{
constexpr std::array<std::string_view, fieldCount> fieldNames {
#define X(name, kind) #name,
#include "LegacyFields.inc"
#undef X
};

constexpr std::array<std::string_view, fieldCount> fieldKinds {
#define X(name, kind) #kind,
#include "LegacyFields.inc"
#undef X
};

constexpr std::array<float, fieldCount> initValues {
#include "LegacyInit.inc"
};

constexpr std::size_t outerHeaderBytes = 28; // CcnK, byteSize, magic, version, fxID, fxVersion, count
constexpr std::size_t futureBytes = 128;
constexpr std::size_t maximumFileBytes = 64u * 1024u * 1024u;
constexpr std::int32_t maximumPrograms = 4096;
constexpr std::int32_t maximumFields = 4096;

std::filesystem::path pathFromUtf8(const std::string& text)
{
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

// Text before the first NUL of a fixed-size name buffer.
std::string terminated(const std::array<char, nameBytesInChunk>& buffer)
{
    const char* begin = buffer.data();
    return { begin, std::find(begin, begin + buffer.size(), '\0') };
}

std::size_t recordBytes(std::int32_t fields) noexcept
{
    return static_cast<std::size_t>(fields) * 4u + nameBytesInChunk;
}

// ---------------------------------------------------------------- byte cursor
class Reader
{
public:
    explicit Reader(std::span<const std::uint8_t> data) noexcept : bytes(data) {}

    bool has(std::size_t count) const noexcept { return count <= bytes.size() - position; }
    std::size_t remaining() const noexcept { return bytes.size() - position; }

    std::uint32_t u32be() noexcept
    {
        const auto* p = bytes.data() + position;
        position += 4;
        return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | std::uint32_t(p[3]);
    }

    std::uint32_t u32le() noexcept
    {
        const auto* p = bytes.data() + position;
        position += 4;
        return (std::uint32_t(p[3]) << 24) | (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[1]) << 8) | std::uint32_t(p[0]);
    }

    std::int32_t i32be() noexcept { return static_cast<std::int32_t>(u32be()); }
    std::int32_t i32le() noexcept { return static_cast<std::int32_t>(u32le()); }
    float f32be() noexcept { return bitsToFloat(u32be()); }
    float f32le() noexcept { return bitsToFloat(u32le()); }

    std::string_view tag() noexcept
    {
        const auto* p = reinterpret_cast<const char*>(bytes.data() + position);
        position += 4;
        return { p, 4 };
    }

    std::string cString(std::size_t width) noexcept
    {
        const auto* p = reinterpret_cast<const char*>(bytes.data() + position);
        position += width;
        return { p, std::find(p, p + width, '\0') };
    }

    std::span<const std::uint8_t> take(std::size_t count) noexcept
    {
        auto view = bytes.subspan(position, count);
        position += count;
        return view;
    }

    void skip(std::size_t count) noexcept { position += count; }

private:
    static float bitsToFloat(std::uint32_t bits) noexcept
    {
        float value;
        std::memcpy(&value, &bits, sizeof value);
        return value;
    }

    std::span<const std::uint8_t> bytes;
    std::size_t position = 0;
};

class Writer
{
public:
    void u32be(std::uint32_t v)
    {
        for (int shift = 24; shift >= 0; shift -= 8)
            out.push_back(std::uint8_t(v >> shift));
    }

    void u32le(std::uint32_t v)
    {
        for (int shift = 0; shift <= 24; shift += 8)
            out.push_back(std::uint8_t(v >> shift));
    }

    void i32be(std::int32_t v) { u32be(static_cast<std::uint32_t>(v)); }
    void i32le(std::int32_t v) { u32le(static_cast<std::uint32_t>(v)); }
    void f32be(float v) { u32be(floatToBits(v)); }
    void f32le(float v) { u32le(floatToBits(v)); }
    void tag(std::string_view t) { out.insert(out.end(), t.begin(), t.begin() + 4); }
    void zeros(std::size_t count) { out.insert(out.end(), count, 0); }
    void bytes(std::span<const std::uint8_t> data) { out.insert(out.end(), data.begin(), data.end()); }

    void cString(const std::string& text, std::size_t width)
    {
        const auto length = std::min(text.size(), width - 1); // always NUL terminated
        out.insert(out.end(), text.begin(), text.begin() + static_cast<std::ptrdiff_t>(length));
        zeros(width - length);
    }

    void patchU32beAt(std::size_t offset, std::uint32_t value)
    {
        for (int index = 0; index < 4; ++index)
            out[offset + static_cast<std::size_t>(index)] = std::uint8_t(value >> (24 - 8 * index));
    }

    std::size_t size() const noexcept { return out.size(); }
    std::vector<std::uint8_t> take() { return std::move(out); }

private:
    static std::uint32_t floatToBits(float v) noexcept
    {
        std::uint32_t bits;
        std::memcpy(&bits, &v, sizeof bits);
        return bits;
    }

    std::vector<std::uint8_t> out;
};

// ---------------------------------------------------------------- chunk
struct ChunkHeader
{
    std::int32_t version = 0;
    std::int32_t programs = 0;
    std::int32_t fields = 0;
    std::int32_t currentProgram = 0;
};

std::optional<ChunkHeader> peekHeader(std::span<const std::uint8_t> chunk) noexcept
{
    if (!looksLikeChunk(chunk))
        return std::nullopt;
    Reader in(chunk);
    in.skip(4);
    ChunkHeader header;
    header.version = in.i32le();
    header.programs = in.i32le();
    header.fields = in.i32le();
    header.currentProgram = in.i32le();
    return header;
}

bool plausible(const ChunkHeader& header, std::size_t chunkSize) noexcept
{
    return header.programs > 0 && header.programs <= maximumPrograms && header.fields >= fieldCount
        && header.fields <= maximumFields
        && chunkHeaderBytes + static_cast<std::size_t>(header.programs) * recordBytes(header.fields) <= chunkSize;
}

bool isZlibHeader(std::span<const std::uint8_t> bytes) noexcept
{
    return bytes.size() >= 2 && (bytes[0] & 0x0f) == 8 && ((bytes[0] << 8) | bytes[1]) % 31 == 0;
}

// Yields the uncompressed chunk, inflating a 3.045-style body when needed.
bool expandChunk(std::span<const std::uint8_t> chunk, const Zlib& zlib, std::vector<std::uint8_t>& storage,
                 std::span<const std::uint8_t>& plain, bool& compressed, std::string& error)
{
    const auto header = peekHeader(chunk);
    if (!header)
    {
        error = "chunk does not start with the the reference synth signature";
        return false;
    }

    compressed = false;
    plain = chunk;
    if (plausible(*header, chunk.size()))
        return true;

    const auto body = chunk.subspan(8);
    if (!isZlibHeader(body))
    {
        error = "The reference synth chunk header is inconsistent (" + std::to_string(header->programs) + " programs of "
              + std::to_string(header->fields) + " fields in " + std::to_string(chunk.size()) + " bytes)";
        return false;
    }
    if (!zlib.inflate)
    {
        error = "this the reference synth " + std::to_string(header->version) + " file is compressed and no zlib decoder is available";
        return false;
    }
    if (!zlib.inflate(body, storage, maximumFileBytes))
    {
        error = "compressed the reference synth chunk is corrupt or larger than 64 MB";
        return false;
    }

    compressed = true;
    plain = storage;
    const auto inner = peekHeader(plain);
    if (!inner || !plausible(*inner, plain.size()))
    {
        error = "decompressed the reference synth chunk is inconsistent";
        return false;
    }
    return true;
}

bool parseChunk(std::span<const std::uint8_t> chunk, const Zlib& zlib, Document& document, ReadResult& result)
{
    std::vector<std::uint8_t> storage;
    std::span<const std::uint8_t> plain;
    bool compressed = false;
    if (!expandChunk(chunk, zlib, storage, plain, compressed, result.error))
        return false;

    const auto header = *peekHeader(plain);
    auto& info = document.info;
    info.chunkVersion = header.version;
    info.fieldsPerProgram = header.fields;
    info.chunkCurrentProgram = header.currentProgram;
    info.compressed = compressed;

    Reader in(plain);
    in.skip(chunkHeaderBytes);
    bool reportedNonFinite = false;
    document.patches.resize(static_cast<std::size_t>(header.programs));
    for (auto& patch : document.patches)
    {
        for (auto& value : patch.values)
            value = in.f32le();
        patch.extraFields.resize(static_cast<std::size_t>(header.fields - fieldCount));
        for (auto& value : patch.extraFields)
            value = in.f32le();
        const auto nameBytes = in.take(nameBytesInChunk);
        patch.rawName.emplace();
        std::memcpy(patch.rawName->data(), nameBytes.data(), nameBytesInChunk);
        patch.name = terminated(*patch.rawName);

        const auto finite = std::all_of(patch.values.begin(), patch.values.end(), [](float v) { return std::isfinite(v); });
        if (!finite && !reportedNonFinite)
        {
            result.warnings.push_back("program '" + patch.name + "' contains non-finite values");
            reportedNonFinite = true;
        }
    }

    const auto tail = in.take(in.remaining());
    info.chunkTail.assign(tail.begin(), tail.end());
    if (!tail.empty())
        result.warnings.push_back(std::to_string(tail.size()) + " unrecognised bytes after the last program were preserved");
    return true;
}

std::vector<std::uint8_t> chunkBytes(const std::vector<Patch>& patches, const ContainerInfo& info, const Zlib& zlib)
{
    const auto fields = std::max<std::int32_t>(info.fieldsPerProgram, fieldCount);
    const auto init = initPatch();
    Writer plain;
    plain.tag("1lys");
    plain.i32le(info.chunkVersion);
    plain.i32le(static_cast<std::int32_t>(patches.size()));
    plain.i32le(fields);
    plain.i32le(info.chunkCurrentProgram);
    for (const auto& patch : patches)
    {
        for (const auto value : patch.values)
            plain.f32le(value);
        for (std::size_t extra = 0; extra < static_cast<std::size_t>(fields - fieldCount); ++extra)
        {
            const auto& source = extra < patch.extraFields.size() ? patch.extraFields : init.extraFields;
            plain.f32le(extra < source.size() ? source[extra] : 0.0f);
        }
        const auto& raw = patch.rawName;
        if (raw && terminated(*raw) == patch.name)
            plain.bytes({ reinterpret_cast<const std::uint8_t*>(raw->data()), raw->size() });
        else
            plain.cString(patch.name, nameBytesInChunk);
    }
    plain.bytes(info.chunkTail);

    if (!info.compressed)
        return plain.take();

    const auto inner = plain.take();
    Writer out;
    out.tag("1lys");
    out.i32le(info.chunkVersion);
    out.bytes(zlib.deflate ? zlib.deflate(inner) : storedZlib(inner));
    return out.take();
}

// ---------------------------------------------------------------- outer containers
void beginContainer(Writer& out, std::string_view magic, const ContainerInfo& info, std::int32_t count)
{
    out.tag("CcnK");
    out.u32be(0); // byteSize, patched by finishContainer
    out.tag(magic);
    out.i32be(info.fxpVersion);
    out.tag("syl1");
    out.i32be(info.fxVersion);
    out.i32be(count);
}

void finishContainer(Writer& out, std::size_t start)
{
    out.patchU32beAt(start + 4, static_cast<std::uint32_t>(out.size() - start - 8));
}

void writeFuture(Writer& out, const ContainerInfo& info)
{
    if (info.future.size() == futureBytes)
    {
        out.bytes(info.future);
        return;
    }
    out.i32be(info.fxpVersion >= 2 ? info.outerCurrentProgram : 0);
    out.zeros(futureBytes - 4);
}

void writeParameterProgram(Writer& out, const Patch& patch, const ContainerInfo& info)
{
    const auto start = out.size();
    beginContainer(out, "FxCk", info, fieldCount + static_cast<std::int32_t>(patch.extraFields.size()));
    out.cString(patch.name, nameBytesInFxHeader);
    for (const auto value : patch.values)
        out.f32be(value);
    for (const auto value : patch.extraFields)
        out.f32be(value);
    finishContainer(out, start);
}

bool parseParameterProgram(Reader& in, Patch& patch, ReadResult& result, std::size_t index)
{
    const auto label = "parameter program " + std::to_string(index + 1);
    if (!in.has(outerHeaderBytes + nameBytesInFxHeader))
    {
        result.error = label + " is truncated";
        return false;
    }

    const auto chunkMagic = in.tag();
    in.skip(4); // byteSize
    const auto magic = in.tag();
    in.skip(4); // version
    const auto id = in.tag();
    in.skip(4); // fxVersion
    const auto paramCount = in.i32be();
    if (chunkMagic != "CcnK" || magic != "FxCk" || id != "syl1")
    {
        result.error = label + " is not a the reference synth FxCk record";
        return false;
    }
    if (paramCount < fieldCount || paramCount > maximumFields
        || !in.has(nameBytesInFxHeader + static_cast<std::size_t>(paramCount) * 4))
    {
        result.error = label + " has " + std::to_string(paramCount) + " parameters; the reference synth needs at least "
                     + std::to_string(fieldCount);
        return false;
    }

    patch.name = in.cString(nameBytesInFxHeader);
    for (auto& value : patch.values)
        value = in.f32be();
    patch.extraFields.resize(static_cast<std::size_t>(paramCount - fieldCount));
    for (auto& value : patch.extraFields)
        value = in.f32be();
    return true;
}

std::uint32_t adler32(std::span<const std::uint8_t> data) noexcept
{
    std::uint32_t a = 1;
    std::uint32_t b = 0;
    for (const auto byte : data)
    {
        a = (a + byte) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}
} // namespace

std::string_view fieldName(Field field) noexcept
{
    const auto index = static_cast<std::size_t>(field);
    return index < fieldNames.size() ? fieldNames[index] : std::string_view {};
}

std::string_view fieldKind(Field field) noexcept
{
    const auto index = static_cast<std::size_t>(field);
    return index < fieldKinds.size() ? fieldKinds[index] : std::string_view {};
}

std::optional<Field> fieldFromName(std::string_view name) noexcept
{
    for (std::size_t index = 0; index < fieldNames.size(); ++index)
        if (fieldNames[index] == name)
            return static_cast<Field>(index);
    return std::nullopt;
}

Patch initPatch()
{
    Patch patch;
    patch.name = "Init";
    patch.values = initValues;
    patch.extraFields = { 0.0f, 0.0f }; // ZEQ 1-pole; the 3.045 addition is 0 in every observed program
    return patch;
}

bool looksLikeChunk(std::span<const std::uint8_t> bytes) noexcept
{
    return bytes.size() >= chunkHeaderBytes && std::memcmp(bytes.data(), "1lys", 4) == 0;
}

ReadResult readChunk(std::span<const std::uint8_t> chunk, const Zlib& zlib)
{
    ReadResult result;
    Document document;
    if (!parseChunk(chunk, zlib, document, result))
        return result;
    document.info.kind = document.patches.size() == 1 ? ContainerKind::ChunkProgram : ContainerKind::ChunkBank;
    result.document = std::move(document);
    return result;
}

ReadResult read(std::span<const std::uint8_t> bytes, const Zlib& zlib)
{
    ReadResult result;
    Reader in(bytes);
    if (!in.has(outerHeaderBytes))
    {
        result.error = "file is too small to be an .fxp or .fxb";
        return result;
    }
    if (in.tag() != "CcnK")
    {
        result.error = "not an .fxp/.fxb file (missing CcnK signature)";
        return result;
    }

    const auto byteSize = in.u32be();
    const auto magic = std::string(in.tag());
    Document document;
    document.info.fxpVersion = in.i32be();
    const auto pluginId = std::string(in.tag());
    document.info.fxVersion = in.i32be();
    const auto count = in.i32be();

    if (pluginId != "syl1")
    {
        result.error = "file belongs to another plugin (id '" + pluginId + "'), not the reference synth";
        return result;
    }
    if (static_cast<std::uint64_t>(byteSize) + 8u != bytes.size())
        result.warnings.push_back("container byte size field disagrees with the file length");

    if (magic == "FPCh" || magic == "FBCh")
    {
        const auto isBank = magic == "FBCh";
        std::string headerName;
        if (!in.has((isBank ? futureBytes : nameBytesInFxHeader) + 4))
        {
            result.error = std::string(isBank ? "bank" : "program") + " header is truncated";
            return result;
        }
        if (isBank)
        {
            const auto reserved = in.take(futureBytes);
            document.info.future.assign(reserved.begin(), reserved.end());
            if (document.info.fxpVersion >= 2)
                document.info.outerCurrentProgram = Reader(reserved).i32be();
        }
        else
        {
            headerName = in.cString(nameBytesInFxHeader);
        }

        const auto chunkSize = in.u32be();
        if (chunkSize > in.remaining())
        {
            result.error = "chunk size (" + std::to_string(chunkSize) + ") exceeds the file";
            return result;
        }
        if (!parseChunk(in.take(chunkSize), zlib, document, result))
            return result;

        document.info.kind = isBank ? ContainerKind::ChunkBank : ContainerKind::ChunkProgram;
        if (!isBank && document.patches.size() != 1)
        {
            result.error = "program file holds " + std::to_string(document.patches.size()) + " programs";
            return result;
        }
        if (!isBank && document.patches.front().name.empty())
            document.patches.front().name = headerName;
        if (isBank && count != static_cast<std::int32_t>(document.patches.size()))
            result.warnings.push_back("bank header program count (" + std::to_string(count)
                                      + ") differs from the chunk (" + std::to_string(document.patches.size()) + ")");
    }
    else if (magic == "FxCk")
    {
        document.info.kind = ContainerKind::ParameterProgram;
        Patch patch;
        Reader again(bytes);
        if (!parseParameterProgram(again, patch, result, 0))
            return result;
        document.patches.push_back(std::move(patch));
    }
    else if (magic == "FxBk")
    {
        document.info.kind = ContainerKind::ParameterBank;
        if (count <= 0 || count > maximumPrograms || !in.has(futureBytes))
        {
            result.error = "parameter bank header is invalid";
            return result;
        }
        const auto reserved = in.take(futureBytes);
        document.info.future.assign(reserved.begin(), reserved.end());
        if (document.info.fxpVersion >= 2)
            document.info.outerCurrentProgram = Reader(reserved).i32be();
        document.patches.resize(static_cast<std::size_t>(count));
        for (std::size_t index = 0; index < document.patches.size(); ++index)
            if (!parseParameterProgram(in, document.patches[index], result, index))
                return result;
    }
    else
    {
        result.error = "unsupported container type '" + magic + "'";
        return result;
    }

    result.document = std::move(document);
    return result;
}

ReadResult readFile(const std::string& utf8Path, const Zlib& zlib)
{
    ReadResult result;
    const auto path = pathFromUtf8(utf8Path);
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec)
    {
        result.error = "cannot read " + utf8Path + ": " + ec.message();
        return result;
    }
    if (size > maximumFileBytes)
    {
        result.error = "file is larger than 64 MB";
        return result;
    }

    std::ifstream stream(path, std::ios::binary);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (!stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
    {
        result.error = "cannot read " + utf8Path;
        return result;
    }
    return read(bytes, zlib);
}

std::vector<std::uint8_t> writeBank(const std::vector<Patch>& patches, const ContainerInfo& info, const Zlib& zlib)
{
    Writer out;
    beginContainer(out, "FBCh", info, static_cast<std::int32_t>(patches.size()));
    writeFuture(out, info);
    const auto chunk = chunkBytes(patches, info, zlib);
    out.u32be(static_cast<std::uint32_t>(chunk.size()));
    out.bytes(chunk);
    finishContainer(out, 0);
    return out.take();
}

std::vector<std::uint8_t> writeProgram(const Patch& patch, const ContainerInfo& info, const Zlib& zlib)
{
    Writer out;
    beginContainer(out, "FPCh", info, std::max<std::int32_t>(info.fieldsPerProgram, fieldCount));
    out.cString(patch.name, nameBytesInFxHeader);
    const auto chunk = chunkBytes({ patch }, info, zlib);
    out.u32be(static_cast<std::uint32_t>(chunk.size()));
    out.bytes(chunk);
    finishContainer(out, 0);
    return out.take();
}

std::vector<std::uint8_t> write(const Document& document, const Zlib& zlib)
{
    switch (document.info.kind)
    {
        case ContainerKind::ChunkBank:
            return writeBank(document.patches, document.info, zlib);
        case ContainerKind::ChunkProgram:
            return document.patches.empty() ? std::vector<std::uint8_t> {}
                                            : writeProgram(document.patches.front(), document.info, zlib);
        case ContainerKind::ParameterProgram: {
            Writer out;
            if (!document.patches.empty())
                writeParameterProgram(out, document.patches.front(), document.info);
            return out.take();
        }
        case ContainerKind::ParameterBank: {
            Writer out;
            beginContainer(out, "FxBk", document.info, static_cast<std::int32_t>(document.patches.size()));
            writeFuture(out, document.info);
            for (const auto& patch : document.patches)
                writeParameterProgram(out, patch, document.info);
            finishContainer(out, 0);
            return out.take();
        }
    }
    return {};
}

void padWithInit(std::vector<Patch>& patches, std::size_t count)
{
    if (patches.size() < count)
        patches.resize(count, initPatch());
}

ContainerInfo infoForSources(const std::vector<ContainerInfo>& sources, ContainerKind kind)
{
    ContainerInfo info;
    info.kind = kind;
    if (sources.empty())
        return info; // 3045 compressed with 244 fields: what the reference synth 3.x writes
    info.chunkVersion = 0;
    info.fieldsPerProgram = fieldCount;
    info.compressed = false;
    for (const auto& source : sources)
    {
        info.chunkVersion = std::max(info.chunkVersion, source.chunkVersion);
        info.fieldsPerProgram = std::max(info.fieldsPerProgram, source.fieldsPerProgram);
        info.compressed = info.compressed || source.compressed;
        info.fxpVersion = source.fxpVersion;
        info.fxVersion = source.fxVersion;
    }
    return info;
}

bool writeFile(const std::string& utf8Path, std::span<const std::uint8_t> bytes, std::string& error)
{
    const auto path = pathFromUtf8(utf8Path);
    auto temporary = path;
    temporary += ".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
        {
            error = "cannot write " + utf8Path;
            return false;
        }
    }

    std::error_code ec;
    std::filesystem::rename(temporary, path, ec);
    if (ec)
    {
        std::filesystem::remove(temporary, ec);
        error = "cannot replace " + utf8Path + ": " + ec.message();
        return false;
    }
    return true;
}

std::vector<std::uint8_t> storedZlib(std::span<const std::uint8_t> data)
{
    std::vector<std::uint8_t> out { 0x78, 0x01 };
    std::size_t offset = 0;
    do
    {
        const auto length = std::min<std::size_t>(data.size() - offset, 65535u);
        const auto last = offset + length == data.size();
        out.push_back(last ? 1 : 0);
        out.push_back(std::uint8_t(length));
        out.push_back(std::uint8_t(length >> 8));
        out.push_back(std::uint8_t(~length));
        out.push_back(std::uint8_t(~length >> 8));
        out.insert(out.end(), data.begin() + static_cast<std::ptrdiff_t>(offset),
                   data.begin() + static_cast<std::ptrdiff_t>(offset + length));
        offset += length;
    } while (offset < data.size());

    const auto checksum = adler32(data);
    for (int shift = 24; shift >= 0; shift -= 8)
        out.push_back(std::uint8_t(checksum >> shift));
    return out;
}
} // namespace synth::legacy
