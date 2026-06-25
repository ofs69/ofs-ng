#pragma once
#include <vector>

// Concrete definitions of the opaque types forward-declared in PluginApi.h.
// Include only in ProcessingSystem.cpp (fills buffers) and PluginManager.cpp
// (implements accessor function pointers).

namespace ofs {

struct OfsDiscreteInput {
    std::vector<double> times;
    std::vector<float> positions; // float [0,100] matching internal GraphSample
};

struct OfsDiscreteOutput {
    std::vector<double> times;
    std::vector<float> positions;
};

} // namespace ofs
