#!/usr/bin/env python3
"""Reproduce a back-compat *plugin binary* from the most recent release tag.

Why this exists
---------------
Plugins are recompiled against the current Ofs.Api on every build, so the plugin
suite can only ever prove "a plugin built against *today's* API loads against
today's host" — which is tautological. It can never reveal an API break, because a
recompile just rebinds the plugin to whatever surface Ofs.Api now exposes.

The only honest witness for the back-compat guarantee (PluginBootstrapper: same
Ofs.Api MAJOR + plugin <= host) is a plugin binary built against an OLDER Ofs.Api
that is NOT recompiled. Rather than commit such a binary, we reproduce one from git:
the most recent release tag still holds that exact Ofs.Api source and a plugin to
build against it. We `git archive` those subtrees at the tag and `dotnet build` them
into a staging dir; the plugin suite then loads that binary against the *current*
host (see tests/plugins/test_plugin_compat.cpp).

Same-major selection
--------------------
The release *tag* names the app version (e.g. v0.3.0); the compatibility contract is
the Ofs.Api **AssemblyVersion** (e.g. 1.x), which can differ. So we walk tags newest
-first and pick the first whose Ofs.Api AssemblyVersion MAJOR equals the working
tree's. That guarantees the reproduced plugin is same-major — the host must load it —
so the C++ test always expects success. A MAJOR bump naturally yields no match (older
tags carry the previous major) and this no-ops until the first release of the new
major is tagged, exactly mirroring the no-git-tags gotcha in the update checker.

Non-fatal by design
--------------------
This runs as part of every build. A failure to reproduce (no matching tag, archive
miss, or the old source not building under the current SDK) prints a warning, clears
the stage, and exits 0 — it MUST NOT block a developer build. The test then skips with
a message. Only a successfully reproduced binary turns the back-compat check on.
"""

import argparse
import io
import re
import shutil
import subprocess
import sys
import tarfile
from pathlib import Path

# The plugin reproduced as the back-compat witness. Ofs.TestPlugin is purpose-built: its OnLoad drives a
# broad slice of the public API (Commands.Register, Nodes.AddNode, Player.Seek), so a removed/changed
# public member surfaces as a load-time failure when the old binary binds to the current host. Its csproj
# ProjectReferences Ofs.Api, so building it from the tag also builds the tag's Ofs.Api — nothing else needed.
PLUGIN_CSPROJ = "tests/plugins/Ofs.TestPlugin/Ofs.TestPlugin.csproj"

# Subtrees git-archived from the tag. The two Directory.Build.props carry the strict C# policy the plugin
# builds under (tests/plugins' imports managed/'s); include both so the reproduction matches the real build.
# Analyzer strictness is overridden off at build time anyway (see DOTNET_BUILD_PROPS) so newer-SDK analyzer
# rules can't fail an old source tree — the goal is the binary, not a re-litigation of old code style.
ARCHIVE_PATHS = [
    "managed/Directory.Build.props",
    "managed/Ofs.Api",
    "tests/plugins/Directory.Build.props",
    "tests/plugins/Ofs.TestPlugin",
]

# Output assembly/folder name. Renamed off Ofs.TestPlugin so the reproduced binary never collides with the
# live test plugin staged by the other plugin-suite cases (the loader keys off <folder>/<folder>.dll). The
# host namespaces the plugin's commands by this load name, so OnLoad's "ping" becomes "Ofs.CompatPlugin.ping".
COMPAT_NAME = "Ofs.CompatPlugin"

# Records which tag the current stage was built from, so a rerun with the same selected tag skips the
# (slow) dotnet build, and a newly-cut tag triggers a rebuild.
MARKER = ".compat-baseline"

# NB: AssemblyName is injected into the extracted plugin csproj (patch_assembly_name), NOT passed as a
# global -p: property — a global property propagates to the referenced Ofs.Api too, renaming both projects
# to the same assembly and failing the restore with "Ambiguous project name".
DOTNET_BUILD_PROPS = [
    "-p:RunAnalyzers=false",        # old source under a newer analyzer set must not fail the reproduction
    "-p:TreatWarningsAsErrors=false",
    "-p:PassOutputPathToReferencedProjects=false",
]


def warn(msg: str) -> None:
    print(f"[build_compat_plugin] {msg}", file=sys.stderr)


def assembly_major(csproj_text: str) -> int | None:
    """MAJOR of <AssemblyVersion> (the binding identity PluginBootstrapper compares), else <Version>."""
    for tag in ("AssemblyVersion", "Version"):
        m = re.search(rf"<{tag}>\s*(\d+)\.", csproj_text)
        if m:
            return int(m.group(1))
    return None


def git(repo: Path, git_exe: str, *args: str, binary: bool = False) -> subprocess.CompletedProcess:
    return subprocess.run([git_exe, "-C", str(repo), *args],
                          capture_output=True, check=False,
                          text=not binary)


