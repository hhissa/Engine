#include "controller_mode.h"
#include "sculpt_document.h"
#include "sculpt_shared.h"

#include <core/application.h>
#include <core/hmemory.h>
#include <core/input.h>
#include <core/logger.h>
#include <game_types.h>
#include <renderer/camera.h>
#include <renderer/renderer_frontend.h>

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

// Controller mode: the sculpt scene in the engine's own window, with no Qt.
// Controls follow "Gamepad controls-1.docx" (next to this tool's
// CMakeLists.txt). That plan is incomplete and will change. So far:
//
//   Left stick        left/right moves the brush (X) -- past the screen
//                     edge the camera strafes instead; up/down moves the
//                     camera and brush together forward/back (Z)
//   L1 + left stick   strafe mode: brush left/right (X) and up/down (Y),
//                     same edge rule
//   Right stick       look
//   R1 (hold)         orbit the camera around the brush point:
//                       left stick  left/right swings around, up/down zooms
//                       right stick left/right swings around, up/down tilts
//   L1 + R1           move the camera to frame the object under the
//                     reticle (or the whole sculpt if there isn't one)
//   R2 (hold)         grab the object under the reticle:
//                       left stick  moves it left/right, forward/back
//                       R1/L1 + left stick  moves it left/right, up/down
//                                   (strafe, as for the camera)
//                       right stick left/right rotates it about the point
//                                   halfway between it and the camera;
//                                   up/down moves it up/down
//                       D-pad up/down  grows / shrinks it
//   L1 + R2 (hold)    clone the object under the reticle and grab the copy.
//                     That makes a chain: the original is its start, the
//                     held copy its end. While still holding R2:
//                       D-pad left   (Fill) adds a copy in between (all
//                                    spaced evenly from start to end) --
//                                    or, while there are copies past the
//                                    end, removes one of those instead
//                       D-pad right  (Repeat) adds a copy past the end,
//                                    repeating the start-to-held step --
//                                    or, while there are copies in
//                                    between, removes one of those
//                     The chain follows the held copy live (position in a
//                     straight line, plus each link's share of its turn
//                     and resize) and is left in place on release.
//   L2 (hold)         rotate the object under the reticle about its centre:
//                       either stick up/down  about X (screen right)
//                       left stick left/right about Y (screen up)
//                       right stick left/right about Z (view direction)
//   Cross / A         select the object under the reticle, or deselect it
//                     if it is already selected. Grabbing, cloning,
//                     rotating, flipping, snapping or deleting any selected
//                     object acts on the whole selection. Selected objects
//                     get a dotted outline. Over a toolbar button, A
//                     presses it instead.
//   B                 leave scope-in (see the toolbar)
//   Triangle / Y      while grabbing (R2) or rotating (L2): snap -- the
//                     held object's position to a 0.25 grid, rotation to
//                     the nearest 45 degrees (a group moves with it).
//                     Otherwise: delete the object (or group) under the
//                     reticle
//   Start (hold)      recenter the reticle on the screen
//   D-pad left/right  undo / redo (except mid-clone, where they Fill and
//                     Repeat -- see above). A step is each finished action:
//                     a grab, rotate, flip, delete, clone, group change, or
//                     R2 stroke in a mode.
//   L3 (click) + left stick direction
//                     flip the held object (or the one under the reticle)
//                     90 degrees that way, like tipping it over: up tips
//                     it away, down toward you, left/right onto that side
//
// Toolbar: a column of round buttons on the right, centred vertically
// (inside the area the reticle can reach), pressed by putting the reticle on one and pressing A:
//   Group     groups the selected objects permanently: selecting or
//             grabbing one member then always takes the whole group, and the
//             group gets one merged outline. Pressed with exactly one group
//             selected, it ungroups it instead.
//   Scope in  with one group selected: everything else is blurred and
//             greyed, and only the group's members can be picked -- one at a
//             time, so they can be selected and moved individually. Press
//             it again (or B) to scope back out.
//   Delete    deletes every selected object.
//   (one placeholder button that does nothing yet)
// The column slides in from the right edge while something is selected (or
// a group is scoped into) and slides back out when nothing is.
//
// Top bar: buttons at the top centre, each opening a row of buttons
// underneath it (pressing the open one again closes it). Normally it holds
// Tools (empty for now) and Modes (Sculpt, Paint, Coat, Style, Effects --
// only Coat works so far). Inside a mode, an Exit button in the top-right
// corner (or L1 + B) goes back to the normal controls.
//
// Coat mode: the controls stay the same, except
//   R2 (hold)  applies the current coat tool to the object (or group) under
//              the reticle, gradually, up to 100%
//   Y          switches between adding and subtracting (instead of delete)
//   L1 + B     leaves coat mode (as does the Exit button, top right)
// and the top bar becomes Tools | Add/Subtract | Colour | Finishes:
//   Tools      Tint (move the colour toward the picked colour), Hue (take
//              the picked colour's hue, keeping the object's brightness),
//              Finish (move toward the picked finish), Glow (glow in the
//              object's own colour) and Revert (back toward plain).
//              Subtracting fades that effect back toward plain instead.
//   Colour     10 colours; picking one also shows its 5 shades underneath
//              (B hides them again)
//   Finishes   the finishes, previewed as spheres in the picked colour
// Every object starts plain: slightly grey, no texture, Default finish.
//
// Style mode: the same controls as coat mode (R2 applies the tool to the
// object or group under the reticle, Y switches add/subtract, L1 + B or the
// Exit button leaves), with the top bar Tools | Add/Subtract | Fleck Style.
// The surface is drawn as flecks (splats); the tools change them:
//   Fleck Style  gives the object the picked fleck shape (subtract: back to
//                the default circle)
//   Looseness    fewer, bigger flecks, showing their shape (subtract:
//                tighter, back to a smooth surface)
//   Comb         turns the flecks to line up, all one way
//   Ruffle       the inverse of Comb: scatters the flecks back to random
//                directions (subtract: combs them, as Comb does)
//   Impasto      lifts each fleck's tip off the surface, base still
//                attached, like thick paint
//   Neatness     lays the flecks out in neat rows, each row offset half a
//                fleck from the next (subtract: scattered again)
//   Revert       eases every style setting back to its default (style
//                changes only; coat and effects are left alone)
// Fleck Style holds the 16 Dreams fleck styles. L2 does nothing here.
//
// Effects mode: style mode's controls, with the top bar Tools |
// Add/Subtract. Each tool animates the flecks, R2 building it up (subtract:
// calming it back down):
//   Boil       flecks bubble up and burst, like boiling water
//   Flow       flecks stream downhill across the surface, like a river
//   Wave       flecks sway and flutter, like grass in the wind
//   Evaporate  flecks soften and drift up and away, like steam
//   Throb      flecks swell in and out together, like a pulse
//   Revert     calms every effect back down (effects only)
// An object with any effect is drawn as flecks even at looseness 0.
//
// A tooltip beside the reticle lists, with a button icon and one word
// each, what the buttons do right now: the object actions when hovering an
// object, the stick moves while grabbing or rotating one, a button's name
// (and shortcut, if it has one) when hovering a button, and getting around
// plus undo/redo when over nothing. The icons are
// PNGs in bin/assets/ui/gamepad/ (made by icons/make_gamepad_icons.sh).
//
// "Under the reticle" means hit by the line of sight through it. An object
// is a group of stamps (one Qt-mode stroke); objects only come from a
// loaded .stamps file for now -- run with `--controller FILE`. The brush is
// a point floating in front of the camera, kept in camera space so it
// travels with the view, and drawn as a reticle. Escape (handled by the
// engine) quits.
//
// Screen orientation: this engine draws world +Y DOWNWARD (the raymarch
// shader flips uv.y), so Camera::up() points down on screen and a positive
// pitch looks down. Everything below is written in "on screen" terms and
// converts at that boundary.

namespace {
using input::GamepadAxis;
using input::GamepadButton;

constexpr f32 kStickDeadzone = 0.15f;
constexpr f32 kTriggerPressed = 0.5f;
constexpr f32 kTriggerReleased = 0.3f; // hysteresis so a half-held trigger can't chatter

// Brush movement. Sideways speed scales with the brush's depth so it
// crosses the screen at the same rate however far away it is.
constexpr f32 kBrushScreenSpeed = 0.9f; // screen-heights per second
constexpr f32 kForwardSpeed = 3.0f; // world units per second, left stick Z
constexpr f32 kDefaultBrushDepth = 5.0f; // the Qt mode's kStampDistance
constexpr f32 kMinBrushDepth = 1.0f;
constexpr f32 kMaxBrushDepth = 12.0f;
// How far from the centre (as a fraction of the half-screen) the brush may
// go before the camera starts strafing instead.
constexpr f32 kScreenEdge = 0.85f;

constexpr f32 kLookRate = 2.0f;  // radians per second at full stick
constexpr f32 kOrbitRate = 2.0f; // radians per second at full stick
// R1 + left stick up/down: fraction of the distance to the pivot covered
// per second at full stick.
constexpr f32 kZoomRate = 1.5f;
// Pitch the camera may reach while orbiting -- just inside Camera's own
// +-89 degree clamp, so an orbit can never be silently cut short by it.
constexpr f32 kMaxOrbitPitch = 1.50f;

// L1 + R1 framing.
constexpr f32 kFrameDistanceScale = 2.5f; // camera distance, in object radii
constexpr f32 kMinFrameDistance = 2.0f;
constexpr f32 kFrameDuration = 0.3f; // seconds to glide into place

// Start: how long it must be held before the reticle recenters (so a tap
// doesn't), and how long the glide back takes.
constexpr f32 kRecenterHoldTime = 0.4f;
constexpr f32 kRecenterDuration = 0.2f;

// L3 flip: how far the stick must be pushed for the click to count as a
// direction, the flip angle, and how long the tip-over animation takes.
constexpr f32 kFlipStickThreshold = 0.5f;
constexpr f32 kFlipAngle = 1.5707963f; // 90 degrees
constexpr f32 kFlipDuration = 0.15f;

// Picking: how far along the line of sight an object can be.
constexpr f32 kPickMaxDistance = 200.0f;

// Object manipulation.
constexpr f32 kGrabScreenSpeed = 0.6f; // screen-heights per second (scaled by depth)
constexpr f32 kGrabTurnRate = 1.0f;    // radians per second, R2 + right stick
// A grabbed object stays between these distances in front of the camera.
// The far limit keeps it inside the range the renderer draws sculpts at
// (see STREAM_RADIUS_CHUNKS) -- past it the object would vanish -- and the
// near one keeps it from being pulled through the camera.
constexpr f32 kMinGrabDistance = 1.5f;
constexpr f32 kMaxGrabDistance = 24.0f;
// Grab speed scales with distance (so the object crosses the screen at a
// steady rate) only up to here; beyond it, pushing an object away would
// otherwise accelerate it without bound.
constexpr f32 kMaxGrabSpeedDistance = 12.0f;
constexpr f32 kRotateRate = 1.5f;      // radians per second, L2 + sticks
// D-pad resize while grabbing: size doubles (or halves) every this many
// seconds the button is held.
constexpr f32 kResizeDoublingTime = 1.0f;
// Y snapping. The grid matches the engine's coarse cell size.
constexpr f32 kSnapGrid = 0.25f;
constexpr f32 kSnapAngle = 0.78539816f; // 45 degrees

// Reticle (pixels).
constexpr f32 kReticleGap = 5.0f;
constexpr f32 kReticleArm = 9.0f;
constexpr glm::vec4 kReticleIdle{1.0f, 1.0f, 1.0f, 0.9f};
constexpr glm::vec4 kReticleOverObject{1.0f, 0.85f, 0.3f, 1.0f};
constexpr glm::vec4 kReticleGrabbing{0.4f, 1.0f, 0.5f, 1.0f};
constexpr glm::vec4 kReticleRotating{0.4f, 0.85f, 1.0f, 1.0f};
// Toolbar (pixels): button radius, gap between buttons, and the margin
// inside the area the reticle can reach (see kScreenEdge).
constexpr f32 kButtonRadius = 28.0f;
constexpr f32 kButtonGap = 14.0f;
constexpr f32 kButtonMargin = 8.0f;
constexpr f32 kButtonIconScale = 0.62f; // icon size, as a fraction of the button
constexpr f32 kButtonDisabledAlpha = 0.35f;
constexpr f32 kButtonLabelGap = 10.0f; // between a button and its label
// How far outside a button the reticle still counts as on it -- a little
// aim assist, since a stick can't place the reticle precisely.
constexpr f32 kButtonHoverSlop = 14.0f;
// Toolbar slide in/out: seconds for one button's slide, and the delay
// between neighbouring buttons (top first).
constexpr f32 kToolbarSlideTime = 0.22f;
constexpr f32 kToolbarSlideStagger = 0.05f;
// Top bar sub-row: gap below the top row, and its drop-down animation.
constexpr f32 kMenuRowGap = 8.0f;
constexpr f32 kMenuDropTime = 0.18f;
constexpr f32 kMenuDropStagger = 0.04f;

// Coat mode.
constexpr f32 kCoatFullTime = 1.5f; // seconds of R2 to go from 0 to 100%
constexpr glm::vec3 kPlainColour{0.38f, 0.38f, 0.38f};
constexpr i32 kPlainSlot = 2;     // material slot every uncoated object uses
// A glowing object's light: intensity at full glow, per unit of object
// radius squared (point lights fall off with 1/distance^2 from its centre).
constexpr f32 kGlowLightIntensity = 5.0f;
constexpr i32 kMaxCoatSlot = 999; // the engine has 1000 material slots
constexpr int kCoatColourCount = 10;
constexpr int kCoatShadeCount = 5; // darkest .. lightest; the base is the middle
constexpr const char *kCoatColourNames[kCoatColourCount] = {
    "Red", "Orange", "Yellow", "Green", "Teal",
    "Blue", "Purple", "Pink", "Brown", "Grey"};
constexpr glm::vec3 kCoatColours[kCoatColourCount] = {
    {0.84f, 0.18f, 0.18f}, {0.94f, 0.50f, 0.10f}, {0.95f, 0.82f, 0.14f},
    {0.25f, 0.68f, 0.29f}, {0.12f, 0.64f, 0.64f}, {0.18f, 0.42f, 0.84f},
    {0.49f, 0.25f, 0.77f}, {0.91f, 0.35f, 0.65f}, {0.54f, 0.35f, 0.21f},
    {0.55f, 0.55f, 0.55f}};
constexpr const char *kShadeNames[kCoatShadeCount] = {
    "Darker", "Dark", "Base", "Light", "Lighter"};
// The finishes: same values and order as the Qt mode's presets (and
// icons/make_coat_icons.py, which draws their preview spheres).
struct CoatFinish {
  const char *name;
  f32 roughness;
  f32 metallic;
  bool waxy;
};
constexpr CoatFinish kCoatFinishes[] = {
    {"Default", 0.6f, 0.0f, false},   {"Plastic", 0.35f, 0.0f, false},
    {"Shiny", 0.08f, 0.0f, false},    {"Rough", 0.9f, 0.0f, false},
    {"Metal", 0.35f, 1.0f, false},    {"Wax", 0.6f, 0.0f, true},
    {"Shiny Metal", 0.08f, 1.0f, false}, {"Shiny Wax", 0.08f, 0.0f, true},
    {"Rough Metal", 0.9f, 1.0f, false},  {"Rough Wax", 0.9f, 0.0f, true},
    {"Clay", 0.78f, 0.0f, true}};
constexpr int kCoatFinishCount =
    static_cast<int>(sizeof(kCoatFinishes) / sizeof(kCoatFinishes[0]));

// Colour `index` at shade `shade` (0 = darkest, 2 = the colour itself).
glm::vec3 coat_shade(int index, int shade) {
  const glm::vec3 base = kCoatColours[index];
  constexpr f32 kMix[kCoatShadeCount] = {-0.6f, -0.3f, 0.0f, 0.35f, 0.65f};
  const f32 m = kMix[shade];
  return m < 0.0f ? glm::mix(base, glm::vec3(0.0f), -m)
                  : glm::mix(base, glm::vec3(1.0f), m);
}

glm::vec3 rgb_to_hsv(glm::vec3 c) {
  const f32 mx = std::max({c.r, c.g, c.b});
  const f32 mn = std::min({c.r, c.g, c.b});
  const f32 d = mx - mn;
  f32 h = 0.0f;
  if (d > 1e-6f) {
    if (mx == c.r) {
      h = std::fmod((c.g - c.b) / d, 6.0f);
    } else if (mx == c.g) {
      h = (c.b - c.r) / d + 2.0f;
    } else {
      h = (c.r - c.g) / d + 4.0f;
    }
    h /= 6.0f;
    if (h < 0.0f) {
      h += 1.0f;
    }
  }
  return {h, mx > 1e-6f ? d / mx : 0.0f, mx};
}

glm::vec3 hsv_to_rgb(glm::vec3 hsv) {
  const f32 h = hsv.x * 6.0f;
  const int i = static_cast<int>(std::floor(h)) % 6;
  const f32 f = h - std::floor(h);
  const f32 v = hsv.z, s = hsv.y;
  const f32 p = v * (1 - s), q = v * (1 - f * s), t = v * (1 - (1 - f) * s);
  switch (i) {
  case 0: return {v, t, p};
  case 1: return {q, v, p};
  case 2: return {p, v, t};
  case 3: return {p, q, v};
  case 4: return {t, p, v};
  default: return {v, p, q};
  }
}

f32 move_toward(f32 from, f32 to, f32 step) {
  return from < to ? std::min(from + step, to) : std::max(from - step, to);
}
glm::vec3 move_toward(glm::vec3 from, glm::vec3 to, f32 step) {
  return {move_toward(from.x, to.x, step), move_toward(from.y, to.y, step),
          move_toward(from.z, to.z, step)};
}

// Dotted outline drawn around each selected object.
constexpr glm::vec4 kSelectionOutline{1.0f, 0.55f, 0.1f, 1.0f};

// Tooltip layout (pixels). The engine's font is baked at 28px; text is
// drawn scaled down from that, and its width estimated from an average
// glyph width since there's no text-measuring API.
constexpr f32 kTipFontScale = 0.68f;
constexpr f32 kTipFontPixels = 28.0f * kTipFontScale;
constexpr f32 kTipAverageGlyphWidth = 0.58f * kTipFontPixels;
constexpr f32 kTipIconSize = 28.0f;
constexpr f32 kTipRowHeight = 34.0f;
constexpr f32 kTipIconGap = 4.0f;  // between the icons of a combo
constexpr f32 kTipTextGap = 10.0f; // between the icons and the word
constexpr f32 kTipOffset = 24.0f;  // from the reticle centre
constexpr glm::vec4 kTipShadow{0.0f, 0.0f, 0.0f, 0.85f};
constexpr f32 kTipShadowOffset = 1.5f;
constexpr size_t kTipMaxRows = 4; // per column; extra rows go left
// Seconds for the tooltip to fade fully in / out.
constexpr f32 kTipFadeInTime = 0.15f;
constexpr f32 kTipFadeOutTime = 0.25f;
constexpr glm::vec4 kTipText{1.0f, 1.0f, 1.0f, 1.0f};
constexpr glm::vec4 kTipPlus{0.75f, 0.78f, 0.82f, 1.0f};

// One tooltip row: up to two button icons (a combo, drawn "A + B") and
// the word for what it does.
struct TipRow {
  const char *icons[2];
  std::string word;
};

f32 apply_deadzone(f32 value) {
  const f32 magnitude = std::fabs(value);
  if (magnitude < kStickDeadzone) {
    return 0.0f;
  }
  return std::copysign((magnitude - kStickDeadzone) / (1.0f - kStickDeadzone),
                       value);
}

f32 stick_x(GamepadAxis axis) {
  return apply_deadzone(input::gamepad_axis(axis));
}

// A stick's Y axis as "positive = pushed up". The platform layers disagree
// on the raw sign: Linux's joystick API reports up as negative, Windows'
// XInput reports it as positive.
f32 stick_up(GamepadAxis axis) {
#if KPLATFORM_WINDOWS
  return apply_deadzone(input::gamepad_axis(axis));
#else
  return -apply_deadzone(input::gamepad_axis(axis));
#endif
}

// The shortest rotation taking unit vector `from` onto unit vector `to`.
// Only used for small per-frame tilts, never for opposite vectors.
glm::quat rotation_between(glm::vec3 from, glm::vec3 to) {
  return glm::normalize(glm::quat(1.0f + glm::dot(from, to), glm::cross(from, to)));
}

bool held(GamepadButton button) {
  return input::is_gamepad_button_down(button);
}

// True on the frame the button goes down.
bool pressed(GamepadButton button) {
  return input::is_gamepad_button_down(button) &&
         !input::was_gamepad_button_down(button);
}

// A trigger read as a button, with hysteresis.
struct TriggerButton {
  GamepadAxis axis;
  bool down = false;

