# Synthax1 interface guide

These are unmodified captures of Synthax1 0.1.0 Standalone, taken on 2026-10-09 at the native 908 × 591 size. The interface is drawn from owned code and uses the Synthax1 identity. The classic the reference synth reference guides the recreation; these images document Synthax1, and do not establish identical pixels or sound. See the [product contract](../SPEC.md#1-current-release-contract) for the separate visual, behavior, audio, and persistence requirements.

## Part A: start with a pluck

![Synthax1 Part A with the Pluck Core 01 factory preset](assets/screenshots/part-a-pluck.png)

Preset: [Pluck Core 01](<../presets/factory/Pluck/PL - Pluck Core 01.Synthax1Preset>). **Part Select** at the top chooses A or B. The selected part's two oscillators flank its amplitude envelope; the left filter below them shapes that part's sound. **Filter Control** changes the shared filter settings, while **Mixer** on the right balances A, B, and the master output.

The lower row contains two modulation envelopes, two LFOs, and miscellaneous source/destination routes. The keyboard and pitch/modulation wheels run along the bottom; bend range sits on the left and mono/legato and portamento sit on the right.

## Part B: edit the second part

![Synthax1 Part B with the Pad Wide 01 factory preset](assets/screenshots/part-b-pad.png)

Preset: [Pad Wide 01](<../presets/factory/Pad/PD - Pad Wide 01.Synthax1Preset>). **PART B** is selected, revealing oscillator B1/B2, **AMP ENV B**, and **FILTER B**. Their settings are independent of Part A; the shared modulation panels and mixer remain available on the same screen.

## Arpeggiator: edit a pattern

![Synthax1 arpeggiator page with the Arp Motion 01 factory preset](assets/screenshots/arpeggiator-motion.png)

Preset: [Arp Motion 01](<../presets/factory/Arp/ARP - Arp Motion 01.Synthax1Preset>). **ARPEG** selects the central LCD page. This patch shows UpDown mode, two octaves, and step transpose/velocity values. The small square beside ARPEG enables the arpeggiator; **Sync** at the top selects synchronized timing.

The LCD's top bar names the current program. Buttons **1–4** select subbanks, the arrows step through programs, and **MENU** provides program editing and preset/bank load and save. See the [current preset and program workflow](PRESET_SCHEMA.md#program-workflow).

## Effects: select a page and enable its module

![Synthax1 delay page with the FX Space 01 factory preset](assets/screenshots/effects-delay.png)

Preset: [FX Space 01](<../presets/factory/FX/FX - Space 01.Synthax1Preset>). **DELAY** selects the LCD page with left/right timing, filtering, feedback, dry/wet, smear, spread, width, and ping-pong controls. The squares beside the effect names enable each module; selecting its page alone does not enable it. The rest of the instrument stays visible while editing effects.

Capture procedure and binding checks are described in [Validation](VALIDATION.md#headless-ui-snapshot). These screenshots show stationary UI state, not host playback or release qualification.
