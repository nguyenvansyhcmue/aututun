#include "PluginProcessor.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace
{
void require (bool condition, const char* message)
{
    if (! condition)
    {
        std::cerr << "FAILED: " << message << '\n';
        std::exit (1);
    }
}

NotePolicySnapshot cMajor()
{
    NotePolicySnapshot policy;
    constexpr std::array<bool, 12> legal {{ true, false, true, false, true, true, false, true, false, true, false, true }};
    for (int midi = 0; midi <= 127; ++midi)
    {
        const auto rule = legal[static_cast<size_t> (midi % 12)] ? NoteRule::Legal : NoteRule::IllegalNearest;
        policy.pitchClassDefaults[static_cast<size_t> (midi % 12)] = rule;
    }
    return policy;
}

constexpr std::array<std::array<bool, 12>, 14> scaleIntervals {{
    {{ true, true, true, true, true, true, true, true, true, true, true, true }},
    {{ true, false, true, false, true, true, false, true, false, true, false, true }},
    {{ true, false, true, true, false, true, false, true, true, false, true, false }},
    {{ true, false, true, false, true, false, false, true, false, true, false, false }},
    {{ true, false, false, true, false, true, false, true, false, false, true, false }},
    {{ true, false, true, true, false, true, false, true, true, false, false, true }},
    {{ true, false, true, true, false, true, false, true, false, true, false, true }}, // Melodic Minor ascending
    {{ true, false, true, false, true, true, false, true, false, true, true, false }}, // Dorian
    {{ true, true, false, true, false, true, false, true, true, false, true, false }},
    {{ true, false, true, false, true, false, true, true, false, true, false, true }},
    {{ true, false, true, false, true, true, false, true, false, true, true, false }},
    {{ true, true, false, true, false, true, true, false, true, false, true, false }},
    {{ true, false, false, true, false, true, true, true, false, false, true, false }},
    {{ true, false, true, false, true, false, true, false, true, false, true, false }}
}};

NotePolicySnapshot policyFor (int key, int scale)
{
    NotePolicySnapshot policy;
    for (int midi = 0; midi <= 127; ++midi)
    {
        const auto pitchClass = midi % 12;
        const auto legal = scaleIntervals[static_cast<size_t> (scale)]
                                     [static_cast<size_t> ((pitchClass - key + 12) % 12)];
        const auto rule = legal ? NoteRule::Legal : NoteRule::IllegalNearest;
        policy.pitchClassDefaults[static_cast<size_t> (pitchClass)] = rule;
    }
    return policy;
}

CorrectionTrajectoryData settle (CorrectionTrajectory& trajectory, int midi, const NotePolicySnapshot& policy)
{
    const auto frequency = 440.0 * std::exp2 ((static_cast<double> (midi) - 69.0) / 12.0);
    CorrectionTrajectoryData output;
    for (int frame = 0; frame < 4; ++frame)
        output = trajectory.update (frequency, 440.0, 0, policy, 0.0, 0.0, 0.0, 0.0, 48000.0, 512, true);
    return output;
}
}

