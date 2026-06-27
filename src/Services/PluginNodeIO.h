#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

// Concrete definitions of the opaque types forward-declared in PluginApi.h, plus the discrete-I/O
// accessor implementations wired into both HostApi tables (PluginManager for the plugin host, ScriptSystem
// for the script host). Defined once here so the two hosts can't drift — they read job-local buffers only,
// so they are safe on a worker thread.

namespace ofs {

struct OfsDiscreteInput {
    std::vector<double> times;
    std::vector<float> positions; // float [0,100] matching internal GraphSample
};

struct OfsDiscreteOutput {
    std::vector<double> times;
    std::vector<float> positions;
};

inline int nodeInputCount(const OfsDiscreteInput *in) {
    return in ? static_cast<int>(in->times.size()) : 0;
}

inline double nodeInputTime(const OfsDiscreteInput *in, int i) {
    if (!in || i < 0 || i >= static_cast<int>(in->times.size()))
        return 0.0;
    return in->times[static_cast<size_t>(i)];
}

inline int nodeInputPosition(const OfsDiscreteInput *in, int i) {
    if (!in || i < 0 || i >= static_cast<int>(in->positions.size()))
        return 0;
    return static_cast<int>(std::round(in->positions[static_cast<size_t>(i)]));
}

inline void nodeAddAction(OfsDiscreteOutput *out, double time, int position) {
    if (!out)
        return;
    // A NaN/inf time would break the sorted-by-time invariant every downstream consumer relies on (and
    // poison binary searches); a negative time falls before the start of the timeline. Drop either —
    // mirroring the silent clamp applied to position.
    if (!std::isfinite(time) || time < 0.0)
        return;
    out->times.push_back(time);
    out->positions.push_back(static_cast<float>(std::clamp(position, 0, 100)));
}

} // namespace ofs
