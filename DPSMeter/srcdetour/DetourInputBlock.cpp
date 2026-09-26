#include <windows.h>
#include <string.h>
#include <intrin.h>
#include <detours.h>
#include "DetourInputBlock.h"
#include "Logger.h"

//=============================================================================
// Minimal COM/DInput types (no dinput.h/dxguid.lib dependency).
// IDirectInputDevice8 vtable: GetDeviceState=[9], GetDeviceData=[10],
// GetDeviceInfo=[15].
//=============================================================================
typedef HRESULT (STDMETHODCALLTYPE *FnGetDeviceState)(
    void* This, DWORD cbData, void* lpvData);
typedef HRESULT (STDMETHODCALLTYPE *FnGetDeviceData)(
    void* This, DWORD cbObjectData, void* rgdod, DWORD* pdwInOut, DWORD dwFlags);
typedef HRESULT (STDMETHODCALLTYPE *FnGetDeviceInfo)(
    void* This, void* pdidi);
typedef HRESULT (WINAPI *FnDirectInput8Create)(
    HINSTANCE, DWORD, REFGUID, LPVOID*, LPUNKNOWN);

// {6F1D2B60-D5A0-11CF-BFC7-444553540000} == GUID_SysKeyboard
static const GUID kGuidSysKeyboard = {
    0x6F1D2B60, 0xD5A0, 0x11CF,
    { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 }
};
// IID_IDirectInput8A
static const GUID kIidDirectInput8A = {
    0xBF798030, 0x483A, 0x4DA0,
    { 0xB6, 0x71, 0xC5, 0x6B, 0xEC, 0xB4, 0xAC, 0xEE }
};

// DIDEVICEINSTANCEA prefix (only fields we read)
struct DeviceInfoPrefix
{
    DWORD dwSize;
    GUID guidInstance;
    GUID guidProduct;
    DWORD dwDevType;
};
#define DIDEVTYPE_KEYBOARD_PREFIX 3
#define DIDEVTYPE_MOUSE_PREFIX 2

//=============================================================================
// Static members
//=============================================================================
DetourInputBlock* DetourInputBlock::sSelf_ = NULL;
std::atomic<bool> DetourInputBlock::s_blockKeys_(false);
volatile LONG DetourInputBlock::s_attachState_ = 0;

static FnGetDeviceState s_realGetDeviceState = NULL;
static FnGetDeviceData s_realGetDeviceData = NULL;

// --- user32 polling hooks: the game reads keys itself (message swallowing
// does not stop polling), so report "key up" to everyone except our module.
typedef SHORT (WINAPI *FnGetAsyncKeyState)(int);
typedef BOOL (WINAPI *FnGetKeyboardState)(PBYTE);
typedef SHORT (WINAPI *FnGetKeyState)(int);
static FnGetAsyncKeyState s_realGetAsyncKeyState = NULL;
static FnGetKeyboardState s_realGetKeyboardState = NULL;
static FnGetKeyState s_realGetKeyState = NULL;
static uintptr_t s_ownBase = 0;
static uintptr_t s_ownEnd = 0;

static bool IsOwnCaller()
{
    void* ret = _ReturnAddress();
    uintptr_t a = (uintptr_t)ret;
    return (a >= s_ownBase && a < s_ownEnd);
}

// Throttled caller-module spy: proves which input API the game uses.
// Full per-(api,module) volumes go through CountBlockedCall below.
static void SpyCaller(const char* api)
{
    static int spyCount = 0;
    if (spyCount >= 6)
        return;
    spyCount++;
    void* ret = _ReturnAddress();
    HMODULE hMod = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)ret, &hMod);
    char name[MAX_PATH] = { 0 };
    if (hMod)
        GetModuleFileNameA(hMod, name, sizeof(name) - 1);
    const char* base = strrchr(name, '\\');
    base = base ? base + 1 : name;
    LOGF("InputSpy: %s called by %s\n", api, base[0] ? base : "?");
}

// Volume counters per (api, caller module base): dumped periodically while
// typing so one run maps every input path the game actually uses.
enum { API_ASYNC = 0, API_KBSTATE, API_KEYSTATE, API_RAW };
struct ApiCount { int api; uintptr_t modBase; volatile LONG n; };
static ApiCount s_apiCounts[32];
static int s_apiCountN = 0;
static DWORD s_apiDumpTick = 0;