  // Returns true on the frame it goes down.
  bool update() {
    const f32 value = input::gamepad_axis(axis);
    const bool was_down = down;
    down = was_down ? value > kTriggerReleased : value > kTriggerPressed;
    return down && !was_down;
  }
};

class ControllerSculptGame : public Game {
public:
  explicit ControllerSculptGame(std::string stamps_path)
      : stamps_path_(std::move(stamps_path)) {
    app_config.start_pos_x = 100;
    app_config.start_pos_y = 100;
    app_config.start_width = 1280;
    app_config.start_height = 800;
    app_config.name = "Modelling Software v2 (Controller)";
  }

  b8 initialize() override {
    sculpt_shared::setup_sculpt_environment();
    // Same starting view as the Qt mode's SceneViewport.
    camera_.set_position(glm::vec3(0.0f, 0.0f, -3.0f));
    on_resize(static_cast<u32>(app_config.start_width),
              static_cast<u32>(app_config.start_height));
    if (!stamps_path_.empty()) {
      load_stamps(stamps_path_);
    }
    make_everything_plain();
    adopt_loaded_looseness();
    committed_ = capture_scene(); // the state undo returns to first
    return true;
  }

  b8 update(f32 dt) override {
    const bool r1 = held(GamepadButton::RightBumper);
    const bool l1 = held(GamepadButton::LeftBumper);
    const bool r2_pressed = r2_.update();
    const bool l2_pressed = l2_.update();

    // L1 + R1: frame, once per press of the combination.
    const bool frame_combo = l1 && r1;
    if (frame_combo && !frame_combo_was_held_) {
      begin_frame();
    }
    frame_combo_was_held_ = frame_combo;

    update_recenter_hold(dt);
    if (pressed(GamepadButton::LeftStick)) {
      begin_flip();
    }
    update_flip(dt);
    // In coat mode R2 coats instead of grabbing (see update_coat()).
    update_manipulation_state(mode_ != EditMode::Default ? false : r2_pressed,
                              l2_pressed, l1);
    update_toolbar_slide(dt);
    update_menu(dt);
    hovered_button_ = button_under_reticle();
    hovered_top_ = hovered_button_ < 0 ? top_bar_under_reticle() : -1;
    if (pressed(GamepadButton::A)) {
      if (hovered_button_ >= 0) {
        press_button(static_cast<ToolbarButton>(hovered_button_));
      } else if (hovered_top_ >= 0) {
        press_top_bar(hovered_top_);
      } else {
        toggle_selection();
      }
    }
    if (pressed(GamepadButton::B) && l1 && mode_ != EditMode::Default) {
      exit_mode(); // L1 + B: leave the mode
    } else if (pressed(GamepadButton::B)) {
      if (shades_open_) {
        shades_open_ = false; // hide the shades row first
      } else if (scoped_group_ >= 0) {
        scope_out();
      }
    }
    update_coat(dt);
    update_style(dt);
    update_effects(dt);
    sync_glow_lights();
    if (pressed(GamepadButton::Y) && mode_ != EditMode::Default) {
      toggle_coat_subtract();
    } else if (pressed(GamepadButton::Y)) {
      if (grabbed_object_ >= 0 || rotating_object_ >= 0) {
        snap_object();
      } else {
        delete_under_reticle();
      }
    }

    if (chain_.active && grabbed_object_ >= 0) {
      if (pressed(GamepadButton::DpadLeft)) {
        step_chain(false);
      }
      if (pressed(GamepadButton::DpadRight)) {
        step_chain(true);
      }
    } else if (grabbed_object_ < 0 && rotating_object_ < 0 && !r2_.down) {
      // Otherwise D-pad left / right: undo / redo.
      if (pressed(GamepadButton::DpadLeft)) {
        undo();
      } else if (pressed(GamepadButton::DpadRight)) {
        redo();
      }
    }
    commit_history_if_idle();

    if (framing_) {
      update_framing(dt);
    } else if (recentering_) {
      update_recenter(dt);
    } else if (grabbed_object_ >= 0) {
      update_grab(dt);
    } else if (rotating_object_ >= 0) {
      update_rotate(dt);
    } else if (l1 && r1) {
      // The framing combo -- the sticks do nothing while it's held.
    } else if (r1) {
      update_orbit(dt);
    } else if (l1) {
      update_strafe_mode(dt);
      update_look(dt);
    } else {
      update_move_mode(dt);
      update_look(dt);
    }

    update_chain(); // after this frame's moves of the held copy
    renderer_set_camera(camera_);
    return true;
  }

  b8 render(f32 dt) override {
    hovered_object_ =
        (grabbed_object_ < 0 && rotating_object_ < 0 && hovered_button_ < 0 &&
         hovered_top_ < 0)
            ? pick_object_under_reticle()
            : -1;
    draw_selection();
    draw_toolbar();
    draw_top_bar();
    draw_reticle();
    draw_tooltip(dt);
    return true;
  }

  void on_resize(u32 width, u32 height) override {
    if (width > 0 && height > 0) {
      screen_width_ = static_cast<f32>(width);
      screen_height_ = static_cast<f32>(height);
    }
  }

private:
  // --- Loading ---------------------------------------------------------------

  void load_stamps(const std::string &path) {
    SculptDocument document;
    if (!document.load(path)) {
      KERROR("Could not read stamps file '{}'.", path);
      return;
    }
    sculpt_shared::replay_document(document);
    for (const SculptStampRecord &stamp : document.stamps()) {
      if (stamp.looseness > 0.0f) {
        f32 &loose = loaded_looseness_[stamp.object_id];
        loose = std::max(loose, stamp.looseness);
      }
    }
    KINFO("Loaded {} stamp(s) from '{}'.", document.stamps().size(), path);
  }

  // A file's per-stamp looseness becomes its object's style-mode looseness
  // (and the stamps go back to 0): the shader draws the larger of the two,
  // so leaving it on the stamps would stop Subtract going below it.
  void adopt_loaded_looseness() {
    for (const auto &[object, looseness] : loaded_looseness_) {
      ObjectCoat *coat = own_coat(object);
      if (!coat) {
        continue;
      }
      coat->looseness = std::max(coat->looseness, looseness);
      apply_coat_material(*coat);
      renderer_sculpt_set_object_looseness(&object, 1, 0.0f);
    }
    loaded_looseness_.clear();
  }

  // --- The brush point -------------------------------------------------------
  //
  // brush_ is in camera space, in on-screen terms: x = right, y = up on
  // screen, z = depth along forward. On screen, a point at depth z appears
  // at (x / z, y / z) in units of screen height from the centre -- the same
  // projection the raymarch shader uses -- so the visible half-screen is
  // 0.5 * aspect wide and 0.5 tall.

  f32 aspect() const { return screen_width_ / screen_height_; }

  glm::vec3 screen_up() const { return -camera_.up(); }

  glm::vec3 brush_world() const {
    return camera_.position() + camera_.right() * brush_.x +
           screen_up() * brush_.y + camera_.forward() * brush_.z;
  }

  // Inverse of brush_world(): a world point in camera space (the camera's
  // right/up/forward are orthonormal).
  glm::vec3 to_camera_space(glm::vec3 world) const {
    const glm::vec3 d = world - camera_.position();
    return glm::vec3(glm::dot(d, camera_.right()), glm::dot(d, screen_up()),
                     glm::dot(d, camera_.forward()));
  }

  // Moves the brush sideways by `delta` (camera-space units along `axis`,
  // 0 = x, 1 = y), clamped to the screen edge; whatever would have gone
  // past the edge moves the camera instead, so the brush stays pinned at
  // the edge while the view strafes.
  void move_brush_across(int axis, f32 delta) {
    const f32 half_extent = (axis == 0 ? 0.5f * aspect() : 0.5f) * kScreenEdge;
    const f32 limit = half_extent * brush_.z;
    const f32 wanted = brush_[axis] + delta;
    const f32 clamped = std::clamp(wanted, -limit, limit);
    brush_[axis] = clamped;
    const f32 overflow = wanted - clamped;
    if (overflow != 0.0f) {
      const glm::vec3 direction = axis == 0 ? camera_.right() : screen_up();
      camera_.move(direction * overflow);
    }
  }

  // --- Camera modes ----------------------------------------------------------

  // Left stick: left/right moves the brush (X); up/down moves the camera
  // -- and the brush with it, since the brush lives in camera space --
  // forward/back along the view direction (Z).
  void update_move_mode(f32 dt) {
    const f32 x = stick_x(GamepadAxis::LeftX);
    const f32 z = stick_up(GamepadAxis::LeftY);
    if (x != 0.0f) {
      move_brush_across(0, x * kBrushScreenSpeed * brush_.z * dt);
    }
    if (z != 0.0f) {
      camera_.move(camera_.forward() * (z * kForwardSpeed * dt));
    }
  }

  // R1 + left stick: brush X and Y.
  void update_strafe_mode(f32 dt) {
    const f32 x = stick_x(GamepadAxis::LeftX);
    const f32 y = stick_up(GamepadAxis::LeftY);
    if (x != 0.0f) {
      move_brush_across(0, x * kBrushScreenSpeed * brush_.z * dt);
    }
    if (y != 0.0f) {
      move_brush_across(1, y * kBrushScreenSpeed * brush_.z * dt);
    }
  }

  // Right stick: look. A positive yaw turns forward toward +X, which is
  // screen-right, so stick right = positive yaw. Up looks up, which in this
  // engine is a NEGATIVE pitch (see the orientation note above).
  void update_look(f32 dt) {
    const f32 x = stick_x(GamepadAxis::RightX);
    const f32 y = stick_up(GamepadAxis::RightY);
    if (x != 0.0f) {
      camera_.yaw(x * kLookRate * dt);
    }
    if (y != 0.0f) {
      camera_.pitch(-y * kLookRate * dt);
    }
  }

  // R1 held: orbit the camera around the brush point. Left/right on either
  // stick swings around the vertical axis; right stick up/down tilts over
  // the top / underneath; left stick up/down zooms toward / away from the
  // point. The camera's offset from the pivot is rotated by exactly the
  // rotation applied to its view direction, so the brush stays put in the
  // world AND at the same spot on screen.
  void update_orbit(f32 dt) {
    const f32 swing = std::clamp(stick_x(GamepadAxis::LeftX) +
                                     stick_x(GamepadAxis::RightX),
                                 -1.0f, 1.0f);
    const f32 tilt = stick_up(GamepadAxis::RightY);
    const f32 zoom = stick_up(GamepadAxis::LeftY);
    if (swing == 0.0f && tilt == 0.0f && zoom == 0.0f) {
      return;
    }
    const glm::vec3 pivot = brush_world();
    glm::vec3 offset = camera_.position() - pivot;

    // Swing: around the world vertical axis. Camera yaw +a is a rotation of
    // forward by +a about world +Y (see Camera::forward()), so rotating the
    // offset the same way keeps the pivot centred. Stick right swings the
    // camera to the right around the pivot, which turns it to face left
    // (negative yaw).
    if (swing != 0.0f) {
      const f32 angle = -swing * kOrbitRate * dt;
      offset = glm::angleAxis(angle, glm::vec3(0.0f, 1.0f, 0.0f)) * offset;
      camera_.yaw(angle);
    }

    // Tilt: stick up raises the camera on screen (looking further down at
    // the pivot), which in this engine is a positive pitch. Done as "change
    // pitch, then rotate the offset by whatever rotation took the old
    // forward to the new one" so the clamp and the sign conventions can't
    // disagree.
    if (tilt != 0.0f) {
      const glm::vec3 old_forward = camera_.forward();
      const f32 target =
          std::clamp(camera_.pitch() + tilt * kOrbitRate * dt, -kMaxOrbitPitch,
                     kMaxOrbitPitch);
      camera_.pitch(target - camera_.pitch());
      offset = rotation_between(old_forward, camera_.forward()) * offset;
    }

    // Zoom: stick up moves in. Never closer than the brush's minimum depth.
    if (zoom != 0.0f) {
      const f32 distance = glm::length(offset);
      const f32 wanted = distance * (1.0f - zoom * kZoomRate * dt);
      if (distance > 0.0f) {
        offset *= std::max(wanted, kMinBrushDepth) / distance;
      }
    }

    camera_.set_position(pivot + offset);
    // The pivot is fixed in the world; re-express it in the new camera
    // frame (zooming changes its depth).
    brush_ = to_camera_space(pivot);
  }

