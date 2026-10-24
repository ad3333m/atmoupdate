#pragma once
#include <string>

namespace install {

// Extracts zip into dest_root ("sdmc:/" or "sdmc:/firmware/23.0.1/").
// Files Atmosphère keeps open while running are written as "<name>.aio";
// `staged` counts them (a reboot through the swap payload applies them).
bool extract_zip_to(const std::string& zip_path, const std::string& dest_root,
                    int& staged, std::string& err);

bool copy_file(const std::string& src, const std::string& dst, std::string& err);
bool exists(const std::string& path);
bool mkdirs_for(const std::string& path);
bool is_zip(const std::string& path);
bool read_file(const std::string& path, std::string& out);
bool write_file(const std::string& path, const std::string& content);

// True while package3.aio / stratosphere.romfs.aio wait for the swap payload.
bool ams_update_pending();

std::string detect_hekate_version();      // newest hekate_ctcaer_X.Y.Z.bin on the SD
std::string detect_atmosphere_version();  // live, via splGetConfig(ExosphereVersion)
std::string detect_firmware_version();    // live, via setsys

std::string read_saved(const char* key);
void write_saved(const char* key, const std::string& value);

}
