#!/usr/bin/env bash
#
# Builds a self-contained ofs-ng AppImage from an already-built Linux tree (bin/<config>).
#
# Run it after `cmake --build <dir> --target ofs-ng`. It is deliberately independent of CMake — it reads
# the staged runtime files rather than the build graph, so it can package a CI artifact just as well as a
# local build. The `dist` target's .zip and this AppImage are produced from the same file set.
#
# What lands in the AppDir, and why:
#   usr/bin/         ofs-ng, data.pak, managed/ (the .NET host assemblies + first-party plugins).
#                    They must stay siblings: the app resolves both from SDL_GetBasePath(), which is the
#                    directory holding the binary.
#   usr/bin/         ffmpeg, ffprobe — static builds. Found by spawning them off PATH, never by path.
#   usr/lib/         libmpv.so.2 and its non-driver dependencies, bundled by linuxdeploy.
#   usr/share/dotnet host/fxr + the Microsoft.NETCore.App shared framework. No `dotnet` muxer is needed:
#                    the app boots CoreCLR through libnethost/hostfxr directly, and hostfxr discovery
#                    honours DOTNET_ROOT (which AppRun sets).
#
# Library bundling is delegated to linuxdeploy so we inherit the canonical AppImage excludelist instead
# of hand-maintaining one. That list is what keeps libGL/libEGL/libvulkan/libva/libdrm and the
# X11/Wayland client libraries OUT of the AppDir: bundling any of them would shadow the host's graphics
# stack and break the GL context both ImGui and mpv's render API draw into. libmpv is passed explicitly
# with `-l` because MpvLoader dlopen's it by soname — it is not a DT_NEEDED entry, so linuxdeploy cannot
# discover it from the ELF headers.
#
# The .NET runtime and the ffmpeg binaries are copied in AFTER linuxdeploy has run, so it never tries to
# rewrite the RPATH of, or strip, the runtime we ship.
#
# Usage:
#   tools/appimage/make-appimage.sh --bin-dir bin/release [--app-name ofs-ng] [--output <path>]
#
# Options:
#   --bin-dir DIR    directory holding the built app, data.pak and managed/ (required)
#   --app-name NAME  binary name; ofs-ng or ofs-ng-compat (default: autodetected in --bin-dir)
#   --output PATH    output .AppImage (default: bin/dist/<same name the `dist` zip uses>.AppImage)
#   --no-mpv         do not bundle libmpv (video falls back to the host's, then degrades gracefully)
#   --no-ffmpeg      do not bundle ffmpeg/ffprobe (transcode hides itself when they are absent)
#
# Environment:
#   DOTNET_ROOT          .NET root to bundle from (default: derived from `dotnet --list-runtimes`)
#   OFS_FFMPEG_URL       static ffmpeg tarball, overriding the pinned one; requires OFS_FFMPEG_SHA256
#   OFS_FFMPEG_SHA256    expected SHA-256 of OFS_FFMPEG_URL
#   OFS_APPIMAGE_TOOLS   cache dir for linuxdeploy/appimagetool (default: bin/appimage-tools)
#   APPIMAGE_EXTRACT_AND_RUN=1 is exported for us — the tools are themselves AppImages, and CI runners
#   and containers usually have no FUSE.

set -euo pipefail

# --- Pinned downloads ------------------------------------------------------------------------------
# Every byte this script pulls off the network is pinned to an exact version and checked against a
# SHA-256 recorded here. These tools run on the release runner, and their output is what users execute,
# so an upstream force-push or a compromised mirror would land straight in a signed release.
#
# The `continuous` tag both projects publish is expressly NOT used: its asset bytes change underneath a
# fixed URL, which is exactly the property a pin has to exclude.
#
# To bump one: change the version, re-run, and copy the hash from the mismatch error it prints.
linuxdeploy_version="1-alpha-20251107-1"
linuxdeploy_sha256="c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d"
appimagetool_version="1.9.1"
appimagetool_sha256="ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0"
# johnvansickle publishes only an .md5 beside the tarball. MD5 is not collision-resistant, so it is worth
# nothing as a supply-chain check; this hash was taken once from a download whose md5 did match upstream.
ffmpeg_version="7.0.2"
ffmpeg_sha256="abda8d77ce8309141f83ab8edf0596834087c52467f6badf376a6a2a4c87cf67"

