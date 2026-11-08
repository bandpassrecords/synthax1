#include "PluginEditor.h"
#include "PresetBrowser.h"
#include "../dsp/Lfo.h"
#include "../modulation/ModulationRouteModel.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <map>
#include <set>
#include <tuple>
#include <vector>

namespace
{
constexpr int canvasWidth = 908;
constexpr int modulationExtra = 87; // the modulation section is taller than the original layout; everything under it moves down
constexpr int canvasHeight = 591 + modulationExtra;
const auto graphite = juce::Colour(0xff292b2c);
const auto silver = juce::Colour(0xffd0d4d2);
const auto lettering = juce::Colour(0xffe7e5dc);
const auto displayBlue = juce::Colour(0xffa8c9d1);
const auto displayInk = juce::Colour(0xff3e6077);
const auto amber = juce::Colour(0xffe8b663);

// Each modulation source has its own colour: on its display, on its drag handle and on the ring it draws around a target.
juce::Colour modulationColour(int slot)
{
    switch (slot)
    {
        case 1: return juce::Colour(0xffe8b663);
        case 2: return juce::Colour(0xff6fd0c4);
        case 3: return juce::Colour(0xffa98cf0);
        case 4: return juce::Colour(0xfff08aa5);
        default: return juce::Colour(0xff9bb4ff);
    }
}
juce::String modulationName(int slot)
{
    static const char* const names[] = { "Mod Env 1", "Mod Env 2", "LFO 1", "LFO 2" };
    return slot >= 1 && slot <= 4 ? juce::String(names[slot - 1]) : "Mod " + juce::String(slot);
}
constexpr const char* modulationDragPrefix = "synthax1.mod:";

// The first four slots belong to Mod Env 1, Mod Env 2, LFO 1 and LFO 2; the others take any source.
int fixedModulationSource(int slot)
{
    switch (slot)
    {
        case 1: return static_cast<int>(synth::ModSource::ModEnv);
        case 2: return static_cast<int>(synth::ModSource::ModEnv2);
        case 3: return static_cast<int>(synth::ModSource::Lfo);
        case 4: return static_cast<int>(synth::ModSource::Lfo2);
        default: return -1;
    }
}
float readParameterValue(SynthAudioProcessor& owner, const std::string& id)
{
    const auto* value = owner.getValueTreeState().getRawParameterValue(id);
    return value != nullptr ? value->load() : 0.0f;
}
void setParameterValue(SynthAudioProcessor& owner, const std::string& id, float physical)
{
    if (auto* parameter = owner.getValueTreeState().getParameter(id))
    {
        parameter->beginChangeGesture();
        parameter->setValueNotifyingHost(parameter->convertTo0to1(physical));
        parameter->endChangeGesture();
    }
}
void activateModulationSlot(SynthAudioProcessor& owner, int slot)
{
    const auto prefix = "transmod." + std::to_string(slot) + ".";
    setParameterValue(owner, prefix + "enabled", 1.0f);
    if (const auto fixed = fixedModulationSource(slot); fixed >= 0)
        setParameterValue(owner, prefix + "source", static_cast<float>(fixed));
}
// A source reaches a destination through that destination's depth parameter on the source's slot. Any number of
// destinations can be set on one slot, so a source is not limited to two targets.
std::string modulationDepthId(int slot, int destinationIndex)
{
    const auto& catalog = synth::modulationDestinationCatalog();
    if (destinationIndex < 0 || destinationIndex >= static_cast<int>(catalog.size()))
        return {};
    return synth::transModDepthParameterId(slot, catalog[static_cast<std::size_t>(destinationIndex)]);
}
float modulationDepthScale(SynthAudioProcessor& owner, const std::string& depthId)
{
    if (const auto* parameter = owner.getValueTreeState().getParameter(depthId))
    {
        const auto range = parameter->getNormalisableRange();
        return std::max(std::abs(range.start), std::abs(range.end));
    }
    return 1.0f;
}
float modulationAmount(SynthAudioProcessor& owner, int slot, int destinationIndex)
{
    const auto id = modulationDepthId(slot, destinationIndex);
    return id.empty() ? 0.0f : readParameterValue(owner, id) / modulationDepthScale(owner, id);
}
void setModulationAmount(SynthAudioProcessor& owner, int slot, int destinationIndex, float amount)
{
    const auto id = modulationDepthId(slot, destinationIndex);
    if (id.empty())
        return;
    activateModulationSlot(owner, slot);
    setParameterValue(owner, id, juce::jlimit(-1.0f, 1.0f, amount) * modulationDepthScale(owner, id));
}

// Menus open at the mouse, wherever the editor sits and however it is scaled.
juce::PopupMenu::Options optionsAtMouse()
{
    const auto position = juce::Desktop::getInstance().getMainMouseSource().getScreenPosition().roundToInt();
    return juce::PopupMenu::Options().withTargetScreenArea(juce::Rectangle<int>(position.x, position.y, 1, 1));
}

juce::Font font(float height, bool bold = false, bool italic = false)
{
    return juce::Font(juce::FontOptions("Arial", height,
                                        (bold ? juce::Font::bold : juce::Font::plain) | (italic ? juce::Font::italic : 0)));
}

void drawText(juce::Graphics& g, const juce::String& value, juce::Rectangle<int> bounds,
              float height = 9.0f, juce::Colour colour = lettering,
              juce::Justification justification = juce::Justification::centred,
              bool bold = false, bool italic = false)
{
    g.setColour(colour);
    g.setFont(font(height, bold, italic));
    g.drawText(value, bounds, justification, false);
}

void recessedField(juce::Graphics& g, juce::Rectangle<float> area,
                   juce::Colour fill = displayBlue)
{
    g.setColour(juce::Colours::black.withAlpha(0.7f));
    g.fillRoundedRectangle(area.expanded(1.5f), 3.5f);
    g.setColour(silver.withAlpha(0.38f));
    g.drawRoundedRectangle(area.translated(0, 1), 3.0f, 1.0f);
    g.setGradientFill(juce::ColourGradient(fill.darker(0.18f), area.getTopLeft(),
                                           fill.brighter(0.08f), area.getBottomLeft(), false));
    g.fillRoundedRectangle(area, 2.5f);
    g.setColour(juce::Colours::white.withAlpha(0.22f));
    g.drawHorizontalLine(juce::roundToInt(area.getY() + 1), area.getX() + 3, area.getRight() - 3);
}

void panel(juce::Graphics& g, juce::Rectangle<int> bounds, const juce::String& title,
           bool partPanel = false, int selectedPart = 0)
{
    const auto body = partPanel ? (selectedPart == 0 ? juce::Colour(0xff726458)
                                                     : juce::Colour(0xff626b73))
                                : juce::Colour(0xff454747);
    g.setColour(juce::Colour(0xff121414));
    g.fillRoundedRectangle(bounds.toFloat(), 8.0f);
    g.setGradientFill(juce::ColourGradient(body.brighter(0.05f), bounds.getTopLeft().toFloat(),
                                           body.darker(0.15f), bounds.getBottomLeft().toFloat(), false));
    g.fillRoundedRectangle(bounds.toFloat().reduced(1.5f), 7.0f);
    auto header = bounds.withHeight(16).reduced(2, 1);
    g.setGradientFill(juce::ColourGradient(juce::Colour(0xff333535), header.getTopLeft().toFloat(),
                                           juce::Colour(0xff141616), header.getBottomLeft().toFloat(), false));
    g.fillRoundedRectangle(header.toFloat(), 6.0f);
    g.setColour(juce::Colours::white.withAlpha(0.16f));
    g.drawHorizontalLine(header.getY() + 1, static_cast<float>(header.getX() + 6),
                         static_cast<float>(header.getRight() - 6));
    drawText(g, title, header, 10.1f, juce::Colour(0xfff2d1a0), juce::Justification::centred, true);
}

juce::String formattedValue(const synth::ParameterSpec& spec, float value)
{
    if (std::abs(value) < 0.00001f) value = 0.0f;
    if (spec.kind == synth::ParameterKind::Choice)
    {
        const auto index = juce::jlimit(0, static_cast<int>(spec.choices.size()) - 1, juce::roundToInt(value));
        return spec.choices[static_cast<std::size_t>(index)];
    }
    if (spec.kind == synth::ParameterKind::Bool)
        return value >= 0.5f ? "On" : "Off";
    if (spec.unit == "milliseconds")
        return value >= 1000 ? juce::String(value / 1000, 2) + " s" : juce::String(value, 1) + " ms";
    if (spec.unit == "Hz")
        return juce::String(value, value < 10 ? 2 : 1) + " Hz";
    if (spec.unit == "normalized" || spec.unit == "percent")
        return juce::String(value * 100, 1) + " %";
    if (spec.unit == "dB")
        return juce::String(value, 1) + " dB";
    if (spec.interval >= 1)
        return juce::String(juce::roundToInt(value));
    return juce::String(value, 2) + (spec.unit == "degrees" ? " deg" : "");
}

class ClassicParameterControl final : public juce::Component, public juce::DragAndDropTarget
{
public:
    enum class Style
    {
        Knob,
        DisplayKnob,
        SmallKnob,
        Fader,
        Field,
        Switch,
        Wave,
        Filter,
        Slope,
        Selector,
        Route
    };
    ClassicParameterControl(SynthAudioProcessor& processor, std::string parameterId,
                            juce::String caption, Style controlStyle,
                            std::function<void(const juce::String&, const juce::String&)> readout)
        : owner(processor), id(std::move(parameterId)), label(std::move(caption)), style(controlStyle),
          publishReadout(std::move(readout)), parameter(owner.getValueTreeState().getParameter(id)),
          specification(synth::findParameterSpec(id))
    {
        setName(specification != nullptr ? specification->name : id);
        if (style == Style::Slope) setName(getName().replace("Mode", "Slope"));
        if (id == "voice.mode") setName("Mono Legato");
        setTitle(getName());
        setDescription("Instrument parameter " + juce::String(id));
        setMouseCursor(juce::MouseCursor::DraggingHandCursor);
        setWantsKeyboardFocus(true);
        if (parameter == nullptr || specification == nullptr)
            juce::Logger::writeToLog("Synthax1 editor has no parameter binding for: " + juce::String(id));
        jassert(parameter != nullptr && specification != nullptr);
        findModulationTarget();
        updateValue();
    }

    ~ClassicParameterControl() override
    {
        if (dragging && parameter != nullptr) parameter->endChangeGesture();
    }

    const std::string& parameterId() const noexcept { return id; }
    void updateValue()
    {
        if (parameter == nullptr)
            return;
        if (style == Style::Filter)
        {
            const auto enabledId = id.substr(0, id.size() - 4) + "enabled";
            const auto* enabledParameter = owner.getValueTreeState().getRawParameterValue(enabledId);
            const auto nextEnabled = enabledParameter == nullptr || enabledParameter->load() >= 0.5f;
            if (filterEnabled != nextEnabled)
            {
                filterEnabled = nextEnabled;
                repaint();
                if (auto* handler = getAccessibilityHandler()) handler->notifyAccessibilityEvent(juce::AccessibilityEvent::valueChanged);
            }
        }
        refreshModulation();
        const auto next = parameter->getValue();
        if (std::abs(next - normalized) > 0.0000001f)
        {
            normalized = next;
            repaint();
            if (auto* handler = getAccessibilityHandler()) handler->notifyAccessibilityEvent(juce::AccessibilityEvent::valueChanged);
        }
    }
    float physical() const { return parameter != nullptr ? parameter->convertFrom0to1(normalized) : 0; }
    // Knobs and faders read 0 to 10 (or -10 to 10 when the control goes both ways), as on the reference synth.
    juce::String displayText(float physicalValue) const
    {
        const auto knobStyle = style == Style::Knob || style == Style::DisplayKnob || style == Style::SmallKnob || style == Style::Fader;
        if (specification == nullptr || parameter == nullptr || !knobStyle || specification->kind != synth::ParameterKind::Float
            || specification->interval >= 1.0f)
            return specification != nullptr ? formattedValue(*specification, physicalValue) : juce::String();
        const auto position = parameter->convertTo0to1(physicalValue);
        // The Filter Control knobs (cutoff, resonance, ...) read 0 to 10 with the default at 5, like the part knobs.
        const auto bipolar = specification->minimum < 0.0f && id.rfind("filter_control.", 0) != 0;
        const auto scaled = bipolar ? (position * 2.0f - 1.0f) * 10.0f : position * 10.0f;
        return juce::String(std::abs(scaled) < 0.05f ? 0.0f : scaled, 1);
    }
    void writePhysical(float value, bool completeGesture = true)
    {
        if (parameter == nullptr || specification == nullptr || !std::isfinite(value))
            return;
        value = synth::clampPhysicalParameterValue(*specification, value);
        if (completeGesture && dragging)
        {
            parameter->endChangeGesture();
            dragging = false;
        }
        if (completeGesture)
            parameter->beginChangeGesture();
        parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
        if (id == "layer.1.solo" || id == "layer.2.solo")
        {
            const auto otherId = id == "layer.1.solo" ? "layer.2.solo" : "layer.1.solo";
            if (auto* other = owner.getValueTreeState().getParameter(otherId))
            {
                other->beginChangeGesture();
                other->setValueNotifyingHost(0.0f);
                other->endChangeGesture();
            }
        }
        if (completeGesture)
            parameter->endChangeGesture();
        updateValue();
        announce();
    }
    void paint(juce::Graphics& g) override
    {
        if (specification == nullptr)
        {
            drawText(g, "UNBOUND", getLocalBounds(), 7, juce::Colours::red);
            return;
        }
        const auto area = getLocalBounds().toFloat();
        if (style == Style::Slope || style == Style::Selector)
        {
            auto housing = area.reduced(4, 2);
            g.setColour(juce::Colour(0xff181d1b));
            g.fillRoundedRectangle(housing, housing.getWidth() * 0.5f);
            const auto selected = style == Style::Slope ? (juce::roundToInt(physical()) % 2 != 0) : physical() >= 0.5f;
            auto thumb = juce::Rectangle<float>(housing.getWidth() - 2, housing.getWidth() - 2)
                             .withCentre({ housing.getCentreX(), selected ? housing.getBottom() - housing.getWidth() * 0.5f : housing.getY() + housing.getWidth() * 0.5f });
            g.setGradientFill(juce::ColourGradient(silver, thumb.getTopLeft(), juce::Colour(0xff505956), thumb.getBottomLeft(), false));
            g.fillEllipse(thumb);
            return;
        }
        if (style == Style::Switch)
        {
            auto lamp = area.reduced(2, 2);
            g.setColour(juce::Colour(0xff151716));
            g.fillRoundedRectangle(lamp.expanded(1), 2);
            const auto active = id == "voice.mode" ? juce::roundToInt(physical()) == 1 : physical() >= 0.5f;
            if (active)
            {
                g.setColour(amber.withAlpha(0.22f));
                g.fillRoundedRectangle(lamp.expanded(2), 3);
            }
            g.setGradientFill(juce::ColourGradient(active ? juce::Colour(0xfffffae5) : juce::Colour(0xff82847e),
                                                   lamp.getTopLeft(), active ? amber : juce::Colour(0xff494c48),
                                                   lamp.getBottomLeft(), false));
            g.fillRoundedRectangle(lamp, 2);
            return;
        }
        if (style == Style::Field || style == Style::Wave || style == Style::Filter || style == Style::Route)
        {
            recessedField(g, area.reduced(1.5f), (style == Style::Wave || style == Style::Filter) ? juce::Colour(0xff464d4a) : displayBlue);
            if (style == Style::Filter)
            {
                const auto mode = juce::roundToInt(physical());
                const auto response = area.reduced(5, 4);
                juce::Path curve;
                if (!filterEnabled)
                {
                    curve.startNewSubPath(response.getX(), response.getCentreY());
                    curve.lineTo(response.getRight(), response.getCentreY());
                }
                else if (mode <= 1)
                {
                    curve.startNewSubPath(response.getX(), response.getY());
                    curve.lineTo(response.getCentreX(), response.getY());
                    curve.cubicTo(response.getCentreX() + 2, response.getY(), response.getCentreX() + 3, response.getBottom(), response.getRight(), response.getBottom());
                }
                else if (mode <= 3)
                {
                    curve.startNewSubPath(response.getBottomLeft());
                    curve.cubicTo(response.getCentreX() - 2, response.getBottom(), response.getCentreX() - 3, response.getY(), response.getCentreX(), response.getY());
                    curve.cubicTo(response.getCentreX() + 3, response.getY(), response.getCentreX() + 2, response.getBottom(), response.getRight(), response.getBottom());
                }
                else
                {
                    curve.startNewSubPath(response.getBottomLeft());
                    curve.cubicTo(response.getCentreX() - 3, response.getBottom(), response.getCentreX() - 2, response.getY(), response.getCentreX(), response.getY());
                    curve.lineTo(response.getTopRight());
                }
                g.setColour(juce::Colour(0xffffda99));
                g.strokePath(curve, juce::PathStrokeType(1.2f));
            }
            else if (style == Style::Wave)
            {
                const auto waveName = formattedValue(*specification, physical()).toLowerCase();
                juce::Path path;
                const auto waveArea = area.reduced(5, 4);
                for (int i = 0; i <= 30; ++i)
                {
                    const auto phase = static_cast<float>(i) / 30;
                    float sample = std::sin(phase * juce::MathConstants<float>::twoPi);
                    if (waveName == "triangle") sample = 1 - 4 * std::abs(phase - 0.5f);
                    else if (waveName == "saw" || waveName == "sawup") sample = 2 * phase - 1;
                    else if (waveName == "sawdown") sample = 1 - 2 * phase;
                    else if (waveName.contains("pulse") || waveName == "square")
                        sample = phase < (waveName == "quarterpulse" ? 0.25f : 0.5f) ? 1.0f : -1.0f;
                    else if (waveName == "noise" || waveName == "samplehold" || waveName == "step")
                        sample = std::sin(phase * 51) * std::cos(phase * 24);
                    else if (waveName == "sawtriangle")
                        sample = (2 * phase - 1 + (1 - 4 * std::abs(phase - 0.5f))) * 0.5f;
                    const auto point = juce::Point<float>(waveArea.getX() + phase * waveArea.getWidth(),
                                                          waveArea.getCentreY() - sample * waveArea.getHeight() * 0.42f);
                    if (i == 0) path.startNewSubPath(point);
                    else path.lineTo(point);
                }
                g.setColour(juce::Colour(0xffffda99));
                g.strokePath(path, juce::PathStrokeType(1.1f));
            }
            else
            {
                auto value = formattedValue(*specification, physical());
                value = value.replace("StepKey", "Step + Key").replace("StepHold", "Step + Hold").replace("StepSequence", "Step Seq").replace("StepChord", "Step Chord");
                if (style == Style::Route)
                {
                    value = value.toUpperCase().replace("FILTER ", "").replace("OSCILLATOR ", "OSC ").replace("SEMITONES", "").replace("NONE", "---");
                }
                if (specification->kind == synth::ParameterKind::Float)
                    value = specification->interval >= 1 ? juce::String(juce::roundToInt(physical()))
                                                         : juce::String(physical(), 1);
                if (id.rfind("arp.step.", 0) == 0 && id.ends_with(".velocity"))
                    value = juce::String(juce::roundToInt(physical() * 127));
                drawText(g, value, getLocalBounds().reduced(3, 0), style == Style::Route ? 8.7f : 10.0f,
                         displayInk, juce::Justification::centred, false, true);
            }
            return;
        }
        if (style == Style::Fader)
        {
            auto track = area.reduced(area.getWidth() * 0.34f, 5);
            g.setColour(juce::Colour(0xff191c1b));
            g.fillRect(track);
            g.setColour(juce::Colour(0xffa3a5a0));
            g.drawVerticalLine(juce::roundToInt(track.getRight()), track.getY(), track.getBottom());
            for (int step = 0; step <= 10; ++step)
            {
                const auto y = track.getBottom() - static_cast<float>(step) * track.getHeight() / 10;
                g.setColour(juce::Colour(0xffb6b9b2));
                g.drawHorizontalLine(juce::roundToInt(y), area.getX() + 1, track.getX() - 2);
                g.drawHorizontalLine(juce::roundToInt(y), track.getRight() + 3, area.getRight() - 1);
            }
            const auto y = track.getBottom() - normalized * track.getHeight();
            auto handle = juce::Rectangle<float>(1, y - 7, area.getWidth() - 2, 14);
            g.setColour(juce::Colours::black.withAlpha(0.75f));
            g.fillRect(handle.translated(1, 2));
            g.setGradientFill(juce::ColourGradient(silver, handle.getTopLeft(), juce::Colour(0xff414846),
                                                   handle.getBottomLeft(), false));
            g.fillRect(handle);
            g.setColour(juce::Colour(0xff151818));
            g.drawHorizontalLine(juce::roundToInt(y), handle.getX(), handle.getRight());
            g.setColour(silver.brighter(0.3f));
            g.drawHorizontalLine(juce::roundToInt(y - 2), handle.getX() + 1, handle.getRight() - 1);
            return;
        }
        const bool small = style == Style::SmallKnob;
        const bool lcdKnob = style == Style::DisplayKnob;
        const auto diameter = std::min(area.getWidth(), area.getHeight() - (small ? 0 : 13)) * (small ? 0.86f : 0.71f);
        const auto centre = juce::Point<float>(area.getCentreX(), small ? area.getCentreY() : (area.getHeight() - 13) * 0.5f);
        const auto radius = diameter * 0.5f;
        if (!small)
        {
            for (int tick = 0; tick <= 40; ++tick)
            {
                const auto angle = juce::MathConstants<float>::pi * (1.22f + 1.56f * static_cast<float>(tick) / 40);
                const auto inner = radius + (tick % 4 == 0 ? 3.1f : 4.8f);
                const auto outer = radius + 6.4f;
                g.setColour((lcdKnob ? displayInk : lettering).withAlpha(tick % 4 == 0 ? 0.9f : 0.64f));
                g.drawLine(centre.x + std::sin(angle) * inner, centre.y - std::cos(angle) * inner,
                           centre.x + std::sin(angle) * outer, centre.y - std::cos(angle) * outer,
                           tick % 4 == 0 ? 1 : 0.7f);
            }
            g.setColour(lcdKnob ? displayInk : lettering);
            g.setFont(font(8.4f));
            g.drawFittedText(label, getLocalBounds().removeFromBottom(12), juce::Justification::centred, 1, 0.7f);
            const auto mid = id.ends_with("phase_degrees") ? "180" : (specification->minimum < 0 ? "0" : "5");
            drawText(g, mid, { juce::roundToInt(centre.x) - 9, 0, 18, 7 }, 6.2f, lcdKnob ? displayInk : lettering);
            drawText(g, specification->minimum < 0 ? "-10" : "0", { 0, juce::roundToInt(centre.y + radius - 1), 13, 8 }, 6.2f, lcdKnob ? displayInk : lettering);
            drawText(g, id.ends_with("phase_degrees") ? "360" : "10", { getWidth() - 14, juce::roundToInt(centre.y + radius - 1), 14, 8 }, 6.2f, lcdKnob ? displayInk : lettering);
        }
        auto disc = juce::Rectangle<float>(diameter, diameter).withCentre(centre);
        g.setColour(juce::Colours::black.withAlpha(0.55f));
        g.fillEllipse(disc.expanded(2).translated(1, 2));
        g.setGradientFill(juce::ColourGradient(lcdKnob ? juce::Colour(0xff789aae) : juce::Colour(0xffeeeeea), disc.getTopLeft(),
                                               lcdKnob ? displayInk : juce::Colour(0xff494e4c), disc.getBottomRight(), false));
        g.fillEllipse(disc);
        g.setColour(juce::Colour(0xff131819));
        g.drawEllipse(disc, 1.2f);
        g.setGradientFill(juce::ColourGradient(lcdKnob ? juce::Colour(0xff809eb2) : (small ? juce::Colour(0xff737d7b) : juce::Colour(0xffd8dfdc)),
                                               disc.getTopLeft(), lcdKnob ? displayInk : juce::Colour(0xff444b48),
                                               disc.getBottomRight(), false));
        g.fillEllipse(disc.reduced(small ? 2 : 4));
        if (!small && !lcdKnob)
        {
            for (int flute = 0; flute < 12; ++flute)
            {
                const auto angle = static_cast<float>(flute) * juce::MathConstants<float>::twoPi / 12;
                g.setColour(juce::Colours::black.withAlpha(0.28f));
                g.drawLine(centre.x + std::sin(angle) * (radius - 1), centre.y - std::cos(angle) * (radius - 1),
                           centre.x + std::sin(angle) * (radius - 5), centre.y - std::cos(angle) * (radius - 5), 2);
            }
        }
        const auto angle = juce::MathConstants<float>::pi * (1.22f + 1.56f * normalized);
        g.setColour(lcdKnob ? juce::Colour(0xffd5e5ed) : (small ? juce::Colour(0xffe5e9e3) : juce::Colour(0xff181f1f)));
        g.drawLine(centre.x + std::sin(angle) * radius * 0.14f, centre.y - std::cos(angle) * radius * 0.14f,
                   centre.x + std::sin(angle) * radius * 0.81f, centre.y - std::cos(angle) * radius * 0.81f,
                   small ? 1.2f : 3.0f);
        paintModulation(g, centre, radius + (small ? 2.5f : 8.5f));
    }
    void mouseDown(const juce::MouseEvent& event) override
    {
        if (parameter == nullptr || specification == nullptr)
            return;
        announce();
        if (event.mods.isCtrlDown() || event.mods.isCommandDown())
        {
            reset();
            return;
        }
        if (event.mods.isPopupMenu())
        {
            showContextMenu();
            return;
        }
        if (isToggleControl() || (specification->kind == synth::ParameterKind::Choice && (style == Style::Field || style == Style::Route || style == Style::Filter)))
        {
            activate();
            return;
        }
        dragStart = normalized;
        dragging = true;
        parameter->beginChangeGesture();
    }
    void mouseDrag(const juce::MouseEvent& event) override
    {
        if (!dragging || parameter == nullptr)
            return;
        const float distance = static_cast<float>(event.getDistanceFromDragStartY());
        const auto sensitivity = event.mods.isShiftDown() ? 0.00035f : 0.0035f;
        auto next = juce::jlimit(0.0f, 1.0f, dragStart - distance * sensitivity);
        if (event.mods.isAltDown() && (style == Style::Knob || style == Style::DisplayKnob || style == Style::SmallKnob))
        {
            const auto offset = event.position - getLocalBounds().toFloat().getCentre();
            auto angle = std::atan2(offset.x, -offset.y);
            if (angle < 0) angle += juce::MathConstants<float>::twoPi;
            // Map the continuous 280-degree arc to the physical parameter range.
            if (angle < juce::MathConstants<float>::pi) angle += juce::MathConstants<float>::twoPi;
            next = juce::jlimit(0.0f, 1.0f, (angle / juce::MathConstants<float>::pi - 1.22f) / 1.56f);
        }
        const auto physicalValue = parameter->convertFrom0to1(next);
        const auto snapped = parameter->convertTo0to1(physicalValue);
        parameter->setValueNotifyingHost(snapped);
        updateValue();
        announce();
    }
    void mouseUp(const juce::MouseEvent& event) override
    {
        if (dragging && parameter != nullptr) parameter->endChangeGesture();
        dragging = false;
        // A plain click on the waveform shows the list of waveforms; dragging up and down still steps through them.
        if (style == Style::Wave && specification != nullptr && specification->kind == synth::ParameterKind::Choice
            && !event.mods.isPopupMenu() && !event.mods.isCtrlDown() && !event.mods.isCommandDown()
            && event.getDistanceFromDragStart() < 4)
            showChoices();
    }
    void mouseDoubleClick(const juce::MouseEvent&) override { reset(); }
    bool keyPressed(const juce::KeyPress& key) override
    {
        if (key == juce::KeyPress::upKey || key == juce::KeyPress::rightKey
            || key == juce::KeyPress::downKey || key == juce::KeyPress::leftKey)
        {
            const auto direction = (key == juce::KeyPress::upKey || key == juce::KeyPress::rightKey) ? 1.0f : -1.0f;
            writePhysical(physical() + direction * (specification->interval > 0 ? specification->interval : (specification->maximum - specification->minimum) * 0.01f));
            return true;
        }
        return false;
    }

    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override
    {
        return std::make_unique<ParameterAccessibilityHandler>(*this);
    }

