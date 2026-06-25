#pragma once

#include "Core/StandardAxis.h"
#include <string>
#include <vector>

namespace ofs {

// Remembered parameters of a project's last funscript export, so "Quick Export" can re-run with no
// dialog. Persisted in the .ofp (see Format/Project). `format` mirrors
// ExportFunscriptRequestEvent::format (0 = 1.0 per-file, 1 = 1.1, 2 = 2.0). `outputPath` is UTF-8:
// the destination folder for format 0, a single .funscript file for formats 1/2.
struct ExportConfig {
    int format = 0;
    std::vector<StandardAxis> axes;
    std::string outputPath;
};

} // namespace ofs
