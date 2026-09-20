#pragma once
#include <string>
#include "DetourBase.h"

//=============================================================================
// Teleport feature for GDDPSMeter
// Hooks InitiatePlayerTeleport to build a teleport list.
// User adds boss/custom locations by standing at the spot in-game
// and clicking "Save Current Position".
//=============================================================================

class DetourMain;

// Mangled symbols from Game.dll / Engine.dll
#define SYM_INITIATE_PLAYER_TELEPORT  "?InitiatePlayerTeleport@GameEngine@GAME@@QAEXHHHW4TeleportEffect@2@_N@Z"
#define SYM_MAIN_PLAYER_CAN_USE_TELEPORT "?MainPlayerCanUsePersonalTeleport@GameEngine@GAME@@QBE_NXZ"
#define SYM_IS_IN_TELEPORT_MODE       "?IsInTeleportMode@Engine@GAME@@QBE_NXZ"
#define SYM_SET_TELEPORT_MODE         "?SetTeleportMode@Engine@GAME@@QAEX_N@Z"
// Win32 (x86) versions - verified in game_exports.txt
#define SYM_GET_FOOT_COORDS_PLAYER  "?GetFootCoords@Player@GAME@@UAE?AVWorldCoords@2@_N@Z"
#define SYM_GET_FOOT_COORDS_CHAR    "?GetFootCoords@Character@GAME@@MAE?AVWorldCoords@2@_N@Z"
#define SYM_GET_CHAT_COORDS         "?GetChatCoords@Character@GAME@@UBE?AVWorldCoords@2@XZ"
#define SYM_GET_SPAWN_POINT          "?GetSpawnPoint@Character@GAME@@QBEABVWorldCoords@2@XZ"
// Game.dll entity spawn (verified via dumpbin exports)
#define SYM_DEBUG_CREATE_ENTITY "?DebugCreateEntity@GameEngine@GAME@@QAEXPBD@Z"
#define SYM_CREATE_ENTITY "?CreateEntity@GameEngine@GAME@@QAEXABVWorldCoords@2@ABV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@@Z"
#define SYM_GET_CHAR_LEVEL "?GetCharLevel@Character@GAME@@QBE?BIXZ"
// Fires on every kill (XP grant) - robust GameEngine* capture point
#define SYM_CHAR_EXP_OUTBOUND "?CharacterExperienceOutbound@GameEngine@GAME@@QAEXII@Z"
// Single-player kill/XP calc + item drops - capture points that actually
// fire offline (Outbound variants are network-only)
#define SYM_CALC_EXP_REWARD "?CalculateExperienceReward@GameEngine@GAME@@QAEIIIIABVWorldVec3@2@@Z"
#define SYM_CREATE_ITEM "?CreateItem@GameEngine@GAME@@QAEXABVWorldCoords@2@AAUItemReplicaInfo@2@@Z"

// Persistent roster of captured spawn targets (name|path|level)
struct SpawnTarget
{
    char name[128];
    char path[260];
    unsigned int level;
    char cat[32];  // BossQuest, Nemesis, Hero, Bounty, Boss, Captured
};
#define MAX_SPAWN_TARGETS 4000
// Engine.dll - direct entity teleport toolkit (verified via Engine.dll exports)
#define SYM_ENTITY_GETCOORDS  "?GetCoords@Entity@GAME@@QBE?AVWorldCoords@2@XZ"
#define SYM_ENTITY_SETCOORDS  "?SetCoords@Entity@GAME@@IAEXABVWorldCoords@2@@Z"
// Engine's own in-region mover: void (WorldVec3* This, const Vec3& delta).
// Preserves exact Y (no floor re-snap) - preferred teleport primitive.
#define SYM_TRANSLATE_IN_REGION "?TranslateInRegion@WorldVec3@GAME@@QAEXABVVec3@2@@Z"
// Old (wrong/arch-mismatched) names kept for reference - DO NOT USE:
//   "?GetFootCoords@Character@GAME@@MEAA?AVWorldCoords@2@_N@Z" is x64
//   "?GetChatCoords@Character@GAME@@UEBA?AVWorldCoords@2@XZ" is x64