    // Drag a modulation handle onto a knob to route that source to it (the first free route of the source's slot).
    bool isInterestedInDragSource(const SourceDetails& details) override
    {
        return modulationDestination >= 0 && details.description.toString().startsWith(modulationDragPrefix);
    }
    void itemDragEnter(const SourceDetails&) override { dropHover = true; repaint(); }
    void itemDragExit(const SourceDetails&) override { dropHover = false; repaint(); }
    void itemDropped(const SourceDetails& details) override
    {
        dropHover = false;
        assignModulation(details.description.toString().fromFirstOccurrenceOf(modulationDragPrefix, false, false).getIntValue());
        refreshModulation();
        repaint();
    }

private:
    struct ModulationRoute
    {
        int slot = 0;
        const std::atomic<float>* destination = nullptr;
        const std::atomic<float>* amount = nullptr;
    };
    struct ModulationDepth
    {
        int slot = 0;
        const std::atomic<float>* depth = nullptr;
        float scale = 1.0f;
    };
    // Finds out whether any modulation route can target this control, and keeps cheap handles to those routes.
    void findModulationTarget()
    {
        if (specification == nullptr || style == Style::Switch || style == Style::Selector || style == Style::Slope)
            return;
        const auto& catalog = synth::modulationDestinationCatalog();
        for (std::size_t index = 0; index < catalog.size(); ++index)
            if (catalog[index].nativeIndex >= 0 && catalog[index].targetParameterId == id)
                modulationDestination = static_cast<int>(index) + 1; // choice 0 is "None"
        if (modulationDestination < 0 && id == "filter_control.cutoff_semitones")
            for (std::size_t index = 0; index < catalog.size(); ++index)
                if (catalog[index].destination == synth::ModulationDestination::FilterCutoff)
                    modulationDestination = static_cast<int>(index) + 1; // acts on both parts
        if (modulationDestination < 0)
            return;
        modulationDestinations.push_back(modulationDestination);
        // The general "Filter Cutoff" and "Osc Pitch" routes act on every part, so they show on these knobs too.
        const auto endsWith = [this](const char* suffix) { return juce::String(id).endsWith(suffix); };
        for (std::size_t index = 0; index < catalog.size(); ++index)
            if ((catalog[index].destination == synth::ModulationDestination::FilterCutoff && endsWith(".filter.cutoff_semitones"))
                || (catalog[index].destination == synth::ModulationDestination::OscPitch && endsWith(".note")))
                modulationDestinations.push_back(static_cast<int>(index) + 1);
        auto& state = owner.getValueTreeState();
        for (int slot = 1; slot <= synth::transModSlotCount; ++slot)
        {
            const auto depthId = modulationDepthId(slot, modulationDestination - 1);
            modulationDepths.push_back({ slot, state.getRawParameterValue(depthId), modulationDepthScale(owner, depthId) });
            for (int route = 1; route <= 2; ++route)
            {
                const auto prefix = "transmod." + std::to_string(slot) + ".route." + std::to_string(route) + ".";
                modulationRoutes.push_back({ slot, state.getRawParameterValue(prefix + "destination"),
                                             state.getRawParameterValue(prefix + "amount") });
            }
        }
    }
    void refreshModulation()
    {
        if (modulationRoutes.empty() && modulationDepths.empty())
            return;
        std::array<float, 9> next {};
        for (const auto& entry : modulationDepths)
            if (entry.depth != nullptr && entry.scale > 0.0f)
                next[static_cast<std::size_t>(entry.slot)] += entry.depth->load() / entry.scale;
        for (const auto& route : modulationRoutes)
            if (route.destination != nullptr && route.amount != nullptr
                && std::find(modulationDestinations.begin(), modulationDestinations.end(),
                             juce::roundToInt(route.destination->load())) != modulationDestinations.end())
                next[static_cast<std::size_t>(route.slot)] += route.amount->load();
        if (next != modulationAmounts)
        {
            modulationAmounts = next;
            repaint();
        }
    }
    void paintModulation(juce::Graphics& g, juce::Point<float> centre, float radius) const
    {
        // The control clips what it draws, so the ring is drawn no wider than the control itself.
        radius = std::min(radius, std::min({ centre.x, static_cast<float>(getWidth()) - centre.x, centre.y }) - 2.0f);
        const auto position = [](float value) { return juce::MathConstants<float>::pi * (1.22f + 1.56f * juce::jlimit(0.0f, 1.0f, value)); };
        for (int slot = 1; slot <= synth::transModSlotCount; ++slot)
        {
            const auto amount = modulationAmounts[static_cast<std::size_t>(slot)];
            if (std::abs(amount) < 0.001f)
                continue;
            juce::Path arc;
            arc.addCentredArc(centre.x, centre.y, radius, radius, 0.0f, position(normalized),
                              position(normalized + amount), true);
            g.setColour(modulationColour(slot));
            g.strokePath(arc, juce::PathStrokeType(2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
        if (dropHover)
        {
            g.setColour(juce::Colours::white.withAlpha(0.85f));
            g.drawEllipse(centre.x - radius - 2, centre.y - radius - 2, (radius + 2) * 2, (radius + 2) * 2, 1.5f);
        }
    }
    void assignModulation(int slot)
    {
        if (slot < 1 || slot > synth::transModSlotCount || modulationDestination < 1)
            return;
        // A route that already points here keeps its amount; a new one starts at half.
        if (std::abs(modulationAmount(owner, slot, modulationDestination - 1)) < 0.001f)
            setModulationAmount(owner, slot, modulationDestination - 1, 0.5f);
        else
            activateModulationSlot(owner, slot);
        publishReadout(modulationName(slot), "-> " + getName());
    }
    bool isToggleControl() const noexcept
    {
        return style == Style::Switch || style == Style::Slope || style == Style::Selector;
    }
    float currentPhysicalValue() const
    {
        return parameter != nullptr ? parameter->convertFrom0to1(parameter->getValue()) : 0.0f;
    }
    bool isChecked() const
    {
        const auto value = currentPhysicalValue();
        if (style == Style::Slope) return juce::roundToInt(value) % 2 != 0;
        if (id == "voice.mode") return juce::roundToInt(value) == 1;
        return value >= 0.5f;
    }
    void activate()
    {
        if (!isEnabled() || specification == nullptr) return;
        if (style == Style::Slope) writePhysical(static_cast<float>(juce::roundToInt(currentPhysicalValue()) ^ 1));
        else if (isToggleControl()) setAccessibleValue(isChecked() ? 0.0 : 1.0);
        else if (specification->kind == synth::ParameterKind::Choice) showChoices();
    }
    void setAccessibleValue(double value)
    {
        if (!isEnabled() || parameter == nullptr || specification == nullptr || !std::isfinite(value)) return;
        if (isToggleControl())
        {
            const auto checked = juce::jlimit(0.0, 1.0, value) >= 0.5;
            if (style == Style::Slope)
                writePhysical(static_cast<float>((juce::roundToInt(currentPhysicalValue()) & ~1) + (checked ? 1 : 0)));
            else if (id == "voice.mode") writePhysical(checked ? 1.0f : 2.0f);
            else writePhysical(checked ? 1.0f : 0.0f);
            return;
        }
        value = juce::jlimit(static_cast<double>(specification->minimum), static_cast<double>(specification->maximum), value);
        if (style == Style::Filter) writeFilterEnabled(true);
        writePhysical(static_cast<float>(value));
    }
    void writeFilterEnabled(bool enabled)
    {
        const auto enabledId = id.substr(0, id.size() - 4) + "enabled";
        if (auto* value = owner.getValueTreeState().getParameter(enabledId))
        {
            value->beginChangeGesture();
            value->setValueNotifyingHost(enabled ? 1.0f : 0.0f);
            value->endChangeGesture();
        }
        updateValue();
    }
    void setAccessibleText(const juce::String& input)
    {
        if (!isEnabled() || specification == nullptr) return;
        const auto text = input.trim();
        if (isToggleControl())
        {
            if (text.equalsIgnoreCase("On") || text.equalsIgnoreCase("true")
                || (style == Style::Slope && text.equalsIgnoreCase("24 dB"))
                || (style == Style::Selector && text.equalsIgnoreCase("Slide")))
            {
                setAccessibleValue(1);
                return;
            }
            if (text.equalsIgnoreCase("Off") || text.equalsIgnoreCase("false")
                || (style == Style::Slope && text.equalsIgnoreCase("12 dB"))
                || (style == Style::Selector && text.equalsIgnoreCase("Normal")))
            {
                setAccessibleValue(0);
                return;
            }
        }
        else if (specification->kind == synth::ParameterKind::Choice)
        {
            if (style == Style::Filter)
            {
                if (text.equalsIgnoreCase("Bypass"))
                {
                    writeFilterEnabled(false);
                    return;
                }
                const juce::StringArray types { "Low Pass", "Band Pass", "High Pass" };
                for (int index = 0; index < types.size(); ++index)
                    if (text.equalsIgnoreCase(types[index]))
                    {
                        setAccessibleValue(index * 2 + juce::roundToInt(currentPhysicalValue()) % 2);
                        return;
                    }
            }
            for (std::size_t index = 0; index < specification->choices.size(); ++index)
                if (text.equalsIgnoreCase(juce::String(specification->choices[index])))
                {
                    setAccessibleValue(static_cast<double>(index));
                    return;
                }
        }
        const auto* begin = text.toRawUTF8();
        char* end = nullptr;
        auto value = std::strtod(begin, &end);
        if (end == begin || !std::isfinite(value)) return;
        const auto suffix = juce::String::fromUTF8(end).trim().toLowerCase();
        if (suffix == "%" && (specification->unit == "normalized" || specification->unit == "percent")) value /= 100;
        else if (suffix == "s" && specification->unit == "milliseconds") value *= 1000;
        else if (suffix.isNotEmpty() && !((suffix == "ms" && specification->unit == "milliseconds") || (suffix == "hz" && specification->unit == "Hz") || (suffix == "db" && specification->unit == "dB") || (suffix == "deg" && specification->unit == "degrees"))) return;
        setAccessibleValue(value);
    }
    class ParameterAccessibilityHandler final : public juce::AccessibilityHandler
    {
    public:
        explicit ParameterAccessibilityHandler(ClassicParameterControl& component)
            : AccessibilityHandler(component, component.isToggleControl() ? juce::AccessibilityRole::toggleButton : (component.specification != nullptr && component.specification->kind == synth::ParameterKind::Choice ? juce::AccessibilityRole::comboBox : juce::AccessibilityRole::slider),
                                   actions(component), Interfaces { std::make_unique<ValueInterface>(component) }),
              control(component) {}
        juce::AccessibleState getCurrentState() const override
        {
            auto state = AccessibilityHandler::getCurrentState();
            if (control.isToggleControl())
            {
                state = state.withCheckable();
                if (control.isChecked()) state = state.withChecked();
            }
            return state;
        }

    private:
        class ValueInterface final : public juce::AccessibilityValueInterface
        {
        public:
            explicit ValueInterface(ClassicParameterControl& component) : control(component) {}
            bool isReadOnly() const override { return !control.isEnabled() || control.parameter == nullptr || control.specification == nullptr; }
            double getCurrentValue() const override { return control.isToggleControl() ? (control.isChecked() ? 1.0 : 0.0) : control.currentPhysicalValue(); }
            juce::String getCurrentValueAsString() const override
            {
                if (control.specification == nullptr) return {};
                if (control.style == Style::Slope) return control.isChecked() ? "24 dB" : "12 dB";
                if (control.style == Style::Selector) return control.isChecked() ? "Slide" : "Normal";
                if (control.isToggleControl()) return control.isChecked() ? "On" : "Off";
                if (control.style == Style::Filter
                    && control.owner.getEffectiveParameterValue(control.id.substr(0, control.id.size() - 4) + "enabled") < 0.5f) return "Bypass";
                return control.displayText(control.currentPhysicalValue());
            }
            void setValue(double value) override { control.setAccessibleValue(value); }
            void setValueAsString(const juce::String& value) override { control.setAccessibleText(value); }
            AccessibleValueRange getRange() const override
            {
                if (control.specification == nullptr) return {};
                if (control.isToggleControl()) return { { 0, 1 }, 1 };
                const auto& spec = *control.specification;
                const auto interval = spec.kind == synth::ParameterKind::Choice ? 1.0
                                                                                : (spec.interval > 0 ? spec.interval : (spec.maximum - spec.minimum) * 0.01);
                return { { spec.minimum, spec.maximum }, interval };
            }

        private:
            ClassicParameterControl& control;
        };
        static juce::AccessibilityActions actions(ClassicParameterControl& control)
        {
            const auto safe = juce::Component::SafePointer<ClassicParameterControl>(&control);
            juce::AccessibilityActions result;
            result.addAction(juce::AccessibilityActionType::showMenu, [safe] {
                if (safe != nullptr && safe->isEnabled())
                {
                    if (!safe->isToggleControl() && safe->specification != nullptr
                        && safe->specification->kind == synth::ParameterKind::Choice) safe->showChoices();
                    else safe->showContextMenu();
                }
            });
            if (control.isToggleControl() || (control.specification != nullptr && control.specification->kind == synth::ParameterKind::Choice))
                result.addAction(juce::AccessibilityActionType::press, [safe] { if (safe != nullptr) safe->activate(); });
            if (control.isToggleControl())
                result.addAction(juce::AccessibilityActionType::toggle, [safe] { if (safe != nullptr) safe->activate(); });
            return result;
        }
        ClassicParameterControl& control;
    };
    void announce()
    {
        if (specification != nullptr) publishReadout(specification->name, displayText(physical()));
    }
    void reset()
    {
        if (dragging && parameter != nullptr)
        {
            parameter->endChangeGesture();
            dragging = false;
        }
        if (specification != nullptr)
            writePhysical(synth::initialParameterValue(*specification));
    }
    void showChoices()
    {
        juce::PopupMenu menu;
        if (style == Style::Filter)
        {
            menu.addItem(1000, "Bypass", true, !filterEnabled);
            const std::array<juce::String, 3> filterTypes { "Low Pass", "Band Pass", "High Pass" };
            for (std::size_t i = 0; i < filterTypes.size(); ++i)
                menu.addItem(1010 + static_cast<int>(i), filterTypes[i], true,
                             filterEnabled && juce::roundToInt(physical()) / 2 == static_cast<int>(i));
        }
        else
        {
            for (std::size_t i = 0; i < specification->choices.size(); ++i)
            {
                menu.addItem(static_cast<int>(i) + 1, specification->choices[i], true,
                             juce::roundToInt(physical()) == static_cast<int>(i));
            }
        }
        menu.showMenuAsync(optionsAtMouse(),
                           [safe = juce::Component::SafePointer<ClassicParameterControl>(this)](int result) {
                               if (safe == nullptr || result <= 0) return;
                               if (safe->style == Style::Filter)
                               {
                                   safe->writeFilterEnabled(result != 1000);
                               }
                               if (result >= 1010 && result <= 1012)
                                   safe->writePhysical(static_cast<float>((result - 1010) * 2 + juce::roundToInt(safe->physical()) % 2));
                               else if (result != 1000) safe->writePhysical(static_cast<float>(result - 1));
                               safe->updateValue();
                           });
    }
    void showContextMenu()
    {
        juce::PopupMenu menu;
        if (const auto controller = owner.getLearnedMidiController(id))
            menu.addSectionHeader("Assigned to MIDI CC " + juce::String(*controller));
        menu.addItem(1, "Reset to default");
        menu.addItem(2, "MIDI Learn");
        menu.addItem(3, "Forget MIDI assignment");
        menu.addItem(4, "Enter value...");
        if (specification->kind == synth::ParameterKind::Choice) menu.addItem(5, "Select value...");
        // The host's own menu for this parameter (create or show its automation lane, learn, and so on).
        std::shared_ptr<juce::HostProvidedContextMenu> hostMenu;
        if (auto* editor = findParentComponentOfClass<juce::AudioProcessorEditor>())
            if (auto* context = editor->getHostContext())
                hostMenu = context->getContextMenuForParameter(parameter);
        if (hostMenu != nullptr)
        {
            menu.addSeparator();
            menu.addSubMenu("Automation (host)", hostMenu->getEquivalentPopupMenu());
        }
        menu.showMenuAsync(optionsAtMouse(),
                           [hostMenu, safe = juce::Component::SafePointer<ClassicParameterControl>(this)](int result) {
                               if (safe == nullptr) return;
                               juce::String message;
                               if (result == 1) safe->reset();
                               else if (result == 2) safe->owner.startMidiLearn(safe->id, message);
                               else if (result == 3) safe->owner.forgetMidiControllerForParameter(safe->id, message);
                               else if (result == 4) safe->showValueEditor();
                               else if (result == 5) safe->showChoices();
                               if (message.isNotEmpty()) safe->publishReadout("MIDI", message);
                           });
    }
    void showValueEditor()
    {
        auto ownedWindow = std::make_unique<juce::AlertWindow>("Parameter value", getName(), juce::MessageBoxIconType::NoIcon);
        auto* window = ownedWindow.get();
        window->addTextEditor("value", juce::String(physical(), 3));
        window->addButton("Apply", 1, juce::KeyPress(juce::KeyPress::returnKey));
        window->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
        window->enterModalState(true, juce::ModalCallbackFunction::create([safe = juce::Component::SafePointer<ClassicParameterControl>(this), window](int result) {
                                    if (safe != nullptr && result == 1) safe->writePhysical(window->getTextEditorContents("value").getFloatValue());
                                }),
                                true);
        ownedWindow.release(); // JUCE deletes the window after the modal callback.
    }
    SynthAudioProcessor& owner;
    std::string id;
    juce::String label;
    Style style;
    std::function<void(const juce::String&, const juce::String&)> publishReadout;
    juce::RangedAudioParameter* parameter = nullptr;
    const synth::ParameterSpec* specification = nullptr;
    float normalized = -1;
    float dragStart = 0;
    bool dragging = false;
    bool filterEnabled = true;
    int modulationDestination = -1; // choice index of this control in a route's destination list, or -1
    std::vector<int> modulationDestinations; // the choice indices that act on this control
    std::vector<ModulationRoute> modulationRoutes;
    std::vector<ModulationDepth> modulationDepths;
    std::array<float, 9> modulationAmounts {};
    bool dropHover = false;
};

class ClassicAction final : public juce::Component
{
public:
    ClassicAction(juce::String labelValue, std::function<void()> function, bool displayButton,
                  const juce::String& accessibleTitle, bool selectable, bool menu)
        : label(std::move(labelValue)), action(std::move(function)), lcd(displayButton),
          selectableAction(selectable), menuAction(menu)
    {
        setName(accessibleTitle);
        setTitle(accessibleTitle);
        setDescription(accessibleTitle);
        setWantsKeyboardFocus(true);
        setMouseCursor(juce::MouseCursor::PointingHandCursor);
    }
    void paint(juce::Graphics& g) override
    {
        auto area = getLocalBounds().toFloat().reduced(0.7f);
        g.setColour(juce::Colour(0xff161919));
        g.fillRoundedRectangle(area, 2);
        g.setGradientFill(juce::ColourGradient(active ? juce::Colour(0xfffff8df) : (lcd ? displayBlue : juce::Colour(0xff6a6d69)),
                                               area.getTopLeft(), active ? amber : (lcd ? displayBlue.darker(0.18f) : juce::Colour(0xff353935)),
                                               area.getBottomLeft(), false));
        g.fillRoundedRectangle(area.reduced(1), 2);
        if (label == "list")
        {
            // Three rows with a dot each: the preset list.
            const auto box = area.reduced(3.5f, 3.0f);
            g.setColour(lcd ? displayInk : lettering);
            for (int row = 0; row < 3; ++row)
            {
                const auto y = box.getY() + (box.getHeight() - 1.6f) * static_cast<float>(row) / 2.0f;
                g.fillRect(box.getX(), y, 1.8f, 1.6f);
                g.fillRect(box.getX() + 3.2f, y, box.getWidth() - 3.2f, 1.6f);
            }
            return;
        }
        if (label == "v" || label == "<" || label == ">")
        {
            const auto centre = area.getCentre();
            juce::Path arrow;
            if (label == "v")
            {
                arrow.startNewSubPath(centre.x - 3, centre.y - 1.5f);
                arrow.lineTo(centre.x + 3, centre.y - 1.5f);
                arrow.lineTo(centre.x, centre.y + 2);
            }
            else
            {
                const auto direction = label == "<" ? -1.0f : 1.0f;
                arrow.startNewSubPath(centre.x + direction * 2, centre.y);
                arrow.lineTo(centre.x - direction * 2, centre.y - 3);
                arrow.lineTo(centre.x - direction * 2, centre.y + 3);
            }
            arrow.closeSubPath();
            g.setColour(lcd ? displayInk : lettering);
            g.fillPath(arrow);
            return;
        }
        drawText(g, label, getLocalBounds(), 10, active ? amber.darker(0.4f) : (lcd ? displayInk : lettering),
                 juce::Justification::centred, false, true);
    }
    void mouseDown(const juce::MouseEvent&) override { invokeAction(); }
    bool keyPressed(const juce::KeyPress& key) override
    {
        if (key == juce::KeyPress::returnKey || key == juce::KeyPress::spaceKey)
        {
            requestActivation();
            return true;
        }
        return false;
    }
    void setSelected(bool selected)
    {
        if (active == selected) return;
        active = selected;
        repaint();
        if (auto* handler = getAccessibilityHandler()) handler->notifyAccessibilityEvent(juce::AccessibilityEvent::valueChanged);
    }
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override
    {
        return std::make_unique<ActionAccessibilityHandler>(*this);
    }

private:
    void invokeAction()
    {
        if (!isEnabled()) return;
        auto callback = action;
        callback(); // May destroy this component. The copied callback survives the rebuild.
    }
    void requestActivation()
    {
        const auto safe = juce::Component::SafePointer<ClassicAction>(this);
        // Return from the native accessibility action before rebuilding and deleting its handler.
        juce::MessageManager::callAsync([safe] { if (safe != nullptr) safe->invokeAction(); });
    }
    class ActionAccessibilityHandler final : public juce::AccessibilityHandler
    {
    public:
        explicit ActionAccessibilityHandler(ClassicAction& component)
            : AccessibilityHandler(component, component.selectableAction ? juce::AccessibilityRole::radioButton : juce::AccessibilityRole::button,
                                   actions(component)),
              control(component) {}
        juce::AccessibleState getCurrentState() const override
        {
            auto state = AccessibilityHandler::getCurrentState();
            if (control.selectableAction)
            {
                state = state.withSelectable().withCheckable();
                if (control.active) state = state.withSelected().withChecked();
            }
            return state;
        }

    private:
        static juce::AccessibilityActions actions(ClassicAction& control)
        {
            const auto safe = juce::Component::SafePointer<ClassicAction>(&control);
            const auto activate = [safe] { if (safe != nullptr && safe->isEnabled()) safe->requestActivation(); };
            juce::AccessibilityActions result;
            result.addAction(juce::AccessibilityActionType::press, activate);
            if (control.selectableAction) result.addAction(juce::AccessibilityActionType::toggle, activate);
            if (control.menuAction) result.addAction(juce::AccessibilityActionType::showMenu, activate);
            return result;
        }
        ClassicAction& control;
    };
    juce::String label;
    std::function<void()> action;
    bool lcd = false;
    bool active = false;
    bool selectableAction = false;
    bool menuAction = false;
};

// ---- Modulation displays (experiment) -------------------------------------------------------------------------
// The Mod Env and LFO panels draw what the parameters do and let the curve itself be dragged. The drag handle in each
// panel header routes its source: drop it on any knob that can be a modulation destination.
using ReadoutCallback = std::function<void(const juce::String&, const juce::String&)>;

class ModulationDisplay : public juce::Component
{
public:
    virtual void refresh() = 0;
};

void displayPlate(juce::Graphics& g, juce::Rectangle<float> area)
{
    g.setColour(juce::Colours::black.withAlpha(0.7f));
    g.fillRoundedRectangle(area.expanded(1.5f), 4.0f);
    g.setGradientFill(juce::ColourGradient(juce::Colour(0xff1b2123), area.getTopLeft(),
                                           juce::Colour(0xff0f1314), area.getBottomLeft(), false));
    g.fillRoundedRectangle(area, 3.0f);
}

// Draws and edits attack, decay, sustain and release as one curve. The time axis gives each stage a fixed share of the
// width; a handle moves along its share as the parameter goes from its minimum to its maximum.
class EnvelopeCurveDisplay final : public ModulationDisplay
{
public:
    EnvelopeCurveDisplay(SynthAudioProcessor& processor, const std::string& prefix, int slot, ReadoutCallback readoutCallback)
        : colour(modulationColour(slot)), readout(std::move(readoutCallback))
    {
        const std::array<const char*, 4> suffixes { "attack_ms", "decay_ms", "sustain", "release_ms" };
        for (std::size_t i = 0; i < 4; ++i)
        {
            const auto id = prefix + suffixes[i];
            parameters[i] = processor.getValueTreeState().getParameter(id);
            specifications[i] = synth::findParameterSpec(id);
            cached[i] = parameters[i] != nullptr ? parameters[i]->getValue() : 0.0f;
        }
        setTitle(modulationName(slot) + " envelope");
        setDescription("Drag the three handles to set attack, decay, sustain and release");
    }
    ~EnvelopeCurveDisplay() override { endGestures(); }

    void refresh() override
    {
        for (std::size_t i = 0; i < 4; ++i)
        {
            const auto next = parameters[i] != nullptr ? parameters[i]->getValue() : 0.0f;
            if (std::abs(next - cached[i]) > 0.0000001f)
            {
                cached[i] = next;
                repaint();
            }
        }
    }

    void paint(juce::Graphics& g) override
    {
        displayPlate(g, getLocalBounds().toFloat().reduced(1.0f));
        const auto shape = layout();
        // Each stage is labelled under its own span, and a faint line marks where one stage ends.
        const std::array<const char*, 4> letters { "A", "D", "S", "R" };
        const std::array<const Stage*, 4> stages { &shape.attack_, &shape.decay_, &shape.hold_, &shape.release_ };
        for (std::size_t i = 0; i < 4; ++i)
        {
            const auto left = stages[i]->start.x;
            const auto right = stages[i]->end.x;
            if (i > 0)
            {
                g.setColour(juce::Colours::white.withAlpha(0.08f));
                g.drawVerticalLine(juce::roundToInt(left), shape.plot.getY(), shape.plot.getBottom());
            }
            drawText(g, letters[i], { juce::roundToInt((left + right) * 0.5f) - 6, juce::roundToInt(shape.plot.getBottom()) + 1, 12, 9 },
                     7.0f, lettering.withAlpha(0.5f));
        }
        g.setColour(juce::Colours::white.withAlpha(0.07f));
        g.drawHorizontalLine(juce::roundToInt(shape.plot.getY()), shape.plot.getX(), shape.plot.getRight());
        // The sustain level, as a dotted line across the display.
        g.setColour(colour.withAlpha(0.18f));
        for (float x = shape.plot.getX(); x < shape.plot.getRight(); x += 6.0f)
            g.drawHorizontalLine(juce::roundToInt(shape.hold_.start.y), x, x + 2.5f);

        const auto curve = curvePath(shape);
        juce::Path fill(curve);
        fill.lineTo(shape.release_.end.x, shape.plot.getBottom());
        fill.lineTo(shape.plot.getX(), shape.plot.getBottom());
        fill.closeSubPath();
        g.setGradientFill(juce::ColourGradient(colour.withAlpha(0.32f), 0, shape.plot.getY(),
                                               colour.withAlpha(0.03f), 0, shape.plot.getBottom(), false));
        g.fillPath(fill);
        g.setColour(colour);
        g.strokePath(curve, juce::PathStrokeType(1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        for (int handle = 0; handle < 4; ++handle)
        {
            const auto point = handlePosition(shape, handle);
            const auto active = handle == dragged || handle == hovered;
            g.setColour(juce::Colour(0xff0b0e0f));
            g.fillEllipse(point.x - 5.0f, point.y - 5.0f, 10.0f, 10.0f);
            g.setColour(active ? juce::Colours::white : colour);
            g.fillEllipse(point.x - (active ? 4.0f : 3.2f), point.y - (active ? 4.0f : 3.2f),
                          active ? 8.0f : 6.4f, active ? 8.0f : 6.4f);
        }
    }

    void mouseMove(const juce::MouseEvent& event) override
    {
        const auto next = handleAt(event.position);
        if (next != hovered)
        {
            hovered = next;
            setMouseCursor(next >= 0 ? juce::MouseCursor::UpDownLeftRightResizeCursor : juce::MouseCursor::NormalCursor);
            repaint();
        }
    }
    void mouseExit(const juce::MouseEvent&) override
    {
        hovered = -1;
        repaint();
    }
    void mouseDown(const juce::MouseEvent& event) override
    {
        endGestures();
        dragged = handleAt(event.position);
        if (dragged < 0)
            return;
        for (const auto index : parametersOfHandle(dragged))
            if (parameters[index] != nullptr)
            {
                parameters[index]->beginChangeGesture();
                gestureOpen[index] = true;
            }
        publish(parametersOfHandle(dragged).front());
        repaint();
    }
    void mouseDrag(const juce::MouseEvent& event) override
    {
        if (dragged < 0)
            return;
        const auto shape = layout();
        const auto position = event.position;
        if (dragged == 0)
            setNormalized(0, unstretch((position.x - shape.attack_.start.x) / shape.attack));
        else if (dragged == 1)
            setNormalized(1, unstretch((position.x - shape.attack_.end.x) / shape.decay));
        else if (dragged == 2)
            setNormalized(2, (shape.plot.getBottom() - position.y) / shape.plot.getHeight());
        else
            setNormalized(3, unstretch((position.x - shape.hold_.end.x) / shape.release));
        repaint();
    }
    void mouseUp(const juce::MouseEvent&) override
    {
        endGestures();
        dragged = -1;
        repaint();
    }
    void mouseDoubleClick(const juce::MouseEvent& event) override
    {
        const auto handle = handleAt(event.position);
        if (handle < 0)
            return;
        for (const auto index : parametersOfHandle(handle))
            if (auto* parameter = parameters[index])
            {
                parameter->beginChangeGesture();
                parameter->setValueNotifyingHost(parameter->getDefaultValue());
                parameter->endChangeGesture();
            }
        repaint();
    }

private:
    struct Stage
    {
        juce::Point<float> start;
        juce::Point<float> end;
    };
    struct Layout
    {
        juce::Rectangle<float> plot;
        float attack = 0, decay = 0, hold = 0, release = 0; // the width each stage owns
        Stage attack_, decay_, hold_, release_;
        float sustain = 0;
    };
    // The decay and release curves reach their end in finite time (see Envelope.cpp); k is that fraction of the time constant.
    static constexpr float overshoot = 0.1545f;

    // The time parameters are strongly skewed, so short times would crowd the left of each stage; the square root spreads them.
    // Every stage keeps at least a sixth of its share, so a stage set to zero is still a visible span.
    static constexpr float minimumShare = 0.17f;
    static float stretch(float position) { return minimumShare + (1.0f - minimumShare) * std::sqrt(juce::jlimit(0.0f, 1.0f, position)); }
    static float unstretch(float fraction)
    {
        const auto along = juce::jlimit(0.0f, 1.0f, (fraction - minimumShare) / (1.0f - minimumShare));
        return along * along;
    }
    float normalizedValue(std::size_t index) const { return parameters[index] != nullptr ? parameters[index]->getValue() : 0.0f; }
    Layout layout() const
    {
        Layout result;
        result.plot = getLocalBounds().toFloat().reduced(8.0f, 9.0f).withTrimmedBottom(2.0f);
        const auto width = result.plot.getWidth();
        result.attack = width * 0.24f;
        result.decay = width * 0.28f;
        result.hold = width * 0.14f;
        result.release = width * 0.34f;
        result.sustain = normalizedValue(2);
        const auto bottom = result.plot.getBottom();
        const auto top = result.plot.getY();
        const auto sustainY = bottom - result.sustain * result.plot.getHeight();
        result.attack_ = { { result.plot.getX(), bottom }, { result.plot.getX() + result.attack * stretch(normalizedValue(0)), top } };
        result.decay_ = { result.attack_.end, { result.attack_.end.x + result.decay * stretch(normalizedValue(1)), sustainY } };
        result.hold_ = { result.decay_.end, { result.decay_.end.x + result.hold, sustainY } };
        result.release_ = { result.hold_.end, { result.hold_.end.x + result.release * stretch(normalizedValue(3)), bottom } };
        return result;
    }
    static juce::Point<float> handlePosition(const Layout& shape, int handle)
    {
        if (handle == 2) // the sustain handle sits in the middle of the hold
            return { (shape.hold_.start.x + shape.hold_.end.x) * 0.5f, shape.hold_.start.y };
        return handle == 0 ? shape.attack_.end : handle == 1 ? shape.decay_.end : shape.release_.end;
    }
    // The display draws every join smoothly: the decay settles into the sustain level without a corner, and the release
    // leaves the sustain level the same way. (The sound uses exponential stages; the times are shown on the knobs.)
    juce::Path curvePath(const Layout& shape) const
    {
        const auto height = shape.plot.getHeight();
        const auto bottom = shape.plot.getBottom();
        const auto sustain = shape.sustain;
        const auto k = std::log((1.0f + overshoot) / overshoot);
        juce::Path path;
        path.startNewSubPath(shape.attack_.start);
        constexpr int steps = 20;
        for (int i = 1; i <= steps; ++i)
        {
            const auto u = static_cast<float>(i) / steps;
            const auto level = std::min(1.0f, (1.0f + overshoot) * (1.0f - std::exp(-k * u)));
            path.lineTo(shape.attack_.start.x + (shape.attack_.end.x - shape.attack_.start.x) * u, bottom - level * height);
        }
        for (int i = 1; i <= steps; ++i) // decay: steep at first, level where it meets the sustain
        {
            const auto u = static_cast<float>(i) / steps;
            const auto level = sustain + (1.0f - sustain) * (1.0f - u) * (1.0f - u);
            path.lineTo(shape.decay_.start.x + (shape.decay_.end.x - shape.decay_.start.x) * u, bottom - level * height);
        }
        path.lineTo(shape.hold_.end);
        for (int i = 1; i <= steps; ++i) // release: level at first, then falling away
        {
            const auto u = static_cast<float>(i) / steps;
            const auto level = sustain * (1.0f - u * u);
            path.lineTo(shape.release_.start.x + (shape.release_.end.x - shape.release_.start.x) * u, bottom - level * height);
        }
        return path;
    }
    int handleAt(juce::Point<float> position) const
    {
        const auto shape = layout();
        int best = -1;
        auto bestDistance = 10.0f;
        for (int handle = 0; handle < 4; ++handle)
        {
            const auto distance = position.getDistanceFrom(handlePosition(shape, handle));
            if (distance < bestDistance)
            {
                best = handle;
                bestDistance = distance;
            }
        }
        return best;
    }
    static std::vector<std::size_t> parametersOfHandle(int handle)
    {
        return { static_cast<std::size_t>(handle) }; // attack, decay, sustain and release each have their own handle
    }
    void setNormalized(std::size_t index, float value)
    {
        auto* parameter = parameters[index];
        if (parameter == nullptr)
            return;
        const auto snapped = parameter->convertTo0to1(parameter->convertFrom0to1(juce::jlimit(0.0f, 1.0f, value)));
        parameter->setValueNotifyingHost(snapped);
        cached[index] = parameter->getValue();
        publish(index);
    }
    void publish(std::size_t index)
    {
        if (parameters[index] != nullptr && specifications[index] != nullptr && readout)
            readout(specifications[index]->name, formattedValue(*specifications[index], parameters[index]->convertFrom0to1(parameters[index]->getValue())));
    }
    void endGestures()
    {
        for (std::size_t i = 0; i < 4; ++i)
            if (gestureOpen[i] && parameters[i] != nullptr)
                parameters[i]->endChangeGesture();
        gestureOpen = {};
    }

    std::array<juce::RangedAudioParameter*, 4> parameters {};
    std::array<const synth::ParameterSpec*, 4> specifications {};
    std::array<float, 4> cached {};
    std::array<bool, 4> gestureOpen {};
    juce::Colour colour;
    ReadoutCallback readout;
    int dragged = -1;
    int hovered = -1;
};

// Draws the LFO as it is set (output = gain x (wave + offset)), two cycles for the ordinary waves. The dot on the right
// edge sets the gain, the dot on the left edge the offset, dragging the curve sideways sets the rate, and the two chips pick
// the wave and switch between host sync and free running. With the Step wave the display shows one cycle of steps
// instead: click or drag across the columns to draw the curve, and use the arrows to change how many steps there are.
class LfoCurveDisplay final : public ModulationDisplay
{
public:
    LfoCurveDisplay(SynthAudioProcessor& processor, const std::string& prefix, const std::string& rateId, int slot,
                    ReadoutCallback readoutCallback)
        : owner(processor), idPrefix(prefix), colour(modulationColour(slot)), readout(std::move(readoutCallback))
    {
        auto& state = processor.getValueTreeState();
        const std::array<std::string, 6> ids { prefix + "shape", prefix + "gain", prefix + "offset", rateId, prefix + "free",
                                               prefix + "step_count" };
        for (std::size_t i = 0; i < ids.size(); ++i)
        {
            parameters[i] = state.getParameter(ids[i]);
            specifications[i] = synth::findParameterSpec(ids[i]);
        }
        for (std::size_t i = 0; i < stepParameters.size(); ++i)
            stepParameters[i] = state.getParameter(prefix + "step." + std::to_string(i + 1));
        signature = currentSignature();
        setTitle(modulationName(slot) + " waveform");
        setDescription("Drag the dots to set gain and offset, drag the curve sideways to set the rate");
    }
    ~LfoCurveDisplay() override { endGestures(); }

    void refresh() override
    {
        const auto next = currentSignature();
        if (next != signature)
        {
            signature = next;
            repaint();
        }
    }

    void paint(juce::Graphics& g) override
    {
        displayPlate(g, getLocalBounds().toFloat().reduced(1.0f));
        const auto plot = plotArea();
        const auto centreY = plot.getCentreY();
        const auto half = plot.getHeight() * 0.5f;
        g.setColour(juce::Colours::white.withAlpha(0.09f));
        g.drawHorizontalLine(juce::roundToInt(centreY), plot.getX(), plot.getRight());

        if (stepMode())
            paintSteps(g, plot);
        else
        {
            g.drawVerticalLine(juce::roundToInt(plot.getCentreX()), plot.getY(), plot.getBottom());
            const auto gain = value(1);
            const auto offset = value(2, true);
            const auto points = preview(160);
            juce::Path curve;
            for (std::size_t i = 0; i < points.size(); ++i)
            {
                const auto x = plot.getX() + plot.getWidth() * static_cast<float>(i) / static_cast<float>(points.size() - 1);
                const auto y = centreY - juce::jlimit(-1.0f, 1.0f, gain * (points[i] + offset)) * half;
                if (i == 0) curve.startNewSubPath(x, y); else curve.lineTo(x, y);
            }
            juce::Path fill(curve);
            fill.lineTo(plot.getRight(), centreY);
            fill.lineTo(plot.getX(), centreY);
            fill.closeSubPath();
            g.setColour(colour.withAlpha(0.16f));
            g.fillPath(fill);
            g.setColour(colour);
            g.strokePath(curve, juce::PathStrokeType(1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            for (int dot = 0; dot < 2; ++dot)
            {
                const auto point = dotPosition(dot);
                const auto active = dragMode == (dot == 0 ? Mode::Gain : Mode::Offset);
                g.setColour(juce::Colours::white.withAlpha(0.1f));
                g.drawVerticalLine(juce::roundToInt(point.x), plot.getY(), plot.getBottom());
                g.setColour(juce::Colour(0xff0b0e0f));
                g.fillEllipse(point.x - 4.5f, point.y - 4.5f, 9.0f, 9.0f);
                g.setColour(active ? juce::Colours::white : colour);
                g.fillEllipse(point.x - 3.0f, point.y - 3.0f, 6.0f, 6.0f);
            }
        }

        const auto free = value(4) >= 0.5f;
        drawChip(g, chip(Mode::Shape), shapeName(), false);
        drawChip(g, chip(Mode::Free), free ? "FREE" : "SYNC", free);
        if (stepMode())
            drawChip(g, chip(Mode::StepCount), "<  " + juce::String(stepCount()) + "  >", false);
    }

    void mouseDown(const juce::MouseEvent& event) override
    {
        endGestures();
        dragMode = modeAt(event.position);
        if (dragMode == Mode::None)
            return;
        if (dragMode == Mode::Steps)
        {
            paintStep(event.position);
            return;
        }
        if (const auto index = parameterOf(dragMode); index >= 0 && dragMode != Mode::Shape && dragMode != Mode::Free
            && dragMode != Mode::StepCount && parameters[static_cast<std::size_t>(index)] != nullptr)
        {
            gestureParameter = parameters[static_cast<std::size_t>(index)];
            gestureParameter->beginChangeGesture();
            startValue = gestureParameter->getValue();
            publish(static_cast<std::size_t>(index));
        }
        repaint();
    }
    void mouseDrag(const juce::MouseEvent& event) override
    {
        if (dragMode == Mode::Steps)
        {
            paintStep(event.position);
            return;
        }
        if (gestureParameter == nullptr)
            return;
        const auto plot = plotArea();
        auto next = startValue;
        if (dragMode == Mode::Gain)
            next = (plot.getBottom() - event.position.y) / plot.getHeight();
        else if (dragMode == Mode::Offset)
            next = ((plot.getCentreY() - event.position.y) / (plot.getHeight() * 0.5f) + 1.0f) * 0.5f;
        else if (dragMode == Mode::Rate)
            next = startValue + static_cast<float>(event.getDistanceFromDragStartX()) / (plot.getWidth() * (event.mods.isShiftDown() ? 8.0f : 1.0f));
        const auto snapped = gestureParameter->convertTo0to1(gestureParameter->convertFrom0to1(juce::jlimit(0.0f, 1.0f, next)));
        gestureParameter->setValueNotifyingHost(snapped);
        publish(static_cast<std::size_t>(parameterOf(dragMode)));
        repaint();
    }
    void mouseUp(const juce::MouseEvent& event) override
    {
        const auto mode = dragMode;
        endGestures();
        dragMode = Mode::None;
        repaint();
        if (event.getDistanceFromDragStart() > 3)
            return;
        if (mode == Mode::Free)
        {
            if (auto* parameter = parameters[4])
            {
                parameter->beginChangeGesture();
                parameter->setValueNotifyingHost(parameter->getValue() >= 0.5f ? 0.0f : 1.0f);
                parameter->endChangeGesture();
            }
        }
        else if (mode == Mode::StepCount && parameters[5] != nullptr)
        {
            const auto area = chip(Mode::StepCount);
            const auto delta = event.position.x < area.getCentreX() ? -1 : 1;
            setParameterValue(owner, idPrefix + "step_count", static_cast<float>(juce::jlimit(2, synth::lfoMaxSteps, stepCount() + delta)));
        }
        else if (mode == Mode::Shape && parameters[0] != nullptr && specifications[0] != nullptr)
        {
            juce::PopupMenu menu;
            const auto current = juce::roundToInt(parameters[0]->convertFrom0to1(parameters[0]->getValue()));
            for (std::size_t i = 0; i < specifications[0]->choices.size(); ++i)
                menu.addItem(static_cast<int>(i) + 1, specifications[0]->choices[i], true, static_cast<int>(i) == current);
            menu.showMenuAsync(optionsAtMouse(), [safe = juce::Component::SafePointer<LfoCurveDisplay>(this)](int result) {
                if (result <= 0 || safe == nullptr || safe->parameters[0] == nullptr)
                    return;
                safe->parameters[0]->beginChangeGesture();
                safe->parameters[0]->setValueNotifyingHost(safe->parameters[0]->convertTo0to1(static_cast<float>(result - 1)));
                safe->parameters[0]->endChangeGesture();
            });
        }
    }
    void mouseDoubleClick(const juce::MouseEvent& event) override
    {
        const auto mode = modeAt(event.position);
        const auto index = parameterOf(mode);
        if (index < 0 || mode == Mode::Shape || mode == Mode::Free || mode == Mode::StepCount
            || parameters[static_cast<std::size_t>(index)] == nullptr)
            return;
        auto* parameter = parameters[static_cast<std::size_t>(index)];
        parameter->beginChangeGesture();
        parameter->setValueNotifyingHost(parameter->getDefaultValue());
        parameter->endChangeGesture();
    }

private:
    enum class Mode { None, Shape, Free, Gain, Offset, Rate, Steps, StepCount };
    static int parameterOf(Mode mode)
    {
        switch (mode)
        {
            case Mode::Shape: return 0;
            case Mode::Gain: return 1;
            case Mode::Offset: return 2;
            case Mode::Rate: return 3;
            case Mode::Free: return 4;
            case Mode::StepCount: return 5;
            default: return -1;
        }
    }
    static void drawChip(juce::Graphics& g, juce::Rectangle<float> area, const juce::String& text, bool lit)
    {
        g.setColour(juce::Colour(0xff0b0e0f).withAlpha(0.82f));
        g.fillRoundedRectangle(area, 2.0f);
        drawText(g, text, area.toNearestInt(), 7.5f, lit ? lettering : lettering.withAlpha(0.8f), juce::Justification::centred, lit);
    }
    bool stepMode() const { return shapeName() == "STEP"; }
    int stepCount() const
    {
        return parameters[5] != nullptr ? juce::jlimit(2, synth::lfoMaxSteps, juce::roundToInt(parameters[5]->convertFrom0to1(parameters[5]->getValue()))) : 8;
    }
    juce::Rectangle<float> plotArea() const { return getLocalBounds().toFloat().reduced(14.0f, 6.0f).withTrimmedTop(2.0f); }
    juce::Rectangle<float> chip(Mode mode) const
    {
        const auto plot = plotArea();
        switch (mode)
        {
            case Mode::Shape: return { plot.getX() + 1, 4.0f, 48.0f, 10.0f };
            case Mode::StepCount: return { plot.getRight() - 80, 4.0f, 40.0f, 10.0f };
            default: return { plot.getRight() - 37, 4.0f, 36.0f, 10.0f };
        }
    }
    juce::Point<float> dotPosition(int dot) const
    {
        const auto plot = plotArea();
        return dot == 0 ? juce::Point<float>(plot.getRight() + 7.0f, plot.getBottom() - value(1) * plot.getHeight())
                        : juce::Point<float>(plot.getX() - 7.0f, plot.getCentreY() - value(2, true) * plot.getHeight() * 0.5f);
    }
    Mode modeAt(juce::Point<float> position) const
    {
        if (chip(Mode::Shape).expanded(1.0f).contains(position)) return Mode::Shape;
        if (chip(Mode::Free).expanded(1.0f).contains(position)) return Mode::Free;
        if (stepMode())
        {
            if (chip(Mode::StepCount).expanded(1.0f).contains(position)) return Mode::StepCount;
            return plotArea().expanded(2.0f).contains(position) ? Mode::Steps : Mode::None;
        }
        if (position.getDistanceFrom(dotPosition(0)) <= 8.0f) return Mode::Gain;
        if (position.getDistanceFrom(dotPosition(1)) <= 8.0f) return Mode::Offset;
        return getLocalBounds().contains(position.roundToInt()) ? Mode::Rate : Mode::None;
    }
    // The normalized value of a parameter; bipolar ones are mapped to -1..1.
    float value(std::size_t index, bool bipolar = false) const
    {
        const auto position = parameters[index] != nullptr ? parameters[index]->getValue() : 0.0f;
        return bipolar ? position * 2.0f - 1.0f : position;
    }
    float stepValue(int index) const
    {
        auto* parameter = stepParameters[static_cast<std::size_t>(index)];
        return parameter != nullptr ? parameter->convertFrom0to1(parameter->getValue()) : 0.0f;
    }
    void paintSteps(juce::Graphics& g, juce::Rectangle<float> plot) const
    {
        const auto count = stepCount();
        const auto column = plot.getWidth() / static_cast<float>(count);
        const auto centreY = plot.getCentreY();
        juce::Path outline;
        for (int i = 0; i < count; ++i)
        {
            const auto x = plot.getX() + column * static_cast<float>(i);
            const auto y = centreY - juce::jlimit(-1.0f, 1.0f, stepValue(i)) * plot.getHeight() * 0.5f;
            g.setColour(colour.withAlpha(0.2f));
            g.fillRect(juce::Rectangle<float>(x + 1, std::min(y, centreY), column - 2, std::abs(y - centreY)));
            if (i == 0) outline.startNewSubPath(x, y); else outline.lineTo(x, y);
            outline.lineTo(x + column, y);
        }
        g.setColour(colour);
        g.strokePath(outline, juce::PathStrokeType(1.6f));
    }
    // Sets the step under the mouse; dragging across the columns draws a curve.
    void paintStep(juce::Point<float> position)
    {
        const auto plot = plotArea();
        const auto count = stepCount();
        const auto index = juce::jlimit(0, count - 1, static_cast<int>((position.x - plot.getX()) / (plot.getWidth() / static_cast<float>(count))));
        auto* parameter = stepParameters[static_cast<std::size_t>(index)];
        if (parameter == nullptr)
            return;
        if (openSteps.insert(index).second)
            parameter->beginChangeGesture();
        const auto physical = juce::jlimit(-1.0f, 1.0f, (plot.getCentreY() - position.y) / (plot.getHeight() * 0.5f));
        parameter->setValueNotifyingHost(parameter->convertTo0to1(physical));
        readout("Step " + juce::String(index + 1), juce::String(physical * 100.0f, 0) + " %");
        repaint();
    }
    juce::String shapeName() const
    {
        if (specifications[0] == nullptr || parameters[0] == nullptr)
            return {};
        const auto index = juce::jlimit(0, static_cast<int>(specifications[0]->choices.size()) - 1,
                                        juce::roundToInt(parameters[0]->convertFrom0to1(parameters[0]->getValue())));
        return juce::String(specifications[0]->choices[static_cast<std::size_t>(index)]).toUpperCase();
    }
    std::vector<float> preview(int count) const
    {
        synth::Lfo lfo;
        lfo.prepare(static_cast<double>(count) * 0.5); // 1 Hz, so two cycles fill the display
        lfo.setRateHz(1.0f);
        const auto name = shapeName();
        static const std::map<juce::String, synth::LfoShape> shapes {
            { "SINE", synth::LfoShape::Sine }, { "TRIANGLE", synth::LfoShape::Triangle }, { "SAWUP", synth::LfoShape::SawUp },
            { "SAWDOWN", synth::LfoShape::SawDown }, { "SQUARE", synth::LfoShape::Square },
            { "SAMPLEHOLD", synth::LfoShape::SampleHold }, { "NOISE", synth::LfoShape::Noise }, { "STEP", synth::LfoShape::Step } };
        const auto found = shapes.find(name);
        lfo.setShape(found != shapes.end() ? found->second : synth::LfoShape::Sine);
        lfo.resetPhase();
        std::vector<float> points;
        points.reserve(static_cast<std::size_t>(count));
        points.push_back(lfo.getValue());
        for (int i = 1; i < count; ++i)
            points.push_back(lfo.process());
        return points;
    }
    std::array<float, 8> currentSignature() const
    {
        std::array<float, 8> result {};
        for (std::size_t i = 0; i < parameters.size(); ++i)
            result[i] = parameters[i] != nullptr ? parameters[i]->getValue() : 0.0f;
        for (std::size_t i = 0; i < stepParameters.size(); ++i)
            if (stepParameters[i] != nullptr)
                result[6] += stepParameters[i]->getValue() * static_cast<float>(i + 1);
        result[7] = readParameterValue(owner, idPrefix + "step_smooth");
        return result;
    }
    void publish(std::size_t index)
    {
        if (parameters[index] != nullptr && specifications[index] != nullptr && readout)
            readout(specifications[index]->name, formattedValue(*specifications[index], parameters[index]->convertFrom0to1(parameters[index]->getValue())));
    }
    void endGestures()
    {
        if (gestureParameter != nullptr)
            gestureParameter->endChangeGesture();
        gestureParameter = nullptr;
        for (const auto index : openSteps)
            if (auto* parameter = stepParameters[static_cast<std::size_t>(index)])
                parameter->endChangeGesture();
        openSteps.clear();
    }

    SynthAudioProcessor& owner;
    std::string idPrefix;
    std::array<juce::RangedAudioParameter*, 6> parameters {};
    std::array<const synth::ParameterSpec*, 6> specifications {};
    std::array<juce::RangedAudioParameter*, synth::lfoMaxSteps> stepParameters {};
    std::set<int> openSteps;
    std::array<float, 8> signature {};
    juce::Colour colour;
    ReadoutCallback readout;
    Mode dragMode = Mode::None;
    juce::RangedAudioParameter* gestureParameter = nullptr;
    float startValue = 0.0f;
};

// The dot in a panel header that carries the source: drag it onto a knob to modulate that knob.
class ModulationSourceHandle final : public ModulationDisplay
{
public:
    ModulationSourceHandle(SynthAudioProcessor& processor, int slotNumber) : owner(processor), slot(slotNumber)
    {
        setMouseCursor(juce::MouseCursor::DraggingHandCursor);
        setTitle(modulationName(slot) + " modulation source");
        setDescription("Drag onto a knob to modulate it");
        used = routesInUse();
    }
    void refresh() override
    {
        const auto next = routesInUse();
        if (next != used)
        {
            used = next;
            repaint();
        }
    }
    void paint(juce::Graphics& g) override
    {
        const auto area = getLocalBounds().toFloat().reduced(1.0f);
        const auto colour = modulationColour(slot);
        g.setColour(juce::Colour(0xff0b0e0f));
        g.fillEllipse(area);
        g.setGradientFill(juce::ColourGradient(colour.brighter(0.35f), area.getCentreX(), area.getY(),
                                               colour.darker(0.25f), area.getCentreX(), area.getBottom(), false));
        g.fillEllipse(area.reduced(1.5f));
        for (int i = 0; i < used; ++i) // one pip for each route in use
        {
            g.setColour(juce::Colour(0xff0b0e0f));
            g.fillEllipse(area.getCentreX() - 2.6f + static_cast<float>(i) * 3.4f - (used - 1) * 1.7f, area.getCentreY() - 1.2f, 2.4f, 2.4f);
        }
    }
    void mouseDrag(const juce::MouseEvent& event) override
    {
        if (event.getDistanceFromDragStart() < 4)
            return;
        auto* container = juce::DragAndDropContainer::findParentDragContainerFor(this);
        if (container == nullptr || container->isDragAndDropActive())
            return;
        juce::Image image(juce::Image::ARGB, 16, 16, true);
        {
            juce::Graphics g(image);
            g.setColour(modulationColour(slot).withAlpha(0.9f));
            g.fillEllipse(1.0f, 1.0f, 14.0f, 14.0f);
            g.setColour(juce::Colours::white.withAlpha(0.8f));
            g.drawEllipse(1.0f, 1.0f, 14.0f, 14.0f, 1.0f);
        }
        const juce::Point<int> offset(-8, -8);
        container->startDragging(juce::String(modulationDragPrefix) + juce::String(slot), this, juce::ScaledImage(image), false, &offset);
    }

private:
    int routesInUse() const
    {
        int count = 0;
        for (int route = 1; route <= 2; ++route)
            if (const auto* destination = owner.getValueTreeState().getRawParameterValue(
                    "transmod." + std::to_string(slot) + ".route." + std::to_string(route) + ".destination"))
                count += juce::roundToInt(destination->load()) != 0 ? 1 : 0;
        return count;
    }
    SynthAudioProcessor& owner;
    int slot;
    int used = 0;
};

// The routes of one source (or of the slots that take any source), listed under its panel: destination and amount, with
// as many rows as there are routes. A source reaches a destination through that destination's depth on the source's
// slot, so there is no limit of two targets. Routes saved by older presets (the destination and amount pairs) are listed
// and edited here too. The list of the free slots (twoLine) also shows and changes the source of each route.
class ModulationRouteList final : public ModulationDisplay
{
public:
    ModulationRouteList(SynthAudioProcessor& processor, std::vector<int> slotNumbers, bool twoLineRows, ReadoutCallback readoutCallback)
        : owner(processor), slots(std::move(slotNumbers)), twoLine(twoLineRows), readout(std::move(readoutCallback))
    {
        setTitle(twoLine ? "Other modulation routes" : modulationName(slots.front()) + " routes");
        setDescription("Modulation routes: destination and amount");
        rebuild();
    }
    ~ModulationRouteList() override { endGesture(); }

    void refresh() override
    {
        const auto before = rows;
        const auto amountsBefore = amounts;
        rebuild();
        if (rows != before || amounts != amountsBefore)
            repaint();
    }

    void paint(juce::Graphics& g) override
    {
        displayPlate(g, getLocalBounds().toFloat().reduced(1.0f));
        const auto labelColour = lettering.withAlpha(0.55f);
        drawText(g, juce::String(rows.size()) + (rows.size() == 1 ? " route" : " routes"), { 6, 1, 70, headerHeight }, 7.5f, labelColour,
                 juce::Justification::centredLeft);
        const auto add = addArea();
        g.setColour(juce::Colour(0xff263033));
        g.fillRoundedRectangle(add, 2.0f);
        drawText(g, "+ ADD", add.toNearestInt(), 7.5f, lettering, juce::Justification::centred, true);

        if (rows.empty())
        {
            const juce::String first = twoLine ? "Press + ADD to route" : "Drag the dot above onto";
            const juce::String second = twoLine ? "velocity, key, wheel..." : "a knob, or press + ADD.";
            drawText(g, first, { 4, headerHeight + 16, getWidth() - 8, 12 }, 8.5f, lettering.withAlpha(0.5f));
            drawText(g, second, { 4, headerHeight + 28, getWidth() - 8, 12 }, 8.5f, lettering.withAlpha(0.5f));
            return;
        }
        const auto rowH = rowHeight();
        for (int visible = 0; visible < visibleRows(); ++visible)
        {
            const auto index = firstRow + visible;
            if (index >= static_cast<int>(rows.size()))
                break;
            const auto& row = rows[static_cast<std::size_t>(index)];
            const auto y = headerHeight + visible * rowH;
            if (visible % 2 == 0)
            {
                g.setColour(juce::Colours::white.withAlpha(0.035f));
                g.fillRect(2, y, getWidth() - 4, rowH);
            }
            const auto colour = modulationColour(row.slot);
            const auto lineY = twoLine ? y + 15 : y;
            if (twoLine)
            {
                g.setColour(colour);
                g.fillEllipse(5.0f, static_cast<float>(y + 4), 7.0f, 7.0f);
                drawText(g, sourceLabel(row.slot), { 16, y, getWidth() - 36, 14 }, 8.5f, lettering, juce::Justification::centredLeft, true);
            }
            drawText(g, destinationLabel(row), { 5, lineY, destinationWidth(), 14 }, 8.5f, lettering, juce::Justification::centredLeft);
            const auto amount = amounts[static_cast<std::size_t>(index)];
            const auto track = juce::Rectangle<float>(static_cast<float>(barX()), static_cast<float>(lineY + 4), static_cast<float>(barWidth()), 6.0f);
            g.setColour(juce::Colour(0xff0b0e0f));
            g.fillRoundedRectangle(track, 3.0f);
            g.setColour(juce::Colours::white.withAlpha(0.25f));
            g.drawVerticalLine(juce::roundToInt(track.getCentreX()), track.getY() - 1, track.getBottom() + 1);
            const auto x = track.getCentreX() + amount * track.getWidth() * 0.5f;
            g.setColour(colour);
            g.fillRoundedRectangle(juce::Rectangle<float>(std::min(x, track.getCentreX()), track.getY(), std::abs(x - track.getCentreX()), track.getHeight()), 2.0f);
            g.fillEllipse(x - 3.5f, track.getCentreY() - 3.5f, 7.0f, 7.0f);
            drawText(g, "x", { removeX(), twoLine ? y : lineY, 12, 14 }, 9.5f, lettering.withAlpha(0.6f));
        }
        if (static_cast<int>(rows.size()) > visibleRows())
            drawText(g, "scroll", { getWidth() - 40, getHeight() - 10, 36, 9 }, 6.5f, lettering.withAlpha(0.35f), juce::Justification::centredRight);
    }

    void mouseDown(const juce::MouseEvent& event) override
    {
        endGesture();
        if (addArea().contains(event.position))
        {
            addRoute();
            return;
        }
        const auto index = rowAt(event.position);
        if (index < 0)
            return;
        const auto row = rows[static_cast<std::size_t>(index)];
        const auto x = static_cast<int>(event.position.x);
        const auto lineY = event.position.y - static_cast<float>(headerHeight + (index - firstRow) * rowHeight());
        const auto onSourceLine = twoLine && lineY < 15.0f;
        if (x >= removeX() && (onSourceLine || !twoLine))
            removeRow(row);
        else if (onSourceLine)
        {
            if (row.slot > 4)
                chooseSlotSource(row.slot);
        }
        else if (x >= barX() - 2)
        {
            draggedRow = index;
            beginGesture(row);
            dragAmount(event.position.x);
        }
        else
            chooseDestination(row);
    }
    void mouseDrag(const juce::MouseEvent& event) override
    {
        if (draggedRow >= 0)
            dragAmount(event.position.x);
    }
    void mouseUp(const juce::MouseEvent&) override
    {
        endGesture();
        draggedRow = -1;
    }
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& wheel) override
    {
        const auto limit = std::max(0, static_cast<int>(rows.size()) - visibleRows());
        firstRow = juce::jlimit(0, limit, firstRow + (wheel.deltaY > 0 ? -1 : 1));
        repaint();
    }

private:
    struct Row
    {
        int slot = 0;
        int destination = 0; // index into the destination catalog
        int route = 0;       // 0: a depth; 1 or 2: an older route entry (destination and amount)
        bool operator==(const Row&) const = default;
    };
    static constexpr int headerHeight = 14;

    int rowHeight() const { return twoLine ? 30 : 17; }
    int visibleRows() const { return std::max(1, (getHeight() - headerHeight - 2) / rowHeight()); }
    int barWidth() const { return twoLine ? 56 : 46; }
    int barX() const { return getWidth() - barWidth() - (twoLine ? 8 : 24); }
    int removeX() const { return getWidth() - 16; }
    int destinationWidth() const { return twoLine ? barX() - 8 : barX() - 8; }
    juce::Rectangle<float> addArea() const { return { static_cast<float>(getWidth() - 46), 2.0f, 42.0f, 11.0f }; }
    int rowAt(juce::Point<float> position) const
    {
        if (position.y < headerHeight)
            return -1;
        const auto visible = static_cast<int>((position.y - headerHeight) / rowHeight());
        if (visible < 0 || visible >= visibleRows())
            return -1;
        const auto index = firstRow + visible;
        return index < static_cast<int>(rows.size()) ? index : -1;
    }
    juce::String sourceLabel(int slot) const
    {
        if (slot <= 4)
            return modulationName(slot);
        const auto* spec = synth::findParameterSpec("transmod." + std::to_string(slot) + ".source");
        const auto index = juce::roundToInt(readParameterValue(owner, "transmod." + std::to_string(slot) + ".source"));
        if (spec != nullptr && index > 0 && index < static_cast<int>(spec->choices.size()))
            return juce::String(spec->choices[static_cast<std::size_t>(index)]);
        return "Source " + juce::String(slot - 4);
    }
    juce::String destinationLabel(const Row& row) const
    {
        const auto& catalog = synth::modulationDestinationCatalog();
        return juce::String(catalog[static_cast<std::size_t>(row.destination)].label);
    }
    std::string routeId(const Row& row, const char* suffix) const
    {
        return "transmod." + std::to_string(row.slot) + ".route." + std::to_string(row.route) + "." + suffix;
    }
    float amountOf(const Row& row) const
    {
        if (row.route == 0)
            return modulationAmount(owner, row.slot, row.destination);
        return readParameterValue(owner, routeId(row, "amount"));
    }
    void rebuild()
    {
        rows.clear();
        amounts.clear();
        const auto count = static_cast<int>(synth::modulationDestinationCatalog().size());
        for (const auto slot : slots)
        {
            for (int destination = 0; destination < count; ++destination)
                if (std::abs(modulationAmount(owner, slot, destination)) > 0.0001f)
                    rows.push_back({ slot, destination, 0 });
            for (int route = 1; route <= 2; ++route)
            {
                const auto selection = juce::roundToInt(readParameterValue(owner, "transmod." + std::to_string(slot) + ".route." + std::to_string(route) + ".destination")) - 1;
                if (selection >= 0 && selection < count)
                    rows.push_back({ slot, selection, route });
            }
        }
        for (const auto& row : rows)
            amounts.push_back(amountOf(row));
        firstRow = juce::jlimit(0, std::max(0, static_cast<int>(rows.size()) - visibleRows()), firstRow);
    }
    void beginGesture(const Row& row)
    {
        const auto id = row.route == 0 ? modulationDepthId(row.slot, row.destination) : routeId(row, "amount");
        gestureParameter = owner.getValueTreeState().getParameter(id);
        if (gestureParameter != nullptr)
            gestureParameter->beginChangeGesture();
        activateModulationSlot(owner, row.slot);
    }
    void endGesture()
    {
        if (gestureParameter != nullptr)
            gestureParameter->endChangeGesture();
        gestureParameter = nullptr;
    }
    void dragAmount(float x)
    {
        if (gestureParameter == nullptr || draggedRow < 0 || draggedRow >= static_cast<int>(rows.size()))
            return;
        const auto amount = std::round(juce::jlimit(-1.0f, 1.0f, ((x - static_cast<float>(barX())) / static_cast<float>(barWidth())) * 2.0f - 1.0f) * 100.0f) / 100.0f;
        const auto& row = rows[static_cast<std::size_t>(draggedRow)];
        if (row.route == 0)
        {
            const auto id = modulationDepthId(row.slot, row.destination);
            gestureParameter->setValueNotifyingHost(gestureParameter->convertTo0to1(amount * modulationDepthScale(owner, id)));
        }
        else
            gestureParameter->setValueNotifyingHost(gestureParameter->convertTo0to1(amount));
        amounts[static_cast<std::size_t>(draggedRow)] = amount;
        readout(sourceLabel(row.slot), destinationLabel(row) + "  " + juce::String(juce::roundToInt(amount * 100.0f)) + " %");
        repaint();
    }
    void removeRow(const Row& row)
    {
        if (row.route == 0)
            setParameterValue(owner, modulationDepthId(row.slot, row.destination), 0.0f);
        else
        {
            setParameterValue(owner, routeId(row, "destination"), 0.0f);
            setParameterValue(owner, routeId(row, "amount"), 0.0f);
        }
        rebuild();
        repaint();
    }
    // Every destination, grouped so the list stays short.
    static void buildDestinationMenu(juce::PopupMenu& menu, int idBase, int current)
    {
        const auto& catalog = synth::modulationDestinationCatalog();
        juce::PopupMenu oscillators, filters, parts, lfos;
        std::vector<std::pair<int, juce::String>> general;
        for (int i = 0; i < static_cast<int>(catalog.size()); ++i)
        {
            const juce::String label(catalog[static_cast<std::size_t>(i)].label);
            const auto ticked = i == current;
            if (label.length() > 3 && (label[0] == 'A' || label[0] == 'B') && (label[1] == '1' || label[1] == '2') && label[2] == ' ')
                oscillators.addItem(idBase + i, label, true, ticked);
            else if (label.startsWith("Filter ") && label.length() > 8 && label[7] >= 'A' && label[7] <= 'B')
                filters.addItem(idBase + i, label, true, ticked);
            else if (label.startsWith("Part "))
                parts.addItem(idBase + i, label, true, ticked);
            else if (label.startsWith("LFO "))
                lfos.addItem(idBase + i, label, true, ticked);
            else
                general.emplace_back(i, label);
        }
        for (const auto& [i, label] : general)
            menu.addItem(idBase + i, label, true, i == current);
        menu.addSeparator();
        menu.addSubMenu("Oscillators", oscillators);
        menu.addSubMenu("Filters", filters);
        menu.addSubMenu("Parts", parts);
        menu.addSubMenu("LFOs", lfos);
    }
    void chooseDestination(Row row)
    {
        juce::PopupMenu menu;
        buildDestinationMenu(menu, 1000, row.destination);
        menu.showMenuAsync(optionsAtMouse(), [safe = juce::Component::SafePointer<ModulationRouteList>(this), row](int result) {
            if (result < 1000 || safe == nullptr)
                return;
            safe->moveRow(row, result - 1000);
        });
    }
    void moveRow(const Row& row, int destination)
    {
        if (destination == row.destination)
            return;
        if (row.route == 0)
        {
            const auto amount = modulationAmount(owner, row.slot, row.destination);
            setParameterValue(owner, modulationDepthId(row.slot, row.destination), 0.0f);
            setModulationAmount(owner, row.slot, destination, amount);
        }
        else
            setParameterValue(owner, routeId(row, "destination"), static_cast<float>(destination + 1));
        rebuild();
        repaint();
    }
    // The sources the free slots can take: every source except the four that have their own panel.
    void addSourceItems(juce::PopupMenu& menu, int idBase, int current) const
    {
        const auto* spec = synth::findParameterSpec("transmod.5.source");
        if (spec == nullptr)
            return;
        for (int i = 1; i < static_cast<int>(spec->choices.size()); ++i)
            if (i != fixedModulationSource(1) && i != fixedModulationSource(2) && i != fixedModulationSource(3) && i != fixedModulationSource(4))
                menu.addItem(idBase + i, spec->choices[static_cast<std::size_t>(i)], true, i == current);
    }
    void chooseSlotSource(int slot)
    {
        juce::PopupMenu menu;
        addSourceItems(menu, 0, juce::roundToInt(readParameterValue(owner, "transmod." + std::to_string(slot) + ".source")));
        menu.showMenuAsync(optionsAtMouse(), [safe = juce::Component::SafePointer<ModulationRouteList>(this), slot](int result) {
            if (result <= 0 || safe == nullptr)
                return;
            setParameterValue(safe->owner, "transmod." + std::to_string(slot) + ".source", static_cast<float>(result));
            safe->rebuild();
            safe->repaint();
        });
    }
    void addRoute()
    {
        if (!twoLine)
        {
            juce::PopupMenu destinations;
            buildDestinationMenu(destinations, 1000, -1);
            destinations.showMenuAsync(optionsAtMouse(), [safe = juce::Component::SafePointer<ModulationRouteList>(this)](int chosen) {
                if (chosen < 1000 || safe == nullptr)
                    return;
                safe->addAmount(safe->slots.front(), chosen - 1000);
            });
            return;
        }
        juce::PopupMenu menu;
        addSourceItems(menu, 100, -1);
        menu.showMenuAsync(optionsAtMouse(), [safe = juce::Component::SafePointer<ModulationRouteList>(this)](int result) {
            if (result <= 100 || safe == nullptr)
                return;
            const auto slot = safe->slotForSource(result - 100);
            if (slot <= 0)
            {
                safe->readout("Modulation", "All four free source slots are in use");
                return;
            }
            juce::PopupMenu destinations;
            buildDestinationMenu(destinations, 1000, -1);
            destinations.showMenuAsync(optionsAtMouse(), [safe, slot, source = result - 100](int chosen) {
                if (chosen < 1000 || safe == nullptr)
                    return;
                setParameterValue(safe->owner, "transmod." + std::to_string(slot) + ".source", static_cast<float>(source));
                safe->addAmount(slot, chosen - 1000);
            });
        });
    }
    void addAmount(int slot, int destination)
    {
        setModulationAmount(owner, slot, destination, 0.5f);
        rebuild();
        firstRow = std::max(0, static_cast<int>(rows.size()) - visibleRows());
        repaint();
    }
    // The free slot that already carries this source, or the first one nothing uses; 0 if none is left.
    int slotForSource(int source) const
    {
        int empty = 0;
        for (const auto slot : slots)
        {
            const auto current = juce::roundToInt(readParameterValue(owner, "transmod." + std::to_string(slot) + ".source"));
            if (current == source)
                return slot;
            const auto used = std::any_of(rows.begin(), rows.end(), [slot](const Row& row) { return row.slot == slot; });
            if (empty == 0 && !used)
                empty = slot;
        }
        return empty;
    }

    SynthAudioProcessor& owner;
    std::vector<int> slots;
    bool twoLine;
    ReadoutCallback readout;
    std::vector<Row> rows;
    std::vector<float> amounts;
    int firstRow = 0;
    int draggedRow = -1;
    juce::RangedAudioParameter* gestureParameter = nullptr;
};
} // namespace

class SynthAudioProcessorEditor::ClassicSurface final : public juce::Component, public juce::DragAndDropContainer, private juce::Timer
{
public:
    explicit ClassicSurface(SynthAudioProcessor& processor) : owner(processor)
    {
        setSize(canvasWidth, canvasHeight);
        selectedPart = owner.getSelectedPart();
        buildControls();
        if (const auto* spec = synth::findParameterSpec("master.level_db"))
            readoutValue = formattedValue(*spec, owner.getEffectiveParameterValue("master.level_db"));
        setWantsKeyboardFocus(true);
        startTimerHz(30);
        if (juce::SystemStats::getEnvironmentVariable("SYNTHAX1_OPEN_BROWSER", {}).isNotEmpty()) // development hook
            juce::MessageManager::callAsync([safe = juce::Component::SafePointer<ClassicSurface>(this)] { if (safe != nullptr) safe->openPresetBrowser(); });
    }
    ~ClassicSurface() override
    {
        if (activeKey >= 0) owner.submitUiMidiMessage(juce::MidiMessage::noteOff(1, activeKey));
        endWheelGesture();
    }
    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& event) override;
    void mouseDrag(const juce::MouseEvent& event) override;
    void mouseUp(const juce::MouseEvent&) override;
    // Ctrl+Z undoes and Ctrl+Y (or Ctrl+Shift+Z) redoes parameter edits, without limit.
    bool keyPressed(const juce::KeyPress& key) override
    {
        const auto control = key.getModifiers().isCtrlDown() || key.getModifiers().isCommandDown();
        if (!control)
            return false;
        const auto code = key.getKeyCode();
        const auto shift = key.getModifiers().isShiftDown();
        auto& history = owner.getParameterHistory();
        if ((code == 'Z' || code == 'z') && !shift)
            return history.undo() || true;
        if (code == 'Y' || code == 'y' || ((code == 'Z' || code == 'z') && shift))
            return history.redo() || true;
        return false;
    }
    void setPart(int part)
    {
        const auto next = juce::jlimit(0, 1, part);
        if (next != selectedPart)
        {
            const auto soloActive = owner.getEffectiveParameterValue("layer.1.solo") >= 0.5f
                || owner.getEffectiveParameterValue("layer.2.solo") >= 0.5f;
            if (soloActive)
            {
                writeParameter("layer.1.solo", next == 0 ? 1.0f : 0.0f);
                writeParameter("layer.2.solo", next == 1 ? 1.0f : 0.0f);
            }
        }
        selectedPart = next;
        owner.setSelectedPart(selectedPart);
        buildControls();
        repaint();
    }
    void setPage(int page)
    {
        selectedPage = juce::jlimit(0, 7, page);
        buildControls();
        repaint();
    }
    void configureSnapshot();

private:
    using Style = ClassicParameterControl::Style;
    struct Caption
    {
        juce::String value;
        juce::Rectangle<int> bounds;
        float height;
        juce::Colour colour;
    };
    void buildControls();
    int bindingStateNow() const
    {
        return (owner.getEffectiveParameterValue("global.sync") >= 0.5f ? 1 : 0)
            | (owner.getEffectiveParameterValue("lfo.free") >= 0.5f ? 2 : 0)
            | (owner.getEffectiveParameterValue("lfo.2.free") >= 0.5f ? 4 : 0)
            | (juce::roundToInt(owner.getEffectiveParameterValue("lfo.shape")) == 7 ? 8 : 0)
            | (juce::roundToInt(owner.getEffectiveParameterValue("lfo.2.shape")) == 7 ? 16 : 0);
    }
    void add(const std::string& id, const juce::String& label, Style style, int x, int y, int width, int height);
    void caption(const juce::String& value, int x, int y, int width, int height = 11, float size = 8.7f,
                 juce::Colour colour = lettering);
    ClassicAction* action(const juce::String& value, int x, int y, int width, int height,
                          std::function<void()> callback, bool lcd = false, const juce::String& accessibleTitle = {}, bool selectable = false);
    void envelope(const std::string& prefix, int x, int y, int width);
    static constexpr int modulationPanelWidth = 172;
    static constexpr int modulationPanelTop = 352;
    static int modulationPanelX(int index) { return 31 + index * 176; }
    void modulationEnvelope(const std::string& prefix, int slot, int x);
    void modulationHandle(int slot, int x, int y);
    void addDisplay(std::unique_ptr<ModulationDisplay> display, int x, int y, int width, int height);
    void oscillator(int slot, int x);
    void modulationRoutes(int slot, int x, int y, int width);
    void lfo(int number, int x);
    void lcdControls();
    void sectionMenu(const juce::String& title, const std::vector<std::string>& prefixes);
    void programMenu();
    // The wheels are the parameters performance.pitch_bend and performance.mod_wheel, so they can be automated.
    juce::RangedAudioParameter* wheelParameter(int wheel)
    {
        return owner.getValueTreeState().getParameter(wheel == 0 ? "performance.pitch_bend" : "performance.mod_wheel");
    }
    void endWheelGesture()
    {
        if (draggingWheel < 0)
            return;
        if (auto* parameter = wheelParameter(draggingWheel))
        {
            if (draggingWheel == 0)
                parameter->setValueNotifyingHost(0.5f); // the bend wheel springs back to centre
            parameter->endChangeGesture();
        }
        draggingWheel = -1;
    }
    void openPresetBrowser();
    void closePresetBrowser();
    // Loading a preset rebuilds the controls; the browser must stay in front of them.
    void childrenChanged() override
    {
        if (browserPanel != nullptr && getNumChildComponents() > 0 && getChildComponent(getNumChildComponents() - 1) != browserPanel.get())
            browserPanel->toFront(false);
    }
    std::unique_ptr<juce::Component> browserPanel;
    void renameProgram();
    void loadFile(bool bank);
    void saveFile(bool bank);
    void writeParameter(const std::string& id, float physical);
    void announce(const juce::String& name, const juce::String& value);
    void timerCallback() override;
    void noteAt(juce::Point<float> position);
    int keyboardNoteAt(juce::Point<float> position) const;
    SynthAudioProcessor& owner;
    std::vector<std::unique_ptr<ClassicParameterControl>> controls;
    std::vector<std::unique_ptr<ClassicAction>> actions;
    std::vector<std::unique_ptr<ModulationDisplay>> displays;
    std::vector<Caption> captions;
    std::map<std::string, float> sectionClipboard;
    juce::String clipboardSection;
    std::unique_ptr<juce::FileChooser> fileChooser;
    juce::String readoutName { "Main Volume" };
    juce::String readoutValue;
    int selectedPart = 0;
    int selectedPage = 0;
    int arpPage = 0;
    int activeKey = -1;
    std::uint64_t shownHeld[2] {};
    bool isKeyDown(int note) const noexcept { return activeKey == note || owner.isNoteHeld(note); }
    int draggingWheel = -1;
    int lastVoices = -1;
    int lastVoiceUnits = -1;
    int lastOscillatorSlotVoices = -1;
    int controlBindingState = -1;
    int lastProgramIndex = -1;
    float lastPeakLeft = -1;
    float lastPeakRight = -1;
    float lastPitchWheel = -2;
    float lastModWheel = -1;
    juce::String lastPreset;
};

void SynthAudioProcessorEditor::ClassicSurface::caption(const juce::String& value, int x, int y,
                                                        int width, int height, float size, juce::Colour colour)
{
    captions.push_back({ value, { x, y, width, height }, size, colour });
}
void SynthAudioProcessorEditor::ClassicSurface::add(const std::string& id, const juce::String& label,
                                                    Style style, int x, int y, int width, int height)
{
    auto control = std::make_unique<ClassicParameterControl>(owner, id, label, style,
                                                             [this](const juce::String& name, const juce::String& value) { announce(name, value); });
    control->setBounds(x, y, width, height);
    addAndMakeVisible(*control);
    controls.push_back(std::move(control));
}
ClassicAction* SynthAudioProcessorEditor::ClassicSurface::action(const juce::String& value,
                                                                 int x, int y, int width, int height, std::function<void()> callback, bool lcd, const juce::String& accessibleTitle, bool selectable)
{
    auto control = std::make_unique<ClassicAction>(value, std::move(callback), lcd,
                                                   accessibleTitle.isEmpty() ? value : accessibleTitle, selectable, value == "v" || value == "MENU");
    control->setBounds(x, y, width, height);
    auto* result = control.get();
    addAndMakeVisible(*control);
    actions.push_back(std::move(control));
    return result;
}
void SynthAudioProcessorEditor::ClassicSurface::announce(const juce::String& name, const juce::String& value)
{
    readoutName = name;
    readoutValue = value;
    repaint(371, 178, 247, 20);
}
void SynthAudioProcessorEditor::ClassicSurface::writeParameter(const std::string& id, float physical)
{
    if (auto* parameter = owner.getValueTreeState().getParameter(id))
    {
        parameter->beginChangeGesture();
        parameter->setValueNotifyingHost(parameter->convertTo0to1(physical));
        parameter->endChangeGesture();
    }
}
void SynthAudioProcessorEditor::ClassicSurface::envelope(const std::string& prefix, int x, int y, int width)
{
    const std::array<std::string, 4> suffixes { "attack_ms", "decay_ms", "sustain", "release_ms" };
    const std::array<juce::String, 4> labels { "A", "D", "S", "R" };
    const auto interval = (width - 28) / 4;
    for (int i = 0; i < 4; ++i)
    {
        caption(labels[static_cast<std::size_t>(i)], x + 14 + i * interval, y, interval, 12, 9);
        add(prefix + suffixes[static_cast<std::size_t>(i)], labels[static_cast<std::size_t>(i)], Style::Fader,
            x + 14 + i * interval, y + 13, interval - 3, 72);
    }
    caption("10", x + 3, y + 14, 14, 10, 7);
    caption("0", x + 3, y + 75, 14, 10, 7);
}
void SynthAudioProcessorEditor::ClassicSurface::addDisplay(std::unique_ptr<ModulationDisplay> display, int x, int y, int width, int height)
{
    display->setBounds(x, y, width, height);
    addAndMakeVisible(*display);
    displays.push_back(std::move(display));
}
void SynthAudioProcessorEditor::ClassicSurface::modulationEnvelope(const std::string& prefix, int slot, int x)
{
    addDisplay(std::make_unique<EnvelopeCurveDisplay>(owner, prefix, slot,
                                                      [this](const juce::String& name, const juce::String& value) { announce(name, value); }),
               x + 4, modulationPanelTop + 19, modulationPanelWidth - 8, 88);
    const std::array<std::string, 4> suffixes { "attack_ms", "decay_ms", "sustain", "release_ms" };
    const std::array<juce::String, 4> labels { "ATTACK", "DECAY", "SUSTAIN", "RELEASE" };
    for (std::size_t i = 0; i < 4; ++i)
        add(prefix + suffixes[i], labels[i], Style::Knob, x + 4 + static_cast<int>(i) * 41, modulationPanelTop + 109, 40, 53);
    action("v", x + modulationPanelWidth - 19, modulationPanelTop + 3, 15, 12,
           [this, slot, prefix] { sectionMenu("Modulation Envelope", { prefix, "transmod." + std::to_string(slot) + "." }); }, false,
           "Modulation envelope " + juce::String(slot) + " menu");
    modulationHandle(slot, x + modulationPanelWidth - 38, modulationPanelTop + 2);
}
void SynthAudioProcessorEditor::ClassicSurface::modulationHandle(int slot, int x, int y)
{
    addDisplay(std::make_unique<ModulationSourceHandle>(owner, slot), x, y, 13, 13);
}
void SynthAudioProcessorEditor::ClassicSurface::oscillator(int slot, int x)
{
    const auto prefix = "layer." + std::to_string(selectedPart + 1) + ".osc." + std::to_string(slot) + ".";
    caption("PITCH", x + 20, 47, 65);
    caption("OCTAVE", x + 4, 62, 43);
    caption("NOTE", x + 4, 82, 43);
    caption("FINE", x + 4, 101, 43);
    add(prefix + "octave", "Octave", Style::Field, x + 51, 59, 21, 16);
    add(prefix + "note", "Note", Style::Field, x + 51, 79, 21, 16);
    add(prefix + "fine_cents", "Fine", Style::SmallKnob, x + 33, 104, 21, 22);
    const std::array<std::string, 5> ids { "level", "phase_degrees", "detune", "stereo", "pan" };
    const std::array<juce::String, 5> labels { "VOLUME", "PHASE", "DETUNE", "STEREO", "PAN" };
    for (int i = 0; i < 5; ++i)
        add(prefix + ids[static_cast<std::size_t>(i)], labels[static_cast<std::size_t>(i)], Style::Knob,
            x + 92 + i * 47, 48, 42, 57);
    caption("INV", x + 69, 111, 28);
    add(prefix + "invert", "Invert", Style::Switch, x + 96, 108, 24, 18);
    caption("WAVE", x + 122, 111, 33);
    add(prefix + "waveform", "Wave", Style::Wave, x + 155, 109, 28, 17);
    caption("VOICES", x + 188, 111, 40);
    add(prefix + "voices", "Voices", Style::Field, x + 227, 109, 24, 17);
    caption("RETRIG", x + 252, 111, 43);
    add(prefix + "retrigger", "Retrigger", Style::Switch, x + 295, 107, 25, 20);
    action("v", x + 317, 32, 16, 13, [this, prefix] { sectionMenu("Oscillator", { prefix }); }, false, "Oscillator " + juce::String(selectedPart == 0 ? "A" : "B") + juce::String(slot) + " menu");
}
void SynthAudioProcessorEditor::ClassicSurface::modulationRoutes(int slot, int x, int y, int width)
{
    const auto prefix = "transmod." + std::to_string(slot) + ".route.";
    for (int route = 1; route <= 2; ++route)
    {
        const auto routePrefix = prefix + std::to_string(route) + ".";
        add(routePrefix + "amount", "Amount", Style::SmallKnob, x + 8, y + (route - 1) * 24, 20, 21);
        add(routePrefix + "destination", "Destination", Style::Route, x + 32, y + 4 + (route - 1) * 24, width - 48, 16);
    }
}
void SynthAudioProcessorEditor::ClassicSurface::lfo(int number, int x)
{
    const auto prefix = number == 1 ? std::string("lfo.") : std::string("lfo.2.");
    const auto synchronized = owner.getEffectiveParameterValue("global.sync") >= 0.5f
        && owner.getEffectiveParameterValue(prefix + "free") < 0.5f;
    const auto rateId = prefix + (synchronized ? "sync_division" : "rate_hz");
    const auto stepShape = juce::roundToInt(owner.getEffectiveParameterValue(prefix + "shape")) == 7;
    addDisplay(std::make_unique<LfoCurveDisplay>(owner, prefix, rateId, number + 2,
                                                 [this](const juce::String& name, const juce::String& value) { announce(name, value); }),
               x + 4, modulationPanelTop + 19, modulationPanelWidth - 8, 80);
    const auto y = modulationPanelTop + 101;
    add(rateId, "RATE", Style::Knob, x + 4, y, 40, 53);
    add(prefix + "gain", "GAIN", Style::Knob, x + 45, y, 40, 53);
    add(prefix + "offset", "OFFSET", Style::Knob, x + 86, y, 40, 53);
    if (stepShape)
        add(prefix + "step_smooth", "SMOOTH", Style::Knob, x + 127, y, 40, 53);
    else
        add(prefix + "phase_degrees", "PHASE", Style::Knob, x + 127, y, 40, 53);
    action("v", x + modulationPanelWidth - 19, modulationPanelTop + 3, 15, 12,
           [this, prefix, number] { sectionMenu("LFO", { prefix, "transmod." + std::to_string(number + 2) + "." }); }, false, "LFO " + juce::String(number) + " menu");
    modulationHandle(number + 2, x + modulationPanelWidth - 38, modulationPanelTop + 2);
}

void SynthAudioProcessorEditor::ClassicSurface::buildControls()
{
    lastProgramIndex = owner.getCurrentProgram();
    controlBindingState = bindingStateNow();
    controls.clear();
    actions.clear();
    displays.clear();
    captions.clear();
    caption("Polyphony", 57, 5, 73, 18, 12);
    add("voice.polyphony", "Polyphony", Style::Field, 132, 6, 22, 16);
    caption("Voices", 188, 4, 59, 18, 12);
    caption("Part Select", 326, 4, 83, 18, 12);
    action("PART A", 408, 5, 44, 18, [this] { setPart(0); }, false, "Part A", true)->setSelected(selectedPart == 0);
    action("PART B", 455, 5, 44, 18, [this] { setPart(1); }, false, "Part B", true)->setSelected(selectedPart == 1);
    caption("Solo", 533, 4, 35, 18, 12);
    add("layer." + std::to_string(selectedPart + 1) + ".solo", "Solo", Style::Switch, 569, 4, 22, 19);
    caption("Sync", 617, 4, 39, 18, 12);
    add("global.sync", "Sync", Style::Switch, 656, 3, 24, 21);
    oscillator(1, 32);
    oscillator(2, 538);
    const auto layer = "layer." + std::to_string(selectedPart + 1) + ".";
    envelope(layer + "amp_env.", 376, 50, 154);
    action("v", 512, 33, 16, 12, [this, layer] { sectionMenu("Amplifier Envelope", { layer + "amp_env." }); }, false, "Amplifier envelope " + juce::String(selectedPart == 0 ? "A" : "B") + " menu");

    caption("OSC A", 39, 152, 33, 13, 8, amber);
    caption("OSC B", 39, 167, 33, 13, 8);
    caption("FILTER", 99, 152, 42, 13, 8, amber);
    caption("INPUT SELECT", 37, 177, 73);
    add(layer + "filter.input", "Input", Style::Field, 109, 175, 25, 17);
    caption("FILTER TYPE", 145, 161, 76);
    add(layer + "filter.mode", "Filter Type", Style::Filter, 162, 176, 30, 17);
    caption("12 dB", 207, 171, 34, 12, 8);
    add(layer + "filter.mode", "Slope", Style::Slope, 215, 186, 20, 30);
    caption("24 dB", 207, 216, 34, 12, 8);
    add(layer + "filter.cutoff_semitones", "CUTOFF", Style::Knob, 39, 198, 44, 53);
    add(layer + "filter.resonance", "RESONANCE", Style::Knob, 95, 198, 45, 53);
    add(layer + "filter.drive", "DRIVE", Style::Knob, 156, 198, 44, 53);
    action("v", 229, 137, 15, 12, [this, layer] { sectionMenu("Filter", { layer + "filter." }); }, false, "Filter " + juce::String(selectedPart == 0 ? "A" : "B") + " menu");
    add("filter_control.cutoff_semitones", "CUTOFF", Style::Knob, 43, 279, 52, 66);
    add("filter_control.resonance", "RESONANCE", Style::Knob, 102, 279, 52, 66);
    caption("WARM", 153, 287, 43, 10, 8);
    caption("DRIVE", 153, 296, 43, 10, 8);
    add("filter_control.warm_drive", "Warm Drive", Style::Switch, 162, 309, 23, 20);
    add("filter_control.keytrack", "KEY TRACK", Style::Knob, 193, 293, 45, 52);

    add("layer.1.level_db", "Mix A", Style::Fader, 681, 170, 30, 79);
    add("layer.2.level_db", "Mix B", Style::Fader, 711, 170, 30, 79);
    add("master.level_db", "Main Volume", Style::Fader, 759, 170, 30, 79);
    caption("MIX A", 674, 152, 43);
    caption("MIX B", 712, 152, 40);
    caption("MAIN VOL", 752, 152, 55);
    caption("LEFT", 808, 152, 30);
    caption("RIGHT", 839, 152, 32);

    modulationEnvelope("mod_env.", 1, modulationPanelX(0));
    modulationEnvelope("mod_env.2.", 2, modulationPanelX(1));
    lfo(1, modulationPanelX(2));
    lfo(2, modulationPanelX(3));
    const auto readout = [this](const juce::String& name, const juce::String& value) { announce(name, value); };
    for (int slot = 1; slot <= 4; ++slot)
        addDisplay(std::make_unique<ModulationRouteList>(owner, std::vector<int> { slot }, false, readout),
                   modulationPanelX(slot - 1) + 3, modulationPanelTop + 171, modulationPanelWidth - 6, 68);
    addDisplay(std::make_unique<ModulationRouteList>(owner, std::vector<int> { 5, 6, 7, 8 }, true, readout),
               modulationPanelX(4) + 3, modulationPanelTop + 19, modulationPanelWidth - 6 - 29, 220);
    constexpr int low = modulationExtra;
    caption("BEND", 33, 518 + low, 34, 11, 8);
    caption("RANGE", 32, 529 + low, 35, 11, 8);
    add("voice.pitch_bend_range", "Bend Range", Style::Field, 37, 546 + low, 22, 17);
    caption("MONO", 797, 509 + low, 46, 10, 8);
    caption("LEGATO", 796, 519 + low, 48, 10, 8);
    add("voice.mode", "Mono Legato", Style::Switch, 843, 507 + low, 28, 23);
    add("voice.glide_ms", "PORTAMENTO", Style::Knob, 796, 533 + low, 50, 53);
    caption("MODE", 846, 530 + low, 28);
    add("voice.portamento_mode", "Portamento Mode", Style::Selector, 850, 544 + low, 15, 27);
    caption("N", 865, 543 + low, 9, 12, 8);
    caption("S", 865, 558 + low, 9, 12, 8);
    lcdControls();
    if (auto* handler = getAccessibilityHandler()) handler->notifyAccessibilityEvent(juce::AccessibilityEvent::structureChanged);
}

void SynthAudioProcessorEditor::ClassicSurface::lcdControls()
{
    for (int bank = 0; bank < 4; ++bank)
        action(juce::String(bank + 1), 286 + bank * 18, 160, 17, 18, [this, bank] {
            juce::String message;
            owner.selectProgram(bank * 128 + owner.getCurrentProgram() % 128, message);
            announce("Program", message); }, true, "Program bank " + juce::String(bank + 1), true)->setSelected(owner.getCurrentProgram() / 128 == bank);
    action("<", 591, 160, 15, 18, [this] { juce::String message; owner.previousProgram(message); announce("Program", message); }, true, "Previous program");
    action(">", 607, 160, 15, 18, [this] { juce::String message; owner.nextProgram(message); announce("Program", message); }, true, "Next program");
    action("MENU", 286, 180, 85, 17, [this] { programMenu(); }, true, "Program menu");
    action("list", 569, 161, 17, 16, [this] { openPresetBrowser(); }, true, "Preset browser");
    const std::array<juce::String, 8> names { "ARPEG", "DISTORT", "PHASER", "CHORUS", "EQ", "DELAY", "REVERB", "COMPRESS" };
    const std::array<juce::String, 8> pageTitles { "Arpeggiator", "Distortion", "Phaser", "Chorus", "Equalizer", "Delay", "Reverb", "Compressor" };
    const std::array<std::string, 8> enabled { "arp.enabled", "fx.saturation_enabled", "fx.phaser_enabled", "fx.chorus_enabled", "fx.eq_enabled", "fx.delay_enabled", "fx.reverb_enabled", "fx.compressor_enabled" };
    for (int page = 0; page < 8; ++page)
    {
        add(enabled[static_cast<std::size_t>(page)], "Enable", Style::Switch, 287, 198 + page * 16, 14, 15);
        action(names[static_cast<std::size_t>(page)], 302, 198 + page * 16, 69, 15, [this, page] { setPage(page); }, true, pageTitles[static_cast<std::size_t>(page)] + " page", true)->setSelected(selectedPage == page);
    }
    const std::array<std::vector<std::string>, 8> pagePrefixes {
        std::vector<std::string> { "arp." }, { "fx.saturation_", "fx.distortion_mode" }, { "fx.phaser_" }, { "fx.chorus_" }, { "fx.eq_" }, { "fx.delay_" }, { "fx.reverb_" }, { "fx.compressor_" }
    };
    action("v", 607, 200, 13, 12, [this, names, pagePrefixes] { sectionMenu(names[static_cast<std::size_t>(selectedPage)], pagePrefixes[static_cast<std::size_t>(selectedPage)]); }, true, pageTitles[static_cast<std::size_t>(selectedPage)] + " menu");
    const auto field = [this](const std::string& id, const juce::String& label, int y, int width = 82) {
        caption(label, 376, y + 2, 51, 12, 8, displayInk);
        add(id, label, Style::Field, 432, y, width, 17);
    };
    const auto knob = [this](const std::string& id, const juce::String& label, int x, int y) {
        add(id, label, Style::DisplayKnob, x, y, 46, 53);
    };
    if (selectedPage == 0)
    {
        field("arp.mode", "MODE", 209, 66);
        field("arp.velocity_mode", "VELOCITY", 226, 66);
        field("arp.octaves", "OCTAVE", 243, 66);
        field("arp.wrap", "WRAP", 260, 66);
        // Three knobs share the LCD's right-hand strip (x 500..620), so they are 40 px wide here.
        const auto arpKnob = [this](const std::string& id, const juce::String& label, int x) {
            add(id, label, Style::DisplayKnob, x, 211, 40, 53);
        };
        arpKnob("arp.rate", "TIME", 500); // always a note division, 1/1 to 1/64
        arpKnob("arp.gate", "GATE", 540);
        arpKnob("arp.swing", "SWING", 580);
        caption("PAGE", 537, 268, 33, 11, 8, displayInk);
        action(juce::String(arpPage + 1), 577, 267, 24, 13, [this] { arpPage = 1 - arpPage; buildControls(); repaint(); }, true, "Arpeggiator pattern page " + juce::String(arpPage + 1));
        caption("HOLD", 392, 280, 41, 11, 8, displayInk);
        caption("TRANSPOSE", 375, 293, 58, 11, 8, displayInk);
        caption("VELOCITY", 378, 307, 55, 11, 8, displayInk);
        for (int step = 0; step < 8; ++step)
        {
            const auto prefix = "arp.step." + std::to_string(arpPage * 8 + step + 1) + ".";
            const auto x = 439 + step * 22;
            add(prefix + "tie", "Hold", Style::Switch, x, 279, 22, 13);
            add(prefix + "pitch_semitones", "Transpose", Style::Field, x, 293, 22, 13);
            add(prefix + "velocity", "Velocity", Style::Field, x, 307, 22, 13);
        }
    }
    else
    {
        std::vector<std::pair<std::string, juce::String>> parameters;
        std::vector<std::pair<std::string, juce::String>> fields;
        if (selectedPage == 1)
        {
            fields = { { "fx.distortion_mode", "TYPE" } };
            parameters = { { "fx.saturation_drive", "AMOUNT" }, { "fx.saturation_mix", "DRY/WET" } };
        }
        else if (selectedPage == 2)
            parameters = { { "fx.phaser_center_hz", "CENTER" }, { "fx.phaser_spread", "SPREAD" }, { owner.getEffectiveParameterValue("global.sync") >= 0.5f ? "fx.phaser_sync_division" : "fx.phaser_rate_hz", "RATE" }, { "fx.phaser_depth", "GAIN" }, { "fx.phaser_feedback", "FEEDBACK" }, { "fx.phaser_mix", "DRY/WET" }, { "fx.phaser_lr_offset", "OFFSET" }, { "fx.phaser_width", "WIDTH" } };
        else if (selectedPage == 3)
            parameters = { { "fx.chorus_delay_ms", "DELAY" }, { owner.getEffectiveParameterValue("global.sync") >= 0.5f ? "fx.chorus_sync_division" : "fx.chorus_rate_hz", "RATE" }, { "fx.chorus_depth_ms", "DEPTH" }, { "fx.chorus_feedback", "FEEDBACK" }, { "fx.chorus_mix", "DRY/WET" }, { "fx.chorus_width", "WIDTH" } };
        else if (selectedPage == 4)
            parameters = { { "fx.eq_low_frequency_hz", "BASS FREQ" }, { "fx.eq_low_gain_db", "BASS" }, { "fx.eq_high_frequency_hz", "TREBLE FREQ" }, { "fx.eq_high_gain_db", "TREBLE" } };
        else if (selectedPage == 5)
        {
            const auto synchronized = owner.getEffectiveParameterValue("global.sync") >= 0.5f;
            fields = { { synchronized ? "fx.delay_sync_division" : "fx.delay_time_left_ms", "LEFT" },
                       { synchronized ? "fx.delay_right_sync_division" : "fx.delay_time_right_ms", "RIGHT" } };
            parameters = { { "fx.delay_low_cut_hz", "LOW CUT" }, { "fx.delay_high_cut_hz", "HIGH CUT" }, { "fx.delay_feedback", "FEEDBACK" }, { "fx.delay_mix", "DRY/WET" }, { "fx.delay_smear", "SMEAR" }, { "fx.delay_spread", "SPREAD" }, { "fx.delay_width", "WIDTH" } };
        }
        else if (selectedPage == 6)
            parameters = { { "fx.reverb_pre_delay_ms", "PREDELAY" }, { "fx.reverb_decay", "SIZE" }, { "fx.reverb_damp", "DAMP" }, { "fx.reverb_width", "WIDTH" }, { "fx.reverb_mix", "DRY/WET" } };
        else
            parameters = { { "fx.compressor_attack_ms", "ATTACK" }, { "fx.compressor_release_ms", "RELEASE" }, { "fx.compressor_threshold_db", "THRESHOLD" }, { "fx.compressor_ratio", "RATIO" }, { "fx.compressor_makeup_db", "GAIN" }, { "fx.compressor_mix", "DRY/WET" } };
        for (std::size_t i = 0; i < fields.size(); ++i)
        {
            if (selectedPage == 5)
            {
                const auto x = 376 + static_cast<int>(i) * 123;
                caption(fields[i].second, x, 202, 31, 12, 8, displayInk);
                add(fields[i].first, fields[i].second, Style::Field, x + 33, 201, 74, 17);
            }
            else field(fields[i].first, fields[i].second, 210 + static_cast<int>(i) * 20, 86);
        }
        const auto top = selectedPage == 5 ? 219 : (fields.empty() ? 213 : 249);
        const auto columns = selectedPage == 5 ? 4 : (fields.empty() ? 4 : 5);
        const auto spacing = selectedPage == 5 ? 58 : (fields.empty() ? 58 : 47);
        for (std::size_t i = 0; i < parameters.size(); ++i)
        {
            const auto x = 378 + static_cast<int>(i % static_cast<std::size_t>(columns)) * spacing;
            const auto y = top + static_cast<int>(i / static_cast<std::size_t>(columns)) * 55;
            if (selectedPage == 5) add(parameters[i].first, parameters[i].second, Style::DisplayKnob, x, y, 46, 49);
            else knob(parameters[i].first, parameters[i].second, x, y);
        }
        if (selectedPage == 3)
        {
            caption("DUAL", 571, 291, 35, 12, 8, displayInk);
            add("fx.chorus_dual_mode", "Dual", Style::Switch, 582, 306, 22, 16);
        }
        if (selectedPage == 5)
        {
            caption("PING PONG", 548, 303, 69, 12, 8, displayInk);
            add("fx.delay_ping_pong", "Ping Pong", Style::Switch, 573, 280, 22, 16);
        }
    }
}

void SynthAudioProcessorEditor::ClassicSurface::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff191c1c));
    // Deterministic drawn wood grain. No reference pixels or external assets are shipped.
    for (const int x : { 0, 882 })
    {
        const auto rail = juce::Rectangle<int>(x, 0, 26, canvasHeight);
        g.setGradientFill(juce::ColourGradient(juce::Colour(0xff512e1b), static_cast<float>(x), 0,
                                               juce::Colour(0xffbb8050), static_cast<float>(x + 17), 0, false));
        g.fillRect(rail);
        for (int line = 0; line < 29; ++line)
        {
            juce::Path grain;
            for (int y = 0; y <= canvasHeight; y += 5)
            {
                const auto offset = static_cast<float>(line) * 0.83f + std::sin(static_cast<float>(y) * 0.013f + static_cast<float>(line) * 1.7f) * 1.1f;
                if (y == 0) grain.startNewSubPath(static_cast<float>(x) + offset, 0);
                else grain.lineTo(static_cast<float>(x) + offset, static_cast<float>(y));
            }
            g.setColour((line % 3 == 0 ? juce::Colour(0xff3d281d) : juce::Colour(0xffe1a46c)).withAlpha(0.35f));
            g.strokePath(grain, juce::PathStrokeType(line % 3 == 0 ? 0.7f : 0.4f));
        }
        g.setColour(juce::Colour(0xff1d1612));
        g.drawVerticalLine(x + 25, 0, static_cast<float>(canvasHeight));
        g.setColour(juce::Colour(0xffe4b085).withAlpha(0.5f));
        g.drawVerticalLine(x + 2, 0, static_cast<float>(canvasHeight));
    }
    g.setGradientFill(juce::ColourGradient(juce::Colour(0xff3d4040), 0, 0,
                                           juce::Colour(0xff181b1b), 0, 28, false));
    g.fillRect(27, 0, 854, 28);
    g.setColour(juce::Colours::black);
    g.drawHorizontalLine(27, 27, 881);
    drawText(g, "SYNTHAX1", { 722, 3, 147, 23 }, 15, silver, juce::Justification::centredRight);
    panel(g, { 31, 30, 340, 101 }, "OSCILLATOR " + juce::String(selectedPart == 0 ? "A1" : "B1"), true, selectedPart);
    panel(g, { 375, 30, 159, 101 }, "AMP ENV " + juce::String(selectedPart == 0 ? "A" : "B"), true, selectedPart);
    panel(g, { 538, 30, 340, 101 }, "OSCILLATOR " + juce::String(selectedPart == 0 ? "A2" : "B2"), true, selectedPart);
    panel(g, { 31, 134, 219, 121 }, "FILTER " + juce::String(selectedPart == 0 ? "A" : "B"), true, selectedPart);
    panel(g, { 31, 257, 219, 91 }, "FILTER CONTROL");
    panel(g, { 658, 134, 220, 129 }, "MIXER");
    panel(g, { modulationPanelX(0), modulationPanelTop, modulationPanelWidth, 242 }, "MOD ENV 1");
    panel(g, { modulationPanelX(1), modulationPanelTop, modulationPanelWidth, 242 }, "MOD ENV 2");
    panel(g, { modulationPanelX(2), modulationPanelTop, modulationPanelWidth, 242 }, "LFO 1");
    panel(g, { modulationPanelX(3), modulationPanelTop, modulationPanelWidth, 242 }, "LFO 2");
    panel(g, { modulationPanelX(4), modulationPanelTop, 878 - modulationPanelX(4) + 1, 242 }, "MISC");

    // The LCD bezel has nested machined edges surrounding the pale blue screen.
    auto bezel = juce::Rectangle<float>(255, 134, 399, 214);
    g.setColour(juce::Colour(0xff111515));
    g.fillRoundedRectangle(bezel, 14);
    g.setGradientFill(juce::ColourGradient(juce::Colour(0xff565c5c), bezel.getTopLeft(),
                                           juce::Colour(0xff151b1c), bezel.getBottomRight(), false));
    g.fillRoundedRectangle(bezel.reduced(7), 9);
    g.setColour(juce::Colour(0xff111415));
    g.drawRoundedRectangle(bezel.reduced(11), 7, 3);
    g.setColour(juce::Colour(0xff959d98));
    g.drawRoundedRectangle(bezel.reduced(16), 4, 2);
    recessedField(g, { 281, 156, 347, 172 }, displayBlue);
    g.setColour(displayInk.withAlpha(0.55f));
    g.drawRect(286, 160, 337, 164, 1);
    g.drawVerticalLine(372, 179, 324);
    g.drawHorizontalLine(197, 286, 623);
    recessedField(g, { 358, 160, 232, 18 }, juce::Colour(0xff638ba0));
    const auto program = owner.getCurrentProgram();
    drawText(g, juce::String(program % 128 + 1).paddedLeft('0', 3) + ": " + owner.getCurrentPresetName(),
             { 361, 160, 229, 18 }, 12, juce::Colour(0xffe1edf1), juce::Justification::centred, true);
    drawText(g, readoutName, { 376, 180, 181, 17 }, 11, displayInk, juce::Justification::centredLeft);
    drawText(g, readoutValue, { 554, 180, 64, 17 }, 11, displayInk, juce::Justification::centredRight);

    const auto diagnostics = owner.getDiagnosticsSnapshot();
    recessedField(g, { 242, 6, 53, 16 });
    drawText(g, juce::String(diagnostics.activeVoices * diagnostics.patchCost.oscillatorSlotVoices) + " / " + juce::String(diagnostics.patchCost.voiceUnits),
             { 243, 6, 51, 16 }, 10, displayInk, juce::Justification::centred, false, true);
    // Channel peak feeds come from the rendered output, independent of the UI keyboard.
    for (int channel = 0; channel < 2; ++channel)
    {
        const auto peak = channel == 0 ? diagnostics.peakLeft : diagnostics.peakRight;
        const auto peakDb = peak > 0 ? juce::Decibels::gainToDecibels(peak) : -100.0f;
        const auto x = 812 + channel * 35;
        g.setColour(juce::Colour(0xff1b2420));
        g.fillRoundedRectangle(static_cast<float>(x - 4), 166, 13, 87, 3);
        for (int segment = 0; segment < 12; ++segment)
        {
            const auto y = 246 - segment * 7;
            const auto threshold = -33.0f + static_cast<float>(segment) * 3;
            const auto lit = peakDb >= threshold;
            const auto colour = segment >= 10 ? juce::Colour(0xffffd166) : juce::Colour(0xff89ef8e);
            g.setColour(lit ? colour : colour.withAlpha(0.19f));
            g.fillRoundedRectangle(static_cast<float>(x - 1), static_cast<float>(y), 5, 5, 1.5f);
            if (segment % 2 == 0)
                drawText(g, juce::String(juce::roundToInt(threshold)), { x + 6, y - 1, 17, 9 }, 6.5f, silver);
        }
    }
    drawText(g, "synthax1", { 675, 271, 188, 47 }, 40, juce::Colour(0xffe1e3df), juce::Justification::centred, true, true);
    drawText(g, "NATIVE INSTRUMENT", { 676, 314, 100, 11 }, 7, silver, juce::Justification::centredLeft);
    drawText(g, JucePlugin_VersionString, { 846, 266, 29, 11 }, 8, silver);

    g.saveState();
    g.addTransform(juce::AffineTransform::translation(0.0f, static_cast<float>(modulationExtra)));
    g.setColour(juce::Colour(0xff141717));
    g.fillRoundedRectangle(30, 510, 102, 77, 4);
    const auto visual = owner.getUiVisualSnapshot();
    for (int wheel = 0; wheel < 2; ++wheel)
    {
        const auto x = 75 + wheel * 28;
        g.setColour(juce::Colours::black);
        g.fillRect(x - 2, 514, 21, 71);
        g.setGradientFill(juce::ColourGradient(juce::Colour(0xff52575a), static_cast<float>(x), 515,
                                               juce::Colour(0xff191c1c), static_cast<float>(x + 16), 576, false));
        g.fillRect(x, 515, 17, 67);
        g.setColour(juce::Colour(0xff101515));
        for (int y = 520; y < 579; y += 4) g.drawHorizontalLine(y, static_cast<float>(x + 1), static_cast<float>(x + 16));
        const auto value = wheel == 0 ? (visual.pitchBend + 1) * 0.5f : visual.modWheel;
        const auto y = 578 - juce::roundToInt(value * 58);
        g.setColour(silver.withAlpha(0.75f));
        g.drawHorizontalLine(y, static_cast<float>(x + 2), static_cast<float>(x + 15));
    }
    // Thirty white keys, C2 through D6. Black keys are hit-tested first.
    const std::array<int, 7> noteOffsets { 0, 2, 4, 5, 7, 9, 11 };
    constexpr float whiteWidth = 21.8f;
    for (int key = 0; key < 30; ++key)
    {
        const auto midiNote = 36 + (key / 7) * 12 + noteOffsets[static_cast<std::size_t>(key % 7)];
        const auto x = 133 + static_cast<float>(key) * whiteWidth;
        auto keyBounds = juce::Rectangle<float>(x, 511, whiteWidth - 1, 74);
        g.setGradientFill(juce::ColourGradient(isKeyDown(midiNote) ? juce::Colour(0xff6fb4c4) : juce::Colour(0xfff9f9f4),
                                               keyBounds.getTopLeft(), juce::Colour(0xffc5c9c5), keyBounds.getBottomLeft(), false));
        g.fillRoundedRectangle(keyBounds, 1.5f);
        g.setColour(juce::Colour(0xff878d8a));
        g.drawVerticalLine(juce::roundToInt(x), 512, 584);
    }
    for (int key = 0; key < 29; ++key)
    {
        const auto index = key % 7;
        if (index == 2 || index == 6) continue;
        const auto midiNote = 37 + (key / 7) * 12 + noteOffsets[static_cast<std::size_t>(index)];
        const auto x = 133 + static_cast<float>(key + 1) * whiteWidth - 6;
        auto keyBounds = juce::Rectangle<float>(x, 511, 12, 49);
        g.setColour(juce::Colours::black.withAlpha(0.6f));
        g.fillRect(keyBounds.translated(1.5f, 1.5f));
        g.setGradientFill(juce::ColourGradient(isKeyDown(midiNote) ? juce::Colour(0xff2a8ea6) : juce::Colour(0xff111313),
                                               keyBounds.getTopLeft(), juce::Colour(0xff363a39), keyBounds.getBottomLeft(), false));
        g.fillRoundedRectangle(keyBounds, 1);
        g.setColour(juce::Colour(0xff737a76));
        g.drawHorizontalLine(555, x + 2, x + 10);
    }
    g.restoreState();
    for (const auto& item : captions)
        drawText(g, item.value, item.bounds, item.height, item.colour, juce::Justification::centred, false, item.bounds.getY() < 30);
}

int SynthAudioProcessorEditor::ClassicSurface::keyboardNoteAt(juce::Point<float> position) const
{
    position.y -= static_cast<float>(modulationExtra);
    if (position.x < 133 || position.x >= 787 || position.y < 511 || position.y >= 586) return -1;
    const std::array<int, 7> offsets { 0, 2, 4, 5, 7, 9, 11 };
    constexpr float width = 21.8f;
    if (position.y < 560)
    {
        for (int key = 0; key < 29; ++key)
        {
            const auto index = key % 7;
            if (index == 2 || index == 6) continue;
            const auto x = 133 + static_cast<float>(key + 1) * width - 6;
            if (position.x >= x && position.x < x + 12)
                return 37 + (key / 7) * 12 + offsets[static_cast<std::size_t>(index)];
        }
    }
    const auto key = static_cast<int>((position.x - 133) / width);
    return 36 + (key / 7) * 12 + offsets[static_cast<std::size_t>(key % 7)];
}
void SynthAudioProcessorEditor::ClassicSurface::noteAt(juce::Point<float> position)
{
    const auto note = keyboardNoteAt(position);
    if (note == activeKey) return;
    if (activeKey >= 0) owner.submitUiMidiMessage(juce::MidiMessage::noteOff(1, activeKey));
    activeKey = note;
    if (activeKey >= 0) owner.submitUiMidiMessage(juce::MidiMessage::noteOn(1, activeKey, static_cast<juce::uint8>(100)));
    repaint(132, 510 + modulationExtra, 656, 77);
}
void SynthAudioProcessorEditor::ClassicSurface::mouseDown(const juce::MouseEvent& event)
{
    grabKeyboardFocus(); // so Ctrl+Z and Ctrl+Y reach the surface
    if (keyboardNoteAt(event.position) >= 0)
    {
        noteAt(event.position);
        return;
    }
    if (event.position.y >= 515 + modulationExtra && event.position.y <= 583 + modulationExtra)
    {
        if (event.position.x >= 75 && event.position.x <= 92) draggingWheel = 0;
        else if (event.position.x >= 103 && event.position.x <= 120) draggingWheel = 1;
        if (draggingWheel >= 0)
        {
            if (auto* parameter = wheelParameter(draggingWheel))
                parameter->beginChangeGesture();
            mouseDrag(event);
        }
    }
    if (juce::Rectangle<int>(358, 160, 232, 18).contains(event.position.toInt())) programMenu();
}
void SynthAudioProcessorEditor::ClassicSurface::mouseDrag(const juce::MouseEvent& event)
{
    if (draggingWheel >= 0)
    {
        const auto normalized = juce::jlimit(0.0f, 1.0f, (583 + modulationExtra - event.position.y) / 68);
        if (auto* parameter = wheelParameter(draggingWheel))
            parameter->setValueNotifyingHost(normalized);
        announce(draggingWheel == 0 ? "Pitch Bend" : "Mod Wheel", juce::String(normalized * 100, 1) + " %");
    }
    else noteAt(event.position);
}
void SynthAudioProcessorEditor::ClassicSurface::mouseUp(const juce::MouseEvent&)
{
    if (activeKey >= 0) owner.submitUiMidiMessage(juce::MidiMessage::noteOff(1, activeKey));
    activeKey = -1;
    endWheelGesture();
    repaint(132, 510 + modulationExtra, 656, 77);
}

void SynthAudioProcessorEditor::ClassicSurface::sectionMenu(const juce::String& title,
                                                            const std::vector<std::string>& prefixes)
{
    juce::PopupMenu menu;
    menu.addSectionHeader(title);
    menu.addItem(1, "Copy section");
    menu.addItem(2, "Paste section", !sectionClipboard.empty() && clipboardSection == title);
    menu.addItem(3, "Reset section");
    menu.showMenuAsync(optionsAtMouse(),
                       [safe = juce::Component::SafePointer<ClassicSurface>(this), title, prefixes](int result) {
                           if (safe == nullptr || result == 0) return;
                           if (result == 1)
                           {
                               safe->sectionClipboard.clear();
                               safe->clipboardSection = title;
                           }
                           for (const auto& specification : synth::getParameterSpecs())
                           {
                               for (std::size_t prefixIndex = 0; prefixIndex < prefixes.size(); ++prefixIndex)
                               {
                                   const auto& prefix = prefixes[prefixIndex];
                                   if (specification.id.rfind(prefix, 0) != 0) continue;
                                   if ((prefix == "mod_env." || prefix == "lfo.")
                                       && specification.id.rfind(prefix + "2.", 0) == 0) continue;
                                   const auto suffix = std::to_string(prefixIndex) + ":" + specification.id.substr(prefix.size());
                                   if (result == 1)
                                   {
                                       if (const auto* parameter = safe->owner.getValueTreeState().getRawParameterValue(specification.id))
                                           safe->sectionClipboard[suffix] = parameter->load();
                                   }
                                   else if (result == 2)
                                   {
                                       const auto value = safe->sectionClipboard.find(suffix);
                                       if (value != safe->sectionClipboard.end()) safe->writeParameter(specification.id, value->second);
                                   }
                                   else safe->writeParameter(specification.id, synth::initialParameterValue(specification));
                               }
                           }
                           safe->announce(title, result == 1 ? "Copied" : (result == 2 ? "Pasted" : "Reset"));
                       });
}
void SynthAudioProcessorEditor::ClassicSurface::openPresetBrowser()
{
    if (browserPanel != nullptr)
    {
        closePresetBrowser();
        return;
    }
    const juce::Component::SafePointer<ClassicSurface> safe(this);
    browserPanel.reset(createPresetBrowserPanel(owner, [safe] {
        juce::MessageManager::callAsync([safe] {
            if (safe != nullptr)
                safe->closePresetBrowser();
        });
    }));
    // Covers the instrument down to the keyboard, which stays playable while browsing.
    addAndMakeVisible(*browserPanel);
    browserPanel->setBounds(0, 0, canvasWidth, 507 + modulationExtra);
    browserPanel->toFront(true);
    browserPanel->grabKeyboardFocus();
}

void SynthAudioProcessorEditor::ClassicSurface::closePresetBrowser()
{
    browserPanel.reset();
    repaint();
    grabKeyboardFocus();
}

void SynthAudioProcessorEditor::ClassicSurface::programMenu()
{
    juce::PopupMenu menu;
    menu.addItem(1, "Initialize program");
    menu.addItem(2, "Reset program");
    menu.addItem(3, "Randomize program");
    menu.addSeparator();
    menu.addItem(4, "Copy program");
    menu.addItem(5, "Paste program");
    menu.addItem(6, "Insert program");
    menu.addItem(7, "Delete program");
    menu.addSeparator();
    menu.addItem(8, "Load preset...");
    menu.addItem(9, "Save preset...");
    menu.addItem(10, "Load bank...");
    menu.addItem(11, "Save bank...");
    menu.addSeparator();
    menu.addItem(12, "All notes off");
    menu.addItem(13, "Rename program...");
    juce::PopupMenu programs;
    for (int program = 0; program < owner.getNumPrograms(); ++program)
        programs.addItem(1000 + program, juce::String(program + 1).paddedLeft('0', 3) + ": " + owner.getProgramName(program),
                         true, program == owner.getCurrentProgram());
    menu.addSubMenu("Select program", programs);
    menu.showMenuAsync(optionsAtMouse(),
                       [safe = juce::Component::SafePointer<ClassicSurface>(this)](int result) {
                           if (safe == nullptr || result == 0) return;
                           juce::String message;
                           if (result == 1) safe->owner.initializeCurrentPreset(message);
                           else if (result == 2) safe->owner.resetCurrentPreset(message);
                           else if (result == 3) safe->owner.randomizeCurrentPreset(message);
                           else if (result == 4) safe->owner.copyCurrentProgram(message);
                           else if (result == 5) safe->owner.pasteCurrentProgram(message);
                           else if (result == 6) safe->owner.insertCurrentProgram(message);
                           else if (result == 7) safe->owner.deleteCurrentProgram(message);
                           else if (result == 8) safe->loadFile(false);
                           else if (result == 9) safe->saveFile(false);
                           else if (result == 10) safe->loadFile(true);
                           else if (result == 11) safe->saveFile(true);
                           else if (result == 12) safe->owner.requestPanic();
                           else if (result == 13) safe->renameProgram();
                           else if (result >= 1000) safe->owner.selectProgram(result - 1000, message);
                           if (message.isNotEmpty()) safe->announce("Program", message);
                       });
}
void SynthAudioProcessorEditor::ClassicSurface::renameProgram()
{
    const auto programIndex = owner.getCurrentProgram();
    auto ownedWindow = std::make_unique<juce::AlertWindow>("Rename program", "Program name", juce::MessageBoxIconType::NoIcon);
    auto* window = ownedWindow.get();
    window->addTextEditor("name", owner.getProgramName(programIndex));
    window->addButton("Save", 1, juce::KeyPress(juce::KeyPress::returnKey));
    window->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    window->enterModalState(true, juce::ModalCallbackFunction::create([safe = juce::Component::SafePointer<ClassicSurface>(this), window, programIndex](int result) {
                                if (safe != nullptr && result == 1)
                                {
                                    safe->owner.changeProgramName(programIndex, window->getTextEditorContents("name"));
                                    safe->repaint(358, 160, 233, 18);
                                }
                            }),
                            true);
    ownedWindow.release(); // JUCE owns the modal window until its callback completes.
}

void SynthAudioProcessorEditor::ClassicSurface::loadFile(bool bank)
{
    fileChooser = std::make_unique<juce::FileChooser>(bank ? "Load program bank" : "Load preset",
                                                      owner.getUserPresetDirectory(), bank ? "*.Synthax1Bank;*.fxb;*.fxp" : "*.Synthax1Preset;*.json");
    fileChooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                             [safe = juce::Component::SafePointer<ClassicSurface>(this), bank](const juce::FileChooser& chooser) {
                                 if (safe == nullptr || !chooser.getResult().existsAsFile()) return;
                                 juce::String message;
                                 if (bank) safe->owner.loadProgramBank(chooser.getResult(), message);
                                 else safe->owner.loadPresetFile(chooser.getResult(), message);
                                 safe->announce("Load", message);
                             });
}
void SynthAudioProcessorEditor::ClassicSurface::saveFile(bool bank)
{
    fileChooser = std::make_unique<juce::FileChooser>(bank ? "Save program bank" : "Save preset",
                                                      owner.getUserPresetDirectory().getChildFile(owner.getCurrentPresetName() + (bank ? ".Synthax1Bank" : ".Synthax1Preset")),
                                                      bank ? "*.Synthax1Bank;*.fxb;*.fxp" : "*.Synthax1Preset");
    fileChooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                                 | juce::FileBrowserComponent::warnAboutOverwriting,
                             [safe = juce::Component::SafePointer<ClassicSurface>(this), bank](const juce::FileChooser& chooser) {
                                 if (safe == nullptr || chooser.getResult() == juce::File {}) return;
                                 juce::String message;
                                 if (bank) safe->owner.saveProgramBank(chooser.getResult(), true, message);
                                 else safe->owner.savePresetFile(chooser.getResult(), safe->owner.getCurrentPresetName(), message);
                                 safe->announce("Save", message);
                             });
}