  // --- L1 + R1 framing -------------------------------------------------------

  // Glides the camera, keeping its view direction, to where the object
  // under the reticle (or, with none, the whole sculpt) fills the view, and
  // puts the brush at its centre. With nothing stamped it frames the
  // origin, where a new sculpt starts.
  void begin_frame() {
    glm::vec3 center(0.0f);
    f32 distance = kDefaultBrushDepth;
    glm::vec3 lo, hi;
    const i32 object = pick_object_under_reticle();
    const bool have_bounds = object >= 0
                                 ? renderer_sculpt_object_bounds(object, &lo, &hi)
                                 : renderer_sculpt_bounds(&lo, &hi);
    if (have_bounds) {
      center = 0.5f * (lo + hi);
      const f32 radius = 0.5f * glm::length(hi - lo);
      distance = std::max(radius * kFrameDistanceScale, kMinFrameDistance);
    }
    distance = std::clamp(distance, kMinBrushDepth, kMaxBrushDepth);
    frame_from_ = camera_.position();
    frame_to_ = center - camera_.forward() * distance;
    frame_from_brush_ = brush_;
    frame_to_brush_ = glm::vec3(0.0f, 0.0f, distance);
    frame_time_ = 0.0f;
    framing_ = true;
    KINFO("Framing {}.", object >= 0 ? "object " + std::to_string(object)
                                     : std::string("the whole sculpt"));
  }

  void update_framing(f32 dt) {
    frame_time_ += dt;
    const f32 t = std::min(frame_time_ / kFrameDuration, 1.0f);
    const f32 eased = t * t * (3.0f - 2.0f * t); // smoothstep
    camera_.set_position(glm::mix(frame_from_, frame_to_, eased));
    brush_ = glm::mix(frame_from_brush_, frame_to_brush_, eased);
    if (t >= 1.0f) {
      framing_ = false;
    }
  }

  // --- Start: recenter the reticle -------------------------------------------

  // Fires once per hold, after kRecenterHoldTime. Ignored while holding an
  // object, since the reticle is riding on it then.
  void update_recenter_hold(f32 dt) {
    if (!held(GamepadButton::Start)) {
      start_held_time_ = 0.0f;
      recenter_fired_ = false;
      return;
    }
    start_held_time_ += dt;
    if (!recenter_fired_ && start_held_time_ >= kRecenterHoldTime &&
        grabbed_object_ < 0 && rotating_object_ < 0) {
      recenter_fired_ = true;
      recenter_from_ = brush_;
      recenter_time_ = 0.0f;
      recentering_ = true;
      KINFO("Recentering the reticle.");
    }
  }

  // Glides the brush back to the middle of the screen at the default depth.
  void update_recenter(f32 dt) {
    recenter_time_ += dt;
    const f32 t = std::min(recenter_time_ / kRecenterDuration, 1.0f);
    const f32 eased = t * t * (3.0f - 2.0f * t); // smoothstep
    brush_ = glm::mix(recenter_from_,
                      glm::vec3(0.0f, 0.0f, kDefaultBrushDepth), eased);
    if (t >= 1.0f) {
      recentering_ = false;
    }
  }

  // --- L3: flip ----------------------------------------------------------------

  // Starts a 90-degree tip of the held object (or the one under the
  // reticle) toward the direction the left stick is pushed. The axis is
  // fixed now, in the camera's current frame, so moving the camera mid-flip
  // doesn't bend it. Ignored while a flip is already running, or with the
  // stick near centre (no direction to flip in).
  void begin_flip() {
    if (flip_object_ >= 0) {
      return;
    }
    const f32 x = stick_x(GamepadAxis::LeftX);
    const f32 up = stick_up(GamepadAxis::LeftY);
    if (std::max(std::fabs(x), std::fabs(up)) < kFlipStickThreshold) {
      return;
    }
    const i32 object = grabbed_object_ >= 0    ? grabbed_object_
                       : rotating_object_ >= 0 ? rotating_object_
                                               : pick_object_under_reticle();
    if (object < 0) {
      KINFO("Flip: nothing under the reticle.");
      return;
    }
    // Tipping toward a direction turns the object about the axis lying in
    // the screen plane perpendicular to it: up/down about screen-right,
    // left/right about the view direction, with the top of the object
    // moving the way the stick points (up = away from you). Signs, worked
    // through with Camera::up() pointing DOWN on screen (see the
    // orientation note at the top): +90 degrees about right() carries the
    // screen's bottom away from you, so "top away" is the negative turn;
    // +90 about forward() carries screen-right down -- clockwise on screen
    // -- which is "top to the right".
    if (std::fabs(up) >= std::fabs(x)) {
      flip_axis_ = camera_.right() * (up > 0.0f ? -1.0f : 1.0f);
    } else {
      flip_axis_ = camera_.forward() * (x > 0.0f ? 1.0f : -1.0f);
    }
    flip_object_ = object;
    flip_group_ = held_group_for(object);
    flip_pivot_ = group_center(flip_group_);
    flip_remaining_ = kFlipAngle;
    KINFO("Flipping object {}{}.", object, group_note(flip_group_));
  }

  void update_flip(f32 dt) {
    if (flip_object_ < 0) {
      return;
    }
    const f32 step =
        std::min(flip_remaining_, kFlipAngle * dt / kFlipDuration);
    flip_remaining_ -= step;
    const bool held_object =
        flip_object_ == grabbed_object_ || flip_object_ == rotating_object_;
    const glm::vec3 brush_before = brush_;
    move_group(flip_group_, flip_pivot_, glm::angleAxis(step, flip_axis_),
               glm::vec3(0.0f));
    if (!held_object) {
      // Not holding it: the reticle shouldn't swing round with it.
      brush_ = brush_before;
    }
    if (flip_remaining_ <= 0.0f) {
      flip_object_ = -1;
    }
  }

  // --- Objects ---------------------------------------------------------------

  // The object hit by the line of sight through the reticle, or -1. When
  // something is hit and out_hit is given, it receives the hit point.
  i32 pick_object_under_reticle(glm::vec3 *out_hit = nullptr) const {
    const glm::vec3 origin = camera_.position();
    const glm::vec3 direction = glm::normalize(brush_world() - origin);
    f32 t = 0.0f;
    // Scoped in: only the scoped group's members can be picked.
    const Group *scoped = scoped_group();
    const i32 object =
        scoped ? renderer_sculpt_raycast_objects(
                     origin, direction, kPickMaxDistance, scoped->members.data(),
                     static_cast<i32>(scoped->members.size()), &t)
               : renderer_sculpt_raycast_object(origin, direction,
                                                kPickMaxDistance, &t);
    if (object >= 0 && out_hit) {
      *out_hit = origin + direction * t;
    }
    return object;
  }

  // Starts/stops a grab (R2) or rotation (L2). Pressing either picks the
  // object under the reticle; with none there, the press does nothing and
  // the sticks keep driving the camera. Grab wins if both are held.
  void update_manipulation_state(bool r2_pressed, bool l2_pressed, bool l1) {
    if (!r2_.down && grabbed_object_ >= 0) {
      KINFO("Released object {}.", grabbed_object_);
      grabbed_object_ = -1;
      held_group_.clear();
      chain_ = CloneChain{}; // the links stay where they are
    }
    if (!l2_.down && rotating_object_ >= 0) {
      KINFO("Stopped rotating object {}.", rotating_object_);
      rotating_object_ = -1;
      held_group_.clear();
    }
    if (r2_pressed && grabbed_object_ < 0) {
      glm::vec3 hit;
      grabbed_object_ = pick_object_under_reticle(&hit);
      if (grabbed_object_ >= 0) {
        // L1 + R2: grab a fresh copy instead, left exactly on top of the
        // original until it's moved.
        held_group_ = group_for(grabbed_object_);
        if (l1) {
          clone_held_group();
        }
        if (grabbed_object_ >= 0) {
          KINFO("Grabbed object {}{}.", grabbed_object_,
                group_note(held_group_));
          rotating_object_ = -1;
          object_center_ = group_center(held_group_);
          // Put the brush on the surface that was hit, so the reticle
          // rides on the object while it's held.
          brush_ = to_camera_space(hit);
        }
      } else {
        KINFO("Grab: nothing under the reticle.");
      }
    }
    // Style and effects modes have no rotate: L2 does nothing there.
    if (l2_pressed && rotating_object_ < 0 && grabbed_object_ < 0 &&
        mode_ != EditMode::Style && mode_ != EditMode::Effects) {
      glm::vec3 hit;
      rotating_object_ = pick_object_under_reticle(&hit);
      if (rotating_object_ >= 0) {
        held_group_ = group_for(rotating_object_);
        KINFO("Rotating object {}{}.", rotating_object_,
              group_note(held_group_));
        object_center_ = group_center(held_group_);
        brush_ = to_camera_space(hit);
      } else {
        KINFO("Rotate: nothing under the reticle.");
      }
    }
  }

  // --- Selection / groups ----------------------------------------------------

  bool is_selected(i32 object) const {
    return std::find(selection_.begin(), selection_.end(), object) !=
           selection_.end();
  }

  // The objects an action on `object` applies to: the whole selection if
  // it's selected, otherwise its unit (see unit_of()).
  std::vector<i32> group_for(i32 object) const {
    if (is_selected(object)) {
      return selection_;
    }
    return unit_of(object);
  }

  // --- Permanent groups (the Group button) ----------------------------------

  struct Group {
    i32 id;
    std::vector<i32> members;
  };

  Group *find_group(i32 group_id) {
    for (Group &group : groups_) {
      if (group.id == group_id) {
        return &group;
      }
    }
    return nullptr;
  }
  const Group *find_group(i32 group_id) const {
    return const_cast<ControllerSculptGame *>(this)->find_group(group_id);
  }

  const Group *group_of(i32 object) const {
    for (const Group &group : groups_) {
      if (std::find(group.members.begin(), group.members.end(), object) !=
          group.members.end()) {
        return &group;
      }
    }
    return nullptr;
  }

  const Group *scoped_group() const { return find_group(scoped_group_); }

  // What selecting or grabbing `object` takes: its whole group, except when
  // scoped in, where members are picked one at a time.
  std::vector<i32> unit_of(i32 object) const {
    if (scoped_group_ < 0) {
      if (const Group *group = group_of(object)) {
        return group->members;
      }
    }
    return {object};
  }

  static bool same_set(std::vector<i32> a, std::vector<i32> b) {
    std::sort(a.begin(), a.end());
    std::sort(b.begin(), b.end());
    return a == b;
  }

  // The group that is exactly the selection, if there is one.
  const Group *selected_group() const {
    if (selection_.empty()) {
      return nullptr;
    }
    const Group *group = group_of(selection_.front());
    return group && same_set(group->members, selection_) ? group : nullptr;
  }

  // Forgets deleted objects; a group left with fewer than two members
  // isn't a group any more.
  void remove_from_groups(const std::vector<i32> &objects) {
    for (Group &group : groups_) {
      std::erase_if(group.members, [&](i32 id) {
        return std::find(objects.begin(), objects.end(), id) != objects.end();
      });
    }
    if (const Group *scoped = scoped_group(); scoped && scoped->members.empty()) {
      scope_out();
    }
    std::erase_if(groups_, [&](const Group &group) {
      return group.members.size() < 2 && group.id != scoped_group_;
    });
  }

  // --- Toolbar ---------------------------------------------------------------

  enum class ToolbarButton { Group, Scope, Delete, Placeholder, Count };
  static constexpr int kButtonCount = static_cast<int>(ToolbarButton::Count);

  // Button centres, top to bottom: a column at the right edge of the area
  // the reticle can reach, centred vertically.
  glm::vec2 button_center(int index) const {
    const f32 reach_x = 0.5f * kScreenEdge * screen_width_;
    const f32 step = 2.0f * kButtonRadius + kButtonGap;
    const f32 first_y = 0.5f * screen_height_ -
                        0.5f * step * static_cast<f32>(kButtonCount - 1);
    const f32 home_x =
        0.5f * screen_width_ + reach_x - kButtonMargin - kButtonRadius;
    // Slid out: fully past the right edge of the screen.
    const f32 hidden_x = screen_width_ + kButtonRadius + 2.0f;
    const f32 shown = button_shown(index);
    const f32 eased = 1.0f - (1.0f - shown) * (1.0f - shown) * (1.0f - shown);
    return glm::vec2(glm::mix(hidden_x, home_x, eased),
                     first_y + step * static_cast<f32>(index));
  }

  bool toolbar_wanted() const {
    return !selection_.empty() || scoped_group_ >= 0;
  }

  // How far one button is slid in, 0..1: the column's slide progress,
  // staggered so the buttons follow one another.
  f32 button_shown(int index) const {
    const f32 total = kToolbarSlideTime +
                      kToolbarSlideStagger * static_cast<f32>(kButtonCount - 1);
    const f32 t = toolbar_slide_ * total -
                  kToolbarSlideStagger * static_cast<f32>(index);
    return std::clamp(t / kToolbarSlideTime, 0.0f, 1.0f);
  }

  void update_toolbar_slide(f32 dt) {
    const f32 total = kToolbarSlideTime +
                      kToolbarSlideStagger * static_cast<f32>(kButtonCount - 1);
    const f32 step = dt / total;
    toolbar_slide_ = toolbar_wanted() ? std::min(toolbar_slide_ + step, 1.0f)
                                      : std::max(toolbar_slide_ - step, 0.0f);
  }

  int button_under_reticle() const {
    if (brush_.z <= 0.01f) {
      return -1;
    }
    const glm::vec2 reticle = reticle_screen();
    for (int i = 0; i < kButtonCount; ++i) {
      if (button_shown(i) < 1.0f) {
        continue; // only fully slid-in buttons can be pressed
      }
      if (glm::length(reticle - button_center(i)) <=
          kButtonRadius + kButtonHoverSlop) {
        return i;
      }
    }
    return -1;
  }

  bool button_enabled(ToolbarButton button) const {
    switch (button) {
    case ToolbarButton::Group:
      // Ungroup a selected group, or group 2+ selected objects. Not while
      // scoped in (the group being edited stays as it is).
      return scoped_group_ < 0 &&
             (selected_group() != nullptr || selection_.size() >= 2);
    case ToolbarButton::Scope:
      return scoped_group_ >= 0 || selected_group() != nullptr;
    case ToolbarButton::Delete:
      return !selection_.empty();
    default:
      return false;
    }
  }

  bool button_active(ToolbarButton button) const {
    switch (button) {
    case ToolbarButton::Group:
      return scoped_group_ < 0 && selected_group() != nullptr;
    case ToolbarButton::Scope:
      return scoped_group_ >= 0;
    default:
      return false;
    }
  }

  const char *button_label(ToolbarButton button) const {
    switch (button) {
    case ToolbarButton::Group:
      return button_active(button) ? "Ungroup" : "Group";
    case ToolbarButton::Scope:
      return scoped_group_ >= 0 ? "Scope out" : "Scope in";
    case ToolbarButton::Delete:
      return "Delete";
    default:
      return "Coming soon";
    }
  }

