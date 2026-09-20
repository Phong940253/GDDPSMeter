#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cassert>
#include <math.h>

#include "DetourTeleport.h"
#include "DetourMain.h"
#include "DetourUtil.h"
#include "Logger.h"

static bool TripleEq(float ax, float ay, float az, float bx, float by, float bz);

//=============================================================================
// Static members
//=============================================================================
DetourTeleport *DetourTeleport::sDetourTeleport_ = NULL;

ThisFunc<void, void*, int, int, int, unsigned int, bool>
    DetourTeleport::fnInitiatePlayerTeleport_;
ThisFunc<bool, void*>
    DetourTeleport::fnMainPlayerCanUsePersonalTeleport_;
ThisFunc<void, void*, void*, bool>
    DetourTeleport::fnGetFootCoords_;
ThisFunc<void, void*, void*>
    DetourTeleport::fnSetCoords_;
ThisFunc<void, void*, const char*>
    DetourTeleport::fnDebugCreateEntity_;
ThisFunc<void, void*, void*, std::string&>
    DetourTeleport::fnCreateEntity_;
ThisFunc<unsigned int, void*>
    DetourTeleport::fnGetCharLevel_;
ThisFunc<void, void*, unsigned int, unsigned int>
    DetourTeleport::fnCharExpOutbound_;
ThisFunc<unsigned int, void*, unsigned int, unsigned int, unsigned int, void*>
    DetourTeleport::fnCalcExpReward_;
ThisFunc<void, void*, void*, void*>
    DetourTeleport::fnCreateItem_;
ThisFunc<void, void*, void*>
    DetourTeleport::fnGetCoords_;
ThisFunc<void, void*, void*>
    DetourTeleport::fnTranslateInRegion_;

//=============================================================================
// Detours data
//=============================================================================
static DetourFnData datInitiatePlayerTeleport = {
    "game.dll", NULL,
    (VoidFn)&DetourTeleport::DTInitiatePlayerTeleport,
    SYM_INITIATE_PLAYER_TELEPORT
};

static DetourFnData datMainPlayerCanUsePersonalTeleport = {
    "game.dll", NULL,
    (VoidFn)&DetourTeleport::DTMainPlayerCanUsePersonalTeleport,
    SYM_MAIN_PLAYER_CAN_USE_TELEPORT
};

// Call-only (no hook): resolve address so we can CALL GetFootCoords@Player
// to read the player's real current position as WorldCoords (52 bytes)
static DetourFnData datGetFootCoordsPlayer = {
    "game.dll", NULL,
    NULL,
    SYM_GET_FOOT_COORDS_PLAYER
};

// Call-only: Entity::SetCoords (Engine.dll) - the engine's own teleport
// setter. TeleportToSaved builds WorldCoords = current WC with rel triple
// overwritten by the saved spot, then calls this. Same-map only.
static DetourFnData datEntitySetCoords = {
    "engine.dll", NULL,
    NULL,
    SYM_ENTITY_SETCOORDS
};

// Call-only: Entity::GetCoords (Engine.dll) - origin-style triple,
// for feet-vs-origin diagnostic + TranslateInRegion delta math.
static DetourFnData datEntityGetCoords = {
    "engine.dll", NULL,
    NULL,
    SYM_ENTITY_GETCOORDS
};

// Call-only: WorldVec3::TranslateInRegion (Engine.dll) - exact-delta mover
// (preserves Y). Called with live player WorldVec3* found by FindLiveWV.
static DetourFnData datTranslateInRegion = {
    "engine.dll", NULL,
    NULL,
    SYM_TRANSLATE_IN_REGION
};

// Call-only: Game.dll entity spawn functions
static DetourFnData datDebugCreateEntity = {
    "game.dll", NULL,
    NULL,
    SYM_DEBUG_CREATE_ENTITY
};

static DetourFnData datCreateEntity = {
    "game.dll", NULL,
    NULL,
    SYM_CREATE_ENTITY
};

static DetourFnData datGetCharLevel = {
    "game.dll", NULL,
    NULL,
    SYM_GET_CHAR_LEVEL
};

// Hook (not just resolve): fires on every kill -> GameEngine* capture
static DetourFnData datCharExpOutbound = {
    "game.dll", NULL,
    (VoidFn)&DetourTeleport::DTCharacterExperience,
    SYM_CHAR_EXP_OUTBOUND
};

// SP kill/XP calc + loot drop (the Outbound one is network-only)
static DetourFnData datCalcExpReward = {
    "game.dll", NULL,
    (VoidFn)&DetourTeleport::DTCalcExpReward,
    SYM_CALC_EXP_REWARD
};

static DetourFnData datCreateItem = {
    "game.dll", NULL,
    (VoidFn)&DetourTeleport::DTCreateItem,
    SYM_CREATE_ITEM
};

//=============================================================================
// Constructor
//=============================================================================
DetourTeleport::DetourTeleport()
{
    sDetourTeleport_ = this;
    historyCount_ = 0;
    savedLocationCount_ = 0;
    lastX_ = lastY_ = lastZ_ = 0;
    curFx_ = curFy_ = curFz_ = 0.0f;
    hasFloatPos_ = false;
    hasWorldCoords_ = false;
    lastCaptureTime_ = 0;
    posOffset_ = -1;
    canTeleport_ = false;
    inTeleportMode_ = false;
    playerPtr_ = NULL;
    gameEnginePtr_ = NULL;
    sDetourMain_ = NULL;
    memset(history_, 0, sizeof(history_));
    memset(savedLocations_, 0, sizeof(savedLocations_));
    memset(lastWorldCoords_, 0, sizeof(lastWorldCoords_));
    memset(lastOriginWC_, 0, sizeof(lastOriginWC_));
    orgFx_ = orgFy_ = orgFz_ = 0.0f;
    hasOriginPos_ = false;
    playerWV_ = NULL;
    wvOff_ = -1;
    wvCand_ = -1;
    memset(subCur_, 0, sizeof(subCur_));
    phRel_[0] = phRel_[1] = phRel_[2] = 0.0f;
    phRegion_ = 0;
    phStable_ = 0;
    memset(snapPrev_, 0, sizeof(snapPrev_));
    hasSnapPrev_ = false;
    relPrev_[0] = relPrev_[1] = relPrev_[2] = 0.0f;
    snapPrevPlayer_ = NULL;
    memset(snapPrevSub_, 0, sizeof(snapPrevSub_));
    memset(snapPrevSubPtr_, 0, sizeof(snapPrevSubPtr_));
    snapPrevSubCount_ = 0;
    memset(regionOffsets_, 0, sizeof(regionOffsets_));
    regionOffsetCount_ = 0;
    pendingLearn_ = false;
    pendX_ = pendY_ = pendZ_ = 0;
    hookRegion_ = 0;
    hookEffect_ = (unsigned int)TE_Rift;
    hookPersonal_ = false;
    stableRel_[0] = stableRel_[1] = stableRel_[2] = 0.0f;
    stableRegion_ = 0;
    stableCount_ = 0;
    pendingAge_ = 0;
    fastPoll_ = false;
    fastCount_ = 0;
    hookTick_ = 0;
    hookRel_[0] = hookRel_[1] = hookRel_[2] = 0.0f;
    learnedOnce_ = false;
    learnedRel_[0] = learnedRel_[1] = learnedRel_[2] = 0.0f;
    memset(&pendingHop_, 0, sizeof(pendingHop_));
    stickActive_ = false;
    stickTarget_[0] = stickTarget_[1] = stickTarget_[2] = 0.0f;
    stickSource_[0] = stickSource_[1] = stickSource_[2] = 0.0f;
    stickCount_ = 0;
    memset(stickTag_, 0, sizeof(stickTag_));
    memset(lastTemplate_, 0, sizeof(lastTemplate_));
    templateLogCount_ = 0;
    memset(scanCache_, 0, sizeof(scanCache_));
    scanIdx_ = 0;
    memset(spawnTargets_, 0, sizeof(spawnTargets_));
    spawnTargetCount_ = 0;
    selectedSpawn_ = -1;
}

//=============================================================================
// SetupDetour
//=============================================================================
bool DetourTeleport::SetupDetour()
{
    int status = 0;

    status += HookDetour(datInitiatePlayerTeleport);
    fnInitiatePlayerTeleport_.SetFn(datInitiatePlayerTeleport.realFn_);
    LOGF("DetourTeleport: InitiatePlayerTeleport hook=%s addr=%p\n",
         datInitiatePlayerTeleport.realFn_ ? "OK" : "FAILED",
         datInitiatePlayerTeleport.realFn_);

    status += HookDetour(datMainPlayerCanUsePersonalTeleport);
    fnMainPlayerCanUsePersonalTeleport_.SetFn(datMainPlayerCanUsePersonalTeleport.realFn_);
    LOGF("DetourTeleport: MainPlayerCanUsePersonalTeleport hook=%s addr=%p\n",
         datMainPlayerCanUsePersonalTeleport.realFn_ ? "OK" : "FAILED",
         datMainPlayerCanUsePersonalTeleport.realFn_);

    // Resolve GetFootCoords@Player for CALLING (no hook) - reads real position
    status += HookDetour(datGetFootCoordsPlayer);
    fnGetFootCoords_.SetFn(datGetFootCoordsPlayer.realFn_);
    LOGF("DetourTeleport: GetFootCoords@Player resolve=%s addr=%p\n",
         datGetFootCoordsPlayer.realFn_ ? "OK" : "FAILED",
         datGetFootCoordsPlayer.realFn_);

    // Resolve Entity::SetCoords (Engine.dll) for direct teleport.
    // Call-only: do NOT fold into status (teleport degrades gracefully).
    HookDetour(datEntitySetCoords);
    fnSetCoords_.SetFn(datEntitySetCoords.realFn_);
    LOGF("DetourTeleport: Entity::SetCoords resolve=%s addr=%p\n",
         datEntitySetCoords.realFn_ ? "OK" : "FAILED",
         datEntitySetCoords.realFn_);

    HookDetour(datEntityGetCoords);
    fnGetCoords_.SetFn(datEntityGetCoords.realFn_);
    LOGF("DetourTeleport: Entity::GetCoords resolve=%s addr=%p\n",
         datEntityGetCoords.realFn_ ? "OK" : "FAILED",
         datEntityGetCoords.realFn_);

    HookDetour(datTranslateInRegion);
    fnTranslateInRegion_.SetFn(datTranslateInRegion.realFn_);
    LOGF("DetourTeleport: TranslateInRegion resolve=%s addr=%p\n",
         datTranslateInRegion.realFn_ ? "OK" : "FAILED",
         datTranslateInRegion.realFn_);

    HookDetour(datDebugCreateEntity);
    fnDebugCreateEntity_.SetFn(datDebugCreateEntity.realFn_);
    LOGF("DetourTeleport: DebugCreateEntity resolve=%s addr=%p\n",
         datDebugCreateEntity.realFn_ ? "OK" : "FAILED",
         datDebugCreateEntity.realFn_);

    HookDetour(datCreateEntity);
    fnCreateEntity_.SetFn(datCreateEntity.realFn_);
    LOGF("DetourTeleport: CreateEntity resolve=%s addr=%p\n",
         datCreateEntity.realFn_ ? "OK" : "FAILED",
         datCreateEntity.realFn_);

    HookDetour(datGetCharLevel);
    fnGetCharLevel_.SetFn(datGetCharLevel.realFn_);
    LOGF("DetourTeleport: GetCharLevel resolve=%s addr=%p\n",
         datGetCharLevel.realFn_ ? "OK" : "FAILED",
         datGetCharLevel.realFn_);

    status += HookDetour(datCharExpOutbound);
    fnCharExpOutbound_.SetFn(datCharExpOutbound.realFn_);
    LOGF("DetourTeleport: CharacterExperienceOutbound hook=%s addr=%p\n",
         datCharExpOutbound.realFn_ ? "OK" : "FAILED",
         datCharExpOutbound.realFn_);

    status += HookDetour(datCalcExpReward);
    fnCalcExpReward_.SetFn(datCalcExpReward.realFn_);
    LOGF("DetourTeleport: CalculateExperienceReward hook=%s addr=%p\n",
         datCalcExpReward.realFn_ ? "OK" : "FAILED",
         datCalcExpReward.realFn_);

    status += HookDetour(datCreateItem);
    fnCreateItem_.SetFn(datCreateItem.realFn_);
    LOGF("DetourTeleport: CreateItem hook=%s addr=%p\n",
         datCreateItem.realFn_ ? "OK" : "FAILED",
         datCreateItem.realFn_);

    if (status != 0)
    {
        OutputDebugStringA("DetourTeleport::SetupDetour - WARNING: some hooks failed (non-fatal)\n");
    }

    LOGF("DetourTeleport::SetupDetour done, status=%d\n", status);
    LoadFromFile();
    LoadRegions();
    LoadTargets();
    return status == 0;
}

