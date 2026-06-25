#pragma once

#include <string>
#include <vector>

namespace ofs {

struct ProcessingEffect {
    std::string type;
    std::vector<float> params; // indexed parallel to the node's param defs:
                               // EffectDefinition::paramDefs / PluginNodeEntry::params / CompiledScript::params
    bool operator==(const ProcessingEffect &) const = default;
};

} // namespace ofs
