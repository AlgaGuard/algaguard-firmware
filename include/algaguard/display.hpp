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

// Mirrors draw_pixel but clears the bit -- used to "punch out" text against a
// filled highlight bar (see draw_text's invert parameter) since the display
// has no separate background layer to redraw over.
inline void clear_pixel(Framebuffer& framebuffer, std::uint8_t x, std::uint8_t y) {
  if (x >= 128 || y >= 64) return;
  framebuffer[(y / 8U) * 128U + x] &=
      static_cast<std::uint8_t>(~(1U << (y % 8U)));
}

inline void fill_rect(Framebuffer& framebuffer, std::uint8_t x, std::uint8_t y,
                      std::uint8_t width, std::uint8_t height) {
  for (std::uint8_t row = 0; row < height; ++row)
    for (std::uint8_t column = 0; column < width; ++column)
      draw_pixel(framebuffer, static_cast<std::uint8_t>(x + column),
                static_cast<std::uint8_t>(y + row));
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

// `invert`, when true, clears glyph pixels instead of setting them -- used to
// draw "punched out" text against a fill_rect'd highlight bar (the display
// has no separate background layer, so this is the only way to get an
// inverted-looking row on a 1bpp panel).
inline void draw_text(Framebuffer& framebuffer, std::string_view text,
                      std::uint8_t origin_x, std::uint8_t origin_y,
                      bool invert = false) {
  std::uint8_t x = origin_x;
  for (const char character : text) {
    if (x > 123) break;
    const auto rows = glyph(character);
    for (std::uint8_t row = 0; row < rows.size(); ++row) {
      for (std::uint8_t column = 0; column < 3; ++column) {
        if ((rows[row] & (1U << (2U - column))) == 0) continue;
        const auto pixel_x = static_cast<std::uint8_t>(x + column);
        const auto pixel_y = static_cast<std::uint8_t>(origin_y + row);
        if (invert)
          clear_pixel(framebuffer, pixel_x, pixel_y);
        else
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

inline constexpr std::uint8_t kMenuVisibleRows = 4;
inline constexpr std::uint8_t kMenuRowPitch = 11;
inline constexpr std::uint8_t kMenuRowsTop = 19;

// Deliberately decoupled from any concrete menu-item type: takes a plain
// label array + count so this header stays a low-level drawing utility with
// no dependency on main.cpp's menu content/ordering.
inline Framebuffer compose_menu_screen(const char* const* labels,
                                       std::size_t itemCount,
                                       std::size_t selectedIndex) {
  Framebuffer framebuffer{};
  draw_frame_border(framebuffer);
  draw_header_divider(framebuffer);
  draw_text_scaled(framebuffer, "MENU", 3, 3, 2);

  // Recomputed fresh from selectedIndex on every call (no persisted scroll
  // state): if the selection is still within the first window it stays at
  // offset 0, otherwise the window's bottom edge tracks the selection --
  // this alone handles scrolling back up too, since offset returns to 0
  // as soon as selectedIndex is inside [0, kMenuVisibleRows) again.
  std::size_t offset = 0;
  if (itemCount > kMenuVisibleRows) {
    const std::size_t maxOffset = itemCount - kMenuVisibleRows;
    if (selectedIndex >= kMenuVisibleRows)
      offset = selectedIndex - kMenuVisibleRows + 1;
    if (offset > maxOffset) offset = maxOffset;
  }

  const std::size_t visible = std::min<std::size_t>(
      kMenuVisibleRows, itemCount - offset);
  for (std::size_t row = 0; row < visible; ++row) {
    const std::size_t item = offset + row;
    const auto y = static_cast<std::uint8_t>(kMenuRowsTop + row * kMenuRowPitch);
    const bool selected = item == selectedIndex;
    if (selected) fill_rect(framebuffer, 2, static_cast<std::uint8_t>(y - 2),
                            124, kMenuRowPitch - 1);
    draw_text(framebuffer, labels[item], 5, y, selected);
  }

  // Small hand-drawn scroll-affordance triangles -- no font-table entry
  // exists for arrow glyphs, so these are a few direct pixel calls instead.
  if (offset > 0)
    // Apex at (123,16), widening downward to a 5px base at y=18 -- points up.
    for (std::uint8_t row = 0; row < 3; ++row)
      for (std::uint8_t column = 0; column <= row * 2U; ++column)
        draw_pixel(framebuffer,
                  static_cast<std::uint8_t>(123 - row + column),
                  static_cast<std::uint8_t>(16 + row));
  if (offset + visible < itemCount)
    // 5px base at y=59, narrowing to an apex at (123,61) -- points down.
    for (std::uint8_t row = 0; row < 3; ++row) {
      const auto width = static_cast<std::uint8_t>((2U - row) * 2U);
      for (std::uint8_t column = 0; column <= width; ++column)
        draw_pixel(framebuffer,
                  static_cast<std::uint8_t>(123 - width / 2 + column),
                  static_cast<std::uint8_t>(59 + row));
    }

  return framebuffer;
}

}  // namespace algaguard