void SynthAudioProcessorEditor::ClassicSurface::timerCallback()
{
    if (owner.getSelectedPart() != selectedPart) setPart(owner.getSelectedPart());
    const auto bindingState = bindingStateNow();
    if (controlBindingState != bindingState || lastProgramIndex != owner.getCurrentProgram())
    {
        buildControls();
        repaint(281, 156, 347, 172);
    }
    for (auto& control : controls)
    {
        control->updateValue();
    }
    for (auto& display : displays)
        display->refresh();
    {
        std::uint64_t now[2] {};
        for (int note = 36; note < 128; ++note)
            if (owner.isNoteHeld(note)) now[note >> 6] |= std::uint64_t { 1 } << (note & 63);
        if (now[0] != shownHeld[0] || now[1] != shownHeld[1])
        {
            shownHeld[0] = now[0];
            shownHeld[1] = now[1];
            repaint(132, 510 + modulationExtra, 656, 77);
        }
    }
    const auto diagnostics = owner.getDiagnosticsSnapshot();
    if (diagnostics.activeVoices != lastVoices
        || diagnostics.patchCost.voiceUnits != lastVoiceUnits
        || diagnostics.patchCost.oscillatorSlotVoices != lastOscillatorSlotVoices)
    {
        lastVoices = diagnostics.activeVoices;
        lastVoiceUnits = diagnostics.patchCost.voiceUnits;
        lastOscillatorSlotVoices = diagnostics.patchCost.oscillatorSlotVoices;
        repaint(240, 4, 58, 19);
    }
    if (std::abs(diagnostics.peakLeft - lastPeakLeft) > 0.0001f
        || std::abs(diagnostics.peakRight - lastPeakRight) > 0.0001f)
    {
        lastPeakLeft = diagnostics.peakLeft;
        lastPeakRight = diagnostics.peakRight;
        repaint(800, 164, 74, 91);
    }
    const auto visual = owner.getUiVisualSnapshot();
    if (std::abs(visual.pitchBend - lastPitchWheel) > 0.00001f
        || std::abs(visual.modWheel - lastModWheel) > 0.00001f)
    {
        lastPitchWheel = visual.pitchBend;
        lastModWheel = visual.modWheel;
        repaint(73, 513, 49, 72);
    }
    const auto preset = owner.getCurrentPresetName();
    if (preset != lastPreset)
    {
        lastPreset = preset;
        repaint(358, 160, 233, 18);
    }
}