int main()
{
    VibeAutotuneProcessor processor;
    processor.prepareToPlay (48000.0, 64);
    auto* scaleParameter = processor.parameters.getParameter ("scale");
    auto* keyParameter = processor.parameters.getParameter ("key");
    keyParameter->setValueNotifyingHost (keyParameter->convertTo0to1 (0.0f)); // C
    scaleParameter->setValueNotifyingHost (scaleParameter->convertTo0to1 (1.0f)); // Major
    juce::AudioBuffer<float> syncBuffer (2, 64);
    juce::MidiBuffer midi;
    processor.processBlock (syncBuffer, midi); // Applies Key/Scale default policy.
    require (processor.getNoteRuleForPitchClass (0) == NoteRule::Legal
             && processor.getNoteRuleForPitchClass (1) == NoteRule::IllegalNearest
             && processor.getNoteRuleForPitchClass (11) == NoteRule::Legal,
             "Processor did not create the C Major base policy");

    // The live processor must apply every Key/Scale selection to the policy
    // used by CorrectionTrajectory. The raw detected note is intentionally
    // not asserted here: it is independent from musical quantisation.
    for (int key = 0; key < 12; ++key)
        for (int scale = 0; scale < static_cast<int> (scaleIntervals.size()); ++scale)
        {
            keyParameter->setValueNotifyingHost (keyParameter->convertTo0to1 (static_cast<float> (key)));
            scaleParameter->setValueNotifyingHost (scaleParameter->convertTo0to1 (static_cast<float> (scale)));
            processor.processBlock (syncBuffer, midi);
            for (int pitchClass = 0; pitchClass < 12; ++pitchClass)
            {
                const auto expectedLegal = scaleIntervals[static_cast<size_t> (scale)]
                                                      [static_cast<size_t> ((pitchClass - key + 12) % 12)];
                require (processor.getNoteRuleForPitchClass (pitchClass)
                             == (expectedLegal ? NoteRule::Legal : NoteRule::IllegalNearest),
                         "Live Key/Scale policy did not match its selected scale");
            }
        }

    // Target selection itself must only emit legal notes for every preset,
    // independently of the root. This prevents a C-major-only implementation
    // from passing the test suite.
    for (int key = 0; key < 12; ++key)
        for (int scale = 0; scale < static_cast<int> (scaleIntervals.size()); ++scale)
        {
            const auto genericPolicy = policyFor (key, scale);
            for (int inputMidi = 48; inputMidi < 72; ++inputMidi)
            {
                CorrectionTrajectory genericTrajectory;
                const auto genericResult = settle (genericTrajectory, inputMidi, genericPolicy);
                require (genericResult.targetNote >= 0
                             && isLegalNoteRule (genericPolicy.ruleForMidi (genericResult.targetNote)),
                         "Target selection emitted a note outside the selected Key/Scale");
            }
        }

    keyParameter->setValueNotifyingHost (keyParameter->convertTo0to1 (0.0f));
    scaleParameter->setValueNotifyingHost (scaleParameter->convertTo0to1 (1.0f));
    processor.processBlock (syncBuffer, midi);
    processor.setNoteRuleForPitchClass (4, NoteRule::IllegalUp);
    juce::MemoryBlock state;
    processor.getStateInformation (state);
    VibeAutotuneProcessor restoredProcessor;
    restoredProcessor.setStateInformation (state.getData(), static_cast<int> (state.getSize()));
    require (restoredProcessor.getNoteRuleForPitchClass (4) == NoteRule::IllegalUp,
             "Note policy was not restored with processor state");

    // A non-grouped edit is a true MIDI override: it must not overwrite the
    // pitch-class default or neighbouring octaves, and must survive state
    // serialisation.
    processor.setGroupOctaves (false);
    processor.setNoteRuleForMidiNote (61, NoteRule::IllegalUp);
    require (processor.getNoteRuleForMidiNote (61) == NoteRule::IllegalUp
             && processor.getNoteRuleForMidiNote (73) == NoteRule::IllegalNearest,
             "MIDI note override was not isolated to its selected octave");
    juce::MemoryBlock overrideState;
    processor.getStateInformation (overrideState);
    VibeAutotuneProcessor overrideRestoredProcessor;
    overrideRestoredProcessor.setStateInformation (overrideState.getData(), static_cast<int> (overrideState.getSize()));
    require (overrideRestoredProcessor.getNoteRuleForMidiNote (61) == NoteRule::IllegalUp
             && overrideRestoredProcessor.getNoteRuleForMidiNote (73) == NoteRule::IllegalNearest,
             "MIDI note override was not restored with processor state");

    // Selecting a new Key/Scale rebuilds all twelve defaults and clears old
    // local edits. G Major therefore exposes F as '-' and F# as Legal.
    keyParameter->setValueNotifyingHost (keyParameter->convertTo0to1 (7.0f));
    scaleParameter->setValueNotifyingHost (scaleParameter->convertTo0to1 (1.0f));
    processor.processBlock (syncBuffer, midi);
    require (processor.getNoteRuleForPitchClass (5) == NoteRule::IllegalNearest
             && processor.getNoteRuleForPitchClass (6) == NoteRule::Legal
             && processor.getNoteRuleForMidiNote (61) == NoteRule::IllegalNearest,
             "Key/Scale reset did not rebuild the complete note map");

    scaleParameter->setValueNotifyingHost (scaleParameter->convertTo0to1 (14.0f));
    processor.processBlock (syncBuffer, midi);
    for (int pitchClass = 0; pitchClass < 12; ++pitchClass)
        require (processor.getNoteRuleForPitchClass (pitchClass) == NoteRule::IllegalNearest,
                 "User Scale did not create all twelve IllegalNearest defaults");

    auto policy = cMajor();
    for (const auto pitchClass : { 0, 2, 4, 5, 7, 9, 11 })
        require (policy.pitchClassDefaults[static_cast<size_t> (pitchClass)] == NoteRule::Legal, "C Major legal rule mismatch");
    for (const auto pitchClass : { 1, 3, 6, 8, 10 })
        require (policy.pitchClassDefaults[static_cast<size_t> (pitchClass)] == NoteRule::IllegalNearest, "C Major illegal rule mismatch");

    CorrectionTrajectory trajectory;
    auto result = settle (trajectory, 61, policy); // C# -> closest C in tie.
    require (result.targetNote == 60, "IllegalNearest did not select nearest legal note");

    policy.pitchClassDefaults[1] = NoteRule::IllegalUp;
    trajectory.reset();
    result = settle (trajectory, 61, policy);
    require (result.targetNote == 62, "IllegalUp did not select the first legal note above");

    policy.pitchClassDefaults[1] = NoteRule::IllegalDown;
    trajectory.reset();
    result = settle (trajectory, 61, policy);
    require (result.targetNote == 60, "IllegalDown did not select the first legal note below");

    policy = cMajor();
    policy.pitchClassDefaults[4] = NoteRule::IllegalNearest;
    trajectory.reset();
    result = settle (trajectory, 64, policy);
    require (result.targetNote == 65, "Removed E did not choose nearest legal target");

    policy.pitchClassDefaults[4] = NoteRule::BypassLegal;
    trajectory.reset();
    result = settle (trajectory, 64, policy);
    require (result.targetNote == 64 && std::abs (result.correctionCents) < 1.0e-9,
             "BypassLegal did not preserve its own input");

    policy.pitchClassDefaults[5] = NoteRule::IllegalDown;
    trajectory.reset();
    result = settle (trajectory, 65, policy);
    require (result.targetNote == 64 && std::abs (result.correctionCents) > 1.0,
             "BypassLegal was not usable as a legal landing target");

    // Range eligibility is part of policy resolution, not merely a dimming
    // effect in the editor. An excluded B must be able to redirect across an
    // octave to C, provided that C remains in the selected voice range.
    auto ranged = cMajor();
    ranged.targetRange = { 60, 72 };
    ranged.pitchClassDefaults[11] = NoteRule::IllegalUp;
    trajectory.reset();
    result = settle (trajectory, 71, ranged);
    require (result.targetNote == 72,
             "IllegalUp did not find its legal target across an octave within range");

    // Directional rules at a range boundary fail closed: they never wrap,
    // retain a previous target, or reverse direction.
    ranged.pitchClassDefaults[0] = NoteRule::IllegalDown;
    trajectory.reset();
    result = settle (trajectory, 60, ranged);
    require (result.targetNote < 0 && ! result.hasValidCorrection,
             "Range-boundary IllegalDown did not resolve to unity/dry");

    // A completely empty legal set has the same deterministic safe fallback.
    NotePolicySnapshot emptyPolicy;
    emptyPolicy.targetRange = { 0, 127 };
    for (auto& rule : emptyPolicy.pitchClassDefaults)
        rule = NoteRule::IllegalNearest;
    trajectory.reset();
    result = settle (trajectory, 60, emptyPolicy);
    require (result.targetNote < 0 && ! result.hasValidCorrection,
             "Empty legal set did not resolve to unity/dry");

    require (vocalTargetRangeForInputType (0).firstMidi == 54
             && vocalTargetRangeForInputType (0).lastMidi == 83
             && vocalTargetRangeForInputType (3).firstMidi == 33
             && vocalTargetRangeForInputType (3).lastMidi == 62,
             "Vocal target range mapping changed unexpectedly");

    std::cout << "note_policy | C_major=pass G_major=pass nearest=pass up=pass down=pass bypass=pass overrides=pass persistence=pass range=pass empty=pass\n";
}
