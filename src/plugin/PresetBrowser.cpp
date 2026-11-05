#include "PresetBrowser.h"

#include "../presets/PresetLibrary.h"

#include <algorithm>
#include <map>
#include <unordered_map>

namespace
{
// The classic editor's palette: graphite, cream lettering, amber lamps and the blue LCD.
const juce::Colour backgroundColour(0xff191c1c);
const juce::Colour panelColour(0xff121414);
const juce::Colour rowColour(0xff1f2323);
const juce::Colour rowAlternateColour(0xff1a1d1d);
const juce::Colour selectedRowColour(0xff638ba0);
const juce::Colour textColour(0xffd0d4d2);
const juce::Colour selectedTextColour(0xfffbf6e8);
const juce::Colour dimTextColour(0xff8b908d);
const juce::Colour accentColour(0xffe8b663);
const juce::Colour displayBlue(0xffa8c9d1);
const juce::Colour displayInk(0xff3e6077);
const juce::Colour graphite(0xff292b2c);

juce::Font font(float height, bool bold = false)
{
    return juce::Font(juce::FontOptions("Segoe UI", height, bold ? juce::Font::bold : juce::Font::plain));
}

juce::String text(const std::string& value)
{
    return juce::String::fromUTF8(value.c_str());
}

juce::String pathText(const std::filesystem::path& path)
{
    const auto utf8 = path.generic_u8string();
    return juce::String::fromUTF8(reinterpret_cast<const char*>(utf8.c_str()));
}

std::filesystem::path pathFrom(const juce::String& value)
{
    const auto* bytes = value.toRawUTF8();
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(bytes)));
}

std::string lowerCase(const std::string& value)
{
    return juce::String::fromUTF8(value.c_str()).toLowerCase().toStdString();
}

constexpr int columnName = 1, columnLocation = 2, columnCategory = 3, columnDate = 4, columnRating = 5;

enum class SideMode
{
    Categories,
    Banks,
    Vendors,
    Tags
};

// A row of the left list.
struct SideItem
{
    juce::String label;
    std::uint32_t id = 0; // string table index (categories, packs, vendors)
    std::string tag;      // for the tags list
    int count = 0;
    int special = 0; // 1 all presets, 2 favourites
};

void drawStar(juce::Graphics& g, juce::Rectangle<float> area, bool filled, juce::Colour colour)
{
    juce::Path star;
    star.addStar(area.getCentre(), 5, area.getWidth() * 0.22f, area.getWidth() * 0.5f, -juce::MathConstants<float>::halfPi);
    g.setColour(colour);
    if (filled)
        g.fillPath(star);
    else
        g.strokePath(star, juce::PathStrokeType(0.9f));
}

using DataPointer = std::shared_ptr<const synth::LibraryData>;

// Reads the cached catalogue first (so the list appears at once), then scans the folders for what changed.
class LoadThread final : public juce::Thread
{
public:
    LoadThread(std::vector<synth::LibraryRoot> libraryRoots, std::filesystem::path cacheFile, std::function<void(int)> onProgress,
               std::function<void(DataPointer, bool)> onData)
        : juce::Thread("Preset library"), roots(std::move(libraryRoots)), cache(std::move(cacheFile)),
          progress(std::move(onProgress)), deliver(std::move(onData))
    {
    }

    void run() override
    {
        if (auto cached = synth::loadCachedLibrary(roots, cache); cached != nullptr && !threadShouldExit())
            deliver(std::move(cached), false);
        if (threadShouldExit())
            return;
        auto result = synth::scanLibrary(
            roots, cache, [this] { return threadShouldExit(); }, progress,
            [this](DataPointer snapshot) { if (!threadShouldExit()) deliver(std::move(snapshot), false); });
        if (result != nullptr && !threadShouldExit())
            deliver(std::move(result), true);
    }

private:
    std::vector<synth::LibraryRoot> roots;
    std::filesystem::path cache;
    std::function<void(int)> progress;
    std::function<void(DataPointer, bool)> deliver;
};

// Runs a function once, a moment after the last request (so typing does not search on every key).
struct Delay final : juce::Timer
{
    std::function<void()> action;
    void timerCallback() override
    {
        stopTimer();
        if (action)
            action();
    }
};
} // namespace

