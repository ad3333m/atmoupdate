#pragma once
#include <string>
#include <functional>

namespace install {

using LogFn = std::function<void(const std::string&)>;

// Extracts zip at zip_path into sdmc:/, creating parent dirs. Returns true on success.
bool extract_zip_to_root(const std::string& zip_path, const LogFn& log, std::string& err);

// Copy src to dst, creating parent dirs. Overwrites.
bool copy_file(const std::string& src, const std::string& dst, std::string& err);

// Make sure every component of path exists (last component treated as a dir if trailing /, else file).
bool mkdirs_for(const std::string& path);

// Read first bytes to detect ZIP header (PK\x03\x04).
bool is_zip(const std::string& path);

// Read whole file into string.
bool read_file(const std::string& path, std::string& out);

// Write string to file (overwrites).
bool write_file(const std::string& path, const std::string& content);

// Scan common locations for installed hekate payload version. Returns empty string if not found.
std::string detect_hekate_version();

// Query running Atmosphère via splGetConfig(ExosphereVersion). Returns empty if not booted into AMS.
std::string detect_atmosphere_version();

// Read our own saved versions.ini key.
std::string read_saved(const char* key);
void write_saved(const char* key, const std::string& value);

}