# Progress goes to stderr, never stdout: fetch_tool() returns a path through command substitution, and a
# log line on stdout would be captured into it.
log() { printf '\033[1;34m==>\033[0m %s\n' "$*" >&2; }
die() { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

command -v sha256sum >/dev/null || die "sha256sum not found; it is required to verify every download"
sha256_of() { sha256sum "$1" | cut -d' ' -f1; }

# A failed check deletes the file: leaving it behind invites a re-run from cache, and a half-written
# download would otherwise masquerade as a tampered one forever.
verify_sha256() {
    local file="$1" expected="$2" what="$3" actual
    actual=$(sha256_of "$file")
    if [ "$actual" = "$expected" ]; then
        return 0
    fi
    rm -f "$file"
    die "${what}: SHA-256 mismatch — download discarded.
    expected ${expected}
    actual   ${actual}
  If you deliberately bumped the version, update the hash near the top of $(basename "${BASH_SOURCE[0]}")."
}

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
here="${repo_root}/tools/appimage"

bin_dir=""
app_name=""
output=""
bundle_mpv=1
bundle_ffmpeg=1

while [ $# -gt 0 ]; do
    case "$1" in
        --bin-dir)   bin_dir="$2"; shift 2 ;;
        --app-name)  app_name="$2"; shift 2 ;;
        --output)    output="$2"; shift 2 ;;
        --no-mpv)    bundle_mpv=0; shift ;;
        --no-ffmpeg) bundle_ffmpeg=0; shift ;;
        # Prints the header block: everything after the shebang up to the first non-comment line.
        -h|--help)   awk 'NR==1{next} /^#/{print; next} {exit}' "${BASH_SOURCE[0]}"; exit 0 ;;
        *)           die "unknown argument: $1" ;;
    esac
done

[ -n "$bin_dir" ] || die "--bin-dir is required"
bin_dir=$(cd "$bin_dir" && pwd) || die "--bin-dir does not exist"

# Both AVX variants write into the same bin/<config>, so autodetection prefers an explicit --app-name and
# only falls back to whichever of the two names is present.
if [ -z "$app_name" ]; then
    if [ -x "${bin_dir}/ofs-ng" ]; then app_name="ofs-ng"
    elif [ -x "${bin_dir}/ofs-ng-compat" ]; then app_name="ofs-ng-compat"
    else die "no ofs-ng binary in ${bin_dir}; build it first"
    fi
fi
[ -x "${bin_dir}/${app_name}" ] || die "${bin_dir}/${app_name} not found or not executable"
[ -f "${bin_dir}/data.pak" ]    || die "${bin_dir}/data.pak not found; build the ofs_stage_assets target"
[ -d "${bin_dir}/managed" ]     || die "${bin_dir}/managed not found; the .NET host assemblies are required"

app_title="ofs-ng"
[ "$app_name" = "ofs-ng-compat" ] && app_title="ofs-ng (compat)"

# Resolve the ffmpeg source here rather than at the download site near the end: a misconfigured override
# must fail now, not after several minutes of fetching tools and bundling libraries.
#
# Upstream serves the newest build from releases/ and moves each superseded one to old-releases/ under the
# same basename, so a pinned version answers from one path today and the other after the next release. Try
# both; the hash is what decides either way.
ffmpeg_sha="$ffmpeg_sha256"
ffmpeg_urls=(
    "https://johnvansickle.com/ffmpeg/releases/ffmpeg-${ffmpeg_version}-amd64-static.tar.xz"
    "https://johnvansickle.com/ffmpeg/old-releases/ffmpeg-${ffmpeg_version}-amd64-static.tar.xz"
)
# An override replaces the pin, so it has to carry its own hash. Letting OFS_FFMPEG_URL through unverified
# would leave a plain env var as the way to smuggle an arbitrary binary into the bundle.
if [ "$bundle_ffmpeg" -eq 1 ] && [ -n "${OFS_FFMPEG_URL:-}" ]; then
    [ -n "${OFS_FFMPEG_SHA256:-}" ] || die "OFS_FFMPEG_URL requires OFS_FFMPEG_SHA256 (the override is not verified otherwise)"
    ffmpeg_urls=("$OFS_FFMPEG_URL")
    ffmpeg_sha="$OFS_FFMPEG_SHA256"
fi