// WorldCoords returned by GetFootCoords: 52 bytes (0x34)
// (3x 16-byte movups + 1 dword, see Game.dll 0x453A0 disasm)
#define WORLDCOORDS_SIZE 52

// Sub-object snapshot budget (position lives behind heap pointers)
#define SUBSNAP_MAX 48
#define SUBSNAP_SIZE 0x1000

// TeleportEffect enum
enum TeleportEffect : unsigned int
{
    TE_None          = 0,
    TE_Rift          = 1,
    TE_Personal      = 2,
    TE_DevilsCrossing = 3,
    TE_Homestead     = 4,
    TE_FortIkon      = 5,
    TE_Malmouth      = 6,
    TE_Elite         = 7,
    TE_Ultimate      = 8,
};

// Saved teleport destination (auto-recorded from rift/personal teleports)
struct TeleportDest
{
    int x;
    int y;
    int z;
    TeleportEffect effect;
    bool isPersonal;
    char areaName[128];
    unsigned int timestamp;
};

// User-saved custom location (boss, farming spot, etc.)
// x,y,z = GLOBAL tile coords for InitiatePlayerTeleport (rift replay or
// computed as rel + regionOffset). hasInt=false means unknown -> refuse.
struct SavedLocation
{
    char name[128];
    int x;
    int y;
    int z;
    bool hasInt;    // true if x,y,z usable for teleport
    float fx;       // region-relative pos (from GetFootCoords)
    float fy;
    float fz;
    bool hasFloat;  // true if fx/fy/fz valid
    float ox;       // learned region offset: int = rel + offset
    float oy;
    float oz;
    bool hasOffset; // true if ox/oy/oz valid
    float ofx;      // ORIGIN triple (Entity::GetCoords) - for TranslateInRegion
    float ofy;
    float ofz;
    bool hasOrigin; // true if ofx/ofy/ofz valid
    float wx;       // WORLD pos (dormant: mem-write path disabled)
    float wy;
    float wz;
    bool hasWorld;
    unsigned int region;
    unsigned int timestamp;
};

// Learned int<->rel conversion for one region.
// region = Region* (per-session). ox,oy,oz = map-constant offset (persistent).
// rx,ry,rz = a known rift tile in this map (for cross-map hops).
struct RegionOffset
{
    unsigned int region;
    float ox;
    float oy;
    float oz;
    bool valid;
    int rx;
    int ry;
    int rz;
    unsigned int reffect;
    bool rpersonal;
    bool hasRift;
};
#define MAX_REGION_OFFSETS 32

// Pending cross-map teleport: after rift-hop arrival, auto direct-hop.
struct PendingHop
{
    bool active;
    float fx;   // feet target rel
    float fy;
    float fz;
    float ofx;  // origin target rel
    float ofy;
    float ofz;
    float ox;   // spot map offset triple
    float oy;
    float oz;
    bool hasOrg;  // origin triple valid
    int stable;
    int age;
    char name[128];
};

#define MAX_TELEPORT_HISTORY  50
#define MAX_SAVED_LOCATIONS  100

//=============================================================================
//=============================================================================
class DetourTeleport : public DetourBase
{
public:
    DetourTeleport();

    virtual bool SetupDetour();
    virtual void Update(void *player, int idx);
    virtual void SetPlayer(void* player) { playerPtr_ = player; }

    // Capture GameEngine* from any GameEngine method hook
    void CaptureGameEngine(void* engine) { if (!gameEnginePtr_) gameEnginePtr_ = engine; }

    // Instance methods
    void OnInitiatePlayerTeleport(void* This, int x, int y, int z,
                                   unsigned int effect, bool isPersonal);
    bool OnMainPlayerCanUsePersonalTeleport(void* This);
    void OnCharacterExperience(void* This, unsigned int a, unsigned int b);
    unsigned int OnCalcExpReward(void* This, unsigned int a, unsigned int b,
                                 unsigned int c, void *vec);
    void OnCreateItem(void* This, void *wc, void *info);
    bool HasEngine() const { return gameEnginePtr_ != NULL; }

