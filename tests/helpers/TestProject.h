#pragma once

#include "Core/EventQueue.h"
#include "Core/ScriptProject.h"
#include "Services/ScriptRegistry.h"

namespace ofs::test {

struct TestProject {
    ScriptProject project;
    EventQueue eq;
    ScriptRegistryState scriptReg; // empty by default; script-node tests pre-seed it

    TestProject();
};

} // namespace ofs::test
