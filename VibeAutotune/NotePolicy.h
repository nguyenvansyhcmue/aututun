#pragma once

#include <array>
#include <cstdint>

// Musical policy is deliberately independent from rendering colour/icon.
// Its values are persisted by the processor and read as an immutable
// snapshot by CorrectionTrajectory on the audio thread.
enum class NoteRule : uint8_t
{
    Legal = 0,
    IllegalNearest,
    IllegalUp,
    IllegalDown,
    BypassLegal
};

constexpr bool isLegalNoteRule (NoteRule rule) noexcept
{
    return rule == NoteRule::Legal || rule == NoteRule::BypassLegal;
}

struct VocalTargetRange
{
    int firstMidi = 0;
    int lastMidi = 127;
};

// This is a target-selection restriction, not a substitute for the separate
// F0-analysis bandwidth.  The values intentionally match the four input
// profiles used by the editor and pitch detector.
constexpr VocalTargetRange vocalTargetRangeForInputType (int inputType) noexcept
{
    switch (inputType)
    {
        case 0:  return { 54, 83 }; // Soprano: F#3–B5
        case 1:  return { 45, 77 }; // Alto:    A2–F5
        case 2:  return { 40, 71 }; // Tenor:   E2–B4
        case 3:  return { 33, 62 }; // Low:     A1–D4
        default: return { 0, 127 };
    }
}

// Presentation is deliberately a one-way mapping from the DSP state.  The
// glyph is never parsed by the processor or used to choose a target note.
constexpr const char* noteRuleGlyph (NoteRule rule) noexcept
{
    switch (rule)
    {
        case NoteRule::IllegalNearest: return "-";
        case NoteRule::IllegalUp:      return "→";
        case NoteRule::IllegalDown:    return "←";
        case NoteRule::BypassLegal:    return "×";
        case NoteRule::Legal:          break;
    }
    return "";
}

struct NotePolicySnapshot
{
    // A held MIDI note is an explicit monophonic target and takes precedence
    // over automatic Key/Scale selection while it is active.
    int midiTargetNote = -1;
    // The full twelve pitch classes are always present.  A MIDI override is
    // optional and wins over its pitch-class default; this keeps the UI,
    // persistence and DSP on the same note-map model.
    std::array<NoteRule, 12> pitchClassDefaults {};
    std::array<NoteRule, 128> midiNoteOverrides {};
    std::array<bool, 128> hasMidiNoteOverride {};
    bool groupOctaves = true;
    VocalTargetRange targetRange {};

    NoteRule ruleForMidi (int midiNote) const noexcept
    {
        if (midiNote < 0 || midiNote > 127)
            return NoteRule::IllegalNearest;
        return hasMidiNoteOverride[static_cast<size_t> (midiNote)]
             ? midiNoteOverrides[static_cast<size_t> (midiNote)]
             : pitchClassDefaults[static_cast<size_t> (midiNote % 12)];
    }

    bool isLegalTarget (int midiNote) const noexcept
    {
        return midiNote >= targetRange.firstMidi && midiNote <= targetRange.lastMidi
            && isLegalNoteRule (ruleForMidi (midiNote));
    }
};