//=============================================================================
// Update
//=============================================================================
void DetourTeleport::Update(void *player, int idx)
{
    // MainPlayerCanUsePersonalTeleport is a GameEngine method, not Player.
    // Passing player pointer as this to a GameEngine method causes crash.
    // Simple fix: always allow teleport when player is valid.
    if (player)
    {
        canTeleport_ = true;
        // Normal: capture every ~3s. Fast: every frame for ~10s after a
        // teleport so arrival-exact learning + pending-hop fire promptly.
        bool doCap = false;
        if (fastPoll_)
        {
            doCap = true;
            if (++fastCount_ > 600)
                fastPoll_ = false;
        }
        else
        {
            unsigned int now = (unsigned int)timeGetTime();
            if (now - lastCaptureTime_ > 3000)
                doCap = true;
        }
        if (doCap)
        {
            CaptureCurrentPosition(false);
            TryLearnOffset();  // detect rift arrival -> learn region offset
            UpdatePendingHop();  // cross-map teleport second leg
            if (!fastPoll_)
                UpdateStickCheck();  // slow cadence only (3s)
        }
    }
    else
    {
        canTeleport_ = false;
    }
}

//=============================================================================
// CaptureCurrentPosition - call GetFootCoords@Player to get REAL position
// WorldCoords = 52 bytes. Position floats live somewhere inside.
// verbose=true logs full hex + float table + offset scan (used by Save Here).
// verbose=false only updates curFx_/curFy_/curFz_ cache quietly.
// Returns true if a plausible float position was captured.
//=============================================================================
bool DetourTeleport::CaptureCurrentPosition(bool verbose)
{
    if (!playerPtr_ || !fnGetFootCoords_.Fn_)
        return false;

    lastCaptureTime_ = (unsigned int)timeGetTime();

    unsigned char buf[WORLDCOORDS_SIZE + 16];
    memset(buf, 0, sizeof(buf));

    __try
    {
        fnGetFootCoords_.Fn_(playerPtr_, buf, false);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        if (verbose)
            LOGF("DetourTeleport: GetFootCoords CRASHED (player=%p)\n", playerPtr_);
        return false;
    }

    memcpy(lastWorldCoords_, buf, WORLDCOORDS_SIZE);
    hasWorldCoords_ = true;

    // Origin-style triple via Entity::GetCoords (feet-vs-origin diagnostic).
    // Same 52B layout assumption (+0x04 triple).
    if (fnGetCoords_.Fn_)
    {
        unsigned char obuf[WORLDCOORDS_SIZE + 16];
        memset(obuf, 0, sizeof(obuf));
        __try
        {
            fnGetCoords_.Fn_(playerPtr_, obuf);
            memcpy(lastOriginWC_, obuf, WORLDCOORDS_SIZE);
            float *of = (float*)(obuf + 4);
            orgFx_ = of[0];
            orgFy_ = of[1];
            orgFz_ = of[2];
            hasOriginPos_ = true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            if (verbose)
                LOGF("DetourTeleport: GetCoords CRASHED\n");
        }
    }

    // Interpret buffer as float array to find plausible position triple.
    // World position floats are typically in range [-5000, 5000].
    float *f = (float*)buf;
    int nFloats = WORLDCOORDS_SIZE / 4;  // 13

    if (verbose)
    {
        LOGF("DetourTeleport: WorldCoords hex (player=%p):\n", playerPtr_);
        for (int i = 0; i < WORLDCOORDS_SIZE; i += 16)
        {
            LOGF("  +%02X: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\n",
                 i, buf[i], buf[i+1], buf[i+2], buf[i+3], buf[i+4], buf[i+5],
                 buf[i+6], buf[i+7], buf[i+8], buf[i+9], buf[i+10], buf[i+11],
                 buf[i+12], buf[i+13], buf[i+14], buf[i+15]);
        }
        LOGF("DetourTeleport: WorldCoords as floats+ints:\n");
        for (int i = 0; i < nFloats; ++i)
        {
            int iv = ((int*)buf)[i];
            LOGF("  [%2d] off 0x%02X: float=%12.4f int=%d hex=%08X\n",
                 i, i * 4, f[i], iv, (unsigned int)iv);
        }
        int *words = (int*)buf;
        LOGF("DetourTeleport: regionPtr=0x%08X\n", (unsigned int)words[0]);
    }

    // Layout hypothesis (from 2 rift samples + disasm):
    //   +0x00 Region* | +0x04,+0x08,+0x0C region-relative pos |
    //   +0x10..+0x30 orientation axes (unit vectors, e.g. facing at +0x28)
    // So canonical position = words[1..3]. The old "last plausible triple"
    // heuristic picked the facing vector - keep it only as cross-check.
    curFx_ = f[1];
    curFy_ = f[2];
    curFz_ = f[3];
    hasFloatPos_ = true;
    if (verbose)
    {
        LOGF("DetourTeleport: pos@0x04=(%.3f,%.3f,%.3f)\n", curFx_, curFy_, curFz_);
        if (hasOriginPos_)
            LOGF("DetourTeleport: origin=(%.3f,%.3f,%.3f) feet-origin=(%.3f,%.3f,%.3f)\n",
                 orgFx_, orgFy_, orgFz_,
                 curFx_ - orgFx_, curFy_ - orgFy_, curFz_ - orgFz_);
    }
    return true;
}

