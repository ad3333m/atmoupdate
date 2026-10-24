#pragma once
#include <string>

namespace reboot {

// Reboots into romfs:/swap_payload.bin, which renames the staged *.aio
// Atmosphère files into place and then boots hekate. Only returns on failure.
bool finish_ams_update(std::string& err);

}
