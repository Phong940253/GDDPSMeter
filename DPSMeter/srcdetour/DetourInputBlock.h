#pragma once
#include <atomic>
#include "DetourBase.h"

//=============================================================================
// DetourInputBlock - stop the game from also handling keystrokes while the
// user types in an ImGui text field (spawn search, location name, ...).
//
// Why: key events reach ImGui through the low-level keyboard hook, but Grim
// Dawn polls the keyboard itself through DirectInput, so swallowing the
// Windows message is not enough - the character keeps moving/attacking.
//
// How (robust to late injection - the loader attaches to a running game, so
// the game's keyboard device usually already exists): create our own
// throwaway keyboard device to learn the shared IDirectInputDevice8 vtable,
// then detour GetDeviceState[9]/GetDeviceData[10] globally. While the block
// flag is set, keyboard state reads return empty. Mouse/joystick traffic is
// identified by buffer size + cached DIDEVTYPE check and passes through.
// Non-fatal if dinput8 is unused.
//=============================================================================
class DetourInputBlock : public DetourBase
{
public:
    DetourInputBlock();

    virtual bool SetupDetour();
    virtual void Update(void* player, int idx);
    virtual void SetPlayer(void* player) { (void)player; }

    // Called every overlay frame from the UI thread.
    static void SetBlockGameKeyboard(bool block);
    // Read by the hooked DirectInput methods (any game thread).
    static bool WantsBlock();

private:
    static DetourInputBlock* sSelf_;
    static std::atomic<bool> s_blockKeys_;
    static volatile LONG s_attachState_; // 0=free, 1=attach attempted
    void TryAttach();
};
