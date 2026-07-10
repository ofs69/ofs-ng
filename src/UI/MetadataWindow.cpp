#include "UI/MetadataWindow.h"

#include "Core/EventQueue.h"
#include "Core/Events.h"
#include "Core/FunscriptMetadata.h"
#include "Core/ScriptProject.h"
#include "Format/AppSettings.h"
#include "Localization/Translator.h"
#include "UI/Icons.h"
#include "UI/ImGuiHelpers.h"
#include "UI/Modals.h"
#include "UI/Theme.h"
#include "Util/FrameAllocator.h"
#include "imgui.h"
#include "imgui_stdlib.h"
#include <algorithm>
#include <exception>
#include <nlohmann/json.hpp>

namespace ofs {

using ofs::ui::beginForm;
using ofs::ui::buttonW;
using ofs::ui::endForm;
using ofs::ui::formRow;
using ofs::ui::kRightGap;

MetadataWindow::MetadataWindow(const AppSettings &appSettings) : appSettings(appSettings) {}

namespace {
constexpr int kCustomTypeCount = 6;

// Localized display name for a custom-field JSON type index (0..5).
const char *customTypeName(int t) {
    switch (t) {
    case 1:
        return Str::PcfTypeBool;
    case 2:
        return Str::PcfTypeNumber;
    case 3:
        return Str::PcfTypeString;
    case 4:
        return Str::PcfTypeArray;
    case 5:
        return Str::PcfTypeObject;
    default:
        return Str::PcfTypeNull;
    }
}

// Combo item label: the localized type name plus a stable ###cf_opt<t> id, so the popup items are
// addressable by id (not the translated label) and survive label/order changes — see suite_metadata.cpp.
const char *customTypeItem(int t) {
    return fmtScratch("{}###cf_opt{}", customTypeName(t), t);
}

// Type dropdown rendered via BeginCombo so each option gets an explicit id. Returns true and writes
// *type when the selection changes.
bool customTypeCombo(const char *id, int *type) {
    bool changed = false;
    if (ImGui::BeginCombo(id, customTypeName(*type))) {
        for (int t = 0; t < kCustomTypeCount; ++t) {
            const bool sel = (*type == t);
            if (ImGui::Selectable(customTypeItem(t), sel)) {
                *type = t;
                changed = true;
            }
            if (sel)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    return changed;
}

// Combo index for a json value's type (all number_* collapse to "Number").
int customTypeIndex(const nlohmann::json &v) {
    if (v.is_boolean())
        return 1;
    if (v.is_number())
        return 2;
    if (v.is_string())
        return 3;
    if (v.is_array())
        return 4;
    if (v.is_object())
        return 5;
    return 0; // null
}

nlohmann::json customDefaultForType(int type) {
    switch (type) {
    case 1:
        return false;
    case 2:
        return 0;
    case 3:
        return std::string{};
    case 4:
        return nlohmann::json::array();
    case 5:
        return nlohmann::json::object();
    default:
        return nlohmann::json{}; // null
    }
}
} // namespace

void MetadataWindow::render(const ScriptProject &project, EventQueue &eq, bool &open) {
    if (!open)
        return;

    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_FirstUseEver, {0.5f, 0.5f});
    // Same start size + min-constraints as the Project window (ProjectConfigWindow) so the two dialogs
    // open at a consistent, comfortably-large size rather than clipping the form.
    ImGui::SetNextWindowSize({vp->Size.x * 0.45f, vp->Size.y * 0.7f}, ImGuiCond_FirstUseEver);
    const float em = ImGui::GetFontSize();
    ImGui::SetNextWindowSizeConstraints({em * 24.f, em * 18.f}, {em * 64.f, em * 72.f});

    if (ImGui::Begin(Str::PcfTabMetadata.id("metadata_window"), &open, ImGuiWindowFlags_NoCollapse)) {
        // Re-sync the editing mirror only on an actual document change (value compare, no heap alloc),
        // then bind the widgets to it. By render time this frame's events have already drained, so an
        // in-progress edit pushed last frame is now reflected in project.metadata and compares equal —
        // the mirror is only overwritten by genuine external changes (preset load, reset, file load).
        if (metaBuf != project.metadata)
            metaBuf = project.metadata;
        FunscriptMetadata &meta = metaBuf;

        const float spacing = ImGui::GetStyle().ItemSpacing.x;

        // --- Presets ---
        ImGui::SeparatorText(Str::PcfPresets);
        const auto &presets = appSettings.metadataPresets;

        if (selectedPresetIdx >= static_cast<int>(presets.size()))
            selectedPresetIdx = -1;

        // Row 1: preset picker | Load | Delete. The two verb buttons auto-size to their (translated)
        // labels and the combo fills the rest, so a longer translation never clips.
        const char *loadLbl = Str::PcfLoad.id("preset_load");
        const char *deleteLbl = Str::PcfDelete.id("preset_delete");
        const float loadW = buttonW(loadLbl);
        const float deleteW = buttonW(deleteLbl);
        const char *previewLabel =
            selectedPresetIdx >= 0 ? presets[selectedPresetIdx].name.c_str() : Str::PcfSelectPreset.c_str();
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - loadW - deleteW - spacing * 2.f - kRightGap);
        if (ImGui::BeginCombo("###preset_combo", previewLabel)) {
            for (int i = 0; i < static_cast<int>(presets.size()); ++i) {
                bool sel = (i == selectedPresetIdx);
                // Stable ###id keyed on the list index so the (user-supplied) preset name doesn't decide
                // the item's widget id.
                if (ImGui::Selectable(fmtScratch("{}###preset_opt{}", presets[i].name, i), sel)) {
                    selectedPresetIdx = i;
                    newPresetName = presets[i].name;
                }
                if (sel)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(selectedPresetIdx < 0);
        if (ImGui::Button(loadLbl, {loadW, 0.f})) {
            eq.push(ModifyEvent<FunscriptMetadata>{
                [md = presets[selectedPresetIdx].metadata](FunscriptMetadata &m) { m = md; }});
            eq.push(NotifyEvent{.level = NotifyLevel::Success,
                                .message = Str::PcfPresetLoaded.fmt(presets[selectedPresetIdx].name)});
            meta = presets[selectedPresetIdx].metadata;
        }
        ImGui::SameLine();
        if (ImGui::Button(deleteLbl, {deleteW, 0.f})) {
            const int delIdx = selectedPresetIdx;
            confirmAsync(eq,
                         {.title = Str::PcfPresetDeleteConfirmTitle.c_str(),
                          .message = Str::PcfPresetDeleteConfirmBody.fmt(presets[delIdx].name),
                          .buttons = {Str::PcfDelete.c_str(), Str::AppCancel.c_str()},
                          .severity = ofs::ModalSeverity::Warning},
                         [this, eqp = &eq, delIdx](int idx) {
                             if (idx != 0)
                                 return;
                             eqp->push(ModifyEvent<AppSettings>{[delIdx](AppSettings &s) {
                                 if (delIdx >= 0 && delIdx < static_cast<int>(s.metadataPresets.size()))
                                     s.metadataPresets.erase(s.metadataPresets.begin() + delIdx);
                             }});
                             selectedPresetIdx = -1;
                             newPresetName.clear();
                         });
        }
        ImGui::EndDisabled();

        // Row 2: name input | Save (creates new or overwrites if name matches existing) | Reset (clears metadata)
        const char *saveLbl = Str::PcfSave.id("preset_save");
        const float saveW = buttonW(saveLbl);
        const char *resetLbl = Str::PcfReset.id("reset_meta");
        const float resetW = buttonW(resetLbl);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - saveW - resetW - 2 * spacing - kRightGap);
        ImGui::InputTextWithHint("###preset_name", Str::PcfPresetNameHint.c_str(), &newPresetName);
        ImGui::SameLine();
        ImGui::BeginDisabled(newPresetName.empty());
        if (ImGui::Button(saveLbl, {saveW, 0.f})) {
            const auto it = std::ranges::find_if(presets, [&](const auto &p) { return p.name == newPresetName; });
            selectedPresetIdx =
                it != presets.end() ? static_cast<int>(it - presets.begin()) : static_cast<int>(presets.size());
            // Overwrite a same-named preset in place, else append. The lambda re-derives the match so it
            // stays correct even if the list changed between this frame and the drain.
            eq.push(ModifyEvent<AppSettings>{[name = newPresetName, md = meta](AppSettings &s) {
                const auto e = std::ranges::find_if(s.metadataPresets, [&](const auto &p) { return p.name == name; });
                if (e != s.metadataPresets.end())
                    e->metadata = md;
                else
                    s.metadataPresets.push_back({.name = name, .metadata = md});
            }});
            eq.push(NotifyEvent{.level = NotifyLevel::Success, .message = Str::PcfPresetSaved.fmt(newPresetName)});
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button(resetLbl, {resetW, 0.f})) {
            eq.push(ModifyEvent<FunscriptMetadata>{[](FunscriptMetadata &m) { m = FunscriptMetadata{}; }});
            meta = FunscriptMetadata{};
            jsonEdits.clear();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            ImGui::SetTooltip("%s", Str::PcfClearAllTip.c_str());

        // --- Script Info ---
        ImGui::SeparatorText(Str::PcfScriptInfo);
        if (beginForm("##info_form")) {
            formRow(Str::PcfFieldTitle);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::InputText("###title", &meta.title))
                eq.push(ModifyEvent<FunscriptMetadata>{[v = meta.title](FunscriptMetadata &m) { m.title = v; }});
            formRow(Str::PcfCreator);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::InputText("##creator", &meta.creator))
                eq.push(ModifyEvent<FunscriptMetadata>{[v = meta.creator](FunscriptMetadata &m) { m.creator = v; }});
            formRow(Str::PcfScriptUrl);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::InputText("##surl", &meta.scriptUrl))
                eq.push(
                    ModifyEvent<FunscriptMetadata>{[v = meta.scriptUrl](FunscriptMetadata &m) { m.scriptUrl = v; }});
            formRow(Str::PcfVideoUrl);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::InputText("##vurl", &meta.videoUrl))
                eq.push(ModifyEvent<FunscriptMetadata>{[v = meta.videoUrl](FunscriptMetadata &m) { m.videoUrl = v; }});
            formRow(Str::PcfLicense);
            // Visible labels are localized but each option carries a stable ###id; the *stored* license
            // value stays the English token ("Free"/"Paid"/"") since it is persisted to the funscript.
            const char *licenseOptions[] = {"—###lic_none", Str::PcfLicenseFree.id("lic_free"),
                                            Str::PcfLicensePaid.id("lic_paid")};
            int licenseIdx = 0;
            if (meta.license == "Free")
                licenseIdx = 1;
            else if (meta.license == "Paid")
                licenseIdx = 2;
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::Combo("##license", &licenseIdx, licenseOptions, IM_ARRAYSIZE(licenseOptions))) {
                meta.license = licenseIdx == 1 ? "Free" : (licenseIdx == 2 ? "Paid" : "");
                eq.push(ModifyEvent<FunscriptMetadata>{[v = meta.license](FunscriptMetadata &m) { m.license = v; }});
            }
            endForm();
        }

        // --- Description ---
        ImGui::SeparatorText(Str::PcfDescription);
        if (ImGui::InputTextMultiline("##description", &meta.description,
                                      {ImGui::GetContentRegionAvail().x - kRightGap, ImGui::GetTextLineHeight() * 4.f}))
            eq.push(
                ModifyEvent<FunscriptMetadata>{[v = meta.description](FunscriptMetadata &m) { m.description = v; }});

        // --- Notes ---
        ImGui::SeparatorText(Str::PcfNotes);
        if (ImGui::InputTextMultiline("##notes", &meta.notes,
                                      {ImGui::GetContentRegionAvail().x - kRightGap, ImGui::GetTextLineHeight() * 4.f}))
            eq.push(ModifyEvent<FunscriptMetadata>{[v = meta.notes](FunscriptMetadata &m) { m.notes = v; }});

        // --- Tags ---
        ImGui::SeparatorText(Str::PcfTags);
        if (!meta.tags.empty()) {
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {4.f, 2.f});
            for (int i = 0; i < static_cast<int>(meta.tags.size()); ++i) {
                // Stable ###tag<i> id so the visible tag text doesn't decide the widget id (test-addressable).
                if (ImGui::SmallButton(fmtScratch("{}###tag{}", meta.tags[i], i))) {
                    eq.push(ModifyEvent<FunscriptMetadata>{[i](FunscriptMetadata &m) {
                        if (i >= 0 && i < static_cast<int>(m.tags.size()))
                            m.tags.erase(m.tags.begin() + i);
                    }});
                    meta.tags.erase(meta.tags.begin() + i);
                    --i;
                }
                if (i + 1 < static_cast<int>(meta.tags.size()))
                    ImGui::SameLine();
            }
            ImGui::PopStyleVar();
        }
        {
            const float addBtnW = buttonW(ICON_PLUS);
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - addBtnW - spacing - kRightGap);
            bool enterTag = ImGui::InputText("###new_tag", &newTagInput, ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            if ((ImGui::Button(ICON_PLUS "###add_tag", {addBtnW, 0.f}) || enterTag) && !newTagInput.empty()) {
                eq.push(
                    ModifyEvent<FunscriptMetadata>{[v = newTagInput](FunscriptMetadata &m) { m.tags.push_back(v); }});
                meta.tags.push_back(newTagInput);
                newTagInput.clear();
            }
        }

        // --- Performers ---
        ImGui::SeparatorText(Str::PcfPerformers);
        if (!meta.performers.empty()) {
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {4.f, 2.f});
            for (int i = 0; i < static_cast<int>(meta.performers.size()); ++i) {
                if (ImGui::SmallButton(fmtScratch("{}###perf{}", meta.performers[i], i))) {
                    eq.push(ModifyEvent<FunscriptMetadata>{[i](FunscriptMetadata &m) {
                        if (i >= 0 && i < static_cast<int>(m.performers.size()))
                            m.performers.erase(m.performers.begin() + i);
                    }});
                    meta.performers.erase(meta.performers.begin() + i);
                    --i;
                }
                if (i + 1 < static_cast<int>(meta.performers.size()))
                    ImGui::SameLine();
            }
            ImGui::PopStyleVar();
        }
        {
            const float addBtnW = buttonW(ICON_PLUS);
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - addBtnW - spacing - kRightGap);
            bool enterPerformer =
                ImGui::InputText("###new_performer", &newPerformerInput, ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            if ((ImGui::Button(ICON_PLUS "###add_perf", {addBtnW, 0.f}) || enterPerformer) &&
                !newPerformerInput.empty()) {
                eq.push(ModifyEvent<FunscriptMetadata>{
                    [v = newPerformerInput](FunscriptMetadata &m) { m.performers.push_back(v); }});
                meta.performers.push_back(newPerformerInput);
                newPerformerInput.clear();
            }
        }

        renderCustomFields(meta, eq);
    }
    ImGui::End();
}

