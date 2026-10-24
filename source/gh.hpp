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
    std::string published;   // ISO 8601
    std::string body;        // markdown
    std::vector<Asset> assets;
};

bool latest(const std::string& owner_repo, Release& out, std::string& err);
bool releases(const std::string& owner_repo, std::vector<Release>& out, std::string& err);

// "2026-10-16T00:05:51Z" -> "Oct 16, 2026"
std::string pretty_date(const std::string& iso);

}
