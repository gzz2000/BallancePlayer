#ifdef PLAYER_WEB_INTERACTIVE
extern "C" void SdlInputManagerSetWebDeadline(unsigned long long);
#endif
#include "GamePlayer.h"
#include "PlayerOptions.h"
#if __has_include("CKSortPolicy.h")
#include "CKSortPolicy.h"
#define PLAYER_HAS_CONTEXT_SORT_POLICY 1
#endif

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#if defined(__APPLE__) || defined(__linux__)
#include <dlfcn.h>
#endif
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_video.h>

#ifdef BALLANCE_STATIC_MODULES
#include "StaticPlugins.h"
#endif
#include "Logger.h"
#include "PlayerSdlShortcuts.h"
#include "Utils.h"
#include "InterfaceManager.h"

#define ARRAY_NUM(Array) \
    (sizeof(Array) / sizeof(Array[0]))

namespace {
constexpr uint64_t kNanosecondsPerSecond = 1000000000ULL;
constexpr uint64_t kBehaviorRate = 264ULL;
// Script, animation and audio-facing time advances once per 264 Hz sample.
// Ballance's two physics ticks per sample are configured independently below.
constexpr float kBehaviorDeltaMs = 500.0f / 132.0f;
constexpr uint64_t kMaximumSchedulerBacklogNs = 250000000ULL;
constexpr uint64_t kDisplayProbeIntervalNs = 1000000000ULL;
}

extern bool EditScript(CKLevel *level, const CGameConfig &config, const char *resolvedFile);

static CKSTRING ToCKString(const char *value)
{
    return const_cast<CKSTRING>(value);
}

// Resolve against the actual plugin instance. A host without the negotiated
// interface keeps its ordinary rendering path; no stale function pointer is
// retained across ClearAll/plugin unload or another game load.
static void *BmlFunction(const char *name)
{
    auto *plugins = CKGetPluginManager();
    auto *entry = plugins ? plugins->FindComponent(CKGUID(0x32a40332, 0x3bf12a51)) : nullptr;
    auto *dll = entry ? plugins->GetPluginDllInfo(entry->m_PluginDllIndex) : nullptr;
    return dll ? reinterpret_cast<void *>(dll->GetFunctionPtr(ToCKString(name))) : nullptr;
}

using ConfigureRenderSkip = CKERROR (*)(CKRenderContext *, CKBOOL);
using ConsumeRenderSkip = CKBOOL (*)(CKRenderContext *);

static void PushDiagnosticKey(SDL_Window *window, SDL_Scancode scancode)
{
    if (!window)
        return;

    for (int down = 1; down >= 0; --down)
    {
        SDL_Event event = {};
        event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
        event.key.timestamp = SDL_GetTicksNS();
        event.key.windowID = SDL_GetWindowID(window);
        event.key.scancode = scancode;
        event.key.key = SDL_GetKeyFromScancode(scancode, SDL_KMOD_NONE, false);
        event.key.down = down != 0;
        SDL_PushEvent(&event);
    }
}

static void RunDiagnosticMenuAutomation(SDL_Window *window)
{
    // Opt-in test hook for VNC sessions where macOS refuses synthetic keyboard
    // input.  It follows the default selections: Start, then Level 1.
    static const bool enabled = getenv("BALLANCE_AUTOMATE_START") != NULL;
    static const Uint64 startedAt = SDL_GetTicks();
    static const Uint64 firstActionAt = []() -> Uint64 {
        const char *value = getenv("BALLANCE_AUTOMATE_START_DELAY_MS");
        if (!value || !*value)
            return 6200;
        const unsigned long long parsed = strtoull(value, nullptr, 10);
        return parsed > 0 ? (Uint64)parsed : 6200;
    }();
    static const bool skipTutorial = getenv("BALLANCE_AUTOMATE_SKIP_TUTORIAL") != NULL;
    static int step = 0;
    if (!enabled)
        return;

    const Uint64 elapsed = SDL_GetTicks() - startedAt;
    if (step == 0 && elapsed >= firstActionAt)
    {
        PushDiagnosticKey(window, SDL_SCANCODE_RETURN);
        CLogger::Get().Info("Diagnostic automation: selected Start");
        ++step;
    }
    else if (step == 1 && elapsed >= firstActionAt + 2500)
    {
        PushDiagnosticKey(window, SDL_SCANCODE_RETURN);
        CLogger::Get().Info("Diagnostic automation: selected Level 1");
        ++step;
    }
    else if (step == 2 && skipTutorial && elapsed >= firstActionAt + 15000)
    {
        PushDiagnosticKey(window, SDL_SCANCODE_Q);
        CLogger::Get().Info("Diagnostic automation: skipped Level 1 tutorial");
        ++step;
    }
}

static void RunDiagnosticResizeAutomation(SDL_Window *window)
{
    // Opt-in resize hook used by the native packaging smoke test.  Keeping it
    // inside SDL also works on VNC sessions where macOS Accessibility refuses
    // to resize the app window.
    static const char *requestedSize = getenv("BALLANCE_AUTOMATE_RESIZE");
    static const Uint64 startedAt = SDL_GetTicks();
    static bool finished = false;
    if (finished || !requestedSize || !*requestedSize || !window ||
        SDL_GetTicks() - startedAt < 1000)
        return;

    int width = 0;
    int height = 0;
    if (sscanf(requestedSize, "%dx%d", &width, &height) == 2 &&
        width > 0 && height > 0 && SDL_SetWindowSize(window, width, height))
    {
        SDL_SyncWindow(window);
        CLogger::Get().Info("Diagnostic automation: resized window to %dx%d", width, height);
    }
    else
    {
        CLogger::Get().Error("Diagnostic automation: failed to resize window to '%s': %s",
                             requestedSize, SDL_GetError());
    }
    finished = true;
}

static bool RunDiagnosticQuitAutomation()
{
    static const char *requestedDelay = getenv("BALLANCE_AUTOMATE_QUIT_AFTER_MS");
    static const Uint64 startedAt = SDL_GetTicks();
    static const Uint64 delay = requestedDelay && *requestedDelay
        ? (Uint64)strtoull(requestedDelay, nullptr, 10) : 0;
    return delay > 0 && SDL_GetTicks() - startedAt >= delay;
}

static void RunDiagnosticPracticeAutomation()
{
#if defined(__APPLE__) || defined(__linux__)
    // Comma-separated absolute wall-clock actions used by the native snapshot
    // regression test, for example: "30000:S,31500:R,32500:R,34000:S,35500:R".
    // Calling the mod's public automation entry points avoids synthesizing F
    // keys and therefore also works in headless/VNC sessions.
    static const char *sequence = getenv("BALLANCE_AUTOMATE_PRACTICE_SEQUENCE");
    static const char *cursor = sequence;
    static const Uint64 startedAt = SDL_GetTicks();
    if (!cursor || !*cursor)
        return;

    char *end = nullptr;
    const unsigned long long deadline = strtoull(cursor, &end, 10);
    if (end == cursor || *end != ':' || !end[1]) {
        CLogger::Get().Error("Diagnostic practice automation: malformed sequence near '%s'", cursor);
        cursor = nullptr;
        return;
    }
    if (SDL_GetTicks() - startedAt < deadline)
        return;

    const char action = end[1];
    const char *symbol = action == 'S' || action == 's'
        ? "BML_PracticeCaptureState"
        : action == 'R' || action == 'r'
            ? "BML_PracticeRestoreState" : nullptr;
    int result = 0;
    if (symbol) {
        using PracticeAction = int (*)();
        PracticeAction function = reinterpret_cast<PracticeAction>(
            dlsym(RTLD_DEFAULT, symbol));
#if defined(__APPLE__)
        // BML intentionally loads mods RTLD_LOCAL. Locate the already-loaded
        // image rather than weakening isolation for every third-party mod.
        if (!function) {
            for (uint32_t index = 0; index < _dyld_image_count(); ++index) {
                const char *image = _dyld_get_image_name(index);
                if (!image || !strstr(image, "PracticeTools.bmodp"))
                    continue;
                void *handle = dlopen(image, RTLD_LAZY | RTLD_NOLOAD);
                if (handle)
                    function = reinterpret_cast<PracticeAction>(dlsym(handle, symbol));
                if (function)
                    break;
            }
        }
#endif
        if (function)
            result = function();
        else
            CLogger::Get().Error("Diagnostic practice automation: %s is unavailable", symbol);
    } else {
        CLogger::Get().Error("Diagnostic practice automation: unknown action '%c'", action);
    }
    CLogger::Get().Info("Diagnostic practice automation: %c at %llu ms returned %d",
                        action, deadline, result);

    const char *next = strchr(end + 2, ',');
    cursor = next ? next + 1 : nullptr;
#endif
}

static int AsciiICompare(const char *lhs, const char *rhs)
{
    if (!lhs || !rhs)
        return lhs == rhs ? 0 : (lhs ? 1 : -1);
    while (*lhs && *rhs)
    {
        int lc = tolower(static_cast<unsigned char>(*lhs));
        int rc = tolower(static_cast<unsigned char>(*rhs));
        if (lc != rc)
            return lc - rc;
        ++lhs;
        ++rhs;
    }
    return tolower(static_cast<unsigned char>(*lhs)) - tolower(static_cast<unsigned char>(*rhs));
}

static bool IsDefaultRenderEngineDll(const char *dllPath)
{
    if (!dllPath || !*dllPath)
        return false;

    const char *name = utils::FindLastPathSeparator(dllPath);
    name = name ? name + 1 : dllPath;

    XString filename = name;
    XWORD extension = filename.RFind('.');
    if (extension != XString::NOTFOUND)
        filename.Cut(extension, filename.Length() - extension);

    name = filename.CStr();
    if (strlen(name) >= 3 &&
        tolower(static_cast<unsigned char>(name[0])) == 'l' &&
        tolower(static_cast<unsigned char>(name[1])) == 'i' &&
        tolower(static_cast<unsigned char>(name[2])) == 'b')
        name += 3;

    return AsciiICompare(name, "CK2_3D") == 0;
}

