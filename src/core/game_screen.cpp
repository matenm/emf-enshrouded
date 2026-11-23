// The chat key gate (game_screen.h).
#include "core/game_screen.h"

#include <windows.h>

#include <cstring>
#include <iterator>

#include "common/seh.h"

namespace emf::core {

bool screen_takes_text(uint8_t screen) {
    return screen == kScreenTextChat || screen == kScreenSocial || screen == kScreenRename;
}

void GameScreen::seen(uint8_t screen, uint64_t now_ms) {
    if (screen == kScreenInGame) armed_.store(true, std::memory_order_relaxed);
    screen_.store(screen, std::memory_order_relaxed);
    seen_ms_.store(now_ms ? now_ms : 1, std::memory_order_release);
}

void GameScreen::read(const uint8_t* uipd, uint64_t now_ms) {
    const uint32_t off = offset();
    uint8_t screen = 0;
    if (!off || !uipd || !emf::seh_read(uipd + off, &screen, 1)) return;
    seen(screen, now_ms);
}

std::optional<uint8_t> GameScreen::screen(uint64_t now_ms) const {
    const uint64_t at = seen_ms_.load(std::memory_order_acquire);
    // a reading newer than the caller's clock (taken just before) is fresh
    if (!at || (now_ms > at && now_ms - at > kGameScreenStaleMs)) return std::nullopt;
    return static_cast<uint8_t>(screen_.load(std::memory_order_relaxed));
}

bool GameScreen::typing(uint64_t now_ms) const {
    if (!armed_.load(std::memory_order_relaxed)) return false;
    const auto s = screen(now_ms);
    return s && screen_takes_text(*s);
}

bool TypingKeyGate::hide(int vk, bool down, bool typing) {
    const bool marks = vk > 0 && vk < 256;
    std::atomic<uint64_t>* word = marks ? &held_[vk >> 6] : nullptr;
    const uint64_t bit = marks ? uint64_t{1} << (vk & 63) : 0;
    if (typing) {
        if (marks && down) word->fetch_or(bit, std::memory_order_relaxed);
        else if (marks) word->fetch_and(~bit, std::memory_order_relaxed);  // let go while typing: nothing held over
        return true;
    }
    if (!marks || !(word->load(std::memory_order_relaxed) & bit)) return false;
    if (down) return true;               // held since the player typed: still not a press
    word->fetch_and(~bit, std::memory_order_relaxed);  // released: it counts again
    return false;
}

bool TypingKeyGate::any() const {
    for (const auto& w : held_)
        if (w.load(std::memory_order_relaxed)) return true;
    return false;
}

uint32_t hud_screen_offset_at(const uint8_t* match) {
    // movzx r15d, byte [rdi + disp32]; cmp r15b, imm8 (TextChat): the read the chat window makes, nothing else
    static const uint8_t kMovzx[] = {0x44, 0x0F, 0xB6, 0xBF};
    static const uint8_t kCmp[] = {0x41, 0x80, 0xFF};
    uint8_t code[12];
    if (!match || !emf::seh_read(match, code, sizeof code) || code[11] != kScreenTextChat) return 0;
    if (std::memcmp(code, kMovzx, sizeof kMovzx) != 0 || std::memcmp(code + 8, kCmp, sizeof kCmp) != 0) return 0;
    uint32_t off = 0;
    std::memcpy(&off, code + 4, sizeof off);
    return off > 0 && off < 0x10000 ? off : 0;  // UiPlayData is some KB, not more
}

void TypingKeyGate::released(int vk) {
    const auto clear = [this](int k) { held_[k >> 6].fetch_and(~(uint64_t{1} << (k & 63)), std::memory_order_relaxed); };
    if (vk <= 0 || vk >= 256) return;
    clear(vk);
    // Shift, Ctrl and Alt: a mod may ask by the generic code or by either side; the window reports one side
    static const int kGroups[3][3] = {{VK_SHIFT, VK_LSHIFT, VK_RSHIFT},
                                      {VK_CONTROL, VK_LCONTROL, VK_RCONTROL},
                                      {VK_MENU, VK_LMENU, VK_RMENU}};
    for (const auto& g : kGroups)
        if (vk == g[0] || vk == g[1] || vk == g[2])
            for (int k : g) clear(k);
}

std::string game_screen_line(std::optional<uint8_t> screen, uint32_t offset, bool server, bool key_hooks, bool armed) {
    // keen::HudScreenType as the game reflects it
    static const char* const kNames[] = {
        "Invalid", "InGame", "SystemMenu", "SettingsMenu", "InGameMenu", "Map", "Journal", "Character", "PlayerCrafting",
        "Backpack", "SkillTree", "SkillTreeRespec", "FlameAltar", "AltarUpgrade", "FlameUpgrade", "AltarDeactivate",
        "AltarReskill", "AltarItemPickup", "LoreReading", "Signpost", "LogicEditor", "ReadOnlyLootChest", "StorageChest",
        "Collection", "FeedingStation", "StationBasedCrafting", "NpcDialog", "NpcCrafting", "FactoryCrafting",
        "BlueprintMultiTool", "BlueprintMultiToolMaterialCatalog", "OvergrowthMultiTool", "DecayMultiTool",
        "ScrewdriverMultiToolBuildingMode", "ScrewdriverMultiToolGraphManager", "ScrewdriverMultiToolEntityConfig",
        "TerraformingSideMenu", "NpcSummonerMenu", "UserFeedback", "TwitchDrops", "RedeemCommerceProducts",
        "CommerceProductChest", "ItemTinting", "TextChat", "Social", "ShroudForge", "CharacterCustomization", "Rename",
        "TakeScreenshot", "InfoPopup", "NewCommerceUnlock", "NoUi", "TechGallery", "YcPlayground"};
    if (server) return "screen: the dedicated server has no game screen";
    if (!offset)
        return "screen: not known: the chat key gate is off (the HudScreenRead signature is not usable on this game build)";
    if (!screen) return "screen: not known (no game world on this client right now)";
    std::string line = "screen: " + std::to_string(*screen);
    if (*screen < std::size(kNames)) line += std::string(" ") + kNames[*screen];
    if (!key_hooks) return line + ": mods' keys are not paused (EMF's key hooks are not installed: the overlay is off)";
    if (!armed) return line + ": mods' keys as usual (the chat key gate is not armed yet: no reading has said InGame)";
    return line + (screen_takes_text(*screen) ? ": mods' keys read as up (the game's text box has them)"
                                              : ": mods' keys as usual");
}

GameScreen& game_screen() {
    static GameScreen g;
    return g;
}

TypingKeyGate& typing_gate(bool async) {
    static TypingKeyGate a, s;
    return async ? a : s;
}

void typing_key_released(int vk) {
    typing_gate(true).released(vk);
    typing_gate(false).released(vk);
}

bool game_typing_now() { return game_screen().typing(GetTickCount64()); }

}  // namespace emf::core
