#include <windows.h>
#include <string>
#include <cassert>

#include "DetourCommon.h"
#include "DetourUtil.h"
#include "Logger.h"

//=============================================================================
//=============================================================================
DetourFnData datGenerateUISkillInfo = { "game.dll", NULL, NULL, SYM_GAMEENGINE_GENERATEUISKILLINFO };
DetourFnData datGetObjectId = { "engine.dll", NULL, NULL, SYM_OBJECT_GETOBJECTID };
DetourFnData datGetObjectName = { "engine.dll", NULL, NULL, SYM_OBJECT_GETOBJECTNAME };
DetourFnData datCharacterGetSkillList = { "game.dll", NULL, NULL, SYM_CHAR_GETSKILLLIST };
DetourFnData datSkillMgrGetItemSkillList = { "game.dll", NULL, NULL, SYM_SKILLMGR_GETITEMSKILLLIST };
DetourFnData datCharGetSkillMgr = { "game.dll", NULL, NULL, SYM_CHAR_GETSKILLMGR };
DetourFnData datCharGetCharBio = { "game.dll", NULL, NULL,SYM_CHAR_GETCHARBIO  };
DetourFnData datCharBioGetBonusLifeAmt = { "game.dll", NULL, NULL,  SYM_CHARBIO_GETBONUSLIFEAMOUNT };
DetourFnData datCharGetCombatMgr = { "game.dll", NULL, NULL, SYM_CHAR_GETCOMBATMGR };
DetourFnData datCombatMgrGetAttackerId = { "game.dll", NULL, NULL, SYM_COMBATMGR_GETATTACKERID };
DetourFnData datCombatMgrGetCharacter = { "game.dll", NULL, NULL, SYM_COMBATMGR_GETCHARACTER };
DetourFnData datSkillTrackableTotalTime = { "game.dll", NULL, NULL, SYM_SKILLBUFFSELFDURATION_TRACKABLETOTALTIME };

DetourFnData datSkillGetCooldownCompletion = { "game.dll", NULL, NULL, SYM_SKILL_GETCOOLDOWNCOMPLETION };
DetourFnData datSkillGetCooldownRemaining = { "game.dll", NULL, NULL, SYM_SKILL_GETCOOLDOWNREMAINING };
DetourFnData datSkillGetCooldownTime = { "game.dll", NULL, NULL, SYM_SKILL_GETCOOLDOWNTIME };
DetourFnData datSkillGetCooldownTotal = { "game.dll", NULL, NULL, SYM_SKILL_GETCOOLDOWNTOTAL };
DetourFnData datGetGameTime = { "engine.dll", NULL, NULL, SYM_GETGAMETIME };
DetourFnData datCharGetPortraitName = { "game.dll", NULL, NULL, SYM_CHAR_GETPORTRAITNAME };
DetourFnData datActorGetDescriptionTag = { "engine.dll", NULL, NULL, SYM_ACTOR_GETDESCRIPTIONTAG_X64 };
DetourFnData datLocMgrInstance = { "engine.dll", NULL, NULL, SYM_LOCMGR_INSTANCE_X64 };
DetourFnData datLocMgrLocalize = { "engine.dll", NULL, NULL, SYM_LOCMGR_LOCALIZE_X64 };