class PresetBrowserPanel final : public juce::Component,
                                 private juce::TableListBoxModel,
                                 private juce::Timer,
                                 private juce::TextEditor::Listener
{
public:
    PresetBrowserPanel(SynthAudioProcessor& processorToUse, std::function<void()> onClose)
        : closeRequested(std::move(onClose)), owner(processorToUse), settings(synth::readLibrarySettings()),
          store(synth::libraryDataDirectory() / "library-user.json"), sideModel(*this), committed(processorToUse.capturePresetSnapshot())
    {
        data = std::make_shared<synth::LibraryData>();

        searchBox.setTextToShowWhenEmpty("Search presets   (cat:bass  pack:factory  tag:warm  fav:1  rating:3)", displayInk.withAlpha(0.7f));
        searchBox.setFont(font(14.0f));
        searchBox.setColour(juce::TextEditor::backgroundColourId, displayBlue);
        searchBox.setColour(juce::TextEditor::textColourId, displayInk.darker(0.4f));
        searchBox.setColour(juce::TextEditor::highlightColourId, displayInk.withAlpha(0.35f));
        searchBox.setColour(juce::TextEditor::outlineColourId, juce::Colour(0xff121414));
        searchBox.setColour(juce::TextEditor::focusedOutlineColourId, accentColour);
        searchBox.setColour(juce::CaretComponent::caretColourId, displayInk);
        searchBox.setIndents(30, 6);
        searchBox.addListener(this);
        addAndMakeVisible(searchBox);
        searchDelay.action = [this] { applyFilter(); };

        for (const auto& [mode, title] : { std::pair { SideMode::Categories, "CATEGORY" }, std::pair { SideMode::Banks, "PACK" },
                                           std::pair { SideMode::Vendors, "VENDOR" }, std::pair { SideMode::Tags, "TAGS" } })
        {
            auto* tab = tabs.add(new juce::TextButton(title));
            tab->setClickingTogglesState(true);
            tab->setRadioGroupId(4711);
            tab->setColour(juce::TextButton::buttonColourId, graphite);
            tab->setColour(juce::TextButton::buttonOnColourId, panelColour);
            tab->setColour(juce::TextButton::textColourOffId, dimTextColour);
            tab->setColour(juce::TextButton::textColourOnId, accentColour);
            tab->onClick = [this, mode] {
                if (sideMode != mode)
                {
                    sideMode = mode;
                    sideSelection = SideItem { {}, 0, {}, 0, 1 };
                    rebuildSide();
                    applyFilter();
                }
            };
            addAndMakeVisible(tab);
        }
        tabs[0]->setToggleState(true, juce::dontSendNotification);
        for (auto* tab : tabs)
            tab->setLookAndFeel(&smallButtons);

        sideList.setModel(&sideModel);
        sideList.setRowHeight(22);
        sideList.setColour(juce::ListBox::backgroundColourId, backgroundColour);
        sideList.setColour(juce::ListBox::outlineColourId, backgroundColour);
        addAndMakeVisible(sideList);

        table.setModel(this);
        table.setRowHeight(25);
        table.setColour(juce::ListBox::backgroundColourId, backgroundColour);
        table.setColour(juce::ListBox::outlineColourId, backgroundColour);
        auto& header = table.getHeader();
        header.setColour(juce::TableHeaderComponent::backgroundColourId, graphite);
        header.setColour(juce::TableHeaderComponent::textColourId, selectedTextColour);
        header.setColour(juce::TableHeaderComponent::outlineColourId, juce::Colour(0xff343836));
        header.addColumn("NAME", columnName, 300, 120, 600, juce::TableHeaderComponent::defaultFlags);
        header.addColumn("LOCATION", columnLocation, 220, 80, 500, juce::TableHeaderComponent::defaultFlags);
        header.addColumn("CATEGORY", columnCategory, 100, 60, 200, juce::TableHeaderComponent::defaultFlags);
        header.addColumn("MODIFIED", columnDate, 110, 80, 200, juce::TableHeaderComponent::defaultFlags);
        header.addColumn("RATING", columnRating, 100, 100, 100, juce::TableHeaderComponent::notResizable | juce::TableHeaderComponent::visible);
        header.setSortColumnId(columnName, true);
        header.setStretchToFitActive(true);
        table.getViewport()->setScrollBarsShown(true, false);
        for (auto* bar : { &table.getVerticalScrollBar(), &sideList.getVerticalScrollBar() })
        {
            bar->setColour(juce::ScrollBar::thumbColourId, juce::Colour(0xff6b7571));
            bar->setColour(juce::ScrollBar::trackColourId, juce::Colour(0xff121414));
        }
        addAndMakeVisible(table);

        for (auto* label : { &nameValue, &authorValue, &categoryValue, &bankValue, &locationValue, &descriptionValue, &tagsValue })
        {
            label->setFont(font(13.0f));
            label->setColour(juce::Label::textColourId, displayInk.darker(0.4f));
            label->setColour(juce::Label::backgroundColourId, displayBlue);
            label->setColour(juce::Label::outlineColourId, juce::Colour(0xff121414));
            label->setMinimumHorizontalScale(0.7f);
            addAndMakeVisible(*label);
        }
        notesBox.setMultiLine(true, true);
        notesBox.setReturnKeyStartsNewLine(true);
        notesBox.setFont(font(13.0f));
        notesBox.setColour(juce::TextEditor::backgroundColourId, juce::Colour(0xff1d2121));
        notesBox.setColour(juce::TextEditor::textColourId, textColour);
        notesBox.setColour(juce::TextEditor::outlineColourId, juce::Colour(0xff4a4f4d));
        notesBox.setColour(juce::TextEditor::focusedOutlineColourId, accentColour);
        notesBox.onFocusLost = [this] { saveNotesFor(detailEntry); };
        notesBox.onReturnKey = [] {};
        addAndMakeVisible(notesBox);

        favouriteButton.setButtonText("FAVOURITE");
        favouriteButton.setColour(juce::ToggleButton::textColourId, textColour);
        favouriteButton.setColour(juce::ToggleButton::tickColourId, accentColour);
        favouriteButton.onClick = [this] { setFavourite(favouriteButton.getToggleState()); };
        addAndMakeVisible(favouriteButton);

        previewButton.setButtonText("PLAY PREVIEW");
        previewButton.setToggleState(settings.previewOnSelect, juce::dontSendNotification);
        previewButton.setColour(juce::ToggleButton::textColourId, textColour);
        previewButton.setColour(juce::ToggleButton::tickColourId, accentColour);
        previewButton.onClick = [this] { settings.previewOnSelect = previewButton.getToggleState(); synth::writeLibrarySettings(settings); };
        addAndMakeVisible(previewButton);

        duplicatesButton.setButtonText("HIDE DUPLICATES");
        duplicatesButton.setToggleState(settings.hideDuplicates, juce::dontSendNotification);
        duplicatesButton.setColour(juce::ToggleButton::textColourId, textColour);
        duplicatesButton.setColour(juce::ToggleButton::tickColourId, accentColour);
        duplicatesButton.onClick = [this] {
            settings.hideDuplicates = duplicatesButton.getToggleState();
            synth::writeLibrarySettings(settings);
            rebuildSide();
            applyFilter();
        };
        addAndMakeVisible(duplicatesButton);

        hintLabel.setText("Double click to select", juce::dontSendNotification);
        hintLabel.setFont(font(12.0f));
        hintLabel.setColour(juce::Label::textColourId, accentColour);
        addAndMakeVisible(hintLabel);

        settingsButton.setButtonText("FOLDERS / SCAN");
        settingsButton.setColour(juce::TextButton::buttonColourId, graphite);
        settingsButton.setColour(juce::TextButton::textColourOffId, textColour);
        settingsButton.setLookAndFeel(&smallButtons);
        settingsButton.onClick = [this] { showSettingsMenu(); };
        addAndMakeVisible(settingsButton);

        chooseFolderButton.setButtonText("CHOOSE YOUR PRESET FOLDER...");
        chooseFolderButton.setColour(juce::TextButton::buttonColourId, graphite);
        chooseFolderButton.setColour(juce::TextButton::textColourOffId, accentColour);
        chooseFolderButton.onClick = [this] { chooseLibraryFolder(); };
        addChildComponent(chooseFolderButton);

        closeButton.setButtonText("CLOSE");
        closeButton.setColour(juce::TextButton::buttonColourId, graphite);
        closeButton.setColour(juce::TextButton::textColourOffId, accentColour);
        closeButton.setLookAndFeel(&smallButtons);
        closeButton.onClick = [this] { if (closeRequested) closeRequested(); };
        addAndMakeVisible(closeButton);

        statusLabel.setFont(font(12.0f));
        statusLabel.setColour(juce::Label::textColourId, dimTextColour);
        statusLabel.setJustificationType(juce::Justification::centredRight);
        addAndMakeVisible(statusLabel);

        setWantsKeyboardFocus(true);
        startLoading();
    }

    ~PresetBrowserPanel() override
    {
        for (auto* tab : tabs)
            tab->setLookAndFeel(nullptr);
        settingsButton.setLookAndFeel(nullptr);
        closeButton.setLookAndFeel(nullptr);
        stopPreview();
        if (auditioning)
        {
            juce::String message;
            owner.restorePresetSnapshot(committed, message);
        }
        saveNotesFor(detailEntry);
        if (loader != nullptr)
            loader->stopThread(8000);
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(backgroundColour);
        // Search magnifier.
        const auto box = searchBox.getBounds().toFloat();
        const auto centre = juce::Point<float>(box.getX() + 16.0f, box.getCentreY() - 1.0f);
        g.setColour(displayInk);
        g.drawEllipse(centre.x - 5.0f, centre.y - 5.0f, 9.0f, 9.0f, 1.4f);
        g.drawLine(centre.x + 2.5f, centre.y + 2.5f, centre.x + 7.0f, centre.y + 7.0f, 1.6f);

        // Right panel headings.
        g.setColour(panelColour);
        g.fillRect(detailsArea);
        g.setColour(dimTextColour);
        g.setFont(font(11.0f, true));
        for (const auto& [title, y] : headingPositions)
            g.drawText(title, detailsArea.getX() + 14, y, detailsArea.getWidth() - 28, 14, juce::Justification::left);

        // Rating stars in the details panel.
        const auto user = detailEntry >= 0 ? userOf(detailEntry) : synth::LibraryUserData {};
        for (int star = 0; star < 5; ++star)
            drawStar(g, starBounds(star).toFloat(), star < user.rating, star < user.rating ? accentColour : dimTextColour);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(8);
        auto top = area.removeFromTop(32);
        closeButton.setBounds(top.removeFromRight(70));
        top.removeFromRight(8);
        settingsButton.setBounds(top.removeFromRight(120));
        top.removeFromRight(8);
        searchBox.setBounds(top);
        area.removeFromTop(8);

        auto bottom = area.removeFromBottom(26);
        statusLabel.setBounds(bottom.removeFromRight(380));
        previewButton.setBounds(bottom.removeFromLeft(150));
        duplicatesButton.setBounds(bottom.removeFromLeft(160));
        bottom.removeFromLeft(10);
        hintLabel.setBounds(bottom.removeFromLeft(170));
        area.removeFromBottom(6);

        const auto leftWidth = juce::jlimit(150, 230, getWidth() / 5);
        const auto detailsWidth = juce::jlimit(200, 290, getWidth() * 27 / 100);
        auto left = area.removeFromLeft(leftWidth);
        auto tabRow = left.removeFromTop(24);
        const auto tabWidth = tabRow.getWidth() / 4;
        for (auto* tab : tabs)
            tab->setBounds(tabRow.removeFromLeft(tabWidth));
        sideList.setBounds(left);
        area.removeFromLeft(8);

        detailsArea = area.removeFromRight(detailsWidth);
        area.removeFromRight(8);
        table.setBounds(area);
        chooseFolderButton.setBounds(area.getCentreX() - 150, area.getCentreY() - 22, 300, 44);
        table.getHeader().resizeAllColumnsToFit(table.getVisibleContentWidth());

        layoutDetails();
        repaint();
    }

    bool keyPressed(const juce::KeyPress& key) override
    {
        if (key == juce::KeyPress::escapeKey)
        {
            if (closeRequested)
                closeRequested();
            return true;
        }
        if (key == juce::KeyPress::returnKey && selectedEntry >= 0)
        {
            commitSelected();
            return true;
        }
        if (key.isKeyCode('F') && key.getModifiers().isCtrlDown())
        {
            searchBox.grabKeyboardFocus();
            return true;
        }
        return false;
    }

    void mouseDown(const juce::MouseEvent& event) override
    {
        if (detailEntry < 0)
            return;
        for (int star = 0; star < 5; ++star)
            if (starBounds(star).contains(event.getPosition()))
            {
                auto user = userOf(detailEntry);
                user.rating = user.rating == star + 1 ? 0 : star + 1;
                setUser(detailEntry, user);
                table.repaint();
                repaint();
                return;
            }
    }

private:
    // ---- the catalogue --------------------------------------------------------------------------------------------
    // With duplicates hidden, only the first of several identical sounds is listed.
    bool listed(int index) const
    {
        return !settings.hideDuplicates || index >= static_cast<int>(data->canonical.size())
               || data->canonical[static_cast<std::size_t>(index)] == index;
    }

    const synth::LibraryEntry& entryAt(int index) const { return data->entries[static_cast<std::size_t>(index)]; }

    synth::LibraryUserData userOf(int index) const
    {
        const auto found = userByEntry.find(index);
        return found == userByEntry.end() ? synth::LibraryUserData {} : found->second;
    }

    void setUser(int index, const synth::LibraryUserData& user)
    {
        store.set(data->idOf(entryAt(index)), user);
        if (!user.favourite && user.rating == 0 && user.notes.empty())
            userByEntry.erase(index);
        else
            userByEntry[index] = user;
    }

    void rebuildUserIndex()
    {
        userByEntry.clear();
        for (const auto& [id, user] : store.all())
            if (const auto index = data->find(id); index >= 0)
                userByEntry[index] = user;
    }

    // ---- table model ----------------------------------------------------------------------------------------------
    int getNumRows() override { return static_cast<int>(rows.size()); }

    void paintRowBackground(juce::Graphics& g, int row, int width, int height, bool selected) override
    {
        g.setColour(selected ? selectedRowColour : (row % 2 == 0 ? rowColour : rowAlternateColour));
        g.fillRect(0, 0, width, height);
    }

    void paintCell(juce::Graphics& g, int row, int columnId, int width, int height, bool selected) override
    {
        if (row < 0 || row >= static_cast<int>(rows.size()))
            return;
        const auto index = rows[static_cast<std::size_t>(row)];
        const auto& entry = entryAt(index);
        g.setFont(font(13.0f));
        g.setColour(selected ? selectedTextColour : textColour);
        if (columnId == columnName)
        {
            // Play icon, then the name.
            const auto icon = juce::Rectangle<float>(7.0f, (height - 15.0f) * 0.5f, 15.0f, 15.0f);
            g.setColour(selected ? juce::Colour(0xff4a6f84) : juce::Colour(0xff3a3d3c));
            g.fillEllipse(icon);
            juce::Path triangle;
            triangle.addTriangle(icon.getX() + 5.5f, icon.getY() + 4.0f, icon.getX() + 5.5f, icon.getBottom() - 4.0f,
                                 icon.getRight() - 3.5f, icon.getCentreY());
            g.setColour(textColour);
            g.fillPath(triangle);
            auto left = 28;
            if (!userByEntry.empty() && userOf(index).favourite)
            {
                drawStar(g, { 28.0f, (height - 12.0f) * 0.5f, 12.0f, 12.0f }, true, accentColour);
                left = 44;
            }
            g.setColour(selected ? selectedTextColour : textColour);
            g.drawText(text(entry.name), left, 0, width - left - 4, height, juce::Justification::centredLeft, true);
        }
        else if (columnId == columnLocation)
            g.drawText(text(data->text(entry.location)), 6, 0, width - 8, height, juce::Justification::centredLeft, true);
        else if (columnId == columnCategory)
        {
            const auto& category = data->text(entry.category);
            g.setColour(category.empty() ? dimTextColour : (selected ? selectedTextColour : textColour));
            g.drawText(category.empty() ? "-" : text(category), 6, 0, width - 8, height, juce::Justification::centredLeft, true);
        }
        else if (columnId == columnDate)
            g.drawText(juce::Time(entry.modified).formatted("%Y-%m-%d"), 6, 0, width - 8, height, juce::Justification::centredLeft, true);
        else if (columnId == columnRating)
        {
            const auto rating = userByEntry.empty() ? 0 : userOf(index).rating;
            for (int star = 0; star < 5; ++star)
                drawStar(g, { 6.0f + static_cast<float>(star) * 17.0f, (height - 13.0f) * 0.5f, 13.0f, 13.0f }, star < rating,
                         star < rating ? accentColour : (selected ? juce::Colour(0xff9bb7c4) : juce::Colour(0xff4d514f)));
        }
    }

    void cellClicked(int row, int columnId, const juce::MouseEvent& event) override
    {
        if (row < 0 || row >= static_cast<int>(rows.size()))
            return;
        const auto index = rows[static_cast<std::size_t>(row)];
        if (columnId == columnRating)
        {
            const auto star = juce::jlimit(0, 4, (event.x - 6) / 17);
            auto user = userOf(index);
            user.rating = user.rating == star + 1 ? 0 : star + 1;
            setUser(index, user);
            table.repaintRow(row);
            repaint();
        }
        else if (columnId == columnName && event.x >= 4 && event.x <= 24)
        {
            // The play icon: load this preset for a moment and hear it.
            table.selectRow(row);
            selectedEntry = index;
            auditionSelected();
        }
        else if (event.mods.isPopupMenu())
        {
            table.selectRow(row);
            showRowMenu(index);
        }
    }

    void cellDoubleClicked(int, int, const juce::MouseEvent&) override { commitSelected(); }
    void returnKeyPressed(int) override { commitSelected(); }

    void sortOrderChanged(int newSortColumnId, bool isForwards) override
    {
        sortColumn = newSortColumnId;
        sortForwards = isForwards;
        resort();
        applyFilter();
    }

    void selectedRowsChanged(int lastRow) override
    {
        if (lastRow < 0 || lastRow >= static_cast<int>(rows.size()))
        {
            selectedEntry = -1;
            updateDetails();
            return;
        }
        selectedEntry = rows[static_cast<std::size_t>(lastRow)];
        updateDetails();
        if (ignoreSelection)
            return;
        if (settings.previewOnSelect)
            auditionSelected();
    }

    // ---- left list ------------------------------------------------------------------------------------------------
    class SideModel final : public juce::ListBoxModel
    {
    public:
        explicit SideModel(PresetBrowserPanel& content) : owner(content) {}
        int getNumRows() override { return static_cast<int>(owner.sideItems.size()); }
        void paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected) override
        {
            if (row < 0 || row >= getNumRows())
                return;
            const auto& item = owner.sideItems[static_cast<std::size_t>(row)];
            if (selected)
            {
                g.setColour(selectedRowColour);
                g.fillRect(0, 0, width, height);
            }
            g.setFont(font(13.0f, item.special != 0));
            g.setColour(selected ? selectedTextColour : (item.special != 0 ? textColour : textColour.withAlpha(0.85f)));
            g.drawText(item.label, 12, 0, width - 56, height, juce::Justification::centredLeft, true);
            g.setColour(dimTextColour);
            g.setFont(font(11.0f));
            g.drawText(juce::String(item.count), width - 50, 0, 42, height, juce::Justification::centredRight);
        }
        void selectedRowsChanged(int lastRow) override
        {
            if (owner.rebuildingSide || lastRow < 0 || lastRow >= getNumRows())
                return;
            owner.sideSelection = owner.sideItems[static_cast<std::size_t>(lastRow)];
            owner.applyFilter();
        }

    private:
        PresetBrowserPanel& owner;
    };

    // ---- loading --------------------------------------------------------------------------------------------------
    void startLoading()
    {
        if (loader != nullptr)
            loader->stopThread(8000);
        scanning = true;
        scannedFiles = 0;
        const juce::Component::SafePointer<PresetBrowserPanel> safe(this);
        loader = std::make_unique<LoadThread>(
            synth::libraryRoots(settings), synth::libraryDataDirectory() / "library-index.json",
            [safe](int files) { juce::MessageManager::callAsync([safe, files] { if (safe != nullptr) { safe->scannedFiles = files; safe->updateStatus(); } }); },
            [safe](DataPointer loaded, bool finished) {
                juce::MessageManager::callAsync([safe, loaded = std::move(loaded), finished] {
                    if (safe != nullptr)
                        safe->dataArrived(loaded, finished);
                });
            });
        loader->startThread();
        updateStatus();
    }

    void dataArrived(DataPointer loaded, bool finished)
    {
        const auto selectedId = selectedEntry >= 0 && selectedEntry < static_cast<int>(data->entries.size())
                                    ? data->idOf(entryAt(selectedEntry)) : std::string();
        saveNotesFor(detailEntry);
        data = std::move(loaded);
        selectedEntry = detailEntry = -1;
        scanning = !finished;
        rebuildUserIndex();
        resort();
        rebuildSide();
        applyFilter();
        if (!selectedId.empty())
            if (const auto index = data->find(selectedId); index >= 0)
                selectIndex(index);
        updateStatus();
    }

    void rebuildSide()
    {
        rebuildingSide = true;
        sideItems.clear();
        const auto favourites = static_cast<int>(std::count_if(userByEntry.begin(), userByEntry.end(), [](const auto& item) { return item.second.favourite; }));
        int shown = 0, loose = 0;
        for (std::size_t i = 0; i < data->entries.size(); ++i)
            if (listed(static_cast<int>(i)))
            {
                ++shown;
                loose += data->entries[i].loose ? 1 : 0;
            }
        sideItems.push_back({ "All presets", 0, {}, shown, 1 });
        sideItems.push_back({ "Favourites", 0, {}, favourites, 2 });
        sideItems.push_back({ "Loose presets", 0, {}, loose, 3 });
        if (sideMode == SideMode::Tags)
        {
            std::map<std::string, int> counts;
            for (std::size_t i = 0; i < data->entries.size(); ++i)
                if (data->entries[i].tags != 0 && listed(static_cast<int>(i)))
                    for (const auto& tag : data->tagsOf(data->entries[i]))
                        ++counts[tag];
            for (const auto& [tag, count] : counts)
                sideItems.push_back({ text(tag), 0, tag, count, 0 });
        }
        else
        {
            std::vector<int> counts(data->strings.size(), 0);
            for (std::size_t i = 0; i < data->entries.size(); ++i)
            {
                if (!listed(static_cast<int>(i)))
                    continue;
                const auto& entry = data->entries[i];
                const auto id = sideMode == SideMode::Categories ? entry.category : (sideMode == SideMode::Banks ? entry.bank : entry.vendor);
                ++counts[id];
            }
            std::vector<std::uint32_t> ids;
            for (std::uint32_t id = 0; id < counts.size(); ++id)
                if (counts[id] > 0 && (id != 0 || sideMode == SideMode::Categories))
                    ids.push_back(id);
            std::sort(ids.begin(), ids.end(), [this](std::uint32_t a, std::uint32_t b) {
                return lowerCase(data->text(a)) < lowerCase(data->text(b));
            });
            for (const auto id : ids)
                sideItems.push_back({ id == 0 ? juce::String("(no category)") : text(data->text(id)), id, {}, counts[id], 0 });
        }
        sideList.updateContent();
        int select = 0;
        for (std::size_t i = 0; i < sideItems.size(); ++i)
        {
            const auto& item = sideItems[i];
            if (item.special == sideSelection.special && item.special != 0)
                select = static_cast<int>(i);
            else if (sideSelection.special == 0 && item.special == 0
                     && ((sideMode == SideMode::Tags && item.tag == sideSelection.tag) || (sideMode != SideMode::Tags && item.id == sideSelection.id)))
                select = static_cast<int>(i);
        }
        sideList.selectRow(select, false, true);
        rebuildingSide = false;
    }

    // Sorts every entry once per column change; filtering keeps this order.
    void resort()
    {
        order.resize(data->entries.size());
        for (std::size_t i = 0; i < order.size(); ++i)
            order[i] = static_cast<int>(i);
        const auto& blobs = data->searchText;
        const auto byName = [&blobs](int a, int b) { return blobs[static_cast<std::size_t>(a)] < blobs[static_cast<std::size_t>(b)]; };
        std::function<bool(int, int)> before = byName;
        switch (sortColumn)
        {
            case columnLocation:
                before = [this, &byName](int a, int b) {
                    const auto& left = data->text(entryAt(a).location);
                    const auto& right = data->text(entryAt(b).location);
                    return left == right ? byName(a, b) : left < right;
                };
                break;
            case columnCategory:
                before = [this, &byName](int a, int b) {
                    const auto& left = data->text(entryAt(a).category);
                    const auto& right = data->text(entryAt(b).category);
                    return left == right ? byName(a, b) : left < right;
                };
                break;
            case columnDate:
                before = [this, &byName](int a, int b) {
                    return entryAt(a).modified == entryAt(b).modified ? byName(a, b) : entryAt(a).modified < entryAt(b).modified;
                };
                break;
            case columnRating:
                before = [this, &byName](int a, int b) {
                    const auto left = userOf(a).rating;
                    const auto right = userOf(b).rating;
                    return left == right ? byName(a, b) : left > right;
                };
                break;
            default: break;
        }
        std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return sortForwards ? before(a, b) : before(b, a); });
    }

    void applyFilter()
    {
        const auto keep = selectedEntry;
        // Terms: plain words match anywhere; key:value terms match one field.
        struct Term
        {
            std::string word;
            char key = 0; // 'c' category, 'p' pack, 'v' vendor, 'a' author, 't' tag, 'f' favourite, 'r' rating
            std::vector<char> poolMatch;
            int number = 0;
        };
        std::vector<Term> terms;
        for (const auto& word : juce::StringArray::fromTokens(searchBox.getText().toLowerCase(), " ", "\""))
        {
            if (word.isEmpty())
                continue;
            Term term;
            term.word = word.unquoted().toStdString();
            const auto colon = term.word.find(':');
            if (colon != std::string::npos && colon > 0 && colon + 1 < term.word.size())
            {
                const auto key = term.word.substr(0, colon);
                const auto value = term.word.substr(colon + 1);
                const auto matchPool = [&](char code) {
                    term.key = code;
                    term.word = value;
                    term.poolMatch.assign(data->strings.size(), 0);
                    for (std::size_t i = 0; i < data->strings.size(); ++i)
                        term.poolMatch[i] = lowerCase(data->strings[i]).find(value) != std::string::npos;
                };
                if (key == "cat" || key == "category") matchPool('c');
                else if (key == "pack" || key == "bank") matchPool('p');
                else if (key == "vendor") matchPool('v');
                else if (key == "author") matchPool('a');
                else if (key == "tag") matchPool('t');
                else if (key == "fav" || key == "favourite") { term.key = 'f'; term.number = value != "0"; }
                else if (key == "rating") { term.key = 'r'; term.number = juce::String(value).getIntValue(); }
            }
            terms.push_back(std::move(term));
        }

        rows.clear();
        rows.reserve(order.size());
        for (const auto index : order)
        {
            const auto& entry = entryAt(index);
            if (!listed(index))
                continue;
            if (sideSelection.special == 2 && !userOf(index).favourite)
                continue;
            if (sideSelection.special == 3 && !entry.loose)
                continue;
            if (sideSelection.special == 0)
            {
                if (sideMode == SideMode::Tags)
                {
                    const auto tags = data->tagsOf(entry);
                    if (std::find(tags.begin(), tags.end(), sideSelection.tag) == tags.end())
                        continue;
                }
                else
                {
                    const auto id = sideMode == SideMode::Categories ? entry.category : (sideMode == SideMode::Banks ? entry.bank : entry.vendor);
                    if (id != sideSelection.id)
                        continue;
                }
            }
            auto keepEntry = true;
            for (const auto& term : terms)
            {
                switch (term.key)
                {
                    case 'c': keepEntry = term.poolMatch[entry.category] != 0; break;
                    case 'p': keepEntry = term.poolMatch[entry.bank] != 0; break;
                    case 'v': keepEntry = term.poolMatch[entry.vendor] != 0; break;
                    case 'a': keepEntry = term.poolMatch[entry.author] != 0; break;
                    case 't': keepEntry = term.poolMatch[entry.tags] != 0; break;
                    case 'f': keepEntry = userOf(index).favourite == (term.number != 0); break;
                    case 'r': keepEntry = userOf(index).rating >= term.number; break;
                    default: keepEntry = data->searchText[static_cast<std::size_t>(index)].find(term.word) != std::string::npos; break;
                }
                if (!keepEntry)
                    break;
            }
            if (keepEntry)
                rows.push_back(index);
        }
        ignoreSelection = true;
        table.updateContent();
        selectedEntry = -1;
        if (keep >= 0)
            selectIndex(keep);
        else
            table.deselectAllRows();
        ignoreSelection = false;
        table.repaint();
        updateDetails();
        updateStatus();
    }

    void selectIndex(int index)
    {
        const auto found = std::find(rows.begin(), rows.end(), index);
        if (found == rows.end())
            return;
        const auto row = static_cast<int>(found - rows.begin());
        const auto wasIgnoring = ignoreSelection;
        ignoreSelection = true;
        table.selectRow(row);
        table.scrollToEnsureRowIsOnscreen(row);
        ignoreSelection = wasIgnoring;
        selectedEntry = index;
        updateDetails();
    }

    // ---- actions --------------------------------------------------------------------------------------------------
    // A single click only selects. With Play Preview on it also loads the preset for a moment and plays middle C;
    // the sound that was loaded before is put back when the browser closes without a double click.
    void auditionSelected()
    {
        if (selectedEntry < 0)
            return;
        const auto& entry = entryAt(selectedEntry);
        juce::String message;
        if (owner.loadLibraryEntry(data->pathOf(entry), entry.kind, entry.programIndex, message))
        {
            auditioning = true;
            statusText = "Previewing: " + text(entry.name) + "   (double click to select)";
            playPreview();
        }
        else
            statusText = message;
        updateStatus();
    }

    // A double click (or Enter) makes the preset the loaded one for good.
    void commitSelected()
    {
        if (selectedEntry < 0)
            return;
        const auto& entry = entryAt(selectedEntry);
        juce::String message;
        const auto ok = owner.loadLibraryEntry(data->pathOf(entry), entry.kind, entry.programIndex, message);
        statusText = ok ? "Loaded: " + text(entry.name) : message;
        if (ok)
        {
            committed = owner.capturePresetSnapshot();
            auditioning = false;
            if (closeRequested)
                closeRequested(); // picking a preset takes you back to the instrument
        }
        updateStatus();
    }

    void playPreview()
    {
        stopPreview();
        owner.submitUiMidiMessage(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)));
        previewing = true;
        startTimer(1600);
    }

    void stopPreview()
    {
        stopTimer();
        if (previewing)
            owner.submitUiMidiMessage(juce::MidiMessage::noteOff(1, 60));
        previewing = false;
    }

    void timerCallback() override { stopPreview(); }

    void setFavourite(bool favourite)
    {
        if (detailEntry < 0)
            return;
        auto user = userOf(detailEntry);
        user.favourite = favourite;
        setUser(detailEntry, user);
        rebuildSide();
        table.repaint();
    }

    void saveNotesFor(int index)
    {
        if (index < 0 || index >= static_cast<int>(data->entries.size()))
            return;
        auto user = userOf(index);
        const auto notes = notesBox.getText().toStdString();
        if (user.notes != notes)
        {
            user.notes = notes;
            setUser(index, user);
        }
    }

    void showRowMenu(int index)
    {
        juce::PopupMenu menu;
        const auto user = userOf(index);
        menu.addItem(1, user.favourite ? "Remove from favourites" : "Add to favourites");
        menu.addItem(2, "Show file in explorer");
        const auto path = data->pathOf(entryAt(index));
        menu.showMenuAsync({}, [this, index, path](int result) {
            if (result == 1)
            {
                auto stored = userOf(index);
                stored.favourite = !stored.favourite;
                setUser(index, stored);
                rebuildSide();
                applyFilter();
            }
            else if (result == 2)
                juce::File(pathText(path)).revealToUser();
        });
    }

    void chooseLibraryFolder()
    {
        chooser = std::make_unique<juce::FileChooser>("Choose the folder with all your banks and presets (subfolders are included)",
                                                      settings.libraryFolder.empty() ? juce::File() : juce::File(pathText(settings.libraryFolder)));
        chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                             [this](const juce::FileChooser& picked) {
                                 const auto folder = picked.getResult();
                                 if (folder != juce::File())
                                 {
                                     settings.libraryFolder = pathFrom(folder.getFullPathName());
                                     synth::writeLibrarySettings(settings);
                                     startLoading();
                                 }
                             });
    }

    void showSettingsMenu()
    {
        juce::PopupMenu menu;
        menu.addSectionHeader("Library folder (all banks and presets, subfolders included)");
        menu.addItem(10, settings.libraryFolder.empty() ? juce::String("Choose library folder...")
                                                         : pathText(settings.libraryFolder) + "   (change...)");
        if (!settings.libraryFolder.empty())
            menu.addItem(11, "Clear library folder");
        menu.addSeparator();
        menu.addItem(1, "Add another folder...");
        juce::PopupMenu remove;
        for (std::size_t i = 0; i < settings.folders.size(); ++i)
            remove.addItem(100 + static_cast<int>(i), pathText(settings.folders[i]));
        menu.addSubMenu("Remove folder", remove, !settings.folders.empty());
        menu.addSeparator();
        menu.addItem(2, "Rescan all folders");
        menu.addItem(3, "Open the user preset folder");
        const auto mouse = juce::Desktop::getInstance().getMainMouseSource().getScreenPosition().roundToInt();
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetScreenArea(juce::Rectangle<int>(mouse.x, mouse.y, 1, 1)), [this](int result) {
            if (result == 10)
                chooseLibraryFolder();
            else if (result == 11)
            {
                settings.libraryFolder.clear();
                synth::writeLibrarySettings(settings);
                startLoading();
            }
            else if (result == 1)
            {
                chooser = std::make_unique<juce::FileChooser>("Choose another folder with presets or Sylenth1 banks");
                chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                                     [this](const juce::FileChooser& picked) {
                                         const auto folder = picked.getResult();
                                         if (folder != juce::File())
                                         {
                                             settings.folders.push_back(pathFrom(folder.getFullPathName()));
                                             synth::writeLibrarySettings(settings);
                                             startLoading();
                                         }
                                     });
            }
            else if (result >= 100 && result < 100 + static_cast<int>(settings.folders.size()))
            {
                settings.folders.erase(settings.folders.begin() + (result - 100));
                synth::writeLibrarySettings(settings);
                startLoading();
            }
            else if (result == 2)
                startLoading();
            else if (result == 3)
            {
                const auto directory = synth::defaultUserPresetDirectory();
                std::error_code ignored;
                std::filesystem::create_directories(directory, ignored);
                juce::File(pathText(directory)).startAsProcess();
            }
        });
    }

    void updateStatus()
    {
        auto line = juce::String(static_cast<int>(rows.size())) + " of " + juce::String(static_cast<int>(data->entries.size())) + " presets";
        if (settings.hideDuplicates && !data->canonical.empty())
        {
            const auto hidden = static_cast<int>(std::count_if(data->copies.begin(), data->copies.end(), [](int copies) { return copies > 0; }));
            int copiesTotal = 0;
            for (const auto copies : data->copies)
                copiesTotal += copies;
            if (hidden > 0)
                line += "   (" + juce::String(copiesTotal) + " duplicates hidden)";
        }
        if (scanning)
            line = "Scanning... " + juce::String(scannedFiles) + " files   " + line;
        else if (statusText.isNotEmpty())
            line = statusText + "     " + line;
        const auto needsFolder = settings.libraryFolder.empty() && settings.folders.empty();
        chooseFolderButton.setVisible(needsFolder && !scanning);
        if (!scanning && needsFolder)
            line = "Choose your library folder in FOLDERS / SCAN     " + line;
        statusLabel.setText(line, juce::dontSendNotification);
    }

    // ---- details panel --------------------------------------------------------------------------------------------
    void layoutDetails()
    {
        headingPositions.clear();
        auto area = detailsArea.reduced(14, 8);
        auto place = [&](const juce::String& title, juce::Component& component, int height) {
            headingPositions.emplace_back(title, area.getY());
            area.removeFromTop(14);
            component.setBounds(area.removeFromTop(height));
            area.removeFromTop(6);
        };
        place("PRESET NAME", nameValue, 22);
        place("AUTHOR", authorValue, 22);
        place("CATEGORY", categoryValue, 22);
        place("PACK", bankValue, 22);
        place("LOCATION", locationValue, 22);
        place("DESCRIPTION", descriptionValue, 36);
        headingPositions.emplace_back("RATING", area.getY());
        area.removeFromTop(14);
        ratingRow = area.removeFromTop(20);
        favouriteButton.setBounds(ratingRow.removeFromRight(110));
        area.removeFromTop(6);
        place("TAGS", tagsValue, 28);
        headingPositions.emplace_back("NOTES", area.getY());
        area.removeFromTop(14);
        notesBox.setBounds(area);
    }

    juce::Rectangle<int> starBounds(int star) const
    {
        return { ratingRow.getX() + star * 22, ratingRow.getY() + 2, 20, 18 };
    }

    void updateDetails()
    {
        saveNotesFor(detailEntry);
        detailEntry = selectedEntry;
        const synth::LibraryEntry* entry = detailEntry >= 0 ? &entryAt(detailEntry) : nullptr;
        const auto user = detailEntry >= 0 ? userOf(detailEntry) : synth::LibraryUserData {};
        const auto set = [](juce::Label& label, const std::string& value) { label.setText(" " + text(value), juce::dontSendNotification); };
        set(nameValue, entry != nullptr ? entry->name : std::string());
        set(authorValue, entry != nullptr ? data->text(entry->author) : std::string());
        set(categoryValue, entry != nullptr ? data->text(entry->category) : std::string());
        set(bankValue, entry != nullptr ? data->text(entry->bank) : std::string());
        std::string where = entry != nullptr ? data->text(entry->location) : std::string();
        if (detailEntry >= 0 && detailEntry < static_cast<int>(data->copies.size()) && data->copies[static_cast<std::size_t>(detailEntry)] > 0)
            where += "   (+" + std::to_string(data->copies[static_cast<std::size_t>(detailEntry)]) + " identical copies)";
        set(locationValue, where);
        set(descriptionValue, entry != nullptr ? data->text(entry->description) : std::string());
        std::string tags;
        if (entry != nullptr)
            for (const auto& tag : data->tagsOf(*entry))
                tags += (tags.empty() ? "" : "   ") + tag;
        set(tagsValue, tags);
        notesBox.setText(text(user.notes), juce::dontSendNotification);
        notesBox.setEnabled(entry != nullptr);
        favouriteButton.setEnabled(entry != nullptr);
        favouriteButton.setToggleState(user.favourite, juce::dontSendNotification);
        repaint(detailsArea);
    }

    void textEditorTextChanged(juce::TextEditor& editor) override
    {
        if (&editor == &searchBox)
            searchDelay.startTimer(160);
    }

    void textEditorEscapeKeyPressed(juce::TextEditor& editor) override
    {
        if (&editor != &searchBox)
            return;
        if (editor.isEmpty())
        {
            if (closeRequested)
                closeRequested();
            return;
        }
        editor.clear();
        applyFilter();
    }

    struct SmallButtons final : juce::LookAndFeel_V4
    {
        juce::Font getTextButtonFont(juce::TextButton&, int) override { return font(10.0f, true); }
    };

    SmallButtons smallButtons;
    std::function<void()> closeRequested;
    juce::TextButton closeButton, chooseFolderButton;
    SynthAudioProcessor& owner;
    synth::LibrarySettings settings;
    synth::LibraryUserStore store;
    DataPointer data;
    std::unordered_map<int, synth::LibraryUserData> userByEntry; // by entry index, for entries with favourites, ratings or notes
    std::vector<int> order; // every entry, sorted by the current column
    std::vector<int> rows;  // the entries that pass the filter, in that order
    int selectedEntry = -1;
    int detailEntry = -1;
    int sortColumn = columnName;
    bool sortForwards = true;
    bool ignoreSelection = false;
    bool rebuildingSide = false;
    bool previewing = false;
    bool scanning = false;
    int scannedFiles = 0;
    juce::String statusText;

    SideMode sideMode = SideMode::Categories;
    SideItem sideSelection { {}, 0, {}, 0, 1 };
    std::vector<SideItem> sideItems;
    SideModel sideModel;
    juce::ListBox sideList;
    juce::OwnedArray<juce::TextButton> tabs;
    Delay searchDelay;

    juce::TextEditor searchBox;
    juce::TableListBox table;
    juce::TextButton settingsButton;
    juce::ToggleButton previewButton, favouriteButton, duplicatesButton;
    juce::Label hintLabel;
    SynthAudioProcessor::PresetSnapshot committed;
    bool auditioning = false;
    juce::Label statusLabel;
    juce::Label nameValue, authorValue, categoryValue, bankValue, locationValue, descriptionValue, tagsValue;
    juce::TextEditor notesBox;
    juce::Rectangle<int> detailsArea, ratingRow;
    std::vector<std::pair<juce::String, int>> headingPositions;
    std::unique_ptr<LoadThread> loader;
    std::unique_ptr<juce::FileChooser> chooser;
};

juce::Component* createPresetBrowserPanel(SynthAudioProcessor& processor, std::function<void()> onClose)
{
    return new PresetBrowserPanel(processor, std::move(onClose));
}
