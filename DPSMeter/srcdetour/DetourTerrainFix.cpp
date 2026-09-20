#include <windows.h>
#include "DetourTerrainFix.h"
#include "Logger.h"

//=============================================================================
// Static members
//=============================================================================
DetourTerrainFix* DetourTerrainFix::sSelf_ = NULL;

ThisFunc<void, void*, void*> DetourTerrainFix::fnSetCoords_;
ThisFunc<void, void*> DetourTerrainFix::fnInitialUpdate_;
ThisFunc<bool, void*> DetourTerrainFix::fnIsGameLoading_;

volatile LONG DetourTerrainFix::okSetCoords_ = 0;
volatile LONG DetourTerrainFix::skippedNullThis_ = 0;
volatile LONG DetourTerrainFix::skippedNullWC_ = 0;
volatile LONG DetourTerrainFix::skippedException_ = 0;
volatile LONG DetourTerrainFix::okInitialUpdate_ = 0;
volatile LONG DetourTerrainFix::skippedNullInitialUpdate_ = 0;
volatile LONG DetourTerrainFix::logCount_ = 0;

// Detour descriptors. mangleName_ is patched at runtime to the first
// variant that resolves (x64 first, then x86).
static DetourFnData datTerrainSetCoords = {
    "engine.dll", NULL,
    (VoidFn)&DetourTerrainFix::DTSetCoords,
    SYM_TERRAINPATCH_SETCOORDS_X64
};

static DetourFnData datTerrainInitialUpdate = {
    "engine.dll", NULL,
    (VoidFn)&DetourTerrainFix::DTInitialUpdate,
    SYM_TERRAINPATCH_INITIALUPDATE_X64
};

// Call-only: never hooked, only resolved for loading-state context in logs.
static DetourFnData datIsGameLoading = {
    "game.dll", NULL,
    NULL,
    SYM_ISGAMELOADING_X64
};

// Anything below this is never a valid object pointer (matches
// DetourUtil::MemValidity threshold). Cheap integer check on the hot path.
static const uintptr_t kMinValidPtr = 0x10000;

// Log first N skips verbosely, then throttle to 1 per 500 to avoid
// spamming DebugView while SetCoords runs per-frame.
bool DetourTerrainFix::ShouldLogSkip()
{
    LONG n = InterlockedIncrement(&logCount_);
    return (n <= 10) || (n % 500 == 0);
}

//=============================================================================
// Construction
//=============================================================================
DetourTerrainFix::DetourTerrainFix()
{
    sSelf_ = this;
}

//=============================================================================
// SetupDetour - resolve x64 name, fall back to x86, warn-only on failure.
//=============================================================================
bool DetourTerrainFix::SetupDetour()
{
    // --- SetCoords ---
    int err = HookDetour(datTerrainSetCoords);
    if (datTerrainSetCoords.realFn_ == NULL)
    {
        datTerrainSetCoords.mangleName_ = SYM_TERRAINPATCH_SETCOORDS_X86;
        err = HookDetour(datTerrainSetCoords);
    }
    fnSetCoords_.SetFn(datTerrainSetCoords.realFn_);
    LOGF("DetourTerrainFix: TerrainPatch::SetCoords %s addr=%p\n",
         datTerrainSetCoords.realFn_ ? "OK" : "FAILED (non-fatal)",
         datTerrainSetCoords.realFn_);

    // --- InitialUpdate ---
    int err2 = HookDetour(datTerrainInitialUpdate);
    if (datTerrainInitialUpdate.realFn_ == NULL)
    {
        datTerrainInitialUpdate.mangleName_ = SYM_TERRAINPATCH_INITIALUPDATE_X86;
        err2 = HookDetour(datTerrainInitialUpdate);
    }
    fnInitialUpdate_.SetFn(datTerrainInitialUpdate.realFn_);
    LOGF("DetourTerrainFix: TerrainPatch::InitialUpdate %s addr=%p\n",
         datTerrainInitialUpdate.realFn_ ? "OK" : "FAILED (non-fatal)",
         datTerrainInitialUpdate.realFn_);

    // --- IsGameLoading (context only, failure is fine) ---
    HookDetour(datIsGameLoading);
    if (datIsGameLoading.realFn_ == NULL)
    {
        DetourFnData retry = { "game.dll", NULL, NULL, SYM_ISGAMELOADING_X86 };
        HookDetour(retry);
        if (retry.realFn_)
            datIsGameLoading.realFn_ = retry.realFn_;
    }
    fnIsGameLoading_.SetFn(datIsGameLoading.realFn_);

    if ((datTerrainSetCoords.realFn_ == NULL) &&
        (datTerrainInitialUpdate.realFn_ == NULL))
    {
        OutputDebugStringA("DetourTerrainFix::SetupDetour - WARNING: no terrain hooks (non-fatal)\n");
    }

    (void)err;
    (void)err2;
    return true; // never fatal: guard must not kill the process
}

