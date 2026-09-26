#include <windows.h>
#include <mmsystem.h>
#include <ctype.h>
#include "MainImgui.h"
#include "TeleportUI.h"
#include "DetourTeleport.h"
#include "Logger.h"

// Case-insensitive substring match on target name (empty query matches all).
static bool SpawnNameMatch(const SpawnTarget &t, const char *query)
{
    if (!query || query[0] == '\0')
        return true;
    const char *name = t.name;
    for (const char *s = name; *s; ++s)
    {
        const char *a = s;
        const char *b = query;
        while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b))
        {
            ++a;
            ++b;
        }
        if (*b == '\0')
            return true;
    }
    return false;
}

//=============================================================================
//=============================================================================
TeleportUI::TeleportUI()
{
    pTeleport_ = NULL;
    selectedIndex_ = -1;
    spawnCat_ = 0;
    memset(inputName_, 0, sizeof(inputName_));
    memset(spawnSearch_, 0, sizeof(spawnSearch_));
    spawnSearchFocus_ = false;
    spawnSearchWasActive_ = false;
}

void TeleportUI::ShowWin(bool &showWindow)
{
    if (!showWindow)
        return;

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImVec2 work_size = viewport->WorkSize;

    ImGuiWindowFlags window_flags = ImGuiWindowFlags_AlwaysAutoResize |
                                     ImGuiWindowFlags_NoCollapse;
    if (!moveUI_)
        window_flags |= ImGuiWindowFlags_NoMove;

    ImGui::SetNextWindowPos(ImVec2(work_size.x - 20.0f, 100.0f),
                            ImGuiCond_FirstUseEver, ImVec2(1.0f, 0.0f));

    Draw("Teleport List");
}