# --- Output name -----------------------------------------------------------------------------------
# Mirrors cmake/MakeDist.cmake: <plat>-<app>-<tag>+<shorthash>[-compat]. Resolved from git at run time so
# the archive tracks the checked-out revision; a tag-less or git-less tree degrades to a shorter name
# rather than failing. Keep the two naming schemes in step — a release lists the .zip and the .AppImage
# side by side.
if [ -z "$output" ]; then
    tag=""; short=""
    if command -v git >/dev/null && git -C "$repo_root" rev-parse --git-dir >/dev/null 2>&1; then
        tag=$(git -C "$repo_root" describe --tags --dirty 2>/dev/null || true)
        short=$(git -C "$repo_root" rev-parse --short HEAD 2>/dev/null || true)
    fi
    name="linux-ofs-ng"
    if [ -n "$tag" ] && [ -n "$short" ]; then name="${name}-${tag}+${short}"
    elif [ -n "$tag" ];   then name="${name}-${tag}"
    elif [ -n "$short" ]; then name="${name}-g${short}"
    fi
    [ "$app_name" = "ofs-ng-compat" ] && name="${name}-compat"
    output="${repo_root}/bin/dist/${name}.AppImage"
fi
mkdir -p "$(dirname "$output")"

# --- Tools -----------------------------------------------------------------------------------------
tools_dir="${OFS_APPIMAGE_TOOLS:-${repo_root}/bin/appimage-tools}"
mkdir -p "$tools_dir"
# The tools are AppImages themselves; CI runners and containers rarely have FUSE, so tell them to unpack
# into a temp dir and run from there instead of mounting.
export APPIMAGE_EXTRACT_AND_RUN=1

# The cache key carries the version, so bumping a pin fetches afresh instead of reusing the old binary.
# A cached copy is re-verified on every run rather than trusted on presence: the cache dir is an ordinary
# writable directory, and a stale or edited entry there must not be able to reach the release.
fetch_tool() {
    local name="$1" version="$2" url="$3" sha="$4"
    # Separate statement: `local` expands every argument before it assigns any of them, so a dest built
    # from ${name}/${version} on the line above would see them unset (and trip `set -u`).
    local dest="${tools_dir}/${name}-${version}.AppImage"
    # Presence decides reuse and the hash decides trust — testing -x would conflate the two and re-download
    # a byte-identical tool whose +x bit did not survive an archive round-trip or a noexec cache dir.
    if [ -f "$dest" ] && [ "$(sha256_of "$dest")" = "$sha" ]; then
        chmod +x "$dest"
        printf '%s' "$dest"
        return 0
    fi
    log "fetching ${name} ${version}"
    rm -f "$dest"
    curl -fsSL "$url" -o "$dest" || die "failed to download ${name} ${version} from ${url}"
    verify_sha256 "$dest" "$sha" "${name} ${version}"
    chmod +x "$dest"
    printf '%s' "$dest"
}

linuxdeploy=$(fetch_tool linuxdeploy "$linuxdeploy_version" \
    "https://github.com/linuxdeploy/linuxdeploy/releases/download/${linuxdeploy_version}/linuxdeploy-x86_64.AppImage" \
    "$linuxdeploy_sha256")
appimagetool=$(fetch_tool appimagetool "$appimagetool_version" \
    "https://github.com/AppImage/appimagetool/releases/download/${appimagetool_version}/appimagetool-x86_64.AppImage" \
    "$appimagetool_sha256")

# --- Stage the AppDir ------------------------------------------------------------------------------
appdir="${repo_root}/bin/AppDir-${app_name}"
rm -rf "$appdir"
mkdir -p "${appdir}/usr/bin" "${appdir}/usr/lib" "${appdir}/usr/share/dotnet"

log "staging ${app_name} into AppDir"
cp    "${bin_dir}/${app_name}" "${appdir}/usr/bin/"
cp    "${bin_dir}/data.pak"    "${appdir}/usr/bin/"
cp -r "${bin_dir}/managed"     "${appdir}/usr/bin/"
# The managed .pdb are debug symbols for the C# hosts/plugins; they are never shipped on Linux.
find "${appdir}/usr/bin/managed" -name '*.pdb' -delete

