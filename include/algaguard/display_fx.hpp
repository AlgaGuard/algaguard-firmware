#pragma once

#include <algorithm>
#include <cstdint>

#include "algaguard/display.hpp"

// Pure framebuffer transforms for OLED animation. Every I2C page write is a
// full 1024-byte flush (no partial updates, no display library), so
// "animation" here means a short, bounded sequence of full frames rather
// than a pixel tween: each function below is a deterministic function of a
// step index, host-testable without touching hardware.
namespace algaguard {

// Left-to-right reveal of `to` over `from`, `step` of `total_steps`.
// step == 0 reproduces `from` exactly; step >= total_steps reproduces `to`.
inline Framebuffer wipe_transition(const Framebuffer& from, const Framebuffer& to,
                                   std::uint8_t step, std::uint8_t total_steps) {
  if (total_steps == 0 || step >= total_steps) return to;
  if (step == 0) return from;
  Framebuffer result{};
  const auto threshold = static_cast<std::uint16_t>(
      (static_cast<std::uint16_t>(step) * 128U) / total_steps);
  for (std::uint8_t page = 0; page < 8; ++page) {
    for (std::uint16_t x = 0; x < 128; ++x) {
      const auto index = static_cast<std::size_t>(page) * 128U + x;
      result[index] = (x < threshold) ? to[index] : from[index];
    }
  }
  return result;
}

// Progressive boot reveal: the logo wipes in top-to-bottom, then the
// wordmark and tagline settle in as `step` approaches `total_steps`.
inline Framebuffer splash_frame(std::uint8_t step, std::uint8_t total_steps) {
  const std::uint8_t bounded_total = total_steps == 0 ? std::uint8_t{1} : total_steps;
  const std::uint8_t bounded_step = std::min(step, bounded_total);
  Framebuffer framebuffer{};
  const auto reveal_rows = static_cast<std::uint8_t>(
      (static_cast<std::uint16_t>(bounded_step) * brand::kLogoHeight) / bounded_total);
  draw_brand_logo(framebuffer, 6, 10, reveal_rows);
  if (static_cast<std::uint16_t>(bounded_step) * 2U >= bounded_total) {
    draw_text_scaled(framebuffer, "ALGA", 50, 12, 2);
    draw_text_scaled(framebuffer, "GUARD", 50, 28, 2);
  }
  if (bounded_step >= bounded_total)
    draw_text(framebuffer, "TANK MONITOR", 50, 48);
  return framebuffer;
}

}  // namespace algaguard
