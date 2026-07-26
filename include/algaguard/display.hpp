#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "algaguard/config.hpp"
#include "algaguard/startup.hpp"

namespace algaguard {

struct DiagnosticScreen {
  std::array<std::string, 4> lines;

  bool safe() const {
    for (const auto& line : lines)
      if (contains_private_material(line) ||
          line.find("password") != std::string::npos ||
          line.find("CERTIFICATE") != std::string::npos)
        return false;
    return true;
  }
};

inline DiagnosticScreen boot_screen(const FirmwareConfig& config) {
  return {{{"AlgaGuard", "BOOT", std::string{config.environment_id},
            std::string{config.device_id}}}};
}

inline DiagnosticScreen hardware_screen(std::uint32_t flash_bytes,
                                        std::uint32_t psram_bytes,
                                        bool profile_matches) {
  return {{{"ESP32-S3 N16R8",
            "FLASH " + std::to_string(flash_bytes / (1024U * 1024U)) + " MB",
            "PSRAM " + std::to_string(psram_bytes / (1024U * 1024U)) + " MB",
            profile_matches ? "PROFILE OK" : "PROFILE MISMATCH"}}};
}

inline DiagnosticScreen state_screen(StartupState state, StartupReason reason) {
  return {{{"AlgaGuard", std::string{startup_state_name(state)},
            "REASON " + std::to_string(static_cast<unsigned>(reason)), "NO SECRETS"}}};
}

}  // namespace algaguard