static void CountBlockedCall(int api)
{
    void* ret = _ReturnAddress();
    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t base = 0;
    if (VirtualQuery(ret, &mbi, sizeof(mbi)) == sizeof(mbi))
        base = (uintptr_t)mbi.AllocationBase;
    for (int i = 0; i < s_apiCountN; ++i)
    {
        if (s_apiCounts[i].api == api && s_apiCounts[i].modBase == base)
        {
            InterlockedIncrement(&s_apiCounts[i].n);
            return;
        }
    }
    if (s_apiCountN < 32)
    {
        s_apiCounts[s_apiCountN].api = api;
        s_apiCounts[s_apiCountN].modBase = base;
        s_apiCounts[s_apiCountN].n = 1;
        s_apiCountN++;
    }
}

static void DumpBlockedCalls()
{
    DWORD now = timeGetTime();
    if (now - s_apiDumpTick < 10000)
        return;
    s_apiDumpTick = now;
    for (int i = 0; i < s_apiCountN; ++i)
    {
        LONG n = s_apiCounts[i].n;
        if (n <= 0)
            continue;
        char name[MAX_PATH] = { 0 };
        GetModuleFileNameA((HMODULE)s_apiCounts[i].modBase, name, sizeof(name) - 1);
        const char* base = strrchr(name, '\\');
        base = base ? base + 1 : name;
        static const char* apiNames[] = { "GetAsyncKeyState", "GetKeyboardState", "GetKeyState", "GetRawInputData" };
        LOGF("InputVolume: %s by %s x%ld\n", apiNames[s_apiCounts[i].api],
             base[0] ? base : "?", n);
    }
}

// Per-instance keyboard cache (avoids a COM call per poll).
struct KbCacheEntry { void* dev; bool isKb; };
static KbCacheEntry s_kbCache[16];
static int s_kbCacheCount = 0;

static bool IsKeyboardDevice(void* dev)
{
    for (int i = 0; i < s_kbCacheCount; ++i)
    {
        if (s_kbCache[i].dev == dev)
            return s_kbCache[i].isKb;
    }
    bool isKb = false;
    __try
    {
        // GetDeviceInfo is vtable[15]; works without Acquire.
        typedef HRESULT (STDMETHODCALLTYPE *FnGetInfo)(void*, void*);
        void** vt = *(void***)dev;
        FnGetInfo getInfo = (FnGetInfo)vt[15];
        char buf[1100];
        memset(buf, 0, sizeof(buf));
        ((DeviceInfoPrefix*)buf)->dwSize = 576; // sizeof(DIDEVICEINSTANCEA)
        if (getInfo(dev, buf) == 0)
            isKb = ((((DeviceInfoPrefix*)buf)->dwDevType & 0xFF) == DIDEVTYPE_KEYBOARD_PREFIX);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        isKb = false;
    }
    if (s_kbCacheCount < 16)
    {
        s_kbCache[s_kbCacheCount].dev = dev;
        s_kbCache[s_kbCacheCount].isKb = isKb;
        s_kbCacheCount++;
    }
    return isKb;
}

