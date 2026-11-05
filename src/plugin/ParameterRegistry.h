#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <string>
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
    bool automatable = true;
    bool presetSerialized = true;
    float smoothMs = 0.0f;
    int auVersionHint = 1;
    std::vector<std::string> choices;
    int defaultChoice = 0;
    // Sylenth1's Init value for this parameter, when Init sets it: the start value of a new instance and of
    // the Init preset, and what Ctrl+click resets to. Presets load on top of defaultValue, not this.
    bool hasInit = false;
    float initValue = 0.0f;
};

// The value a new instance, the Init preset and Ctrl+click reset use (a choice's index for choices).
inline float initialParameterValue(const ParameterSpec& spec) noexcept
{
    return spec.hasInit ? spec.initValue : (spec.kind == ParameterKind::Choice ? static_cast<float>(spec.defaultChoice) : spec.defaultValue);
}

const std::vector<ParameterSpec>& getParameterSpecs();
const ParameterSpec* findParameterSpec(const std::string& id);
float clampPhysicalParameterValue(const ParameterSpec& spec, float value) noexcept;
std::vector<std::string> validateParameterSpecs();

juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

const char* toString(ParameterKind kind) noexcept;
} // namespace synth
