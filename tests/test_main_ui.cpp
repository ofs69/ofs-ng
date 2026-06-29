#include "App/OfsApp.h"
#include "Core/EventQueue.h"
#include "Core/ScriptProject.h"
#include "Localization/Translator.h"
#include "Services/UpdateChecker.h"
#include "Util/CrashHandler.h"
#include "Util/Log.h"
#include "Util/PathUtil.h"
#include "helpers/OfsAppTestAccess.h"
#include "helpers/TestState.h"
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_video.h>
#include <filesystem>
#include <imgui.h>
#include <imgui_te_context.h>
#include <imgui_te_coroutine.h>
#include <imgui_te_engine.h>
#include <imgui_te_exporters.h>
#include <optional>
#include <string_view>

void RegisterAllTests(ImGuiTestEngine *engine);

static ofs::ScriptProject g_project;
static ofs::EventQueue g_eventQueue;
static TestSharedState g_testState;

TestSharedState &getTestState() {
    return g_testState;
}

class OfsTestApp : public OfsApp {
  public:
    explicit OfsTestApp(const char *testFilter) : OfsApp(g_project, g_eventQueue), testFilter_(testFilter) {}

    ~OfsTestApp() override {
        if (testEngine_) {
            // Stop then destroy here, in the most-derived dtor, so the engine is torn down before the
            // base ~Application runs shutdownImGui(). Destroying the engine while the ImGui context is
            // still alive is the safe order — DestroyContext() unbinds itself from it. The library's
            // usual "destroy ImGui first" rule only guards saved-ini, which ConfigSavedSettings=false
            // disables, so this ordering is explicitly sanctioned.
            ImGuiTestEngine_Stop(testEngine_);
            ImGuiTestEngine_DestroyContext(testEngine_);
        }
    }

    bool init() override {
        if (!OfsApp::init())
            return false;

        g_testState.project = &g_project;
        g_testState.eventQueue = &g_eventQueue;
        g_testState.pluginManager = &OfsAppTestAccess::pluginManager(*this);
        g_testState.commandRegistry = &OfsAppTestAccess::commandRegistry(*this);
        g_testState.bindingSystem = &OfsAppTestAccess::bindingSystem(*this);
        g_testState.effectRegistry = &OfsAppTestAccess::effectRegistry(*this);
        g_testState.scriptRegistry = &OfsAppTestAccess::scriptRegistry(*this);
        g_testState.notifications = &OfsAppTestAccess::notifications(*this);
        g_testState.videoPlayer = OfsAppTestAccess::videoPlayer(*this);
        g_testState.appSettings = &OfsAppTestAccess::appSettings(*this);
        g_testState.processingPanel = &OfsAppTestAccess::processingPanel(*this);
        g_testState.app = this;

        // Keep the update checker off the real network for the whole suite: the silent startup check
        // (and any test that doesn't install its own override) resolves through this stub, which reports
        // an empty feed instead of reaching api.github.com. The updates suite installs its own override.
        ofs::setUpdateFetchOverrideForTesting([] { return std::optional<std::string>{}; });

        testEngine_ = ImGuiTestEngine_CreateContext();
        ImGuiTestEngineIO &io = ImGuiTestEngine_GetIO(testEngine_);
        io.ConfigRunSpeed = ImGuiTestRunSpeed_Fast;
        io.ConfigSavedSettings = false;
        io.CoroutineFuncs = Coroutine_ImplStdThread_GetInterface();
        ImGuiTestEngine_Start(testEngine_, ImGui::GetCurrentContext());
        RegisterAllTests(testEngine_);
        ImGuiTestEngine_QueueTests(testEngine_, ImGuiTestGroup_Tests, testFilter_);
        return true;
    }

    int exitCode() const {
        ImGuiTestEngineResultSummary s;
        ImGuiTestEngine_GetResultSummary(testEngine_, &s);
        // CountTested > 0 guards against a mistyped --test-filter that matches nothing: with an empty
        // queue every count is 0, which would otherwise read as a vacuous pass.
        return (s.CountTested > 0 && s.CountSuccess == s.CountTested && s.CountInQueue == 0) ? 0 : 1;
    }

  protected:
    void onPostRender() override {
        OfsApp::onPostRender();
        ImGuiTestEngine_PostSwap(testEngine_);
        // The loop runs unthrottled: Application::updateSwapInterval forces swap interval 0 under
        // OFS_TEST_ENGINE, so frames are never gated on a vblank and the suite runs at full GPU speed.
        if (ImGuiTestEngine_IsTestQueueEmpty(testEngine_)) {
            logResults();
            stop();
        }
    }