//=============================================================================
// ScanPositionOffset - find float triple (curFx_,curFy_,curFz_) inside the
// Player object memory. Returns byte offset or -1.
// Scans first 0x4000 bytes of player object with SEH protection.
//=============================================================================
int DetourTeleport::ScanPositionOffset()
{
    if (!playerPtr_ || !hasFloatPos_)
        return -1;
    if (posOffset_ >= 0)
        return posOffset_;  // already known

    float tx = curFx_, ty = curFy_, tz = curFz_;
    int found = -1;
    int matchCount = 0;

    __try
    {
        unsigned char *base = (unsigned char*)playerPtr_;
        for (int off = 0; off + 12 <= 0x4000; off += 4)
        {
            float *p = (float*)(base + off);
            // Exact float match (same bytes copied by engine)
            if (p[0] == tx && p[1] == ty && p[2] == tz)
            {
                if (found < 0)
                    found = off;
                matchCount++;
                if (matchCount <= 8)
                    LOGF("DetourTeleport: pos match #%d at player+0x%X\n", matchCount, off);
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LOGF("DetourTeleport: ScanPositionOffset exception (partial scan)\n");
    }

    LOGF("DetourTeleport: ScanPositionOffset target=(%.3f,%.3f,%.3f) matches=%d first=0x%X\n",
         tx, ty, tz, matchCount, found);
    if (found >= 0)
        posOffset_ = found;
    return found;
}

//=============================================================================
// ScanWorldCoordsMatches - diagnostic: map EVERY WorldCoords word into player
// memory (per-word hit counts) + best 52-byte alignment match (embedded copy).
// Tells us whether Entity embeds WorldCoords and which sub-fields are stored.
//=============================================================================
void DetourTeleport::ScanWorldCoordsMatches()
{
    if (!playerPtr_ || !hasWorldCoords_)
        return;

    int *words = (int*)lastWorldCoords_;
    float *fl = (float*)lastWorldCoords_;
    const int range = 0x4000;

    __try
    {
        unsigned char *base = (unsigned char*)playerPtr_;
        for (int w = 0; w < WORLDCOORDS_SIZE / 4; ++w)
        {
            int hits = 0;
            int first = -1;
            for (int off = 0; off + 4 <= range; off += 4)
            {
                if (*(int*)(base + off) == words[w])
                {
                    if (first < 0)
                        first = off;
                    if (++hits > 5)
                        break;
                }
            }
            LOGF("DetourTeleport: wcword[%2d] f=%12.4f hex=%08X -> hits=%d first=0x%X\n",
                 w, fl[w], (unsigned int)words[w], hits, first);
        }

        int bestOff = -1, bestCnt = -1, secondOff = -1, secondCnt = -1;
        for (int off = 0; off + WORLDCOORDS_SIZE <= range; off += 4)
        {
            int cnt = 0;
            for (int w = 0; w < WORLDCOORDS_SIZE / 4; ++w)
                if (*(int*)(base + off + w * 4) == words[w])
                    cnt++;
            if (cnt > bestCnt)
            {
                secondCnt = bestCnt; secondOff = bestOff;
                bestCnt = cnt; bestOff = off;
            }
            else if (cnt > secondCnt)
            {
                secondCnt = cnt; secondOff = off;
            }
        }
        LOGF("DetourTeleport: embedded52 best=0x%X (%d/13) second=0x%X (%d/13)\n",
             bestOff, bestCnt, secondOff, secondCnt);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LOGF("DetourTeleport: ScanWorldCoordsMatches exception\n");
    }
}

bool DetourTeleport::IsReadablePtr(void *p)
{
    if (!p)
        return false;
    unsigned int v = (unsigned int)p;
    // Heap range sanity (player ~0x22xxxxxx, region ~0x3Dxxxxxx)
    if (v < 0x01000000 || v >= 0x80000000 || (v & 3) != 0)
        return false;
    __try
    {
        volatile unsigned int x = *(unsigned int*)p;
        (void)x;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

//=============================================================================
// DeltaMatchBlock - generic triple-delta matcher over a byte block pair.
// Returns number of matches (logs first 20).
//=============================================================================
int DetourTeleport::DeltaMatchBlock(const unsigned char *prevBytes, const unsigned char *curBase,
                                    int size, const char *label)
{
    float rdx = curFx_ - relPrev_[0];
    float rdy = curFy_ - relPrev_[1];
    float rdz = curFz_ - relPrev_[2];

    int found = -1;
    int matchCount = 0;
    __try
    {
        for (int off = 0; off + 12 <= size; off += 4)
        {
            float *prev = (float*)(prevBytes + off);
            float *cur = (float*)(curBase + off);
            bool sane = true;
            for (int k = 0; k < 3; ++k)
            {
                float v = cur[k];
                if (!(v == v) || v < -20000.0f || v > 20000.0f)
                {
                    sane = false;
                    break;
                }
            }
            if (!sane)
                continue;
            float dx = cur[0] - prev[0];
            float dy = cur[1] - prev[1];
            float dz = cur[2] - prev[2];
            if (dx - rdx < 0.02f && dx - rdx > -0.02f &&
                dy - rdy < 0.02f && dy - rdy > -0.02f &&
                dz - rdz < 0.02f && dz - rdz > -0.02f)
            {
                if (found < 0)
                    found = off;
                matchCount++;
                if (matchCount <= 20)
                    LOGF("DetourTeleport: DELTA-MATCH %s #%d at +0x%X cur=(%.2f,%.2f,%.2f) prev=(%.2f,%.2f,%.2f)\n",
                         label, matchCount, off, cur[0], cur[1], cur[2], prev[0], prev[1], prev[2]);
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LOGF("DetourTeleport: DeltaMatchBlock %s exception\n", label);
    }
    return matchCount;
}

//=============================================================================
// ScanRegionNames - dump printable strings inside the Region object.
// Looking for a stable map identity (name/path) so cross-session teleport
// works without re-learning offsets every run. Diagnostic for now.
//=============================================================================
void DetourTeleport::ScanRegionNames(unsigned int region)
{
    if (!region)
        return;
    LOGF("DetourTeleport: region 0x%08X strings:\n", region);
    __try
    {
        unsigned char *base = (unsigned char*)region;
        int shown = 0;
        int off = 0;
        while (off + 6 <= 0x2000 && shown < 15)
        {
            // Find printable run
            int start = off;
            while (off < 0x2000 && base[off] >= 0x20 && base[off] <= 0x7E)
                off++;
            int len = off - start;
            if (len >= 5 && off < 0x2000 && base[off] == 0)
            {
                char tmp[128];
                int cp = len < 127 ? len : 127;
                memcpy(tmp, base + start, cp);
                tmp[cp] = 0;
                LOGF("  +0x%X: \"%s\"\n", start, tmp);
                shown++;
                off++;  // skip NUL
            }
            else if (len > 0)
            {
                // Long non-terminated run - skip it
                if (len >= 5)
                    off = start + 1;
            }
            else
            {
                off++;
            }
        }
        if (shown == 0)
            LOGF("  (no strings found)\n");
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LOGF("DetourTeleport: ScanRegionNames exception\n");
    }
}

//=============================================================================
// DirectTeleportTo - same-map teleport with authority anchoring:
//  1. Entity::SetCoords (authority + exact XZ)
//  2. TranslateInRegion residual (exact Y + fine XZ) when live WV found
// Arms stick-check monitoring. Returns true if arrival verified.
//=============================================================================
bool DetourTeleport::DirectTeleportTo(float fx, float fy, float fz,
                                      float ofx, float ofy, float ofz, bool hasOrigin,
                                      const char *tag)
{
    if (!playerPtr_ || !fnSetCoords_.Fn_ || !hasFloatPos_ || !hasWorldCoords_)
    {
        LOGF("DetourTeleport: %s refused (engine not ready)\n", tag);
        return false;
    }

    float srcFx = curFx_, srcFy = curFy_, srcFz = curFz_;

    // Step 1: authoritative SetCoords (exact XZ)
    unsigned char wc[WORLDCOORDS_SIZE];
    memcpy(wc, lastWorldCoords_, sizeof(wc));
    ((float*)(wc + 4))[0] = fx;
    ((float*)(wc + 4))[1] = fy;
    ((float*)(wc + 4))[2] = fz;
    LOGF("DetourTeleport: %s SetCoords (%.3f,%.3f,%.3f) -> (%.3f,%.3f,%.3f)\n",
         tag, srcFx, srcFy, srcFz, fx, fy, fz);
    __try
    {
        fnSetCoords_.Fn_(playerPtr_, wc);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LOGF("DetourTeleport: %s SetCoords CRASHED\n", tag);
        return false;
    }
    CaptureCurrentPosition(false);

    // Step 2: residual correction via live WorldVec3 (usually just Y)
    float ex = curFx_ - fx, ey = curFy_ - fy, ez = curFz_ - fz;
    if ((ex > 1.0f || ex < -1.0f || ey > 1.5f || ey < -1.5f || ez > 1.0f || ez < -1.0f) &&
        hasOrigin && hasOriginPos_ && fnTranslateInRegion_.Fn_ && FindLiveWV())
    {
        float dx, dy, dz;
        if (wvCand_ == 1)
        {
            dx = ofx - orgFx_;
            dy = ofy - orgFy_;
            dz = ofz - orgFz_;
        }
        else
        {
            dx = fx - curFx_;
            dy = fy - curFy_;
            dz = fz - curFz_;
        }
        if (dx * dx + dy * dy + dz * dz > 0.0025f)
        {
            float delta[3] = { dx, dy, dz };
            LOGF("DetourTeleport: %s residual Translate delta=(%.3f,%.3f,%.3f)\n",
                 tag, dx, dy, dz);
            __try
            {
                fnTranslateInRegion_.Fn_(playerWV_, delta);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                LOGF("DetourTeleport: %s residual Translate CRASHED\n", tag);
                playerWV_ = NULL;
            }
            CaptureCurrentPosition(false);
        }
    }

    ex = curFx_ - fx; ey = curFy_ - fy; ez = curFz_ - fz;
    bool arrived = (ex < 1.0f && ex > -1.0f && ey < 1.5f && ey > -1.5f && ez < 1.0f && ez > -1.0f);
    LOGF("DetourTeleport: %s arrived rel=(%.3f,%.3f,%.3f) err=(%.2f,%.2f,%.2f) %s\n",
         tag, curFx_, curFy_, curFz_, ex, ey, ez, arrived ? "OK" : "SHORT");

    // Arm stick-check: STABLE vs SNAPPED over next captures
    stickActive_ = true;
    stickTarget_[0] = fx; stickTarget_[1] = fy; stickTarget_[2] = fz;
    stickSource_[0] = srcFx; stickSource_[1] = srcFy; stickSource_[2] = srcFz;
    stickCount_ = 0;
    strncpy_s(stickTag_, tag, sizeof(stickTag_) - 1);
    return arrived;
}

//=============================================================================
// UpdateStickCheck - after a teleport, watch next captures: does the player
// stay at target (STABLE/STICK OK) or snap back toward source (RUBBER-BAND)?
// Runs on the slow 3s tick; ~24s window.
//=============================================================================
void DetourTeleport::UpdateStickCheck()
{
    if (!stickActive_ || !hasFloatPos_)
        return;
    float dtx = curFx_ - stickTarget_[0];
    float dty = curFy_ - stickTarget_[1];
    float dtz = curFz_ - stickTarget_[2];
    float dtSq = dtx * dtx + dty * dty + dtz * dtz;
    float dsx = curFx_ - stickSource_[0];
    float dsy = curFy_ - stickSource_[1];
    float dsz = curFz_ - stickSource_[2];
    float dsSq = dsx * dsx + dsy * dsy + dsz * dsz;
    stickCount_++;
    if (dtSq < 9.0f)  // within 3m of target
    {
        LOGF("DetourTeleport: stick '%s' #%d STABLE at target (d=%.1f)\n",
             stickTag_, stickCount_, sqrt(dtSq));
        if (stickCount_ >= 4)
        {
            LOGF("DetourTeleport: stick '%s' STICK OK\n", stickTag_);
            stickActive_ = false;
        }
        return;
    }
    if (dsSq < 9.0f && dtSq > 25.0f)  // back at source, far from target
    {
        LOGF("DetourTeleport: stick '%s' #%d SNAPPED BACK to source! (d_target=%.1f)\n",
             stickTag_, stickCount_, sqrt(dtSq));
        stickActive_ = false;
        return;
    }
    if (stickCount_ > 8)
    {
        LOGF("DetourTeleport: stick '%s' INCONCLUSIVE (d_target=%.1f d_source=%.1f)\n",
             stickTag_, stickCount_, sqrt(dtSq), sqrt(dsSq));
        stickActive_ = false;
    }
}

void DetourTeleport::DumpCurrentPosition()
{
    if (CaptureCurrentPosition(true))
    {
        ScanPositionOffset();
        ScanWorldCoordsMatches();
    }
}

//=============================================================================
// CaptureTargetTemplate - scan a fought entity's memory for its record path
// ("records/...dbr"). Fight anything once -> path captured -> Spawn works
// for EVERY boss with zero database extraction.
//=============================================================================
static bool ExtractRecordPath(const unsigned char *base, int size, char *out, int outSize)
{
    static const char kPrefix[] = "records/";
    for (int off = 0; off + 12 <= size; ++off)
    {
        if (memcmp(base + off, kPrefix, sizeof(kPrefix) - 1) != 0)
            continue;
        // Printable run after prefix (path chars). Accept runs ending in
        // ".dbr" even without NUL terminator (packed/SSO buffers).
        int maxLen = size - off;
        if (maxLen > 240)
            maxLen = 240;
        int len = (int)sizeof(kPrefix) - 1;
        while (len < maxLen && base[off + len] >= 0x20 && base[off + len] <= 0x7E &&
               base[off + len] != ',' && base[off + len] != '"' && base[off + len] != '\'')
            len++;
        if (len < (int)sizeof(kPrefix) + 5 || len < 4)
            continue;
        if (memcmp(base + off + len - 4, ".dbr", 4) != 0)
            continue;
        if (len >= outSize)
            continue;
        memcpy(out, base + off, len);
        out[len] = 0;
        return true;
    }
    return false;
}

void DetourTeleport::CaptureTargetTemplate(void *entity, const char *displayName)
{
    if (!entity)
        return;

    // Ring cache: damage hooks fire per tick - full scan once per entity.
    // Re-hit on a known entity re-selects its path silently.
    for (int i = 0; i < 8; ++i)
    {
        if (scanCache_[i].entity == entity)
        {
            if (scanCache_[i].hit && scanCache_[i].path[0] &&
                strcmp(scanCache_[i].path, lastTemplate_) != 0)
            {
                strncpy_s(lastTemplate_, scanCache_[i].path, sizeof(lastTemplate_) - 1);
                // Re-select roster entry without spamming file saves
                for (int k = 0; k < spawnTargetCount_; ++k)
                {
                    if (strcmp(spawnTargets_[k].path, lastTemplate_) == 0)
                    {
                        selectedSpawn_ = k;
                        break;
                    }
                }
            }
            return;
        }
    }

    char found[260] = {0};
    bool hit = false;

    __try
    {
        // 1. Inline scan (path stored directly in entity, wider range for
        //    big boss entities whose template sits further out)
        if (ExtractRecordPath((unsigned char*)entity, 0x6000, found, sizeof(found)))
        {
            hit = true;
        }
        else
        {
            // 2. Pointer chase: scan first 1KB of each readable target for
            //    the prefix ANYWHERE (not just at offset 0 - covers structs
            //    embedding the string/pointer deeper inside).
            unsigned int *w = (unsigned int*)entity;
            int checked = 0;
            for (int off = 0; off + 4 <= 0x6000 && !hit && checked < 128; off += 4)
            {
                void *p = (void*)w[off / 4];
                if (!IsReadablePtr(p))
                    continue;
                checked++;
                unsigned char blk[1024];
                memcpy(blk, p, sizeof(blk));
                hit = ExtractRecordPath(blk, sizeof(blk), found, sizeof(found));
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return;
    }

    // Record result in ring cache (both hits and misses - memory is stable)
    {
        int slot = scanIdx_ % 8;
        scanCache_[slot].entity = entity;
        scanCache_[slot].hit = hit && found[0] != 0;
        scanCache_[slot].scanned = true;
        if (scanCache_[slot].hit)
            strncpy_s(scanCache_[slot].path, found, sizeof(scanCache_[slot].path) - 1);
        else
            scanCache_[slot].path[0] = 0;
        scanIdx_++;
    }

    if (!hit || found[0] == 0)
    {
        static int missCount = 0;
        if (missCount < 5)
        {
            LOGF("DetourTeleport: template MISS entity=%p\n", entity);
            missCount++;
        }
        return;
    }
    // Spawn only makes sense for creatures (ignore crates, fx, etc.)
    if (memcmp(found, "records/creatures/", 18) != 0)
        return;
    if (strcmp(found, lastTemplate_) == 0)
        return;  // already known - no spam (attacks fire per swing)
    strncpy_s(lastTemplate_, found, sizeof(lastTemplate_) - 1);

    // Read original level (display only, never modified)
    unsigned int level = 0;
    if (fnGetCharLevel_.Fn_)
    {
        __try { level = fnGetCharLevel_.Fn_(entity); }
        __except (EXCEPTION_EXECUTE_HANDLER) { level = 0; }
    }
    AddSpawnTarget(displayName ? displayName : "unknown", found, level);
    if (templateLogCount_ < 40)
    {
        LOGF("DetourTeleport: target template [%d] lv=%u %s = %s\n",
             templateLogCount_, level,
             displayName ? displayName : "?", lastTemplate_);
        templateLogCount_++;
    }
}

//=============================================================================
// Spawn roster persistence (spawn_targets.txt, game dir): name|path|level
//=============================================================================
static const char *GetTargetsFilePath()
{
    static char path[MAX_PATH] = {0};
    if (path[0] == 0)
    {
        GetModuleFileNameA(NULL, path, MAX_PATH);
        char *lastSlash = strrchr(path, '\\');
        if (lastSlash)
            strcpy_s(lastSlash + 1, MAX_PATH - (lastSlash + 1 - path), "spawn_targets.txt");
    }
    return path;
}

void DetourTeleport::AddSpawnTarget(const char *name, const char *path, unsigned int level)
{
    // Dedup by path
    for (int i = 0; i < spawnTargetCount_; ++i)
    {
        if (strcmp(spawnTargets_[i].path, path) == 0)
        {
            bool changed = false;
            if (name && strcmp(spawnTargets_[i].name, name) != 0)
            {
                strncpy_s(spawnTargets_[i].name, name, sizeof(spawnTargets_[i].name) - 1);
                changed = true;
            }
            if (spawnTargets_[i].level != level)
            {
                spawnTargets_[i].level = level;
                changed = true;
            }
            selectedSpawn_ = i;
            if (changed)
                SaveTargets();  // no file write when nothing changed
            return;
        }
    }
    if (spawnTargetCount_ >= MAX_SPAWN_TARGETS)
        return;
    SpawnTarget &t = spawnTargets_[spawnTargetCount_];
    strncpy_s(t.name, name ? name : "unknown", sizeof(t.name) - 1);
    strncpy_s(t.path, path, sizeof(t.path) - 1);
    t.level = level;
    strncpy_s(t.cat, "Captured", sizeof(t.cat) - 1);
    selectedSpawn_ = spawnTargetCount_;
    spawnTargetCount_++;
    SaveTargets();
}

void DetourTeleport::SaveTargets()
{
    FILE *f = NULL;
    fopen_s(&f, GetTargetsFilePath(), "w");
    if (!f)
        return;
    fprintf(f, "# GDDPSMeter Spawn Targets\n");
    fprintf(f, "# name|path|level|cat\n");
    for (int i = 0; i < spawnTargetCount_; ++i)
        fprintf(f, "%s|%s|%u|%s\n", spawnTargets_[i].name, spawnTargets_[i].path,
                spawnTargets_[i].level, spawnTargets_[i].cat);
    fclose(f);
}

void DetourTeleport::LoadTargets()
{
    FILE *f = NULL;
    fopen_s(&f, GetTargetsFilePath(), "r");
    if (!f)
        return;
    char line[512];
    while (fgets(line, sizeof(line), f) && spawnTargetCount_ < MAX_SPAWN_TARGETS)
    {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\0')
            continue;
        char *name = strtok(line, "|");
        char *path = strtok(NULL, "|");
        char *slevel = strtok(NULL, "|");
        char *scat = strtok(NULL, "|");
        if (!name || !path)
            continue;
        size_t len = strlen(name);
        while (len > 0 && (name[len-1] == '\n' || name[len-1] == '\r'))
            name[--len] = '\0';
        SpawnTarget &t = spawnTargets_[spawnTargetCount_];
        strncpy_s(t.name, name, sizeof(t.name) - 1);
        strncpy_s(t.path, path, sizeof(t.path) - 1);
        t.level = slevel ? (unsigned int)atoi(slevel) : 0;
        if (scat)
        {
            // Trim trailing newline (last field)
            size_t cl = strlen(scat);
            while (cl > 0 && (scat[cl-1] == '\n' || scat[cl-1] == '\r'))
                scat[--cl] = '\0';
            strncpy_s(t.cat, scat, sizeof(t.cat) - 1);
        }
        else
        {
            strncpy_s(t.cat, "Captured", sizeof(t.cat) - 1);
        }
        spawnTargetCount_++;
    }
    fclose(f);
    LOGF("DetourTeleport: Loaded %d spawn targets\n", spawnTargetCount_);
}

const char *DetourTeleport::ActiveSpawnPath() const
{
    if (selectedSpawn_ >= 0 && selectedSpawn_ < spawnTargetCount_ &&
        spawnTargets_[selectedSpawn_].path[0])
        return spawnTargets_[selectedSpawn_].path;
    return lastTemplate_[0] ? lastTemplate_ : NULL;
}

// Spawn roster entry at player (facing*3m). No teleport needed.
bool DetourTeleport::SpawnRosterAtMe(int index)
{
    if (index < 0 || index >= spawnTargetCount_ || !spawnTargets_[index].path[0])
        return false;
    CaptureCurrentPosition(false);
    if (!hasFloatPos_ || !hasWorldCoords_)
        return false;
    // Facing vector at WC+0x28 (unit); fall back to +2m X if garbage
    float *fw = (float*)(lastWorldCoords_ + 0x28);
    float fl = fw[0] * fw[0] + fw[1] * fw[1] + fw[2] * fw[2];
    float sx, sy, sz;
    if (fl > 0.5f && fl < 2.0f)
    {
        sx = curFx_ + fw[0] * 3.0f;
        sy = curFy_ + fw[1] * 3.0f;
        sz = curFz_ + fw[2] * 3.0f;
    }
    else
    {
        sx = curFx_ + 2.0f;
        sy = curFy_;
        sz = curFz_;
    }
    unsigned int region = ((unsigned int*)lastWorldCoords_)[0];
    // Temporarily swap active path via lastTemplate_ copy
    char saved[260];
    strncpy_s(saved, lastTemplate_, sizeof(saved) - 1);
    strncpy_s(lastTemplate_, spawnTargets_[index].path, sizeof(lastTemplate_) - 1);
    bool ok = SpawnAtRegion(region, sx, sy, sz);
    strncpy_s(lastTemplate_, saved, sizeof(lastTemplate_) - 1);
    return ok;
}

// Spawn roster entry at a saved spot (same-session Region* required)
bool DetourTeleport::SpawnRosterAtSpot(int index, int spotIdx)
{
    if (index < 0 || index >= spawnTargetCount_ || !spawnTargets_[index].path[0])
        return false;
    if (spotIdx < 0 || spotIdx >= savedLocationCount_)
        return false;
    const SavedLocation &loc = savedLocations_[spotIdx];
    if (!loc.hasFloat || loc.region == 0)
    {
        LOGF("DetourTeleport: SpawnAtSpot refused (spot needs re-save this session)\n");
        return false;
    }
    char saved[260];
    strncpy_s(saved, lastTemplate_, sizeof(saved) - 1);
    strncpy_s(lastTemplate_, spawnTargets_[index].path, sizeof(lastTemplate_) - 1);
    bool ok = SpawnAtRegion(loc.region, loc.fx, loc.fy, loc.fz);
    strncpy_s(lastTemplate_, saved, sizeof(lastTemplate_) - 1);
    return ok;
}

//=============================================================================
// SpawnHere - PoC: DebugCreateEntity(lastTemplate_) - spawns at random
// nearby position. Proves path validity + AI + loot before exact placement.
//=============================================================================
bool DetourTeleport::SpawnHere()
{
    if (lastTemplate_[0] == 0 || !gameEnginePtr_ || !fnDebugCreateEntity_.Fn_)
    {
        LOGF("DetourTeleport: SpawnHere refused (template=%s engine=%p fn=%p)\n",
             lastTemplate_[0] ? lastTemplate_ : "(none)", gameEnginePtr_,
             fnDebugCreateEntity_.Fn_);
        return false;
    }
    LOGF("DetourTeleport: SpawnHere '%s'\n", lastTemplate_);
    __try
    {
        fnDebugCreateEntity_.Fn_(gameEnginePtr_, lastTemplate_);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LOGF("DetourTeleport: SpawnHere CRASHED\n");
        return false;
    }
    LOGF("DetourTeleport: SpawnHere call done\n");
    return true;
}

// Helper without SEH: builds the std::string (object unwinding) outside
// any __try block (C2712 forbids mixing them in one function).
static void InvokeCreateEntity(ThisFunc<void, void*, void*, std::string&> &fn,
                               void *engine, void *wc, const char *path)
{
    std::string s(path);
    fn.Fn_(engine, wc, s);
}

//=============================================================================
// SpawnAt - exact placement: CreateEntity(WorldCoords, path) at rel position.
// WorldCoords = current WC with rel triple overwritten (same map).
//=============================================================================
bool DetourTeleport::SpawnAt(float fx, float fy, float fz)
{
    if (!hasWorldCoords_)
    {
        LOGF("DetourTeleport: SpawnAt refused (no capture)\n");
        return false;
    }
    unsigned int region = ((unsigned int*)lastWorldCoords_)[0];
    return SpawnAtRegion(region, fx, fy, fz);
}

bool DetourTeleport::SpawnAtRegion(unsigned int region, float fx, float fy, float fz)
{
    if (lastTemplate_[0] == 0 || !gameEnginePtr_ || !fnCreateEntity_.Fn_)
    {
        LOGF("DetourTeleport: SpawnAt refused (template=%s engine=%p fn=%p)\n",
             lastTemplate_[0] ? lastTemplate_ : "(none)", gameEnginePtr_,
             fnCreateEntity_.Fn_);
        return false;
    }
    CaptureCurrentPosition(false);
    if (!hasWorldCoords_ || region == 0)
    {
        LOGF("DetourTeleport: SpawnAt refused (no capture)\n");
        return false;
    }
    unsigned char wc[WORLDCOORDS_SIZE];
    memcpy(wc, lastWorldCoords_, sizeof(wc));
    ((unsigned int*)wc)[0] = region;
    ((float*)(wc + 4))[0] = fx;
    ((float*)(wc + 4))[1] = fy;
    ((float*)(wc + 4))[2] = fz;

    LOGF("DetourTeleport: SpawnAt '%s' region=0x%08X rel=(%.3f,%.3f,%.3f)\n",
         lastTemplate_, region, fx, fy, fz);
    __try
    {
        InvokeCreateEntity(fnCreateEntity_, gameEnginePtr_, wc, lastTemplate_);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LOGF("DetourTeleport: SpawnAt CRASHED\n");
        return false;
    }
    LOGF("DetourTeleport: SpawnAt call done\n");
    return true;
}

//=============================================================================
// UpdatePendingHop - second leg of cross-map teleport.
// After the rift hop lands us on the spot's map (offset triple match +
// stability), TranslateInRegion the remaining delta. Timeout ~60s.
//=============================================================================
void DetourTeleport::UpdatePendingHop()
{
    if (!pendingHop_.active)
        return;
    if (!playerPtr_ || !hasFloatPos_ || !hasWorldCoords_ || !hasOriginPos_ || !fnTranslateInRegion_.Fn_)
    {
        pendingHop_.age++;
        if (pendingHop_.age > 20)
        {
            LOGF("DetourTeleport: pending hop '%s' timeout (no capture)\n", pendingHop_.name);
            pendingHop_.active = false;
        }
        return;
    }

    // Region-only stability (rel free to move - delta is computed fresh at
    // fire time; this just skips loading screens). With fast polling this
    // fires ~2 frames after arrival.
    unsigned int curRegion = ((unsigned int*)lastWorldCoords_)[0];
    if (curRegion != phRegion_)
    {
        phRegion_ = curRegion;
        phRel_[0] = orgFx_;
        phRel_[1] = orgFy_;
        phRel_[2] = orgFz_;
        phStable_ = 1;
    }
    else
    {
        phStable_++;
    }
    pendingHop_.stable = phStable_;
    pendingHop_.age++;

    if (pendingHop_.age > 600)
    {
        LOGF("DetourTeleport: pending hop '%s' timeout\n", pendingHop_.name);
        pendingHop_.active = false;
        return;
    }
    if (phStable_ < 2)
        return;

    // Are we on the spot's map? (offset triple match)
    const RegionOffset *curOff = FindOffset(curRegion);
    if (!curOff || !TripleEq(curOff->ox, curOff->oy, curOff->oz,
                             pendingHop_.ox, pendingHop_.oy, pendingHop_.oz))
        return;  // not there yet (or unknown map) - keep waiting

    // Fire second leg (SetCoords anchor + translate residual)
    char htag[192];
    sprintf_s(htag, "hop '%s'", pendingHop_.name);
    pendingHop_.active = false;
    DirectTeleportTo(pendingHop_.fx, pendingHop_.fy, pendingHop_.fz,
                     pendingHop_.ofx, pendingHop_.ofy, pendingHop_.ofz,
                     pendingHop_.hasOrg, htag);
}

//=============================================================================
// Region offset learning: offset[region] = riftDestInt - destRelFloat
// Fast path: per-frame captures right after teleport, learn on FIRST valid
// capture in the new region (player can't have walked yet) + settle-refine.
// Slow path: same-region portal via stillness rule. Wall-clock backstop.
//=============================================================================
const RegionOffset *DetourTeleport::FindOffset(unsigned int region) const
{
    if (region == 0)
        return NULL;
    for (int i = 0; i < regionOffsetCount_ && i < MAX_REGION_OFFSETS; ++i)
        if (regionOffsets_[i].valid && regionOffsets_[i].region == region)
            return &regionOffsets_[i];
    return NULL;
}

// Match by map-constant offset triple (works across sessions).
static bool TripleEq(float ax, float ay, float az, float bx, float by, float bz)
{
    float dx = ax - bx, dy = ay - by, dz = az - bz;
    return dx < 0.5f && dx > -0.5f && dy < 0.5f && dy > -0.5f && dz < 0.5f && dz > -0.5f;
}

const RegionOffset *DetourTeleport::FindOffsetByTriple(float ox, float oy, float oz) const
{
    for (int i = 0; i < regionOffsetCount_ && i < MAX_REGION_OFFSETS; ++i)
        if (regionOffsets_[i].valid && TripleEq(regionOffsets_[i].ox, regionOffsets_[i].oy, regionOffsets_[i].oz, ox, oy, oz))
            return &regionOffsets_[i];
    return NULL;
}

void DetourTeleport::StoreOffset(unsigned int region, float ox, float oy, float oz)
{
    int i;
    // 1. Same pointer: refresh values
    if (region != 0)
    {
        for (i = 0; i < regionOffsetCount_ && i < MAX_REGION_OFFSETS; ++i)
        {
            if (regionOffsets_[i].valid && regionOffsets_[i].region == region)
            {
                regionOffsets_[i].ox = ox;
                regionOffsets_[i].oy = oy;
                regionOffsets_[i].oz = oz;
                return;
            }
        }
    }
    // 2. Same map triple (e.g. file-loaded entry): fill in pointer
    for (i = 0; i < regionOffsetCount_ && i < MAX_REGION_OFFSETS; ++i)
    {
        if (regionOffsets_[i].valid && TripleEq(regionOffsets_[i].ox, regionOffsets_[i].oy, regionOffsets_[i].oz, ox, oy, oz))
        {
            if (region != 0)
                regionOffsets_[i].region = region;
            return;
        }
    }
    // 3. New slot (ring)
    int slot = regionOffsetCount_ < MAX_REGION_OFFSETS
        ? regionOffsetCount_++
        : (regionOffsetCount_++ % MAX_REGION_OFFSETS);
    regionOffsets_[slot].region = region;
    regionOffsets_[slot].ox = ox;
    regionOffsets_[slot].oy = oy;
    regionOffsets_[slot].oz = oz;
    regionOffsets_[slot].valid = true;
    regionOffsets_[slot].hasRift = false;
}

// Region offset table persistence (map-constant offsets + one known rift
// per map). Lets cross-map teleport work without re-learning every session.
static const char *GetRegionsFilePath()
{
    static char path[MAX_PATH] = {0};
    if (path[0] == 0)
    {
        GetModuleFileNameA(NULL, path, MAX_PATH);
        char *lastSlash = strrchr(path, '\\');
        if (lastSlash)
            strcpy_s(lastSlash + 1, MAX_PATH - (lastSlash + 1 - path), "teleport_regions.txt");
    }
    return path;
}

void DetourTeleport::SaveRegions()
{
    const char *filepath = GetRegionsFilePath();
    FILE *f = NULL;
    fopen_s(&f, filepath, "w");
    if (!f)
        return;
    fprintf(f, "# GDDPSMeter Region Offsets\n");
    fprintf(f, "# ox|oy|oz|x|y|z|effect|personal\n");
    for (int i = 0; i < regionOffsetCount_ && i < MAX_REGION_OFFSETS; ++i)
    {
        const RegionOffset &r = regionOffsets_[i];
        if (!r.valid || !r.hasRift)
            continue;
        fprintf(f, "%.4f|%.4f|%.4f|%d|%d|%d|%u|%d\n",
                r.ox, r.oy, r.oz, r.rx, r.ry, r.rz, r.reffect, r.rpersonal ? 1 : 0);
    }
    fclose(f);
}

void DetourTeleport::LoadRegions()
{
    const char *filepath = GetRegionsFilePath();
    FILE *f = NULL;
    fopen_s(&f, filepath, "r");
    if (!f)
        return;
    char line[256];
    while (fgets(line, sizeof(line), f))
    {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\0')
            continue;
        float ox, oy, oz;
        int x, y, z;
        unsigned int eff;
        int pers;
        if (sscanf_s(line, "%f|%f|%f|%d|%d|%d|%u|%d",
                     &ox, &oy, &oz, &x, &y, &z, &eff, &pers) != 8)
            continue;
        // Dedup by triple
        if (FindOffsetByTriple(ox, oy, oz))
            continue;
        int slot = regionOffsetCount_ < MAX_REGION_OFFSETS
            ? regionOffsetCount_++
            : (regionOffsetCount_++ % MAX_REGION_OFFSETS);
        RegionOffset &r = regionOffsets_[slot];
        r.region = 0;  // unknown until visited this session
        r.ox = ox; r.oy = oy; r.oz = oz;
        r.valid = true;
        r.rx = x; r.ry = y; r.rz = z;
        r.reffect = eff; r.rpersonal = pers != 0;
        r.hasRift = true;
    }
    fclose(f);
    LOGF("DetourTeleport: Loaded %d region offsets from %s\n", regionOffsetCount_, filepath);
}

void DetourTeleport::TryLearnOffset()
{
    if (!pendingLearn_ || !hasFloatPos_ || !hasWorldCoords_)
        return;

    unsigned int curRegion = ((unsigned int*)lastWorldCoords_)[0];
    unsigned int now = (unsigned int)timeGetTime();
    pendingAge_++;

    // Wall-clock backstop (~90s): give up
    if (now - hookTick_ > 90000)
    {
        LOGF("DetourTeleport: learn timeout (still region=0x%08X), discarding pending (%d,%d,%d)\n",
             curRegion, pendX_, pendY_, pendZ_);
        pendingLearn_ = false;
        return;
    }

    // Skip garbage captures (zero position during load)
    if (curFx_ == 0.0f && curFy_ == 0.0f && curFz_ == 0.0f)
        return;

    if (curRegion != hookRegion_)
    {
        // Cross-region arrival: learn on FIRST valid capture (player can't
        // have walked meaningfully within a frame or two of arrival).
        // Refine while settling (rel within 2m of learned point); ignore
        // far moves (user walked away - first value was closest to pad).
        if (!learnedOnce_)
        {
            DoLearn(curRegion);
            learnedOnce_ = true;
            learnedRel_[0] = curFx_;
            learnedRel_[1] = curFy_;
            learnedRel_[2] = curFz_;
        }
        else
        {
            float dx = curFx_ - learnedRel_[0];
            float dy = curFy_ - learnedRel_[1];
            float dz = curFz_ - learnedRel_[2];
            if (dx < 2.0f && dx > -2.0f && dy < 2.0f && dy > -2.0f && dz < 2.0f && dz > -2.0f)
            {
                DoLearn(curRegion);  // settle-refine
                learnedRel_[0] = curFx_;
                learnedRel_[1] = curFy_;
                learnedRel_[2] = curFz_;
            }
        }
        return;
    }

    // Same region (personal portal?): stability rule on slow captures.
    // Track stability (same region + same rel across captures)
    if (curRegion != stableRegion_ || curFx_ != stableRel_[0] ||
        curFy_ != stableRel_[1] || curFz_ != stableRel_[2])
    {
        stableRegion_ = curRegion;
        stableRel_[0] = curFx_;
        stableRel_[1] = curFy_;
        stableRel_[2] = curFz_;
        stableCount_ = 1;
    }
    else
    {
        stableCount_++;
    }
    if (stableCount_ < 2)
        return;
    // Only learn if actually displaced from hook point (else it's just source)
    {
        float dx = curFx_ - hookRel_[0];
        float dy = curFy_ - hookRel_[1];
        float dz = curFz_ - hookRel_[2];
        if (dx < 2.0f && dx > -2.0f && dy < 2.0f && dy > -2.0f && dz < 2.0f && dz > -2.0f)
            return;
    }
    DoLearn(curRegion);
    pendingLearn_ = false;
}

// Shared learn body: offset = pendInt - curRel + rift bookkeeping + log
void DetourTeleport::DoLearn(unsigned int curRegion)
{
    float ox = (float)pendX_ - curFx_;
    float oy = (float)pendY_ - curFy_;
    float oz = (float)pendZ_ - curFz_;
    StoreOffset(curRegion, ox, oy, oz);
    // Remember one working rift tile per map for cross-map hops
    RegionOffset *re = NULL;
    for (int i = 0; i < regionOffsetCount_ && i < MAX_REGION_OFFSETS; ++i)
        if (regionOffsets_[i].valid && regionOffsets_[i].region == curRegion) { re = &regionOffsets_[i]; break; }
    if (!re)
        re = (RegionOffset*)FindOffsetByTriple(ox, oy, oz);
    if (re && !re->hasRift)
    {
        re->rx = pendX_; re->ry = pendY_; re->rz = pendZ_;
        re->reffect = hookEffect_; re->rpersonal = hookPersonal_;
        re->hasRift = true;
        SaveRegions();
    }
    LOGF("DetourTeleport: LEARNED region=0x%08X offset=(%.3f,%.3f,%.3f) from rift (%d,%d,%d) rel=(%.3f,%.3f,%.3f)\n",
         curRegion, ox, oy, oz, pendX_, pendY_, pendZ_, curFx_, curFy_, curFz_);
    ScanRegionNames(curRegion);
}

//=============================================================================
// SnapshotPlayerMemory - copy first 0x4000 bytes of player object (SEH-safe)
//=============================================================================
int DetourTeleport::CollectSubObjects(unsigned char blocks[][SUBSNAP_SIZE], void *ptrs[])
{
    int count = 0;
    if (!playerPtr_)
        return 0;
    __try
    {
        unsigned int *w = (unsigned int*)playerPtr_;
        for (int off = 0; off + 4 <= (int)sizeof(snapPrev_) && count < SUBSNAP_MAX; off += 4)
        {
            void *p = (void*)w[off / 4];
            if (!IsReadablePtr(p))
                continue;
            bool dup = false;
            for (int i = 0; i < count; ++i)
                if (ptrs[i] == p) { dup = true; break; }
            if (dup)
                continue;
            ptrs[count] = p;
            memcpy(blocks[count], p, SUBSNAP_SIZE);
            count++;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LOGF("DetourTeleport: CollectSubObjects exception (kept %d)\n", count);
    }
    return count;
}

void DetourTeleport::SnapshotPlayerMemory()
{
    if (!playerPtr_)
        return;
    __try
    {
        memcpy(snapPrev_, playerPtr_, sizeof(snapPrev_));
        relPrev_[0] = curFx_;
        relPrev_[1] = curFy_;
        relPrev_[2] = curFz_;
        snapPrevPlayer_ = playerPtr_;
        hasSnapPrev_ = true;
        LOGF("DetourTeleport: snapshot saved (player=%p rel=%.3f,%.3f,%.3f)\n",
             playerPtr_, curFx_, curFy_, curFz_);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LOGF("DetourTeleport: SnapshotPlayerMemory exception\n");
        hasSnapPrev_ = false;
        return;
    }
    // Snapshot sub-objects behind heap pointers (baselines for next delta)
    snapPrevSubCount_ = CollectSubObjects(snapPrevSub_, snapPrevSubPtr_);
    LOGF("DetourTeleport: snapshot sub-objects=%d\n", snapPrevSubCount_);
}

//=============================================================================
// FindLiveWV - locate the player's live WorldVec3* for TranslateInRegion.
// Scans player + fresh sub-objects for the 16B pattern [Region*,x,y,z]
// (feet triple first, then origin triple). Each hit is validated with a
// 5cm micro-nudge: live objects move the player, stale copies don't.
// The nudge is restored immediately. Sets playerWV_ on success.
// Requires fresh capture beforehand (curFx_/orgFx_ current).
//=============================================================================
bool DetourTeleport::FindLiveWV()
{
    playerWV_ = NULL;
    wvOff_ = -1;
    wvCand_ = -1;
    if (!playerPtr_ || !fnTranslateInRegion_.Fn_ || !hasWorldCoords_)
        return false;

    unsigned int region = ((unsigned int*)lastWorldCoords_)[0];

    // Fresh sub-object snapshots into scratch
    void *subPtr[SUBSNAP_MAX];
    int subCount = CollectSubObjects(subCur_, subPtr);

    // Candidate triples: feet + origin (either may sit in the WorldVec3)
    float cand[2][3] = {
        { curFx_, curFy_, curFz_ },
        { orgFx_, orgFy_, orgFz_ }
    };
    bool candValid[2] = { hasFloatPos_, hasOriginPos_ };

    for (int c = 0; c < 2; ++c)
    {
        if (!candValid[c])
            continue;
        float tx = cand[c][0], ty = cand[c][1], tz = cand[c][2];

        // Gather block list: player object first, then sub-objects
        for (int b = -1; b < subCount; ++b)
        {
            unsigned char *base;
            int size;
            const char *bname;
            static char sname[64];
            if (b < 0) { base = (unsigned char*)playerPtr_; size = sizeof(snapPrev_); bname = "player"; }
            else
            {
                base = subCur_[b];
                size = SUBSNAP_SIZE;
                sprintf_s(sname, "sub[%d]=0x%p", b, subPtr[b]);
                bname = sname;
            }

            __try
            {
                for (int off = 0; off + 16 <= size; off += 4)
                {
                    unsigned int *ip = (unsigned int*)(base + off);
                    float *fp = (float*)(base + off);
                    if (ip[0] != region)
                        continue;
                    if (fp[1] != tx || fp[2] != ty || fp[3] != tz)
                        continue;
                    // Pattern hit: [Region*,x,y,z] at base+off.
                    // For sub-objects, address = subPtr[b]+off; player block
                    // is live memory so address = playerPtr_+off directly.
                    void *wvAddr = (b < 0)
                        ? (void*)((unsigned char*)playerPtr_ + off)
                        : (void*)((unsigned char*)subPtr[b] + off);
                    LOGF("DetourTeleport: WV pattern hit %s +0x%X (triple=%d), validating...\n",
                         bname, off, c);

                    // Micro-nudge validation: +5cm on X, then restore.
                    float before[3] = { curFx_, curFy_, curFz_ };
                    float nudge[3] = { 0.05f, 0.0f, 0.0f };
                    fnTranslateInRegion_.Fn_(wvAddr, nudge);
                    CaptureCurrentPosition(false);
                    float moved = curFx_ - before[0];
                    // Restore
                    float back[3] = { -0.05f, 0.0f, 0.0f };
                    fnTranslateInRegion_.Fn_(wvAddr, back);
                    CaptureCurrentPosition(false);

                    if (moved > 0.02f && moved < 0.2f)
                    {
                        playerWV_ = wvAddr;
                        wvOff_ = off;
                        wvCand_ = c;
                        LOGF("DetourTeleport: LIVE WorldVec3 confirmed at %s +0x%X (nudge moved %.3f, triple=%d)\n",
                             bname, off, moved, c);
                        return true;
                    }
                    LOGF("DetourTeleport: WV candidate %s +0x%X rejected (moved %.3f)\n",
                         bname, off, moved);
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                LOGF("DetourTeleport: WV scan exception in %s\n", bname);
            }
        }
    }

    LOGF("DetourTeleport: FindLiveWV found nothing\n");
    return false;
}

//=============================================================================
// DeltaMatchSnapshots - compare current player memory vs previous snapshot.
// Within one region worldDelta == relDelta, so a triple whose delta matches
// the rel-triple delta is the WORLD position. Logs matches (first 20).
// On a unique match, sets posOffset_ for direct-memory teleport.
//=============================================================================
void DetourTeleport::DeltaMatchSnapshots()
{
    if (!hasSnapPrev_ || !playerPtr_ || playerPtr_ != snapPrevPlayer_ || !hasFloatPos_)
        return;

    float rdx = curFx_ - relPrev_[0];
    float rdy = curFy_ - relPrev_[1];
    float rdz = curFz_ - relPrev_[2];
    LOGF("DetourTeleport: delta-match relDelta=(%.3f,%.3f,%.3f)\n", rdx, rdy, rdz);

    // Ignore tiny moves (need real displacement to discriminate)
    float moveDist = rdx * rdx + rdy * rdy + rdz * rdz;
    if (moveDist < 1.0f)
    {
        LOGF("DetourTeleport: delta-match skipped (moved too little)\n");
        return;
    }

    int found = -1;
    int matchCount = 0;
    // Player block via shared helper (label "player")
    if (playerPtr_ && hasSnapPrev_)
    {
        matchCount = DeltaMatchBlock(snapPrev_, (unsigned char*)playerPtr_,
                                     sizeof(snapPrev_), "player");
        LOGF("DetourTeleport: delta-match player-block matches=%d\n", matchCount);
    }

    // Sub-objects behind heap pointers in player memory (movement manager,
    // controller, physics...). Collect current targets, match same-ptr baselines.
    void *curPtr[SUBSNAP_MAX];
    int curCount = 0;
    __try
    {
        unsigned int *w = (unsigned int*)playerPtr_;
        for (int off = 0; off + 4 <= (int)sizeof(snapPrev_) && curCount < SUBSNAP_MAX; off += 4)
        {
            void *p = (void*)w[off / 4];
            if (!IsReadablePtr(p))
                continue;
            bool dup = false;
            for (int i = 0; i < curCount; ++i)
                if (curPtr[i] == p) { dup = true; break; }
            if (!dup)
                curPtr[curCount++] = p;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LOGF("DetourTeleport: sub-object collect exception\n");
    }
    LOGF("DetourTeleport: sub-object targets cur=%d prev=%d\n", curCount, snapPrevSubCount_);

    unsigned char curSub[SUBSNAP_SIZE];
    for (int i = 0; i < curCount; ++i)
    {
        int pi = -1;
        for (int j = 0; j < snapPrevSubCount_; ++j)
            if (snapPrevSubPtr_[j] == curPtr[i]) { pi = j; break; }
        if (pi < 0)
            continue;  // new pointer, no baseline yet
        __try
        {
            memcpy(curSub, curPtr[i], sizeof(curSub));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            continue;
        }
        char label[64];
        sprintf_s(label, "sub[%d]=0x%p", i, curPtr[i]);
        int n = DeltaMatchBlock(snapPrevSub_[pi], curSub, sizeof(curSub), label);
        if (n > 0)
            LOGF("DetourTeleport: delta-match %s matches=%d\n", label, n);
    }

    LOGF("DetourTeleport: delta-match done (posOffset_ NOT auto-set; review log first)\n");
}

void DetourTeleport::GetCurrentFloatPos(float &x, float &y, float &z) const
{
    x = curFx_;
    y = curFy_;
    z = curFz_;
}

//=============================================================================
// AddToHistory
//=============================================================================
void DetourTeleport::AddToHistory(int x, int y, int z,
                                   TeleportEffect effect, bool isPersonal)
{
    // Avoid exact duplicate
    for (int i = 0; i < historyCount_ && i < MAX_TELEPORT_HISTORY; ++i)
    {
        if (history_[i].x == x && history_[i].y == y && history_[i].z == z)
        {
            history_[i].timestamp = (unsigned int)timeGetTime();
            return;
        }
    }

    int slot = historyCount_ < MAX_TELEPORT_HISTORY
        ? historyCount_++
        : (historyCount_++ % MAX_TELEPORT_HISTORY);

    history_[slot].x = x;
    history_[slot].y = y;
    history_[slot].z = z;
    history_[slot].effect = effect;
    history_[slot].isPersonal = isPersonal;
    history_[slot].timestamp = (unsigned int)timeGetTime();

    const char* effectName = "Teleport";
    switch (effect)
    {
    case TE_Rift:           effectName = "Rift"; break;
    case TE_Personal:       effectName = "Personal"; break;
    case TE_DevilsCrossing: effectName = "Devil's Crossing"; break;
    case TE_Homestead:      effectName = "Homestead"; break;
    case TE_FortIkon:       effectName = "Fort Ikon"; break;
    case TE_Malmouth:       effectName = "Malmouth"; break;
    default: break;
    }
    sprintf_s(history_[slot].areaName, "%s (%d,%d,%d)", effectName, x, y, z);
    LOGF("DetourTeleport: history [%d] = %s\n", slot, history_[slot].areaName);
}

//=============================================================================
// TeleportTo (history)
//=============================================================================
void DetourTeleport::TeleportTo(int index)
{
    if (index < 0 || index >= historyCount_ || !gameEnginePtr_ || !fnInitiatePlayerTeleport_.Fn_)
        return;

    const TeleportDest &dest = history_[index];
    LOGF("TeleportTo history [%d] = (%d,%d,%d)\n", index, dest.x, dest.y, dest.z);
    fnInitiatePlayerTeleport_.Fn_(gameEnginePtr_, dest.x, dest.y, dest.z,
                                   (unsigned int)dest.effect, dest.isPersonal);
}

//=============================================================================
// IsSavedTeleportReady - can we directly teleport to index right now?
// Two sufficient paths:
//  A. Same session + same map: spot's Region* == current Region*.
//     (No rift/offset learning needed - the core save/teleport loop.)
//  B. Cross-session: learned offset for the CURRENT map matches the spot's
//     saved offset (same map, pointers differ across runs).
// reason[] gets a short user-facing explanation when returning false.
//=============================================================================
bool DetourTeleport::IsSavedTeleportReady(int index, char *reason, int reasonSize)
{
    if (reason && reasonSize > 0)
        reason[0] = '\0';

    if (index < 0 || index >= savedLocationCount_)
        return false;

    const SavedLocation &loc = savedLocations_[index];

    if (!playerPtr_ || !fnSetCoords_.Fn_)
    {
        if (reason) strncpy_s(reason, reasonSize, "engine not ready", _TRUNCATE);
        return false;
    }
    if (!hasFloatPos_ || !hasWorldCoords_)
    {
        if (reason) strncpy_s(reason, reasonSize, "reading position...", _TRUNCATE);
        return false;
    }
    if (!loc.hasFloat)
    {
        if (reason) strncpy_s(reason, reasonSize, "re-save this spot", _TRUNCATE);
        return false;
    }
    unsigned int curRegion = ((unsigned int*)lastWorldCoords_)[0];

    // Path A: same session, same map (Region* comparable)
    if (loc.region != 0 && loc.region == curRegion)
        return true;

    // Path B: cross-session same map via learned offsets
    const RegionOffset *curOff = FindOffset(curRegion);
    if (curOff && loc.hasOffset)
    {
        float dx = loc.ox - curOff->ox;
        float dy = loc.oy - curOff->oy;
        float dz = loc.oz - curOff->oz;
        if (dx < 0.5f && dx > -0.5f && dy < 0.5f && dy > -0.5f && dz < 0.5f && dz > -0.5f)
            return true;  // same map: direct teleport
    }

    // Path C: different map but a known rift leads there (two-leg teleport:
    // rift hop now, auto direct-hop on arrival)
    if (loc.hasOffset)
    {
        const RegionOffset *dst = FindOffsetByTriple(loc.ox, loc.oy, loc.oz);
        if (dst && dst->hasRift)
            return true;
        if (reason) strncpy_s(reason, reasonSize, "rift-travel there once first", _TRUNCATE);
        return false;
    }

    if (!curOff)
    {
        if (reason) strncpy_s(reason, reasonSize, "rift-travel to this map first", _TRUNCATE);
        return false;
    }
    if (reason) strncpy_s(reason, reasonSize, "re-save this spot", _TRUNCATE);
    return false;
}

//=============================================================================
// TeleportToSaved (saved custom location)
// Path 1 (preferred): WorldVec3::TranslateInRegion with exact delta -
//   engine-authored move, preserves Y exactly (no floor re-snap).
// Path 2 (fallback): Entity::SetCoords - exact XZ, Y may re-snap on
//   multi-level geometry (proven on flat ground).
//=============================================================================
void DetourTeleport::TeleportToSaved(int index)
{
    char reason[128];
    if (!IsSavedTeleportReady(index, reason, sizeof(reason)))
    {
        LOGF("TeleportToSaved [%d] refused: %s\n", index, reason);
        return;
    }

    const SavedLocation &loc = savedLocations_[index];

    // Fresh capture so delta math uses current position
    CaptureCurrentPosition(false);
    if (!hasFloatPos_ || !hasWorldCoords_)
    {
        LOGF("TeleportToSaved [%d] refused: capture failed\n", index);
        return;
    }
    unsigned int curRegion = ((unsigned int*)lastWorldCoords_)[0];

    // ── Same-map? (pointer equality or offset-triple equality) ──
    bool sameMap = false;
    if (loc.region != 0 && loc.region == curRegion)
    {
        sameMap = true;
    }
    else if (loc.hasOffset)
    {
        const RegionOffset *curOff = FindOffset(curRegion);
        if (curOff && TripleEq(loc.ox, loc.oy, loc.oz, curOff->ox, curOff->oy, curOff->oz))
            sameMap = true;
    }

    if (!sameMap)
    {
        // ── Cross-map: rift hop to a known rift on the spot's map,
        // then auto direct-hop on arrival (UpdatePendingHop) ──
        pendingHop_.active = false;
        if (!loc.hasOffset)
        {
            LOGF("TeleportToSaved [%d] refused: re-save this spot\n", index);
            return;
        }
        const RegionOffset *dst = FindOffsetByTriple(loc.ox, loc.oy, loc.oz);
        if (!dst || !dst->hasRift)
        {
            LOGF("TeleportToSaved [%d] refused: no known rift on that map (rift-travel there once first)\n", index);
            return;
        }
        if (!gameEnginePtr_ || !fnInitiatePlayerTeleport_.Fn_)
        {
            LOGF("TeleportToSaved [%d] refused: engine not ready\n", index);
            return;
        }
        pendingHop_.active = true;
        pendingHop_.fx = loc.fx; pendingHop_.fy = loc.fy; pendingHop_.fz = loc.fz;
        pendingHop_.ofx = loc.ofx; pendingHop_.ofy = loc.ofy; pendingHop_.ofz = loc.ofz;
        pendingHop_.ox = loc.ox; pendingHop_.oy = loc.oy; pendingHop_.oz = loc.oz;
        pendingHop_.hasOrg = loc.hasOrigin;
        pendingHop_.stable = 0;
        pendingHop_.age = 0;
        strncpy_s(pendingHop_.name, loc.name, sizeof(pendingHop_.name) - 1);
        phRegion_ = curRegion;
        phRel_[0] = orgFx_; phRel_[1] = orgFy_; phRel_[2] = orgFz_;
        phStable_ = 1;
        LOGF("TeleportToSaved [%d] '%s' cross-map: rift hop to (%d,%d,%d), auto-hop armed\n",
             index, loc.name, dst->rx, dst->ry, dst->rz);
        fnInitiatePlayerTeleport_.Fn_(gameEnginePtr_, dst->rx, dst->ry, dst->rz,
                                       dst->reffect, dst->rpersonal);
        return;
    }

    // ── Same map: direct teleport. Clear any stale pending hop. ──
    pendingHop_.active = false;

    char dtag[192];
    sprintf_s(dtag, "tele '%s'", loc.name);
    DirectTeleportTo(loc.fx, loc.fy, loc.fz,
                     loc.ofx, loc.ofy, loc.ofz, loc.hasOrigin, dtag);
}

//=============================================================================
// AddSavedLocation - save CURRENT player position with a name
// Uses GetFootCoords (real float position), NOT last teleport coords.
//=============================================================================
bool DetourTeleport::AddSavedLocation(const char *name)
{
    if (!name || strlen(name) == 0 || savedLocationCount_ >= MAX_SAVED_LOCATIONS)
        return false;
    if (!playerPtr_)
        return false;

    // Capture real current position (verbose: dumps WorldCoords + scans offset)
    CaptureCurrentPosition(true);
    if (posOffset_ < 0)
        ScanPositionOffset();
    ScanWorldCoordsMatches();
    // Learn region offset if a rift just completed, then compute global ints
    TryLearnOffset();
    // Research snapshots (delta-match world-pos search, independent path)
    DeltaMatchSnapshots();
    SnapshotPlayerMemory();

    SavedLocation &loc = savedLocations_[savedLocationCount_];
    strncpy_s(loc.name, name, sizeof(loc.name) - 1);
    // Real float position for direct-memory teleport
    loc.fx = curFx_;
    loc.fy = curFy_;
    loc.fz = curFz_;
    loc.hasFloat = hasFloatPos_;
    // Origin triple for TranslateInRegion delta math
    loc.ofx = orgFx_;
    loc.ofy = orgFy_;
    loc.ofz = orgFz_;
    loc.hasOrigin = hasOriginPos_;
    loc.hasOffset = false;
    loc.ox = loc.oy = loc.oz = 0.0f;
    loc.hasInt = false;
    loc.x = loc.y = loc.z = 0;
    unsigned int curRegion = hasWorldCoords_ ? ((unsigned int*)lastWorldCoords_)[0] : 0;
    const RegionOffset *ro = curRegion ? FindOffset(curRegion) : NULL;
    if (ro && hasFloatPos_)
    {
        // int = round(rel + offset)
        float ix = curFx_ + ro->ox;
        float iy = curFy_ + ro->oy;
        float iz = curFz_ + ro->oz;
        loc.x = (int)(ix >= 0.0f ? ix + 0.5f : ix - 0.5f);
        loc.y = (int)(iy >= 0.0f ? iy + 0.5f : iy - 0.5f);
        loc.z = (int)(iz >= 0.0f ? iz + 0.5f : iz - 0.5f);
        loc.hasInt = true;
        loc.hasOffset = true;
        loc.ox = ro->ox;
        loc.oy = ro->oy;
        loc.oz = ro->oz;
    }
    else
    {
        // No learned offset for this region yet: keep last-hook ints as
        // rift-replay fallback (valid only if nonzero = a real teleport).
        loc.x = lastX_;
        loc.y = lastY_;
        loc.z = lastZ_;
        loc.hasInt = (lastX_ != 0 || lastY_ != 0 || lastZ_ != 0);
        if (!loc.hasInt)
            LOGF("DetourTeleport: no offset for region=0x%08X - rift-travel to this map first, then re-save\n", curRegion);
    }
    // WORLD position + region (valid only when offset already known)
    loc.hasWorld = false;
    loc.region = hasWorldCoords_ ? ((unsigned int*)lastWorldCoords_)[0] : 0;
    if (posOffset_ >= 0 && playerPtr_ && hasWorldCoords_)
    {
        __try
        {
            float *p = (float*)((unsigned char*)playerPtr_ + posOffset_);
            loc.wx = p[0];
            loc.wy = p[1];
            loc.wz = p[2];
            loc.hasWorld = true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            loc.hasWorld = false;
        }
    }
    loc.timestamp = (unsigned int)timeGetTime();

    LOGF("AddSavedLocation [%d] '%s' int=(%d,%d,%d) rel=(%.3f,%.3f,%.3f) hasFloat=%d org=(%.3f,%.3f,%.3f) hasOrg=%d region=0x%08X\n",
         savedLocationCount_, loc.name, loc.x, loc.y, loc.z,
         loc.fx, loc.fy, loc.fz, loc.hasFloat ? 1 : 0,
         loc.ofx, loc.ofy, loc.ofz, loc.hasOrigin ? 1 : 0, loc.region);
    savedLocationCount_++;
    SaveToFile();
    return true;
}

//=============================================================================
// RemoveSavedLocation
//=============================================================================
bool DetourTeleport::RemoveSavedLocation(int index)
{
    if (index < 0 || index >= savedLocationCount_)
        return false;

    // Shift remaining entries
    for (int i = index; i < savedLocationCount_ - 1; ++i)
    {
        savedLocations_[i] = savedLocations_[i + 1];
    }
    savedLocationCount_--;
    SaveToFile();
    return true;
}

//=============================================================================
// GetCurrentPosition
//=============================================================================
void DetourTeleport::GetCurrentPosition(int &x, int &y, int &z) const
{
    x = lastX_;
    y = lastY_;
    z = lastZ_;
}

//=============================================================================
// File persistence - save/load to teleport_saves.txt next to the DLL
//=============================================================================
static const char* GetSaveFilePath()
{
    static char path[MAX_PATH] = {0};
    if (path[0] == 0)
    {
        // Save next to the DLL
        GetModuleFileNameA(NULL, path, MAX_PATH);
        // Replace exe name with teleport_saves.txt
        char *lastSlash = strrchr(path, '\\');
        if (lastSlash)
        {
            strcpy_s(lastSlash + 1, MAX_PATH - (lastSlash + 1 - path), "teleport_saves.txt");
        }
    }
    return path;
}

void DetourTeleport::SaveToFile()
{
    const char *filepath = GetSaveFilePath();
    FILE *f = NULL;
    fopen_s(&f, filepath, "w");
    if (!f)
    {
        LOGF("DetourTeleport: Failed to save to %s\n", filepath);
        return;
    }

    fprintf(f, "# GDDPSMeter Teleport Saves v3\n");
    fprintf(f, "# name|x|y|z|hasInt|fx|fy|fz|ox|oy|oz|ofx|ofy|ofz\n");

    for (int i = 0; i < savedLocationCount_; ++i)
    {
        fprintf(f, "%s|%d|%d|%d|%d|%.4f|%.4f|%.4f|%.4f|%.4f|%.4f|%.4f|%.4f|%.4f\n", savedLocations_[i].name,
                savedLocations_[i].x, savedLocations_[i].y, savedLocations_[i].z,
                savedLocations_[i].hasInt ? 1 : 0,
                savedLocations_[i].fx, savedLocations_[i].fy, savedLocations_[i].fz,
                savedLocations_[i].ox, savedLocations_[i].oy, savedLocations_[i].oz,
                savedLocations_[i].ofx, savedLocations_[i].ofy, savedLocations_[i].ofz);
    }

    fclose(f);
    LOGF("DetourTeleport: Saved %d locations to %s\n", savedLocationCount_, filepath);
}

void DetourTeleport::LoadFromFile()
{
    const char *filepath = GetSaveFilePath();
    FILE *f = NULL;
    fopen_s(&f, filepath, "r");
    if (!f)
    {
        // First run - create default saves
        LOGF("DetourTeleport: No save file, creating defaults\n");
        savedLocationCount_ = 0;

        // Default: Zaria from user's screenshot (ints only, no float yet -
        // user should delete + re-save after this build captures real pos)
        strncpy_s(savedLocations_[0].name, "Zaria", sizeof(savedLocations_[0].name) - 1);
        savedLocations_[0].x = 98;
        savedLocations_[0].y = 7;
        savedLocations_[0].z = 41;
        savedLocations_[0].hasInt = true;  // legacy hardcoded rift-style entry
        savedLocations_[0].fx = savedLocations_[0].fy = savedLocations_[0].fz = 0.0f;
        savedLocations_[0].hasFloat = false;
        savedLocations_[0].ox = savedLocations_[0].oy = savedLocations_[0].oz = 0.0f;
        savedLocations_[0].hasOffset = false;
        savedLocations_[0].hasWorld = false;
        savedLocations_[0].region = 0;
        savedLocations_[0].wx = savedLocations_[0].wy = savedLocations_[0].wz = 0.0f;
        savedLocations_[0].timestamp = (unsigned int)timeGetTime();
        savedLocationCount_ = 1;

        SaveToFile();
        return;
    }

    savedLocationCount_ = 0;
    char line[512];

    while (fgets(line, sizeof(line), f) && savedLocationCount_ < MAX_SAVED_LOCATIONS)
    {
        // Skip comments and empty lines
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\0')
            continue;

        // Parse v3: name|x|y|z|hasInt|fx|fy|fz|ox|oy|oz|ofx|ofy|ofz
        //       v2: name|x|y|z|hasInt|fx|fy|fz|ox|oy|oz
        //       v1: name|x|y|z[|fx|fy|fz[|region|wx|wy|wz]]
        //       v0: name|x|y|z
        // v0/v1 nonzero ints = real rift replays -> hasInt=true.
        // (0,0,0) = never teleported -> hasInt=false (refused at teleport).
        char *toks[14];
        int ntok = 0;
        toks[ntok++] = strtok(line, "|");
        while (ntok < 14)
        {
            char *t = strtok(NULL, "|");
            if (!t)
                break;
            toks[ntok++] = t;
        }

        if (ntok >= 4 && toks[0] && toks[1] && toks[2] && toks[3])
        {
            SavedLocation &loc = savedLocations_[savedLocationCount_];
            char *name = toks[0];
            // Trim trailing newline from name
            size_t len = strlen(name);
            while (len > 0 && (name[len-1] == '\n' || name[len-1] == '\r'))
                name[--len] = '\0';

            strncpy_s(loc.name, name, sizeof(loc.name) - 1);
            loc.x = atoi(toks[1]);
            loc.y = atoi(toks[2]);
            loc.z = atoi(toks[3]);
            loc.hasInt = (loc.x != 0 || loc.y != 0 || loc.z != 0);
            loc.fx = loc.fy = loc.fz = 0.0f;
            loc.hasFloat = false;
            loc.ox = loc.oy = loc.oz = 0.0f;
            loc.hasOffset = false;
            loc.ofx = loc.ofy = loc.ofz = 0.0f;
            loc.hasOrigin = false;

            if (ntok >= 14)
            {
                // v3 full line
                loc.hasInt = (atoi(toks[4]) != 0) && loc.hasInt;
                loc.fx = (float)atof(toks[5]);
                loc.fy = (float)atof(toks[6]);
                loc.fz = (float)atof(toks[7]);
                loc.hasFloat = true;
                loc.ox = (float)atof(toks[8]);
                loc.oy = (float)atof(toks[9]);
                loc.oz = (float)atof(toks[10]);
                loc.hasOffset = true;
                loc.ofx = (float)atof(toks[11]);
                loc.ofy = (float)atof(toks[12]);
                loc.ofz = (float)atof(toks[13]);
                loc.hasOrigin = true;
            }
            else if (ntok >= 11)
            {
                // v2 full line
                loc.hasInt = (atoi(toks[4]) != 0) && loc.hasInt;
                loc.fx = (float)atof(toks[5]);
                loc.fy = (float)atof(toks[6]);
                loc.fz = (float)atof(toks[7]);
                loc.hasFloat = true;
                loc.ox = (float)atof(toks[8]);
                loc.oy = (float)atof(toks[9]);
                loc.oz = (float)atof(toks[10]);
                loc.hasOffset = true;
            }
            else if (ntok >= 7 && toks[4] && toks[5] && toks[6])
            {
                // v1 floats (region/world tail ignored)
                loc.fx = (float)atof(toks[4]);
                loc.fy = (float)atof(toks[5]);
                loc.fz = (float)atof(toks[6]);
                loc.hasFloat = true;
            }
            // Dormant mem-write fields: re-captured per session on re-save
            loc.hasWorld = false;
            loc.region = 0;
            loc.wx = loc.wy = loc.wz = 0.0f;
            loc.timestamp = (unsigned int)timeGetTime();
            savedLocationCount_++;
        }
    }

    fclose(f);
    LOGF("DetourTeleport: Loaded %d locations from %s\n", savedLocationCount_, filepath);
}

//=============================================================================
// Instance hook handlers
//=============================================================================
void DetourTeleport::OnInitiatePlayerTeleport(void* This, int x, int y, int z,
                                               unsigned int effect, bool isPersonal)
{
    LOGF("InitiatePlayerTeleport(%d,%d,%d) effect=%u personal=%d\n",
         x, y, z, effect, isPersonal);

    // This is the GameEngine* — save it for calling teleport later
    sDetourTeleport_->gameEnginePtr_ = This;

    // Cache current position for "Save Location" feature
    lastX_ = x;
    lastY_ = y;
    lastZ_ = z;

    // Record in history
    AddToHistory(x, y, z, (TeleportEffect)effect, isPersonal);

    // Arm region-offset learning: destination ints + source region.
    // Once arrival stabilizes in a new region, offset = dest - rel.
    // Refresh current rel/region now so hookRegion_ is the SOURCE region.
    CaptureCurrentPosition(false);
    hookRegion_ = hasWorldCoords_ ? ((unsigned int*)lastWorldCoords_)[0] : 0;
    pendX_ = x;
    pendY_ = y;
    pendZ_ = z;
    hookEffect_ = effect;
    hookPersonal_ = isPersonal;
    stableRegion_ = hookRegion_;
    stableRel_[0] = curFx_;
    stableRel_[1] = curFy_;
    stableRel_[2] = curFz_;
    stableCount_ = 1;
    pendingAge_ = 0;
    pendingLearn_ = true;
    hookTick_ = (unsigned int)timeGetTime();
    hookRel_[0] = curFx_;
    hookRel_[1] = curFy_;
    hookRel_[2] = curFz_;
    learnedOnce_ = false;
    fastPoll_ = true;  // per-frame captures until arrival settles
    fastCount_ = 0;
    LOGF("DetourTeleport: learn armed dest=(%d,%d,%d) srcRegion=0x%08X\n",
         x, y, z, hookRegion_);

    // Call original
    fnInitiatePlayerTeleport_.Fn_(This, x, y, z, effect, isPersonal);
}

bool DetourTeleport::OnMainPlayerCanUsePersonalTeleport(void* This)
{
    bool result = fnMainPlayerCanUsePersonalTeleport_.Fn_(This);
    canTeleport_ = result;
    return result;
}

void DetourTeleport::OnCharacterExperience(void* This, unsigned int a, unsigned int b)
{
    // Every kill grants XP -> reliable GameEngine* capture (works even if
    // injected mid-session, no rift needed)
    if (This)
        CaptureGameEngine(This);
    fnCharExpOutbound_.Fn_(This, a, b);
}

unsigned int DetourTeleport::OnCalcExpReward(void* This, unsigned int a, unsigned int b,
                                             unsigned int c, void *vec)
{
    if (This)
        CaptureGameEngine(This);
    return fnCalcExpReward_.Fn_(This, a, b, c, vec);
}

void DetourTeleport::OnCreateItem(void* This, void *wc, void *info)
{
    if (This)
        CaptureGameEngine(This);
    fnCreateItem_.Fn_(This, wc, info);
}

//=============================================================================
// Static trampolines
//=============================================================================
void __fastcall DetourTeleport::DTInitiatePlayerTeleport(
    void* This, void*,
    int x, int y, int z, unsigned int effect, bool isPersonal)
{
    sDetourTeleport_->OnInitiatePlayerTeleport(This, x, y, z, effect, isPersonal);
}

bool __fastcall DetourTeleport::DTMainPlayerCanUsePersonalTeleport(
    void* This, void*)
{
    return sDetourTeleport_->OnMainPlayerCanUsePersonalTeleport(This);
}

void __fastcall DetourTeleport::DTCharacterExperience(
    void* This, void*, unsigned int a, unsigned int b)
{
    sDetourTeleport_->OnCharacterExperience(This, a, b);
}

unsigned int __fastcall DetourTeleport::DTCalcExpReward(
    void* This, void*, unsigned int a, unsigned int b,
    unsigned int c, void *vec)
{
    return sDetourTeleport_->OnCalcExpReward(This, a, b, c, vec);
}

void __fastcall DetourTeleport::DTCreateItem(
    void* This, void*, void *wc, void *info)
{
    sDetourTeleport_->OnCreateItem(This, wc, info);
}
