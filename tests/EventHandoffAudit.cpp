#include <algorithm>
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
    double spawnExpectedHz = 0.0;
    double dominantExpectedHz = 0.0;
};

struct Commit
{
    double timeSeconds = 0.0;
    int midi = -1;
};

std::vector<std::string> split (const std::string& line)
{
    std::vector<std::string> values;
    std::stringstream stream (line);
    std::string value;
    while (std::getline (stream, value, ','))
        values.push_back (value);
    return values;
}

double asDouble (const std::string& value) { return std::stod (value); }
int asInt (const std::string& value) { return std::stoi (value); }
double centsError (double frequency, double target)
{
    return 1200.0 * std::log2 (frequency / target);
}
double midiToHz (int midi) { return 440.0 * std::exp2 ((static_cast<double> (midi) - 69.0) / 12.0); }

bool inFixture (double time)
{
    return (time >= 0.50 && time <= 0.75)
        || (time >= 3.90 && time <= 4.05)
        || (time >= 4.70 && time <= 5.30)
        || (time >= 8.55 && time <= 8.70);
}
} // namespace

int main (int argc, char* argv[])
{
    if (argc != 3)
    {
        std::cerr << "usage: VibeAutotuneEventHandoffAudit <events.csv> <intervals.csv>\n";
        return 2;
    }

    std::ifstream eventsFile (argv[1]);
    std::ifstream intervalsFile (argv[2]);
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
        if (fields.size() < 4 || fields[2] != "target_commit")
            continue;
        commits.push_back ({ asDouble (fields[1]) * 0.001, asInt (fields[3]) });
    }

    std::getline (intervalsFile, line);
    const auto header = split (line);
    std::unordered_map<std::string, size_t> column;
    for (size_t index = 0; index < header.size(); ++index)
        column[header[index]] = index;
    const auto require = [&] (const std::string& name) { return column.contains (name); };
    if (! require ("timeMs") || ! require ("actualSpawnEpoch") || ! require ("spawnExpectedF0")
        || ! require ("olaDominantGrainExpectedF0"))
    {
        std::cerr << "interval CSV lacks ownership telemetry\n";
        return 2;
    }

    std::vector<Frame> frames;
    while (std::getline (intervalsFile, line))
    {
        const auto fields = split (line);
        frames.push_back ({ asDouble (fields[column["timeMs"]]) * 0.001,
                            asDouble (fields[column["actualSpawnEpoch"]]),
                            asDouble (fields[column["spawnExpectedF0"]]),
                            asDouble (fields[column["olaDominantGrainExpectedF0"]]) });
    }

    int measured = 0;
    for (const auto& commit : commits)
    {
        if (! inFixture (commit.timeSeconds) || commit.midi < 0)
            continue;
        const auto targetHz = midiToHz (commit.midi);
        double previousEpoch = -1.0;
        for (const auto& frame : frames)
            if (frame.timeSeconds <= commit.timeSeconds)
                previousEpoch = frame.spawnEpoch;
        double firstCorrectSpawn = -1.0;
        double firstCorrectDominant = -1.0;
        for (const auto& frame : frames)
        {
            if (frame.timeSeconds < commit.timeSeconds || frame.timeSeconds > commit.timeSeconds + 0.080)
                continue;
            if (firstCorrectSpawn < 0.0 && frame.spawnEpoch != previousEpoch && frame.spawnExpectedHz > 0.0
                && std::abs (centsError (frame.spawnExpectedHz, targetHz)) <= 25.0)
                firstCorrectSpawn = frame.timeSeconds;
            if (firstCorrectDominant < 0.0 && frame.dominantExpectedHz > 0.0
                && std::abs (centsError (frame.dominantExpectedHz, targetHz)) <= 25.0)
                firstCorrectDominant = frame.timeSeconds;
        }
        if (firstCorrectSpawn < 0.0 || firstCorrectDominant < 0.0)
        {
            std::cerr << "FAILED: missing candidate/dominant grain after commit at " << commit.timeSeconds << "s\n";
            return 1;
        }
        const auto spawnMs = (firstCorrectSpawn - commit.timeSeconds) * 1000.0;
        const auto dominantMs = (firstCorrectDominant - commit.timeSeconds) * 1000.0;
        std::cout << "event_handoff,commit_s=" << commit.timeSeconds
                  << ",target_hz=" << targetHz
                  << ",first_correct_spawn_ms=" << spawnMs
                  << ",first_correct_dominant_ms=" << dominantMs
                  << ",ownership_window_ms=" << (dominantMs - spawnMs) << '\n';
        ++measured;
    }
    if (measured == 0)
    {
        std::cerr << "FAILED: no fixture commits found\n";
        return 1;
    }
    std::cout << "event_handoff_audit_events=" << measured << '\n';
    return 0;
}
