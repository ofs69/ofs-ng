# Packs the shipped runtime assets (data/, which holds the script library under data/scripts/lib/, and
# lang/) into one deflate-compressed data.pak, so the app ships and loads a single archive instead of a
# loose file tree. Shipping one artifact — staged as a tracked build output (see src/CMakeLists.txt) — is
# what fixes the old "data not always copied" bug.
#
# Entry names are stored source-relative ("data/fonts/lucide.ttf", "lang/de.toml",
# "data/scripts/lib/Sine.cs") so they match both ofs::res::read() lookups and the OFS_ASSETS_FALLBACK_DIR
# dev fallback (which reads the same names straight from the source tree). file(ARCHIVE_CREATE FORMAT
# zip) deflates by default.
#
# Invoked as: cmake -DSRC=<source-dir> -DOUT=<zip> -P PackAssets.cmake, with the custom command's
# WORKING_DIRECTORY set to SRC so the relative PATHS resolve and store relative.
#
# Args (passed via -D):
#   SRC  absolute source tree root (holds data/ and lang/)
#   OUT  absolute path of the zip to (re)create

if (NOT DEFINED SRC OR NOT DEFINED OUT)
    message(FATAL_ERROR "PackAssets: SRC and OUT are required")
endif ()

file(GLOB_RECURSE _files RELATIVE "${SRC}" "${SRC}/data/*" "${SRC}/lang/*")
if (NOT _files)
    message(FATAL_ERROR "PackAssets: no asset files found under ${SRC}/data or ${SRC}/lang")
endif ()

file(ARCHIVE_CREATE OUTPUT "${OUT}" PATHS ${_files} FORMAT zip)