# AppRun and the desktop entry are templates: the binary name differs between the AVX and compat
# variants, and appimagetool requires Icon= to match the icon basename at the AppDir root.
#
# AppRun is rendered OUTSIDE the AppDir. linuxdeploy copies --custom-apprun into <appdir>/AppRun, and
# handing it a path that already is <appdir>/AppRun makes it copy the file onto itself and fail.
tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT
apprun="${tmpdir}/AppRun"
sed -e "s|@OFS_APP_NAME@|${app_name}|g" "${here}/AppRun.in" > "$apprun"
chmod +x "$apprun"
sed -e "s|@OFS_APP_NAME@|${app_name}|g" -e "s|@OFS_APP_TITLE@|${app_title}|g" \
    "${here}/ofs-ng.desktop.in" > "${appdir}/${app_name}.desktop"
cp "${repo_root}/data/icons/logo256.png" "${appdir}/${app_name}.png"

# --- Bundle libmpv and its dependencies -------------------------------------------------------------
# linuxdeploy's built-in excludelist covers the obvious never-bundle libraries (libc, libstdc++, libGL,
# libEGL, libX11, libdrm, libgbm, libasound, libglib…). It is not sufficient here, for two reasons that
# are specific to how this app loads code:
#
#  1. SDL is linked statically but dlopen's each of its backends by soname at runtime. AppRun puts
#     $APPDIR/usr/lib on LD_LIBRARY_PATH, so ANY copy of those sonames we ship silently wins over the
#     host's. Bundling e.g. libudev.so.1 would hand SDL a udev client from the build container while it
#     talks to the host's udev daemon; bundling the glib stack would rebind the host's GTK3 (which SDL
#     dlopen's for the native file dialogs) onto our glib. Both are latent, hard-to-diagnose failures.
#  2. Ubuntu's libmpv links the full-featured ffmpeg, so its dependency closure reaches libvulkan,
#     libva*, libvdpau and libOpenCL. Those are driver-coupled: the loader/dispatch library must match
#     the host's ICDs and kernel driver, never the build container's. OFS drives mpv purely through the
#     OpenGL render API (MPV_RENDER_API_TYPE_OPENGL), so no hwdec or VO backend is ever initialised and
#     none of them needs to be present at all — they are only DT_NEEDED entries the loader must satisfy.
#
# So everything below stays host-provided. The pure-compute half of the closure (libav*, the codecs,
# libass, libplacebo, libicu, openssl…) is bundled as normal: it has no host coupling.
#
# The test for adding a pattern here is host coupling, not "does SDL dlopen it". The font/text stack
# (libfreetype, libharfbuzz, libfribidi) is on SDL's dlopen list too, but linuxdeploy's built-in list
# already leaves it host-provided, and it has no daemon or driver behind it — so it needs nothing from us.
ofs_excludes=(
    # Backends SDL dlopen's that speak to a host daemon, driver or on-disk database (see 1 above).
    'libudev.so*' 'libdbus-1.so*' 'libdecor-0.so*' 'libxkbcommon*.so*' 'libwayland-*.so*'
    'libX*.so*' 'libxcb*.so*' 'libpulse*.so*' 'libasound.so*' 'libjack.so*' 'libpipewire-*.so*'
    'libsndio.so*'
    # The GTK3 file dialog is the host's; do not rebind it onto our glib/pango/cairo.
    'libglib-2.0.so*' 'libgobject-2.0.so*' 'libgio-2.0.so*' 'libgmodule-2.0.so*' 'libgthread-2.0.so*'
    'libgdk_pixbuf-2.0.so*' 'libpango*.so*' 'libcairo*.so*' 'librsvg-2.so*'
    # Driver-coupled graphics/compute dispatch (see 2 above).
    'libvulkan.so*' 'libva.so*' 'libva-*.so*' 'libvdpau.so*' 'libOpenCL.so*'
    # mpv video outputs OFS never selects; they only pull more of the host's desktop in behind them.
    'libSDL2-*.so*' 'libcaca.so*' 'libsixel.so*'
    # System/service clients that must speak to the host's daemons.
    'libsystemd.so*' 'libselinux.so*' 'libapparmor.so*' 'libcap.so*'
)

mpv_args=()
if [ "$bundle_mpv" -eq 1 ]; then
    # Resolve through the loader cache rather than guessing a multiarch path. .so.1 is MpvLoader's own
    # fallback soname, so honour it here too.
    mpv_lib=$(ldconfig -p | awk '/libmpv\.so\.2/ {print $NF; exit}')
    [ -n "$mpv_lib" ] || mpv_lib=$(ldconfig -p | awk '/libmpv\.so\.1/ {print $NF; exit}')
    [ -n "$mpv_lib" ] || die "libmpv not found; install libmpv2 or pass --no-mpv"
    log "bundling $(basename "$mpv_lib") from ${mpv_lib}"
    mpv_args=(-l "$mpv_lib")
