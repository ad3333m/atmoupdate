#include "gh.hpp"
#include "http.hpp"
#include <cstring>

namespace {

// Minimal JSON-ish extractors: GitHub release JSON is predictable.
std::string find_string(const std::string& s, size_t& pos, const char* key) {
    std::string pat = "\""; pat += key; pat += "\"";
    size_t k = s.find(pat, pos);
    if (k == std::string::npos) return {};
    size_t colon = s.find(':', k);
    if (colon == std::string::npos) return {};
    size_t q1 = s.find('"', colon + 1);
    if (q1 == std::string::npos) return {};
    size_t q2 = q1 + 1;
    while (q2 < s.size()) {
        if (s[q2] == '\\' && q2 + 1 < s.size()) { q2 += 2; continue; }
        if (s[q2] == '"') break;
        q2++;
    }
    pos = q2 + 1;
    std::string raw = s.substr(q1 + 1, q2 - q1 - 1);
    // unescape minimal
    std::string out; out.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); i++) {
        if (raw[i] == '\\' && i + 1 < raw.size()) {
            char c = raw[i+1];
            if (c == 'n') out += '\n';
            else if (c == 't') out += '\t';
            else out += c;
            i++;
        } else out += raw[i];
    }
    return out;
}

long long find_number(const std::string& s, size_t& pos, const char* key) {
    std::string pat = "\""; pat += key; pat += "\"";
    size_t k = s.find(pat, pos);
    if (k == std::string::npos) return 0;
    size_t colon = s.find(':', k);
    if (colon == std::string::npos) return 0;
    size_t i = colon + 1;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
    long long v = 0; bool neg = false;
    if (i < s.size() && s[i] == '-') { neg = true; i++; }
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') { v = v*10 + (s[i]-'0'); i++; }
    pos = i;
    return neg ? -v : v;
}

}

namespace gh {

// Parses one release block starting at scan_from; returns parse-end position (or npos on failure).
// Fills `out` with tag and the assets that live inside this release's JSON object.
size_t parse_release(const std::string& body, size_t scan_from, size_t scan_until, Release& out) {
    size_t p = scan_from;
    out.tag = find_string(body, p, "tag_name");
    if (out.tag.empty() || p > scan_until) return std::string::npos;

    size_t assets_at = body.find("\"assets\"", p);
    if (assets_at == std::string::npos || assets_at > scan_until) return p;
    p = assets_at;
    while (p < scan_until) {
        size_t next_name = body.find("\"name\"", p);
        size_t next_url  = body.find("\"browser_download_url\"", p);
        if (next_name == std::string::npos || next_url == std::string::npos) break;
        if (next_name >= scan_until || next_url >= scan_until) break;
        size_t at = next_name < next_url ? next_name : next_url;
        p = at;
        Asset a;
        size_t pn = p; a.name = find_string(body, pn, "name");
        size_t pu = p; a.url  = find_string(body, pu, "browser_download_url");
        size_t ps = p; a.size = find_number(body, ps, "size");
        if (a.name.empty() || a.url.empty()) break;
        out.assets.push_back(std::move(a));
        p = pu;
    }
    return p;
}

bool latest(const std::string& owner_repo, Release& out, std::string& err) {
    std::string url = "https://api.github.com/repos/" + owner_repo + "/releases/latest";
    std::string body;
    if (!http::get_string(url, body, err)) return false;
    if (parse_release(body, 0, body.size(), out) == std::string::npos) {
        err = "no tag_name"; return false;
    }
    return true;
}

bool releases(const std::string& owner_repo, std::vector<Release>& out, std::string& err) {
    std::string url = "https://api.github.com/repos/" + owner_repo + "/releases?per_page=30";
    std::string body;
    if (!http::get_string(url, body, err)) return false;
    size_t p = 0;
    while (p < body.size()) {
        size_t next = body.find("\"tag_name\"", p);
        if (next == std::string::npos) break;
        size_t end_next = body.find("\"tag_name\"", next + 1);
        size_t stop = (end_next == std::string::npos) ? body.size() : end_next;
        Release r;
        size_t new_p = parse_release(body, next, stop, r);
        if (new_p == std::string::npos) break;
        out.push_back(std::move(r));
        p = stop;
    }
    if (out.empty()) { err = "no releases"; return false; }
    return true;
}

}