static int FindHighestRefreshMatch(const VxDisplayMode *modes, int modeCount,
                                   int width, int height, int bpp,
                                   bool exactBpp)
{
    if (!modes)
        return -1;

    int bestIndex = -1;
    int bestRefresh = -1;
    for (int i = 0; i < modeCount; ++i)
    {
        const VxDisplayMode &mode = modes[i];
        if (mode.Width != width || mode.Height != height)
            continue;

        if (exactBpp)
        {
            if (mode.Bpp != bpp)
                continue;
        }
        else if (mode.Bpp <= 8)
        {
            continue;
        }

        if (mode.RefreshRate > bestRefresh)
        {
            bestRefresh = mode.RefreshRate;
            bestIndex = i;
        }
    }
    return bestIndex;
}

static int FindBestDisplayModeIndex(const VxDisplayMode *modes, int modeCount,
                                    int width, int height, int bpp)
{
    int index = FindHighestRefreshMatch(modes, modeCount, width, height, bpp, true);
    if (index >= 0)
        return index;

    if (bpp != 32)
    {
        index = FindHighestRefreshMatch(modes, modeCount, width, height, 32, true);
        if (index >= 0)
            return index;
    }

    return FindHighestRefreshMatch(modes, modeCount, width, height, bpp, false);
}

static bool ResolveRuntimePath(const CGameConfig &config, PathCategory category, XString &resolvedPath, bool trailing = false)
{
    return config.ResolvePath(category, resolvedPath, trailing);
}

static bool GetExecutableDirectory(XString &path)
{
    path = utils::GetExecutableDirectory();
    if (path.IsEmpty())
        return false;
    return true;
}

static bool ParsePluginsFromExecutableDirectory(CKPluginManager *pluginManager)
{
    XString path;
    if (!pluginManager || !GetExecutableDirectory(path))
        return false;

    if (!utils::DirectoryExists(path.CStr()))
        return false;

    pluginManager->ParsePlugins(ToCKString(path.CStr()));
    return true;
}

static bool AddPathIfMissing(CKPathManager *pathManager, int category, const char *path)
{
    if (!pathManager || !path || !*path)
        return false;

    XString ckPath = path;
    if (pathManager->GetPathIndex(category, ckPath) >= 0)
        return true;

    return pathManager->AddPath(category, ckPath) >= 0;
}

static void AddDirectoryPathIfExists(CKPathManager *pathManager, int category, const char *basePath, const char *relativePath)
{
    XString path = utils::JoinPath(basePath, relativePath, true);
    if (path.IsEmpty())
        return;
    if (utils::DirectoryExists(path.CStr()))
        AddPathIfMissing(pathManager, category, path.CStr());
}

static void RegisterCompositionPaths(CKPathManager *pathManager, const char *resolvedFile)
{
    XString compositionDir = utils::GetFileDirectory(resolvedFile, true);
    if (compositionDir.IsEmpty())
        return;

    AddPathIfMissing(pathManager, DATA_PATH_IDX, compositionDir.CStr());
    AddDirectoryPathIfExists(pathManager, DATA_PATH_IDX, compositionDir.CStr(), "3D Entities");

    AddPathIfMissing(pathManager, SOUND_PATH_IDX, compositionDir.CStr());
    AddDirectoryPathIfExists(pathManager, SOUND_PATH_IDX, compositionDir.CStr(), "Sounds");
    AddDirectoryPathIfExists(pathManager, SOUND_PATH_IDX, compositionDir.CStr(), "Sounds_low");

    AddPathIfMissing(pathManager, BITMAP_PATH_IDX, compositionDir.CStr());
    AddDirectoryPathIfExists(pathManager, BITMAP_PATH_IDX, compositionDir.CStr(), "Textures");
}

static bool SetCompositionEnvironment(const char *resolvedFile)
{
    XString compositionDir = utils::GetFileDirectory(resolvedFile, true);
    if (compositionDir.IsEmpty())
        return false;

#if defined(_WIN32)
    if (_putenv_s("Gravity", compositionDir.CStr()) != 0)
        return false;
#else
    if (setenv("Gravity", compositionDir.CStr(), 1) != 0)
        return false;
#endif

    return true;
}

static void ConfigureSdlVideoDriver()
{
#if defined(__linux__)
    SDL_SetHintWithPriority(SDL_HINT_VIDEO_DRIVER, "x11,wayland", SDL_HINT_DEFAULT);
#endif
}

CGamePlayer::CGamePlayer()
    : m_State(eInitial),
      m_MainWindow(NULL),
      m_RenderWindow(NULL),
      m_SdlWindow(NULL),
      m_CKContext(NULL),
      m_RenderContext(NULL),
      m_RenderManager(NULL),
      m_MessageManager(NULL),
      m_TimeManager(NULL),
      m_AttributeManager(NULL),
      m_InputManager(NULL),
      m_MsgClick(-1),
      m_MsgDoubleClick(-1),
      m_GameInfo(NULL),
      m_AltEnterShortcutDown(false),
      m_AltF4ShortcutDown(false),
      m_NextBehaviorDeadlineNs(0),
      m_BehaviorDeadlineRemainder(0),
      m_BehaviorRate(kBehaviorRate),
      m_RuntimeScheduleRevision(0),
      m_NextRenderDeadlineNs(0),
      m_DisplayRenderPeriodNs(kNanosecondsPerSecond / 60),
      m_RenderPeriodNs(kNanosecondsPerSecond / 60),
      m_LastDisplayProbeNs(0),
      m_ScheduledDisplay(0),
      m_ScheduledRefreshNumerator(0),
      m_ScheduledRefreshDenominator(1) {}

CGamePlayer::~CGamePlayer()
{
    Shutdown();
}

bool CGamePlayer::Init(const CGameConfig &config)
{
    return Init(config, config);
}

bool CGamePlayer::Init(const CGameConfig &runtimeConfig, const CGameConfig &persistentConfig)
{
    if (m_State != eInitial)
        return true;

    m_Config = runtimeConfig;
    m_PersistentConfig = persistentConfig;

    if (!InitWindow())
    {
        CLogger::Get().Error("Failed to initialize window!");
        ShutdownWindow();
        return false;
    }
    if (!InitEngine(m_MainWindow))
    {
        CLogger::Get().Error("Failed to initialize CK Engine!");
        return false;
    }

    if (!InitDriver())
    {
        CLogger::Get().Error("Failed to initialize Render Driver!");
        Shutdown();
        return false;
    }

    CKRECT rc = {0, 0, m_Config.width, m_Config.height};
    if (m_SdlWindow && m_MainWindow && VxGetClientRect(m_MainWindow, &rc) &&
        (rc.right - rc.left != m_Config.width || rc.bottom - rc.top != m_Config.height))
    {
        ResizeWindow();
    }

    WIN_HANDLE handle = (!m_Config.childWindowRendering) ? m_MainWindow : m_RenderWindow;
    // CK/Virtools window coordinates are SDL logical window coordinates.  The
    // rasterizer owns the separate drawable-pixel size used by Retina/HiDPI
    // swapchains.  Mixing the two makes a resize change the game's coordinate
    // system and desynchronizes 2D picking from SDL mouse coordinates.
    CKRECT rect = {0, 0, m_Config.width, m_Config.height};
    m_RenderContext = m_RenderManager->CreateRenderContext(handle, m_Config.driver, &rect, FALSE, m_Config.bpp);
    if (!m_RenderContext)
    {
        CLogger::Get().Error("Failed to create Render Context!");
        return false;
    }

    CLogger::Get().Debug("Render Context created.");

    if (m_Config.fullscreen)
        OnGoFullscreen(false);

    if (m_SdlWindow)
    {
        SDL_ShowWindow(m_SdlWindow);
        SDL_RaiseWindow(m_SdlWindow);
        // Re-query after the hidden Cocoa window has been attached to its
        // display.  OnSize deliberately keeps CK in logical coordinates; the
        // rasterizer tracks the drawable-pixel size independently.
        OnSize();
    }

    m_State = eReady;
    return true;
}