  static const char *button_icon(ToolbarButton button) {
    switch (button) {
    case ToolbarButton::Group:
      return "ui/toolbar/group.png";
    case ToolbarButton::Scope:
      return "ui/toolbar/scope.png";
    case ToolbarButton::Delete:
      return "ui/toolbar/delete.png";
    default:
      return "ui/toolbar/dummy.png";
    }
  }

  void press_button(ToolbarButton button) {
    if (grabbed_object_ >= 0 || rotating_object_ >= 0) {
      return;
    }
    switch (button) {
    case ToolbarButton::Group:
      toggle_group();
      break;
    case ToolbarButton::Scope:
      if (scoped_group_ >= 0) {
        scope_out();
      } else {
        scope_in();
      }
      break;
    case ToolbarButton::Delete:
      delete_selection();
      break;
    default:
      KINFO("That button doesn't do anything yet.");
      break;
    }
  }

  // Group button: ungroups a selected group, otherwise makes the selection
  // one group (absorbing any groups inside it).
  void toggle_group() {
    if (scoped_group_ >= 0) {
      return;
    }
    history_dirty_ = true;
    if (const Group *group = selected_group()) {
      const i32 id = group->id;
      std::erase_if(groups_, [id](const Group &g) { return g.id == id; });
      KINFO("Ungrouped group {}.", id);
      return;
    }
    if (selection_.size() < 2) {
      KINFO("Group: select at least two objects first.");
      return;
    }
    std::erase_if(groups_, [&](const Group &g) {
      return std::all_of(g.members.begin(), g.members.end(),
                         [&](i32 id) { return is_selected(id); });
    });
    groups_.push_back({next_group_id_++, selection_});
    KINFO("Grouped {} objects as group {}.", selection_.size(),
          groups_.back().id);
  }

  void scope_in() {
    const Group *group = selected_group();
    if (!group) {
      KINFO("Scope in: select a group first.");
      return;
    }
    scoped_group_ = group->id;
    selection_.clear(); // members are picked one at a time inside
    KINFO("Scoped into group {}.", scoped_group_);
  }

  void scope_out() {
    const Group *group = scoped_group();
    KINFO("Scoped out of group {}.", scoped_group_);
    scoped_group_ = -1;
    // Leave the group selected, as it was when scoping in.
    selection_ = group && group->members.size() >= 2 ? group->members
                                                     : std::vector<i32>{};
    std::erase_if(groups_,
                  [](const Group &g) { return g.members.size() < 2; });
  }

  // group_for(), except that the object currently held (R2/L2) brings its
  // already-chosen group -- which may be fresh clones not yet selected.
  std::vector<i32> held_group_for(i32 object) const {
    if (!held_group_.empty() &&
        (object == grabbed_object_ || object == rotating_object_)) {
      return held_group_;
    }
    return group_for(object);
  }

  static std::string group_note(const std::vector<i32> &group) {
    return group.size() > 1
               ? " (group of " + std::to_string(group.size()) + ")"
               : std::string();
  }

  // Cross / A: toggles the object under the reticle in the selection.
  // Ignored while holding something, since the reticle is riding on it.
  void toggle_selection() {
    if (grabbed_object_ >= 0 || rotating_object_ >= 0) {
      return;
    }
    const i32 object = pick_object_under_reticle();
    if (object < 0) {
      KINFO("Select: nothing under the reticle.");
      return;
    }
    // A grouped object brings its whole group (see unit_of()).
    const std::vector<i32> unit = unit_of(object);
    if (is_selected(object)) {
      std::erase_if(selection_, [&](i32 id) {
        return std::find(unit.begin(), unit.end(), id) != unit.end();
      });
      KINFO("Deselected object {}{} ({} selected).", object, group_note(unit),
            selection_.size());
    } else {
      for (i32 id : unit) {
        if (!is_selected(id)) {
          selection_.push_back(id);
        }
      }
      KINFO("Selected object {}{} ({} selected).", object, group_note(unit),
            selection_.size());
    }
  }

  // Deletes `objects` and forgets them everywhere (selection, groups, a
  // running flip). Returns false if the engine refused.
  bool delete_objects(const std::vector<i32> &objects) {
    if (flip_object_ >= 0 && std::find(objects.begin(), objects.end(),
                                       flip_object_) != objects.end()) {
      flip_object_ = -1; // don't keep tipping something that's gone
    }
    if (!renderer_sculpt_delete_objects(objects.data(),
                                        static_cast<i32>(objects.size()))) {
      return false;
    }
    std::erase_if(selection_, [&](i32 id) {
      return std::find(objects.begin(), objects.end(), id) != objects.end();
    });
    remove_from_groups(objects);
    forget_coats(objects);
    history_dirty_ = true;
    return true;
  }

  // Toolbar Delete: deletes everything selected.
  void delete_selection() {
    if (selection_.empty()) {
      return;
    }
    const std::vector<i32> objects = selection_;
    if (delete_objects(objects)) {
      KINFO("Deleted {} selected object(s).", objects.size());
    } else {
      KWARN("Delete: the selection could not be deleted.");
    }
  }

  // Triangle / Y when not holding anything: deletes the object under the
  // reticle, or its whole group.
  void delete_under_reticle() {
    const i32 object = pick_object_under_reticle();
    if (object < 0) {
      KINFO("Delete: nothing under the reticle.");
      return;
    }
    const std::vector<i32> group = group_for(object);
    if (!delete_objects(group)) {
      KWARN("Delete: object {} could not be deleted.", object);
      return;
    }
    KINFO("Deleted object {}{}.", object, group_note(group));
  }

  // Copies `originals` (in place, on top of them) and returns the copies,
  // in the same order. Inside a scoped group the copies join it; otherwise
  // each group copied whole gets a matching group of copies.
  std::vector<i32> clone_objects(const std::vector<i32> &originals) {
    history_dirty_ = true;
    std::vector<i32> clones;
    std::vector<std::pair<i32, i32>> copies; // original -> copy
    for (i32 original : originals) {
      const i32 copy = renderer_sculpt_clone_object(original);
      if (copy < 0) {
        continue;
      }
      KINFO("Cloned object {} -> {}.", original, copy);
      clones.push_back(copy);
      copies.emplace_back(original, copy);
      // The copy shares the original's material slot (and so its look);
      // it gets its own the first time it is coated.
      if (auto coat = coats_.find(original); coat != coats_.end()) {
        coats_[copy] = coat->second;
        coats_[copy].light = -1; // gets its own glow light
      }
    }
    if (Group *scoped = find_group(scoped_group_)) {
      scoped->members.insert(scoped->members.end(), clones.begin(),
                             clones.end());
    } else {
      const std::vector<Group> before = groups_;
      for (const Group &group : before) {
        std::vector<i32> copied;
        for (i32 member : group.members) {
          for (const auto &[original, copy] : copies) {
            if (original == member) {
              copied.push_back(copy);
            }
          }
        }
        if (copied.size() == group.members.size()) {
          groups_.push_back({next_group_id_++, copied});
        }
      }
    }
    return clones;
  }

  // L1 + R2: replaces the held group with copies of it (left exactly on
  // top of the originals until moved), and starts a clone chain from the
  // originals to the held copies. If the group was the selection, the
  // selection moves to the copies, so they keep moving together.
  void clone_held_group() {
    const bool was_selection = held_group_.size() > 1 ||
                               is_selected(grabbed_object_);
    const std::vector<i32> originals = held_group_;
    const std::vector<i32> clones = clone_objects(originals);
    i32 primary_clone = -1;
    for (size_t i = 0; i < originals.size() && i < clones.size(); ++i) {
      if (originals[i] == grabbed_object_) {
        primary_clone = clones[i];
      }
    }
    grabbed_object_ = primary_clone;
    held_group_ = clones;
    if (was_selection) {
      selection_ = clones;
    }
    chain_ = CloneChain{};
    chain_.active = primary_clone >= 0;
    chain_.originals = originals;
    chain_.start_center = group_center(originals);
  }

  // --- Clone chain (L1 + R2, then D-pad left/right) --------------------------

  // q raised to a real power: the same axis, k times the angle.
  static glm::quat quat_pow(glm::quat q, f32 k) {
    q = glm::normalize(q);
    if (q.w < 0.0f) {
      q = -q; // the short way round
    }
    const f32 angle = 2.0f * std::acos(std::clamp(q.w, -1.0f, 1.0f));
    const glm::vec3 axis(q.x, q.y, q.z);
    if (angle < 1e-6f || glm::length(axis) < 1e-6f) {
      return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    }
    return glm::angleAxis(angle * k, glm::normalize(axis));
  }

  // D-pad left (Fill) / right (Repeat) while chaining. The two undo each
  // other: Fill first removes copies past the end, one per press, until
  // there are none, and only then adds copies in between; Repeat does the
  // opposite.
  void step_chain(bool repeat) {
    // The kind this press would add, and the kind it removes first.
    const bool remove_beyond = !repeat;
    for (size_t i = chain_.links.size(); i-- > 0;) {
      if (chain_.links[i].beyond == remove_beyond) {
        remove_chain_link(i);
        renumber_chain();
        return;
      }
    }
    add_chain_link(repeat);
  }

  void remove_chain_link(size_t index) {
    const std::vector<i32> objects = chain_.links[index].objects;
    chain_.links.erase(chain_.links.begin() +
                       static_cast<std::ptrdiff_t>(index));
    delete_objects(objects);
  }

  // One more copy in between / past the end. It starts as a plain copy of
  // the originals and is put in its place by update_chain().
  void add_chain_link(bool beyond) {
    CloneChain::Link link;
    link.objects = clone_objects(chain_.originals);
    if (link.objects.empty()) {
      return;
    }
    link.center = chain_.start_center;
    link.beyond = beyond;
    chain_.links.push_back(std::move(link));
    renumber_chain();
  }

  // Chain positions: in-between links at i/(n+1), beyond links at 2, 3, ...
  // (update_chain() moves each link to its new position).
  void renumber_chain() {
    i32 between_count = 0;
    for (const CloneChain::Link &l : chain_.links) {
      between_count += l.beyond ? 0 : 1;
    }
    i32 between_index = 0;
    i32 beyond_index = 0;
    for (CloneChain::Link &l : chain_.links) {
      l.k = l.beyond ? static_cast<f32>(2 + beyond_index++)
                     : static_cast<f32>(++between_index) /
                           static_cast<f32>(between_count + 1);
    }
    KINFO("Chain: {} in between, {} past the end.", between_count,
          beyond_index);
  }

  // Puts every chain link where it belongs for the held copy's current
  // pose: centre start + k * (held - start), rotation and scale k of the
  // way (as powers) from the original's. Only links that moved are
  // re-run.
  void update_chain() {
    if (!chain_.active) {
      return;
    }
    const glm::vec3 step = object_center_ - chain_.start_center;
    for (CloneChain::Link &link : chain_.links) {
      const glm::vec3 center = chain_.start_center + link.k * step;
      const glm::quat rotation = quat_pow(chain_.held_rotation, link.k);
      const f32 scale = std::pow(chain_.held_scale, link.k);
      const glm::quat delta_rotation =
          glm::normalize(rotation * glm::inverse(link.rotation));
      const f32 delta_scale = scale / link.scale;
      const glm::vec3 delta_move = center - link.center;
      if (glm::length(delta_move) < 1e-5f &&
          std::fabs(delta_scale - 1.0f) < 1e-5f &&
          std::fabs(delta_rotation.w) > 1.0f - 1e-7f) {
        continue;
      }
      // About the link's own centre, so its centre only moves by delta_move.
      renderer_sculpt_transform_objects(
          link.objects.data(), static_cast<i32>(link.objects.size()),
          link.center, delta_rotation, delta_move, delta_scale);
      history_dirty_ = true;
      link.center = center;
      link.rotation = rotation;
      link.scale = scale;
    }
  }

  // Centre of the group's combined bounds.
  glm::vec3 group_center(const std::vector<i32> &group) const {
    bool any = false;
    glm::vec3 lo_all(0.0f), hi_all(0.0f);
    for (i32 object : group) {
      glm::vec3 lo, hi;
      if (!renderer_sculpt_object_bounds(object, &lo, &hi)) {
        continue;
      }
      lo_all = any ? glm::min(lo_all, lo) : lo;
      hi_all = any ? glm::max(hi_all, hi) : hi;
      any = true;
    }
    return any ? 0.5f * (lo_all + hi_all) : brush_world();
  }

  // Applies one frame's rigid motion to an object: rotate about `pivot`,
  // then translate. The object's tracked centre and the brush point move
  // with it, so the reticle stays on the object. The engine re-runs the
  // affected region straight away (blended with the rest of the sculpt), so
  // this is called at most once per frame and only when something moved.
  void move_group(const std::vector<i32> &group, glm::vec3 pivot,
                  glm::quat rotation, glm::vec3 translation,
                  f32 scale = 1.0f) {
    if (group.empty() ||
        !renderer_sculpt_transform_objects(group.data(),
                                           static_cast<i32>(group.size()),
                                           pivot, rotation, translation,
                                           scale)) {
      return;
    }
    history_dirty_ = true;
    auto apply = [&](glm::vec3 p) {
      return pivot + scale * (rotation * (p - pivot)) + translation;
    };
    object_center_ = apply(object_center_);
    brush_ = to_camera_space(apply(brush_world()));
    if (chain_.active && group == held_group_) {
      chain_.held_rotation = glm::normalize(rotation * chain_.held_rotation);
      chain_.held_scale *= scale;
    }
  }

  // R2 held: left stick moves the object left/right and forward/back (level
  // with the ground, so it doesn't drift up or down as the camera tilts) --
  // or, with R1 or L1 also held, left/right and up/down (strafe); right
  // stick left/right turns it about the point halfway between it and the
  // camera, up/down lifts and lowers it.
  void update_grab(f32 dt) {
    const bool strafe = held(GamepadButton::RightBumper) ||
                        held(GamepadButton::LeftBumper);
    const f32 lx = stick_x(GamepadAxis::LeftX);
    const f32 left_up = stick_up(GamepadAxis::LeftY);
    const f32 ly = strafe ? 0.0f : left_up;    // forward/back
    const f32 lift = strafe ? left_up : 0.0f;  // up/down
    const f32 rx = stick_x(GamepadAxis::RightX);
    const f32 ry = stick_up(GamepadAxis::RightY);
    const f32 resize = (held(GamepadButton::DpadUp) ? 1.0f : 0.0f) -
                       (held(GamepadButton::DpadDown) ? 1.0f : 0.0f);
    if (lx == 0.0f && ly == 0.0f && lift == 0.0f && rx == 0.0f &&
        ry == 0.0f && resize == 0.0f) {
      return;
    }
    const f32 depth = std::clamp(glm::dot(object_center_ - camera_.position(),
                                          camera_.forward()),
                                 kMinBrushDepth, kMaxGrabSpeedDistance);
    const f32 speed = kGrabScreenSpeed * depth * dt;

    glm::vec3 level_forward = camera_.forward();
    level_forward.y = 0.0f;
    if (glm::length(level_forward) < 1e-4f) {
      level_forward = screen_up(); // looking straight up/down
      level_forward.y = 0.0f;
    }
    level_forward = glm::normalize(level_forward);
    // World "up on screen" is -Y (see the orientation note above).
    const glm::vec3 world_up_on_screen(0.0f, -1.0f, 0.0f);

    glm::vec3 translation = camera_.right() * (lx * speed) +
                            level_forward * (ly * speed) +
                            world_up_on_screen * ((ry + lift) * speed);
    // Keep the object between the near and far grab distances.
    const f32 new_depth = glm::dot(object_center_ + translation -
                                       camera_.position(),
                                   camera_.forward());
    const f32 clamped_depth =
        std::clamp(new_depth, kMinGrabDistance, kMaxGrabDistance);
    translation += camera_.forward() * (clamped_depth - new_depth);

    const glm::vec3 pivot = 0.5f * (object_center_ + camera_.position());
    // Stick right swings the object to the right around the pivot -- the
    // same sense as the camera orbit (see update_orbit()).
    const glm::quat turn =
        glm::angleAxis(-rx * kGrabTurnRate * dt, glm::vec3(0.0f, 1.0f, 0.0f));

    // Resizing is about the object's own centre, so it is applied as a
    // separate step from the turn (which pivots about a different point).
    if (lx != 0.0f || ly != 0.0f || lift != 0.0f || rx != 0.0f ||
        ry != 0.0f) {
      move_group(held_group_, pivot, turn, translation);
    }
    if (resize != 0.0f) {
      const f32 scale = std::exp2(resize * dt / kResizeDoublingTime);
      move_group(held_group_, object_center_,
                 glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(0.0f), scale);
    }
  }

