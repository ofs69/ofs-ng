#include "helpers/TestProject.h"

namespace ofs::test {

TestProject::TestProject() {
    for (int i = 0; i < kStandardAxisCount; ++i) {
        project.axes[i].role = static_cast<StandardAxis>(i);
    }
}

} // namespace ofs::test
