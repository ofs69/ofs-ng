# Build-time localization code generator (host tool) plus the generation rule it drives.
#
# Reads strings.toml and emits StringsGenerated.{h,cpp} into the build tree (OFS_STRINGS_HDR/SRC,
# under OFS_GENERATED_DIR) — these are generated artifacts and are not committed to the src tree.
#
# The generator lands in bin/ (the global CMAKE_RUNTIME_OUTPUT_DIRECTORY). It stays there in every
# config; the `dist` packaging target (see cmake/MakeDist.cmake) excludes its .exe/.pdb from the
# shipped archive, since it is a build-time host tool rather than a runtime artifact.

add_executable(localization_gen generate/main.cpp)
target_link_libraries(localization_gen PRIVATE tomlplusplus::tomlplusplus)
target_compile_features(localization_gen PRIVATE cxx_std_20)

# Generate StringsGenerated.{h,cpp} from the source catalog. Runs only when strings.toml
# (MAIN_DEPENDENCY) or the generator changes; a normal build runs no generator. The ofs_strings_gen
# target wraps the outputs so any consumer (src/ app, tests/) can add_dependencies() on it; this
# subdir is added before src/ and tests/, so the target is visible to both.
set(OFS_STRINGS_TOML "${CMAKE_SOURCE_DIR}/localization/strings.toml")
add_custom_command(
        OUTPUT "${OFS_STRINGS_HDR}" "${OFS_STRINGS_SRC}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${OFS_GENERATED_DIR}/Localization"
        COMMAND localization_gen "${OFS_STRINGS_TOML}" "${OFS_STRINGS_HDR}" "${OFS_STRINGS_SRC}"
        MAIN_DEPENDENCY "${OFS_STRINGS_TOML}"
        DEPENDS localization_gen
        COMMENT "Generating localization strings"
        VERBATIM
)
# Validate every shipped translation (lang/*.toml) against the source catalog at build time: each
# source key must be present with a non-empty translation whose {N} placeholders match the source,
# and no stray/unknown keys. A violation fails the build (localization_gen --validate returns non-zero)
# instead of silently falling back to English at runtime. CONFIGURE_DEPENDS re-globs when a lang file
# is added or removed; the stamp re-runs the check whenever a lang file or the source catalog changes.
file(GLOB OFS_LANG_FILES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/lang/*.toml")
set(OFS_LANG_STAMP "${OFS_GENERATED_DIR}/translations_validated.stamp")
add_custom_command(
        OUTPUT "${OFS_LANG_STAMP}"
        COMMAND localization_gen --validate "${OFS_STRINGS_TOML}" ${OFS_LANG_FILES}
        COMMAND "${CMAKE_COMMAND}" -E touch "${OFS_LANG_STAMP}"
        DEPENDS localization_gen "${OFS_STRINGS_TOML}" ${OFS_LANG_FILES}
        COMMENT "Validating translations against source catalog"
        VERBATIM
)

# Bundling the validation stamp into ofs_strings_gen means every consumer (app + tests) that already
# depends on this target also gets the translation check, with no extra wiring in src/ or tests/.
add_custom_target(ofs_strings_gen DEPENDS "${OFS_STRINGS_HDR}" "${OFS_STRINGS_SRC}" "${OFS_LANG_STAMP}")
