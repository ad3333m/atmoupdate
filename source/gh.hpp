#pragma once
#include <string>
#include <vector>

namespace gh {

struct Asset {
    std::string name;
    std::string url;
    long long size = 0;
};

struct Release {
    std::string tag;
    std::vector<Asset> assets;
};

// Fetches latest release metadata for "owner/repo".
bool latest(const std::string& owner_repo, Release& out, std::string& err);

}
