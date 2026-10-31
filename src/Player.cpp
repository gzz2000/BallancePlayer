#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <filesystem>
#include <cstdlib>
#include <cstdio>
#include <system_error>
#include <memory>
#ifdef PLAYER_WEB_INTERACTIVE
#include <emscripten.h>
#endif

#include "CmdlineParser.h"
#include "GameConfig.h"
#include "GamePlayer.h"
#include "PlayerOptions.h"
#include "Logger.h"
#include "Utils.h"

static bool EnsurePersistentConfigReady(CGameConfig &config);
static bool PrepareUserDataDirectory(CGameConfig &config, CmdlineParser &parser);
static bool SetDatabaseDirectoryEnvironment(const char *path);
static void ShowErrorMessage(const char *text);

namespace fs = std::filesystem;

#ifdef PLAYER_WEB_INTERACTIVE
// The host requests logical canvas dimensions through SDL. This preserves the
// normal window-event path that updates render extents and mouse coordinates.
extern "C" EMSCRIPTEN_KEEPALIVE int BallanceWebResize(int width, int height)
{
    if (width < 320 || height < 240 || width > 4096 || height > 4096)
        return 0;
    int count = 0;
    SDL_Window **windows = SDL_GetWindows(&count);
    const bool result = count == 1 && SDL_SetWindowSize(windows[0], width, height);
    SDL_free(windows);
    return result ? 1 : 0;
}

extern "C" EMSCRIPTEN_KEEPALIVE void BallanceWebRequestQuit()
{
    SDL_Event event{};
    event.type = SDL_EVENT_QUIT;
    SDL_PushEvent(&event);
}
#endif

int main(int argc, char **argv)
{
    SDL_SetMainReady();
    XString runtimeBase = utils::GetExecutableDirectory();

    CmdlineParser parser(argc, argv);

    CGameConfig persistentConfig;
    persistentConfig.SetRuntimeBasePath(runtimeBase.CStr());
    playeroptions::ApplyPathOptions(persistentConfig, parser);

    if (!PrepareUserDataDirectory(persistentConfig, parser))
        return -1;

    if (!EnsurePersistentConfigReady(persistentConfig))
        return -1;

    persistentConfig.LoadFromIni();
    CGameConfig runtimeConfig = persistentConfig;
    if (!playeroptions::ApplyRuntimeOptions(runtimeConfig, parser))
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Invalid rasterizer selection: use --rasterizer sdlgpu|null or --video-driver N; "
                     "the two selectors are mutually exclusive.");
        return 2;
    }

    bool overwrite = runtimeConfig.logMode != eLogAppend;
    XString logPath;
    CLogger::Get().Open(runtimeConfig.ResolvePath(eLogPath, logPath) ? logPath.CStr() : runtimeConfig.GetPath(eLogPath), overwrite);
    if (runtimeConfig.verbose)
        CLogger::Get().SetLevel(CLogger::LEVEL_DEBUG);

    auto owner = std::make_unique<CGamePlayer>();
    CGamePlayer &player = *owner;
    if (!player.Init(runtimeConfig, persistentConfig))
    {
        CLogger::Get().Error("Failed to initialize player!");
        ShowErrorMessage("Failed to initialize player!");
        return -1;
    }

    CLogger::Get().Debug("Loading game composition: %s", runtimeConfig.GetPath(eCmoPath));
    if (!player.Load(runtimeConfig.GetPath(eCmoPath)))
    {
        CLogger::Get().Error("Failed to load game composition!");
        ShowErrorMessage("Failed to load game composition!");
        player.Shutdown();
        return -1;
    }

    player.Play();
#ifdef PLAYER_WEB_INTERACTIVE
    // rAF owns the player after main returns; stack-local ownership would
    // destroy the game before the first browser callback.
    emscripten_set_main_loop_arg([](void *argument) {
        auto *running = static_cast<CGamePlayer *>(argument);
        if (!running->Update()) {
            emscripten_cancel_main_loop();
            running->Shutdown();
            delete running;
            // The browser runtime remains alive for data export. Unlike a
            // native process exit, cancelling rAF does not flush libc streams.
            std::fflush(nullptr);
            EM_ASM({ Module['onBallanceExit']?.(); });
        }
    }, owner.release(), 0, false);