void TeleportUI::Draw(const char* title, bool* p_open)
{
    if (!pTeleport_)
        return;

    if (!ImGui::Begin(title, p_open, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse))
    {
        ImGui::End();
        return;
    }

    // Status
    bool canTP = pTeleport_->IsTeleportAvailable();
    bool inWorld = pTeleport_->IsPlayerInWorld();
    ImGui::TextColored(canTP ? ImVec4(0.0f, 1.0f, 0.0f, 1.0f)
                             : ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
                       "Teleport: %s", canTP ? "Available" : "Unavailable");

    if (inWorld)
    {
        int px, py, pz;
        pTeleport_->GetCurrentPosition(px, py, pz);
        ImGui::TextDisabled("Rift Pos: (%d, %d, %d)", px, py, pz);
        if (pTeleport_->HasFloatPos())
        {
            float fx, fy, fz;
            pTeleport_->GetCurrentFloatPos(fx, fy, fz);
            ImGui::TextDisabled("Float Pos: (%.2f, %.2f, %.2f)", fx, fy, fz);
        }
        else
        {
            ImGui::TextDisabled("Float Pos: (capturing...)");
        }
        int off = pTeleport_->GetPositionOffset();
        if (off >= 0)
            ImGui::TextDisabled("Mem offset: player+0x%X (instant TP ready)", off);
        else
            ImGui::TextDisabled("Mem offset: scanning... (save a location to scan)");
    }

    ImGui::Separator();

    if (ImGui::BeginTabBar("TeleportTabs"))
    {
        // ══════════════════════════════════════════════════════
        // Tab 1: Saved Locations (bosses, farming spots)
        // ══════════════════════════════════════════════════════
        if (ImGui::BeginTabItem("Saved"))
        {
            int count = pTeleport_->GetSavedLocationCount();
            const SavedLocation *locs = pTeleport_->GetSavedLocations();

            // ── Add new location ──
            // Click the field first to focus it (game input is blocked while
            // typing). Empty name = auto "Spot N" so Save works regardless.
            ImGui::Text("Add Location:");
            ImGui::PushItemWidth(200);
            ImGui::InputText("##locname", inputName_, sizeof(inputName_));
            ImGui::PopItemWidth();
            ImGui::SameLine();

            if (inWorld)
            {
                if (ImGui::Button("Save Here"))
                {
                    char autoName[128];
                    const char *nameToSave = inputName_;
                    if (strlen(inputName_) == 0)
                    {
                        if (pTeleport_->HasFloatPos())
                        {
                            float fx, fy, fz;
                            pTeleport_->GetCurrentFloatPos(fx, fy, fz);
                            sprintf_s(autoName, "Spot %d (%.0f, %.0f)", count + 1, fx, fz);
                        }
                        else
                        {
                            sprintf_s(autoName, "Spot %d", count + 1);
                        }
                        nameToSave = autoName;
                    }
                    if (pTeleport_->AddSavedLocation(nameToSave))
                    {
                        memset(inputName_, 0, sizeof(inputName_));
                    }
                }
            }
            else
            {
                ImGui::TextDisabled("Save Here");
            }

            ImGui::Separator();

            // ── List of saved locations ──
            if (count == 0)
            {
                ImGui::TextDisabled("No saved locations yet.");
                ImGui::TextDisabled("Go to a boss/spot, type a name, click 'Save Here'.");
            }
            else
            {
                ImGui::Text("Saved locations (%d):", count);
                ImGui::Separator();

                ImGui::BeginChild("SavedList", ImVec2(320, 200), true);

                for (int i = 0; i < count; ++i)
                {
                    char label[160];
                    sprintf_s(label, "%s##s%d", locs[i].name, i);

                    bool selected = (selectedIndex_ == (i + 10000));
                    if (ImGui::Selectable(label, selected))
                    {
                        selectedIndex_ = i + 10000;
                    }

                    if (ImGui::IsItemHovered())
                    {
                        ImGui::BeginTooltip();
                        if (locs[i].hasInt)
                            ImGui::Text("Tile: (%d, %d, %d)%s", locs[i].x, locs[i].y, locs[i].z,
                                        locs[i].hasOffset ? " [computed]" : " [rift]");
                        else
                            ImGui::TextDisabled("Tile: unknown - rift to this map first, then re-save");
                        if (locs[i].hasFloat)
                            ImGui::Text("Rel: (%.2f, %.2f, %.2f)", locs[i].fx, locs[i].fy, locs[i].fz);
                        else
                            ImGui::TextDisabled("No rel pos - delete + re-save!");
                        ImGui::EndTooltip();
                    }
                }

                ImGui::EndChild();
                ImGui::Separator();

                if (selectedIndex_ >= 10000)
                {
                    int idx = selectedIndex_ - 10000;
                    if (idx >= 0 && idx < count)
                    {
                        ImGui::Text("Selected: %s", locs[idx].name);

                        if (canTP)
                        {
                            char tpReason[128];
                            if (pTeleport_->IsSavedTeleportReady(idx, tpReason, sizeof(tpReason)))
                            {
                                if (ImGui::Button("Teleport", ImVec2(120, 0)))
                                {
                                    LOGF("UI: TeleportToSaved clicked idx=%d\n", idx);
                                    pTeleport_->TeleportToSaved(idx);
                                }
                            }
                            else
                            {
                                ImGui::TextDisabled("Teleport: %s", tpReason);
                            }
                        }
                        else
                        {
                            ImGui::TextDisabled("Teleport not available.");
                        }

                        ImGui::SameLine();
                        if (ImGui::Button("Delete", ImVec2(120, 0)))
                        {
                            pTeleport_->RemoveSavedLocation(idx);
                            selectedIndex_ = -1;
                        }
                    }
                }
            }

            ImGui::EndTabItem();
        }

        // ══════════════════════════════════════════════════════
        // Tab 2: History (auto-recorded from teleports)
        // ══════════════════════════════════════════════════════
        if (ImGui::BeginTabItem("History"))
        {
            const TeleportDest *history = pTeleport_->GetHistory();
            int count = pTeleport_->GetHistoryCount();

            if (count == 0)
            {
                ImGui::TextDisabled("No teleport history yet.");
            }
            else
            {
                ImGui::Text("Recent teleports (%d):", count);
                ImGui::Separator();

                ImGui::BeginChild("HistoryList", ImVec2(320, 220), true);

                for (int i = count - 1; i >= 0; --i)
                {
                    const TeleportDest &dest = history[i % MAX_TELEPORT_HISTORY];

                    unsigned int elapsed = (unsigned int)timeGetTime() - dest.timestamp;
                    char timeStr[32];
                    if (elapsed < 60000)
                        sprintf_s(timeStr, "%us ago", elapsed / 1000);
                    else if (elapsed < 3600000)
                        sprintf_s(timeStr, "%um ago", elapsed / 60000);
                    else
                        sprintf_s(timeStr, "%uh ago", elapsed / 3600000);

                    char label[256];
                    sprintf_s(label, "%s##h%d", dest.areaName, i);

                    if (ImGui::Selectable(label, selectedIndex_ == i))
                    {
                        selectedIndex_ = i;
                    }

                    if (ImGui::IsItemHovered())
                    {
                        ImGui::BeginTooltip();
                        ImGui::Text("Position: (%d, %d, %d)", dest.x, dest.y, dest.z);
                        ImGui::Text("Type: %s", dest.isPersonal ? "Personal" : "Rift/Gate");
                        ImGui::Text("Time: %s", timeStr);
                        ImGui::EndTooltip();
                    }
                }

                ImGui::EndChild();
                ImGui::Separator();

                if (selectedIndex_ >= 0 && selectedIndex_ < count)
                {
                    if (canTP)
                    {
                        if (ImGui::Button("Teleport", ImVec2(-1, 0)))
                        {
                            pTeleport_->TeleportTo(selectedIndex_);
                        }
                    }
                    else
                    {
                        ImGui::TextDisabled("Teleport not available.");
                    }
                }
            }

            ImGui::EndTabItem();
        }

        // ══════════════════════════════════════════════════════
        // Tab 3: Spawn (universal boss clone)
        // Fight anything once -> roster entry -> spawn it anywhere.
        // ══════════════════════════════════════════════════════
        if (ImGui::BeginTabItem("Spawn"))
        {
            int tcount = pTeleport_->GetSpawnTargetCount();
            const SpawnTarget *tgts = pTeleport_->GetSpawnTargets();
            int sel = pTeleport_->GetSelectedSpawn();

            if (tcount == 0)
            {
                ImGui::TextDisabled("No targets yet - fight something first.");
            }
            else
            {
                // Category filter (no typing needed)
                static const char *cats[] = { "All", "BossQuest", "Nemesis", "Hero", "Bounty", "Boss", "Captured" };
                ImGui::PushItemWidth(160);
                ImGui::Combo("##spawncat", &spawnCat_, cats, 7);
                ImGui::PopItemWidth();
                ImGui::SameLine();
                int shown = 0;
                for (int i = 0; i < tcount; ++i)
                    if ((spawnCat_ == 0 || strcmp(tgts[i].cat, cats[spawnCat_]) == 0) &&
                        SpawnNameMatch(tgts[i], spawnSearch_))
                        shown++;
                char cap[64];
                sprintf_s(cap, "(%d)", shown);
                ImGui::Text("%s", cap);

                // Text search (F3 focuses without needing the mouse; Esc
                // releases. Keys are blocked from the game while typing).
                if (ImGui::IsKeyPressed(ImGuiKey_F3))
                    spawnSearchFocus_ = true;
                if (spawnSearchFocus_)
                {
                    ImGui::SetKeyboardFocusHere();
                    spawnSearchFocus_ = false;
                }
                ImGui::PushItemWidth(-1);
                ImGui::InputTextWithHint("##spawnsearch", "Search name... (F3)", spawnSearch_, sizeof(spawnSearch_));
                bool searchActive = ImGui::IsItemActive();
                if (searchActive != spawnSearchWasActive_)
                {
                    LOGF("SpawnSearch: active=%d text='%s'\n", searchActive ? 1 : 0, spawnSearch_);
                    spawnSearchWasActive_ = searchActive;
                }
                ImGui::PopItemWidth();

                ImGui::BeginChild("SpawnList", ImVec2(320, 140), true);
                for (int i = 0; i < tcount; ++i)
                {
                    if (spawnCat_ != 0 && strcmp(tgts[i].cat, cats[spawnCat_]) != 0)
                        continue;
                    if (!SpawnNameMatch(tgts[i], spawnSearch_))
                        continue;
                    char label[192];
                    if (tgts[i].level > 0)
                        sprintf_s(label, "%s (lv%u)##t%d", tgts[i].name, tgts[i].level, i);
                    else
                        sprintf_s(label, "%s##t%d", tgts[i].name, i);
                    if (ImGui::Selectable(label, sel == i))
                        pTeleport_->SetSelectedSpawn(i);
                    if (ImGui::IsItemHovered() && tgts[i].path[0])
                    {
                        ImGui::BeginTooltip();
                        ImGui::Text("%s", tgts[i].path);
                        ImGui::EndTooltip();
                    }
                }
                ImGui::EndChild();
            }

            ImGui::Separator();

            if (sel >= 0 && sel < tcount && inWorld)
            {
                if (!pTeleport_->HasEngine())
                    ImGui::TextDisabled("Linking engine... kill something first.");
                if (ImGui::Button("Spawn at me", ImVec2(-1, 0)))
                {
                    pTeleport_->SpawnRosterAtMe(sel);
                }

                // Spawn at selected saved spot
                int count = pTeleport_->GetSavedLocationCount();
                const SavedLocation *locs = pTeleport_->GetSavedLocations();
                if (selectedIndex_ >= 10000)
                {
                    int idx = selectedIndex_ - 10000;
                    if (idx >= 0 && idx < count && locs[idx].hasFloat)
                    {
                        char btn[192];
                        sprintf_s(btn, "Spawn at '%s'", locs[idx].name);
                        if (ImGui::Button(btn, ImVec2(-1, 0)))
                        {
                            pTeleport_->SpawnRosterAtSpot(sel, idx);
                        }
                    }
                }
            }
            else if (!inWorld)
            {
                ImGui::TextDisabled("Enter world first.");
            }
            else
            {
                ImGui::TextDisabled("Select a target above.");
            }

            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    ImGui::End();
}