bool CGamePlayer::Load(const char *filename)
{
    m_AfterProcessStartup = nullptr;
    m_ProcessStartupFailed = false;
    if (m_State == eInitial)
    {
        CLogger::Get().Error("Player is not initialized!");
        return false;
    }

    if (!filename || (*filename) == '\0')
    {
        filename = m_Config.GetPath(eCmoPath);
        if (!filename || (*filename) == '\0')
        {
            CLogger::Get().Error("No filename specified!");
            return false;
        }
    }

    if (!m_CKContext)
    {
        CLogger::Get().Error("CKContext is not initialized!");
        return false;
    }

    CKPathManager *pm = m_CKContext->GetPathManager();
    if (!pm)
    {
        CLogger::Get().Error("Failed to get CKPathManager!");
        return false;
    }

    XString resolvedFile = filename;
    CKERROR err = pm->ResolveFileName(resolvedFile, DATA_PATH_IDX);
    if (err != CK_OK)
    {
        CLogger::Get().Error("Failed to resolve filename %s", filename);
        return false;
    }

    RegisterCompositionPaths(pm, resolvedFile.CStr());
    if (!SetCompositionEnvironment(resolvedFile.CStr()))
        CLogger::Get().Warn("Failed to set composition environment path.");

    CLogger::Get().Info("Load: context reset begin");
    m_CKContext->Reset();
    m_CKContext->ClearAll();
    CLogger::Get().Info("Load: context reset complete");

    // Load the file and fills the array with loaded objects
    CKFile *f = m_CKContext->CreateCKFile();
    if (!f)
    {
        CLogger::Get().Error("Failed to create CKFile!");
        return false;
    }

    CLogger::Get().Info("Load: OpenFile begin: %s", resolvedFile.CStr());
    CKERROR res = f->OpenFile(resolvedFile.Str(), (CK_LOAD_FLAGS)(CK_LOAD_DEFAULT | CK_LOAD_CHECKDEPENDENCIES));
    if (res != CK_OK)
    {
        if (res == CKERR_PLUGINSMISSING)
        {
            ReportMissingGuids(f, resolvedFile.CStr());
        }
        m_CKContext->DeleteCKFile(f);

        CLogger::Get().Error("Failed to open file: %s", resolvedFile.CStr());
        return false;
    }
    CLogger::Get().Info("Load: OpenFile complete");

    CKObjectArray *array = CreateCKObjectArray();
    if (!array)
    {
        CLogger::Get().Error("Failed to create CKObjectArray!");
        m_CKContext->DeleteCKFile(f);
        return false;
    }

    CLogger::Get().Info("Load: LoadFileData begin");
    res = f->LoadFileData(array);
    if (res != CK_OK)
    {
        CLogger::Get().Error("Failed to load file: %s", resolvedFile.CStr());
        m_CKContext->DeleteCKFile(f);
        DeleteCKObjectArray(array);
        return false;
    }
    CLogger::Get().Info("Load: LoadFileData complete");

    m_CKContext->DeleteCKFile(f);
    DeleteCKObjectArray(array);

    CLogger::Get().Info("Load: FinishLoad begin");
    const bool finishResult = FinishLoad(filename, resolvedFile.CStr());
    CLogger::Get().Info("Load: FinishLoad complete (%d)", finishResult ? 1 : 0);
    return finishResult;
}

void CGamePlayer::Run()
{
    while (Update())
        continue;
}

void CGamePlayer::ResetFrameScheduler()
{
    const uint64_t now = SDL_GetTicksNS();
    m_NextBehaviorDeadlineNs = now;
    m_BehaviorDeadlineRemainder = 0;
    m_NextRenderDeadlineNs = now;
    m_LastDisplayProbeNs = 0;
    RefreshDisplaySchedule(now);
    RefreshFrameRateSchedule(now);
    if (m_TimeManager)
        m_RuntimeScheduleRevision = m_TimeManager->GetRuntimeScheduleRevision();
}

void CGamePlayer::RefreshDisplaySchedule(uint64_t now)
{
    if (!m_SdlWindow)
        return;

    const SDL_DisplayID display = SDL_GetDisplayForWindow(m_SdlWindow);
    const SDL_DisplayMode *mode = display ? SDL_GetCurrentDisplayMode(display) : NULL;
    int numerator = mode ? mode->refresh_rate_numerator : 0;
    int denominator = mode ? mode->refresh_rate_denominator : 1;
    if (numerator <= 0 && mode && mode->refresh_rate > 0.0f) {
        numerator = (int)lroundf(mode->refresh_rate * 1000.0f);
        denominator = 1000;
    }
    if (numerator <= 0 || denominator <= 0) {
        numerator = 60;
        denominator = 1;
    }

    m_LastDisplayProbeNs = now;
    if (m_ScheduledDisplay == display &&
        m_ScheduledRefreshNumerator == numerator &&
        m_ScheduledRefreshDenominator == denominator)
        return;

    m_ScheduledDisplay = display;
    m_ScheduledRefreshNumerator = numerator;
    m_ScheduledRefreshDenominator = denominator;
    m_DisplayRenderPeriodNs = (uint64_t)llround(
        (double)kNanosecondsPerSecond * (double)denominator / (double)numerator);
    if (!m_DisplayRenderPeriodNs)
        m_DisplayRenderPeriodNs = 1;
    m_NextRenderDeadlineNs = now;
    CLogger::Get().Info("Display-locked rendering: %.3f Hz",
                        (double)numerator / (double)denominator);
}

void CGamePlayer::RefreshFrameRateSchedule(uint64_t now)
{
    uint64_t behaviorRate = kBehaviorRate;
    uint64_t renderPeriodNs = m_DisplayRenderPeriodNs;

    // Presentation limits and the deterministic simulation rate are separate.
    // Ballance's opening script sets a 60 Hz presentation limit of its own; it
    // must not slow the 264 Hz script timeline. Native BML+ practice controls
    // request slow motion explicitly through FixedProcessRate instead.
    if (m_TimeManager)
    {
        const float requestedRate = m_TimeManager->GetFixedProcessRate();
        if (requestedRate > 0.0f && isfinite(requestedRate))
        {
            uint64_t rate = (uint64_t)llroundf(requestedRate);
            if (rate < 1)
                rate = 1;
            behaviorRate = rate < kBehaviorRate ? rate : kBehaviorRate;
        }
    }

    if (m_TimeManager &&
        (m_TimeManager->GetLimitOptions() & CK_FRAMERATE_MASK) == CK_FRAMERATE_LIMIT)
    {
        const float requestedLimit = m_TimeManager->GetFrameRateLimit();
        if (requestedLimit > 0.0f && isfinite(requestedLimit))
        {
            const uint64_t requestedRenderPeriod =
                (uint64_t)ceil((double)kNanosecondsPerSecond / (double)requestedLimit);
            if (requestedRenderPeriod > renderPeriodNs)
                renderPeriodNs = requestedRenderPeriod;
        }
    }

    if (behaviorRate != m_BehaviorRate)
    {
        m_BehaviorRate = behaviorRate;
        m_BehaviorDeadlineRemainder = 0;
        m_NextBehaviorDeadlineNs = now;
        AdvanceBehaviorDeadline();
        CLogger::Get().Info("Deterministic gameplay schedule: %llu samples/s",
                            (unsigned long long)m_BehaviorRate);
    }

    if (renderPeriodNs != m_RenderPeriodNs)
    {
        m_RenderPeriodNs = renderPeriodNs;
        m_NextRenderDeadlineNs = now;
    }
}

void CGamePlayer::AdvanceBehaviorDeadline()
{
    m_NextBehaviorDeadlineNs += kNanosecondsPerSecond / m_BehaviorRate;
    m_BehaviorDeadlineRemainder += kNanosecondsPerSecond % m_BehaviorRate;
    if (m_BehaviorDeadlineRemainder >= m_BehaviorRate) {
        ++m_NextBehaviorDeadlineNs;
        m_BehaviorDeadlineRemainder -= m_BehaviorRate;
    }
}

bool CGamePlayer::Update()
{
    if (m_ProcessStartupFailed)
        return false;
    DrainPlayerCommands();

    SDL_PumpEvents();
    RunDiagnosticMenuAutomation(m_SdlWindow);
    RunDiagnosticResizeAutomation(m_SdlWindow);
    RunDiagnosticPracticeAutomation();
    if (RunDiagnosticQuitAutomation())
        return false;
    SDL_Event event;
    if (SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_EVENT_QUIT, SDL_EVENT_QUIT) > 0)
        return false;

    SDL_Event windowEvents[16];
    int windowEventCount = SDL_PeepEvents(windowEvents, 16, SDL_GETEVENT,
                                          SDL_EVENT_WINDOW_FIRST, SDL_EVENT_WINDOW_LAST);
    for (int i = 0; i < windowEventCount; ++i)
    {
        const SDL_Event &windowEvent = windowEvents[i];
        if (!m_SdlWindow || windowEvent.window.windowID != SDL_GetWindowID(m_SdlWindow))
            continue;

        switch (windowEvent.type)
        {
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            return false;
        case SDL_EVENT_WINDOW_RESIZED:
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            OnSize();
            break;
        case SDL_EVENT_WINDOW_MOVED:
            OnMove();
            break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
            OnActivateApp(true);
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            OnActivateApp(false);
            break;
        default:
            break;
        }
    }

    player_sdl_shortcuts::KeyboardShortcutState shortcutState = {
        m_AltEnterShortcutDown,
        m_AltF4ShortcutDown,
    };
    switch (player_sdl_shortcuts::UpdateKeyboardShortcuts(SDL_GetKeyboardState(NULL),
                                                          SDL_GetModState(),
                                                          shortcutState))
    {
    case player_sdl_shortcuts::KeyboardShortcutToggleFullscreen:
        OnSwitchFullscreen();
        break;
    case player_sdl_shortcuts::KeyboardShortcutQuit:
        return false;
    case player_sdl_shortcuts::KeyboardShortcutNone:
        break;
    }
    m_AltEnterShortcutDown = shortcutState.altEnterDown;
    m_AltF4ShortcutDown = shortcutState.altF4Down;

    uint64_t now = SDL_GetTicksNS();
    if (!m_NextBehaviorDeadlineNs)
        ResetFrameScheduler();
    if (now - m_LastDisplayProbeNs >= kDisplayProbeIntervalNs)
        RefreshDisplaySchedule(now);
    RefreshFrameRateSchedule(now);

    // Deterministic test/TAS fast-forward.  Fixed CK time and TAS input are
    // consumed once per Process(), so batching simulation samples changes only
    // wall-clock presentation, never the recorded timeline.  Keep one render
    // and one SDL event pump per batch so loading screens and automation can
    // still make progress without paying the display refresh delay.
    static const int fastForwardBatch = []() {
        const char *value = getenv("BALLANCE_FAST_FORWARD_BATCH");
        const int parsed = value ? atoi(value) : 0;
        return parsed > 0 ? parsed : 0;
    }();
    const char *fastForwardActive = getenv("BALLANCE_FAST_FORWARD_ACTIVE");
    if (fastForwardBatch > 0 && fastForwardActive &&
        fastForwardActive[0] == '1')
    {
#ifdef PLAYER_WEB_INTERACTIVE
        SdlInputManagerSetWebDeadline(0);
#endif
        for (int sample = 0; sample < fastForwardBatch; ++sample)
            Process();
        Render();
        m_NextBehaviorDeadlineNs = SDL_GetTicksNS();
        m_BehaviorDeadlineRemainder = 0;
        m_NextRenderDeadlineNs = m_NextBehaviorDeadlineNs;
        return true;
    }

    // Loading a level, moving between displays or suspending the process can
    // leave a large wall-clock gap. Do not fast-forward gameplay to repay it;
    // resume the deterministic timeline from the current instant instead.
    if (now > m_NextBehaviorDeadlineNs &&
        now - m_NextBehaviorDeadlineNs > kMaximumSchedulerBacklogNs) {
        m_NextBehaviorDeadlineNs = now;
        m_BehaviorDeadlineRemainder = 0;
    }
    if (now > m_NextRenderDeadlineNs &&
        now - m_NextRenderDeadlineNs > kMaximumSchedulerBacklogNs)
        m_NextRenderDeadlineNs = now;

    while (now >= m_NextBehaviorDeadlineNs) {
#ifdef PLAYER_WEB_INTERACTIVE
        SdlInputManagerSetWebDeadline(m_NextBehaviorDeadlineNs);
#endif
        AdvanceBehaviorDeadline();
        Process();
        now = SDL_GetTicksNS();
        // A rate change, rewind, or long synchronous practice operation must
        // not be interpreted as simulation debt.  Restart both deadlines at
        // the present wall-clock instant before another game tick can run.
        if (m_TimeManager && m_RuntimeScheduleRevision !=
                m_TimeManager->GetRuntimeScheduleRevision()) {
            ResetFrameScheduler();
            now = SDL_GetTicksNS();
            break;
        }
    }