  // Triangle / Y while holding: snaps the held object so its anchor -- the
  // first stamp's position and rotation -- lands on the grid and on
  // 45-degree angles; the rest of its group moves rigidly with it.
  // Pressing it again changes nothing.
  void snap_object() {
    const i32 object =
        grabbed_object_ >= 0 ? grabbed_object_ : rotating_object_;
    glm::vec3 anchor;
    glm::quat anchor_rotation;
    if (object < 0 ||
        !renderer_sculpt_object_anchor(object, &anchor, &anchor_rotation)) {
      return;
    }
    // Rotation: round each Euler angle to 45 degrees, then turn the object
    // about its anchor by whatever takes the anchor's rotation there.
    const glm::vec3 euler = glm::eulerAngles(anchor_rotation);
    const glm::vec3 snapped_euler = glm::round(euler / kSnapAngle) * kSnapAngle;
    const glm::quat snapped_rotation = glm::quat(snapped_euler);
    const glm::quat turn =
        glm::normalize(snapped_rotation * glm::inverse(anchor_rotation));
    // Position: the anchor itself stays put under that turn, so it just
    // moves to the nearest grid point.
    const glm::vec3 translation =
        glm::round(anchor / kSnapGrid) * kSnapGrid - anchor;
    move_group(held_group_, anchor, turn, translation);
    KINFO("Snapped object {}{}.", object, group_note(held_group_));
  }

  // L2 held: rotate the object about its own centre, around the screen's
  // axes -- X = screen right, Y = screen up, Z = view direction. Up/down on
  // EITHER stick turns about X (the doc maps both); left/right on the left
  // stick about Y, on the right stick about Z.
  void update_rotate(f32 dt) {
    const f32 lx = stick_x(GamepadAxis::LeftX);
    const f32 ly = stick_up(GamepadAxis::LeftY);
    const f32 rx = stick_x(GamepadAxis::RightX);
    const f32 ry = stick_up(GamepadAxis::RightY);
    const f32 about_x = std::clamp(ly + ry, -1.0f, 1.0f);
    if (about_x == 0.0f && lx == 0.0f && rx == 0.0f) {
      return;
    }
    const f32 step = kRotateRate * dt;
    const glm::quat rotation =
        glm::angleAxis(about_x * step, camera_.right()) *
        glm::angleAxis(lx * step, screen_up()) *
        glm::angleAxis(rx * step, camera_.forward());
    move_group(held_group_, object_center_, rotation, glm::vec3(0.0f));
  }

  // --- Display ---------------------------------------------------------------

  // Selected objects get a dotted outline round their silhouette, drawn by
  // the renderer (it follows them as they move).
  // A group's members share one outline (merged where they overlap),
  // except inside the scoped group, where members are outlined one by one.
  // The scoped group itself is what the renderer keeps sharp.
  void draw_selection() {
    std::vector<i32> keys;
    keys.reserve(selection_.size());
    for (i32 object : selection_) {
      const Group *group = scoped_group_ < 0 ? group_of(object) : nullptr;
      keys.push_back(group ? group->id : -1 - object);
    }
    renderer_sculpt_set_outlined_objects(selection_.data(),
                                         static_cast<i32>(selection_.size()),
                                         kSelectionOutline, keys.data());
    const Group *scoped = scoped_group();
    renderer_sculpt_set_scope_objects(
        scoped ? scoped->members.data() : nullptr,
        scoped ? static_cast<i32>(scoped->members.size()) : 0);
  }

  // The toolbar: round buttons, highlighted under the reticle, dimmed when
  // they can't be used now, ringed when their mode is on; the hovered one
  // shows its name beside it.
  // One round button: background by state (active ring > hover > plain),
  // icon dimmed when disabled; `alpha` fades the whole button.
  static void draw_round_button(glm::vec2 centre, const char *icon,
                                bool hovered, bool active, bool enabled,
                                f32 alpha) {
    const f32 icon_size = 2.0f * kButtonRadius * kButtonIconScale;
    const char *background = active    ? "ui/toolbar/button_active.png"
                             : hovered ? "ui/toolbar/button_hover.png"
                                       : "ui/toolbar/button.png";
    renderer_draw_image(background, centre - glm::vec2(kButtonRadius),
                        glm::vec2(2.0f * kButtonRadius),
                        glm::vec4(1.0f, 1.0f, 1.0f, alpha));
    if (!icon) {
      return;
    }
    const f32 icon_alpha = (enabled ? 1.0f : kButtonDisabledAlpha) * alpha;
    renderer_draw_image(icon, centre - glm::vec2(0.5f * icon_size),
                        glm::vec2(icon_size),
                        glm::vec4(1.0f, 1.0f, 1.0f, icon_alpha));
  }

  // --- Top bar ------------------------------------------------------------------
  //
  // Up to three layers of round buttons at the top centre: the top row
  // (which depends on the mode), the row of the menu opened from it, and --
  // for Colour -- the picked colour's shades. Built fresh from the current
  // state whenever it's needed (UiItem), so layout, hover, pressing and
  // drawing all share one description.

  enum class Menu {
    None, Tools, Modes, CoatTools, Colour, Finishes, StyleTools, FleckStyles,
    EffectsTools
  };
  enum class EditMode { Default, Coat, Style, Effects };
  // Effects mode's tools: each animates the flecks of the object (or group)
  // under the reticle, R2 building it up (or, subtracting, down) gradually.
  enum class EffectsTool { Boil, Flow, Wave, Evaporate, Throb, Revert };
  static constexpr int kEffectsToolCount = 6;
  static constexpr const char *kEffectsToolNames[kEffectsToolCount] = {
      "Boil", "Flow", "Wave", "Evaporate", "Throb", "Revert"};
  static constexpr const char *kEffectsToolIcons[kEffectsToolCount] = {
      "ui/toolbar/boil.png", "ui/toolbar/flow.png", "ui/toolbar/wave.png",
      "ui/toolbar/evaporate.png", "ui/toolbar/throb.png",
      "ui/toolbar/revert.png"};
  enum class StyleTool {
    FleckStyle, Looseness, Comb, Ruffle, Impasto, Neatness, Revert
  };
  static constexpr int kStyleToolCount = 7;
  static constexpr const char *kStyleToolNames[kStyleToolCount] = {
      "Fleck Style", "Looseness", "Comb", "Ruffle", "Impasto", "Neatness",
      "Revert"};
  static constexpr const char *kStyleToolIcons[kStyleToolCount] = {
      "ui/toolbar/fleckstyle.png", "ui/toolbar/looseness.png",
      "ui/toolbar/comb.png", "ui/toolbar/ruffle.png",
      "ui/toolbar/impasto.png", "ui/toolbar/neatness.png",
      "ui/toolbar/revert.png"};
  // The 16 Dreams fleck styles, in the engine's style-id order (see
  // renderer_sculpt_set_material_flecks()).
  static constexpr int kFleckStyleCount = 16;
  static constexpr const char *kFleckStyleNames[kFleckStyleCount] = {
      "Square", "Circle", "Triangle", "Hexagon", "Dots", "Scribble",
      "Veins", "Splat", "Straight", "Heavy", "Hatching", "Cubist",
      "Impressionist", "Expressive", "Baroque", "Streaky"};
  static constexpr int kDefaultFleckStyle = 1; // Circle
  static std::string fleck_style_icon(int style) {
    return "ui/flecks/style_" + std::to_string(style) + ".png";
  }
  // Row buttons per line: a longer row (the 16 fleck styles) wraps.
  static constexpr int kRowWrap = 8;
  enum class CoatTool { Tint, Hue, Finish, Glow, Revert };

  static constexpr int kModeCount = 5;
  static constexpr const char *kModeNames[kModeCount] = {
      "Sculpt", "Paint", "Coat", "Style", "Effects"};
  static constexpr const char *kModeIcons[kModeCount] = {
      "ui/toolbar/sculpt.png", "ui/toolbar/paint.png", "ui/toolbar/coat.png",
      "ui/toolbar/style.png", "ui/toolbar/effects.png"};
  static constexpr int kCoatToolCount = 5;
  static constexpr const char *kCoatToolNames[kCoatToolCount] = {
      "Tint", "Hue", "Finish", "Glow", "Revert"};
  static constexpr const char *kCoatToolIcons[kCoatToolCount] = {
      "ui/toolbar/tint.png", "ui/coat/hue.png", "ui/toolbar/finish.png",
      "ui/toolbar/glow.png", "ui/toolbar/revert.png"};

  struct UiItem {
    std::string label;
    std::string icon;           // an image, drawn as is (white icons)...
    bool swatch = false;        // ...or a disc of `colour`...
    int finish = -1;            // ...or finish `finish` previewed in `colour`
    glm::vec3 colour{1.0f};
    bool active = false;  // ringed: the current choice / open menu
    bool enabled = true;
    std::function<void()> press;
    const char *shortcut[2] = {nullptr, nullptr}; // gamepad icons, if any
  };

  // Layers, for hover ids (layer * kLayerStride + index).
  static constexpr int kLayerTop = 0;
  static constexpr int kLayerRow = 1;
  static constexpr int kLayerShades = 2;
  static constexpr int kLayerExit = 3; // the Exit button (index 0)
  static constexpr int kLayerStride = 100;

  glm::vec3 coat_colour() const { return coat_shade(colour_index_, shade_index_); }

  std::vector<UiItem> top_items() {
    std::vector<UiItem> items;
    auto menu_button = [&](Menu menu, std::string label, std::string icon) {
      UiItem item;
      item.label = label;
      item.icon = icon;
      item.active = open_menu_ == menu;
      item.press = [this, menu] { toggle_menu(menu); };
      return item;
    };
    if (mode_ == EditMode::Coat) {
      const int tool = static_cast<int>(coat_tool_);
      items.push_back(menu_button(
          Menu::CoatTools, std::string("Tools: ") + kCoatToolNames[tool],
          kCoatToolIcons[tool]));
      UiItem add_sub;
      add_sub.label = coat_subtract_ ? "Subtracting" : "Adding";
      add_sub.icon = coat_subtract_ ? "ui/toolbar/minus.png" : "ui/toolbar/plus.png";
      add_sub.press = [this] { toggle_coat_subtract(); };
      add_sub.shortcut[0] = "y";
      items.push_back(add_sub);
      UiItem colour = menu_button(Menu::Colour, "Colour", "");
      colour.swatch = true;
      colour.colour = coat_colour();
      items.push_back(colour);
      UiItem finishes = menu_button(
          Menu::Finishes, std::string("Finish: ") + kCoatFinishes[finish_index_].name,
          "");
      finishes.finish = finish_index_;
      finishes.colour = coat_colour();
      items.push_back(finishes);
    } else if (mode_ == EditMode::Style) {
      const int tool = static_cast<int>(style_tool_);
      items.push_back(menu_button(
          Menu::StyleTools, std::string("Tools: ") + kStyleToolNames[tool],
          kStyleToolIcons[tool]));
      UiItem add_sub;
      add_sub.label = coat_subtract_ ? "Subtracting" : "Adding";
      add_sub.icon = coat_subtract_ ? "ui/toolbar/minus.png" : "ui/toolbar/plus.png";
      add_sub.press = [this] { toggle_coat_subtract(); };
      add_sub.shortcut[0] = "y";
      items.push_back(add_sub);
      items.push_back(menu_button(
          Menu::FleckStyles,
          std::string("Fleck Style: ") + kFleckStyleNames[fleck_style_index_],
          fleck_style_icon(fleck_style_index_)));
    } else if (mode_ == EditMode::Effects) {
      const int tool = static_cast<int>(effects_tool_);
      items.push_back(menu_button(
          Menu::EffectsTools, std::string("Tools: ") + kEffectsToolNames[tool],
          kEffectsToolIcons[tool]));
      UiItem add_sub;
      add_sub.label = coat_subtract_ ? "Subtracting" : "Adding";
      add_sub.icon = coat_subtract_ ? "ui/toolbar/minus.png" : "ui/toolbar/plus.png";
      add_sub.press = [this] { toggle_coat_subtract(); };
      add_sub.shortcut[0] = "y";
      items.push_back(add_sub);
    } else {
      items.push_back(menu_button(Menu::Tools, "Tools", "ui/toolbar/tools.png"));
      items.push_back(menu_button(Menu::Modes, "Modes", "ui/toolbar/modes.png"));
    }
    return items;
  }

  std::vector<UiItem> row_items(Menu menu) {
    std::vector<UiItem> items;
    switch (menu) {
    case Menu::Modes:
      for (int i = 0; i < kModeCount; ++i) {
        UiItem item;
        item.label = kModeNames[i];
        item.icon = kModeIcons[i];
        item.active = (i == 2 && mode_ == EditMode::Coat) ||
                      (i == 3 && mode_ == EditMode::Style) ||
                      (i == 4 && mode_ == EditMode::Effects);
        item.press = [this, i] { select_mode(i); };
        items.push_back(item);
      }
      break;
    case Menu::CoatTools:
      for (int i = 0; i < kCoatToolCount; ++i) {
        UiItem item;
        item.label = kCoatToolNames[i];
        item.icon = kCoatToolIcons[i];
        item.active = static_cast<int>(coat_tool_) == i;
        item.press = [this, i] {
          coat_tool_ = static_cast<CoatTool>(i);
          KINFO("Coat tool: {}.", kCoatToolNames[i]);
        };
        items.push_back(item);
      }
      break;
    case Menu::Colour:
      for (int i = 0; i < kCoatColourCount; ++i) {
        UiItem item;
        item.label = kCoatColourNames[i];
        item.swatch = true;
        item.colour = kCoatColours[i];
        item.active = colour_index_ == i;
        item.press = [this, i] {
          colour_index_ = i;
          shade_index_ = kCoatShadeCount / 2;
          shades_open_ = true;
          shades_progress_ = 0.0f;
          KINFO("Coat colour: {}.", kCoatColourNames[i]);
        };
        items.push_back(item);
      }
      break;
    case Menu::Finishes:
      for (int i = 0; i < kCoatFinishCount; ++i) {
        UiItem item;
        item.label = kCoatFinishes[i].name;
        item.finish = i;
        item.colour = coat_colour();
        item.active = finish_index_ == i;
        item.press = [this, i] {
          finish_index_ = i;
          KINFO("Coat finish: {}.", kCoatFinishes[i].name);
        };
        items.push_back(item);
      }
      break;
    case Menu::StyleTools:
      for (int i = 0; i < kStyleToolCount; ++i) {
        UiItem item;
        item.label = kStyleToolNames[i];
        item.icon = kStyleToolIcons[i];
        item.active = static_cast<int>(style_tool_) == i;
        item.press = [this, i] {
          style_tool_ = static_cast<StyleTool>(i);
          KINFO("Style tool: {}.", kStyleToolNames[i]);
        };
        items.push_back(item);
      }
      break;
    case Menu::EffectsTools:
      for (int i = 0; i < kEffectsToolCount; ++i) {
        UiItem item;
        item.label = kEffectsToolNames[i];
        item.icon = kEffectsToolIcons[i];
        item.active = static_cast<int>(effects_tool_) == i;
        item.press = [this, i] {
          effects_tool_ = static_cast<EffectsTool>(i);
          KINFO("Effects tool: {}.", kEffectsToolNames[i]);
        };
        items.push_back(item);
      }
      break;
    case Menu::FleckStyles:
      for (int i = 0; i < kFleckStyleCount; ++i) {
        UiItem item;
        item.label = kFleckStyleNames[i];
        item.icon = fleck_style_icon(i);
        item.active = fleck_style_index_ == i;
        item.press = [this, i] {
          fleck_style_index_ = i;
          KINFO("Fleck style: {}.", kFleckStyleNames[i]);
        };
        items.push_back(item);
      }
      break;
    default: // Tools (default mode): nothing yet
      break;
    }
    return items;
  }

