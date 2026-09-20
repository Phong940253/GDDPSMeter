#pragma once
#include "BaseRender.h"

class DetourTeleport;

//=============================================================================
// TeleportUI - ImGui window showing teleport history list
// Allows player to re-teleport to any previously visited location.
//=============================================================================
class TeleportUI : public BaseRender
{
public:
    TeleportUI();

    virtual void ShowWin(bool &showWindow);
    virtual void Draw(const char* title, bool* p_open = NULL);

    void SetTeleportDetour(DetourTeleport *teleport) { pTeleport_ = teleport; }

private:
    DetourTeleport *pTeleport_;
    int selectedIndex_;
    int spawnCat_;
    char inputName_[128];
};