fi

exclude_args=()
for pat in "${ofs_excludes[@]}"; do exclude_args+=("--exclude-library=${pat}"); done

log "running linuxdeploy (excludelist decides what stays host-provided)"
"$linuxdeploy" --appdir "$appdir" \
    -e "${appdir}/usr/bin/${app_name}" \
    -d "${appdir}/${app_name}.desktop" \
    -i "${appdir}/${app_name}.png" \
    "${mpv_args[@]}" \
    "${exclude_args[@]}" \
    --custom-apprun "$apprun"
[ -x "${appdir}/AppRun" ] || die "linuxdeploy did not install the custom AppRun"

# A regression here is silent and only bites on someone else's machine, so assert it at build time:
# nothing SDL dlopen's, and nothing driver-coupled, may end up in the AppDir.
banned=$(ls "${appdir}/usr/lib" 2>/dev/null | grep -E \
    '^(libudev|libsystemd|libvulkan|libva|libvdpau|libOpenCL|libX|libxcb|libxkbcommon|libwayland|libdbus-1|libdecor|libglib-2|libgobject-2|libgio-2|libpulse|libasound|libSDL2)' || true)
[ -z "$banned" ] || die "host-coupled libraries leaked into the AppDir:"$'\n'"$banned"

# --- Bundle the .NET runtime (after linuxdeploy, so it is left untouched) ----------------------------
dotnet_root="${DOTNET_ROOT:-}"
if [ -z "$dotnet_root" ]; then
    command -v dotnet >/dev/null || die "no DOTNET_ROOT and no dotnet on PATH"
    dotnet_root=$(dirname "$(readlink -f "$(command -v dotnet)")")
fi
[ -d "${dotnet_root}/shared/Microsoft.NETCore.App" ] || die "no shared framework under ${dotnet_root}"

# Highest installed version of each. hostfxr and the framework are versioned independently, so resolve
# them separately rather than assuming one version string covers both.
fxr_ver=$(ls "${dotnet_root}/host/fxr" | sort -V | tail -1)
run_ver=$(ls "${dotnet_root}/shared/Microsoft.NETCore.App" | sort -V | tail -1)
log "bundling .NET runtime ${run_ver} (hostfxr ${fxr_ver})"
mkdir -p "${appdir}/usr/share/dotnet/host/fxr" "${appdir}/usr/share/dotnet/shared/Microsoft.NETCore.App"
cp -r "${dotnet_root}/host/fxr/${fxr_ver}" "${appdir}/usr/share/dotnet/host/fxr/"
cp -r "${dotnet_root}/shared/Microsoft.NETCore.App/${run_ver}" \
      "${appdir}/usr/share/dotnet/shared/Microsoft.NETCore.App/"

# --- Bundle static ffmpeg / ffprobe -----------------------------------------------------------------
if [ "$bundle_ffmpeg" -eq 1 ]; then
    log "bundling static ffmpeg/ffprobe ${ffmpeg_version}"
    ff="${tmpdir}/ffmpeg"; mkdir -p "$ff"
    got=0
    for url in "${ffmpeg_urls[@]}"; do
        if curl -fsSL "$url" -o "${ff}/ffmpeg.tar.xz"; then got=1; break; fi
        log "not served from ${url}"
    done
    [ "$got" -eq 1 ] || die "could not download ffmpeg ${ffmpeg_version} from any known location"
    verify_sha256 "${ff}/ffmpeg.tar.xz" "$ffmpeg_sha" "ffmpeg ${ffmpeg_version}"
    tar -xJf "${ff}/ffmpeg.tar.xz" -C "$ff" --strip-components=1
    cp "${ff}/ffmpeg" "${ff}/ffprobe" "${appdir}/usr/bin/"
    chmod +x "${appdir}/usr/bin/ffmpeg" "${appdir}/usr/bin/ffprobe"
fi

# --- Pack ------------------------------------------------------------------------------------------
log "packing ${output}"
rm -f "$output"
ARCH=x86_64 "$appimagetool" "$appdir" "$output"
chmod +x "$output"

log "wrote $(du -h "$output" | cut -f1) → ${output}"
readelf -d "${appdir}/usr/bin/${app_name}" | awk '/NEEDED/ {print "    host lib:", $NF}'