  std::vector<UiItem> shade_items() {
    std::vector<UiItem> items;
    for (int i = 0; i < kCoatShadeCount; ++i) {
      UiItem item;
      item.label = kShadeNames[i];
      item.swatch = true;
      item.colour = coat_shade(colour_index_, i);
      item.active = shade_index_ == i;
      item.press = [this, i] { shade_index_ = i; };
      items.push_back(item);
    }
    return items;
  }

  bool shades_visible() const {
    return shown_menu_ == Menu::Colour && shades_progress_ > 0.0f;
  }

  void toggle_menu(Menu menu) {
    if (open_menu_ == menu) {
      open_menu_ = Menu::None; // close; the row animates away
      shades_open_ = false;
    } else {
      open_menu_ = menu;
      shown_menu_ = menu;
      menu_progress_ = 0.0f; // drop the new row in from the top
      shades_open_ = false;
      shades_progress_ = 0.0f;
    }
  }

  void select_mode(int index) {
    if (index >= 2 && index <= 4) { // Coat, Style, Effects
      mode_ = index == 2   ? EditMode::Coat
              : index == 3 ? EditMode::Style
                           : EditMode::Effects;
      open_menu_ = Menu::None;
      shades_open_ = false;
      KINFO("Entered {} mode.", mode_name());
      return;
    }
    KINFO("{} mode isn't implemented yet.", kModeNames[index]);
  }

  // Exit button / L1 + B: back to the normal controls.
  void exit_mode() {
    if (mode_ == EditMode::Default) {
      return;
    }
    KINFO("Left {} mode.", mode_name());
    mode_ = EditMode::Default;
    open_menu_ = Menu::None;
    shades_open_ = false;
  }

  const char *mode_name() const {
    switch (mode_) {
    case EditMode::Coat: return "coat";
    case EditMode::Style: return "style";
    case EditMode::Effects: return "effects";
    default: return "";
    }
  }

  // The Exit button: top-right corner of the area the reticle can reach,
  // level with the top bar; only shown inside a mode.
  glm::vec2 exit_button_center() const {
    const f32 reach_x = 0.5f * kScreenEdge * screen_width_;
    return {0.5f * screen_width_ + reach_x - kButtonMargin - kButtonRadius,
            top_row_y()};
  }

  void toggle_coat_subtract() {
    coat_subtract_ = !coat_subtract_;
    KINFO("Coat: {}.", coat_subtract_ ? "subtracting" : "adding");
  }

  // --- Top bar layout / animation --------------------------------------------

  f32 top_row_y() const {
    const f32 reach_y = 0.5f * kScreenEdge * screen_height_;
    return 0.5f * screen_height_ - reach_y + kButtonMargin + kButtonRadius;
  }
  f32 row_home_y() const { return top_row_y() + 2.0f * kButtonRadius + kMenuRowGap; }
  // The shades row sits a label's height lower, so the colour names fit.
  f32 shades_home_y() const {
    return row_home_y() + 2.0f * kButtonRadius + kMenuRowGap + kTipFontPixels + 6.0f;
  }

  static f32 row_x(f32 centre_x, int index, int count) {
    const f32 step = 2.0f * kButtonRadius + kButtonGap;
    return centre_x +
           step * (static_cast<f32>(index) - 0.5f * static_cast<f32>(count - 1));
  }

  static f32 staggered(f32 progress, int index, int count) {
    const f32 total = kMenuDropTime + kMenuDropStagger * static_cast<f32>(count - 1);
    const f32 t = progress * total - kMenuDropStagger * static_cast<f32>(index);
    return std::clamp(t / kMenuDropTime, 0.0f, 1.0f);
  }
  static f32 ease_out(f32 t) { return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t); }

  // Where item `index` of `layer` is, and how far it has dropped in (0..1).
  glm::vec2 item_center(int layer, int index, int count, f32 *out_shown) const {
    const f32 cx = 0.5f * screen_width_;
    if (layer == kLayerTop) {
      *out_shown = 1.0f;
      return {row_x(cx, index, count), top_row_y()};
    }
    const bool shades = layer == kLayerShades;
    const f32 shown =
        staggered(shades ? shades_progress_ : menu_progress_, index, count);
    *out_shown = shown;
    // A long row wraps after kRowWrap buttons, each further line a button
    // plus a label's height lower.
    const int line = shades ? 0 : index / kRowWrap;
    const int col = shades ? index : index % kRowWrap;
    const int line_count =
        shades ? count : std::min(count - line * kRowWrap, kRowWrap);
    const f32 line_step = 2.0f * kButtonRadius + kMenuRowGap + kTipFontPixels + 6.0f;
    const f32 from = shades ? row_home_y() : top_row_y();
    const f32 to = (shades ? shades_home_y() : row_home_y()) +
                   line_step * static_cast<f32>(line);
    return {row_x(cx, col, line_count), glm::mix(from, to, ease_out(shown))};
  }

  void update_menu(f32 dt) {
    const f32 total = kMenuDropTime + kMenuDropStagger * 10.0f;
    if (open_menu_ != Menu::None) {
      menu_progress_ = std::min(menu_progress_ + dt / total, 1.0f);
    } else {
      menu_progress_ = std::max(menu_progress_ - dt / total, 0.0f);
      if (menu_progress_ <= 0.0f) {
        shown_menu_ = Menu::None;
      }
    }
    shades_progress_ = shades_open_ ? std::min(shades_progress_ + dt / total, 1.0f)
                                    : std::max(shades_progress_ - dt / total, 0.0f);
  }

  // Hover id under the reticle, or -1. Only fully dropped-in items count.
  int top_bar_under_reticle() {
    if (brush_.z <= 0.01f) {
      return -1;
    }
    const glm::vec2 reticle = reticle_screen();
    const f32 reach = kButtonRadius + kButtonHoverSlop;
    auto test = [&](int layer, int count) {
      for (int i = 0; i < count; ++i) {
        f32 shown = 0.0f;
        const glm::vec2 c = item_center(layer, i, count, &shown);
        if (shown >= 1.0f && glm::length(reticle - c) <= reach) {
          return layer * kLayerStride + i;
        }
      }
      return -1;
    };
    if (mode_ != EditMode::Default &&
        glm::length(reticle - exit_button_center()) <= reach) {
      return kLayerExit * kLayerStride;
    }
    if (shades_visible()) {
      if (int id = test(kLayerShades, kCoatShadeCount); id >= 0) {
        return id;
      }
    }
    if (shown_menu_ != Menu::None) {
      if (int id = test(kLayerRow, static_cast<int>(row_items(shown_menu_).size()));
          id >= 0) {
        return id;
      }
    }
    return test(kLayerTop, static_cast<int>(top_items().size()));
  }

  void press_top_bar(int id) {
    const int layer = id / kLayerStride;
    const int index = id % kLayerStride;
    if (layer == kLayerExit) {
      KINFO("Pressed Exit.");
      exit_mode();
      return;
    }
    std::vector<UiItem> items = layer == kLayerTop   ? top_items()
                                : layer == kLayerRow ? row_items(shown_menu_)
                                                     : shade_items();
    if (index < static_cast<int>(items.size()) && items[index].press) {
      KINFO("Pressed {}.", items[index].label);
      items[index].press();
    }
  }

  // A finish preview sphere in `colour`: the shading layer tinted by it
  // (dimmer for metals, which have little diffuse colour), then the
  // highlight -- tinted too for metals, which reflect in their colour.
  static void draw_finish_sphere(glm::vec2 centre, f32 size, int finish,
                                 glm::vec3 colour, f32 alpha) {
    const CoatFinish &f = kCoatFinishes[finish];
    const std::string base = "ui/coat/finish_" + std::to_string(finish);
    const glm::vec2 corner = centre - glm::vec2(0.5f * size);
    renderer_draw_image(base + "_base.png", corner, glm::vec2(size),
                        glm::vec4(colour, alpha));
    renderer_draw_image(base + "_spec.png", corner, glm::vec2(size),
                        glm::vec4(glm::mix(glm::vec3(1.0f), colour, f.metallic),
                                  alpha));
  }

  void draw_ui_item(const UiItem &item, glm::vec2 centre, bool hovered,
                    f32 alpha) {
    if (!item.icon.empty()) {
      draw_round_button(centre, item.icon.c_str(), hovered, item.active,
                        item.enabled, alpha);
      return;
    }
    // Swatch / finish: the button background, then the colour or sphere.
    draw_round_button(centre, nullptr, hovered, item.active, item.enabled, alpha);
    const f32 size = 2.0f * kButtonRadius * (item.finish >= 0 ? 0.78f : 0.7f);
    if (item.finish >= 0) {
      draw_finish_sphere(centre, size, item.finish, item.colour, alpha);
    } else {
      renderer_draw_image("ui/coat/swatch.png", centre - glm::vec2(0.5f * size),
                          glm::vec2(size), glm::vec4(item.colour, alpha));
    }
  }

  void draw_layer(int layer, const std::vector<UiItem> &items) {
    const int count = static_cast<int>(items.size());
    for (int i = 0; i < count; ++i) {
      f32 shown = 0.0f;
      const glm::vec2 centre = item_center(layer, i, count, &shown);
      if (shown <= 0.0f) {
        continue;
      }
      const bool hovered = hovered_top_ == layer * kLayerStride + i;
      draw_ui_item(items[i], centre, hovered, shown); // name: the tooltip
    }
  }

  void draw_top_bar() {
    if (mode_ != EditMode::Default) {
      const glm::vec2 centre = exit_button_center();
      const bool hovered = hovered_top_ == kLayerExit * kLayerStride;
      draw_round_button(centre, "ui/toolbar/exit.png", hovered, false, true,
                        1.0f);
    }
    draw_layer(kLayerTop, top_items());
    if (shown_menu_ == Menu::Tools && menu_progress_ > 0.0f) {
      const char *empty = "No tools yet";
      draw_shadowed_text(
          empty,
          glm::vec2(0.5f * screen_width_ - 0.5f * text_width(empty),
                    row_home_y() + 0.36f * kTipFontPixels),
          glm::vec4(1.0f, 1.0f, 1.0f, 0.8f), staggered(menu_progress_, 0, 1));
    }
    if (shown_menu_ != Menu::None) {
      draw_layer(kLayerRow, row_items(shown_menu_));
    }
    if (shades_visible()) {
      draw_layer(kLayerShades, shade_items());
    }
  }

  // --- Coat --------------------------------------------------------------------

  // An object's coat: its own material slot and the look painted onto it so
  // far. Objects with none are on the shared plain slot.
  struct ObjectCoat {
    i32 slot = kPlainSlot;
    glm::vec3 colour = kPlainColour;
    f32 roughness = kCoatFinishes[0].roughness;
    f32 metallic = 0.0f;
    f32 waxy = 0.0f; // 0..1; on past halfway
    f32 glow = 0.0f;
    i32 light = -1; // its glow light (see sync_glow_lights()), or -1
    // Flecks (style mode).
    f32 looseness = 0.0f;
    i32 fleck_style = kDefaultFleckStyle;
    f32 comb = 0.0f;
    f32 ruffle = 0.5f;
    f32 impasto = 0.0f;
    f32 neatness = 0.0f;
    // Effects (effects mode), each 0 = still .. 1 = full.
    f32 boil = 0.0f;
    f32 flow = 0.0f;
    f32 wave = 0.0f;
    f32 evaporate = 0.0f;
    f32 throb = 0.0f;
  };

  static void apply_coat_material(const ObjectCoat &coat) {
    renderer_sculpt_set_material(coat.slot, coat.colour, coat.roughness,
                                 coat.metallic, coat.waxy > 0.5f);
    renderer_sculpt_set_material_glow(coat.slot, coat.glow);
    renderer_sculpt_set_material_flecks(coat.slot, coat.looseness,
                                        coat.fleck_style, coat.comb,
                                        coat.ruffle, coat.impasto,
                                        coat.neatness);
    renderer_sculpt_set_material_effects(coat.slot, coat.boil, coat.flow,
                                         coat.wave, coat.evaporate,
                                         coat.throb);
  }

  // Every object starts plain: one shared slot, set up once.
  void make_everything_plain() {
    ObjectCoat plain;
    apply_coat_material(plain);
    const i32 count = renderer_sculpt_object_ids(nullptr, 0);
    std::vector<i32> ids(static_cast<size_t>(count));
    renderer_sculpt_object_ids(ids.data(), count);
    if (!ids.empty()) {
      renderer_sculpt_set_object_material(ids.data(), count, kPlainSlot);
    }
  }

  i32 allocate_coat_slot() {
    if (!free_coat_slots_.empty()) {
      const i32 slot = free_coat_slots_.back();
      free_coat_slots_.pop_back();
      return slot;
    }
    return next_coat_slot_ <= kMaxCoatSlot ? next_coat_slot_++ : -1;
  }

  int coat_slot_users(i32 slot) const {
    int users = 0;
    for (const auto &[object, coat] : coats_) {
      users += coat.slot == slot ? 1 : 0;
    }
    return users;
  }

  // The object's coat, on a slot of its own (moving it onto one -- a re-run
  // of its region -- the first time it is coated, or when it still shares
  // its slot with the object it was cloned from). Null if out of slots.
  ObjectCoat *own_coat(i32 object) {
    auto it = coats_.find(object);
    ObjectCoat coat = it != coats_.end() ? it->second : ObjectCoat{};
    if (coat.slot != kPlainSlot && coat_slot_users(coat.slot) <= 1) {
      return &coats_[object];
    }
    const i32 slot = allocate_coat_slot();
    if (slot < 0) {
      if (!coat_slots_warned_) {
        KWARN("Coat: out of material slots; more objects can't be coated.");
        coat_slots_warned_ = true;
      }
      return nullptr;
    }
    coat.slot = slot;
    apply_coat_material(coat);
    renderer_sculpt_set_object_material(&object, 1, slot);
    history_dirty_ = true;
    coats_[object] = coat;
    return &coats_[object];
  }

  // Forgets deleted objects' coats, freeing slots nothing uses any more.
  void forget_coats(const std::vector<i32> &objects) {
    for (i32 object : objects) {
      auto it = coats_.find(object);
      if (it == coats_.end()) {
        continue;
      }
      const i32 slot = it->second.slot;
      release_glow_light(it->second);
      coats_.erase(it);
      if (slot != kPlainSlot && coat_slot_users(slot) == 0) {
        free_coat_slots_.push_back(slot);
      }
    }
  }

  void release_glow_light(ObjectCoat &coat) {
    if (coat.light < 0) {
      return;
    }
    renderer_set_point_light(coat.light, glm::vec3(0.0f), glm::vec3(0.0f),
                             0.0f, -1);
    free_glow_lights_.push_back(coat.light);
    coat.light = -1;
  }

  // A glowing object lights up its surroundings: a point light at its
  // centre, in its colour, as bright as its glow (scaled by its size), and
  // marked with its material so its own shell doesn't shadow the light
  // (the shading pass also draws a halo of scattered light around it).
  // Kept in step every frame, so it follows the object when moved.
  void sync_glow_lights() {
    for (auto &[object, coat] : coats_) {
      if (coat.glow <= 0.0f) {
        release_glow_light(coat);
        continue;
      }
      glm::vec3 lo, hi;
      if (!renderer_sculpt_object_bounds(object, &lo, &hi)) {
        continue;
      }
      const glm::vec3 centre = 0.5f * (lo + hi);
      // The bounds are generous (rotated/blended stamps); about half their
      // half-diagonal is a fair "size".
      const f32 radius = std::max(0.25f * glm::length(hi - lo), 0.3f);
      if (coat.light < 0) {
        if (!free_glow_lights_.empty()) {
          coat.light = free_glow_lights_.back();
          free_glow_lights_.pop_back();
        } else {
          coat.light = renderer_add_point_light(centre, coat.colour, 0.0f);
          if (coat.light < 0) {
            continue; // the engine's light list is full
          }
        }
      }
      renderer_set_point_light(coat.light, centre, coat.colour,
                               coat.glow * kGlowLightIntensity * radius * radius,
                               coat.slot);
    }
  }

