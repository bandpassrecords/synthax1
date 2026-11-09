#pragma once

// A searchable catalogue of every preset the user can reach: Synthax1 presets (.Synthax1Preset) and the reference synth
// banks and programs (.fxb, .fxp) found under a list of folders.
//
// Built for libraries of hundreds of thousands of presets:
//  - entries are compact (names plus indices into shared string tables, one path per file);
//  - a scan reads only names and metadata, in parallel, and is cached in a binary file by file size and
//    modification time, so a rescan only reads what is new or changed;
//  - the cache loads without touching the folders, so the list appears at once;
//  - a lower-case search text is prepared per entry, so searching and sorting never convert strings.
// User data (favorites, ratings, notes) is kept apart from the catalogue and survives rescans.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace synth
{
enum class LibraryKind : std::uint8_t
{
    Synthax1Preset, // one .Synthax1Preset file
    LegacyProgram // one program of a the reference synth .fxb bank or a lone .fxp
};

// Index 0 of the string table is the empty string.
struct LibraryEntry
{
    std::string name;
    std::int64_t modified = 0; // milliseconds since the epoch
    std::uint32_t file = 0;    // index into LibraryData::files
    std::int32_t programIndex = 0;
    std::uint32_t category = 0, bank = 0, vendor = 0, location = 0, author = 0, description = 0, tags = 0; // string table indices
    LibraryKind kind = LibraryKind::Synthax1Preset;
    bool loose = false;        // a program that lives alone in its own .fxp file, not in a bank
    std::uint64_t hash = 0;    // of the sound itself (0: unknown); identical sounds share it
};

struct LibraryData
{
    std::vector<std::filesystem::path> files;
    std::vector<std::string> strings { std::string() };
    std::vector<LibraryEntry> entries;
    std::vector<std::string> searchText; // per entry: lower-case name, then the other fields, for matching and sorting
    // Identical sounds: canonical[i] is the entry that stands for i (i itself when it is the first of its kind),
    // copies[i] how many other entries are identical to canonical entry i.
    std::vector<std::int32_t> canonical;
    std::vector<std::int32_t> copies;

    const std::string& text(std::uint32_t index) const { return strings[index]; }
    const std::filesystem::path& pathOf(const LibraryEntry& entry) const { return files[entry.file]; }
    std::string idOf(const LibraryEntry& entry) const; // stable across rescans: file path plus program index
    // The entry an id refers to, or -1.
    int find(const std::string& id) const;
    std::vector<std::string> tagsOf(const LibraryEntry& entry) const;
};

struct LibraryRoot
{
    std::filesystem::path path;
    std::string label; // shown as the vendor of everything directly under it
};

struct LibraryUserData
{
    bool favorite = false;
    int rating = 0; // 0 to 5
    std::string notes;
};

// Category for a preset name written with a short code ("BA - Wub", "LD Saw Lead", "ARP Acid Burn").
// Returns an empty string when the name has no recognisable code.
std::string categoryFromPresetName(const std::string& name);

// The catalogue of the last scan, straight from the cache file without touching the folders. Only files under
// one of `roots` are included. Null when there is no usable cache.
std::shared_ptr<const LibraryData> loadCachedLibrary(const std::vector<LibraryRoot>& roots, const std::filesystem::path& cachePath);

// Scans the roots (in parallel). `cancelled` is polled between files. `progress` gets the number of files
// looked at; `partial` is offered a snapshot every few seconds while a long scan is running.
std::shared_ptr<const LibraryData> scanLibrary(const std::vector<LibraryRoot>& roots,
                                               const std::filesystem::path& cachePath,
                                               const std::function<bool()>& cancelled,
                                               const std::function<void(int)>& progress,
                                               const std::function<void(std::shared_ptr<const LibraryData>)>& partial = {});

class LibraryUserStore
{
public:
    explicit LibraryUserStore(std::filesystem::path file) : path(std::move(file)) { load(); }

    LibraryUserData get(const std::string& id) const;
    void set(const std::string& id, const LibraryUserData& data); // saves immediately
    const std::map<std::string, LibraryUserData>& all() const { return values; }

private:
    void load();
    void save() const;
    std::filesystem::path path;
    std::map<std::string, LibraryUserData> values;
};

struct LibrarySettings
{
    std::filesystem::path libraryFolder;        // the one folder where all banks and presets live (empty: none chosen)
    std::vector<std::filesystem::path> folders; // extra folders besides the factory and user preset folders
    bool previewOnSelect = false;
    bool previewOnSurface = true; // the main surface's own preview switch: a program change there plays middle C
    bool hideDuplicates = true; // show one entry for sounds that are identical
};

std::filesystem::path libraryDataDirectory();
LibrarySettings readLibrarySettings();
void writeLibrarySettings(const LibrarySettings& settings);
std::vector<LibraryRoot> libraryRoots(const LibrarySettings& settings);
} // namespace synth