void DetourTerrainFix::Update(void* player, int idx)
{
    (void)player;
    (void)idx;
}

bool DetourTerrainFix::IsGameLoading() const
{
    if (!fnIsGameLoading_.Fn_)
        return false;
    __try
    {
        // This needs a GameEngine*; we do not have one here, so this helper
        // is reserved for Phase 2 (called with a captured engine pointer).
        return false;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

//=============================================================================
// Guards
//=============================================================================
void DetourTerrainFix::OnSetCoords(void* This, void* WorldCoords)
{
    if ((uintptr_t)This < kMinValidPtr)
    {
        InterlockedIncrement(&skippedNullThis_);
        if (ShouldLogSkip())
            LOGF("DetourTerrainFix: SKIP SetCoords This=%p WC=%p (null patch, rift load race)\n",
                 This, WorldCoords);
        return;
    }
    if ((uintptr_t)WorldCoords < kMinValidPtr)
    {
        InterlockedIncrement(&skippedNullWC_);
        if (ShouldLogSkip())
            LOGF("DetourTerrainFix: SKIP SetCoords This=%p WC=%p (null coords)\n",
                 This, WorldCoords);
        return;
    }
    __try
    {
        fnSetCoords_.Fn_(This, WorldCoords);
        InterlockedIncrement(&okSetCoords_);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        InterlockedIncrement(&skippedException_);
        LOGF("DetourTerrainFix: SetCoords EXCEPTION swallowed This=%p WC=%p code=0x%08X\n",
             This, WorldCoords, GetExceptionCode());
    }
}

void DetourTerrainFix::OnInitialUpdate(void* This)
{
    if ((uintptr_t)This < kMinValidPtr)
    {
        InterlockedIncrement(&skippedNullInitialUpdate_);
        if (ShouldLogSkip())
            LOGF("DetourTerrainFix: SKIP InitialUpdate This=%p\n", This);
        return;
    }
    __try
    {
        fnInitialUpdate_.Fn_(This);
        InterlockedIncrement(&okInitialUpdate_);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        InterlockedIncrement(&skippedException_);
        LOGF("DetourTerrainFix: InitialUpdate EXCEPTION swallowed This=%p code=0x%08X\n",
             This, GetExceptionCode());
    }
}

//=============================================================================
// Static trampolines
//=============================================================================
void __fastcall DetourTerrainFix::DTSetCoords(void* This, void*, void* WorldCoords)
{
    if (sSelf_)
        sSelf_->OnSetCoords(This, WorldCoords);
    else if (fnSetCoords_.Fn_)
        fnSetCoords_.Fn_(This, WorldCoords);
}

void __fastcall DetourTerrainFix::DTInitialUpdate(void* This)
{
    if (sSelf_)
        sSelf_->OnInitialUpdate(This);
    else if (fnInitialUpdate_.Fn_)
        fnInitialUpdate_.Fn_(This);
}
