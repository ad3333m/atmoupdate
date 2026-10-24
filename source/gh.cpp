#include "gh.hpp"
#include "http.hpp"
#include <cstdlib>

namespace {

void put_utf8(std::string& o, unsigned cp) {
    if (cp < 0x80) o += char(cp);
    else if (cp < 0x800) { o += char(0xC0 | (cp >> 6)); o += char(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { o += char(0xE0 | (cp >> 12)); o += char(0x80 | ((cp >> 6) & 0x3F)); o += char(0x80 | (cp & 0x3F)); }
    else { o += char(0xF0 | (cp >> 18)); o += char(0x80 | ((cp >> 12) & 0x3F)); o += char(0x80 | ((cp >> 6) & 0x3F)); o += char(0x80 | (cp & 0x3F)); }
}

std::string unescape(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); i++) {
        if (raw[i] != '\\' || i + 1 >= raw.size()) { out += raw[i]; continue; }
        char c = raw[++i];
        switch (c) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': break;
            case 'b': case 'f': break;
            case 'u': {
                if (i + 4 >= raw.size()) break;
                unsigned cp = std::strtoul(raw.substr(i + 1, 4).c_str(), nullptr, 16);
                i += 4;
                if (cp >= 0xD800 && cp < 0xDC00 && i + 6 < raw.size() && raw[i + 1] == '\\' && raw[i + 2] == 'u') {
                    unsigned lo = std::strtoul(raw.substr(i + 3, 4).c_str(), nullptr, 16);
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    i += 6;
                }
                put_utf8(out, cp);
                break;
            }
            default: out += c;   // \" \\ \/
        }
    }
    return out;
}

// Finds "key": "value" in [from, until). Empty for missing keys and non-string values (null).
std::string find_string(const std::string& s, size_t from, size_t until, const char* key, size_t* end = nullptr) {
    std::string pat = std::string("\"") + key + "\"";
    size_t k = s.find(pat, from);
    if (k == std::string::npos || k >= until) return {};
    size_t i = s.find(':', k + pat.size());
    if (i == std::string::npos) return {};
    i++;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\t' || s[i] == '\r')) i++;
    if (i >= s.size() || s[i] != '"') { if (end) *end = i; return {}; }
    size_t q = i + 1;
    while (q < s.size() && s[q] != '"') q += (s[q] == '\\') ? 2 : 1;
    if (end) *end = q + 1;
    return unescape(s.substr(i + 1, q - i - 1));
}

long long find_number(const std::string& s, size_t from, size_t until, const char* key) {
    std::string pat = std::string("\"") + key + "\"";
    size_t k = s.find(pat, from);
    if (k == std::string::npos || k >= until) return 0;
    size_t i = s.find(':', k);
    if (i == std::string::npos) return 0;
    return std::strtoll(s.c_str() + i + 1, nullptr, 10);
}

// One release object spans [from, until): from its "tag_name" to the next release's.
bool parse_release(const std::string& body, size_t from, size_t until, gh::Release& out) {
    out.tag = find_string(body, from, until, "tag_name");
    if (out.tag.empty()) return false;
    out.published = find_string(body, from, until, "published_at");
    out.body = find_string(body, from, until, "body");

    size_t p = body.find("\"assets\"", from);
    if (p == std::string::npos || p >= until) return true;
    while (true) {
        size_t url_at = body.find("\"browser_download_url\"", p);
        if (url_at == std::string::npos || url_at >= until) break;
        // Asset fields run "name" ... "size" ... "browser_download_url" (last), so the
        // first name/size after the previous asset belong to this one.
        gh::Asset a;
        a.name = find_string(body, p, url_at, "name");
        a.size = find_number(body, p, url_at, "size");
        size_t after = url_at;
        a.url = find_string(body, url_at, until, "browser_download_url", &after);
        if (!a.name.empty() && !a.url.empty()) out.assets.push_back(std::move(a));
        p = after;
    }
    return true;
}

}

namespace gh {

bool latest(const std::string& owner_repo, Release& out, std::string& err) {
    std::string body;
    if (!http::get_string("https://api.github.com/repos/" + owner_repo + "/releases/latest", body, err)) return false;
    if (!parse_release(body, 0, body.size(), out)) { err = "Unexpected reply from GitHub"; return false; }
    return true;
}

bool releases(const std::string& owner_repo, std::vector<Release>& out, std::string& err) {
    std::string body;
    if (!http::get_string("https://api.github.com/repos/" + owner_repo + "/releases?per_page=30", body, err)) return false;
    size_t p = body.find("\"tag_name\"");
    while (p != std::string::npos) {
        size_t next = body.find("\"tag_name\"", p + 1);
        Release r;
        if (parse_release(body, p, next == std::string::npos ? body.size() : next, r)) out.push_back(std::move(r));
        p = next;
    }
    if (out.empty()) { err = "No releases found"; return false; }
    return true;
}

std::string pretty_date(const std::string& iso) {
    static const char* m[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
    if (iso.size() < 10) return {};
    int mo = std::atoi(iso.substr(5, 2).c_str());
    int d = std::atoi(iso.substr(8, 2).c_str());
    if (mo < 1 || mo > 12) return {};
    return std::string(m[mo - 1]) + " " + std::to_string(d) + ", " + iso.substr(0, 4);
}

}