    // Static trampolines
    static void __fastcall DTInitiatePlayerTeleport(void* This, void*,
                                                     int x, int y, int z,
                                                     unsigned int effect, bool isPersonal);
    static bool __fastcall DTMainPlayerCanUsePersonalTeleport(void* This, void*);
    static void __fastcall DTCharacterExperience(void* This, void*,
                                                 unsigned int a, unsigned int b);
    static unsigned int __fastcall DTCalcExpReward(void* This, void*,
                                                   unsigned int a, unsigned int b,
                                                   unsigned int c, void *vec);
    static void __fastcall DTCreateItem(void* This, void*, void *wc, void *info);

    // ── History (auto-recorded from teleports) ──
    const TeleportDest* GetHistory() const { return history_; }
    int GetHistoryCount() const { return historyCount_; }
    void TeleportTo(int index);

    // ── Boss spawn (universal: template captured from live target) ──
    // Fight anything once -> record .dbr path captured -> Spawn copy.
    const char *GetLastTemplate() const { return lastTemplate_; }
    void CaptureTargetTemplate(void *entity, const char *displayName);
    bool SpawnHere();  // PoC: DebugCreateEntity(lastTemplate_) near player
    bool SpawnAt(float fx, float fy, float fz);  // exact: CreateEntity @ rel
    // Exact spawn in a (possibly other-region, same session) map:
    // WorldCoords = {region, rel, current axes}
    bool SpawnAtRegion(unsigned int region, float fx, float fy, float fz);
    // ── Spawn roster (persistent across sessions) ──
    const SpawnTarget *GetSpawnTargets() const { return spawnTargets_; }
    int GetSpawnTargetCount() const { return spawnTargetCount_; }
    int GetSelectedSpawn() const { return selectedSpawn_; }
    void SetSelectedSpawn(int idx) { selectedSpawn_ = idx; }
    // Active path = selected roster entry, else last captured template
    const char *ActiveSpawnPath() const;
    bool SpawnRosterAtMe(int index);    // rel = me + facing*3
    bool SpawnRosterAtSpot(int index, int spotIdx);  // needs spot region
    void SaveTargets();
    void LoadTargets();

    // ── Saved locations (user-added boss/farm spots) ──
    const SavedLocation* GetSavedLocations() const { return savedLocations_; }
    int GetSavedLocationCount() const { return savedLocationCount_; }
    void TeleportToSaved(int index);
    bool AddSavedLocation(const char *name);  // saves current player position
    bool RemoveSavedLocation(int index);
    bool IsPlayerInWorld() const { return playerPtr_ != NULL; }
    void GetCurrentPosition(int &x, int &y, int &z) const;
    // Float position (from GetFootCoords) for UI display + direct teleport
    void GetCurrentFloatPos(float &x, float &y, float &z) const;
    int GetPositionOffset() const { return posOffset_; }
    bool HasFloatPos() const { return hasFloatPos_; }
    // True if direct (engine SetCoords) teleport to index is possible now.
    // reason[reasonSize] receives user-facing explanation when false.
    bool IsSavedTeleportReady(int index, char *reason, int reasonSize);
    // Diagnostic: capture current WorldCoords + scan player memory for offset
    void DumpCurrentPosition();

    // File persistence
    void SaveToFile();
    void LoadFromFile();

    // ── State ──
    bool IsTeleportAvailable() const { return canTeleport_; }
    bool IsInTeleportMode() const { return inTeleportMode_; }

    void SetParent(DetourMain *parent) { sDetourMain_ = parent; }

private:
    void AddToHistory(int x, int y, int z, TeleportEffect effect, bool isPersonal);

    // History (auto-recorded)
    TeleportDest history_[MAX_TELEPORT_HISTORY];
    int historyCount_;

    // Saved locations (user-added)
    SavedLocation savedLocations_[MAX_SAVED_LOCATIONS];
    int savedLocationCount_;

    // Current position cache (updated from InitiatePlayerTeleport hook)
    int lastX_, lastY_, lastZ_;