void SynthAudioProcessorEditor::ClassicSurface::configureSnapshot()
{
    const auto snapshotPath = juce::SystemStats::getEnvironmentVariable("SYNTHAX1_UI_SNAPSHOT", {});
    if (snapshotPath.isEmpty()) return;
    const auto preset = juce::SystemStats::getEnvironmentVariable("SYNTHAX1_UI_SNAPSHOT_PRESET", {});
    const auto part = juce::SystemStats::getEnvironmentVariable("SYNTHAX1_UI_SNAPSHOT_PART", "A");
    setPart(part.equalsIgnoreCase("B") ? 1 : 0);
    const juce::StringArray pages { "arp", "distortion", "phaser", "chorus", "eq", "delay", "reverb", "compressor" };
    const auto pageName = juce::SystemStats::getEnvironmentVariable("SYNTHAX1_UI_SNAPSHOT_PAGE", "arp");
    selectedPage = std::max(0, pages.indexOf(pageName.toLowerCase()));
    buildControls();
    stopTimer();
    juce::Timer::callAfterDelay(300, [safe = juce::Component::SafePointer<ClassicSurface>(this), snapshotPath, preset] {
        if (safe == nullptr) return;
        juce::String preparationMessage;
        auto prepared = safe->owner.selectProgram(0, preparationMessage);
        if (prepared)
            prepared = preset.isEmpty()
                ? safe->owner.initializeCurrentPreset(preparationMessage)
                : safe->owner.loadPresetFile(juce::File(preset), preparationMessage);
        if (!prepared)
        {
            juce::Logger::writeToLog("Synthax1 UI snapshot preparation failed: " + preparationMessage);
            juce::File failureReport(snapshotPath + ".bindings.json");
            failureReport.getParentDirectory().createDirectory();
            failureReport.replaceWithText("{\"valid\": false, \"failure\": "
                                          + juce::JSON::toString(juce::var(preparationMessage)) + "}\n");
            if (auto* application = juce::JUCEApplicationBase::getInstance()) application->setApplicationReturnValue(1);
            if (juce::SystemStats::getEnvironmentVariable("SYNTHAX1_UI_SNAPSHOT_QUIT", {}) == "1") juce::JUCEApplicationBase::quit();
            return;
        }
        safe->buildControls();
        if (const auto* specification = synth::findParameterSpec("master.level_db"))
            safe->readoutValue = formattedValue(*specification, safe->owner.getEffectiveParameterValue("master.level_db"));
        for (auto& control : safe->controls) control->updateValue();
        const auto scale = juce::jlimit(1.0f, 2.0f, juce::SystemStats::getEnvironmentVariable("SYNTHAX1_UI_SNAPSHOT_SCALE", "1").getFloatValue());
        bool validBindings = true;
        juce::String records;
        for (const auto& control : safe->controls)
        {
            const auto& id = control->parameterId();
            const auto bound = safe->owner.getValueTreeState().getParameter(id) != nullptr
                && synth::findParameterSpec(id) != nullptr;
            validBindings = validBindings && bound;
            if (records.isNotEmpty()) records += ",\n";
            records += "    {\"parameter\": " + juce::JSON::toString(juce::var(juce::String(id)))
                + ", \"bound\": " + juce::String(bound ? "true" : "false") + "}";
        }
        const auto report = "{\n  \"valid\": " + juce::String(validBindings ? "true" : "false")
            + ",\n  \"controls\": [\n" + records + "\n  ]\n}\n";
        juce::File output(snapshotPath);
        output.getParentDirectory().createDirectory();
        juce::File(snapshotPath + ".bindings.json").replaceWithText(report);
        if (!validBindings) juce::Logger::writeToLog("Synthax1 UI snapshot contains unbound controls");
        auto image = safe->createComponentSnapshot(safe->getLocalBounds(), true, scale);
        juce::FileOutputStream stream(output);
        if (stream.openedOk())
        {
            stream.setPosition(0);
            stream.truncate();
            juce::PNGImageFormat().writeImageToStream(image, stream);
        }
        if (juce::SystemStats::getEnvironmentVariable("SYNTHAX1_UI_SNAPSHOT_QUIT", {}) == "1") juce::JUCEApplicationBase::quit();
    });
}

SynthAudioProcessorEditor::SynthAudioProcessorEditor(SynthAudioProcessor& audioProcessor)
    : AudioProcessorEditor(&audioProcessor), surface(std::make_unique<ClassicSurface>(audioProcessor))
{
    addAndMakeVisible(*surface);
    setResizable(true, true);
    setResizeLimits(canvasWidth, canvasHeight, canvasWidth * 2, canvasHeight * 2);
    getConstrainer()->setFixedAspectRatio(static_cast<double>(canvasWidth) / canvasHeight);
    setSize(juce::roundToInt(canvasWidth * 1.2f), juce::roundToInt(canvasHeight * 1.2f)); // opens 20% larger than the logical canvas
    surface->configureSnapshot();
}
SynthAudioProcessorEditor::~SynthAudioProcessorEditor() = default;
void SynthAudioProcessorEditor::paint(juce::Graphics& g)
{ g.fillAll(juce::Colour(0xff171a1a)); }
void SynthAudioProcessorEditor::resized()
{
    const auto scale = std::min(static_cast<float>(getWidth()) / canvasWidth,
                                static_cast<float>(getHeight()) / canvasHeight);
    surface->setTransform(juce::AffineTransform::scale(scale));
    surface->setTopLeftPosition(0, 0);
}
