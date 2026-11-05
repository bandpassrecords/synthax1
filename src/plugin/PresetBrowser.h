#pragma once

#include <JuceHeader.h>

#include "PluginProcessor.h"

#include <functional>
#include <memory>

// An overlay to browse every preset and bank under the configured folders: categories, banks, vendors
// and tags on the left, a sortable list with search in the middle, details, notes, favourites and ratings on the
// right, and an optional middle C preview when a preset is picked.
// Creates the browser panel (the caller owns it): a component to add inside the main window, over the
// instrument. `onClose` is called when the user asks to close it (Close button or Escape).
juce::Component* createPresetBrowserPanel(SynthAudioProcessor& processor, std::function<void()> onClose);
