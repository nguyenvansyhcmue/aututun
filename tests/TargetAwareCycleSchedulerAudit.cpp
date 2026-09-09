#include <cmath>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace
{
struct Event
{
    double timeSeconds = 0.0;
    std::string name;
    std::string detail;
};

struct Commit
{
    double timeSeconds = 0.0;
    int midi = -1;
};

std::vector<std::string> split (const std::string& line, char delimiter = ',')
{
    std::vector<std::string> values;
    std::stringstream stream (line);
    std::string value;
    while (std::getline (stream, value, delimiter))
        values.push_back (value);
    return values;
}

double midiToHz (int midi) { return 440.0 * std::exp2 ((static_cast<double> (midi) - 69.0) / 12.0); }
double centsError (double frequency, double target) { return 1200.0 * std::log2 (frequency / target); }

double detailValue (const std::string& detail, const std::string& key)
{
    const auto start = detail.find (key + "=");
    if (start == std::string::npos)
        return 0.0;
    const auto valueStart = start + key.size() + 1;
    const auto valueEnd = detail.find (';', valueStart);
    return std::stod (detail.substr (valueStart, valueEnd - valueStart));
}

bool inFixture (double time)
{
    return (time >= 0.50 && time <= 0.75)
        || (time >= 3.90 && time <= 4.05)
        || (time >= 4.70 && time <= 5.30)
        || (time >= 8.55 && time <= 8.70);
}
} // namespace

// This is deliberately a scheduling feasibility audit, not a renderer test.
// It asks: if a target-aware scheduler assigned the new target at the first
// already-safe grain boundary after a commit, how much of the present spawn
// delay could it remove?  It never writes or renders audio.
int main (int argc, char* argv[])
{
    if (argc != 2)
    {
        std::cerr << "usage: VibeAutotuneTargetAwareCycleSchedulerAudit <events.csv>\n";
        return 2;
    }

    std::ifstream eventsFile (argv[1]);
    if (! eventsFile)
    {
        std::cerr << "failed to open events CSV\n";
        return 2;
    }

    std::string line;
    std::getline (eventsFile, line);
    std::vector<Event> events;
    std::vector<Commit> commits;
    while (std::getline (eventsFile, line))
    {
        const auto fields = split (line);
        if (fields.size() < 4)
            continue;
        const auto timeSeconds = std::stod (fields[1]) * 0.001;
        events.push_back ({ timeSeconds, fields[2], fields[3] });
        if (fields[2] == "target_commit")
            commits.push_back ({ timeSeconds, std::stoi (fields[3]) });
    }

    int measured = 0;
    for (const auto& commit : commits)
    {
        if (! inFixture (commit.timeSeconds) || commit.midi < 0)
            continue;

        const auto targetHz = midiToHz (commit.midi);
        double firstBoundary = -1.0;
        double firstCorrectBoundary = -1.0;
        for (const auto& event : events)
        {
            if (event.name != "grain_spawn" || event.timeSeconds < commit.timeSeconds
                || event.timeSeconds > commit.timeSeconds + 0.080)
                continue;
            if (firstBoundary < 0.0)
                firstBoundary = event.timeSeconds;
            const auto expectedHz = detailValue (event.detail, "expectedF0");
            if (firstCorrectBoundary < 0.0 && expectedHz > 0.0
                && std::abs (centsError (expectedHz, targetHz)) <= 25.0)
                firstCorrectBoundary = event.timeSeconds;
        }

        if (firstBoundary < 0.0 || firstCorrectBoundary < 0.0)
        {
            std::cerr << "FAILED: missing grain boundary after commit at " << commit.timeSeconds << "s\n";
            return 1;
        }

        const auto earliestBoundaryMs = (firstBoundary - commit.timeSeconds) * 1000.0;
        const auto currentCorrectMs = (firstCorrectBoundary - commit.timeSeconds) * 1000.0;
        std::cout << "cycle_scheduler_feasibility,commit_s=" << commit.timeSeconds
                  << ",target_hz=" << targetHz
                  << ",target_period_samples=" << (48000.0 / targetHz)
                  << ",earliest_safe_boundary_ms=" << earliestBoundaryMs
                  << ",current_correct_spawn_ms=" << currentCorrectMs
                  << ",schedulable_spawn_lead_ms=" << (currentCorrectMs - earliestBoundaryMs) << '\n';
        ++measured;
    }

    if (measured == 0)
    {
        std::cerr << "FAILED: no fixture commits found\n";
        return 1;
    }
    std::cout << "cycle_scheduler_feasibility_events=" << measured << '\n';
    return 0;
}
