#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <filesystem>
#include <system_error>

#include "CmdlineParser.h"
#include "GameConfig.h"
#include "GamePlayer.h"
#include "PlayerOptions.h"
#include "Logger.h"
#include "Utils.h"

static bool EnsurePersistentConfigReady(CGameConfig &config);
static bool PrepareUserDataDirectory(CGameConfig &config, CmdlineParser &parser);
static void ShowErrorMessage(const char *text);

namespace fs = std::filesystem;

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

    CGamePlayer player;
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
    player.Run();
    player.Shutdown();

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