#ifdef PLAYER_WEB_INTERACTIVE
    // Browser presentation follows rAF; simulation above keeps its independent
    // integer sample clock. Never block the event loop waiting for the next tick.
    Render();
#else
    if (now >= m_NextRenderDeadlineNs) {
        do {
            m_NextRenderDeadlineNs += m_RenderPeriodNs;
        } while (m_NextRenderDeadlineNs <= now);
        Render();
        now = SDL_GetTicksNS();
    }

    const uint64_t nextDeadline = m_NextBehaviorDeadlineNs < m_NextRenderDeadlineNs
        ? m_NextBehaviorDeadlineNs : m_NextRenderDeadlineNs;
    if (nextDeadline > now)
        SDL_DelayPrecise(nextDeadline - now);
#endif

    return true;
}

void CGamePlayer::Process()
{
    const bool wasPlaying = m_CKContext->IsPlaying() != FALSE;
    m_CKContext->Process();
    if (wasPlaying && m_AfterProcessStartup) {
        const CKERROR result = m_AfterProcessStartup(m_CKContext);
        if (result != CK_OK) {
            CLogger::Get().Error("Player-driven BML startup failed after Process: %d", result);
            m_ProcessStartupFailed = true;
            m_CKContext->Pause();
        }
    }
}

void CGamePlayer::Render()
{
    auto consume = reinterpret_cast<ConsumeRenderSkip>(BmlFunction("BML_ConsumePlayerRenderSkipV1"));
    if (consume && consume(m_RenderContext))
        return;
    const CKERROR renderResult = m_RenderContext->Render();
#ifdef PLAYER_WEB_INTERACTIVE
    static CKERROR lastRenderResult = CK_OK;
    if (renderResult != lastRenderResult) {
        CLogger::Get().Info("Browser render status: %d", renderResult);
        lastRenderResult = renderResult;
    }
#endif

    // Diagnostic capture for environments where the window server cannot
    // screenshot a Metal swapchain (for example, some VNC display modes).
    // This is deliberately opt-in and captures the engine backbuffer itself.
    static const char *capturePath = getenv("BALLANCE_CAPTURE_FRAME");
    static int framesUntilCapture = []() {
        const char *value = getenv("BALLANCE_CAPTURE_AFTER");
        int frames = value ? atoi(value) : 120;
        return frames > 0 ? frames : 1;
    }();
    static bool captureFinished = false;
    if (capturePath && *capturePath && !captureFinished && --framesUntilCapture <= 0)
    {
        captureFinished = true;
        CKERROR result = m_RenderContext->DumpToFile(ToCKString(capturePath), NULL,
                                                     VXBUFFER_BACKBUFFER);
        if (result == CK_OK)
            CLogger::Get().Info("Captured diagnostic frame: %s", capturePath);
        else
            CLogger::Get().Error("Failed to capture diagnostic frame (%d): %s",
                                 result, capturePath);
    }
}

void CGamePlayer::Shutdown()
{
    if (m_State != eInitial && !m_PersistentConfig.SaveToIni())
        CLogger::Get().Error("Failed to save config: %s", m_PersistentConfig.GetPath(eConfigPath));

    if (m_GameInfo)
    {
        delete m_GameInfo;
        m_GameInfo = NULL;
    }

    ShutdownEngine();
    ShutdownWindow();

    if (m_State != eInitial)
    {
        m_State = eInitial;
        CLogger::Get().Debug("Player shut down.");
    }
}

void CGamePlayer::Play()
{
    m_State = ePlaying;
    m_CKContext->Play();
    CLogger::Get().Debug("Game is playing.");
}

void CGamePlayer::Pause()
{
    m_State = ePaused;
    m_CKContext->Pause();
    CLogger::Get().Debug("Game is paused.");
}

void CGamePlayer::Reset()
{
    m_State = ePlaying;
    m_CKContext->Reset();
    m_CKContext->Play();
    CLogger::Get().Debug("Game is reset.");
}

static CKERROR LogRedirect(CKUICallbackStruct &cbStruct, void *userData)
{
    if (cbStruct.Reason == CKUIM_OUTTOCONSOLE ||
        cbStruct.Reason == CKUIM_OUTTOINFOBAR ||
        cbStruct.Reason == CKUIM_DEBUGMESSAGESEND)
    {
        static XString text = "";
        if (text.Compare(cbStruct.ConsoleString))
        {
            CLogger::Get().Info(cbStruct.ConsoleString);
            text = cbStruct.ConsoleString;
        }
    }
    return CK_OK;
}

