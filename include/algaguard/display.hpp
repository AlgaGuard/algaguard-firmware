#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>

#include "algaguard/brand_splash.hpp"
#include "algaguard/config.hpp"
#include "algaguard/startup.hpp"

namespace algaguard {

// Raw 128x64 monochrome page-major framebuffer: byte (page*128 + x) holds the
// 8 vertically-stacked pixels at column x for rows [page*8, page*8+8).
using Framebuffer = std::array<std::uint8_t, 1024>;

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

inline void draw_pixel(Framebuffer& framebuffer, std::uint8_t x, std::uint8_t y) {
  if (x >= 128 || y >= 64) return;
  framebuffer[(y / 8U) * 128U + x] |= static_cast<std::uint8_t>(1U << (y % 8U));
}

inline std::array<std::uint8_t, 5> glyph(char raw) {
  const char value =
      static_cast<char>(std::toupper(static_cast<unsigned char>(raw)));
  if (value >= '0' && value <= '9') {
    constexpr std::array<std::array<std::uint8_t, 5>, 10> digits{{
        {{7, 5, 5, 5, 7}}, {{2, 6, 2, 2, 7}}, {{7, 1, 7, 4, 7}},
        {{7, 1, 7, 1, 7}}, {{5, 5, 7, 1, 1}}, {{7, 4, 7, 1, 7}},
        {{7, 4, 7, 5, 7}}, {{7, 1, 1, 2, 2}}, {{7, 5, 7, 5, 7}},
        {{7, 5, 7, 1, 7}},
    }};
    return digits[static_cast<std::size_t>(value - '0')];
  }
  if (value >= 'A' && value <= 'Z') {
    constexpr std::array<std::array<std::uint8_t, 5>, 26> letters{{
        {{2, 5, 7, 5, 5}}, {{6, 5, 6, 5, 6}}, {{3, 4, 4, 4, 3}},
        {{6, 5, 5, 5, 6}}, {{7, 4, 6, 4, 7}}, {{7, 4, 6, 4, 4}},
        {{3, 4, 5, 5, 3}}, {{5, 5, 7, 5, 5}}, {{7, 2, 2, 2, 7}},
        {{1, 1, 1, 5, 2}}, {{5, 5, 6, 5, 5}}, {{4, 4, 4, 4, 7}},
        {{5, 7, 7, 5, 5}}, {{5, 7, 7, 7, 5}}, {{2, 5, 5, 5, 2}},
        {{6, 5, 6, 4, 4}}, {{2, 5, 5, 7, 3}}, {{6, 5, 6, 5, 5}},
        {{3, 4, 2, 1, 6}}, {{7, 2, 2, 2, 2}}, {{5, 5, 5, 5, 7}},
        {{5, 5, 5, 5, 2}}, {{5, 5, 7, 7, 5}}, {{5, 5, 2, 5, 5}},
        {{5, 5, 2, 2, 2}}, {{7, 1, 2, 4, 7}},
    }};
    return letters[static_cast<std::size_t>(value - 'A')];
  }
  if (value == '-') return {{0, 0, 7, 0, 0}};
  if (value == '_') return {{0, 0, 0, 0, 7}};
  if (value == ':') return {{0, 2, 0, 2, 0}};
  if (value == '.') return {{0, 0, 0, 0, 2}};
  return {{0, 0, 0, 0, 0}};
}

inline void draw_text(Framebuffer& framebuffer, std::string_view text,
                      std::uint8_t origin_x, std::uint8_t origin_y) {
  std::uint8_t x = origin_x;
  for (const char character : text) {
    if (x > 123) break;
    const auto rows = glyph(character);
    for (std::uint8_t row = 0; row < rows.size(); ++row) {
      for (std::uint8_t column = 0; column < 3; ++column) {
        if ((rows[row] & (1U << (2U - column))) == 0) continue;
        const auto pixel_x = static_cast<std::uint8_t>(x + column);
        const auto pixel_y = static_cast<std::uint8_t>(origin_y + row);
        draw_pixel(framebuffer, pixel_x, pixel_y);
      }
    }
    x = static_cast<std::uint8_t>(x + 4U);
  }
}

inline void draw_text_scaled(Framebuffer& framebuffer, std::string_view text,
                             std::uint8_t origin_x, std::uint8_t origin_y,
                             std::uint8_t scale) {
  std::uint8_t x = origin_x;
  for (const char character : text) {
    const auto rows = glyph(character);
    for (std::uint8_t row = 0; row < rows.size(); ++row)
      for (std::uint8_t column = 0; column < 3; ++column) {
        if ((rows[row] & (1U << (2U - column))) == 0) continue;
        for (std::uint8_t dy = 0; dy < scale; ++dy)
          for (std::uint8_t dx = 0; dx < scale; ++dx) {
            const auto px = static_cast<std::uint8_t>(x + column * scale + dx);
            const auto py = static_cast<std::uint8_t>(origin_y + row * scale + dy);
            draw_pixel(framebuffer, px, py);
          }
      }
    x = static_cast<std::uint8_t>(x + 4U * scale);
    if (x >= 128) break;
  }
}

// `reveal_rows` lets the boot animation draw the logo progressively, top to
// bottom; it defaults to the full mark for every other caller.
inline void draw_brand_logo(Framebuffer& framebuffer, std::uint8_t origin_x,
                            std::uint8_t origin_y,
                            std::uint8_t reveal_rows = brand::kLogoHeight) {
  const auto rows = std::min<std::uint8_t>(reveal_rows, brand::kLogoHeight);
  for (std::uint8_t y = 0; y < rows; ++y)
    for (std::uint8_t x = 0; x < brand::kLogoWidth; ++x) {
      const auto byte = brand::kLogoMask[
          static_cast<std::size_t>(y) * brand::kLogoStride + x / 8U];
      if ((byte & (1U << (7U - x % 8U))) == 0) continue;
      const auto px = static_cast<std::uint8_t>(origin_x + x);
      const auto py = static_cast<std::uint8_t>(origin_y + y);
      draw_pixel(framebuffer, px, py);
    }
}

inline void draw_frame_border(Framebuffer& framebuffer) {
  for (std::uint8_t x = 0; x < 128; ++x) {
    draw_pixel(framebuffer, x, 0);
    draw_pixel(framebuffer, x, 63);
  }
  for (std::uint8_t y = 0; y < 64; ++y) {
    draw_pixel(framebuffer, 0, y);
    draw_pixel(framebuffer, 127, y);
  }
}

inline void draw_header_divider(Framebuffer& framebuffer) {
  for (std::uint8_t x = 3; x < 125; ++x) draw_pixel(framebuffer, x, 16);
}

// Every screen in the firmware (startup states, physical-test states, QR
// onboarding, local-demo pages, the physical-unpair prompt) is a 4-line
// DiagnosticScreen funnelled through this single composition point, so the
// frame/divider chrome and text layout below apply uniformly everywhere
// without touching each screen's call site.
inline Framebuffer compose_screen(const DiagnosticScreen& screen) {
  Framebuffer framebuffer{};
  draw_frame_border(framebuffer);
  draw_header_divider(framebuffer);
  if (screen.lines[0].size() <= 10)
    draw_text_scaled(framebuffer, screen.lines[0], 3, 3, 2);
  else
    draw_text(framebuffer, screen.lines[0], 3, 4);
  for (std::uint8_t index = 1; index < screen.lines.size(); ++index)
    draw_text(framebuffer, screen.lines[index], 3,
              static_cast<std::uint8_t>(20U + (index - 1U) * 14U));
  return framebuffer;
}

}  // namespace algaguard
