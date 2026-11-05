#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <map>
#include <vector>

namespace synth
{
// Unlimited undo and redo of parameter edits made from the editor. One entry is one gesture (a knob drag,
// a click, a Ctrl+click reset). Edits that arrive without a gesture (host automation, program loads) are not
// recorded; loading a program clears the history so an undo never reaches back across it.
class ParameterHistory final : private juce::AudioProcessorParameter::Listener
{
public:
    ParameterHistory() = default;
    ~ParameterHistory() override { detach(); }

    void attach(juce::AudioProcessor& target)
    {
        detach();
        processor = &target;
        for (auto* parameter : processor->getParameters())
            parameter->addListener(this);
    }

    void detach()
    {
        if (processor == nullptr)
            return;
        for (auto* parameter : processor->getParameters())
            parameter->removeListener(this);
        processor = nullptr;
    }

    void clear()
    {
        undoStack.clear();
        redoStack.clear();
        openGestures.clear();
    }

    bool canUndo() const noexcept { return !undoStack.empty(); }
    bool canRedo() const noexcept { return !redoStack.empty(); }

    bool undo()
    {
        if (undoStack.empty())
            return false;
        const auto edit = undoStack.back();
        undoStack.pop_back();
        apply(edit.index, edit.before);
        redoStack.push_back(edit);
        return true;
    }

    bool redo()
    {
        if (redoStack.empty())
            return false;
        const auto edit = redoStack.back();
        redoStack.pop_back();
        apply(edit.index, edit.after);
        undoStack.push_back(edit);
        return true;
    }

private:
    struct Edit
    {
        int index = 0;
        float before = 0.0f;
        float after = 0.0f;
    };

    void parameterValueChanged(int, float) override {}

    void parameterGestureChanged(int index, bool starting) override
    {
        if (applying || processor == nullptr || index < 0 || index >= processor->getParameters().size())
            return;
        auto* parameter = processor->getParameters()[index];
        if (starting)
        {
            openGestures.emplace(index, parameter->getValue()); // keeps the first start if gestures nest
            return;
        }
        const auto found = openGestures.find(index);
        if (found == openGestures.end())
            return;
        const auto before = found->second;
        openGestures.erase(found);
        const auto after = parameter->getValue();
        if (std::abs(after - before) > 1.0e-6f)
        {
            undoStack.push_back({ index, before, after });
            redoStack.clear();
        }
    }

    void apply(int index, float value)
    {
        if (processor == nullptr || index < 0 || index >= processor->getParameters().size())
            return;
        auto* parameter = processor->getParameters()[index];
        applying = true;
        parameter->beginChangeGesture();
        parameter->setValueNotifyingHost(value);
        parameter->endChangeGesture();
        applying = false;
    }

    juce::AudioProcessor* processor = nullptr;
    std::vector<Edit> undoStack;
    std::vector<Edit> redoStack;
    std::map<int, float> openGestures;
    bool applying = false;
};
} // namespace synth