  private:
    ImGuiTestEngine *testEngine_ = nullptr;
    const char *testFilter_ = nullptr;

    void logResults() {
        // The runtime --log-level filter quiets the app's per-frame logging, but the
        // test report is the whole point of the run — force it visible, then restore.
        auto &logger = *ofs::Log::getCoreLogger();
        const auto prevLevel = logger.level();
        logger.set_level(spdlog::level::info);

        ImVector<ImGuiTest *> tests;
        ImGuiTestEngine_GetTestList(testEngine_, &tests);
        for (ImGuiTest *t : tests) {
            if (t->Output.Status == ImGuiTestStatus_Success) {
                OFS_CORE_INFO("[PASS] {}/{}", t->Category, t->Name);
            } else if (t->Output.Status == ImGuiTestStatus_Error) {
                OFS_CORE_ERROR("[FAIL] {}/{}", t->Category, t->Name);
                if (!t->Output.Log.IsEmpty())
                    OFS_CORE_ERROR("{}", t->Output.Log.GetText());
            }
        }
        ImGuiTestEngine_PrintResultSummary(testEngine_);
        logger.set_level(prevLevel);
    }
};

// Map a --log-level= value to an spdlog level. Defaults to warn so the app's
// per-frame logging (FrameAllocator peaks, undo churn, …) stays out of the way;
// the test report is forced visible separately in logResults().
static spdlog::level::level_enum parseLogLevel(std::string_view v) {
    if (v == "off" || v == "none")
        return spdlog::level::off;
    if (v == "err" || v == "error")
        return spdlog::level::err;
    if (v == "warn")
        return spdlog::level::warn;
    if (v == "info")
        return spdlog::level::info;
    if (v == "debug")
        return spdlog::level::debug;
    if (v == "trace")
        return spdlog::level::trace;
    return spdlog::level::warn;
}

int main(int argc, char *argv[]) {
    // The pref path resolves to a temp directory at compile time via the
    // OFS_TEST_PREF_SUBDIR define (tests/CMakeLists.txt), so the tests never write
    // settings.json/imgui.ini/the log into the real app data dir — and never record
    // the test fixture into lastProjectPaths, which previously made the next real
    // app launch auto-open the test project.
    //
    // Wipe that dir up front so persisted state (layouts.json, settings.json, imgui.ini)
    // never leaks between runs and every run starts from a deterministic clean slate.
    // has_filename() guards against ever pointing at a drive/temp root.
    if (const auto prefDir = ofs::util::getPrefPath(); prefDir.has_filename()) {
        std::error_code ec;
        std::filesystem::remove_all(prefDir, ec);
        std::filesystem::create_directories(prefDir, ec);
    }

    ofs::Log::init();
    // Catches both asserts and hardware faults (e.g. a null deref in a test) and prints a symbolized
    // trace — without the minidump/MessageBox that would hang a headless run. The engine hook records
    // which test was running and exports partial results before the trace is printed.
    ofs::installCrashHandlerHeadless(ImGuiTestEngine_CrashHandler);

    const char *filter = nullptr;
    std::string_view language;                                // empty => built-in English
    spdlog::level::level_enum logLevel = spdlog::level::warn; // quiet by default
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg.starts_with("--test-filter="))
            filter = argv[i] + sizeof("--test-filter=") - 1;
        else if (arg.starts_with("--language="))
            language = arg.substr(sizeof("--language=") - 1);
        else if (arg.starts_with("--log-level="))
            logLevel = parseLogLevel(arg.substr(sizeof("--log-level=") - 1));
    }
    ofs::Log::getCoreLogger()->set_level(logLevel);

    OfsTestApp app(filter);
    // Run the whole suite under a translation to prove every localized widget keeps a stable,
    // language-independent ###id: the suites address widgets by id, never by visible text, so a
    // pass here means the translated labels never changed widget identity. Load the language BEFORE
    // init() — the wiped test pref dir carries no language setting, so OfsApp::init's own loader is a
    // no-op and leaves this in place. This mirrors the real app (active language known at init time) so
    // a CJK language eager-loads the CJK font before the first frame instead of rendering it as boxes.
    if (!language.empty() && !ofs::loc::Translator::instance().load(language)) {
        OFS_CORE_ERROR("ui-tests: failed to load language '{}'", language);
        return -1;
    }
    if (!app.init())
        return -1;
    app.run();
    return app.exitCode();
}
