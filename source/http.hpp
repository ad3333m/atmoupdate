#pragma once
#include <string>
#include <functional>

namespace http {

// (downloaded, total) -> keep going? Return false to abort.
using ProgressFn = std::function<bool(double, double)>;

bool get_string(const std::string& url, std::string& out, std::string& err);
bool get_file(const std::string& url, const std::string& dest_path,
              const ProgressFn& progress, std::string& err);

}