//=============================================================================
// Global detours (shared implementation => covers devices created before us).
//=============================================================================
static HRESULT STDMETHODCALLTYPE HookGetDeviceState(
    void* This, DWORD cbData, void* lpvData)
{
    HRESULT hr = E_FAIL;
    if (s_realGetDeviceState)
        hr = s_realGetDeviceState(This, cbData, lpvData);
    // 256-byte state buffer == keyboard (mouse=16/40). Device check cached.
    if (DetourInputBlock::WantsBlock() && cbData == 256 && lpvData &&
        IsKeyboardDevice(This))
    {
        memset(lpvData, 0, 256); // game sees no keys held
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE HookGetDeviceData(
    void* This, DWORD cbObjectData, void* rgdod, DWORD* pdwInOut, DWORD dwFlags)
{
    if (DetourInputBlock::WantsBlock() && IsKeyboardDevice(This))
    {
        if (pdwInOut)
            *pdwInOut = 0; // swallow buffered key events
        return 0; // S_OK
    }
    if (s_realGetDeviceData)
        return s_realGetDeviceData(This, cbObjectData, rgdod, pdwInOut, dwFlags);
    return E_FAIL;
}

// user32 polling detours: active only while typing in an overlay field.
static SHORT WINAPI HookGetAsyncKeyState(int vKey)
{
    if (DetourInputBlock::WantsBlock() && !IsOwnCaller())
    {
        SpyCaller("GetAsyncKeyState");
        CountBlockedCall(API_ASYNC);
        return 0;
    }
    return s_realGetAsyncKeyState ? s_realGetAsyncKeyState(vKey) : 0;
}

static BOOL WINAPI HookGetKeyboardState(PBYTE lpKeyState)
{
    if (DetourInputBlock::WantsBlock() && !IsOwnCaller())
    {
        SpyCaller("GetKeyboardState");
        CountBlockedCall(API_KBSTATE);
        if (lpKeyState)
            memset(lpKeyState, 0, 256);
        return TRUE;
    }
    return s_realGetKeyboardState ? s_realGetKeyboardState(lpKeyState) : FALSE;
}

static SHORT WINAPI HookGetKeyState(int nVirtKey)
{
    if (DetourInputBlock::WantsBlock() && !IsOwnCaller())
    {
        SpyCaller("GetKeyState");
        CountBlockedCall(API_KEYSTATE);
        return 0;
    }
    return s_realGetKeyState ? s_realGetKeyState(nVirtKey) : 0;
}

// Raw Input spy+block: RID_INPUT keyboard reads while typing.
typedef UINT (WINAPI *FnGetRawInputData)(
    HRAWINPUT, UINT, LPVOID, PUINT, UINT);
static FnGetRawInputData s_realGetRawInputData = NULL;
static UINT WINAPI HookGetRawInputData(
    HRAWINPUT hRawInput, UINT uiCommand, LPVOID pData, PUINT pcbSize, UINT cbSizeHeader)
{
    if (DetourInputBlock::WantsBlock() && !IsOwnCaller() &&
        uiCommand == 0x10000003 /*RID_INPUT*/ && pData && pcbSize)
    {
        DWORD type = *(DWORD*)pData; // RAWINPUT.header.dwType
        if (type == 1 /*RIM_TYPEKEYBOARD*/)
        {
            SpyCaller("GetRawInputData(kb)");
            CountBlockedCall(API_RAW);
            return 0; // swallow: report no data
        }
    }
    return s_realGetRawInputData ?
        s_realGetRawInputData(hRawInput, uiCommand, pData, pcbSize, cbSizeHeader) : 0;
}

//=============================================================================
// DetourInputBlock
//=============================================================================
DetourInputBlock::DetourInputBlock()
{
    sSelf_ = this;
}

void DetourInputBlock::SetBlockGameKeyboard(bool block)
{
    s_blockKeys_.store(block);
}

bool DetourInputBlock::WantsBlock()
{
    return s_blockKeys_.load();
}

void DetourInputBlock::TryAttach()
{
    // SetupDetour (attach thread) and Update (game thread) can race when
    // dinput8 loads late - attach exactly once.
    if (InterlockedCompareExchange(&s_attachState_, 1, 0) != 0)
        return;

    // Own module range: our poll-feed and backend must see real key state.
    if (s_ownBase == 0)
    {
        HMODULE hOwn = NULL;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)&HookGetAsyncKeyState, &hOwn);
        if (hOwn)
        {
            PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)hOwn;
            PIMAGE_NT_HEADERS nt = (PIMAGE_NT_HEADERS)((BYTE*)hOwn + dos->e_lfanew);
            if (dos->e_magic == IMAGE_DOS_SIGNATURE &&
                nt->Signature == IMAGE_NT_SIGNATURE)
            {
                s_ownBase = (uintptr_t)hOwn;
                s_ownEnd = s_ownBase + nt->OptionalHeader.SizeOfImage;
            }
        }
        LOGF("DetourInputBlock: own range [%p,%p) selftest=%d\n",
             (void*)s_ownBase, (void*)s_ownEnd, IsOwnCaller() ? 1 : 0);
    }

    // user32 polling hooks: always present, no late-load issue.
    HMODULE u32 = GetModuleHandleA("user32.dll");
    if (u32 && !s_realGetAsyncKeyState)
    {
        s_realGetAsyncKeyState =
            (FnGetAsyncKeyState)GetProcAddress(u32, "GetAsyncKeyState");
        s_realGetKeyboardState =
            (FnGetKeyboardState)GetProcAddress(u32, "GetKeyboardState");
        s_realGetKeyState =
            (FnGetKeyState)GetProcAddress(u32, "GetKeyState");
        s_realGetRawInputData =
            (FnGetRawInputData)GetProcAddress(u32, "GetRawInputData");
        if (s_realGetAsyncKeyState && s_realGetKeyboardState &&
            s_realGetKeyState && s_realGetRawInputData)
        {
            DetourTransactionBegin();
            DetourUpdateThread(GetCurrentThread());
            DetourAttach((PVOID*)&s_realGetAsyncKeyState, HookGetAsyncKeyState);
            DetourAttach((PVOID*)&s_realGetKeyboardState, HookGetKeyboardState);
            DetourAttach((PVOID*)&s_realGetKeyState, HookGetKeyState);
            DetourAttach((PVOID*)&s_realGetRawInputData, HookGetRawInputData);
            int uerr = DetourTransactionCommit();
            LOGF("DetourInputBlock: user32 poll hooks %s err=%d\n",
                 uerr == 0 ? "OK" : "FAILED", uerr);
        }
    }

    HMODULE d8 = GetModuleHandleA("dinput8.dll");
    if (!d8)
    {
        InterlockedExchange(&s_attachState_, 0);
        return;
    }

    FnDirectInput8Create createFn =
        (FnDirectInput8Create)GetProcAddress(d8, "DirectInput8Create");
    if (!createFn)
    {
        InterlockedExchange(&s_attachState_, 0);
        OutputDebugStringA("DetourInputBlock: DirectInput8Create export not found\n");
        return;
    }

    // Throwaway device purely to learn the shared vtable addresses.
    void* pDI = NULL;
    void* pDev = NULL;
    __try
    {
        if (createFn(GetModuleHandleA(NULL), 0x0800, kIidDirectInput8A,
                     (LPVOID*)&pDI, NULL) != 0 || !pDI)
        {
            InterlockedExchange(&s_attachState_, 0);
            return;
        }
        typedef HRESULT (STDMETHODCALLTYPE *FnCreateDev)(
            void*, REFGUID, void**, void*);
        void** diVt = *(void***)pDI;
        FnCreateDev createDev = (FnCreateDev)diVt[3];
        if (createDev(pDI, kGuidSysKeyboard, &pDev, NULL) != 0 || !pDev)
        {
            typedef HRESULT (STDMETHODCALLTYPE *FnRelease)(void*);
            ((FnRelease)(*(void***)pDI)[2])(pDI);
            InterlockedExchange(&s_attachState_, 0);
            LOGF("DetourInputBlock: own keyboard device failed\n");
            return;
        }
        void** devVt = *(void***)pDev;
        s_realGetDeviceState = (FnGetDeviceState)devVt[9];
        s_realGetDeviceData = (FnGetDeviceData)devVt[10];

        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourAttach((PVOID*)&s_realGetDeviceState, HookGetDeviceState);
        DetourAttach((PVOID*)&s_realGetDeviceData, HookGetDeviceData);
        int err = DetourTransactionCommit();

        // Release our references; the code addresses stay valid.
        typedef HRESULT (STDMETHODCALLTYPE *FnRelease)(void*);
        ((FnRelease)(*(void***)pDev)[2])(pDev);
        ((FnRelease)(*(void***)pDI)[2])(pDI);

        if (err == 0 && s_realGetDeviceState && s_realGetDeviceData)
        {
            LOGF("DetourInputBlock: keyboard GetDeviceState/Data hooked state=%p data=%p\n",
                 s_realGetDeviceState, s_realGetDeviceData);
            return; // keep s_attachState_ == 1 (done)
        }
        LOGF("DetourInputBlock: attach failed err=%d\n", err);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LOGF("DetourInputBlock: attach exception\n");
    }
    InterlockedExchange(&s_attachState_, 0);
}

bool DetourInputBlock::SetupDetour()
{
    TryAttach();
    return true; // never fatal
}

void DetourInputBlock::Update(void* player, int idx)
{
    (void)player;
    (void)idx;
    if (s_realGetDeviceState == NULL && GetModuleHandleA("dinput8.dll") != NULL)
        TryAttach();
    DumpBlockedCalls();
}
