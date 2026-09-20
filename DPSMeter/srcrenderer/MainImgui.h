#pragma once
class DetourTeleport;

class ImGuiMain
{
public:
  static void ImGuiStartup();
  static void SetHwnWindow(void*);
  static void SetTeleportDetour(DetourTeleport*);
};

void HookMouse();

