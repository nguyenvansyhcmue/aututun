#include "PluginProcessor.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>

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

}

int main()
{
    const auto a440 = VibeAutotuneProcessor::calculateInputPitchDisplay (440.0, 440.0, true);
    require (a440.midiNote == 69 && std::abs (a440.cents) < 0.001f,
             "440 Hz was not mapped to A4 at 0 cents");

    const auto a445 = VibeAutotuneProcessor::calculateInputPitchDisplay (445.0, 440.0, true);
    require (a445.midiNote == 69 && a445.cents > 0.0f,
             "445 Hz was not mapped to sharp A4");

    const auto a435 = VibeAutotuneProcessor::calculateInputPitchDisplay (435.0, 440.0, true);
    require (a435.midiNote == 69 && a435.cents < 0.0f,
             "435 Hz was not mapped to flat A4");

    const auto unvoiced = VibeAutotuneProcessor::calculateInputPitchDisplay (std::numeric_limits<double>::quiet_NaN(), 440.0, false);
    require (unvoiced.midiNote == -1 && unvoiced.cents == 0.0f,
             "unvoiced audio retained a pitch-display value");
}
