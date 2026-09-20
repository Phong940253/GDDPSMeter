#pragma once
#include "DetourBase.h"

//=============================================================================
// DetourTerrainFix - Phase 1 guard for coop rift crash.
//
// Dump evidence (minidump.dmp):
//   Exception C0000005 READ Addr=Engine.dll+0x193FA0 Param[1]=0x68
//   Fault context: RIP=Engine+0x193FA0, RCX(This)=0
//   Nearest exports: 00193F30 ?SetCoords@TerrainPatch / 00193FC0 ?InitialUpdate
//   Stack: Game.dll network/spawn path -> Engine TerrainPatch::SetCoords
//   log.html ends right after SpawnPlayerPacket 13 fragments over Radmin VPN.
//
// Fix: hook SetCoords + InitialUpdate, null/low-pointer guard + SEH,
// skip original call instead of crashing. Non-fatal if symbols missing
// (version-specific). Counters + throttled logging for Phase 2 analysis.
//=============================================================================

// x64 (v1.3.0.8 x64 crash dump) vs x86 (engine_exports.txt dumpbin) names.
#define SYM_TERRAINPATCH_SETCOORDS_X64 "?SetCoords@TerrainPatch@GAME@@QEAAXABVWorldCoords@2@@Z"
#define SYM_TERRAINPATCH_SETCOORDS_X86 "?SetCoords@TerrainPatch@GAME@@QAEXABVWorldCoords@2@@Z"
#define SYM_TERRAINPATCH_INITIALUPDATE_X64 "?InitialUpdate@TerrainPatch@GAME@@UEAAXXZ"
#define SYM_TERRAINPATCH_INITIALUPDATE_X86 "?InitialUpdate@TerrainPatch@GAME@@UAEXXZ"

// Context helper for Phase 2: true while area load is in progress.
#define SYM_ISGAMELOADING_X64 "?IsGameLoading@GameEngine@GAME@@QEBA_NXZ"
#define SYM_ISGAMELOADING_X86 "?IsGameLoading@GameEngine@GAME@@QBE_NXZ"

class DetourTerrainFix : public DetourBase
{
public:
    DetourTerrainFix();

    virtual bool SetupDetour();
    virtual void Update(void* player, int idx);
    virtual void SetPlayer(void* player) { (void)player; }

    // Stats for DebugView / DPSMeter.log (Phase 2 tuning).
    static long GetOkSetCoords() { return okSetCoords_; }
    static long GetSkippedNullThis() { return skippedNullThis_; }
    static long GetSkippedNullWC() { return skippedNullWC_; }
    static long GetSkippedException() { return skippedException_; }
    static long GetOkInitialUpdate() { return okInitialUpdate_; }
    static long GetSkippedNullInitialUpdate() { return skippedNullInitialUpdate_; }

    // Static trampolines (match DetourBase __fastcall convention).
    static void __fastcall DTSetCoords(void* This, void*, void* WorldCoords);
    static void __fastcall DTInitialUpdate(void* This);

private:
    void OnSetCoords(void* This, void* WorldCoords);
    void OnInitialUpdate(void* This);
    bool IsGameLoading() const;
    static bool ShouldLogSkip();

    static DetourTerrainFix* sSelf_;

    static ThisFunc<void, void*, void*> fnSetCoords_;
    static ThisFunc<void, void*> fnInitialUpdate_;
    static ThisFunc<bool, void*> fnIsGameLoading_;

    static volatile LONG okSetCoords_;
    static volatile LONG skippedNullThis_;
    static volatile LONG skippedNullWC_;
    static volatile LONG skippedException_;
    static volatile LONG okInitialUpdate_;
    static volatile LONG skippedNullInitialUpdate_;
    static volatile LONG logCount_;
};