#else
    player.Run();
    player.Shutdown();
#endif

    return 0;
}

static bool PrepareUserDataDirectory(CGameConfig &config, CmdlineParser &parser)
{
    XString requestedPath;
    if (!playeroptions::GetUserDataPath(parser, requestedPath))
        return true;

    const XString userDataPath = utils::ResolvePathAgainstBase(
        config.GetRuntimeBasePath(), requestedPath.CStr(), false);
    if (userDataPath.IsEmpty())
    {
        ShowErrorMessage("Failed to determine user data directory.");
        return false;
    }

    std::error_code error;
    const fs::path directory = fs::u8path(userDataPath.CStr());
    fs::create_directories(directory / "ModLoader" / "Maps", error);
    if (!error)
        fs::create_directories(directory / "ModLoader" / "Mods", error);
    if (!error)
        fs::create_directories(directory / "ModLoader" / "Configs", error);
    if (!error)
        fs::create_directories(directory / "Screenshots", error);
    if (error)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create user data directory '%s': %s",
                     userDataPath.CStr(), error.message().c_str());
        ShowErrorMessage("Failed to create user data directory.");
        return false;
    }

    // TT Database Manager receives the database name directly from the game
    // composition and does not use CKPathManager or the process working
    // directory when that name is absolute.  Tell it where mutable databases
    // belong before base.cmo starts executing.
    if (!SetDatabaseDirectoryEnvironment(userDataPath.CStr()))
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Failed to configure writable database directory '%s'.",
                     userDataPath.CStr());
        ShowErrorMessage("Failed to configure the score database path.");
        return false;
    }

    // Command-line file paths remain authoritative.  Otherwise, keep all
    // mutable Player state beside the script-owned Database.tdb.
    if (!playeroptions::HasCommandLineOption(parser, "--config"))
        config.SetPath(eConfigPath, utils::JoinPath(userDataPath.CStr(), "Player.ini").CStr());
    if (!playeroptions::HasCommandLineOption(parser, "--log"))
        config.SetPath(eLogPath, utils::JoinPath(userDataPath.CStr(), "Player.log").CStr());

    // Seed the writable database once.  Existing scores are never replaced.
    XString rootPath;
    if (config.ResolvePath(eRootPath, rootPath, true))
    {
        const fs::path source = fs::u8path(utils::JoinPath(rootPath.CStr(), "Database.tdb").CStr());
        const fs::path destination = directory / "Database.tdb";
        error.clear();
        if (fs::exists(source, error) && !fs::exists(destination, error))
            fs::copy_file(source, destination, fs::copy_options::none, error);
        if (error)
        {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to seed Database.tdb: %s",
                         error.message().c_str());
            ShowErrorMessage("Failed to initialize the score database.");
            return false;
        }
    }

    error.clear();
    fs::current_path(directory, error);
    if (error)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to enter user data directory '%s': %s",
                     userDataPath.CStr(), error.message().c_str());
        ShowErrorMessage("Failed to enter user data directory.");
        return false;
    }
    return true;
}

static bool SetDatabaseDirectoryEnvironment(const char *path)
{
    if (!path || path[0] == '\0')
        return false;
#if defined(_WIN32)
    return _putenv_s("TT_DATABASE_DIRECTORY", path) == 0;
#else
    return setenv("TT_DATABASE_DIRECTORY", path, 1) == 0;
#endif
}

static bool EnsurePersistentConfigReady(CGameConfig &config)
{
    if (!config.EnsureConfigPath())
    {
        ShowErrorMessage("Failed to determine configuration file path.");
        return false;
    }

    if (utils::FileOrDirectoryExists(config.GetPath(eConfigPath)))
        return true;

    return config.SaveToIni();
}

static void ShowErrorMessage(const char *text)
{
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Error", text ? text : "Error", NULL);
}
