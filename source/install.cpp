#include "install.hpp"
#include "miniz.h"
#include <sys/stat.h>
#include <dirent.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <switch.h>

namespace {

constexpr const char* kSavedPath = "sdmc:/switch/AtmoUpdate/versions.ini";

bool ensure_dir(const std::string& dir) {
    if (dir.empty() || dir == "sdmc:" || dir == "sdmc:/") return true;
    struct stat st{};
    if (stat(dir.c_str(), &st) == 0) return (st.st_mode & S_IFDIR) != 0;
    // recurse
    size_t slash = dir.find_last_of('/');
    if (slash != std::string::npos && slash > 5) {
        if (!ensure_dir(dir.substr(0, slash))) return false;
    }
    return mkdir(dir.c_str(), 0777) == 0 || errno == EEXIST;
}

std::string sd_path(const std::string& inside_zip) {
    // Strip any leading "./" or "/" to avoid escaping sdmc:/.
    size_t i = 0;
    while (i < inside_zip.size() && (inside_zip[i] == '/' || inside_zip[i] == '.')) i++;
    return "sdmc:/" + inside_zip.substr(i);
}

}

namespace install {

bool mkdirs_for(const std::string& path) {
    size_t slash = path.find_last_of('/');
    if (slash == std::string::npos || slash <= 5) return true;
    return ensure_dir(path.substr(0, slash));
}

bool is_zip(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    unsigned char hdr[4] = {0};
    size_t n = std::fread(hdr, 1, 4, f);
    std::fclose(f);
    return n == 4 && hdr[0] == 'P' && hdr[1] == 'K' && hdr[2] == 0x03 && hdr[3] == 0x04;
}

bool extract_zip_to_root(const std::string& zip_path, const LogFn& log, std::string& err) {
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_file(&zip, zip_path.c_str(), 0)) {
        err = "bad zip";
        return false;
    }
    unsigned n = mz_zip_reader_get_num_files(&zip);
    for (unsigned i = 0; i < n; i++) {
        mz_zip_archive_file_stat st{};
        if (!mz_zip_reader_file_stat(&zip, i, &st)) continue;
        std::string name = st.m_filename;
        if (name.empty()) continue;
        std::string dest = sd_path(name);
        if (st.m_is_directory || dest.back() == '/') {
            ensure_dir(dest);
            continue;
        }
        mkdirs_for(dest);
        if (!mz_zip_reader_extract_to_file(&zip, i, dest.c_str(), 0)) {
            err = "extract failed: " + name;
            mz_zip_reader_end(&zip);
            return false;
        }
        if (log && (i % 32 == 0)) log(name);
    }
    mz_zip_reader_end(&zip);
    return true;
}

bool copy_file(const std::string& src, const std::string& dst, std::string& err) {
    FILE* a = std::fopen(src.c_str(), "rb");
    if (!a) { err = "open src"; return false; }
    mkdirs_for(dst);
    FILE* b = std::fopen(dst.c_str(), "wb");
    if (!b) { std::fclose(a); err = "open dst"; return false; }
    char buf[64 * 1024];
    size_t r;
    while ((r = std::fread(buf, 1, sizeof(buf), a)) > 0) {
        if (std::fwrite(buf, 1, r, b) != r) { std::fclose(a); std::fclose(b); err = "write"; return false; }
    }
    std::fclose(a); std::fclose(b);
    return true;
}

bool read_file(const std::string& path, std::string& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    char buf[4096]; size_t r;
    while ((r = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, r);
    std::fclose(f);
    return true;
}

bool write_file(const std::string& path, const std::string& content) {
    mkdirs_for(path);
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::fwrite(content.data(), 1, content.size(), f);
    std::fclose(f);
    return true;
}

std::string detect_hekate_version() {
    const char* dirs[] = {"sdmc:/", "sdmc:/bootloader/", "sdmc:/payloads/", "sdmc:/switch/"};
    for (const char* dp : dirs) {
        DIR* d = opendir(dp);
        if (!d) continue;
        while (auto* e = readdir(d)) {
            std::string n = e->d_name;
            if (n.rfind("hekate_ctcaer_", 0) == 0 && n.size() > 18 && n.substr(n.size()-4) == ".bin") {
                std::string ver = n.substr(14, n.size() - 14 - 4);
                size_t us = ver.find('_');          // trim _Nyx_X.Y.Z suffix
                if (us != std::string::npos) ver = ver.substr(0, us);
                closedir(d);
                return ver;
            }
        }
        closedir(d);
    }
    return {};
}

std::string detect_atmosphere_version() {
    if (R_FAILED(splInitialize())) return {};
    u64 ver = 0;
    Result rc = splGetConfig((SplConfigItem)65000, &ver);  // ExosphereVersion
    splExit();
    if (R_FAILED(rc)) return {};
    unsigned major = (ver >> 56) & 0xff;
    unsigned minor = (ver >> 48) & 0xff;
    unsigned micro = (ver >> 40) & 0xff;
    if (major == 0 && minor == 0 && micro == 0) return {};
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u", major, minor, micro);
    return buf;
}

std::string read_saved(const char* key) {
    std::string ini;
    if (!read_file(kSavedPath, ini)) return {};
    std::string pat = std::string(key) + "=";
    size_t at = ini.find(pat);
    if (at == std::string::npos) return {};
    size_t eol = ini.find('\n', at);
    std::string v = ini.substr(at + pat.size(),
                              (eol == std::string::npos ? ini.size() : eol) - (at + pat.size()));
    while (!v.empty() && (v.back() == '\r' || v.back() == ' ')) v.pop_back();
    return v;
}

void write_saved(const char* key, const std::string& value) {
    std::string ini;
    read_file(kSavedPath, ini);
    std::string pat = std::string(key) + "=";
    size_t at = ini.find(pat);
    std::string line = pat + value + "\n";
    if (at == std::string::npos) {
        if (!ini.empty() && ini.back() != '\n') ini += '\n';
        ini += line;
    } else {
        size_t eol = ini.find('\n', at);
        if (eol == std::string::npos) eol = ini.size();
        else eol += 1;
        ini.replace(at, eol - at, line);
    }
    write_file(kSavedPath, ini);
}

}
