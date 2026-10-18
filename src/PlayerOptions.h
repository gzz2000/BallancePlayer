#ifndef PLAYER_PLAYEROPTIONS_H
#define PLAYER_PLAYEROPTIONS_H

#include "GameConfig.h"

class CmdlineParser;

namespace playeroptions
{
    // --user-data-path intentionally is not a CGameConfig resource path.  It
    // selects the writable working directory used by game scripts (notably
    // Database.tdb), while --root-path continues to select read-only assets.
    bool GetUserDataPath(CmdlineParser &parser, XString &path);
    bool HasCommandLineOption(CmdlineParser &parser, const char *longopt);

    void ApplyPathOptions(CGameConfig &config, CmdlineParser &parser);
    bool ApplyConfigOptions(CGameConfig &config, CmdlineParser &parser);
    bool ApplyRuntimeOptions(CGameConfig &config, CmdlineParser &parser);

    bool IsRasterizerName(const char *name);
    const char *RasterizerNameForDriver(const char *description);
    int GetConfigOptionCount();
    int GetPathOptionCount();
    bool HasConfigOption(const char *longopt, char shortopt);
    bool HasPathOption(const char *longopt);
}

#endif // PLAYER_PLAYEROPTIONS_H
