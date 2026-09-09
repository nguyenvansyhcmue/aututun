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
    // The full twelve pitch classes are always present.  A MIDI override is
    // optional and wins over its pitch-class default; this keeps the UI,
    // persistence and DSP on the same note-map model.
    std::array<NoteRule, 12> pitchClassDefaults {};
    std::array<NoteRule, 128> midiNoteOverrides {};
    std::array<bool, 128> hasMidiNoteOverride {};
    bool groupOctaves = true;

    NoteRule ruleForMidi (int midiNote) const noexcept
    {
        if (midiNote < 0 || midiNote > 127)
            return NoteRule::IllegalNearest;
        return hasMidiNoteOverride[static_cast<size_t> (midiNote)]
             ? midiNoteOverrides[static_cast<size_t> (midiNote)]
             : pitchClassDefaults[static_cast<size_t> (midiNote % 12)];
    }
};