bool CGamePlayer::InitWindow()
{
    SDL_SetMainReady();
    ConfigureSdlVideoDriver();

    CLogger::Get().Info("Window setup: headless=%s rasterizer=%s",
                        getenv("BALLANCE_HEADLESS") ? getenv("BALLANCE_HEADLESS") : "<unset>",
                        m_Config.rasterizer.CStr());

    // The null rasterizer does not consume a native window.  Keeping this
    // opt-in lets deterministic TAS/physics tests run from SSH and CI without
    // an Aqua/Win32 display while leaving every interactive launch unchanged.
    // CK's input plugin is still loaded so the TAS input hook sees the same
    // manager and object graph as a displayed run.
    if (getenv("BALLANCE_HEADLESS") ||
        m_Config.rasterizer.Compare("null") == 0)
    {
        m_Config.childWindowRendering = false;
        m_Config.fullscreen = false;
        m_SdlWindow = NULL;
#ifdef _WIN32
        m_MainWindow = NULL;
#else
        m_MainWindow = NULL;
#endif
        m_RenderWindow = NULL;
        CLogger::Get().Info("Headless null-rasterizer window mode enabled.");
        return true;
    }

    if (!SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS))
    {
        CLogger::Get().Error("Failed to initialize SDL video backend: %s", SDL_GetError());
        return false;
    }

    m_Config.childWindowRendering = false;

    SDL_WindowFlags flags = SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE |
                            SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (m_Config.borderless)
        flags = (SDL_WindowFlags)(flags | SDL_WINDOW_BORDERLESS);
    if (m_Config.fullscreen)
        flags = (SDL_WindowFlags)(flags | SDL_WINDOW_FULLSCREEN);

    m_SdlWindow = SDL_CreateWindow("Ballance", m_Config.width, m_Config.height, flags);
    if (!m_SdlWindow)
    {
        CLogger::Get().Error("Failed to create SDL window: %s", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return false;
    }

    int x = m_Config.posX;
    int y = m_Config.posY;
    if (x == 2147483647 || y == 2147483647)
    {
        SDL_SetWindowPosition(m_SdlWindow, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
        SDL_GetWindowPosition(m_SdlWindow, &x, &y);
    }
    else
    {
        SDL_SetWindowPosition(m_SdlWindow, x, y);
    }

#ifdef _WIN32
    // CK and legacy Windows plugins define WIN_HANDLE as HWND.  Keep SDL's
    // ownership in m_SdlWindow, but publish the native handle through CK so
    // Win32 integrations such as BML+'s ImGui input backend receive the type
    // their ABI promises.  The SDL_gpu rasterizer can map this HWND back to
    // the owning SDL_Window through SDL's window properties.
    m_MainWindow = (WIN_HANDLE)SDL_GetPointerProperty(
        SDL_GetWindowProperties(m_SdlWindow),
        SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    if (!m_MainWindow)
    {
        CLogger::Get().Error("Failed to retrieve Win32 window handle: %s", SDL_GetError());
        SDL_DestroyWindow(m_SdlWindow);
        m_SdlWindow = NULL;
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return false;
    }
#else
    m_MainWindow = (WIN_HANDLE)m_SdlWindow;
#endif
    m_RenderWindow = NULL;

    m_Config.posX = x;
    m_Config.posY = y;

    return true;
}

void CGamePlayer::ShutdownWindow()
{
    if (m_Config.fullscreen)
        RestoreDisplayMode();

    if (m_SdlWindow)
    {
        SDL_DestroyWindow(m_SdlWindow);
        m_SdlWindow = NULL;
    }
    m_MainWindow = NULL;
    m_RenderWindow = NULL;

    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    return;
}

bool CGamePlayer::InitEngine(WIN_HANDLE mainWindow)
{
    CLogger::Get().Info("InitEngine: CKStartUp begin");
    if (CKStartUp() != CK_OK)
    {
        CLogger::Get().Error("CK Engine can not start up!");
        return false;
    }
    CLogger::Get().Info("InitEngine: CKStartUp complete");

    CLogger::Get().Debug("CK Engine starts up successfully.");

    CKPluginManager *pluginManager = CKGetPluginManager();
    CLogger::Get().Info("InitEngine: InitPlugins begin");
    if (!InitPlugins(pluginManager))
    {
        CLogger::Get().Error("Failed to initialize plugins.");
        CKShutdown();
        return false;
    }
    CLogger::Get().Info("InitEngine: InitPlugins complete");

    int renderEngine = FindRenderEngine(pluginManager);
    if (renderEngine == -1)
    {
        CLogger::Get().Error("Failed to initialize render engine.");
        CKShutdown();
        return false;
    }

    CKDWORD contextFlags = 0;
    if (m_Config.legacyTasSort) {
#ifdef PLAYER_HAS_CONTEXT_SORT_POLICY
        contextFlags |= CK_CONFIG_WINE_MSVCRT_SORT;
#else
        CLogger::Get().Error("LegacyTASSort requires the reconstructed CK2 SDK.");
        CKShutdown();
        return false;
#endif
    }
    CLogger::Get().Info("Behavior sort policy: %s",
                        m_Config.legacyTasSort ? "Wine 11 legacy TAS" : "host native");
    CLogger::Get().Debug("Render Engine initialized.");
    CLogger::Get().Info("InitEngine: CKCreateContext begin (render engine %d)", renderEngine);
#if CKVERSION == 0x13022002
    CKERROR res = CKCreateContext(&m_CKContext, mainWindow, renderEngine, contextFlags);
#else
    CKERROR res = CKCreateContext(&m_CKContext, mainWindow, contextFlags);
#endif
    if (res != CK_OK)
    {
        CLogger::Get().Error("Failed to initialize CK Engine.");
        ShutdownEngine();
        return false;
    }
    CLogger::Get().Info("InitEngine: CKCreateContext complete");

    CLogger::Get().Debug("CK Engine initialized.");

    m_CKContext->SetVirtoolsVersion(CK_VIRTOOLS_DEV, 0x2000043);
    m_CKContext->SetInterfaceMode(FALSE, LogRedirect, 0);

    CLogger::Get().Info("InitEngine: SetupManagers begin");
    if (!SetupManagers())
    {
        CLogger::Get().Error("Failed to setup managers.");
        ShutdownEngine();
        return false;
    }
    CLogger::Get().Info("InitEngine: SetupManagers complete");

    CLogger::Get().Info("InitEngine: SetupPaths begin");
    if (!SetupPaths())
    {
        CLogger::Get().Error("Failed to setup paths.");
        ShutdownEngine();
        return false;
    }
    CLogger::Get().Info("InitEngine: SetupPaths complete");

    return true;
}

void CGamePlayer::ShutdownEngine()
{
    // Plugin function pointers cannot outlive the engine's loaded modules.
    m_AfterProcessStartup = nullptr;
    if (m_CKContext)
    {
        if (auto configure = reinterpret_cast<ConfigureRenderSkip>(
                BmlFunction("BML_SetPlayerDrivenRenderSkipV1")))
            configure(m_RenderContext, FALSE);
        m_CKContext->Reset();
        m_CKContext->ClearAll();

        if (m_RenderManager && m_RenderContext)
        {
            m_RenderManager->DestroyRenderContext(m_RenderContext);
            m_RenderContext = NULL;
        }

        CKCloseContext(m_CKContext);
        m_CKContext = NULL;

        m_RenderManager = NULL;
        m_MessageManager = NULL;
        m_TimeManager = NULL;
        m_AttributeManager = NULL;
        m_InputManager = NULL;

        CKShutdown();
    }
}

bool CGamePlayer::InitDriver()
{
    int driverCount = m_RenderManager->GetRenderDriverCount();
    if (driverCount == 0)
    {
        CLogger::Get().Error("No render driver found.");
        return false;
    }

    if (driverCount == 1)
    {
        CLogger::Get().Debug("Found a render driver");
    }
    else
    {
        CLogger::Get().Debug("Found %d render drivers", driverCount);
    }

    if (!m_Config.rasterizer.IsEmpty())
    {
        m_Config.driver = -1;
        for (int i = 0; i < driverCount; ++i)
        {
            VxDriverDesc *candidate = m_RenderManager->GetRenderDriverDescription(i);
            if (candidate && m_Config.rasterizer.Compare(playeroptions::RasterizerNameForDriver(candidate->DriverDesc)) == 0)
            {
                m_Config.driver = i;
                break;
            }
        }
        if (m_Config.driver < 0)
        {
            CLogger::Get().Error("Requested rasterizer '%s' is unavailable.", m_Config.rasterizer.CStr());
            return false;
        }
        CLogger::Get().Info("Selected rasterizer: %s (driver %d)", m_Config.rasterizer.CStr(), m_Config.driver);
    }

    if (m_Config.manualSetup)
    {
        if (OpenSetupDialog())
            SyncPersistentDisplayConfig();
        m_Config.manualSetup = false;
        m_PersistentConfig.manualSetup = false;
    }

    bool tryFailed = false;

    VxDriverDesc *drDesc = m_RenderManager->GetRenderDriverDescription(m_Config.driver);
    if (!drDesc)
    {
        CLogger::Get().Error("Unable to find driver %d", m_Config.driver);
        m_Config.driver = 0;
        tryFailed = true;
        if (OpenSetupDialog())
        {
            SyncPersistentDisplayConfig();
        }
        else
        {
            SetDefaultValuesForDriver();
        }
    }

    if (tryFailed)
    {
        drDesc = m_RenderManager->GetRenderDriverDescription(m_Config.driver);
        if (!drDesc)
        {
            CLogger::Get().Error("Unable to find driver %d", m_Config.driver);
            return false;
        }
    }

    CLogger::Get().Debug("Render Driver ID: %d", m_Config.driver);
    CLogger::Get().Debug("Render Driver Name: %s", drDesc->DriverName);
    CLogger::Get().Debug("Render Driver Desc: %s", drDesc->DriverDesc);

    tryFailed = false;

    m_Config.screenMode = FindScreenMode(m_Config.width, m_Config.height, m_Config.bpp, m_Config.driver);
    if (m_Config.screenMode == -1)
    {
        CLogger::Get().Error("Unable to find screen mode: %d x %d x %d", m_Config.width, m_Config.height, m_Config.bpp);
        tryFailed = true;
        if (OpenSetupDialog())
        {
            SyncPersistentDisplayConfig();
        }
        else
        {
            SetDefaultValuesForDriver();
        }
    }

    if (tryFailed)
    {
        m_Config.screenMode = FindScreenMode(m_Config.width, m_Config.height, m_Config.bpp, m_Config.driver);
        if (m_Config.screenMode == -1)
        {
            CLogger::Get().Error("Unable to find screen mode: %d x %d x %d", m_Config.width, m_Config.height, m_Config.bpp);
            return false;
        }
    }

    CLogger::Get().Debug("Screen Mode: %d x %d x %d", m_Config.width, m_Config.height, m_Config.bpp);

    return true;
}

bool CGamePlayer::ConfigureBmlStartup()
{
    const char *option = getenv("BALLANCE_BML_START_AFTER_PROCESS");
    auto *plugins = CKGetPluginManager();
    auto *entry = plugins ? plugins->FindComponent(CKGUID(0x32a40332, 0x3bf12a51)) : nullptr;
    auto *dll = entry ? plugins->GetPluginDllInfo(entry->m_PluginDllIndex) : nullptr;
    using Configure = CKERROR (*)(CKContext *, CKBOOL);
    auto configure = dll ? reinterpret_cast<Configure>(dll->GetFunctionPtr(
        ToCKString("BML_SetPlayerDrivenStartupV1"))) : nullptr;
    auto complete = dll ? reinterpret_cast<AfterProcessStartup>(dll->GetFunctionPtr(
        ToCKString("BML_CompletePlayerStepV1"))) : nullptr;
    // A supporting BML initializes after the composition's first Process has
    // completed all manager cleanup. Negotiate this per loaded plugin/context;
    // ordinary launches must not depend on a diagnostic environment variable.
    // Older BML versions without this interface retain their OnCKPlay startup.
    if ((configure != nullptr) != (complete != nullptr)) {
        CLogger::Get().Error("BML provides an incomplete version 1 Player startup interface.");
        return false;
    }
    if (option && strcmp(option, "0") != 0 && strcmp(option, "1") != 0) {
        CLogger::Get().Error("BALLANCE_BML_START_AFTER_PROCESS must be 0 or 1.");
        return false;
    }
    const bool requested = option ? strcmp(option, "1") == 0 : configure != nullptr;
    if (requested && (!configure || !complete)) {
        CLogger::Get().Error("Deferred BML startup requires the version 1 Player startup interface.");
        return false;
    }
    if (configure && configure(m_CKContext, requested ? TRUE : FALSE) != CK_OK) {
        CLogger::Get().Error("BML rejected Player startup configuration.");
        return false;
    }
    auto configureRender = reinterpret_cast<ConfigureRenderSkip>(
        BmlFunction("BML_SetPlayerDrivenRenderSkipV1"));
    auto consumeRender = reinterpret_cast<ConsumeRenderSkip>(
        BmlFunction("BML_ConsumePlayerRenderSkipV1"));
    if ((configureRender != nullptr) != (consumeRender != nullptr) ||
        (configureRender && configureRender(m_RenderContext, TRUE) != CK_OK)) {
        CLogger::Get().Error("BML render-skip negotiation failed.");
        return false;
    }
    if (configureRender)
        CLogger::Get().Info("BML render skip: Player-driven version 1.");
    m_AfterProcessStartup = requested ? complete : nullptr;
    if (configure)
        CLogger::Get().Info("BML startup: %s (%s)",
                           requested ? "after first Process" : "OnCKPlay",
                           option ? "explicit override" : "negotiated");
    return true;
}

bool CGamePlayer::FinishLoad(const char *filename, const char *resolvedFile)
{
    if (!filename)
        return false;

    InterfaceManager *im = (InterfaceManager *)m_CKContext->GetManagerByGuid(TT_INTERFACE_MANAGER_GUID);
    if (im)
    {
        im->SetDriver(m_Config.driver);
        im->SetScreenMode(m_Config.screenMode);
        im->SetRookie(m_Config.rookie);
        im->SetTaskSwitchEnabled(true);

        if (m_GameInfo)
        {
            delete m_GameInfo;
            m_GameInfo = NULL;
        }

        m_GameInfo = new CGameInfo;
        strcpy(m_GameInfo->path, ".");
        strcpy(m_GameInfo->fileName, filename);
        im->SetGameInfo(m_GameInfo);
    }

    // Retrieve the level
    CKLevel *level = m_CKContext->GetCurrentLevel();
    if (!level)
    {
        CLogger::Get().Error("Failed to retrieve the level!");
        return false;
    }

    // Add the render context to the level
    level->AddRenderContext(m_RenderContext, TRUE);

    // Take the first camera we found and attach the viewpoint to it.
    // (in case it is not set by the composition later)
    const XObjectPointerArray cams = m_CKContext->GetObjectListByType(CKCID_CAMERA, TRUE);
    if (cams.Size() != 0)
        m_RenderContext->AttachViewpointToCamera((CKCamera *)cams[0]);

    // Hide curves ?
    int curveCount = m_CKContext->GetObjectsCountByClassID(CKCID_CURVE);
    CK_ID *curve_ids = m_CKContext->GetObjectsListByClassID(CKCID_CURVE);
    for (int i = 0; i < curveCount; ++i)
    {
        CKMesh *mesh = ((CKCurve *)m_CKContext->GetObject(curve_ids[i]))->GetCurrentMesh();
        if (mesh)
            mesh->Show(CKHIDE);
    }

    if (m_Config.applyHotfix && m_CKContext->GetManagerByGuid(TT_INTERFACE_MANAGER_GUID) != NULL)
    {
        if (!EditScript(level, m_Config, resolvedFile))
        {
            CLogger::Get().Warn("Failed to apply hotfixes on script!");
        }

        CLogger::Get().Debug("Hotfixes applied on script.");
    }

    if (!ConfigureBmlStartup())
        return false;

    // Launch the default scene
    level->LaunchScene(NULL);

    // ReRegister OnClick Message in case it changed
    m_MsgClick = m_MessageManager->AddMessageType("OnClick");
    m_MsgDoubleClick = m_MessageManager->AddMessageType("OnDblClick");

    // Render the first frame
    m_RenderContext->Render();

    return true;
}

void CGamePlayer::ReportMissingGuids(CKFile *file, const char *resolvedFile)
{
    // retrieve the list of missing plugins/guids
    const XClassArray<CKFilePluginDependencies> *p = file->GetMissingPlugins();
    for (CKFilePluginDependencies *it = p->Begin(); it != p->End(); it++)
    {
        const int count = it->m_Guids.Size();
        for (int i = 0; i < count; i++)
        {
            if (!it->ValidGuids[i])
            {
                if (resolvedFile)
                    CLogger::Get().Error("File Name : %s\nMissing GUIDS:\n", resolvedFile);
                CLogger::Get().Error("%x,%x\n", it->m_Guids[i].d1, it->m_Guids[i].d2);
            }
        }
    }
}

bool CGamePlayer::InitPlugins(CKPluginManager *pluginManager)
{
    if (!pluginManager)
        return false;

#ifdef BALLANCE_STATIC_MODULES
    if (!RegisterStaticPlugins(pluginManager))
    {
        CLogger::Get().Error("Failed to register static plugins.");
        return false;
    }

    CLogger::Get().Debug("Static plugins registered.");
    return true;
#else
    if (!LoadRenderEngines(pluginManager))
    {
        CLogger::Get().Error("Failed to load render engine!");
        return false;
    }

    if (!LoadManagers(pluginManager))
    {
        CLogger::Get().Error("Failed to load managers!");
        return false;
    }

    if (!LoadBuildingBlocks(pluginManager))
    {
        CLogger::Get().Error("Failed to load building blocks!");
        return false;
    }

    if (!LoadPlugins(pluginManager))
    {
        CLogger::Get().Error("Failed to load plugins!");
        return false;
    }

    return true;
#endif
}

bool CGamePlayer::LoadRenderEngines(CKPluginManager *pluginManager)
{
    if (!pluginManager)
        return false;

    XString path;
    if ((!ResolveRuntimePath(m_Config, eRenderEnginePath, path, true) ||
         !utils::DirectoryExists(path.CStr()) ||
         pluginManager->ParsePlugins(ToCKString(path.CStr())) == 0) &&
        !ParsePluginsFromExecutableDirectory(pluginManager))
    {
        CLogger::Get().Error("Render engine parse error.");
        return false;
    }

    CLogger::Get().Debug("Render engine loaded.");

    return true;
}

bool CGamePlayer::LoadManagers(CKPluginManager *pluginManager)
{
    if (!pluginManager)
        return false;

    XString path;
    if (!ResolveRuntimePath(m_Config, eManagerPath, path, true) || !utils::DirectoryExists(path.CStr()))
    {
        if (!ParsePluginsFromExecutableDirectory(pluginManager))
        {
            CLogger::Get().Error("Managers directory does not exist!");
            return false;
        }
        CLogger::Get().Debug("Managers loaded.");
        return true;
    }

    CLogger::Get().Debug("Loading managers from %s", path.CStr());

    if (pluginManager->ParsePlugins(ToCKString(path.CStr())) == 0)
    {
        CLogger::Get().Error("Managers parse error.");
        return false;
    }

    CLogger::Get().Debug("Managers loaded.");

    return true;
}

bool CGamePlayer::LoadBuildingBlocks(CKPluginManager *pluginManager)
{
    if (!pluginManager)
        return false;

    XString path;
    if (!ResolveRuntimePath(m_Config, eBuildingBlockPath, path, true) || !utils::DirectoryExists(path.CStr()))
    {
        if (!ParsePluginsFromExecutableDirectory(pluginManager))
        {
            CLogger::Get().Error("BuildingBlocks directory does not exist!");
            return false;
        }
        CLogger::Get().Debug("Building blocks loaded.");
        return true;
    }

    CLogger::Get().Debug("Loading building blocks from %s", path.CStr());

    if (pluginManager->ParsePlugins(ToCKString(path.CStr())) == 0)
    {
        CLogger::Get().Error("Behaviors parse error.");
        return false;
    }

    CLogger::Get().Debug("Building blocks loaded.");

    return true;
}

bool CGamePlayer::LoadPlugins(CKPluginManager *pluginManager)
{
    if (!pluginManager)
        return false;

    XString path;
    if (!ResolveRuntimePath(m_Config, ePluginPath, path, true) || !utils::DirectoryExists(path.CStr()))
    {
        if (!ParsePluginsFromExecutableDirectory(pluginManager))
        {
            CLogger::Get().Error("Plugins directory does not exist!");
            return false;
        }
        CLogger::Get().Debug("Plugins loaded.");
        return true;
    }

    CLogger::Get().Debug("Loading plugins from %s", path.CStr());

    if (pluginManager->ParsePlugins(ToCKString(path.CStr())) == 0)
    {
        CLogger::Get().Error("Plugins parse error.");
        return false;
    }

    CLogger::Get().Debug("Plugins loaded.");

    return true;
}

bool CGamePlayer::UnloadPlugins(CKPluginManager *pluginManager, CK_PLUGIN_TYPE type, CKGUID guid)
{
    if (!pluginManager)
        return false;

    const int count = pluginManager->GetPluginCount(type);
    for (int i = 0; i < count; ++i)
    {
        CKPluginEntry *entry = pluginManager->GetPluginInfo(type, i);
        CKPluginInfo &info = entry->m_PluginInfo;
        if (info.m_GUID == guid)
        {
            if (pluginManager->UnLoadPluginDll(entry->m_PluginDllIndex) == CK_OK)
                return true;
            break;
        }
    }

    return false;
}

int CGamePlayer::FindRenderEngine(CKPluginManager *pluginManager)
{
    if (!pluginManager)
        return -1;

    int count = pluginManager->GetPluginCount(CKPLUGIN_RENDERENGINE_DLL);
    for (int i = 0; i < count; ++i)
    {
        CKPluginEntry *entry = pluginManager->GetPluginInfo(CKPLUGIN_RENDERENGINE_DLL, i);
        if (!entry)
            break;

        CKPluginDll *dll = pluginManager->GetPluginDllInfo(entry->m_PluginDllIndex);
        if (!dll)
            break;

        if (IsDefaultRenderEngineDll(dll->m_DllFileName.Str()))
            return i;
    }

    return -1;
}

bool CGamePlayer::SetupManagers()
{
    m_RenderManager = m_CKContext->GetRenderManager();
    if (!m_RenderManager)
    {
        CLogger::Get().Error("Unable to get Render Manager.");
        return false;
    }

    m_MessageManager = m_CKContext->GetMessageManager();
    if (!m_MessageManager)
    {
        CLogger::Get().Error("Unable to get Message Manager.");
        return false;
    }

    m_TimeManager = m_CKContext->GetTimeManager();
    if (!m_TimeManager)
    {
        CLogger::Get().Error("Unable to get Time Manager.");
        return false;
    }
    m_TimeManager->SetFixedDeltaTime(kBehaviorDeltaMs);
    m_TimeManager->SetFixedProcessRate((float)kBehaviorRate);
    // Set Physics Globals owns the game's factor, including tutorial freezes.
    // A second host multiplier would double the script's factor after BML's
    // physics hook is correctly bound to PostProcess on native platforms.
    m_TimeManager->SetFixedPhysicsTimeScale(1.0f);
    m_TimeManager->SetBehavioralRateLimit((float)kBehaviorRate);
    m_TimeManager->ChangeLimitOptions(CK_FRAMERATE_SYNC, CK_BEHRATE_LIMIT);
    ResetFrameScheduler();

    m_AttributeManager = m_CKContext->GetAttributeManager();
    if (!m_AttributeManager)
    {
        CLogger::Get().Error("Unable to get Attribute Manager.");
        return false;
    }

    m_InputManager = (CKInputManager *)m_CKContext->GetManagerByGuid(INPUT_MANAGER_GUID);
    if (!m_InputManager)
    {
        CLogger::Get().Error("Unable to get Input Manager.");
        return false;
    }

    InterfaceManager *interfaceManager = InterfaceManager::GetManager(m_CKContext);
    if (interfaceManager)
        interfaceManager->SetPlayerCommandHandler(&CGamePlayer::HandlePlayerCommand, this);

    return true;
}

void CGamePlayer::DrainPlayerCommands()
{
    if (!m_CKContext)
        return;

    InterfaceManager *interfaceManager = InterfaceManager::GetManager(m_CKContext);
    if (!interfaceManager)
        return;

    TTPlayerCommand command;
    while (interfaceManager->PollPlayerCommand(command))
        ExecutePlayerCommand(interfaceManager, command);
}

int CGamePlayer::ExecutePlayerCommand(InterfaceManager *manager, const TTPlayerCommand &command)
{
    (void)manager;

    switch (command.type)
    {
    case TT_PLAYER_COMMAND_NO_GAMEINFO:
        OnExceptionCMO();
        return 1;

    case TT_PLAYER_COMMAND_CMO_RESTART:
        OnReturn();
        return 1;

    case TT_PLAYER_COMMAND_CMO_LOAD:
        return OnLoadCMO(command.text.CStr()) ? 1 : 0;

    case TT_PLAYER_COMMAND_EXIT_TO_SYSTEM:
        OnExitToSystem();
        return 1;

    case TT_PLAYER_COMMAND_EXIT_TO_TITLE:
        OnExitToTitle();
        return 1;

    case TT_PLAYER_COMMAND_LIMIT_FPS:
        if (m_TimeManager && command.param0 > 0)
        {
            m_TimeManager->SetFrameRateLimit((float)command.param0);
            m_TimeManager->ChangeLimitOptions(CK_FRAMERATE_LIMIT);
        }
        return 1;

    case TT_PLAYER_COMMAND_SCREEN_MODE_CHANGE:
        return OnChangeScreenMode(command.param1, command.param0);

    case TT_PLAYER_COMMAND_GO_FULLSCREEN:
        OnGoFullscreen();
        return 1;

    case TT_PLAYER_COMMAND_STOP_FULLSCREEN:
        OnStopFullscreen();
        return 1;

    default:
        CLogger::Get().Warn("Unknown player command: %d", command.type);
        return 0;
    }
}

int CGamePlayer::HandlePlayerCommand(InterfaceManager *manager, const TTPlayerCommand &command, void *userData)
{
    CGamePlayer *player = (CGamePlayer *)userData;
    if (!player)
        return 0;
    return player->ExecutePlayerCommand(manager, command);
}

bool CGamePlayer::SetupPaths()
{
    CKPathManager *pm = m_CKContext->GetPathManager();
    if (!pm)
    {
        CLogger::Get().Error("Unable to get Path Manager.");
        return false;
    }

    XString path;
    if (!ResolveRuntimePath(m_Config, eDataPath, path, true))
    {
        CLogger::Get().Error("Failed to resolve data path.");
        return false;
    }
    if (!utils::DirectoryExists(path.CStr()))
    {
        CLogger::Get().Error("Data path is not found.");
        return false;
    }
    XString dataPath = path.CStr();
    pm->AddPath(DATA_PATH_IDX, dataPath);
    CLogger::Get().Debug("Data path: %s", dataPath.CStr());

    if (!ResolveRuntimePath(m_Config, eSoundPath, path, true))
    {
        CLogger::Get().Error("Failed to resolve sounds path.");
        return false;
    }
    if (!utils::DirectoryExists(path.CStr()))
    {
        CLogger::Get().Error("Sounds path is not found.");
        return false;
    }
    XString soundPath = path.CStr();
    pm->AddPath(SOUND_PATH_IDX, soundPath);
    CLogger::Get().Debug("Sounds path: %s", soundPath.CStr());

    if (!ResolveRuntimePath(m_Config, eBitmapPath, path, true))
    {
        CLogger::Get().Error("Failed to resolve bitmap path.");
        return false;
    }
    if (!utils::DirectoryExists(path.CStr()))
    {
        CLogger::Get().Error("Bitmap path is not found.");
        return false;
    }
    XString bitmapPath = path.CStr();
    pm->AddPath(BITMAP_PATH_IDX, bitmapPath);
    CLogger::Get().Debug("Bitmap path: %s", bitmapPath.CStr());

    return true;
}

void CGamePlayer::ResizeWindow()
{
    if (m_SdlWindow)
    {
        SDL_SetWindowSize(m_SdlWindow, m_Config.width, m_Config.height);
        if (!SDL_SyncWindow(m_SdlWindow))
            CLogger::Get().Warn("Failed to sync resized window: %s", SDL_GetError());
    }
    return;
}

int CGamePlayer::FindScreenMode(int width, int height, int bpp, int driver)
{
    if (!m_RenderManager)
    {
        CLogger::Get().Error("RenderManager is not initialized");
        return -1;
    }

    VxDriverDesc *drDesc = m_RenderManager->GetRenderDriverDescription(driver);
    if (!drDesc)
    {
        CLogger::Get().Error("Unable to find render driver %d.", driver);
        return -1;
    }

#if CKVERSION == 0x13022002
    const VxDisplayMode *displayModes = drDesc->DisplayModes;
    const int displayModeCount = drDesc->DisplayModeCount;
#else
    XArray<VxDisplayMode> &displayModes = drDesc->DisplayModes;
    const int displayModeCount = displayModes.Size();
#endif

#if CKVERSION == 0x13022002
    int screenMode = FindBestDisplayModeIndex(displayModes, displayModeCount, width, height, bpp);
#else
    int screenMode = FindBestDisplayModeIndex(displayModes.Begin(), displayModeCount, width, height, bpp);
#endif
    if (screenMode < 0)
        CLogger::Get().Error("No matching screen mode found for %d x %d x %d", width, height, bpp);

    return screenMode;
}

bool CGamePlayer::GetDisplayMode(int &width, int &height, int &bpp, int driver, int screenMode)
{
    VxDriverDesc *drDesc = m_RenderManager->GetRenderDriverDescription(driver);
    if (!drDesc)
        return false;

#if CKVERSION == 0x13022002
    if (screenMode < 0 || screenMode >= drDesc->DisplayModeCount)
        return false;
#else
    if (screenMode < 0 || screenMode >= drDesc->DisplayModes.Size())
        return false;
#endif

    const VxDisplayMode &dm = drDesc->DisplayModes[screenMode];
    width = dm.Width;
    height = dm.Height;
    bpp = dm.Bpp;

    return true;
}

void CGamePlayer::SetDefaultValuesForDriver()
{
    m_Config.driver = 0;
    m_Config.width = PLAYER_DEFAULT_WIDTH;
    m_Config.height = PLAYER_DEFAULT_HEIGHT;
    m_Config.bpp = PLAYER_DEFAULT_BPP;
    SyncPersistentDisplayConfig();
}

void CGamePlayer::SyncPersistentDisplayConfig()
{
    m_PersistentConfig.driver = m_Config.driver;
    m_PersistentConfig.width = m_Config.width;
    m_PersistentConfig.height = m_Config.height;
    m_PersistentConfig.bpp = m_Config.bpp;
}

void CGamePlayer::SyncPersistentWindowPosition()
{
    m_PersistentConfig.posX = m_Config.posX;
    m_PersistentConfig.posY = m_Config.posY;
}

bool CGamePlayer::IsRenderFullscreen() const
{
    if (!m_RenderContext)
        return false;
    return m_RenderContext->IsFullScreen() == TRUE;
}

bool CGamePlayer::GoFullscreen()
{
    if (!m_RenderContext)
        return false;

    if (IsRenderFullscreen())
        return true;

    int width = m_Config.width;
    int height = m_Config.height;
    if (m_MainWindow)
    {
        CKRECT rc;
        if (VxGetClientRect(m_MainWindow, &rc) && rc.right > rc.left && rc.bottom > rc.top)
        {
            width = rc.right - rc.left;
            height = rc.bottom - rc.top;
        }
    }

    m_Config.fullscreen = true;
    if (m_RenderContext->GoFullScreen(width, height, m_Config.bpp, m_Config.driver) != CK_OK)
    {
        CLogger::Get().Debug("GoFullScreen Failed");
        m_Config.fullscreen = false;
        return false;
    }

    return true;
}

bool CGamePlayer::StopFullscreen()
{
    if (!m_RenderContext)
        return false;

    if (!IsRenderFullscreen())
    {
        m_Config.fullscreen = false;
        return true;
    }

    if (m_RenderContext->StopFullScreen() != CK_OK)
    {
        CLogger::Get().Debug("StopFullscreen Failed");
        return false;
    }

    m_Config.fullscreen = false;
    return true;
}

void CGamePlayer::SetFullscreenDisplayMode()
{
    if (m_SdlWindow)
    {
        if (!SDL_SetWindowFullscreenMode(m_SdlWindow, NULL))
            CLogger::Get().Warn("Failed to set borderless fullscreen display mode: %s", SDL_GetError());
        if (!SDL_SetWindowFullscreen(m_SdlWindow, true))
            CLogger::Get().Warn("Failed to enter fullscreen: %s", SDL_GetError());
        if (!SDL_SyncWindow(m_SdlWindow))
            CLogger::Get().Warn("Failed to sync fullscreen window: %s", SDL_GetError());
    }
    return;
}

void CGamePlayer::RestoreDisplayMode()
{
    if (m_SdlWindow)
    {
        if (!SDL_SetWindowFullscreen(m_SdlWindow, false))
            CLogger::Get().Warn("Failed to leave fullscreen: %s", SDL_GetError());
        if (!SDL_SyncWindow(m_SdlWindow))
            CLogger::Get().Warn("Failed to sync windowed mode: %s", SDL_GetError());
    }
}

void CGamePlayer::SetWindowedWindowStyle()
{
    if (m_SdlWindow)
    {
        SDL_SetWindowFullscreen(m_SdlWindow, false);
        SDL_SetWindowBordered(m_SdlWindow, !m_Config.borderless);
        SDL_SetWindowSize(m_SdlWindow, m_Config.width, m_Config.height);
        SDL_SetWindowPosition(m_SdlWindow, m_Config.posX, m_Config.posY);
        SDL_SyncWindow(m_SdlWindow);
    }
    return;
}

bool CGamePlayer::ClipCursor()
{
    if (m_SdlWindow)
        SDL_SetWindowMouseGrab(m_SdlWindow, m_Config.clipCursor ? true : false);
    return true;
}

bool CGamePlayer::ReleaseCursorClip()
{
    if (m_SdlWindow)
        SDL_SetWindowMouseGrab(m_SdlWindow, false);
    return true;
}

bool CGamePlayer::OpenSetupDialog()
{
    return false;
}

void CGamePlayer::OnDestroy()
{
}

void CGamePlayer::OnMove()
{
    if (!m_Config.fullscreen)
    {
        if (m_SdlWindow)
            SDL_GetWindowPosition(m_SdlWindow, &m_Config.posX, &m_Config.posY);
        if (m_State != eInitial)
            SyncPersistentWindowPosition();
    }
}

void CGamePlayer::OnSize()
{
    if (m_MainWindow)
    {
        CKRECT rc;
        if (VxGetClientRect(m_MainWindow, &rc))
        {
            int width = rc.right - rc.left;
            int height = rc.bottom - rc.top;
            if (m_SdlWindow)
                SDL_GetWindowSize(m_SdlWindow, &width, &height);
            if (width > 0 && height > 0)
            {
                if (!m_Config.fullscreen && !IsRenderFullscreen())
                {
                    m_Config.width = width;
                    m_Config.height = height;
                    if (m_RenderContext && (m_RenderContext->GetWidth() != width || m_RenderContext->GetHeight() != height))
                    {
                        CKERROR res = m_RenderContext->Resize(0, 0, width, height, VX_RESIZE_NOMOVE);
                        if (res != CK_OK)
                            CLogger::Get().Warn("Failed to resize render context: %d", res);
                        else
                            CLogger::Get().Debug("Render context resized to %dx%d logical units", width, height);
                    }
                }
            }
        }
    }

    ClipCursor();
}

void CGamePlayer::OnPaint()
{
    if (m_RenderContext && !m_Config.fullscreen)
        Render();
}

void CGamePlayer::OnClose()
{
    SDL_Event quitEvent;
    SDL_zero(quitEvent);
    quitEvent.type = SDL_EVENT_QUIT;
    SDL_PushEvent(&quitEvent);
}

void CGamePlayer::OnActivateApp(bool active)
{
    static bool wasFullscreen = false;
    static bool firstDeActivate = true;

    if (m_State == eInitial)
        return;

    if (!active)
    {
        if (m_CKContext)
        {
            if (!m_Config.alwaysHandleInput)
                m_InputManager->Pause(TRUE);

            ReleaseCursorClip();

            if (IsRenderFullscreen())
            {
                if (firstDeActivate)
                    wasFullscreen = true;
                OnStopFullscreen(false);
            }
            else if (firstDeActivate)
            {
                wasFullscreen = false;
            }
        }
        firstDeActivate = false;
        m_State = eFocusLost;
    }
    else
    {
        if (wasFullscreen && !firstDeActivate)
            OnGoFullscreen(false);

        ClipCursor();

        if (!m_Config.alwaysHandleInput)
            m_InputManager->Pause(FALSE);

        firstDeActivate = true;
        m_State = ePlaying;
    }
}


void CGamePlayer::OnClick(bool dblClk)
{
    if (!m_RenderManager)
        return;

    float mouseX = 0.0f;
    float mouseY = 0.0f;
    SDL_GetMouseState(&mouseX, &mouseY);
    int pickX = static_cast<int>(mouseX);
    int pickY = static_cast<int>(mouseY);

    CKMessageType msgType = (!dblClk) ? m_MsgClick : m_MsgDoubleClick;

#if CKVERSION == 0x13022002
    CKPOINT ckpt = {pickX, pickY};
    CKPICKRESULT res;
    CKObject *obj = m_RenderContext->Pick(ckpt, &res, FALSE);
    if (obj && CKIsChildClassOf(obj, CKCID_BEOBJECT))
        m_MessageManager->SendMessageSingle(msgType, (CKBeObject *)obj, NULL);
    if (res.Sprite)
    {
        CKObject *sprite = m_CKContext->GetObject(res.Sprite);
        if (sprite)
            m_MessageManager->SendMessageSingle(msgType, (CKBeObject *)sprite, NULL);
    }
#else
    VxIntersectionDesc desc;
    CKObject *obj = m_RenderContext->Pick(pickX, pickY, &desc);
    if (obj && CKIsChildClassOf(obj, CKCID_BEOBJECT))
        m_MessageManager->SendMessageSingle(msgType, (CKBeObject *)obj);
    if (desc.Sprite)
        m_MessageManager->SendMessageSingle(msgType, (CKBeObject *)desc.Sprite);
#endif
}


void CGamePlayer::OnExceptionCMO()
{
    CLogger::Get().Error("Exception in the CMO - Abort");
    OnExitToSystem();
}

void CGamePlayer::OnReturn()
{
    XString filename = m_GameInfo->fileName;
    if (!Load(filename.CStr()))
    {
        OnClose();
        return;
    }

    Play();
}

bool CGamePlayer::OnLoadCMO(const char *filename)
{
    return Load(filename);
}

void CGamePlayer::OnExitToSystem()
{
    bool fullscreen = m_Config.fullscreen;
    OnStopFullscreen(false);
    m_Config.fullscreen = fullscreen;
    OnClose();
}

void CGamePlayer::OnExitToTitle()
{
}

int CGamePlayer::OnChangeScreenMode(int driver, int screenMode)
{
    int width, height, bpp;
    if (!GetDisplayMode(width, height, bpp, driver, screenMode))
    {
        CLogger::Get().Error("Failed to change screen mode.");
        return 0;
    }

    bool fullscreen = m_Config.fullscreen;
    if (fullscreen)
        OnStopFullscreen(false);

    ClipCursor();

    m_Config.driver = driver;
    m_Config.screenMode = screenMode;
    m_Config.width = width;
    m_Config.height = height;
    m_Config.bpp = bpp;
    SyncPersistentDisplayConfig();

    InterfaceManager *im = (InterfaceManager *)m_CKContext->GetManagerByGuid(TT_INTERFACE_MANAGER_GUID);
    if (im)
    {
        im->SetDriver(m_Config.driver);
        im->SetScreenMode(m_Config.screenMode);
    }

    ResizeWindow();
    OnSize();

    if (fullscreen)
        OnGoFullscreen(false);

    return 1;
}

void CGamePlayer::OnGoFullscreen(bool persistChange)
{
    Pause();

    ReleaseCursorClip();

    m_Config.fullscreen = true;
    SetFullscreenDisplayMode();

    if (GoFullscreen())
    {
        if (persistChange)
            m_PersistentConfig.fullscreen = true;

        if (m_SdlWindow)
        {
            SDL_ShowWindow(m_SdlWindow);
            SDL_RaiseWindow(m_SdlWindow);
        }
    }
    else
    {
        RestoreDisplayMode();
        SetWindowedWindowStyle();
    }

    Play();
}

void CGamePlayer::OnStopFullscreen(bool persistChange)
{
    Pause();

    bool fullscreen = m_Config.fullscreen || IsRenderFullscreen();
    const int windowedWidth = m_Config.width;
    const int windowedHeight = m_Config.height;
    if (StopFullscreen())
    {
        if (persistChange)
            m_PersistentConfig.fullscreen = false;

        if (fullscreen)
        {
            RestoreDisplayMode();
            m_Config.width = windowedWidth;
            m_Config.height = windowedHeight;
            SetWindowedWindowStyle();
        }

        if (m_SdlWindow)
        {
            SDL_ShowWindow(m_SdlWindow);
            SDL_RaiseWindow(m_SdlWindow);
        }
    }

    ClipCursor();

    Play();
}

void CGamePlayer::OnSwitchFullscreen()
{
    if (m_State == eInitial)
        return;

    if (!IsRenderFullscreen())
        OnGoFullscreen(true);
    else
        OnStopFullscreen(true);
}