  // R2 held in style mode: one frame of the current fleck tool on the
  // object (or group) under the reticle.
  void update_style(f32 dt) {
    if (mode_ != EditMode::Style || !r2_.down || hovered_button_ >= 0 ||
        hovered_top_ >= 0) {
      return;
    }
    const i32 object = pick_object_under_reticle();
    if (object < 0) {
      return;
    }
    const f32 step = dt / kCoatFullTime;
    const f32 target = coat_subtract_ ? 0.0f : 1.0f;
    for (i32 id : group_for(object)) {
      ObjectCoat *coat = own_coat(id);
      if (!coat) {
        continue;
      }
      switch (style_tool_) {
      case StyleTool::FleckStyle:
        coat->fleck_style =
            coat_subtract_ ? kDefaultFleckStyle : fleck_style_index_;
        break;
      case StyleTool::Looseness:
        coat->looseness = move_toward(coat->looseness, target, step);
        break;
      case StyleTool::Comb:
        coat->comb = move_toward(coat->comb, target, step);
        break;
      case StyleTool::Ruffle: // Comb run backwards
        coat->comb = move_toward(coat->comb, 1.0f - target, step);
        break;
      case StyleTool::Impasto:
        coat->impasto = move_toward(coat->impasto, target, step);
        break;
      case StyleTool::Neatness:
        coat->neatness = move_toward(coat->neatness, target, step);
        break;
      case StyleTool::Revert: { // every style setting back toward default
        const ObjectCoat plain;
        coat->fleck_style = plain.fleck_style;
        coat->looseness = move_toward(coat->looseness, plain.looseness, step);
        coat->comb = move_toward(coat->comb, plain.comb, step);
        coat->ruffle = move_toward(coat->ruffle, plain.ruffle, step);
        coat->impasto = move_toward(coat->impasto, plain.impasto, step);
        coat->neatness = move_toward(coat->neatness, plain.neatness, step);
        break;
      }
      }
      history_dirty_ = true;
      apply_coat_material(*coat);
    }
  }

  // R2 held in effects mode: builds the current effect up (or down) on the
  // object (or group) under the reticle, like a style tool.
  void update_effects(f32 dt) {
    if (mode_ != EditMode::Effects || !r2_.down || hovered_button_ >= 0 ||
        hovered_top_ >= 0) {
      return;
    }
    const i32 object = pick_object_under_reticle();
    if (object < 0) {
      return;
    }
    const f32 step = dt / kCoatFullTime;
    const f32 target = coat_subtract_ ? 0.0f : 1.0f;
    for (i32 id : group_for(object)) {
      ObjectCoat *coat = own_coat(id);
      if (!coat) {
        continue;
      }
      if (effects_tool_ == EffectsTool::Revert) { // every effect toward still
        for (f32 *e : {&coat->boil, &coat->flow, &coat->wave, &coat->evaporate,
                       &coat->throb}) {
          *e = move_toward(*e, 0.0f, step);
        }
        apply_coat_material(*coat);
        history_dirty_ = true;
        continue;
      }
      f32 *effect = nullptr;
      switch (effects_tool_) {
      case EffectsTool::Boil: effect = &coat->boil; break;
      case EffectsTool::Flow: effect = &coat->flow; break;
      case EffectsTool::Wave: effect = &coat->wave; break;
      case EffectsTool::Evaporate: effect = &coat->evaporate; break;
      case EffectsTool::Throb: effect = &coat->throb; break;
      case EffectsTool::Revert: break; // handled above
      }
      *effect = move_toward(*effect, target, step);
      apply_coat_material(*coat);
      history_dirty_ = true;
    }
  }

  // R2 held in coat mode: one frame of the current tool on the object (or
  // group) under the reticle.
  void update_coat(f32 dt) {
    if (mode_ != EditMode::Coat || !r2_.down || hovered_button_ >= 0 ||
        hovered_top_ >= 0) {
      coating_ = false;
      return;
    }
    const i32 object = pick_object_under_reticle();
    coating_ = object >= 0;
    if (object < 0) {
      return;
    }
    const f32 step = dt / kCoatFullTime;
    const glm::vec3 picked = coat_colour();
    const CoatFinish &finish = kCoatFinishes[finish_index_];
    const bool revert = coat_tool_ == CoatTool::Revert;
    const bool subtract = coat_subtract_ || revert;
    for (i32 id : group_for(object)) {
      ObjectCoat *coat = own_coat(id);
      if (!coat) {
        continue;
      }
      const bool all = revert;
      if (coat_tool_ == CoatTool::Tint || coat_tool_ == CoatTool::Hue || all) {
        glm::vec3 target = kPlainColour;
        if (!subtract && coat_tool_ == CoatTool::Tint) {
          target = picked;
        } else if (!subtract) { // Hue: the picked hue/saturation, own brightness
          const glm::vec3 hsv = rgb_to_hsv(picked);
          target = hsv_to_rgb({hsv.x, hsv.y, rgb_to_hsv(coat->colour).z});
        }
        coat->colour = move_toward(coat->colour, target, step);
      }
      if (coat_tool_ == CoatTool::Finish || all) {
        const CoatFinish &to = subtract ? kCoatFinishes[0] : finish;
        coat->roughness = move_toward(coat->roughness, to.roughness, step);
        coat->metallic = move_toward(coat->metallic, to.metallic, step);
        coat->waxy = move_toward(coat->waxy, to.waxy ? 1.0f : 0.0f, step);
      }
      if (coat_tool_ == CoatTool::Glow || all) {
        coat->glow = move_toward(coat->glow, subtract ? 0.0f : 1.0f, step);
      }
      history_dirty_ = true;
      apply_coat_material(*coat);
    }
  }

  void draw_toolbar() {
    for (int i = 0; i < kButtonCount; ++i) {
      if (button_shown(i) <= 0.0f) {
        continue;
      }
      const auto button = static_cast<ToolbarButton>(i);
      const glm::vec2 centre = button_center(i);
      const bool hovered = hovered_button_ == i;
      draw_round_button(centre, button_icon(button), hovered,
                        button_active(button), button_enabled(button), 1.0f);
    }
  }

  // The single floating reticle: a crosshair at the brush point's place on
  // screen, coloured by what it would act on. This replaces the brush
  // preview sphere the camera-only version showed.
  void draw_reticle() {
    if (brush_.z <= 0.01f) {
      return;
    }
    // Same projection as the raymarch shader: offsets are in units of
    // screen height from the centre, and screen y grows downward.
    const glm::vec2 centre(
        0.5f * screen_width_ + (brush_.x / brush_.z) * screen_height_,
        0.5f * screen_height_ - (brush_.y / brush_.z) * screen_height_);

    glm::vec4 colour = kReticleIdle;
    if (grabbed_object_ >= 0) {
      colour = kReticleGrabbing;
    } else if (rotating_object_ >= 0) {
      colour = kReticleRotating;
    } else if (hovered_object_ >= 0) {
      colour = kReticleOverObject;
    }

    const f32 inner = kReticleGap;
    const f32 outer = kReticleGap + kReticleArm;
    renderer_draw_line(centre + glm::vec2(-outer, 0.0f),
                       centre + glm::vec2(-inner, 0.0f), colour);
    renderer_draw_line(centre + glm::vec2(inner, 0.0f),
                       centre + glm::vec2(outer, 0.0f), colour);
    renderer_draw_line(centre + glm::vec2(0.0f, -outer),
                       centre + glm::vec2(0.0f, -inner), colour);
    renderer_draw_line(centre + glm::vec2(0.0f, inner),
                       centre + glm::vec2(0.0f, outer), colour);
  }

  // The reticle's pixel position (see draw_reticle()).
  glm::vec2 reticle_screen() const {
    return glm::vec2(
        0.5f * screen_width_ + (brush_.x / brush_.z) * screen_height_,
        0.5f * screen_height_ - (brush_.y / brush_.z) * screen_height_);
  }

  // The rows for what the reticle is doing now; empty when there's nothing
  // to explain (not on an object, or mid-animation).
  std::vector<TipRow> tooltip_rows() {
    if (framing_ || recentering_) {
      return {};
    }
    // Over a button: its name (A presses it) and its shortcut, if any.
    if (hovered_button_ >= 0 && grabbed_object_ < 0 && rotating_object_ < 0) {
      const auto button = static_cast<ToolbarButton>(hovered_button_);
      std::vector<TipRow> rows = {{{"a", nullptr}, button_label(button)}};
      if (button == ToolbarButton::Scope && scoped_group_ >= 0) {
        rows.push_back({{"b", nullptr}, "Shortcut"});
      }
      return rows;
    }
    if (hovered_top_ >= 0 && grabbed_object_ < 0 && rotating_object_ < 0) {
      const int layer = hovered_top_ / kLayerStride;
      const int index = hovered_top_ % kLayerStride;
      if (layer == kLayerExit) {
        return {{{"a", nullptr}, std::string("Exit ") + mode_name() + " mode"},
                {{"l1", "b"}, "Shortcut"}};
      }
      const std::vector<UiItem> items = layer == kLayerTop   ? top_items()
                                        : layer == kLayerRow ? row_items(shown_menu_)
                                                             : shade_items();
      if (index >= static_cast<int>(items.size())) {
        return {};
      }
      std::vector<TipRow> rows = {{{"a", nullptr}, items[index].label}};
      if (items[index].shortcut[0]) {
        rows.push_back({{items[index].shortcut[0], items[index].shortcut[1]},
                        "Shortcut"});
      }
      return rows;
    }
    if (grabbed_object_ >= 0 && chain_.active) {
      // Cloning: Fill/Repeat take Flip's place (it still works).
      return {
          {{"ls", nullptr}, "Move"},
          {{"rs", nullptr}, "Turn"},
          {{"dpad_left", nullptr}, "Fill"},
          {{"dpad_right", nullptr}, "Repeat"},
          {{"l1", "ls"}, "Lift"},
          {{"dpad_updown", nullptr}, "Resize"},
          {{"y", nullptr}, "Snap"},
          {{"r2", nullptr}, "Drop"},
      };
    }
    if (grabbed_object_ >= 0) {
      return {
          {{"ls", nullptr}, "Move"},
          {{"l1", "ls"}, "Lift"},
          {{"rs", nullptr}, "Turn"},
          {{"dpad_updown", nullptr}, "Resize"},
          {{"y", nullptr}, "Snap"},
          {{"l3", nullptr}, "Flip"},
          {{"r2", nullptr}, "Drop"},
      };
    }
    if (rotating_object_ >= 0) {
      return {
          {{"ls", nullptr}, "Spin"},
          {{"rs", nullptr}, "Roll"},
          {{"y", nullptr}, "Snap"},
          {{"l2", nullptr}, "Done"},
      };
    }
    if (hovered_object_ >= 0 &&
        (mode_ == EditMode::Style || mode_ == EditMode::Effects)) {
      const bool reverting = mode_ == EditMode::Style
                                 ? style_tool_ == StyleTool::Revert
                                 : effects_tool_ == EffectsTool::Revert;
      const std::string style_word =
          reverting ? std::string("Revert")
                    : std::string(mode_ == EditMode::Style
                                      ? kStyleToolNames[static_cast<int>(style_tool_)]
                                      : kEffectsToolNames[static_cast<int>(effects_tool_)]) +
                          (coat_subtract_ ? " -" : " +");
      return {
          {{"r2", nullptr}, style_word},
          {{"y", nullptr}, coat_subtract_ ? "Add" : "Subtract"},
          {{"a", nullptr},
           is_selected(hovered_object_) ? "Deselect" : "Select"},
          {{"l3", nullptr}, "Flip"},
          {{"l1", "r1"}, "Frame"},
          {{"l1", "b"}, "Exit"},
      };
    }
    if (hovered_object_ >= 0 && mode_ == EditMode::Coat) {
      static std::string apply_word;
      apply_word = coat_tool_ == CoatTool::Revert
                       ? "Revert"
                       : std::string(kCoatToolNames[static_cast<int>(coat_tool_)]) +
                             (coat_subtract_ ? " -" : " +");
      return {
          {{"r2", nullptr}, apply_word},
          {{"y", nullptr}, coat_subtract_ ? "Add" : "Subtract"},
          {{"l2", nullptr}, "Rotate"},
          {{"a", nullptr},
           is_selected(hovered_object_) ? "Deselect" : "Select"},
          {{"l3", nullptr}, "Flip"},
          {{"l1", "r1"}, "Frame"},
          {{"l1", "b"}, "Exit"},
      };
    }
    if (hovered_object_ >= 0) {
      return {
          {{"r2", nullptr}, "Grab"},
          {{"l2", nullptr}, "Rotate"},
          {{"l1", "r2"}, "Clone"},
          {{"a", nullptr},
           is_selected(hovered_object_) ? "Deselect" : "Select"},
          {{"y", nullptr}, "Delete"},
          {{"l3", nullptr}, "Flip"},
          {{"l1", "r1"}, "Frame"},
      };
    }
    // Over nothing: getting around, and what can be done from here.
    if (mode_ == EditMode::Default) {
      return {
          {{"ls", nullptr}, "Move"},
          {{"rs", nullptr}, "Look"},
          {{"r1", nullptr}, "Orbit"},
          {{"l1", "ls"}, "Strafe"},
          {{"dpad_left", nullptr}, "Undo"},
          {{"dpad_right", nullptr}, "Redo"},
          {{"l1", "r1"}, "Frame all"},
          {{"start", nullptr}, "Recenter"},
      };
    }
    return {
        {{"ls", nullptr}, "Move"},
        {{"rs", nullptr}, "Look"},
        {{"r1", nullptr}, "Orbit"},
        {{"l1", "ls"}, "Strafe"},
        {{"dpad_left", nullptr}, "Undo"},
        {{"dpad_right", nullptr}, "Redo"},
        {{"l1", "r1"}, "Frame all"},
        {{"l1", "b"}, "Exit"},
    };
  }

  // Draws the tooltip around the reticle with no background: at most
  // kTipMaxRows rows per column, the first column to the right of the
  // reticle and the rest to its left, each centred vertically on it. Text
  // gets a drop shadow so it reads over any background.
  //
  // Fades in when there is something to show and out when there isn't;
  // while fading out it keeps showing the last rows it had.
  void draw_tooltip(f32 dt) {
    const std::vector<TipRow> current = tooltip_rows();
    if (!current.empty()) {
      tip_rows_ = current;
      tip_fade_ = std::min(tip_fade_ + dt / kTipFadeInTime, 1.0f);
    } else {
      tip_fade_ = std::max(tip_fade_ - dt / kTipFadeOutTime, 0.0f);
    }
    if (tip_fade_ <= 0.0f || tip_rows_.empty() || brush_.z <= 0.01f) {
      return;
    }
    tip_alpha_ = tip_fade_ * tip_fade_ * (3.0f - 2.0f * tip_fade_); // smoothstep
    const std::vector<TipRow> &rows = tip_rows_;
    glm::vec2 reticle = reticle_screen();
    const size_t right_count = std::min(rows.size(), kTipMaxRows);
    const size_t left_last = std::min(rows.size(), right_count + kTipMaxRows);
    // Near a screen edge, slide the whole tooltip in (both columns
    // together) rather than squashing one column onto the other.
    const f32 right_width = tip_column_width(rows, 0, right_count);
    const f32 left_width = tip_column_width(rows, right_count, left_last);
    const f32 min_x = left_width > 0.0f ? kTipOffset + left_width : 0.0f;
    const f32 max_x = screen_width_ - kTipOffset - right_width;
    if (min_x <= max_x) {
      reticle.x = std::clamp(reticle.x, min_x, max_x);
    }
    draw_tip_column(rows, 0, right_count, reticle, true);
    if (rows.size() > right_count) {
      draw_tip_column(rows, right_count, left_last, reticle, false);
    }
  }