void MetadataWindow::renderCustomFields(FunscriptMetadata &meta, EventQueue &eq) {
    ImGui::SeparatorText(Str::PcfCustomFields);

    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    // Width the trash button to the glyph plus symmetric padding so the icon sits centered (a bare
    // frame-height square is narrower than the glyph needs and clips it left-of-center).
    const float delBtnW = ImGui::CalcTextSize(ICON_TRASH).x + ImGui::GetStyle().FramePadding.x * 2.f;
    // Font-relative key column (scales with font/DPI); the type column is sized to the widest *localized*
    // type name so a longer translation never clips and the value column always starts past it.
    const float keyW = ImGui::GetFontSize() * 10.f;
    float typeW = 0.f;
    for (int t = 0; t < kCustomTypeCount; ++t)
        typeW = ImMax(typeW, ImGui::CalcTextSize(customTypeName(t)).x);
    typeW += ImGui::GetStyle().FramePadding.x * 2.f;

    for (int i = 0; i < static_cast<int>(meta.customFields.size()); ++i) {
        auto &cf = meta.customFields[i];
        ImGui::PushID(i);

        // Row 1: key, type, scalar value (if scalar), delete. Both the key and the type are fixed
        // once created — the key is the field's identity (it keys customJsonText and the on-disk JSON
        // object) and the type is derived from the stored value, so neither can be edited in place
        // without orphaning state or risking a value/type mismatch. To change either, delete and
        // re-add. Both are shown as plain labels, frame-aligned to sit level with the value editor.
        const float typeColX = keyW + spacing;
        const float valColX = typeColX + typeW + spacing;
        const int type = customTypeIndex(cf.value);

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(cf.key.c_str());
        ImGui::SameLine(typeColX);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(customTypeName(type));

        const bool scalar = type <= 3;
        if (scalar) {
            ImGui::SameLine(valColX);
            const float valW = ImGui::GetContentRegionAvail().x - delBtnW - spacing - kRightGap;
            switch (type) {
            case 1: { // bool
                bool b = cf.value.get<bool>();
                if (ImGui::Checkbox("###cf_bool", &b)) {
                    cf.value = b; // keep the mirror in sync so no full resync copy is needed next frame
                    eq.push(ModifyEvent<FunscriptMetadata>{[i, b](FunscriptMetadata &m) {
                        if (i >= 0 && i < static_cast<int>(m.customFields.size()))
                            m.customFields[i].value = b;
                    }});
                }
                break;
            }
            case 2: { // number
                double d = cf.value.get<double>();
                ImGui::SetNextItemWidth(valW);
                if (ImGui::InputDouble("###cf_num", &d)) {
                    cf.value = d;
                    eq.push(ModifyEvent<FunscriptMetadata>{[i, d](FunscriptMetadata &m) {
                        if (i >= 0 && i < static_cast<int>(m.customFields.size()))
                            m.customFields[i].value = d;
                    }});
                }
                break;
            }
            case 3: { // string
                // Bind straight to the json's internal string (no per-frame copy); the edit lands in
                // the mirror immediately and is forwarded to the document via the event.
                std::string *s = cf.value.get_ptr<std::string *>();
                ImGui::SetNextItemWidth(valW);
                if (s && ImGui::InputText("###cf_str", s))
                    eq.push(ModifyEvent<FunscriptMetadata>{[i, v = *s](FunscriptMetadata &m) {
                        if (i >= 0 && i < static_cast<int>(m.customFields.size()))
                            m.customFields[i].value = v;
                    }});
                break;
            }
            default: // null — no value editor
                ImGui::TextDisabled("%s", Str::PcfNullValue.c_str());
                break;
            }
        }

        ImGui::SameLine();
        bool deleted = false;
        if (ImGui::Button(ICON_TRASH "###cf_del", {delBtnW, 0.f})) {
            jsonEdits.erase(cf.key);
            eq.push(ModifyEvent<FunscriptMetadata>{[i](FunscriptMetadata &m) {
                if (i >= 0 && i < static_cast<int>(m.customFields.size()))
                    m.customFields.erase(m.customFields.begin() + i);
            }});
            meta.customFields.erase(meta.customFields.begin() + i);
            --i;
            deleted = true;
        }

        // Row 2: array/object edited as raw JSON text, validated on commit. Parsing only happens on
        // an actual edit; the per-frame render path does no json parse or dump.
        if (!deleted && type >= 4) {
            auto it = jsonEdits.find(cf.key);
            if (it == jsonEdits.end())
                it = jsonEdits.emplace(cf.key, JsonEditState{.text = cf.value.dump(4), .lastValue = cf.value}).first;
            JsonEditState &st = it->second;

            if (!st.valid) {
                ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);
                ImGui::PushStyleColor(ImGuiCol_Border, ofs::theme::GetStyleColorVec4(AppCol_Error));
            }
            const float boxW = ImGui::GetContentRegionAvail().x - kRightGap;
            bool changed = ImGui::InputTextMultiline("###cf_json", &st.text, {boxW, ImGui::GetTextLineHeight() * 4.f});
            const bool editing = ImGui::IsItemActive();
            if (!st.valid) {
                ImGui::PopStyleColor();
                ImGui::PopStyleVar();
                ImGui::TextColored(ofs::theme::GetStyleColorVec4(AppCol_Error), "%s", Str::PcfInvalidJson.c_str());
            }

            if (changed) {
                // Re-parse only the keystroke that changed the buffer; cache validity for the next
                // frame's red border so the render path never parses.
                try {
                    nlohmann::json edited = nlohmann::json::parse(st.text);
                    st.valid = true;
                    cf.value = edited; // keep the mirror in sync so no full resync copy is needed next frame
                    eq.push(ModifyEvent<FunscriptMetadata>{[i, nv = std::move(edited)](FunscriptMetadata &m) {
                        if (i >= 0 && i < static_cast<int>(m.customFields.size()))
                            m.customFields[i].value = nv;
                    }});
                } catch (const std::exception &) {
                    st.valid = false; // mid-edit invalid JSON — keep the committed value until it parses
                }
            }
            // While the box isn't being edited, reflow it to pretty-printed (4-space) JSON whenever the
            // committed value has changed since we last synced — picks up external changes (preset load
            // / reset) and normalizes the user's own input once they finish. The value compare is
            // allocation-free; the dump runs only on a real change, not every frame.
            else if (!editing && cf.value != st.lastValue) {
                st.text = cf.value.dump(4);
                st.lastValue = cf.value;
                st.valid = true;
            }
        }

        ImGui::PopID();
    }

    // Add-new-field row.
    {
        ImGui::SetNextItemWidth(keyW);
        ImGui::InputTextWithHint("###cf_new_key", Str::PcfFieldNameHint.c_str(), &newCustomFieldKey);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(typeW);
        customTypeCombo("###cf_new_type", &newCustomFieldType);
        ImGui::SameLine();
        if (ImGui::Button(ICON_PLUS "###cf_add", {buttonW(ICON_PLUS), 0.f}) && !newCustomFieldKey.empty()) {
            eq.push(ModifyEvent<FunscriptMetadata>{
                [k = newCustomFieldKey, nv = customDefaultForType(newCustomFieldType)](FunscriptMetadata &m) {
                    m.customFields.push_back({.key = k, .value = nv});
                }});
            meta.customFields.push_back({.key = newCustomFieldKey, .value = customDefaultForType(newCustomFieldType)});
            newCustomFieldKey.clear();
        }
    }
}

} // namespace ofs