//=============================================================================
// SEH helpers: POD-only, no C++ objects -> __try legal (C2712).
// All game.dll/engine.dll calls go through these; methods below only
// validate + interpret results, so callers with maps/strings need no __try.
//=============================================================================
namespace
{
typedef unsigned int (__thiscall *RawObjIdFn)(void*);
typedef const char* (__thiscall *RawObjNameFn)(void*);
typedef void (__cdecl *RawSkillInfoFn)(void*, unsigned int&);
typedef int (__thiscall *RawCooldownFn)(void*);
typedef void* (__thiscall *RawMgrFn)(void*);

unsigned int SEH_CallObjId(RawObjIdFn fn, void* obj)
{
    if (!fn || !obj) return 0;
    __try { return fn(obj); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

const char* SEH_CallObjName(RawObjNameFn fn, void* obj)
{
    if (!fn || !obj) return 0;
    __try { return fn(obj); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

bool SEH_CallSkillInfo(RawSkillInfoFn fn, void* skill, unsigned int& out)
{
    if (!fn || !skill) return false;
    __try { fn(skill, out); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

int SEH_CallCooldown(RawCooldownFn fn, void* skill)
{
    if (!fn || !skill) return 0;
    __try { return fn(skill); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

void* SEH_CallMgr(RawMgrFn fn, void* obj)
{
    if (!fn || !obj) return 0;
    __try { return fn(obj); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
} // namespace

//=============================================================================
DetourCommon::DetourCommon()
{
  initialized_ = false;
}

bool DetourCommon::SetupDetour()
{
    int status = 0;

    status = HookDetour(datGetObjectId);
    fnGetObjectId_.SetFn(datGetObjectId.realFn_);


    status += HookDetour(datGetObjectName);
    fnObjectGetObjectName_.SetFn(datGetObjectName.realFn_);

    status += HookDetour(datCharacterGetSkillList);
	fnCharacterGetSkillList_.SetFn(datCharacterGetSkillList.realFn_);

    status += HookDetour(datSkillMgrGetItemSkillList);
    fnSkillManagerGetItemSkillList_.SetFn(datSkillMgrGetItemSkillList.realFn_);

    status += HookDetour(datCharGetSkillMgr);
	fnCharacterGetSkillManager_.SetFn(datCharGetSkillMgr.realFn_);

    status += HookDetour(datCharGetCharBio);
	fnCharacterGetCharacterBio_.SetFn(datCharGetCharBio.realFn_);

    status += HookDetour(datCharBioGetBonusLifeAmt);
	fnCharacterBioGetBonusLifeAmount_.SetFn(datCharBioGetBonusLifeAmt.realFn_);

    status += HookDetour(datCharGetCombatMgr);
    fnCharacterGetCombatManager_.SetFn(datCharGetCombatMgr.realFn_);

    status += HookDetour(datCombatMgrGetAttackerId);
    fnCombatMgrGetAttackerId_.SetFn(datCombatMgrGetAttackerId.realFn_);

    status += HookDetour(datCombatMgrGetCharacter);
    fnCombatManagerGetCharacter_.SetFn(datCombatMgrGetCharacter.realFn_);

    status += HookDetour(datSkillTrackableTotalTime);
    fnSkillTrackableTotalTime_.SetFn(datSkillTrackableTotalTime.realFn_);

    status += HookDetour(datSkillGetCooldownCompletion);
    fnSkillGetCooldownCompletion_.SetFn(datSkillGetCooldownCompletion.realFn_);

    status += HookDetour(datSkillGetCooldownRemaining);
	fnSkillGetCooldownRemaining_.SetFn(datSkillGetCooldownRemaining.realFn_);

    status += HookDetour(datSkillGetCooldownTime);
    fnSkillGetCooldownTime_.SetFn(datSkillGetCooldownTime.realFn_);

    status += HookDetour(datSkillGetCooldownTotal);
    fnSkillGetCooldownTotal_.SetFn(datSkillGetCooldownTotal.realFn_);

	status += HookDetour(datCharGetPortraitName);
	fnCharGetPortraitName_.SetFn(datCharGetPortraitName.realFn_);

	status += HookDetour(datGenerateUISkillInfo);
	fnGenerateUISkillInfo_ = (FnGenerateUISkillInfo)datGenerateUISkillInfo.realFn_;

	status += HookDetour(datGetGameTime);
    fnGetGameTime_ = (FnGetGameTime)datGetGameTime.realFn_;

    // Display-name chain (call-only, x64 first then x86). Not folded into
    // status: silently unavailable on unmatched versions.
    HookDetour(datActorGetDescriptionTag);
    if (datActorGetDescriptionTag.realFn_ == NULL)
    {
        datActorGetDescriptionTag.mangleName_ = SYM_ACTOR_GETDESCRIPTIONTAG_X86;
        HookDetour(datActorGetDescriptionTag);
    }
    fnActorGetDescriptionTag_.SetFn(datActorGetDescriptionTag.realFn_);
    LOGF("DetourCommon: Actor::GetDescriptionTag %s\n",
         datActorGetDescriptionTag.realFn_ ? "OK" : "MISSING (names fallback to record)");

    HookDetour(datLocMgrInstance);
    if (datLocMgrInstance.realFn_ == NULL)
    {
        datLocMgrInstance.mangleName_ = SYM_LOCMGR_INSTANCE_X86;
        HookDetour(datLocMgrInstance);
    }
    fnLocMgrInstance_.SetFn(datLocMgrInstance.realFn_);

    HookDetour(datLocMgrLocalize);
    if (datLocMgrLocalize.realFn_ == NULL)
    {
        datLocMgrLocalize.mangleName_ = SYM_LOCMGR_LOCALIZE_X86;
        HookDetour(datLocMgrLocalize);
    }
    fnLocMgrLocalize_.SetFn(datLocMgrLocalize.realFn_);
    LOGF("DetourCommon: LocalizationManager %s\n",
         (datLocMgrInstance.realFn_ && datLocMgrLocalize.realFn_) ? "OK" : "MISSING");

    if (status != 0)
    {
        OutputDebugStringA("DetourCommon::SetupDetour - WARNING: some functions not found (non-fatal)\n");
        // SetError("Error in DetourCommon::SetupDetour()");
    }

    return status == 0;
}

void DetourCommon::Update(void *player, int idx)
{
}

unsigned int DetourCommon::GetObjectId(void* obj) const
{
    if (!obj || !DetourUtil::MemValidity(obj) || !fnGetObjectId_.Fn_)
    {
        return 0;
    }
    return SEH_CallObjId((RawObjIdFn)fnGetObjectId_.Fn_, obj);
}

void DetourCommon::GetObjectName(void* obj, std::string &name)
{
    if (!obj || !DetourUtil::MemValidity(obj) || !fnObjectGetObjectName_.Fn_)
    {
        name = "*";
        return;
    }
    const char *objname = SEH_CallObjName((RawObjNameFn)fnObjectGetObjectName_.Fn_, obj);

    if (objname)
    {
        std::string strName = std::string(objname);
        std::size_t pos = strName.find_last_of("/");
        if (pos != std::string::npos)
        {
        //omit the path and the extension(.dbr): len - (pos + 1) - 4
        name = strName.substr(pos + 1, strName.length() - pos - 5);
        }
        else
        {
        name = "entity";
        }
    }
    else
    {
        name = "*";
        LOGF("  obj name not found\n");
    }
}

bool DetourCommon::GetEntityDisplayName(void* entity, char* out, int outSize) const
{
    if (out && outSize > 0)
        out[0] = '\0';
    if (!entity || !out || outSize <= 0 || !fnActorGetDescriptionTag_.Fn_ ||
        !fnLocMgrInstance_.Fn_ || !fnLocMgrLocalize_.Fn_)
        return false;

    __try
    {
        const char *tag = fnActorGetDescriptionTag_.Fn_(entity);
        if (!tag || !tag[0])
            return false;
        // Parameterized tags ("%s ...") cannot be called with zero varargs.
        if (strchr(tag, '%'))
            return false;
        void *locMgr = fnLocMgrInstance_.Fn_();
        if (!locMgr)
            return false;
        const wchar_t *w = fnLocMgrLocalize_.Fn_(locMgr, tag);
        if (!w || !w[0])
            return false;
        char *conv = DetourUtil::WStr2CharStr("%ls", (const wchar_t*)w);
        if (!conv || !conv[0])
            return false;
        strncpy_s(out, outSize, conv, _TRUNCATE);
        return out[0] != '\0';
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        if (out && outSize > 0)
            out[0] = '\0';
        return false;
    }
}


const char* DetourCommon::GetSkillName(void* skillptr)
{
    return GetSafeSkillName(skillptr);
}

bool DetourCommon::GetSkillRecord(void* skillptr, std::string &record) const
{
    record.clear();
    if (!skillptr || !DetourUtil::MemValidity(skillptr) || !fnObjectGetObjectName_.Fn_)
    {
        return false;
    }
    // skptr[1] is the record filename per decompile; use API, not raw deref
    const char *objname = SEH_CallObjName((RawObjNameFn)fnObjectGetObjectName_.Fn_, skillptr);
    if (!objname || !DetourUtil::MemValidity((void*)objname))
    {
        return false;
    }
    // Copy with length cap: record strings are short paths; avoid huge copy on corrupt ptr
    size_t len = 0;
    while (len < 512 && objname[len] != '\0')
    {
        ++len;
    }
    if (len == 0 || len >= 512)
    {
        return false;
    }
    record.assign(objname, len);
    return !record.empty();
}

const char* DetourCommon::GetSafeSkillName(void* skillptr)
{
    if (!skillptr || !DetourUtil::MemValidity(skillptr) || !fnGenerateUISkillInfo_)
    {
        return "null";
    }

    unsigned char skillbuf[STR_BUF_SIZE] = { 0 };

    if (!SEH_CallSkillInfo((RawSkillInfoFn)fnGenerateUISkillInfo_, skillptr, (unsigned int&)skillbuf))
    {
        return "null";
    }

    // skillbuf[0] holds pointer to wide text; validate before touching.
    unsigned int ptrVal = ((unsigned int*)&skillbuf)[0];
    if (ptrVal < 0x10000 || !DetourUtil::MemValidity((void*)ptrVal))
    {
        return "null";
    }
    //what's returned are double wide chars, convert this to single wide char
    const unsigned short *uname = (const unsigned short*)ptrVal;
    if (!DetourUtil::MemValidity((void*)uname) || !DetourUtil::MemValidity((void*)(uname + 2)))
    {
        return "null";
    }
    uname += 0x2;

    unsigned int* uptr = (unsigned int*)uname;
    if (!DetourUtil::MemValidity(uptr))
    {
        return "null";
    }
    unsigned int memval = (unsigned int)uptr[0];
    bool isWideChar = (memval & 0xff000000) != 0;

    if (isWideChar)
    {
        // wstring lives in game heap: validate begin/end before formatting.
        const wchar_t* wbegin = (const wchar_t*)uname;
        if (!DetourUtil::MemValidity((void*)wbegin))
        {
            return "null";
        }
        char *converted = DetourUtil::WStr2CharStr("%ls", wbegin);
        if (!converted)
        {
            return "null";
        }
        sprintf_s(wcharbuff, STR_BUF_SIZE - 1, "%s", converted);
    }
    else
    {
        //copy unsigned as char
        for (int i = 0; i < STR_BUF_SIZE; ++i)
        {
            if (!DetourUtil::MemValidity((void*)&uname[i]))
            {
                wcharbuff[i] = '\0';
                break;
            }
            const unsigned short v = uname[i];
            char c = (char)v;
            wcharbuff[i] = c;
            if (v == 0)
            break;
        }
        wcharbuff[STR_BUF_SIZE - 1] = '\0';
    }

    //strip out '(' 
    for (unsigned int i = 0; i < STR_BUF_SIZE; ++i)
    {
        if (wcharbuff[i] == '(')
        {
            if (i > 0 && wcharbuff[i - 1] == ' ')
            {
                wcharbuff[i - 1] = '\0';
            }
            else
            {
                wcharbuff[i] = '\0';
            }
            break;
        }
        if (wcharbuff[i] == '\0')
        {
            break;
        }
    }

    return wcharbuff;
}

std::vector<unsigned int*>& DetourCommon::CharGetSkillList(void *charPtr) const
{
    return fnCharacterGetSkillList_.Fn_(charPtr);
}

std::vector<unsigned int*>& DetourCommon::SkillMgrGetItemSkillList(void *This) const
{
    return fnSkillManagerGetItemSkillList_.Fn_(This);
}

unsigned int& DetourCommon::CharGetSkillMgr(void *charPtr) const
{
    return fnCharacterGetSkillManager_.Fn_(charPtr);
}

unsigned int& DetourCommon::CharGetCharacterBio(void* charPtr) const
{
    return fnCharacterGetCharacterBio_.Fn_(charPtr);

}

float DetourCommon::CharBioGetBonusLifeAmount(void *charBio, unsigned int& bonusRef) const
{
    return fnCharacterBioGetBonusLifeAmount_.Fn_(charBio, bonusRef);
}

void* DetourCommon::CharacterGetCombatManager(void* charPtr) const
{
    if (!charPtr || !DetourUtil::MemValidity(charPtr)) return 0;
    return SEH_CallMgr((RawMgrFn)fnCharacterGetCombatManager_.Fn_, charPtr);
}
unsigned int DetourCommon::CombatMgrGetAttackerId(void* This) const
{
    if (!This || !DetourUtil::MemValidity(This)) return 0;
    __try { return fnCombatMgrGetAttackerId_.Fn_(This); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

void* DetourCommon::CombatManagerGetCharacter(void* This) const
{
    if (!This || !DetourUtil::MemValidity(This)) return 0;
    return SEH_CallMgr((RawMgrFn)fnCombatManagerGetCharacter_.Fn_, This);
}

int DetourCommon::SkillTrackableTotalTime(void* This, unsigned int a) const
{
    if (!This || !DetourUtil::MemValidity(This)) return 0;
    __try { return fnSkillTrackableTotalTime_.Fn_(This, a); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}


float DetourCommon::SkillGetCooldownCompletion(void* This) const
{
    if (!This || !DetourUtil::MemValidity(This)) return 0.0f;
    __try { return fnSkillGetCooldownCompletion_.Fn_(This); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0.0f; }
}

int DetourCommon::SkillGetCooldownRemaining(void* This) const
{
    if (!This || !DetourUtil::MemValidity(This)) return 0;
    return SEH_CallCooldown((RawCooldownFn)fnSkillGetCooldownRemaining_.Fn_, This);
}

float DetourCommon::SkillGetCooldownTime(void* This, bool flag) const
{
    if (!This || !DetourUtil::MemValidity(This)) return 0.0f;
    __try { return fnSkillGetCooldownTime_.Fn_(This, flag); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0.0f; }
}

int DetourCommon::SkillGetCooldownTotal(void* This) const
{
    if (!This || !DetourUtil::MemValidity(This)) return 0;
    return SEH_CallCooldown((RawCooldownFn)fnSkillGetCooldownTotal_.Fn_, This);
}

int DetourCommon::GetGameTime() const
{
    return fnGetGameTime_();
}

unsigned int& DetourCommon::CharGetPortraitName(void* charPtr) const
{
    return fnCharGetPortraitName_.Fn_(charPtr);
}

