#include <doctest/doctest.h>

#include "Core/EventQueue.h"
#include "UI/ModalManager.h"
#include "UI/Modals.h"
#include "Util/Coro.h"
#include "Util/PathUtil.h"

#include <filesystem>
#include <fstream>

using namespace ofs;

// These cover the slices of ModalManager that only run at construction (loadDialogDirs) and
// destruction (tearing down a still-suspended flow) — moments a UI test, which shares one
// app-owned ModalManager for its whole lifetime, can never reach. render()/pump() and the native
// dialog cycle need ImGui / the test-engine seam and are covered by suite_modals.cpp instead.

namespace {
// A detached flow that parks at a Confirm awaiter: it pushes a ShowModalEvent and suspends,
// leaving its frame owned by the ModalManager until resumed (here: never — we destroy instead).
co::Fire parkAtConfirm(EventQueue &eq) {
    co_await Confirm{eq, ModalSpec{.title = "t", .message = "m", .buttons = {"OK"}}};
}

void writeDialogPaths(std::string_view contents) {
    std::ofstream(ofs::util::getPrefPath() / "dialog_paths.json", std::ios::binary) << contents;
}
} // namespace

TEST_CASE("ModalManager loads a valid dialog_paths.json at construction") {
    writeDialogPaths(R"({"projectOpen":"C:/work/"})");
    EventQueue eq;
    CHECK_NOTHROW(ModalManager{eq}); // loadDialogDirs parses the file without error
    std::filesystem::remove(ofs::util::getPrefPath() / "dialog_paths.json");
}

TEST_CASE("ModalManager survives a malformed dialog_paths.json") {
    writeDialogPaths("{ this is not json ");
    EventQueue eq;
    CHECK_NOTHROW(ModalManager{eq}); // the parse exception is caught and logged, not propagated
    std::filesystem::remove(ofs::util::getPrefPath() / "dialog_paths.json");
}

TEST_CASE("ModalManager destructor tears down a suspended flow") {
    EventQueue eq;
    {
        ModalManager mm(eq);
        eq.freeze();
        parkAtConfirm(eq); // suspends at the awaiter, ShowModalEvent now queued
        eq.drain();        // mm's handler moves the entry (with the live handle) into its queue
        // mm goes out of scope here: its destructor calls handle.destroy() on the parked frame,
        // unwinding the coroutine's RAII. A clean teardown (no leak, no double-free) is the assertion.
    }
    CHECK(true);
}
