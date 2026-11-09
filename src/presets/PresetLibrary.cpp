#include "PresetLibrary.h"

#include "PresetManager.h"
#include "../legacy/LegacyImport.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstring>
#include <mutex>
#include <system_error>
#include <thread>
#include <unordered_map>

namespace synth
{
namespace
{
std::string lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::string utf8(const std::filesystem::path& path)
{
    const auto text = path.generic_u8string();
    return std::string(text.begin(), text.end());
}

std::filesystem::path pathFromUtf8(const std::string& text)
{
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

juce::var toVar(const std::vector<std::string>& values)
{
    juce::Array<juce::var> array;
    for (const auto& value : values)
        array.add(juce::String::fromUTF8(value.c_str()));
    return array;
}

std::vector<std::string> toStrings(const juce::var& value)
{
    std::vector<std::string> out;
    if (const auto* array = value.getArray())
        for (const auto& item : *array)
            out.push_back(item.toString().toStdString());
    return out;
}

std::string text(const juce::var& value)
{
    return value.toString().toStdString();
}

bool hasExtension(const std::filesystem::path& path, const char* extension)
{
    return lower(utf8(path.extension())) == extension;
}

std::string cleanBankName(std::string name)
{
    std::replace(name.begin(), name.end(), '_', ' ');
    return name;
}

std::uint64_t fnv(const void* bytes, std::size_t count, std::uint64_t seed = 1469598103934665603ull)
{
    auto hash = seed;
    const auto* p = static_cast<const unsigned char*>(bytes);
    for (std::size_t i = 0; i < count; ++i)
        hash = (hash ^ p[i]) * 1099511628211ull;
    return hash == 0 ? 1 : hash;
}

// What one file holds, before it is placed in the catalogue.
struct RawEntry
{
    std::string name;
    int programIndex = 0;
    std::string category, vendor, author, description, tags; // tags joined by '\n'
    std::uint64_t hash = 0;
};

struct RawFile
{
    LibraryKind kind = LibraryKind::Synthax1Preset;
    std::string bank;
    std::vector<RawEntry> entries;
};

RawFile readSynthax1Preset(const std::filesystem::path& path)
{
    RawFile result;
    const auto parsed = juce::JSON::parse(juce::File(juce::String::fromUTF8(utf8(path).c_str())));
    const auto* object = parsed.getDynamicObject();
    if (object == nullptr)
        return result;
    RawEntry entry;
    entry.name = text(object->getProperty("presetName"));
    if (entry.name.empty())
        if (const auto* preset = object->getProperty("preset").getDynamicObject())
            entry.name = text(preset->getProperty("display_name"));
    if (entry.name.empty())
        entry.name = utf8(path.stem());
    entry.author = text(object->getProperty("presetAuthor"));
    entry.description = text(object->getProperty("presetDescription"));
    entry.category = text(object->getProperty("category"));
    result.bank = text(object->getProperty("bank"));
    entry.vendor = text(object->getProperty("vendor"));
    for (const auto& tag : toStrings(object->getProperty("tags")))
        entry.tags += (entry.tags.empty() ? "" : "\n") + tag;
    if (const auto* preset = object->getProperty("preset").getDynamicObject())
    {
        const auto parameters = juce::JSON::toString(preset->getProperty("parameters"), true).toStdString();
        if (parameters.size() > 4)
            entry.hash = fnv(parameters.data(), parameters.size());
    }
    result.entries.push_back(std::move(entry));
    return result;
}

RawFile readLegacyFile(const std::filesystem::path& path)
{
    RawFile result;
    result.kind = LibraryKind::LegacyProgram;
    const auto file = legacy::readFile(utf8(path), legacy::juceZlib());
    if (!file.ok())
        return result;
    result.bank = cleanBankName(utf8(path.stem()));
    const auto& patches = file.document->patches;
    result.entries.reserve(patches.size());
    for (std::size_t index = 0; index < patches.size(); ++index)
    {
        // Many banks pad or centre names with spaces; the list shows them trimmed.
        auto name = patches[index].name;
        const auto first = name.find_first_not_of(" 	");
        name = first == std::string::npos ? std::string() : name.substr(first, name.find_last_not_of(" 	") - first + 1);
        const auto named = std::any_of(name.begin(), name.end(), [](unsigned char c) { return std::isalnum(c) != 0; });
        if (patches.size() > 1 && (!named || name == "Init"))
            continue; // the filler that pads a bank to 512 slots, and the blank or ---- separator programs
        RawEntry entry;
        entry.programIndex = static_cast<int>(index);
        entry.name = name.empty() ? result.bank : name;
        entry.category = categoryFromPresetName(entry.name);
        entry.hash = fnv(patches[index].values.data(), patches[index].values.size() * sizeof(float));
        if (!patches[index].extraFields.empty())
            entry.hash = fnv(patches[index].extraFields.data(), patches[index].extraFields.size() * sizeof(float), entry.hash);
        result.entries.push_back(std::move(entry));
    }
    return result;
}

// ---- binary cache ------------------------------------------------------------------------------------------------

struct ByteWriter
{
    std::string data;
    void u8(std::uint8_t value) { data.push_back(static_cast<char>(value)); }
    void u32(std::uint32_t value) { data.append(reinterpret_cast<const char*>(&value), 4); }
    void i64(std::int64_t value) { data.append(reinterpret_cast<const char*>(&value), 8); }
    void str(const std::string& value)
    {
        u32(static_cast<std::uint32_t>(value.size()));
        data.append(value);
    }
};

struct ByteReader
{
    const char* cursor = nullptr;
    const char* end = nullptr;
    bool ok = true;
    bool need(std::size_t count)
    {
        if (static_cast<std::size_t>(end - cursor) < count)
            ok = false;
        return ok;
    }
    std::uint8_t u8() { if (!need(1)) return 0; return static_cast<std::uint8_t>(*cursor++); }
    std::uint32_t u32() { std::uint32_t v = 0; if (!need(4)) return 0; std::memcpy(&v, cursor, 4); cursor += 4; return v; }
    std::int64_t i64() { std::int64_t v = 0; if (!need(8)) return 0; std::memcpy(&v, cursor, 8); cursor += 8; return v; }
    std::string str()
    {
        const auto size = u32();
        if (!need(size))
            return {};
        std::string value(cursor, size);
        cursor += size;
        return value;
    }
};

constexpr std::uint32_t cacheMagic = 0x32434c53; // "SLC2"


struct CacheFile
{
    std::int64_t size = 0;
    std::int64_t modified = 0;
    LibraryKind kind = LibraryKind::Synthax1Preset;
    std::string bank;
    std::uint32_t count = 0;
    std::size_t offset = 0; // of the entries in the blob
    std::size_t length = 0;
};

struct CacheIndex
{
    std::string blob;
    std::unordered_map<std::string, CacheFile> files; // by path
    bool valid = false;
};

CacheIndex readCache(const std::filesystem::path& cachePath)
{
    CacheIndex index;
    if (cachePath.empty())
        return index;
    const juce::File file(juce::String::fromUTF8(utf8(cachePath).c_str()));
    juce::MemoryBlock block;
    if (!file.existsAsFile() || !file.loadFileAsData(block) || block.getSize() < 8)
        return index;
    index.blob.assign(static_cast<const char*>(block.getData()), block.getSize());
    ByteReader reader { index.blob.data(), index.blob.data() + index.blob.size() };
    if (reader.u32() != cacheMagic)
        return index;
    const auto count = reader.u32();
    for (std::uint32_t i = 0; i < count && reader.ok; ++i)
    {
        const auto path = reader.str();
        CacheFile entry;
        entry.size = reader.i64();
        entry.modified = reader.i64();
        entry.kind = reader.u8() == 0 ? LibraryKind::Synthax1Preset : LibraryKind::LegacyProgram;
        entry.bank = reader.str();
        entry.count = reader.u32();
        entry.length = reader.u32();
        entry.offset = static_cast<std::size_t>(reader.cursor - index.blob.data());
        if (!reader.need(entry.length))
            break;
        reader.cursor += entry.length;
        index.files.emplace(path, std::move(entry));
    }
    index.valid = reader.ok;
    return index;
}

RawFile decodeCacheFile(const CacheIndex& index, const CacheFile& ref)
{
    RawFile result;
    result.kind = ref.kind;
    result.bank = ref.bank;
    result.entries.reserve(ref.count);
    ByteReader reader { index.blob.data() + ref.offset, index.blob.data() + ref.offset + ref.length };
    for (std::uint32_t i = 0; i < ref.count && reader.ok; ++i)
    {
        RawEntry entry;
        entry.name = reader.str();
        entry.programIndex = static_cast<int>(reader.u32());
        entry.category = reader.str();
        entry.hash = static_cast<std::uint64_t>(reader.i64());
        if (ref.kind == LibraryKind::Synthax1Preset)
        {
            entry.vendor = reader.str();
            entry.author = reader.str();
            entry.description = reader.str();
            entry.tags = reader.str();
        }
        result.entries.push_back(std::move(entry));
    }
    return result;
}

bool isUnderRoot(const std::filesystem::path& path, const std::filesystem::path& root)
{
    const auto relative = path.lexically_relative(root);
    return !relative.empty() && *relative.begin() != "..";
}

// Builds a catalogue file by file, sharing strings.
class Builder
{
public:
    struct FileInfo
    {
        std::int64_t size = 0;
        std::int64_t modified = 0;
        std::size_t firstEntry = 0;
        std::string derivedVendor;
    };

    void add(const std::filesystem::path& path, std::int64_t size, std::int64_t modified, const LibraryRoot& root, RawFile&& file)
    {
        const auto relative = path.parent_path().lexically_relative(root.path);
        const std::string folder = relative.empty() || relative == "." ? std::string() : utf8(relative);
        const auto prefix = root.label + (folder.empty() ? std::string() : "/" + folder);
        const auto derivedVendor = folder.empty() ? root.label : folder.substr(0, folder.find('/'));

        const auto fileIndex = static_cast<std::uint32_t>(data.files.size());
        data.files.push_back(path);
        infos.push_back({ size, modified, data.entries.size(), derivedVendor });

        const auto loose = file.kind == LibraryKind::LegacyProgram && hasExtension(path, ".fxp");
        const auto folderName = folder.empty() ? root.label : utf8(std::filesystem::path(std::u8string(folder.begin(), folder.end())).filename());
        // Single-program files are not a bank: they are grouped by the folder they sit in.
        const auto bankText = loose ? folderName : (!file.bank.empty() ? file.bank : (folder.empty() ? root.label : folder));
        const auto bank = intern(bankText);
        const auto location = intern(file.kind == LibraryKind::Synthax1Preset || loose ? prefix : prefix + "/" + bankText);
        const auto derived = intern(derivedVendor);
        for (auto& raw : file.entries)
        {
            LibraryEntry entry;
            entry.name = std::move(raw.name);
            entry.modified = modified;
            entry.file = fileIndex;
            entry.programIndex = raw.programIndex;
            entry.kind = file.kind;
            entry.loose = loose;
            entry.hash = raw.hash;
            entry.category = intern(raw.category);
            entry.bank = bank;
            entry.location = location;
            entry.vendor = raw.vendor.empty() ? derived : intern(raw.vendor);
            entry.author = intern(raw.author);
            entry.description = intern(raw.description);
            entry.tags = intern(raw.tags);
            data.entries.push_back(std::move(entry));
        }
    }

    std::uint32_t intern(const std::string& value)
    {
        if (value.empty())
            return 0;
        const auto found = lookup.find(value);
        if (found != lookup.end())
            return found->second;
        const auto index = static_cast<std::uint32_t>(data.strings.size());
        data.strings.push_back(value);
        lookup.emplace(value, index);
        return index;
    }

    // A finished copy with the search text prepared.
    std::shared_ptr<const LibraryData> snapshot() const
    {
        auto copy = std::make_shared<LibraryData>(data);
        prepareSearchText(*copy);
        return copy;
    }

    std::shared_ptr<const LibraryData> finish()
    {
        prepareSearchText(data);
        return std::make_shared<LibraryData>(std::move(data));
    }

    void writeCache(const std::filesystem::path& cachePath, const LibraryData& finished) const
    {
        if (cachePath.empty())
            return;
        ByteWriter out;
        out.u32(cacheMagic);
        out.u32(static_cast<std::uint32_t>(finished.files.size()));
        for (std::size_t file = 0; file < finished.files.size(); ++file)
        {
            const auto first = infos[file].firstEntry;
            const auto last = file + 1 < finished.files.size() ? infos[file + 1].firstEntry : finished.entries.size();
            const auto kind = last > first ? finished.entries[first].kind : (hasExtension(finished.files[file], ".synthax1preset") ? LibraryKind::Synthax1Preset : LibraryKind::LegacyProgram);
            ByteWriter body;
            for (auto i = first; i < last; ++i)
            {
                const auto& entry = finished.entries[i];
                body.str(entry.name);
                body.u32(static_cast<std::uint32_t>(entry.programIndex));
                body.str(finished.text(entry.category));
                body.i64(static_cast<std::int64_t>(entry.hash));
                if (kind == LibraryKind::Synthax1Preset)
                {
                    body.str(finished.text(entry.vendor) == infos[file].derivedVendor ? std::string() : finished.text(entry.vendor));
                    body.str(finished.text(entry.author));
                    body.str(finished.text(entry.description));
                    body.str(finished.text(entry.tags));
                }
            }
            out.str(utf8(finished.files[file]));
            out.i64(infos[file].size);
            out.i64(infos[file].modified);
            out.u8(kind == LibraryKind::Synthax1Preset ? 0 : 1);
            out.str(last > first ? finished.text(finished.entries[first].bank) : std::string());
            out.u32(static_cast<std::uint32_t>(last - first));
            out.u32(static_cast<std::uint32_t>(body.data.size()));
            out.data.append(body.data);
        }
        std::error_code ignored;
        std::filesystem::create_directories(cachePath.parent_path(), ignored);
        const juce::File target(juce::String::fromUTF8(utf8(cachePath).c_str()));
        target.replaceWithData(out.data.data(), out.data.size());
    }

private:
    static void prepareSearchText(LibraryData& target)
    {
        target.searchText.clear();
        target.searchText.reserve(target.entries.size());
        for (const auto& entry : target.entries)
        {
            std::string blob = lower(entry.name);
            blob.push_back('\x01'); // sorts before every character, so blobs sort by name first
            for (const auto field : { entry.category, entry.bank, entry.vendor, entry.author, entry.description, entry.tags })
                if (field != 0)
                {
                    blob.append(lower(target.strings[field]));
                    blob.push_back(' ');
                }
            target.searchText.push_back(std::move(blob));
        }
        // Identical sounds: the first entry of each kind stands for the rest.
        target.canonical.assign(target.entries.size(), 0);
        target.copies.assign(target.entries.size(), 0);
        std::unordered_map<std::uint64_t, std::int32_t> firstOfKind;
        for (std::size_t i = 0; i < target.entries.size(); ++i)
        {
            auto self = static_cast<std::int32_t>(i);
            if (const auto hash = target.entries[i].hash; hash != 0)
            {
                const auto inserted = firstOfKind.emplace(hash, self);
                if (!inserted.second)
                {
                    self = inserted.first->second;
                    ++target.copies[static_cast<std::size_t>(self)];
                }
            }
            target.canonical[i] = self;
        }
    }

    LibraryData data;
    std::vector<FileInfo> infos;
    std::unordered_map<std::string, std::uint32_t> lookup;
};
} // namespace

std::string categoryFromPresetName(const std::string& name)
{
    std::string token;
    for (const auto c : name)
    {
        if (c == ' ' || c == '-' || c == '_' || c == '.' || c == ':')
        {
            if (!token.empty())
                break;
            continue;
        }
        token.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    static const std::map<std::string, std::string> codes {
        { "ARP", "Arp" }, { "BA", "Bass" }, { "BS", "Bass" }, { "BASS", "Bass" }, { "LD", "Lead" }, { "LEAD", "Lead" },
        { "PD", "Pad" }, { "PAD", "Pad" }, { "PL", "Pluck" }, { "PLK", "Pluck" }, { "PLUCK", "Pluck" },
        { "SY", "Synth" }, { "SYN", "Synth" }, { "SYNTH", "Synth" }, { "KY", "Keys" }, { "KEY", "Keys" }, { "KEYS", "Keys" },
        { "FX", "FX" }, { "SFX", "FX" }, { "DR", "Drum" }, { "DRM", "Drum" }, { "DRUM", "Drum" }, { "SQ", "Seq" },
        { "SEQ", "Seq" }, { "BL", "Bell" }, { "BELL", "Bell" }, { "BR", "Brass" }, { "BRASS", "Brass" },
        { "OR", "Organ" }, { "ORG", "Organ" }, { "ORGAN", "Organ" }, { "PN", "Piano" }, { "PIANO", "Piano" },
        { "ST", "Strings" }, { "STR", "Strings" }, { "STRINGS", "Strings" }, { "CH", "Chord" }, { "CHORD", "Chord" },
        { "VOX", "Vocal" }, { "AMB", "Ambient" }, { "ATM", "Ambient" }, { "GT", "Guitar" }, { "GTR", "Guitar" },
        { "WUB", "Bass" }, { "ACID", "Bass" }, { "STAB", "Chord" }, { "HOOVER", "Hoover" }
    };
    const auto found = codes.find(token);
    return found == codes.end() ? std::string() : found->second;
}


std::string LibraryData::idOf(const LibraryEntry& entry) const
{
    return utf8(files[entry.file]) + "#" + std::to_string(entry.programIndex);
}

int LibraryData::find(const std::string& id) const
{
    const auto hash = id.rfind('#');
    if (hash == std::string::npos)
        return -1;
    const auto path = id.substr(0, hash);
    const auto program = std::atoi(id.c_str() + hash + 1);
    for (std::size_t file = 0; file < files.size(); ++file)
    {
        if (utf8(files[file]) != path)
            continue;
        // Entries are stored file by file in file order.
        const auto first = std::lower_bound(entries.begin(), entries.end(), static_cast<std::uint32_t>(file),
                                            [](const LibraryEntry& entry, std::uint32_t value) { return entry.file < value; });
        for (auto it = first; it != entries.end() && it->file == file; ++it)
            if (it->programIndex == program)
                return static_cast<int>(it - entries.begin());
        return -1;
    }
    return -1;
}

std::vector<std::string> LibraryData::tagsOf(const LibraryEntry& entry) const
{
    std::vector<std::string> out;
    const auto& joined = strings[entry.tags];
    std::size_t start = 0;
    while (start < joined.size())
    {
        auto end = joined.find('\n', start);
        if (end == std::string::npos)
            end = joined.size();
        if (end > start)
            out.push_back(joined.substr(start, end - start));
        start = end + 1;
    }
    return out;
}

std::shared_ptr<const LibraryData> loadCachedLibrary(const std::vector<LibraryRoot>& roots, const std::filesystem::path& cachePath)
{
    const auto cache = readCache(cachePath);
    if (!cache.valid)
        return nullptr;
    Builder builder;
    for (const auto& [pathText, ref] : cache.files)
    {
        const auto path = pathFromUtf8(pathText);
        const LibraryRoot* owner = nullptr;
        for (const auto& root : roots)
            if (isUnderRoot(path, root.path))
                owner = &root;
        if (owner != nullptr)
            builder.add(path, ref.size, ref.modified, *owner, decodeCacheFile(cache, ref));
    }
    return builder.finish();
}

std::shared_ptr<const LibraryData> scanLibrary(const std::vector<LibraryRoot>& roots,
                                               const std::filesystem::path& cachePath,
                                               const std::function<bool()>& cancelled,
                                               const std::function<void(int)>& progress,
                                               const std::function<void(std::shared_ptr<const LibraryData>)>& partial)
{
    struct Job
    {
        std::filesystem::path path;
        std::size_t root = 0;
        std::int64_t size = 0;
        std::int64_t modified = 0;
        bool isPreset = false;
    };

    // 1. Find the files.
    std::vector<Job> jobs;
    for (std::size_t rootIndex = 0; rootIndex < roots.size(); ++rootIndex)
    {
        std::error_code error;
        if (!std::filesystem::is_directory(roots[rootIndex].path, error))
            continue;
        std::filesystem::recursive_directory_iterator iterator(roots[rootIndex].path,
                                                               std::filesystem::directory_options::skip_permission_denied, error);
        for (; !error && iterator != std::filesystem::recursive_directory_iterator(); iterator.increment(error))
        {
            if (cancelled && cancelled())
                return nullptr;
            const auto& path = iterator->path();
            if (utf8(path.filename()).rfind("._", 0) == 0)
                continue; // macOS resource-fork leftovers next to the real files
            const auto isPreset = hasExtension(path, ".synthax1preset");
            if (!isPreset && !hasExtension(path, ".fxb") && !hasExtension(path, ".fxp"))
                continue;
            std::error_code fileError;
            if (!iterator->is_regular_file(fileError))
                continue;
            Job job;
            job.path = path;
            job.root = rootIndex;
            job.size = static_cast<std::int64_t>(iterator->file_size(fileError));
            job.modified = juce::File(juce::String::fromUTF8(utf8(path).c_str())).getLastModificationTime().toMilliseconds();
            job.isPreset = isPreset;
            jobs.push_back(std::move(job));
        }
    }

    // 2. Read them on several threads, merging the results in order as they complete.
    const auto cache = readCache(cachePath);
    Builder builder;
    std::vector<RawFile> results(jobs.size());
    std::vector<char> done(jobs.size(), 0);
    std::size_t merged = 0;
    std::mutex mergeMutex;
    std::atomic<std::size_t> next { 0 };
    std::atomic<bool> stop { false };
    auto lastPartial = std::chrono::steady_clock::now();

    const auto worker = [&] {
        for (;;)
        {
            if (stop.load() || (cancelled && cancelled()))
            {
                stop.store(true);
                return;
            }
            const auto index = next.fetch_add(1);
            if (index >= jobs.size())
                return;
            const auto& job = jobs[index];
            RawFile file;
            const auto cached = cache.valid ? cache.files.find(utf8(job.path)) : cache.files.end();
            if (cached != cache.files.end() && cached->second.size == job.size && cached->second.modified == job.modified)
                file = decodeCacheFile(cache, cached->second);
            else
                file = job.isPreset ? readSynthax1Preset(job.path) : readLegacyFile(job.path);
            if (job.isPreset)
                file.kind = LibraryKind::Synthax1Preset;

            const std::lock_guard<std::mutex> lock(mergeMutex);
            results[index] = std::move(file);
            done[index] = 1;
            while (merged < jobs.size() && done[merged])
            {
                const auto& ready = jobs[merged];
                builder.add(ready.path, ready.size, ready.modified, roots[ready.root], std::move(results[merged]));
                results[merged] = RawFile();
                ++merged;
                if (progress && merged % 25 == 0)
                    progress(static_cast<int>(merged));
            }
            if (partial && std::chrono::steady_clock::now() - lastPartial > std::chrono::seconds(4))
            {
                lastPartial = std::chrono::steady_clock::now();
                partial(builder.snapshot());
            }
        }
    };
    const auto threadCount = std::clamp(static_cast<int>(std::thread::hardware_concurrency()) - 1, 1, 8);
    std::vector<std::thread> threads;
    for (int i = 1; i < threadCount; ++i)
        threads.emplace_back(worker);
    worker();
    for (auto& thread : threads)
        thread.join();
    if (stop.load() || (cancelled && cancelled()))
        return nullptr;

    // 3. Finish and remember it.
    auto finished = builder.finish();
    builder.writeCache(cachePath, *finished);
    return finished;
}

// ---------------------------------------------------------------------------------------------------------------------

LibraryUserData LibraryUserStore::get(const std::string& id) const
{
    const auto found = values.find(id);
    return found == values.end() ? LibraryUserData {} : found->second;
}

void LibraryUserStore::set(const std::string& id, const LibraryUserData& data)
{
    if (!data.favorite && data.rating == 0 && data.notes.empty())
        values.erase(id);
    else
        values[id] = data;
    save();
}

void LibraryUserStore::load()
{
    const auto parsed = juce::JSON::parse(juce::File(juce::String::fromUTF8(utf8(path).c_str())));
    if (const auto* object = parsed.getDynamicObject())
        for (const auto& property : object->getProperties())
            if (const auto* item = property.value.getDynamicObject())
            {
                LibraryUserData data;
                // Files written before the en-US spelling store "favourite".
                data.favorite = static_cast<bool>(item->hasProperty("favorite") ? item->getProperty("favorite") : item->getProperty("favourite"));
                data.rating = std::clamp(static_cast<int>(item->getProperty("rating")), 0, 5);
                data.notes = text(item->getProperty("notes"));
                values[property.name.toString().toStdString()] = data;
            }
}

void LibraryUserStore::save() const
{
    auto* object = new juce::DynamicObject();
    for (const auto& [id, data] : values)
    {
        auto* item = new juce::DynamicObject();
        item->setProperty("favorite", data.favorite);
        item->setProperty("rating", data.rating);
        item->setProperty("notes", juce::String::fromUTF8(data.notes.c_str()));
        object->setProperty(juce::Identifier(juce::String::fromUTF8(id.c_str())), juce::var(item));
    }
    std::error_code ignored;
    std::filesystem::create_directories(path.parent_path(), ignored);
    juce::File(juce::String::fromUTF8(utf8(path).c_str())).replaceWithText(juce::JSON::toString(juce::var(object), true));
}

// ---------------------------------------------------------------------------------------------------------------------

std::filesystem::path libraryDataDirectory()
{
    return defaultUserPresetDirectory().parent_path();
}

LibrarySettings readLibrarySettings()
{
    LibrarySettings settings;
    const auto parsed = juce::JSON::parse(juce::File(juce::String::fromUTF8(utf8(libraryDataDirectory() / "library-settings.json").c_str())));
    if (const auto* object = parsed.getDynamicObject())
    {
        if (const auto library = text(object->getProperty("libraryFolder")); !library.empty())
            settings.libraryFolder = pathFromUtf8(library);
        for (const auto& folder : toStrings(object->getProperty("folders")))
            settings.folders.push_back(pathFromUtf8(folder));
        if (object->hasProperty("hideDuplicates"))
            settings.hideDuplicates = static_cast<bool>(object->getProperty("hideDuplicates"));
        if (object->hasProperty("previewOnSelect"))
            settings.previewOnSelect = static_cast<bool>(object->getProperty("previewOnSelect"));
    }
    return settings;
}

void writeLibrarySettings(const LibrarySettings& settings)
{
    auto* object = new juce::DynamicObject();
    std::vector<std::string> folders;
    for (const auto& folder : settings.folders)
        folders.push_back(utf8(folder));
    object->setProperty("folders", toVar(folders));
    object->setProperty("libraryFolder", juce::String::fromUTF8(utf8(settings.libraryFolder).c_str()));
    object->setProperty("previewOnSelect", settings.previewOnSelect);
    object->setProperty("hideDuplicates", settings.hideDuplicates);
    const auto directory = libraryDataDirectory();
    std::error_code ignored;
    std::filesystem::create_directories(directory, ignored);
    juce::File(juce::String::fromUTF8(utf8(directory / "library-settings.json").c_str())).replaceWithText(juce::JSON::toString(juce::var(object), true));
}

std::vector<LibraryRoot> libraryRoots(const LibrarySettings& settings)
{
    std::vector<LibraryRoot> roots;
    roots.push_back({ factoryPresetDirectory(), "Factory" });
    roots.push_back({ defaultUserPresetDirectory(), "User" });
    if (!settings.libraryFolder.empty())
        roots.push_back({ settings.libraryFolder, "Library" });
    for (const auto& folder : settings.folders)
        roots.push_back({ folder, utf8(folder.filename().empty() ? folder.parent_path().filename() : folder.filename()) });
    return roots;
}
} // namespace synth
