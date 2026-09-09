# Auto-tune Plugin Blueprint

## System Context
- **Framework**: JUCE Framework (C++20)
- **Target Formats**: VST3, AU
- **Real-time Constraint**: Audio processing under 5ms latency inside `processBlock`.

## Architecture & Responsibilities
1. **Core DSP (Managed by GPT-5.6 Sol)**:
   - Pitch Detection: Implementation of YIN or MPM algorithm.
   - Pitch Shifting: Implementation of PSOLA (Pitch Synchronous Overlap-Add) to preserve formants.
   - Scale Mapping: Quantize detected pitch to the nearest note in a selected musical scale.
2. **UI & Control (Managed by GPT-5.6 Terra)**:
   - Parameters: Retune Speed (ms), Scale/Key Selector, Correction Amount (0-100%).
   - Custom LookAndFeel for knobs and scale visualization.

## Development Workflow
- Create a standard JUCE Audio Plug-In template.
- Implement the DSP processing pipelines independently before hooking into the UI.
- Validate real-time thread safety.
