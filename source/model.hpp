#pragma once
#include <string>
#include <vector>

struct Candidate {
    std::string version;
    std::string asset_url;
    std::string asset_name;
    std::string date;       // "Oct 16, 2026"
    std::string notes;      // release body (markdown)
    long long size = 0;
};

enum class Kind { Atmosphere, Hekate, Firmware };
enum class Phase { Checking, Idle, Downloading, Installing, RebootPending, Done, Error };

struct Card {
    const char* title;
    const char* repo;
    const char* saved_key;
    const char* asset_prefix;
    const char* asset_contains;
    Kind kind;

    std::string installed;
    std::vector<Candidate> cands;   // newest first
    Phase phase = Phase::Checking;
    std::string err;
    double progress = 0;
    long long dl_now = 0, dl_total = 0;
    int target = 0;                 // candidate index to install
    bool need_refresh = true;
    bool want_update = false;
};

enum class Status {
    Checking, UpToDate, UpdateAvailable, NotInstalled, NoRelease,
    Downloading, Installing, RebootPending, Installed, Downloaded, Error
};

inline Status status_of(const Card& c) {
    switch (c.phase) {
        case Phase::Checking:      return Status::Checking;
        case Phase::Downloading:   return Status::Downloading;
        case Phase::Installing:    return Status::Installing;
        case Phase::RebootPending: return Status::RebootPending;
        case Phase::Error:         return Status::Error;
        case Phase::Done:          return c.kind == Kind::Firmware ? Status::Downloaded : Status::Installed;
        case Phase::Idle:          break;
    }
    if (c.cands.empty())                      return Status::NoRelease;
    if (c.installed.empty())                  return Status::NotInstalled;
    if (c.installed == c.cands[0].version)    return Status::UpToDate;
    return Status::UpdateAvailable;
}

enum class Zone { Sidebar, List };
enum class Modal { None, Confirm, Reboot };

struct View {
    std::vector<Card> cards;
    int sel = 0;
    Zone zone = Zone::Sidebar;
    int fw_sel = 0;
    Modal modal = Modal::None;
    bool online = true;
};