    // Float world position (from GetFootCoords@Player, updated in Update)
    float curFx_, curFy_, curFz_;
    bool hasFloatPos_;
    unsigned char lastWorldCoords_[WORLDCOORDS_SIZE];
    bool hasWorldCoords_;
    // Origin-style triple (Entity::GetCoords) - feet-vs-origin diagnostic
    // + TranslateInRegion delta math (must be origin-consistent)
    unsigned char lastOriginWC_[WORLDCOORDS_SIZE];
    float orgFx_, orgFy_, orgFz_;
    bool hasOriginPos_;
    // Live player WorldVec3* for TranslateInRegion (found by 16B pattern
    // scan + micro-nudge validation, refreshed per teleport)
    void *playerWV_;
    int wvOff_;
    int wvCand_;  // 0 = feet triple matched, 1 = origin triple matched
    unsigned char subCur_[SUBSNAP_MAX][SUBSNAP_SIZE];
    // Pending-hop private stability (separate from offset learning)
    float phRel_[3];
    unsigned int phRegion_;
    int phStable_;
    unsigned int lastCaptureTime_;
    // Offset of float position triple inside Player object (-1 = unknown)
    // Found by scanning player memory for GetFootCoords float bytes
    int posOffset_;

    bool CaptureCurrentPosition(bool verbose);  // calls GetFootCoords, updates curF*/lastWorldCoords_
    int ScanPositionOffset();  // scan player memory for cur float triple, returns offset or -1
    void ScanWorldCoordsMatches();  // diagnostic: map all WC words into player memory

    // Snapshot of player memory for delta-match teleport offset search.
    // Player stores WORLD coords; GetFootCoords gives REGION-relative coords.
    // Within one region: worldDelta == relDelta, so comparing two snapshots
    // taken at two Save Here spots pinpoints the world-pos offset.
    unsigned char snapPrev_[0x4000];
    bool hasSnapPrev_;
    float relPrev_[3];
    void *snapPrevPlayer_;
    void SnapshotPlayerMemory();
    void DeltaMatchSnapshots();
    // Collect heap sub-objects of player into blocks[]/ptrs[] (fresh).
    // Returns count. Shared by SnapshotPlayerMemory and FindLiveWV.
    int CollectSubObjects(unsigned char blocks[][SUBSNAP_SIZE], void *ptrs[]);
    // Find live player WorldVec3* via 16B [Region*,x,y,z] pattern + nudge test.
    // Sets playerWV_ on success. Needs fresh capture beforehand.
    bool FindLiveWV();

    // ── Region offset learning (int tile <-> rel float conversion) ──
    // Hook records rift destination ints; once arrival is stable in the new
    // region, offset[region] = destInt - destRel. Custom spots in that
    // region then compute global ints as rel + offset.
    RegionOffset regionOffsets_[MAX_REGION_OFFSETS];
    int regionOffsetCount_;
    bool pendingLearn_;
    int pendX_, pendY_, pendZ_;
    unsigned int hookRegion_;
    unsigned int hookEffect_;
    bool hookPersonal_;
    float stableRel_[3];
    unsigned int stableRegion_;
    int stableCount_;
    int pendingAge_;
    // Fast arrival-exact learning: capture per-frame right after teleport,
    // learn on first new-region capture (player can't have walked yet).
    bool fastPoll_;
    int fastCount_;
    unsigned int hookTick_;
    float hookRel_[3];
    bool learnedOnce_;
    float learnedRel_[3];
    void TryLearnOffset();
    void DoLearn(unsigned int curRegion);
    const RegionOffset *FindOffset(unsigned int region) const;
    const RegionOffset *FindOffsetByTriple(float ox, float oy, float oz) const;
    void StoreOffset(unsigned int region, float ox, float oy, float oz);
    void SaveRegions();
    void LoadRegions();
    PendingHop pendingHop_;
    void UpdatePendingHop();
    // Same-map direct teleport: SetCoords anchor (authority+XZ) then
    // TranslateInRegion residual (exact Y). Arms stick-check. Tag for logs.
    bool DirectTeleportTo(float fx, float fy, float fz,
                          float ofx, float ofy, float ofz, bool hasOrigin,
                          const char *tag);
    void UpdateStickCheck();
    void ScanRegionNames(unsigned int region);
    bool stickActive_;
    float stickTarget_[3];
    float stickSource_[3];
    int stickCount_;
    char stickTag_[64];
    // Sub-object snapshots (position likely lives behind a pointer:
    // movement manager / controller / physics). Up to 48 heap targets.
    unsigned char snapPrevSub_[SUBSNAP_MAX][SUBSNAP_SIZE];
    void *snapPrevSubPtr_[SUBSNAP_MAX];
    int snapPrevSubCount_;
    static bool IsReadablePtr(void *p);
    // Delta-match one block pair; label for logging. Returns match count.
    int DeltaMatchBlock(const unsigned char *prevBytes, const unsigned char *curBase,
                        int size, const char *label);

