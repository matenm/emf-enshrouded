#pragma once
// nxm:// links: what the "Mod Manager Download" button on Nexus Mods' website opens, for example
//   nxm://enshrouded/mods/170/files/1197?key=XXXX&expires=1792790875&user_id=12345
// The key and expiry let a free account ask the API for that one file's download link (Premium members need neither).
// Parsed strictly: only Enshrouded's game domain, positive numbers, a key of the characters keys have. Messages never
// repeat the link's query, so the one-time key stays out of the page and the logs. Pure, unit-tested.
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "nexus/api.h"

namespace emf::nexus {

struct NxmLink {
    std::string game;     // the game domain, lower case ("enshrouded")
    int64_t mod_id = 0;
    int64_t file_id = 0;
    std::string key;      // "" when the link has none
    int64_t expires = 0;  // Unix seconds; 0 when the link has none
    int64_t user_id = 0;  // the Nexus account the link was made for; 0 when the link has none
};

struct NxmParse {
    std::optional<NxmLink> link;
    std::string error;        // why not, for the player
    bool other_game = false;  // a well-formed link for another game
};

// The link as the browser hands it over (spaces around it are ignored). Only links for `game` are accepted.
NxmParse parse_nxm(std::string_view url, std::string_view game = kGameDomain);

// The link's expiry has passed (a link without one never expires here; Nexus decides).
bool nxm_expired(const NxmLink& link, int64_t now_unix);

// https://www.nexusmods.com/enshrouded/mods/<mod>?tab=files&file_id=<file>: where a free account downloads a file.
std::string nexus_file_page(int64_t mod_id, int64_t file_id);

}  // namespace emf::nexus
