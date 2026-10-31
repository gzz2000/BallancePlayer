#include "CKPluginManager.h"
#include <cstring>
#include <BML/StaticMod.h>
#ifdef BALLANCE_STATIC_TAS
extern "C" IMod *TAS_Static_Entry(IBML *);
extern "C" void TAS_Static_Exit(IMod *);
#endif
#ifdef BALLANCE_STATIC_LEGACY_TAS
extern "C" IMod *LegacyTAS_Static_Entry(IBML *);
extern "C" void LegacyTAS_Static_Exit(IMod *);
#endif

int BML_Static_GetPluginInfoCount();
CKPluginInfo *BML_Static_GetPluginInfo(int);
void BML_Static_RegisterBehaviorDeclarations(XObjectDeclarationArray *);
CKERROR BML_SetPlayerDrivenStartupV1(CKContext *, CKBOOL);
CKERROR BML_CompletePlayerStepV1(CKContext *);
extern "C" {
CKERROR BML_SetPlayerDrivenRenderSkipV1(CKRenderContext *, CKBOOL);
CKBOOL BML_ConsumePlayerRenderSkipV1(CKRenderContext *);
}

namespace {
void *ResolveBmlFunction(const char *name) {
    if (!name) return nullptr;
#define BML_HOST_FUNCTION(fn) \
    if (std::strcmp(name, #fn) == 0) return reinterpret_cast<void *>(&fn)
    BML_HOST_FUNCTION(BML_SetPlayerDrivenStartupV1);
    BML_HOST_FUNCTION(BML_CompletePlayerStepV1);
    BML_HOST_FUNCTION(BML_SetPlayerDrivenRenderSkipV1);
    BML_HOST_FUNCTION(BML_ConsumePlayerRenderSkipV1);
#undef BML_HOST_FUNCTION
    return nullptr;
}
}

bool RegisterStaticBml(CKPluginManager *plugins) {
#ifdef BALLANCE_STATIC_TAS
    if (!BML_RegisterStaticModFactory({"BallanceTAS", TAS_Static_Entry, TAS_Static_Exit}))
        return false;
#endif
#ifdef BALLANCE_STATIC_LEGACY_TAS
    if (!BML_RegisterStaticModFactory({"LegacyTASRuntime", LegacyTAS_Static_Entry, LegacyTAS_Static_Exit}))
        return false;
#endif
    const auto result = plugins->RegisterStaticPlugin(
        const_cast<char *>("BMLPlus"), BML_Static_GetPluginInfoCount,
        BML_Static_GetPluginInfo, nullptr, BML_Static_RegisterBehaviorDeclarations,
        ResolveBmlFunction);
    return result == CK_OK || result == CKERR_ALREADYPRESENT;
}
