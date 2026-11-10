#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <string>
#include <utility>
#include <vector>

namespace synth
{
enum class ParameterKind
{
    Float,
    Bool,
    Choice
};

struct ParameterSpec
{
    std::string id;
    std::string name;
    std::string group;
    ParameterKind kind = ParameterKind::Float;
    std::string unit;
    float minimum = 0.0f;
    float maximum = 1.0f;
    float defaultValue = 0.0f;
    float interval = 0.0f;
    float skew = 1.0f;
    // The value grows by the same factor for each step of the knob (minimum x (maximum / minimum)^position); skew is unused.
    bool exponential = false;
    bool automatable = true;
    bool presetSerialized = true;
    float smoothMs = 0.0f;
    int auVersionHint = 1;
    std::vector<std::string> choices;
    int defaultChoice = 0;
    // Earlier labels of renamed choices and the choice each now means. Presets that store an
    // earlier label still load; they are written back with the current label.
    std::vector<std::pair<std::string, int>> formerChoices;
    // The reference synth's Init value for this parameter, when Init sets it: the start value of a new instance and of
    // the Init preset, and what Ctrl+click resets to. Presets load on top of defaultValue, not this.
    bool hasInit = false;
    float initValue = 0.0f;
};

// The value a new instance, the Init preset and Ctrl+click reset use (a choice's index for choices).
inline float initialParameterValue(const ParameterSpec& spec) noexcept
{
    return spec.hasInit ? spec.initValue : (spec.kind == ParameterKind::Choice ? static_cast<float>(spec.defaultChoice) : spec.defaultValue);
}

// The index of a choice label, current or former, or -1 if the parameter has no such choice.
inline int findChoice(const ParameterSpec& spec, const std::string& label) noexcept
{
    for (std::size_t index = 0; index < spec.choices.size(); ++index)
        if (spec.choices[index] == label)
            return static_cast<int>(index);
    for (const auto& [former, index] : spec.formerChoices)
        if (former == label)
            return index;
    return -1;
}

const std::vector<ParameterSpec>& getParameterSpecs();
// The host-facing range of a float parameter: linear with skew, or exponential.
juce::NormalisableRange<float> parameterRange(const ParameterSpec& spec);
const ParameterSpec* findParameterSpec(const std::string& id);
float clampPhysicalParameterValue(const ParameterSpec& spec, float value) noexcept;
std::vector<std::string> validateParameterSpecs();

juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

const char* toString(ParameterKind kind) noexcept;
} // namespace synth
