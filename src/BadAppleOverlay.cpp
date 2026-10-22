#include <pl/Mod.hpp>
#include <pl/ModMenu.hpp>
#include <pl/Input.hpp>
#include <algorithm>
#include <cstdint>
#include <string_view>
#include <vector>

namespace {
constexpr const char* kModuleId = "bad_apple_overlay.window";
constexpr int kPanelW = 360;
constexpr int kPanelH = 220;
constexpr int kTitleH = 36;

std::vector<unsigned char> makeRgba(int w, int h, unsigned char r,
                                   unsigned char g, unsigned char b,
                                   unsigned char a) {
  std::vector<unsigned char> pixels(static_cast<size_t>(w) * h * 4);
  for (size_t i = 0; i < pixels.size(); i += 4) {
    pixels[i] = r; pixels[i + 1] = g; pixels[i + 2] = b; pixels[i + 3] = a;
  }
  return pixels;
}

class BadAppleOverlayMod {
public:
  static BadAppleOverlayMod& instance() {
    static BadAppleOverlayMod value;
    return value;
  }
  BadAppleOverlayMod() : self(*ll::mod::NativeMod::current()) {}
  ll::mod::NativeMod& getSelf() const { return self; }

  bool load() {
    self.getLogger().info("BadAppleOverlay test mod loaded");
    return true;
  }
  bool enable() {
    if (!imagesRegistered) {
      panel = makeRgba(kPanelW, kPanelH, 18, 18, 22, 235);
      title = makeRgba(kPanelW, kTitleH, 60, 60, 68, 255);
      if (!pl::modmenu::registerImage("bad_apple_overlay.panel", panel, kPanelW, kPanelH)) return false;
      if (!pl::modmenu::registerImage("bad_apple_overlay.title", title, kPanelW, kTitleH)) return false;
      imagesRegistered = true;
    }
    if (!pl::modmenu::ModuleBuilder(kModuleId, "Bad Apple Overlay (test)")
          .modId(self.getId())
          .description("Test draggable in-game panel. Video/audio playback is not implemented in this test build.")
          .defaultEnabled(false)
          .registerModule()) return false;
    // The callback cannot be removed with an API we know of, so register it once
    // and let the 'active' flag decide whether it does anything.
    if (!touchRegistered) {
      pl::input::registerTouchCallback([](const pl::input::TouchEvent& e) {
        return instance().onTouch(e);
      });
      touchRegistered = true;
    }
    active = true;
    draw();
    return true;
  }
  bool disable() {
    active = false;
    dragging = false;
    pl::modmenu::submitDrawCommands(kModuleId, {});
    pl::modmenu::unregisterModule(kModuleId);
    return true;
  }
private:
  bool onTouch(const pl::input::TouchEvent& e) {
    if (!active) return false;
    // Android touch action values: DOWN=0, UP=1, MOVE=2, CANCEL=3.
    if (e.action == 0) {
      dragging = (e.x >= x && e.x <= x + kPanelW && e.y >= y && e.y <= y + kTitleH);
      if (dragging) { lastX = e.x; lastY = e.y; return true; }
    } else if (e.action == 2 && dragging) {
      x += static_cast<int>(e.x - lastX);
      y += static_cast<int>(e.y - lastY);
      lastX = e.x; lastY = e.y;
      x = std::max(0, x); y = std::max(0, y);
      draw();
      return true;
    } else if (e.action == 1 || e.action == 3) {
      const bool wasDragging = dragging;
      dragging = false;
      return wasDragging;
    }
    return false;
  }
  void draw() {
    const std::vector<pl::modmenu::DrawCommand> commands = {
      {.type = pl::modmenu::DrawCommandType::Image, .x = static_cast<float>(x), .y = static_cast<float>(y), .w = static_cast<float>(kPanelW), .h = static_cast<float>(kPanelH), .imageId = "bad_apple_overlay.panel"},
      {.type = pl::modmenu::DrawCommandType::Image, .x = static_cast<float>(x), .y = static_cast<float>(y), .w = static_cast<float>(kPanelW), .h = static_cast<float>(kTitleH), .imageId = "bad_apple_overlay.title"}
    };
    pl::modmenu::submitDrawCommands(kModuleId, commands);
  }
  ll::mod::NativeMod& self;
  std::vector<unsigned char> panel, title;
  int x = 40, y = 80;
  float lastX = 0, lastY = 0;
  bool dragging = false;
  bool active = false;
  bool imagesRegistered = false;
  bool touchRegistered = false;
};
}

PL_REGISTER_MOD(BadAppleOverlayMod, BadAppleOverlayMod::instance())
