#include <cmath>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
struct Frame
{
    double timeSeconds = 0.0;
    double spawnEpoch = -1.0;
    int spawnTargetMidi = -1;
    double spawnTargetEventId = -1.0;
    int dominantTargetMidi = -1;
    double dominantTargetEventId = -1.0;
};
struct Commit { double timeSeconds = 0.0; int midi = -1; };

std::vector<std::string> split (const std::string& line)
{
    std::vector<std::string> values;
    std::stringstream stream (line);
    std::string value;
    while (std::getline (stream, value, ',')) values.push_back (value);
    return values;
}
bool inFixture (double time)
{
    return (time >= 0.50 && time <= 0.75) || (time >= 3.90 && time <= 4.05)
        || (time >= 4.70 && time <= 5.30) || (time >= 8.55 && time <= 8.70);
}
} // namespace

// Confirms that every scheduled grain carries a stable event identity, not
// merely a note number. This is the prerequisite for a later A→B→A-aware
// cycle scheduler. The audit is read-only and never touches audio.
int main (int argc, char* argv[])
{
    if (argc != 3)
    {
        std::cerr << "usage: VibeAutotuneTargetEventOwnershipAudit <events.csv> <intervals.csv>\n";
        return 2;
    }
    std::ifstream eventsFile (argv[1]), intervalsFile (argv[2]);
    if (! eventsFile || ! intervalsFile)
    {
        std::cerr << "failed to open audit CSV\n";
        return 2;
    }

    std::string line;
    std::getline (eventsFile, line);
    std::vector<Commit> commits;
    while (std::getline (eventsFile, line))
    {
        const auto fields = split (line);
        if (fields.size() >= 4 && fields[2] == "target_commit")
            commits.push_back ({ std::stod (fields[1]) * 0.001, std::stoi (fields[3]) });
    }

    std::getline (intervalsFile, line);
    const auto header = split (line);
    std::unordered_map<std::string, size_t> column;
    for (size_t index = 0; index < header.size(); ++index) column[header[index]] = index;
    for (const auto& name : { "timeMs", "actualSpawnEpoch", "spawnTargetMidi", "spawnTargetEventId",
                              "olaDominantGrainTargetMidi", "olaDominantGrainTargetEventId" })
        if (! column.contains (name)) { std::cerr << "interval CSV lacks target-event ownership: " << name << '\n'; return 2; }

    std::vector<Frame> frames;
    while (std::getline (intervalsFile, line))
    {
        const auto fields = split (line);
        frames.push_back ({ std::stod (fields[column["timeMs"]]) * 0.001,
                            std::stod (fields[column["actualSpawnEpoch"]]),
                            std::stoi (fields[column["spawnTargetMidi"]]),
                            std::stod (fields[column["spawnTargetEventId"]]),
                            std::stoi (fields[column["olaDominantGrainTargetMidi"]]),
                            std::stod (fields[column["olaDominantGrainTargetEventId"]]) });
    }

    int measured = 0;
    for (const auto& commit : commits)
    {
        if (! inFixture (commit.timeSeconds)) continue;
        double priorEpoch = -1.0, priorEventId = -1.0;
        for (const auto& frame : frames)
            if (frame.timeSeconds < commit.timeSeconds) { priorEpoch = frame.spawnEpoch; priorEventId = frame.spawnTargetEventId; }

        double firstOwnedSpawn = -1.0, firstOwnedDominant = -1.0, eventId = -1.0;
        for (const auto& frame : frames)
        {
            if (frame.timeSeconds < commit.timeSeconds || frame.timeSeconds > commit.timeSeconds + 0.080) continue;
            const auto isNewSpawn = frame.spawnEpoch != priorEpoch;
            if (firstOwnedSpawn < 0.0 && isNewSpawn && frame.spawnTargetMidi == commit.midi
                && frame.spawnTargetEventId >= 0.0 && frame.spawnTargetEventId != priorEventId)
            {
                firstOwnedSpawn = frame.timeSeconds;
                eventId = frame.spawnTargetEventId;
            }
            if (eventId >= 0.0 && firstOwnedDominant < 0.0 && frame.dominantTargetMidi == commit.midi
                && std::abs (frame.dominantTargetEventId - eventId) < 0.5)
                firstOwnedDominant = frame.timeSeconds;
            priorEpoch = frame.spawnEpoch;
        }
        if (firstOwnedSpawn < 0.0 || firstOwnedDominant < 0.0)
        {
            std::cerr << "FAILED: missing owned handoff after commit at " << commit.timeSeconds << "s\n";
            return 1;
        }
        const auto spawnMs = (firstOwnedSpawn - commit.timeSeconds) * 1000.0;
        const auto dominantMs = (firstOwnedDominant - commit.timeSeconds) * 1000.0;
        std::cout << "target_event_ownership,commit_s=" << commit.timeSeconds
                  << ",midi=" << commit.midi << ",event_id=" << eventId
                  << ",first_owned_spawn_ms=" << spawnMs
                  << ",first_owned_dominant_ms=" << dominantMs
                  << ",ownership_window_ms=" << (dominantMs - spawnMs) << '\n';
        ++measured;
    }
    if (measured == 0) { std::cerr << "FAILED: no fixture commits found\n"; return 1; }
    std::cout << "target_event_ownership_events=" << measured << '\n';
    return 0;
}
