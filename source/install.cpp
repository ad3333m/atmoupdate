#include "install.hpp"
#include "miniz.h"
#include <sys/stat.h>
#include <dirent.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <switch.h>

namespace {

constexpr const char* kSavedPath = "sdmc:/switch/AtmoUpdate/versions.ini";

// Held open by Atmosphère while it runs; the swap payload renames *.aio over them at boot.
bool locked_by_ams(const std::string& rel) {
    return rel == "atmosphere/package3" || rel == "atmosphere/stratosphere.romfs";
}

bool ensure_dir(const std::string& dir) {
    if (dir.empty() || dir == "sdmc:" || dir == "sdmc:/") return true;
    struct stat st{};
    if (stat(dir.c_str(), &st) == 0) return S_ISDIR(st.st_mode);
    size_t slash = dir.find_last_of('/');
    if (slash != std::string::npos && slash > 5 && !ensure_dir(dir.substr(0, slash))) return false;
    return mkdir(dir.c_str(), 0777) == 0 || errno == EEXIST;
}

std::string strip_lead(const std::string& p) {
    size_t i = 0;
    while (i < p.size() && (p[i] == '/' || p[i] == '.')) i++;
    return p.substr(i);
}

std::vector<int> ver_parts(const std::string& v) {
    std::vector<int> out;
    int cur = 0; bool any = false;
    for (char ch : v) {
        if (ch >= '0' && ch <= '9') { cur = cur * 10 + (ch - '0'); any = true; }
        else if (ch == '.') { out.push_back(cur); cur = 0; any = false; }
        else break;
    }
    if (any) out.push_back(cur);
    return out;
}

}

namespace install {

bool exists(const std::string& path) {
    struct stat st{};
    return stat(path.c_str(), &st) == 0;
}

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

bool extract_zip_to(const std::string& zip_path, const std::string& dest_root,
                    int& staged, std::string& err) {
    staged = 0;
    std::string root = dest_root;
    if (root.empty() || root.back() != '/') root += '/';
    const bool sd_root = (root == "sdmc:/");

    mz_zip_archive zip{};
    if (!mz_zip_reader_init_file(&zip, zip_path.c_str(), 0)) { err = "Bad zip file"; return false; }

    std::vector<unsigned char> buf(256 * 1024);
    unsigned n = mz_zip_reader_get_num_files(&zip);
    for (unsigned i = 0; i < n; i++) {
        mz_zip_archive_file_stat st{};
        if (!mz_zip_reader_file_stat(&zip, i, &st)) continue;
        std::string rel = strip_lead(st.m_filename);
        if (rel.empty() || rel.find("..") != std::string::npos) continue;
        std::string dest = root + rel;
        if (st.m_is_directory || dest.back() == '/') { ensure_dir(dest); continue; }

        if (sd_root && locked_by_ams(rel)) { dest += ".aio"; staged++; }
        mkdirs_for(dest);

        FILE* f = std::fopen(dest.c_str(), "wb");
        if (!f) {
            err = "Can't write " + rel + " (errno " + std::to_string(errno) + ")";
            mz_zip_reader_end(&zip);
            return false;
        }
        auto* it = mz_zip_reader_extract_iter_new(&zip, i, 0);
        bool ok = it != nullptr;
        while (ok) {
            size_t got = mz_zip_reader_extract_iter_read(it, buf.data(), buf.size());
            if (got == 0) break;
            ok = std::fwrite(buf.data(), 1, got, f) == got;
        }
        if (it) mz_zip_reader_extract_iter_free(it);
        if (std::fclose(f) != 0) ok = false;
        if (!ok) {
            std::remove(dest.c_str());
            err = "Write failed: " + rel + " (SD full?)";
            mz_zip_reader_end(&zip);
            return false;
        }
    }
    mz_zip_reader_end(&zip);
    return true;
}

bool copy_file(const std::string& src, const std::string& dst, std::string& err) {
    FILE* a = std::fopen(src.c_str(), "rb");
    if (!a) { err = "open " + src; return false; }
    mkdirs_for(dst);
    FILE* b = std::fopen(dst.c_str(), "wb");
    if (!b) { std::fclose(a); err = "open " + dst; return false; }
    std::vector<char> buf(64 * 1024);
    size_t r;
    bool ok = true;
    while (ok && (r = std::fread(buf.data(), 1, buf.size(), a)) > 0) ok = std::fwrite(buf.data(), 1, r, b) == r;
    std::fclose(a);
    if (std::fclose(b) != 0) ok = false;
    if (!ok) err = "write " + dst;
    return ok;
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

bool ams_update_pending() {
    return exists("sdmc:/atmosphere/package3.aio") || exists("sdmc:/atmosphere/stratosphere.romfs.aio");
}

std::string detect_hekate_version() {
    std::string best;
    const char* dirs[] = {"sdmc:/", "sdmc:/bootloader/", "sdmc:/payloads/", "sdmc:/bootloader/payloads/"};
    for (const char* dp : dirs) {
        DIR* d = opendir(dp);
        if (!d) continue;
        while (auto* e = readdir(d)) {
            std::string n = e->d_name;
            if (n.rfind("hekate_ctcaer_", 0) != 0 || n.size() < 19 || n.substr(n.size() - 4) != ".bin") continue;
            std::string ver = n.substr(14, n.size() - 18);
            size_t us = ver.find('_');               // _Nyx_x.y.z / __ram8GB
            if (us != std::string::npos) ver = ver.substr(0, us);
            if (ver_parts(ver).empty()) continue;
            if (best.empty() || ver_parts(ver) > ver_parts(best)) best = ver;
        }
        closedir(d);
    }
    return best;
}

std::string detect_firmware_version() {
    if (R_FAILED(setsysInitialize())) return {};
    SetSysFirmwareVersion v{};
    Result rc = setsysGetFirmwareVersion(&v);
    setsysExit();
    if (R_FAILED(rc)) return {};
    v.display_version[sizeof(v.display_version) - 1] = '\0';
    return v.display_version;
}

std::string detect_atmosphere_version() {
    if (R_FAILED(splInitialize())) return {};
    u64 ver = 0;
    Result rc = splGetConfig((SplConfigItem)65000, &ver);  // ExosphereVersion
    splExit();
    if (R_FAILED(rc)) return {};
    unsigned major = (ver >> 56) & 0xff, minor = (ver >> 48) & 0xff, micro = (ver >> 40) & 0xff;
    if (!major && !minor && !micro) return {};
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
    std::string v = ini.substr(at + pat.size(), (eol == std::string::npos ? ini.size() : eol) - (at + pat.size()));
    while (!v.empty() && (v.back() == '\r' || v.back() == ' ')) v.pop_back();
    return v;
}

void write_saved(const char* key, const std::string& value) {
    std::string ini;
    read_file(kSavedPath, ini);
    std::string pat = std::string(key) + "=";
    std::string line = pat + value + "\n";
    size_t at = ini.find(pat);
    if (at == std::string::npos) {
        if (!ini.empty() && ini.back() != '\n') ini += '\n';
        ini += line;
    } else {
        size_t eol = ini.find('\n', at);
        eol = (eol == std::string::npos) ? ini.size() : eol + 1;
        ini.replace(at, eol - at, line);
    }
    write_file(kSavedPath, ini);
}

}
