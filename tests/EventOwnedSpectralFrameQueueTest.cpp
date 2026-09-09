#include "EventOwnedSpectralFrameQueue.h"

#include <cstdlib>
#include <iostream>

namespace
{
void require (bool value, const char* message)
{
    if (! value) { std::cerr << "FAILED: " << message << '\n'; std::exit (1); }
}
}

int main()
{
    EventOwnedSpectralFrameQueue queue;
    // 1024 is currently audible. The first two frames are already contributing
    // to the output and must remain event A; later frames belong to A only
    // until a target B event replaces them.
    auto* audibleTail = queue.pushFrame (896, 1, 1.0);
    require (audibleTail != nullptr, "push audible tail");
    audibleTail->analysisPayload[0] = 0.25f;
    audibleTail->synthesisPayload[0] = 0.50f;
    require (queue.push (1024, 1, 1.0), "push current frame");
    require (queue.push (1152, 1, 1.0), "push future frame one");
    require (queue.push (1280, 1, 1.0), "push future frame two");

    const auto replaced = queue.replaceUnheard (1152, 1, 2, 1.059463094);
    require (replaced == 4, "every frame with future overlap must be replaced");
    require (queue.countOwnedBy (1) == 0, "old ownership must not survive into future overlap");
    require (queue.countOwnedBy (2) == 4, "B must own every future overlap");
    require (queue.diagnosticFrames()[0].analysisPayload[0] == 0.25f
             && queue.diagnosticFrames()[0].synthesisPayload[0] == 0.50f,
             "audible payload must remain untouched");
    queue.retireBefore (1919);
    require (queue.countOwnedBy (2) == 4,
             "an active overlap tail must remain replaceable until its last sample");
    queue.retireBefore (1920);
    require (queue.countOwnedBy (2) == 3,
             "the completed frame must retire immediately after its last sample");
    std::cout << "EVENT_OWNED_SPECTRAL_QUEUE: PASS | replaced=" << replaced << '\n';
}
