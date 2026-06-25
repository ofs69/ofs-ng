#include "UI/ImGuiHelpers.h"

#include "Localization/Translator.h"
#include "UI/Icons.h"
#include "Util/PathUtil.h"
#include "imgui.h"

namespace ofs::ui {

void showInFileBrowserItem(std::string_view utf8Path) {
    const bool hasPath = !utf8Path.empty();
    ImGui::BeginDisabled(!hasPath);
    if (ImGui::MenuItem(Str::ShowInFileBrowser.iconId(ICON_FOLDER_OPEN, "show_in_file_browser")) && hasPath)
        ofs::util::revealInFileBrowser(ofs::util::fromUtf8(utf8Path));
    ImGui::EndDisabled();
}

} // namespace ofs::ui