def select_baseline_tag(repo: Path, git_exe: str, current_major: int) -> str | None:
    """Newest release tag whose Ofs.Api AssemblyVersion MAJOR matches the working tree's, or None."""
    # -v:refname gives version-aware ordering; newest first.
    res = git(repo, git_exe, "tag", "--list", "v*", "--sort=-v:refname")
    if res.returncode != 0:
        warn(f"git tag failed: {res.stderr.strip()}")
        return None
    tags = [t for t in res.stdout.splitlines() if t.strip()]
    for tag in tags:
        show = git(repo, git_exe, "show", f"{tag}:managed/Ofs.Api/Ofs.Api.csproj")
        if show.returncode != 0:
            continue  # tag predates the csproj at that path — not a usable baseline
        major = assembly_major(show.stdout)
        if major == current_major:
            return tag
    return None


def extract_tag(repo: Path, git_exe: str, tag: str, dest: Path) -> bool:
    """git-archive ARCHIVE_PATHS at `tag` into `dest`, preserving repo-relative layout."""
    res = git(repo, git_exe, "archive", "--format=tar", tag, "--", *ARCHIVE_PATHS, binary=True)
    if res.returncode != 0:
        # res.stderr is bytes here (binary=True); decode defensively for the message.
        err = res.stderr.decode("utf-8", "replace") if isinstance(res.stderr, bytes) else res.stderr
        warn(f"git archive {tag} failed: {err.strip()}")
        return False
    dest.mkdir(parents=True, exist_ok=True)
    with tarfile.open(fileobj=io.BytesIO(res.stdout), mode="r|") as tar:
        tar.extractall(dest)  # archive paths are repo-relative and trusted (our own tag)
    return True


def patch_assembly_name(csproj: Path) -> None:
    """Rename the extracted plugin's output assembly to COMPAT_NAME so the reproduced binary never collides
    with the live test plugin the other cases stage. Scoped to this csproj (not a global -p:) so the renamed
    identity does not leak onto the referenced Ofs.Api."""
    text = csproj.read_text(encoding="utf-8")
    inject = f"\n  <PropertyGroup>\n    <AssemblyName>{COMPAT_NAME}</AssemblyName>\n  </PropertyGroup>\n"
    csproj.write_text(text.replace("</Project>", inject + "</Project>", 1), encoding="utf-8")


def clear_stage(stage: Path, tag_note: str) -> None:
    """Empty the stage and drop a marker so the C++ test skips and reruns know nothing is built."""
    if stage.exists():
        for child in stage.iterdir():
            shutil.rmtree(child) if child.is_dir() else child.unlink()
    stage.mkdir(parents=True, exist_ok=True)
    (stage / MARKER).write_text(tag_note, encoding="utf-8")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", required=True, type=Path)
    ap.add_argument("--dotnet", required=True)
    ap.add_argument("--git", required=True)
    ap.add_argument("--config", required=True)
    ap.add_argument("--out", required=True, type=Path, help="stage dir; receives <COMPAT_NAME>/ on success")
    a = ap.parse_args()

    repo: Path = a.repo
    stage: Path = a.out

    cur = (repo / "managed/Ofs.Api/Ofs.Api.csproj").read_text(encoding="utf-8")
    current_major = assembly_major(cur)
    if current_major is None:
        warn("could not parse current Ofs.Api AssemblyVersion — skipping back-compat fixture")
        clear_stage(stage, "none")
        return 0

    tag = select_baseline_tag(repo, a.git, current_major)
    if tag is None:
        # No release tagged yet for this Ofs.Api MAJOR (e.g. the first release of a new major before it's
        # tagged). Nothing to witness against yet.
        clear_stage(stage, "none")
        print(f"[build_compat_plugin] no release tag with Ofs.Api major {current_major}; "
              f"back-compat fixture skipped")
        return 0

    # Already built from this tag and the binary is present → nothing to do (keeps incremental builds cheap).
    marker = stage / MARKER
    built_dll = stage / COMPAT_NAME / f"{COMPAT_NAME}.dll"
    if marker.exists() and marker.read_text(encoding="utf-8").strip() == tag and built_dll.exists():
        return 0

    clear_stage(stage, "none")  # marker stays "none" until the build below actually succeeds

    src = stage / "src"
    if not extract_tag(repo, a.git, tag, src):
        return 0  # non-fatal: warned already, test will skip

    csproj = src / PLUGIN_CSPROJ
    if not csproj.exists():
        warn(f"{PLUGIN_CSPROJ} absent at {tag} — back-compat fixture skipped")
        return 0
    patch_assembly_name(csproj)

    out_dir = stage / COMPAT_NAME
    cmd = [a.dotnet, "build", str(csproj), "-c", a.config, "-o", str(out_dir),
           *DOTNET_BUILD_PROPS, "--nologo", "-v", "q"]
    proc = subprocess.run(cmd, check=False)
    if proc.returncode != 0 or not built_dll.exists():
        warn(f"reproducing {COMPAT_NAME} from {tag} failed (dotnet build) — back-compat fixture skipped")
        # Leave the stage cleared (marker 'none') so the test skips rather than loading a partial build.
        if out_dir.exists():
            shutil.rmtree(out_dir)
        return 0

    shutil.rmtree(src, ignore_errors=True)  # keep only the built plugin in the stage
    marker.write_text(tag, encoding="utf-8")
    print(f"[build_compat_plugin] reproduced {COMPAT_NAME} from {tag} "
          f"(Ofs.Api major {current_major}) -> {out_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
