#pragma once

#include "netease_music_service.h"

#include <memory>

namespace netease_music {

// Factory for the manager-api implementation. Its base URL is explicitly configurable and,
// when blank, is derived from the configured OTA URL on the same manager service.
std::unique_ptr<Client> CreateNeteaseMusicClient();

Service& GetDeviceService();

}  // namespace netease_music