    bool canTeleport_;
    bool inTeleportMode_;
    void *playerPtr_;
    void *gameEnginePtr_;  // captured from InitiatePlayerTeleport hook's This pointer
    DetourMain *sDetourMain_;

    static DetourTeleport *sDetourTeleport_;

    // Original function pointers
    static ThisFunc<void, void*, int, int, int, unsigned int, bool> fnInitiatePlayerTeleport_;
    static ThisFunc<bool, void*> fnMainPlayerCanUsePersonalTeleport_;
    // GetFootCoords@Player: WorldCoords __thiscall (bool)
    // Returns via hidden ptr -> ThisFunc<void, This, retBuf, bool>
    // Call as fn(playerPtr, buf, false); WorldCoords size = 52 bytes
    static ThisFunc<void, void*, void*, bool> fnGetFootCoords_;
    // Entity::SetCoords: void __thiscall (Entity* This, const WorldCoords&)
    // Engine's own teleport setter - call as fn(playerPtr, wcBuf52)
    static ThisFunc<void, void*, void*> fnSetCoords_;
    // DebugCreateEntity: void __thiscall (GameEngine*, const char* path)
    // Spawns template at random nearby pos - PoC, no ABI risk.
    static ThisFunc<void, void*, const char*> fnDebugCreateEntity_;
    // CreateEntity: void __thiscall (GameEngine*, WorldCoords&, std::string&)
    // Exact placement. MSVCP140 ABI matches our VS2022 Release build.
    static ThisFunc<void, void*, void*, std::string&> fnCreateEntity_;
    // Last captured target template path (from fought entity memory)
    char lastTemplate_[260];
    int templateLogCount_;
    // Per-entity scan cache ring (damage hooks fire per tick - scan once).
    // Re-hitting a known entity re-selects its path silently.
    struct ScanCache { void *entity; bool scanned; bool hit; char path[260]; };
    ScanCache scanCache_[8];
    int scanIdx_;
    // GetCharLevel: unsigned __thiscall (Character*) - read-only, no fix
    static ThisFunc<unsigned int, void*> fnGetCharLevel_;
    // CharacterExperienceOutbound hook: void (GameEngine*, uint, uint)
    static ThisFunc<void, void*, unsigned int, unsigned int> fnCharExpOutbound_;
    // CalculateExperienceReward hook: uint (GameEngine*, uint, uint, uint, Vec3*)
    static ThisFunc<unsigned int, void*, unsigned int, unsigned int, unsigned int, void*> fnCalcExpReward_;
    // CreateItem hook: void (GameEngine*, WorldCoords*, ItemInfo*)
    static ThisFunc<void, void*, void*, void*> fnCreateItem_;
    SpawnTarget spawnTargets_[MAX_SPAWN_TARGETS];
    int spawnTargetCount_;
    int selectedSpawn_;
    void AddSpawnTarget(const char *name, const char *path, unsigned int level);
    // Entity::GetCoords: WorldCoords __thiscall () - origin-style triple
    static ThisFunc<void, void*, void*> fnGetCoords_;
    // WorldVec3::TranslateInRegion: void __thiscall (WorldVec3*, Vec3& delta)
    static ThisFunc<void, void*, void*> fnTranslateInRegion_;
};