  f32 tip_column_width(const std::vector<TipRow> &rows, size_t first,
                       size_t last) const {
    f32 icon_column = 0.0f;
    f32 width = 0.0f;
    for (size_t i = first; i < last; ++i) {
      icon_column = std::max(icon_column, tip_icons_width(rows[i]));
    }
    for (size_t i = first; i < last; ++i) {
      width = std::max(width, icon_column + kTipTextGap +
                                  text_width(rows[i].word.c_str()));
    }
    return width;
  }

  f32 tip_icons_width(const TipRow &row) const {
    return row.icons[1] ? 2.0f * kTipIconSize + 2.0f * kTipIconGap +
                              kTipAverageGlyphWidth
                        : kTipIconSize;
  }

  // One column of rows [first, last). Each row is icons then word; on the
  // right the icons line up next to the reticle, on the left the words do
  // (right-aligned), so both columns read outward from the centre.
  void draw_tip_column(const std::vector<TipRow> &rows, size_t first,
                       size_t last, glm::vec2 reticle, bool right) {
    f32 icon_column = 0.0f;
    for (size_t i = first; i < last; ++i) {
      icon_column = std::max(icon_column, tip_icons_width(rows[i]));
    }
    const f32 width = tip_column_width(rows, first, last);
    const f32 height = kTipRowHeight * static_cast<f32>(last - first);
    f32 left = right ? reticle.x + kTipOffset : reticle.x - kTipOffset - width;
    f32 top = reticle.y - 0.5f * height;
    left = std::clamp(left, 0.0f, std::max(screen_width_ - width, 0.0f));
    top = std::clamp(top, 0.0f, std::max(screen_height_ - height, 0.0f));

    for (size_t i = first; i < last; ++i) {
      const TipRow &row = rows[i];
      const f32 row_top = top + kTipRowHeight * static_cast<f32>(i - first);
      const f32 icon_top = row_top + 0.5f * (kTipRowHeight - kTipIconSize);
      // Baseline that centres the text's cap height on the row.
      const f32 baseline =
          row_top + 0.5f * kTipRowHeight + 0.36f * kTipFontPixels;
      // Icons always sit right against their word. Right column: words
      // line up after the widest combo. Left column: each row is pushed
      // right so its word ends at the column's right edge.
      f32 x = right ? left + icon_column - tip_icons_width(row)
                    : left + width - text_width(row.word.c_str()) - kTipTextGap -
                          tip_icons_width(row);
      for (int k = 0; k < 2 && row.icons[k]; ++k) {
        if (k > 0) {
          x += kTipIconGap;
          draw_shadowed_text("+", glm::vec2(x, baseline), kTipPlus);
          x += kTipAverageGlyphWidth + kTipIconGap;
        }
        renderer_draw_image(std::string("ui/gamepad/") + row.icons[k] + ".png",
                            glm::vec2(x, icon_top), glm::vec2(kTipIconSize),
                            glm::vec4(1.0f, 1.0f, 1.0f, tip_alpha_));
        x += kTipIconSize;
      }
      const f32 text_x = right ? left + icon_column + kTipTextGap
                               : x + kTipTextGap;
      draw_shadowed_text(row.word.c_str(), glm::vec2(text_x, baseline), kTipText);
    }
  }

  static f32 text_width(const char *text) {
    return static_cast<f32>(std::string_view(text).size()) *
           kTipAverageGlyphWidth;
  }

  // alpha < 0: the tooltip's current fade.
  void draw_shadowed_text(const char *text, glm::vec2 position,
                          glm::vec4 colour, f32 alpha = -1.0f) const {
    const glm::vec4 fade(1.0f, 1.0f, 1.0f, alpha < 0.0f ? tip_alpha_ : alpha);
    renderer_draw_text(text, position + glm::vec2(kTipShadowOffset),
                       kTipShadow * fade, kTipFontScale);
    renderer_draw_text(text, position, colour * fade, kTipFontScale);
  }

  // --- Undo / redo -----------------------------------------------------------
  //
  // Whole-scene snapshots: every object's stamps, plus the tool's own state
  // about them (groups, coats/style/effects, selection). An action marks
  // history_dirty_; once it's over (nothing held, R2/L2 up) the scene is
  // captured as one step. Restoring rebuilds only the objects that differ
  // -- deleted ones are re-stamped (under new ids, remapped through the
  // whole history), clones are deleted, moved ones are re-stamped where
  // they were -- and puts the tool state back.

  struct ObjectSnapshot {
    i32 id = -1;
    std::vector<SculptStampInfo> stamps;
  };
  struct SceneSnapshot {
    std::vector<ObjectSnapshot> objects;
    std::vector<Group> groups;
    std::unordered_map<i32, ObjectCoat> coats;
    std::vector<i32> selection;
  };
  static constexpr size_t kMaxUndoSteps = 64;

  static bool same_stamp(const SculptStampInfo &a, const SculptStampInfo &b) {
    return a.center == b.center && a.shape_type == b.shape_type &&
           a.params == b.params && a.smoothness == b.smoothness &&
           a.op == b.op && a.material_slot == b.material_slot &&
           a.looseness == b.looseness && a.rotation == b.rotation;
  }
  static bool same_object(const ObjectSnapshot &a, const ObjectSnapshot &b) {
    return a.stamps.size() == b.stamps.size() &&
           std::equal(a.stamps.begin(), a.stamps.end(), b.stamps.begin(),
                      same_stamp);
  }

  SceneSnapshot capture_scene() const {
    SceneSnapshot snap;
    const i32 count = renderer_sculpt_object_ids(nullptr, 0);
    std::vector<i32> ids(static_cast<size_t>(std::max(count, 0)));
    renderer_sculpt_object_ids(ids.data(), count);
    for (i32 id : ids) {
      ObjectSnapshot object;
      object.id = id;
      object.stamps.resize(
          static_cast<size_t>(renderer_sculpt_object_stamps(id, nullptr, 0)));
      renderer_sculpt_object_stamps(id, object.stamps.data(),
                                    static_cast<i32>(object.stamps.size()));
      snap.objects.push_back(std::move(object));
    }
    snap.groups = groups_;
    snap.coats = coats_;
    for (auto &[id, coat] : snap.coats) {
      coat.light = -1; // lights are runtime, re-made by sync_glow_lights()
    }
    snap.selection = selection_;
    return snap;
  }

  // Called every frame: closes the step an action has been building once
  // the action is over.
  void commit_history_if_idle() {
    if (!history_dirty_ || grabbed_object_ >= 0 || rotating_object_ >= 0 ||
        r2_.down || l2_.down || chain_.active || flip_object_ >= 0) {
      return;
    }
    history_dirty_ = false;
    undo_stack_.push_back(std::move(committed_));
    if (undo_stack_.size() > kMaxUndoSteps) {
      undo_stack_.erase(undo_stack_.begin());
    }
    committed_ = capture_scene();
    redo_stack_.clear();
  }

  void undo() {
    if (undo_stack_.empty()) {
      KINFO("Nothing to undo.");
      return;
    }
    redo_stack_.push_back(capture_scene());
    SceneSnapshot target = std::move(undo_stack_.back());
    undo_stack_.pop_back();
    restore_scene(target);
    KINFO("Undo ({} more).", undo_stack_.size());
  }

  void redo() {
    if (redo_stack_.empty()) {
      KINFO("Nothing to redo.");
      return;
    }
    undo_stack_.push_back(capture_scene());
    SceneSnapshot target = std::move(redo_stack_.back());
    redo_stack_.pop_back();
    restore_scene(target);
    KINFO("Redo ({} more).", redo_stack_.size());
  }

  void restore_scene(SceneSnapshot &target) {
    const SceneSnapshot current = capture_scene();
    // Objects already exactly as the target has them stay untouched.
    std::vector<i32> remove;
    std::vector<bool> kept(target.objects.size(), false);
    for (const ObjectSnapshot &now : current.objects) {
      bool keep = false;
      for (size_t i = 0; i < target.objects.size(); ++i) {
        if (target.objects[i].id == now.id && same_object(target.objects[i], now)) {
          kept[i] = keep = true;
          break;
        }
      }
      if (!keep) {
        remove.push_back(now.id);
      }
    }
    if (!remove.empty()) {
      renderer_sculpt_delete_objects(remove.data(), static_cast<i32>(remove.size()));
    }
    // Everything else is re-stamped as the target had it.
    std::unordered_map<i32, i32> new_id;
    const i32 previous_object = renderer_sculpt_current_object();
    for (size_t i = 0; i < target.objects.size(); ++i) {
      if (kept[i]) {
        continue;
      }
      const i32 id = renderer_sculpt_begin_object();
      renderer_sculpt_set_current_object(id);
      for (const SculptStampInfo &stamp : target.objects[i].stamps) {
        renderer_sculpt_stamp_shape(stamp.center, stamp.shape_type, stamp.params,
                                    stamp.smoothness, stamp.op,
                                    stamp.material_slot, stamp.looseness,
                                    stamp.rotation);
      }
      new_id[target.objects[i].id] = id;
    }
    renderer_sculpt_set_current_object(previous_object);
    // Re-stamped objects have new ids: follow them through all of history.
    if (!new_id.empty()) {
      remap_snapshot(target, new_id);
      remap_snapshot(committed_, new_id);
      for (SceneSnapshot &snap : undo_stack_) {
        remap_snapshot(snap, new_id);
      }
      for (SceneSnapshot &snap : redo_stack_) {
        remap_snapshot(snap, new_id);
      }
    }

    // The tool's own state.
    grabbed_object_ = rotating_object_ = flip_object_ = -1;
    held_group_.clear();
    chain_ = CloneChain{};
    groups_ = target.groups;
    for (const Group &group : groups_) {
      next_group_id_ = std::max(next_group_id_, group.id + 1);
    }
    for (auto &[id, coat] : coats_) {
      release_glow_light(coat);
    }
    coats_ = target.coats;
    free_coat_slots_.clear();
    for (i32 slot = kPlainSlot + 1; slot < next_coat_slot_; ++slot) {
      if (coat_slot_users(slot) == 0) {
        free_coat_slots_.push_back(slot);
      }
    }
    for (auto &[id, coat] : coats_) {
      apply_coat_material(coat);
    }
    selection_ = target.selection;
    if (scoped_group_ >= 0 && !scoped_group()) {
      scoped_group_ = -1;
    }
    committed_ = std::move(target);
    history_dirty_ = false;
  }

  static void remap_snapshot(SceneSnapshot &snap,
                             const std::unordered_map<i32, i32> &new_id) {
    auto map = [&](i32 id) {
      auto it = new_id.find(id);
      return it == new_id.end() ? id : it->second;
    };
    for (ObjectSnapshot &object : snap.objects) {
      object.id = map(object.id);
    }
    for (Group &group : snap.groups) {
      for (i32 &member : group.members) {
        member = map(member);
      }
    }
    std::unordered_map<i32, ObjectCoat> coats;
    for (auto &[id, coat] : snap.coats) {
      coats[map(id)] = coat;
    }
    snap.coats = std::move(coats);
    for (i32 &id : snap.selection) {
      id = map(id);
    }
  }

  std::string stamps_path_;
  Camera camera_;
  f32 screen_width_ = 1280.0f;
  f32 screen_height_ = 800.0f;
  glm::vec3 brush_{0.0f, 0.0f, kDefaultBrushDepth};

  TriggerButton r2_{GamepadAxis::RightTrigger};
  TriggerButton l2_{GamepadAxis::LeftTrigger};
  i32 grabbed_object_ = -1;
  i32 rotating_object_ = -1;
  // What the current grab/rotation moves: the held object's group.
  std::vector<i32> held_group_;
  glm::vec3 object_center_{0.0f};
  std::vector<i32> selection_;
  std::vector<Group> groups_;

  // The clone chain being built (see clone_held_group()). Each link is a
  // copy of the originals at chain position k (0 = the originals, 1 = the
  // held copy); center/rotation/scale are what has been applied to it so
  // far, relative to the originals.
  struct CloneChain {
    bool active = false;
    std::vector<i32> originals;
    glm::vec3 start_center{0.0f};
    glm::quat held_rotation{1.0f, 0.0f, 0.0f, 0.0f}; // held copy vs originals
    f32 held_scale = 1.0f;
    struct Link {
      std::vector<i32> objects;
      bool beyond = false;
      f32 k = 0.0f;
      glm::vec3 center{0.0f};
      glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
      f32 scale = 1.0f;
    };
    std::vector<Link> links;
  };
  CloneChain chain_;
  i32 next_group_id_ = 1;
  i32 scoped_group_ = -1; // the scoped-in group's id, or -1
  int hovered_button_ = -1; // toolbar button under the reticle, or -1
  f32 toolbar_slide_ = 0.0f; // 0 = slid out, 1 = in (see button_shown())
  // Top bar: the menu asked for, the one whose row is on screen (kept while
  // it animates closed), its drop progress, and the hovered top-bar id.
  EditMode mode_ = EditMode::Default;
  // Coat mode state: the tool, add/subtract, the picked colour (index +
  // shade) and finish, each object's coat, and the material slots.
  CoatTool coat_tool_ = CoatTool::Tint;
  StyleTool style_tool_ = StyleTool::Looseness;
  EffectsTool effects_tool_ = EffectsTool::Boil;
  // Undo / redo (see capture_scene()).
  SceneSnapshot committed_;               // the scene as of the last step
  std::vector<SceneSnapshot> undo_stack_; // older steps, newest last
  std::vector<SceneSnapshot> redo_stack_; // undone steps, newest last
  bool history_dirty_ = false;            // an action has changed things
  std::unordered_map<i32, f32> loaded_looseness_; // see adopt_loaded_looseness()
  int fleck_style_index_ = kDefaultFleckStyle;
  bool coat_subtract_ = false;
  int colour_index_ = 0;
  int shade_index_ = kCoatShadeCount / 2;
  int finish_index_ = 0;
  bool coating_ = false; // R2 is coating something this frame
  std::unordered_map<i32, ObjectCoat> coats_;
  i32 next_coat_slot_ = kPlainSlot + 1;
  std::vector<i32> free_coat_slots_;
  std::vector<i32> free_glow_lights_; // engine lights switched off, for reuse
  bool coat_slots_warned_ = false;
  bool shades_open_ = false;
  f32 shades_progress_ = 0.0f;
  Menu open_menu_ = Menu::None;
  Menu shown_menu_ = Menu::None;
  f32 menu_progress_ = 0.0f;
  int hovered_top_ = -1;
  // The object under the reticle this frame when not holding one, else -1
  // (set at the top of render()).
  i32 hovered_object_ = -1;
  // Tooltip fade: progress 0..1, its eased alpha, and the rows last shown
  // (kept so they can fade out after the reticle leaves the object).
  f32 tip_fade_ = 0.0f;
  f32 tip_alpha_ = 0.0f;
  std::vector<TipRow> tip_rows_;

  i32 flip_object_ = -1;
  std::vector<i32> flip_group_;
  glm::vec3 flip_axis_{1.0f, 0.0f, 0.0f};
  glm::vec3 flip_pivot_{0.0f};
  f32 flip_remaining_ = 0.0f;

  f32 start_held_time_ = 0.0f;
  bool recenter_fired_ = false;
  bool recentering_ = false;
  f32 recenter_time_ = 0.0f;
  glm::vec3 recenter_from_{0.0f};

  bool frame_combo_was_held_ = false;
  bool framing_ = false;
  f32 frame_time_ = 0.0f;
  glm::vec3 frame_from_{0.0f};
  glm::vec3 frame_to_{0.0f};
  glm::vec3 frame_from_brush_{0.0f};
  glm::vec3 frame_to_brush_{0.0f};
};

} // namespace

int run_controller_mode(const char *stamps_path) {
  // Same sequence as the engine's entry.h main().
  Memory::initialize();
  int exit_code = 0;
  {
    ControllerSculptGame game(stamps_path ? stamps_path : "");
    Application app(&game);
    if (!app.application_start()) {
      KINFO("Application did not shut down gracefully.");
      exit_code = 2;
    }
  }
  Memory::shutdown();
  return exit_code;
}
