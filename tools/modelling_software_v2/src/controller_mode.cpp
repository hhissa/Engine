// Qt's signals/slots/emit keyword macros would clash with names here
// (ObjectCoat::slots()); only QImage is used, for library thumbnails.
#define QT_NO_KEYWORDS
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

#include <QImage>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <filesystem>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Controller mode: the sculpt scene in the engine's own window, with no Qt.
// Controls follow "Gamepad controls-1.docx" (next to this tool's
// CMakeLists.txt). That plan is incomplete and will change. So far:
//
//   Left stick        moves the brush left/right (X) and up/down (Y) --
//                     past the screen edge the camera strafes instead
//   L1 + left stick   left/right moves the brush (X, same edge rule);
//                     up/down moves the camera and brush together
//                     forward/back (Z)
//   Right stick       look (while a smear/spray stroke is being drawn it
//                     slides the camera instead, so the line stays straight)
//   Both sticks snap to a straight axis when pushed within ~15 degrees of
//   one, so a slightly off-centre thumb doesn't bend a line.
//   R1 (hold)         orbit the camera around the brush point:
//                       left stick  left/right swings around, up/down zooms
//                       right stick left/right swings around, up/down tilts
//   L1 + R1           move the camera to frame the object under the
//                     reticle (or the whole sculpt if there isn't one)
//   R2 (hold)         grab the object under the reticle:
//                       left stick  moves it left/right, up/down
//                       R1/L1 + left stick  moves it left/right,
//                                   forward/back
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
//             time, so they can be selected and moved individually. With
//             one object selected (on its own, or a member picked inside a
//             scoped group -- even a single shape): only that object is in
//             focus, and Sculpt mode then edits it instead of starting a new
//             sculpture. Press it again (or B) to scope back out a level.
//   Delete    deletes every selected object (as does Y on one of them).
// Shortcuts: D-pad up presses Group, D-pad down presses Scope.
//   (one placeholder button that does nothing yet)
//   Shape Options  always there, in every mode (see Sculpt mode below)
// The object buttons slide in from the right edge while something is
// selected (or scoped into) and slide back out when nothing is.
//
// Top bar: buttons at the top centre, each opening a row of buttons
// underneath it (pressing the open one again closes it). Normally it holds
// Tools and Modes (Sculpt, Paint, Coat, Style, Effects -- all but Paint
// work), and Guides. Inside a mode, an Exit button in the top-right corner
// (or L1 + B) goes back to the normal controls.
//
// Move: the first tool in every Tools row, base mode's included (where it is
// the only one, and always on). Picked inside a mode it stands that mode's
// tool down and gives the normal controls back -- grab, rotate, select,
// delete, flip, Group / Scope -- without leaving the mode; picking any other
// tool goes back to it. In sculpt mode the sculpture itself can be moved but
// not deleted.
//
// Guides: settings that hold in every mode, on every mode's top bar (last).
//   Grid Snap         a held object's anchor (its first stamp) moves from
//                     grid corner to grid corner, and sculpting places its
//                     shapes on them; the corners near the reticle are
//                     drawn, the one in use in orange
//   Grid Size         opens a row of cell sizes, 1/16 to 2 units
//   Upright           objects can't tip over: rotate (L2) only turns them
//                     about the vertical (left/right), and L3 flips turn a
//                     quarter turn left/right instead of tipping
//   90 Degree Angles  (with Upright) those turns, and R2 + right stick
//                     left/right, go in quarter turns, in place
//   Precise Move      while grabbing, arrows show the move so far along X,
//                     Y (up) and Z, each labelled with its distance
//   Surface Snap      the sculpt brush sits on the surface under the
//                     reticle; a held object slides over surfaces, resting
//                     against them
//   Tentacle Snap     the same until something is put down -- the first
//                     shape of a stroke, or a held object's first move --
//                     then free for the rest of that stroke or grab (pull a
//                     smear out of a surface). One snap or the other.
//   Mirror            sculpting places each shape again, reflected
//                     left/right through the sculpture's point (its first
//                     shape)
//   Kaleidoscope      sculpting places each shape 2 to 16 times, turned
//                     evenly about the vertical through that point; with
//                     the Guides row open, D-pad left/right sets how many.
//                     With Mirror too, each copy is mirrored as well. The
//                     point, the copies' places and the count are drawn.
//                     (Stamp, Smear and Spray Paint)
//
// Search: finds every .sculpt file on the computer (the library,
// bin/assets/library, and the home folder, skipping hidden folders). Its row
// is Save, then the sculptures found as thumbnails, newest first.
//   Save        stores the selection -- or, with nothing selected, the
//               whole scene -- as library/sculpture_NNN.sculpt with its
//               colours, finishes, flecks and effects, plus a thumbnail
//               (.png beside it) drawn from the current view
//   (a sculpture)  imports it onto the reticle: R2 stamps a copy into the
//               scene each press, D-pad up/down resizes, B (or Y) puts it
//               away
//
// Sculpt mode: entering it starts a new, empty sculpture (or, scoped into an
// object, carries on editing that object; leaving keeps the scope), and everything
// else is blurred and greyed out (as when scoped in) while it is made. The
// shape is placed at the brush point (the reticle), shown there as a
// translucent preview. Moving around is as normal; otherwise:
//   R2         uses the current tool at the brush point
//   Y          switches between adding and subtracting
//   L1 + Y     switches between the Stamp and Smear tools
//   L2 (hold)  turns the brush shape with the sticks, as L2 turns an object
//   L3 + left stick direction  tips the brush shape 90 degrees that way
//   D-pad up/down  grows / shrinks the brush
//   L1 + B     leaves sculpt mode (as does the Exit button)
// The top bar is Tools | Add/Subtract | Shapes | Colour | Flecks |
// Finishes, opening on Shapes with the Smear tool picked. Colour and
// Finishes are the coat mode rows, Flecks the style mode fleck styles:
// together the look of what is placed next. Tools:
//   Stamp        places the shape once per press (subtract: carves it out)
//   Smear        places it continuously while R2 is held, along the
//                brush's path
//   Spray Paint  paints the sculpture's surface inside the shape with the
//                picked colour, finish and fleck (subtract: back to plain).
//                With it picked, the top bar gains Opacity (10% to 100%):
//                one stroke covers that share of the surface, in fine
//                speckles; going over it again with a new stroke builds up
//   Crop         keeps only the part of the sculpture inside the shape
//   Cut Out      cuts the shape out of the sculpture and makes the piece
//                its own sculpture (greyed out with everything else)
// Sculpt mode's edits only ever touch the sculpture (SculptBrushOp::
// Object*, renderer_types.inl), never anything around it. A sculpture is a
// group of its strokes: after the first, each Stamp press or Smear stroke
// that adds is an object of its own in the sculpture's group, so scoped
// into it later each stroke moves on its own; carving, Spray Paint, Crop
// and Cut Out work on all of them.
// Shape Options (the side column's bottom button, in every mode) opens a
// vertical panel beside the column -- Smoothness and Section open their
// choices in a second column next to it; B closes them -- that shapes what
// is placed (by every tool):
//   Precise Scale  (sculpt mode, any tool that places the shape: Stamp,
//                  Smear, Spray Paint, Crop, Cut Out) pins the shape mid-view with a
//                  handle on each face; R2 on a handle drags that face (the
//                  opposite one stays put, whole grid cells with Grid Snap
//                  on); B is done. Shapes with separate sizes per axis only
//                  -- not Sphere, Torus, Octahedron or Link
//   Smoothness     how softly a placed (or carved) shape blends into the
//                  sculpture -- the smooth union / subtraction width, from
//                  Sharp to Softest; every shape
//   Section        keeps a half, quarter or eighth: the round shapes
//                  (not the box family)
//   Colour Variation  a set of colours the tools that use the picked colour
//                  (Stamp, Smear, Spray Paint; coat mode's Tint and Hue) go
//                  through in place of it -- Switch or Blend; also the last
//                  button of the Colour row (see variation_items())
//   Reset          back to the plain shape
// Picking another shape resets Precise Scale's stretch.
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
// A stick pushed within this angle of straight left/right/up/down counts as
// exactly that direction. A thumb never pushes perfectly straight, and
// without this the small sideways part bends every line drawn.
constexpr f32 kStickAxisSnap = 0.26f; // radians, ~15 degrees
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

// Guides (top bar, default mode).
// Kaleidoscope: how many copies about the point, from / to / at first.
constexpr int kKaleidoscopeMin = 2;
constexpr int kKaleidoscopeMax = 16;
constexpr int kKaleidoscopeDefault = 6;
// Buttons in the Guides row (it wraps to two lines; the Grid Size row goes
// under both).
constexpr int kGuideCount = 9;
// Surface snaps find the surface's facing from two more rays this far
// (radians) to the side of the first.
constexpr f32 kSurfaceNormalSpread = 0.002f;
constexpr int kGridSizeCount = 6;
constexpr f32 kGridSizes[kGridSizeCount] = {0.0625f, 0.125f, 0.25f,
                                            0.5f,    1.0f,   2.0f};
constexpr const char *kGridSizeNames[kGridSizeCount] = {"1/16", "1/8", "1/4",
                                                        "1/2",  "1",   "2"};
constexpr const char *kGridSizeIcons[kGridSizeCount] = {
    "ui/toolbar/grid_16.png", "ui/toolbar/grid_8.png", "ui/toolbar/grid_4.png",
    "ui/toolbar/grid_2.png",  "ui/toolbar/grid_1.png", "ui/toolbar/grid_0.png"};
constexpr int kDefaultGridSize = 2; // 1/4, the sculpt cell size
// Grid corners are drawn out to this many cells from the reticle.
constexpr int kGridShowCells = 2;
constexpr f32 kQuarterTurn = 1.5707963f; // 90 degrees
// Precise move arrows: one colour per axis (X red, Y green, Z blue).
constexpr glm::vec4 kAxisColours[3] = {{1.0f, 0.35f, 0.3f, 1.0f},
                                       {0.4f, 1.0f, 0.4f, 1.0f},
                                       {0.35f, 0.6f, 1.0f, 1.0f}};

// Shape options (sculpt mode).
constexpr int kSmoothnessCount = 6;
// How wide the smooth blend is where a placed (or carved) shape meets the
// sculpture -- the smooth union / subtraction radius, in brush sizes. The
// soft end reaches well past the shape itself (the bulge is about a quarter
// of the radius), so Softest melts a shape into whatever is near it.
constexpr f32 kSmoothnessLevels[kSmoothnessCount] = {0.0f, 0.05f, 0.12f,
                                                     0.4f, 1.0f,  2.0f};
// Spray Paint's opacity levels: the share of the surface one stroke covers
// (see paint_covers() in Builtin.SculptChunkRecompute.comp.glsl).
constexpr int kOpacityCount = 5;
constexpr f32 kOpacityLevels[kOpacityCount] = {0.1f, 0.25f, 0.5f, 0.75f, 1.0f};
constexpr const char *kOpacityNames[kOpacityCount] = {"10%", "25%", "50%", "75%",
                                                      "100%"};
constexpr int kDefaultOpacity = kOpacityCount - 1;
// The brush's looseness (Stamp, Smear): what each shape placed carries as
// its own looseness -- loose flecks on that part only. The shader draws the
// larger of a shape's looseness and its object's (style mode's); style
// mode's Looseness takes the shapes' over (take_stamped_looseness()).
constexpr int kBrushLoosenessCount = 5;
constexpr f32 kBrushLoosenessLevels[kBrushLoosenessCount] = {0.0f, 0.25f, 0.5f,
                                                             0.75f, 1.0f};
constexpr const char *kBrushLoosenessNames[kBrushLoosenessCount] = {
    "0%", "25%", "50%", "75%", "100%"};

constexpr const char *kSmoothnessNames[kSmoothnessCount] = {
    "Sharp", "Slight", "Normal", "Soft", "Softer", "Softest"};
constexpr int kDefaultSmoothness = 2; // Normal
constexpr int kSectionCount = 4;
constexpr const char *kSectionNames[kSectionCount] = {"Whole", "Half",
                                                      "Quarter", "Eighth"};
constexpr const char *kSectionIcons[kSectionCount] = {
    "ui/toolbar/section_0.png", "ui/toolbar/section_1.png",
    "ui/toolbar/section_2.png", "ui/toolbar/section_3.png"};
constexpr f32 kHandleReach = 18.0f;   // pixels: how near a handle counts as on it
constexpr f32 kMinShapeSize = 0.02f;  // smallest size Precise Scale allows
constexpr f32 kMinShapeScale = 0.05f; // and the stretch limits per axis
constexpr f32 kMaxShapeScale = 20.0f;

// Reticle (pixels).
constexpr f32 kReticleGap = 5.0f;
constexpr f32 kReticleArm = 9.0f;
constexpr glm::vec4 kReticleIdle{1.0f, 1.0f, 1.0f, 0.9f};
constexpr glm::vec4 kReticleOverObject{1.0f, 0.85f, 0.3f, 1.0f};
constexpr glm::vec4 kReticleGrabbing{0.4f, 1.0f, 0.5f, 1.0f};
constexpr glm::vec4 kReticleRotating{0.4f, 0.85f, 1.0f, 1.0f};
// The reticle sits at the brush point's depth, and parts of it behind
// geometry are hidden: each arm is tested in this many pieces, a hidden
// piece keeps this much of its alpha, and a surface within this fraction
// of the distance in front of it (one it sits on) doesn't count.
constexpr int kReticlePieces = 3;
constexpr f32 kReticleHiddenAlpha = 0.18f;
constexpr f32 kReticleDepthSlack = 0.01f;
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

// Sculpt mode: brush size (roughly the shape's radius), how far apart a
// smear or spray stroke places its shapes, and how wide a blend adding or
// carving makes (both in brush sizes).
constexpr f32 kSculptDefaultSize = 0.3f;
constexpr f32 kSculptMinSize = 0.03f;
constexpr f32 kSculptMaxSize = 3.0f;
constexpr f32 kSculptStrokeSpacing = 0.3f;
// Colour Variation: how far along a smear or spray stroke, in brush sizes,
// one colour lasts (Switch) or fades into the next (Blend); how many steps
// a fade takes (each step is its own material slot); and how many colours
// the set holds.
constexpr f32 kVariationLength = 4.0f;
constexpr int kVariationBlendSteps = 6;
constexpr int kMaxVariationColours = 6;
// Stamp's Colour Variation runs on a clock, so each stamp takes whatever
// colour the set is on when R2 goes down: colours of the set per second.
// Fast enough that presses can't easily be timed to it, so stamps come out
// mixed; a non-round rate, so a steady rhythm of presses doesn't keep
// landing on the same colour.
constexpr f32 kStampVariationRate = 3.7f;
constexpr int kSculptMaxStrokeStamps = 16; // per frame, if the brush jumps

// Coat mode.
constexpr f32 kCoatFullTime = 1.5f; // seconds of R2 to go from 0 to 100%
constexpr glm::vec3 kPlainColour{0.38f, 0.38f, 0.38f};
constexpr i32 kPlainSlot = 2;     // material slot every uncoated object uses
// A glowing object's light: intensity at full glow, per unit of object
// radius squared (point lights fall off with 1/distance^2 from its centre).
constexpr f32 kGlowLightIntensity = 5.0f;
constexpr i32 kMaxCoatSlot = 998; // the engine has 1000 material slots...
constexpr i32 kPreviewSlot = 999; // ...and the last shows sculpt's brush preview
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

// A whole stick as (right, up): a round deadzone on the stick's distance
// from centre (a per-axis one lets the small sideways wobble through once
// the other axis is past it), then the axis snap above.
glm::vec2 stick(GamepadAxis x_axis, GamepadAxis y_axis) {
  glm::vec2 v(input::gamepad_axis(x_axis), input::gamepad_axis(y_axis));
#if !KPLATFORM_WINDOWS
  v.y = -v.y; // see stick_up()
#endif
  const f32 magnitude = glm::length(v);
  if (magnitude < kStickDeadzone) {
    return glm::vec2(0.0f);
  }
  const f32 scaled =
      std::min((magnitude - kStickDeadzone) / (1.0f - kStickDeadzone), 1.0f);
  v *= scaled / magnitude;
  const f32 snap = std::tan(kStickAxisSnap);
  if (std::fabs(v.y) <= std::fabs(v.x) * snap) {
    v = glm::vec2(std::copysign(scaled, v.x), 0.0f);
  } else if (std::fabs(v.x) <= std::fabs(v.y) * snap) {
    v = glm::vec2(0.0f, std::copysign(scaled, v.y));
  }
  return v;
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

// --- Benchmark ----------------------------------------------------------------
// MSV2_BENCH="x y z" runs a fixed script instead of waiting for a pad: let the
// scene settle, time frames, then sculpt a new object at (x, y z) the way the
// Smear tool does (one stamp a frame, along a small circle), then pile stamps
// onto one spot, then time frames again. Results go to the log as "BENCH"
// lines, ending with "BENCH DONE". For measuring, not a feature.
class SculptBench {
public:
  static constexpr int kSettleFrames = 240;
  static constexpr int kFrameSamples = 300;
  static constexpr int kSmearStamps = 90;
  static constexpr int kPileStamps = 90;

  bool start_from_env() {
    const char *spec = std::getenv("MSV2_BENCH");
    if (!spec) {
      return false;
    }
    std::sscanf(spec, "%f %f %f", &at_.x, &at_.y, &at_.z);
    if (const char *move = std::getenv("MSV2_BENCH_MOVE")) {
      // "FRAMES [STEP]": with a STEP, move only that way (no return trip).
      std::sscanf(move, "%d %f", &move_frames_, &move_step_);
    }
    on_ = true;
    KINFO("BENCH script on, editing at ({}, {}, {})", at_.x, at_.y, at_.z);
    return true;
  }

  bool on() const { return on_; }

  // cpu_ms: this frame's update + render time on the CPU.
  void frame(f32 dt, f64 cpu_ms) {
    if (!on_ || done_) {
      return;
    }
    ++frame_;
    const int measure_start = kSettleFrames;
    const int smear_start = measure_start + kFrameSamples;
    const int pile_start = smear_start + kSmearStamps;
    const int move_start = pile_start + kPileStamps;
    const int after_start = move_start + move_frames_ + kSettleFrames;
    const int end = after_start + kFrameSamples;
    if (frame_ > measure_start && frame_ <= smear_start) {
      dts_.push_back(dt * 1000.0);
      cpus_.push_back(cpu_ms);
      if (frame_ == smear_start) {
        report_frames("idle");
        object_ = renderer_sculpt_begin_object();
        if (const char *loose = std::getenv("MSV2_BENCH_LOOSE")) {
          renderer_sculpt_set_material_flecks(1, static_cast<f32>(std::atof(loose)),
                                              1, 0.0f, 0.0f);
        }
      }
    } else if (frame_ > smear_start && frame_ <= pile_start) {
      const f32 t = static_cast<f32>(frame_ - smear_start) * 0.12f;
      stamp(at_ + glm::vec3(std::cos(t), std::sin(t), 0.0f) * 0.8f);
      if (frame_ == pile_start) {
        report_edits("smear");
      }
    } else if (frame_ > pile_start && frame_ <= pile_start + kPileStamps) {
      const f32 t = static_cast<f32>(frame_);
      stamp(at_ + glm::vec3(std::sin(t * 1.7f), std::cos(t * 2.3f),
                            std::sin(t * 0.9f)) * 0.05f);
      if (frame_ == pile_start + kPileStamps) {
        report_edits("pile");
      }
    } else if (frame_ > move_start && frame_ <= move_start + move_frames_) {
      // Drag the sculpture away along +Z and back, like R2 + L1 + stick.
      const int i = frame_ - move_start;
      const f32 step = move_step_ != 0.0f
                           ? move_step_
                           : (i * 2 <= move_frames_ ? 1.0f : -1.0f) * 0.05f;
      dts_.push_back(dt * 1000.0);
      cpus_.push_back(cpu_ms);
      const auto begin = std::chrono::steady_clock::now();
      renderer_sculpt_transform_objects(&object_, 1, at_,
                                        glm::quat(1.0f, 0.0f, 0.0f, 0.0f),
                                        glm::vec3(0.0f, 0.0f, step), 1.0f);
      edits_.push_back(std::chrono::duration<f64, std::milli>(
                           std::chrono::steady_clock::now() - begin)
                           .count());
      if (i == move_frames_) {
        report_frames("moving");
        report_edits("move");
      }
    } else if (frame_ > after_start && frame_ <= end) {
      dts_.push_back(dt * 1000.0);
      cpus_.push_back(cpu_ms);
      if (frame_ == end) {
        report_frames("after");
        KINFO("BENCH DONE");
        done_ = true;
      }
    }
  }

private:
  int move_frames_ = 0;
  f32 move_step_ = 0.0f;

  void stamp(glm::vec3 center) {
    SculptStampInfo info;
    info.center = center;
    info.shape_type = SculptBrushShape::Sphere;
    info.params = glm::vec4(0.35f, 0.0f, 0.0f, 0.0f);
    info.smoothness = 0.15f;
    info.op = SculptBrushOp::ObjectUnion;
    info.material_slot = 1;
    const auto begin = std::chrono::steady_clock::now();
    renderer_sculpt_add_stamps(object_, &info, 1);
    edits_.push_back(std::chrono::duration<f64, std::milli>(
                         std::chrono::steady_clock::now() - begin)
                         .count());
  }

  static void summarize(std::vector<f64> &v, f64 *mean, f64 *p95, f64 *max) {
    *mean = 0.0;
    for (f64 x : v) {
      *mean += x;
    }
    *mean /= std::max<size_t>(v.size(), 1);
    std::sort(v.begin(), v.end());
    *p95 = v.empty() ? 0.0 : v[v.size() * 95 / 100];
    *max = v.empty() ? 0.0 : v.back();
  }

  void report_frames(const char *label) {
    f64 mean, p95, max, cmean, cp95, cmax;
    summarize(dts_, &mean, &p95, &max);
    summarize(cpus_, &cmean, &cp95, &cmax);
    KINFO("BENCH frames[{}] frame mean {:.2f} p95 {:.2f} max {:.2f} ms | "
          "cpu update+render mean {:.2f} p95 {:.2f} max {:.2f} ms",
          label, mean, p95, max, cmean, cp95, cmax);
    dts_.clear();
    cpus_.clear();
  }

  void report_edits(const char *label) {
    const f64 first = edits_.empty() ? 0.0 : edits_.front();
    const f64 last = edits_.empty() ? 0.0 : edits_.back();
    f64 mean, p95, max;
    summarize(edits_, &mean, &p95, &max);
    KINFO("BENCH edits[{}] {} stamps: mean {:.2f} p95 {:.2f} max {:.2f} ms "
          "(first {:.2f}, last {:.2f})",
          label, edits_.size(), mean, p95, max, first, last);
    edits_.clear();
  }

  bool on_ = false;
  bool done_ = false;
  int frame_ = 0;
  glm::vec3 at_{0.0f, 0.0f, 3.0f};
  i32 object_ = -1;
  std::vector<f64> dts_;
  std::vector<f64> cpus_;
  std::vector<f64> edits_;
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
    bench_.start_from_env();
    if (!stamps_path_.empty()) {
      const auto begin = std::chrono::steady_clock::now();
      load_stamps(stamps_path_);
      KINFO("BENCH load {:.1f} ms",
            std::chrono::duration<f64, std::milli>(
                std::chrono::steady_clock::now() - begin)
                .count());
    }
    adopt_loaded_coats(); // before the rest go plain: it remaps file slots
    make_everything_plain();
    adopt_loaded_looseness();
    committed_ = capture_scene(); // the state undo returns to first
    refresh_library(); // search for sculptures now, ready for Search
    return true;
  }

  b8 update(f32 dt) override {
    frame_begin_ = std::chrono::steady_clock::now();
    variation_clock_ = std::fmod(variation_clock_ + dt, 1000.0f);
    sync_welds();
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
      if (mode_ == EditMode::Sculpt && !move_tool_) {
        flip_brush(); // tips the brush shape, not an object
      } else {
        begin_flip();
      }
    }
    update_flip(dt);
    // In coat mode R2 coats instead of grabbing (see update_coat()).
    update_manipulation_state(
        !modeless() || placing() ? false : r2_pressed,
        placing() ? false : l2_pressed, l1);
    if (held_object() < 0) {
      guide_object_ = -1;
    }
    if (grabbed_object_ < 0) {
      tentacle_object_ = -1; // the next grab snaps afresh
      move_start_object_ = -1;
    } else if (move_start_object_ != grabbed_object_) {
      move_start_object_ = grabbed_object_;
      move_start_ = object_center_;
    }
    update_toolbar_slide(dt);
    update_menu(dt);
    hovered_button_ = button_under_reticle();
    hovered_top_ = hovered_button_ < 0 ? top_bar_under_reticle() : -1;
    if (pressed(GamepadButton::A)) {
      if (hovered_button_ >= 0) {
        press_button(static_cast<ToolbarButton>(hovered_button_));
      } else if (hovered_top_ >= 0) {
        press_top_bar(hovered_top_);
      } else if ((mode_ != EditMode::Sculpt || move_tool_) && !placing()) {
        toggle_selection();
      }
    }
    if (pressed(GamepadButton::B) && l1 && mode_ != EditMode::Default) {
      exit_mode(); // L1 + B: leave the mode
    } else if (pressed(GamepadButton::B)) {
      if (placing()) {
        end_placing();
      } else if (precise_scale_) {
        end_precise_scale();
      } else if (shades_open_) {
        shades_open_ = false; // hide the shades row first
      } else if (side_sub_open_) {
        side_sub_open_ = false;
      } else if (side_open_) {
        close_side_panel();
      } else if (scoped() && mode_ != EditMode::Sculpt) {
        scope_out();
      }
    }
    update_coat(dt);
    update_style(dt);
    update_effects(dt);
    update_sculpt(dt, r2_pressed);
    sync_glow_lights();
    if (pressed(GamepadButton::Y) && l1 && mode_ == EditMode::Sculpt &&
        !placing()) {
      toggle_stamp_smear(); // L1 + Y: Stamp <-> Smear
    } else if (pressed(GamepadButton::Y) && placing()) {
      end_placing();
    } else if (pressed(GamepadButton::Y) && !modeless()) {
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
    } else if (kaleidoscope_ && open_menu_ == Menu::Guides &&
               (pressed(GamepadButton::DpadLeft) ||
                pressed(GamepadButton::DpadRight))) {
      // With the Guides row open, D-pad left / right: fewer / more
      // kaleidoscope copies.
      step_kaleidoscope(pressed(GamepadButton::DpadRight) ? 1 : -1);
    } else if (grabbed_object_ < 0 && rotating_object_ < 0 && !r2_.down &&
               !placing()) {
      // Otherwise D-pad left / right: undo / redo; up / down: the Group
      // and Scope buttons' shortcuts.
      if (pressed(GamepadButton::DpadLeft)) {
        undo();
      } else if (pressed(GamepadButton::DpadRight)) {
        redo();
      } else if (pressed(GamepadButton::DpadUp) &&
                 button_enabled(ToolbarButton::Group)) {
        press_button(ToolbarButton::Group);
      } else if (pressed(GamepadButton::DpadDown) &&
                 button_enabled(ToolbarButton::Scope)) {
        press_button(ToolbarButton::Scope);
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
    } else if (mode_ == EditMode::Sculpt && !move_tool_ && l2_.down) {
      update_brush_rotate(dt);
    } else if (l1 && r1) {
      // The framing combo -- the sticks do nothing while it's held.
    } else if (r1) {
      update_orbit(dt);
    } else if (l1) {
      update_move_mode(dt); // forward/back
      update_look(dt);
    } else {
      update_strafe_mode(dt); // left/right, up/down
      update_look(dt);
    }

    update_chain(); // after this frame's moves of the held copy
    update_placing(dt, r2_pressed);
    poll_library_scan();
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
    draw_grid_guide();
    draw_symmetry_guide();
    draw_precise_move();
    draw_scale_handles();
    draw_reticle();
    draw_tooltip(dt);
    bench_.frame(dt, std::chrono::duration<f64, std::milli>(
                         std::chrono::steady_clock::now() - frame_begin_)
                         .count());
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
    const std::unordered_map<i32, i32> engine_id_for =
        sculpt_shared::replay_document(document);
    adopt_file_welds(path, engine_id_for);
    // The file's look lines (see the library format, "Search" below),
    // applied by adopt_loaded_coats() once the engine side is set up.
    for (auto &[file_id, coat] : read_file_coats(path)) {
      if (auto it = engine_id_for.find(file_id); it != engine_id_for.end()) {
        loaded_coats_[it->second] = std::move(coat);
      }
    }
    // Per object: the lowest and highest looseness its shapes carry.
    std::unordered_map<i32, std::pair<f32, f32>> range;
    for (const SculptStampRecord &stamp : document.stamps()) {
      // (A paint stamp's looseness is its opacity, not looseness.)
      if (stamp.op == SculptBrushOp::ObjectPaint) {
        continue;
      }
      auto [it, fresh] = range.try_emplace(
          stamp.object_id, std::make_pair(stamp.looseness, stamp.looseness));
      if (!fresh) {
        it->second.first = std::min(it->second.first, stamp.looseness);
        it->second.second = std::max(it->second.second, stamp.looseness);
      }
    }
    for (const auto &[file_id, lo_hi] : range) {
      if (lo_hi.first > 0.0f && lo_hi.first == lo_hi.second) {
        if (auto it = engine_id_for.find(file_id); it != engine_id_for.end()) {
          loaded_looseness_[it->second] = lo_hi.first;
        }
      }
    }
    KINFO("Loaded {} stamp(s) from '{}'.", document.stamps().size(), path);
  }

  void adopt_loaded_coats() {
    for (const auto &[object, coat] : loaded_coats_) {
      adopt_file_coat(object, coat);
    }
    loaded_coats_.clear();
  }

  // A file's looseness that is the same on every shape of an object (as the
  // Qt tool wrote it) becomes that object's style-mode looseness, and the
  // stamps go back to 0: the shader draws the larger of the two, so leaving
  // it on the stamps would stop Subtract going below it. Looseness that
  // differs from shape to shape is the sculpt brush's (kBrushLoosenessLevels)
  // and stays on its shapes.
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

  // A world point's pixel position (same projection as reticle_screen()),
  // or false if it's behind the camera.
  bool project(glm::vec3 world, glm::vec2 *out) const {
    const glm::vec3 c = to_camera_space(world);
    if (c.z < 0.05f) {
      return false;
    }
    *out = glm::vec2(0.5f * screen_width_ + (c.x / c.z) * screen_height_,
                     0.5f * screen_height_ - (c.y / c.z) * screen_height_);
    return true;
  }

  // --- Guides ----------------------------------------------------------------
  //
  // Set from the Guides menu (on every mode's top bar); they apply in every
  // mode. Grid Snap puts a held object's anchor (its first stamp, the same
  // point Y's snap uses) and the sculpt brush's shapes on grid corners.
  // Upright keeps objects from tipping: rotate and flip only turn them
  // about the vertical, in quarter turns with 90 Degree Angles on. Precise
  // Move shows how far a held object has moved, per axis.

  f32 grid_size() const { return kGridSizes[grid_size_index_]; }
  glm::vec3 snap_to_grid(glm::vec3 p) const {
    return glm::round(p / grid_size()) * grid_size();
  }
  // Where sculpting places a shape: the brush point, on the grid if Grid
  // Snap is on.
  glm::vec3 placement_point() const {
    return grid_snap_ ? snap_to_grid(brush_world()) : brush_world();
  }
  bool quarter_turns() const { return upright_ && right_angles_; }

  // --- Surface Snap / Tentacle Snap ------------------------------------------
  //
  // Surface Snap: the sculpt brush sits on the surface under the reticle
  // (it goes to that depth, so the shape is placed there), and a held
  // object slides over the surface under it, resting against it. Tentacle
  // Snap starts the same, then lets go: the brush snaps until R2 goes down
  // -- the first shape lands on the surface -- and then moves freely from
  // there for the rest of the stroke, so a smear pulls out of the surface
  // like a tentacle; a held object snaps onto the surface the first time
  // it moves, then moves freely for the rest of that grab. The two are one
  // or the other.

  bool surface_snapping() const { return surface_snap_ || tentacle_snap_; }

  // What a snap can land on: the objects in scope, or (nothing scoped) all
  // of them, less `exclude`.
  std::vector<i32> snap_targets(const std::vector<i32> &exclude = {}) const {
    std::vector<i32> ids = scoped() ? scope_members() : all_object_ids();
    std::erase_if(ids, [&](i32 id) {
      return std::find(exclude.begin(), exclude.end(), id) != exclude.end();
    });
    return ids;
  }

  // The first surface of `ids` along a ray (unit `dir`): where, and which
  // way it faces (toward the ray's origin), from two more rays just beside
  // it -- or straight back along the ray where they find no clean surface
  // (an edge).
  // stamp_limit >= 0: against the scene as it was at that stamp count.
  bool surface_hit(glm::vec3 origin, glm::vec3 dir, const std::vector<i32> &ids,
                   glm::vec3 *point, glm::vec3 *normal,
                   i32 stamp_limit = -1) const {
    if (ids.empty()) {
      return false;
    }
    auto cast = [&](glm::vec3 d, glm::vec3 *at) {
      f32 t = 0.0f;
      if (renderer_sculpt_raycast_objects_before(
              origin, d, kPickMaxDistance, ids.data(),
              static_cast<i32>(ids.size()), stamp_limit, &t) < 0) {
        return false;
      }
      *at = origin + d * t;
      return true;
    };
    if (!cast(dir, point)) {
      return false;
    }
    glm::vec3 side = glm::cross(dir, glm::vec3(0.0f, 1.0f, 0.0f));
    side = glm::length(side) < 1e-4f ? glm::vec3(1.0f, 0.0f, 0.0f)
                                     : glm::normalize(side);
    const glm::vec3 up = glm::cross(side, dir);
    *normal = -dir;
    glm::vec3 a, b;
    const f32 reach = 20.0f * kSurfaceNormalSpread * glm::length(*point - origin);
    if (cast(glm::normalize(dir + side * kSurfaceNormalSpread), &a) &&
        cast(glm::normalize(dir + up * kSurfaceNormalSpread), &b) &&
        glm::length(a - *point) < reach && glm::length(b - *point) < reach) {
      const glm::vec3 n = glm::cross(a - *point, b - *point);
      if (glm::length(n) > 1e-12f) {
        *normal = glm::normalize(n);
        if (glm::dot(*normal, dir) > 0.0f) {
          *normal = -*normal;
        }
      }
    }
    return true;
  }

  // Sculpt mode: puts the brush on the surface under the reticle (see
  // above). Tentacle Snap only while R2 is up, and on the frame it goes
  // down. Mid-stroke, Surface Snap follows the surface as it was when the
  // stroke began: snapping to the shapes the stroke itself is laying down
  // would walk the brush out toward the camera, a shape at a time.
  void update_surface_brush(bool r2_pressed) {
    const bool mid_stroke = r2_.down && !r2_pressed;
    if (!mid_stroke) {
      snap_stamp_limit_ = renderer_sculpt_stamp_count();
    }
    if (!surface_snapping() || precise_scale_ || framing_) {
      return;
    }
    if (tentacle_snap_ && mid_stroke) {
      return; // free
    }
    const glm::vec3 origin = camera_.position();
    const glm::vec3 dir = glm::normalize(brush_world() - origin);
    glm::vec3 point, normal;
    if (surface_hit(origin, dir, snap_targets(), &point, &normal,
                    snap_stamp_limit_)) {
      brush_ = to_camera_space(point);
    }
  }

  // A held group about to move to centre `target`: where it would rest on
  // the surface under it instead (on the line of sight through `target`,
  // its bounds against the surface). False if there's no surface under it.
  bool rest_on_surface(glm::vec3 target, glm::vec3 *out) const {
    const glm::vec3 origin = camera_.position();
    const glm::vec3 dir = glm::normalize(target - origin);
    glm::vec3 point, normal;
    if (!surface_hit(origin, dir, snap_targets(held_group_), &point, &normal)) {
      return false;
    }
    bool any = false;
    glm::vec3 lo_all(0.0f), hi_all(0.0f);
    for (i32 object : held_group_) {
      glm::vec3 lo, hi;
      if (renderer_sculpt_object_bounds(object, &lo, &hi)) {
        lo_all = any ? glm::min(lo_all, lo) : lo;
        hi_all = any ? glm::max(hi_all, hi) : hi;
        any = true;
      }
    }
    const glm::vec3 half = any ? 0.5f * (hi_all - lo_all) : glm::vec3(0.0f);
    // Back along the line of sight (so it stays where it's pointed at) until
    // its bounds clear the surface along its facing. Pushing straight out
    // along the facing instead slid it off rounded edges, where the facing
    // turns away, and it never got past them.
    const f32 clearance = glm::dot(glm::abs(normal), half);
    const f32 facing = std::max(glm::dot(normal, -dir), 0.25f);
    *out = point - dir * (clearance / facing);
    return true;
  }

  // --- Mirror / Kaleidoscope --------------------------------------------------
  //
  // Sculpting (Stamp, Smear, Spray Paint) with Mirror on places each shape
  // twice, the second reflected left/right (across the world's X) through
  // the sculpture's point; with Kaleidoscope on, kaleidoscope_count_ times,
  // turned evenly about the vertical through that point. Both on: each
  // kaleidoscope copy mirrored too. The point is the sculpture's anchor
  // (its first shape), so a new sculpture's first shape sets it. With the
  // Guides row open, D-pad left/right sets how many copies.

  bool symmetry_on() const {
    return (mirror_ || kaleidoscope_) && mode_ == EditMode::Sculpt &&
           !move_tool_ &&
           (sculpt_tool_ == SculptTool::Stamp || sculpt_tool_ == SculptTool::Smear ||
            sculpt_tool_ == SculptTool::Spray);
  }

  glm::vec3 symmetry_point(glm::vec3 fallback) const {
    glm::vec3 anchor;
    glm::quat unused;
    if (sculpture_ >= 0 && renderer_sculpt_object_anchor(sculpture_, &anchor, &unused)) {
      return anchor;
    }
    return fallback;
  }

  // Each of `stamps` and its copies (the original first).
  std::vector<SculptStampInfo> with_symmetry(
      const std::vector<SculptStampInfo> &stamps, glm::vec3 point) const {
    const int count = kaleidoscope_ ? kaleidoscope_count_ : 1;
    std::vector<SculptStampInfo> out;
    out.reserve(stamps.size() * static_cast<size_t>(count) * (mirror_ ? 2 : 1));
    for (const SculptStampInfo &stamp : stamps) {
      for (int k = 0; k < count; ++k) {
        const glm::quat turn = glm::angleAxis(
            4.0f * kQuarterTurn * static_cast<f32>(k) / static_cast<f32>(count),
            glm::vec3(0.0f, 1.0f, 0.0f));
        SculptStampInfo copy = stamp;
        copy.center = point + turn * (stamp.center - point);
        copy.rotation = glm::normalize(turn * stamp.rotation);
        out.push_back(copy);
        if (mirror_) {
          SculptStampInfo reflected = copy;
          reflected.center.x = 2.0f * point.x - copy.center.x;
          // Reflecting across X: (w, x, y, z) -> (w, x, -y, -z).
          const glm::quat q = copy.rotation;
          reflected.rotation = glm::quat(q.w, q.x, -q.y, -q.z);
          if (glm::length(reflected.center - copy.center) > 1e-4f) {
            out.push_back(reflected);
          }
        }
      }
    }
    return out;
  }

  // The point (an orange cross), and where each copy of the next shape
  // goes (rings); a mirror's plane as a faint square, the kaleidoscope's
  // axis as a faint line, and its count by the point.
  void draw_symmetry_guide() {
    if (!symmetry_on() || framing_ || brush_.z <= 0.01f) {
      return;
    }
    const glm::vec3 at = placement_point();
    const glm::vec3 point = symmetry_point(at);
    const glm::vec4 faint(1.0f, 1.0f, 1.0f, 0.35f);
    const glm::vec4 orange(1.0f, 0.75f, 0.25f, 1.0f);
    auto line = [&](glm::vec3 a3, glm::vec3 b3, glm::vec4 colour) {
      glm::vec2 a, b;
      if (project(a3, &a) && project(b3, &b)) {
        renderer_draw_line(a, b, colour);
      }
    };
    const f32 size = std::max(brush_size_ * 1.5f, 0.25f);
    if (mirror_) {
      const glm::vec3 y(0.0f, size, 0.0f), z(0.0f, 0.0f, size);
      line(point - y - z, point - y + z, faint);
      line(point - y + z, point + y + z, faint);
      line(point + y + z, point + y - z, faint);
      line(point + y - z, point - y - z, faint);
    }
    if (kaleidoscope_) {
      line(point - glm::vec3(0.0f, size, 0.0f), point + glm::vec3(0.0f, size, 0.0f),
           faint);
    }
    const f32 arm = 0.08f * size;
    for (int axis = 0; axis < 3; ++axis) {
      glm::vec3 offset(0.0f);
      offset[axis] = arm;
      line(point - offset, point + offset, orange);
    }
    SculptStampInfo probe;
    probe.center = at;
    const std::vector<SculptStampInfo> copies = with_symmetry({probe}, point);
    for (size_t i = 1; i < copies.size(); ++i) {
      glm::vec2 c;
      if (project(copies[i].center, &c)) {
        renderer_draw_image("ui/coat/swatch.png", c - glm::vec2(7.0f),
                            glm::vec2(14.0f), glm::vec4(0.4f, 0.95f, 1.0f, 0.9f));
      }
    }
    glm::vec2 label;
    if (kaleidoscope_ && project(point, &label)) {
      const std::string text = "x" + std::to_string(kaleidoscope_count_);
      draw_shadowed_text(text.c_str(), label + glm::vec2(10.0f, -kTipFontPixels - 6.0f),
                         orange, 1.0f);
    }
  }

  void step_kaleidoscope(int change) {
    kaleidoscope_count_ = std::clamp(kaleidoscope_count_ + change, kKaleidoscopeMin,
                                     kKaleidoscopeMax);
    KINFO("Kaleidoscope: {} copies.", kaleidoscope_count_);
  }

  // Yaw (turn about the world vertical) of an orientation: the angle of its
  // forward (+Z) axis in the ground plane, +X being +90 degrees -- the same
  // sense glm::angleAxis(a, +Y) turns things.
  static f32 yaw_of(glm::quat rotation) {
    const glm::vec3 f = rotation * glm::vec3(0.0f, 0.0f, 1.0f);
    if (f.x * f.x + f.z * f.z > 1e-4f) {
      return std::atan2(f.x, f.z);
    }
    // Forward points straight up or down: use its X axis instead.
    const glm::vec3 x = rotation * glm::vec3(1.0f, 0.0f, 0.0f);
    return std::atan2(x.x, x.z) - kQuarterTurn;
  }
  static f32 wrap_angle(f32 a) { return std::remainder(a, 4.0f * kQuarterTurn); }

  i32 held_object() const {
    return grabbed_object_ >= 0 ? grabbed_object_ : rotating_object_;
  }

  // Starts tracking the held object's unsnapped anchor and yaw, if it isn't
  // already. False if there's nothing to track.
  bool begin_guided_move() {
    const i32 object = held_object();
    if (object < 0) {
      return false;
    }
    if (guide_object_ == object) {
      return true;
    }
    glm::vec3 anchor;
    glm::quat rotation;
    if (!renderer_sculpt_object_anchor(object, &anchor, &rotation)) {
      return false;
    }
    guide_target_ = anchor;
    guide_yaw_ = yaw_of(rotation);
    guide_object_ = object;
    return true;
  }

  // 90 Degree Angles: turns the held object about the vertical through its
  // centre by `delta` of stick motion, but only ever to a multiple of 90
  // degrees (an object at some other angle squares up to the nearest one
  // on its first turn).
  void turn_in_quarters(f32 delta) {
    if (!begin_guided_move()) {
      return;
    }
    glm::vec3 anchor;
    glm::quat rotation;
    if (!renderer_sculpt_object_anchor(held_object(), &anchor, &rotation)) {
      return;
    }
    guide_yaw_ += delta;
    const f32 target = std::round(guide_yaw_ / kQuarterTurn) * kQuarterTurn;
    const f32 change = wrap_angle(target - yaw_of(rotation));
    if (std::fabs(change) < 1e-4f) {
      return;
    }
    const glm::quat turn = glm::angleAxis(change, glm::vec3(0.0f, 1.0f, 0.0f));
    const glm::vec3 centre = object_center_;
    guide_target_ = centre + turn * (guide_target_ - centre);
    move_group(held_group_, centre, turn, glm::vec3(0.0f));
  }

  // Grid Snap: the cell corners near the reticle, as small 3D crosses that
  // fade with distance. The corner the brush (or the held object's anchor)
  // is on is drawn larger, in orange.
  void draw_grid_guide() {
    if (!grid_snap_ || brush_.z <= 0.01f || framing_) {
      return;
    }
    if ((hovered_button_ >= 0 || hovered_top_ >= 0) && grabbed_object_ < 0) {
      return; // pointing at the UI
    }
    const f32 g = grid_size();
    const glm::vec3 focus = brush_world();
    const glm::vec3 base = snap_to_grid(focus);
    glm::vec3 marked = base;
    glm::vec3 anchor;
    glm::quat unused;
    if (grabbed_object_ >= 0 &&
        renderer_sculpt_object_anchor(grabbed_object_, &anchor, &unused)) {
      marked = anchor;
    }
    const f32 reach = static_cast<f32>(kGridShowCells) + 0.5f;
    for (int i = -kGridShowCells; i <= kGridShowCells; ++i) {
      for (int j = -kGridShowCells; j <= kGridShowCells; ++j) {
        for (int k = -kGridShowCells; k <= kGridShowCells; ++k) {
          const glm::vec3 p = base + glm::vec3(i, j, k) * g;
          const f32 d = glm::length(p - focus) / g;
          if (d > reach) {
            continue;
          }
          const bool on = glm::length(p - marked) < 0.01f * g;
          const f32 fade = 1.0f - d / reach;
          const glm::vec4 colour =
              on ? glm::vec4(1.0f, 0.75f, 0.25f, 1.0f)
                 : glm::vec4(1.0f, 1.0f, 1.0f, 0.15f + 0.6f * fade * fade);
          const f32 arm = (on ? 0.22f : 0.12f) * g;
          for (int axis = 0; axis < 3; ++axis) {
            glm::vec3 offset(0.0f);
            offset[axis] = arm;
            glm::vec2 a, b;
            if (project(p - offset, &a) && project(p + offset, &b)) {
              renderer_draw_line(a, b, colour);
            }
          }
        }
      }
    }
  }

  // Precise Move: while an object is held, how far it has moved since it
  // was grabbed -- one arrow per axis, laid end to end (X, then Y, then Z)
  // from where it started, each labelled with its distance, plus a faint
  // line straight from start to now. Y is shown with up as positive.
  void draw_precise_move() {
    if (!precise_move_ || grabbed_object_ < 0 || move_start_object_ < 0) {
      return;
    }
    const glm::vec3 delta = object_center_ - move_start_;
    glm::vec2 a, b;
    if (glm::length(delta) > 1e-4f && project(move_start_, &a) &&
        project(object_center_, &b)) {
      renderer_draw_line(a, b, glm::vec4(1.0f, 1.0f, 1.0f, 0.35f));
    }
    static constexpr const char *kAxisNames[3] = {"X", "Y", "Z"};
    glm::vec3 from = move_start_;
    for (int axis = 0; axis < 3; ++axis) {
      glm::vec3 step(0.0f);
      step[axis] = delta[axis];
      const glm::vec3 to = from + step;
      // Up on screen is world -Y (see the orientation note above).
      const f32 shown = axis == 1 ? -delta[axis] : delta[axis];
      if (std::fabs(delta[axis]) >= 0.005f && project(from, &a) &&
          project(to, &b)) {
        const glm::vec4 colour = kAxisColours[axis];
        renderer_draw_line(a, b, colour);
        const glm::vec2 along = b - a;
        const f32 length = glm::length(along);
        glm::vec2 label_offset(8.0f, -8.0f);
        if (length > 1.0f) {
          const glm::vec2 dir = along / length;
          const glm::vec2 side(-dir.y, dir.x);
          const f32 head = std::min(10.0f, 0.4f * length);
          renderer_draw_line(b, b - dir * head + side * (0.5f * head), colour);
          renderer_draw_line(b, b - dir * head - side * (0.5f * head), colour);
          label_offset = side * 14.0f - glm::vec2(0.0f, 0.5f * kTipFontPixels);
        }
        char text[32];
        std::snprintf(text, sizeof(text), "%s %+.2f", kAxisNames[axis], shown);
        draw_shadowed_text(text, 0.5f * (a + b) + label_offset, colour, 1.0f);
      }
      from = to;
    }
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

  // L1 + left stick: left/right moves the brush (X); up/down moves the camera
  // -- and the brush with it, since the brush lives in camera space --
  // forward/back along the view direction (Z).
  void update_move_mode(f32 dt) {
    const glm::vec2 left = stick(GamepadAxis::LeftX, GamepadAxis::LeftY);
    const f32 x = left.x;
    const f32 z = left.y;
    if (x != 0.0f) {
      move_brush_across(0, x * kBrushScreenSpeed * brush_.z * dt);
    }
    if (z != 0.0f) {
      camera_.move(camera_.forward() * (z * kForwardSpeed * dt));
    }
  }

  // Left stick: brush X and Y.
  void update_strafe_mode(f32 dt) {
    const glm::vec2 left = stick(GamepadAxis::LeftX, GamepadAxis::LeftY);
    const f32 x = left.x;
    const f32 y = left.y;
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
  //
  // While a smear/spray stroke is being drawn, turning would swing the
  // brush (which rides in camera space) around an arc centred on the
  // camera, so the line would curve. Instead the right stick slides the
  // camera across the screen, which carries the brush along a straight
  // line at the same speed the left stick moves it.
  void update_look(f32 dt) {
    const glm::vec2 right = stick(GamepadAxis::RightX, GamepadAxis::RightY);
    const f32 x = right.x;
    const f32 y = right.y;
    if (mode_ == EditMode::Sculpt && stroke_active_) {
      const f32 speed = kBrushScreenSpeed * brush_.z * dt;
      camera_.move(camera_.right() * (x * speed) + screen_up() * (y * speed));
      return;
    }
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
    if (upright_) {
      // Upright: no tipping over. Left/right turns it a quarter turn about
      // the vertical instead; up/down does nothing.
      if (std::fabs(up) >= std::fabs(x)) {
        KINFO("Flip: Upright is on, so it can only turn left or right.");
        return;
      }
      flip_axis_ = glm::vec3(0.0f, x > 0.0f ? -1.0f : 1.0f, 0.0f);
    } else if (std::fabs(up) >= std::fabs(x)) {
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
      guide_object_ = -1; // the flip moved the anchor
    }
  }

  // --- Objects ---------------------------------------------------------------

  // The object hit by the line of sight through the reticle, or -1. When
  // something is hit and out_hit is given, it receives the hit point.
  i32 pick_object_under_reticle(glm::vec3 *out_hit = nullptr) const {
    const glm::vec3 origin = camera_.position();
    const glm::vec3 direction = glm::normalize(brush_world() - origin);
    f32 t = 0.0f;
    // Scoped in: only what's in scope can be picked.
    const std::vector<i32> in_scope = scope_members();
    const i32 object =
        scoped() ? renderer_sculpt_raycast_objects(
                       origin, direction, kPickMaxDistance, in_scope.data(),
                       static_cast<i32>(in_scope.size()), &t)
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
    // Style and effects modes have no rotate: L2 does nothing there. In
    // sculpt mode it turns the brush shape instead (update_brush_rotate()).
    if (l2_pressed && rotating_object_ < 0 && grabbed_object_ < 0 &&
        (modeless() || mode_ == EditMode::Coat)) {
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

  // Scope has two levels: a group, and inside it (or on its own) one
  // object. Scoped into an object, sculpt mode edits that object.
  bool scoped() const { return scoped_object_ >= 0 || scoped_group_ >= 0; }

  // What's in focus while scoped in (empty when not).
  std::vector<i32> scope_members() const {
    if (scoped_object_ >= 0) {
      return {scoped_object_};
    }
    if (const Group *group = scoped_group()) {
      return group->members;
    }
    return {};
  }

  // Whether Scope would go a level deeper: into the selected group, or into
  // the one selected object (ungrouped, or a member inside the scoped group).
  bool can_scope_in() const {
    if (scoped_object_ >= 0) {
      return false;
    }
    if (scoped_group_ >= 0) {
      return selection_.size() == 1;
    }
    return selected_group() != nullptr ||
           (selection_.size() == 1 && group_of(selection_.front()) == nullptr);
  }

  bool object_exists(i32 object) const {
    const std::vector<i32> ids = all_object_ids();
    return std::find(ids.begin(), ids.end(), object) != ids.end();
  }

  // What selecting or grabbing `object` takes: its whole group, except when
  // scoped in, where members are picked one at a time.
  std::vector<i32> unit_of(i32 object) const {
    if (!scoped()) {
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
    if (std::find(objects.begin(), objects.end(), scoped_object_) != objects.end()) {
      scoped_object_ = -1; // its scope goes with it (back to the group, if any)
    }
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

  enum class ToolbarButton { Group, Scope, Delete, Placeholder, ShapeOptions };

  // What the side column holds: Shape Options always (in every mode), and
  // outside sculpt mode, with something selected (or scoped in), the object
  // buttons above it.
  std::vector<ToolbarButton> side_buttons() const {
    if (mode_ != EditMode::Sculpt && (!selection_.empty() || scoped())) {
      return {ToolbarButton::Group, ToolbarButton::Scope, ToolbarButton::Delete,
              ToolbarButton::Placeholder, ToolbarButton::ShapeOptions};
    }
    return {ToolbarButton::ShapeOptions};
  }
  int side_count() const { return static_cast<int>(shown_side_.size()); }

  // The side column's x: the right edge of the area the reticle can reach.
  f32 side_home_x() const {
    const f32 reach_x = 0.5f * kScreenEdge * screen_width_;
    return 0.5f * screen_width_ + reach_x - kButtonMargin - kButtonRadius;
  }

  // Button centres, top to bottom: a column at the right edge of the area
  // the reticle can reach, centred vertically.
  glm::vec2 button_center(int index) const {
    const f32 reach_x = 0.5f * kScreenEdge * screen_width_;
    const f32 step = 2.0f * kButtonRadius + kButtonGap;
    const f32 first_y = 0.5f * screen_height_ -
                        0.5f * step * static_cast<f32>(side_count() - 1);
    const f32 home_x = side_home_x();
    (void)reach_x;
    // Slid out: fully past the right edge of the screen.
    const f32 hidden_x = screen_width_ + kButtonRadius + 2.0f;
    const f32 shown = button_shown(index);
    const f32 eased = 1.0f - (1.0f - shown) * (1.0f - shown) * (1.0f - shown);
    return glm::vec2(glm::mix(hidden_x, home_x, eased),
                     first_y + step * static_cast<f32>(index));
  }


  // How far one button is slid in, 0..1: the column's slide progress,
  // staggered so the buttons follow one another.
  f32 button_shown(int index) const {
    const f32 total = kToolbarSlideTime +
                      kToolbarSlideStagger * static_cast<f32>(std::max(side_count() - 1, 0));
    const f32 t = toolbar_slide_ * total -
                  kToolbarSlideStagger * static_cast<f32>(index);
    return std::clamp(t / kToolbarSlideTime, 0.0f, 1.0f);
  }

  // Slides the column in when it has buttons to show; a change of buttons
  // slides the old ones out first.
  void update_toolbar_slide(f32 dt) {
    const std::vector<ToolbarButton> wanted = side_buttons();
    if (toolbar_slide_ <= 0.0f) {
      shown_side_ = wanted;
    }
    const f32 total = kToolbarSlideTime +
                      kToolbarSlideStagger * static_cast<f32>(std::max(side_count() - 1, 0));
    const f32 step = dt / total;
    toolbar_slide_ = !wanted.empty() && wanted == shown_side_
                         ? std::min(toolbar_slide_ + step, 1.0f)
                         : std::max(toolbar_slide_ - step, 0.0f);
  }

  int button_under_reticle() const {
    if (brush_.z <= 0.01f) {
      return -1;
    }
    const glm::vec2 reticle = reticle_screen();
    for (int i = 0; i < side_count(); ++i) {
      // Only fully slid-in buttons can be pressed (nearly: rounding can leave
      // the last one a hair short of 1).
      if (button_shown(i) < 0.99f) {
        continue;
      }
      if (glm::length(reticle - button_center(i)) <=
          kButtonRadius + kButtonHoverSlop) {
        return static_cast<int>(shown_side_[i]);
      }
    }
    return -1;
  }

  bool button_enabled(ToolbarButton button) const {
    switch (button) {
    case ToolbarButton::Group:
      // Ungroup a selected group, or group 2+ selected objects. Not while
      // scoped in (the group being edited stays as it is).
      return !scoped() &&
             (selected_group() != nullptr || selection_.size() >= 2);
    case ToolbarButton::Scope:
      return scoped() || can_scope_in();
    case ToolbarButton::Delete:
      return !selection_.empty();
    case ToolbarButton::ShapeOptions:
      return true;
    default:
      return false;
    }
  }

  bool button_active(ToolbarButton button) const {
    switch (button) {
    case ToolbarButton::Group:
      return !scoped() && selected_group() != nullptr;
    case ToolbarButton::Scope:
      return scoped();
    case ToolbarButton::ShapeOptions:
      return side_open_ || precise_scale_;
    default:
      return false;
    }
  }

  const char *button_label(ToolbarButton button) const {
    switch (button) {
    case ToolbarButton::Group:
      return button_active(button) ? "Ungroup" : "Group";
    case ToolbarButton::Scope:
      return can_scope_in() || !scoped() ? "Scope in" : "Scope out";
    case ToolbarButton::Delete:
      return "Delete";
    case ToolbarButton::ShapeOptions:
      return "Shape Options";
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
    case ToolbarButton::ShapeOptions:
      return "ui/toolbar/shape_options.png";
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
      if (can_scope_in()) {
        scope_in();
      } else if (scoped()) {
        scope_out();
      }
      break;
    case ToolbarButton::Delete:
      delete_selection();
      break;
    case ToolbarButton::ShapeOptions:
      toggle_side_panel();
      break;
    default:
      KINFO("That button doesn't do anything yet.");
      break;
    }
  }

  // Group button: ungroups a selected group, otherwise makes the selection
  // one group (absorbing any groups inside it).
  void toggle_group() {
    if (scoped()) {
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
    if (!can_scope_in()) {
      KINFO("Scope in: select one object or one group first.");
      return;
    }
    const Group *group = scoped_group_ < 0 ? selected_group() : nullptr;
    if (!group) {
      scoped_object_ = selection_.front();
      selection_.clear();
      KINFO("Scoped into object {}.", scoped_object_);
      return;
    }
    scoped_group_ = group->id;
    selection_.clear(); // members are picked one at a time inside
    KINFO("Scoped into group {}.", scoped_group_);
  }

  void scope_out() {
    if (scoped_object_ >= 0) {
      // Back up a level, leaving the object selected as it was.
      KINFO("Scoped out of object {}.", scoped_object_);
      selection_ = {scoped_object_};
      scoped_object_ = -1;
      return;
    }
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
    if (mode_ == EditMode::Sculpt &&
        std::find(group.begin(), group.end(), sculpture_) != group.end()) {
      KINFO("Delete: that's the sculpture being worked on (leave sculpt mode "
            "to delete it; scoped in, its other strokes can go).");
      return;
    }
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
      // Copied inside a sculpture: they weld into it, as the originals do.
      for (const auto &[original, copy] : copies) {
        if (auto it = welds_.find(original); it != welds_.end()) {
          welds_[copy] = it->second;
        }
      }
    } else {
      copy_welds(copies);
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

  // R2 held: left stick moves the object left/right and up/down -- or, with
  // R1 or L1 also held, left/right and forward/back (level with the ground,
  // so it doesn't drift up or down as the camera tilts); right
  // stick left/right turns it about the point halfway between it and the
  // camera, up/down lifts and lowers it.
  void update_grab(f32 dt) {
    const bool push = held(GamepadButton::RightBumper) ||
                      held(GamepadButton::LeftBumper);
    const f32 lx = stick_x(GamepadAxis::LeftX);
    const f32 left_up = stick_up(GamepadAxis::LeftY);
    const f32 ly = push ? left_up : 0.0f;    // forward/back
    const f32 lift = push ? 0.0f : left_up;  // up/down
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
    // same sense as the camera orbit (see update_orbit()). With 90 Degree
    // Angles on it turns the object in place in quarter turns instead
    // (below), since swinging it would leave it at any angle.
    const glm::quat identity(1.0f, 0.0f, 0.0f, 0.0f);
    const glm::quat turn =
        quarter_turns() ? identity
                        : glm::angleAxis(-rx * kGrabTurnRate * dt,
                                         glm::vec3(0.0f, 1.0f, 0.0f));

    // Resizing is about the object's own centre, so it is applied as a
    // separate step from the turn (which pivots about a different point).
    if (lx != 0.0f || ly != 0.0f || lift != 0.0f || rx != 0.0f ||
        ry != 0.0f) {
      // Grid Snap: move the unsnapped target as the stick says, then put
      // the anchor on the grid point nearest it.
      glm::vec3 anchor;
      glm::quat unused;
      const glm::vec3 moved_center =
          pivot + turn * (object_center_ - pivot) + translation;
      glm::vec3 rest;
      const bool tentacle_free = tentacle_snap_ && tentacle_object_ == grabbed_object_;
      if (surface_snapping() && !tentacle_free &&
          rest_on_surface(moved_center, &rest)) {
        translation += rest - moved_center; // on the surface (not the grid)
        if (tentacle_snap_) {
          tentacle_object_ = grabbed_object_; // landed: free from here
        }
        guide_object_ = -1;
      } else if (grid_snap_ && begin_guided_move() &&
          renderer_sculpt_object_anchor(grabbed_object_, &anchor, &unused)) {
        guide_target_ = pivot + turn * (guide_target_ - pivot) + translation;
        const glm::vec3 turned = pivot + turn * (anchor - pivot);
        translation = snap_to_grid(guide_target_) - turned;
      }
      if (glm::length(translation) > 1e-6f || turn != identity) {
        move_group(held_group_, pivot, turn, translation);
      }
      if (quarter_turns() && rx != 0.0f) {
        turn_in_quarters(-rx * kGrabTurnRate * dt);
      }
    }
    if (resize != 0.0f) {
      const f32 scale = std::exp2(resize * dt / kResizeDoublingTime);
      move_group(held_group_, object_center_,
                 glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(0.0f), scale);
      guide_object_ = -1; // resizing moved the anchor
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
    guide_object_ = -1;
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
    // Upright: only left/right (either stick) does anything -- a turn about
    // the world vertical, in quarter turns with 90 Degree Angles on.
    if (upright_) {
      const f32 spin = std::clamp(lx + rx, -1.0f, 1.0f);
      if (spin == 0.0f) {
        return;
      }
      if (right_angles_) {
        turn_in_quarters(-spin * step);
      } else {
        move_group(held_group_, object_center_,
                   glm::angleAxis(-spin * step, glm::vec3(0.0f, 1.0f, 0.0f)),
                   glm::vec3(0.0f));
      }
      return;
    }
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
    if (mode_ == EditMode::Sculpt) {
      // Sculpting: only the sculpture (all its strokes) is in focus.
      renderer_sculpt_set_scope_objects(sculpture_members_.data(),
                                        static_cast<i32>(sculpture_members_.size()));
      return;
    }
    const std::vector<i32> in_scope = scope_members();
    renderer_sculpt_set_scope_objects(in_scope.empty() ? nullptr : in_scope.data(),
                                      static_cast<i32>(in_scope.size()));
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
    EffectsTools, SculptTools, Shapes, Guides, Search, Opacity, Looseness
  };
  enum class EditMode { Default, Coat, Style, Effects, Sculpt };
  // Sculpt mode's tools (see the header comment).
  // (Looseness isn't a tool: it's a setting of the brush, on the top bar.)
  enum class SculptTool { Stamp, Smear, Spray, Crop, CutOut };
  static constexpr int kSculptToolCount = 5;
  static constexpr const char *kSculptToolNames[kSculptToolCount] = {
      "Stamp", "Smear", "Spray Paint", "Crop", "Cut Out"};
  static constexpr const char *kSculptToolIcons[kSculptToolCount] = {
      "ui/toolbar/stamp.png", "ui/toolbar/smear.png", "ui/toolbar/spray.png",
      "ui/toolbar/crop.png", "ui/toolbar/cutout.png"};
  static std::string brush_looseness_icon(int level) {
    return "ui/toolbar/looseness_" + std::to_string(level) + ".png";
  }
  // Whether the sculpt tool places solid shapes (and so the brush's
  // looseness means something).
  bool tool_places_solid() const {
    return sculpt_tool_ == SculptTool::Stamp || sculpt_tool_ == SculptTool::Smear;
  }
  // The shapes sculpt mode offers: every brush shape but Plane (a half-
  // space, not a shape you can place). Icons by icons/make_shape_icons.py.
  struct SculptShape {
    SculptBrushShape shape;
    const char *name;
  };
  static constexpr int kSculptShapeCount = 14;
  static constexpr SculptShape kSculptShapes[kSculptShapeCount] = {
      {SculptBrushShape::Sphere, "Sphere"},
      {SculptBrushShape::Box, "Cube"},
      {SculptBrushShape::RoundBox, "Rounded Cube"},
      {SculptBrushShape::Ellipsoid, "Ellipsoid"},
      {SculptBrushShape::Capsule, "Capsule"},
      {SculptBrushShape::CappedCylinder, "Cylinder"},
      {SculptBrushShape::CappedCone, "Cone"},
      {SculptBrushShape::RoundCone, "Rounded Cone"},
      {SculptBrushShape::Pyramid, "Pyramid"},
      {SculptBrushShape::Octahedron, "Octahedron"},
      {SculptBrushShape::HexPrism, "Hex Prism"},
      {SculptBrushShape::Torus, "Torus"},
      {SculptBrushShape::Link, "Link"},
      {SculptBrushShape::BoxFrame, "Frame"}};
  static std::string shape_icon(int index) {
    return "ui/shapes/shape_" +
           std::to_string(static_cast<int>(kSculptShapes[index].shape)) + ".png";
  }
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
    f32 opacity = 1.0f;   // a swatch's colour at this opacity
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
  static constexpr int kLayerSide = 4;    // the Shape Options panel
  static constexpr int kLayerSideSub = 5; // its Smoothness / Section column
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
          Menu::CoatTools,
          std::string("Tools: ") + (move_tool_ ? kMoveToolName : kCoatToolNames[tool]),
          move_tool_ ? kMoveToolIcon : kCoatToolIcons[tool]));
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
          Menu::StyleTools,
          std::string("Tools: ") + (move_tool_ ? kMoveToolName : kStyleToolNames[tool]),
          move_tool_ ? kMoveToolIcon : kStyleToolIcons[tool]));
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
    } else if (mode_ == EditMode::Sculpt) {
      const int tool = static_cast<int>(sculpt_tool_);
      items.push_back(menu_button(
          Menu::SculptTools,
          std::string("Tools: ") + (move_tool_ ? kMoveToolName : kSculptToolNames[tool]),
          move_tool_ ? kMoveToolIcon : kSculptToolIcons[tool]));
      UiItem add_sub;
      add_sub.label = coat_subtract_ ? "Subtracting" : "Adding";
      add_sub.icon = coat_subtract_ ? "ui/toolbar/minus.png" : "ui/toolbar/plus.png";
      add_sub.press = [this] { toggle_coat_subtract(); };
      add_sub.shortcut[0] = "y";
      items.push_back(add_sub);
      items.push_back(menu_button(
          Menu::Shapes, std::string("Shape: ") + kSculptShapes[shape_index_].name,
          shape_icon(shape_index_)));
      UiItem colour = menu_button(Menu::Colour, "Colour", "");
      colour.swatch = true;
      colour.colour = coat_colour();
      items.push_back(colour);
      items.push_back(menu_button(
          Menu::FleckStyles,
          std::string("Fleck: ") + kFleckStyleNames[fleck_style_index_],
          fleck_style_icon(fleck_style_index_)));
      UiItem finishes = menu_button(
          Menu::Finishes, std::string("Finish: ") + kCoatFinishes[finish_index_].name,
          "");
      finishes.finish = finish_index_;
      finishes.colour = coat_colour();
      items.push_back(finishes);
      if (sculpt_tool_ == SculptTool::Spray) {
        items.push_back(menu_button(
            Menu::Opacity, std::string("Opacity: ") + kOpacityNames[opacity_index_],
            "ui/toolbar/opacity.png"));
      }
      if (tool_places_solid()) {
        items.push_back(menu_button(
            Menu::Looseness,
            std::string("Looseness: ") + kBrushLoosenessNames[brush_looseness_index_],
            brush_looseness_icon(brush_looseness_index_)));
      }
    } else if (mode_ == EditMode::Effects) {
      const int tool = static_cast<int>(effects_tool_);
      items.push_back(menu_button(
          Menu::EffectsTools,
          std::string("Tools: ") + (move_tool_ ? kMoveToolName : kEffectsToolNames[tool]),
          move_tool_ ? kMoveToolIcon : kEffectsToolIcons[tool]));
      UiItem add_sub;
      add_sub.label = coat_subtract_ ? "Subtracting" : "Adding";
      add_sub.icon = coat_subtract_ ? "ui/toolbar/minus.png" : "ui/toolbar/plus.png";
      add_sub.press = [this] { toggle_coat_subtract(); };
      add_sub.shortcut[0] = "y";
      items.push_back(add_sub);
    } else {
      items.push_back(menu_button(Menu::Tools, std::string("Tools: ") + kMoveToolName,
                                  kMoveToolIcon));
      items.push_back(menu_button(Menu::Modes, "Modes", "ui/toolbar/modes.png"));
      items.push_back(menu_button(Menu::Guides, "Guides", "ui/toolbar/guides.png"));
      items.push_back(menu_button(Menu::Search, "Search", "ui/toolbar/search.png"));
    }
    if (mode_ != EditMode::Default) {
      // Guides hold in every mode, so every mode's bar reaches them (last).
      items.push_back(menu_button(Menu::Guides, "Guides", "ui/toolbar/guides.png"));
    }
    return items;
  }

  // The Move tool, first in every Tools row (base mode's included): while
  // it's picked the mode's own tool stands down and the controls are base
  // mode's -- R2 grabs, L2 rotates, A selects, Y deletes, L3 flips, the
  // D-pad's Group / Scope -- so moving things never means leaving the mode.
  // In base mode it's always on: base mode is the Move tool.
  static constexpr const char *kMoveToolName = "Move";
  static constexpr const char *kMoveToolIcon = "ui/toolbar/move.png";
  bool modeless() const { return mode_ == EditMode::Default || move_tool_; }

  UiItem move_tool_item() {
    UiItem item;
    item.label = kMoveToolName;
    item.icon = kMoveToolIcon;
    item.active = modeless();
    item.press = [this] {
      if (mode_ == EditMode::Default) {
        return; // already moving
      }
      move_tool_ = true;
      KINFO("{} tool: Move.", mode_name());
    };
    return item;
  }

  std::vector<UiItem> row_items(Menu menu) {
    std::vector<UiItem> items;
    switch (menu) {
    case Menu::Modes:
      for (int i = 0; i < kModeCount; ++i) {
        UiItem item;
        item.label = kModeNames[i];
        item.icon = kModeIcons[i];
        item.active = (i == 0 && mode_ == EditMode::Sculpt) ||
                      (i == 2 && mode_ == EditMode::Coat) ||
                      (i == 3 && mode_ == EditMode::Style) ||
                      (i == 4 && mode_ == EditMode::Effects);
        item.press = [this, i] { select_mode(i); };
        items.push_back(item);
      }
      break;
    case Menu::CoatTools:
      items.push_back(move_tool_item());
      for (int i = 0; i < kCoatToolCount; ++i) {
        UiItem item;
        item.label = kCoatToolNames[i];
        item.icon = kCoatToolIcons[i];
        item.active = !move_tool_ && static_cast<int>(coat_tool_) == i;
        item.press = [this, i] {
          coat_tool_ = static_cast<CoatTool>(i);
          move_tool_ = false;
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
      items.push_back(variation_button());
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
      items.push_back(move_tool_item());
      for (int i = 0; i < kStyleToolCount; ++i) {
        UiItem item;
        item.label = kStyleToolNames[i];
        item.icon = kStyleToolIcons[i];
        item.active = !move_tool_ && static_cast<int>(style_tool_) == i;
        item.press = [this, i] {
          style_tool_ = static_cast<StyleTool>(i);
          move_tool_ = false;
          KINFO("Style tool: {}.", kStyleToolNames[i]);
        };
        items.push_back(item);
      }
      break;
    case Menu::EffectsTools:
      items.push_back(move_tool_item());
      for (int i = 0; i < kEffectsToolCount; ++i) {
        UiItem item;
        item.label = kEffectsToolNames[i];
        item.icon = kEffectsToolIcons[i];
        item.active = !move_tool_ && static_cast<int>(effects_tool_) == i;
        item.press = [this, i] {
          effects_tool_ = static_cast<EffectsTool>(i);
          move_tool_ = false;
          KINFO("Effects tool: {}.", kEffectsToolNames[i]);
        };
        items.push_back(item);
      }
      break;
    case Menu::SculptTools:
      items.push_back(move_tool_item());
      for (int i = 0; i < kSculptToolCount; ++i) {
        UiItem item;
        item.label = kSculptToolNames[i];
        item.icon = kSculptToolIcons[i];
        item.active = !move_tool_ && static_cast<int>(sculpt_tool_) == i;
        if (i == static_cast<int>(SculptTool::Stamp) ||
            i == static_cast<int>(SculptTool::Smear)) {
          item.shortcut[0] = "l1"; // L1 + Y swaps the two
          item.shortcut[1] = "y";
        }
        item.press = [this, i] {
          sculpt_tool_ = static_cast<SculptTool>(i);
          move_tool_ = false;
          KINFO("Sculpt tool: {}.", kSculptToolNames[i]);
        };
        items.push_back(item);
      }
      break;
    case Menu::Shapes:
      for (int i = 0; i < kSculptShapeCount; ++i) {
        UiItem item;
        item.label = kSculptShapes[i].name;
        item.icon = shape_icon(i);
        item.active = shape_index_ == i;
        item.press = [this, i] {
          shape_index_ = i;
          shape_scale_ = glm::vec3(1.0f); // stretch was for the old shape
          if (!shape_scalable(kSculptShapes[i].shape)) {
            end_precise_scale();
          }
          KINFO("Sculpt shape: {}.", kSculptShapes[i].name);
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
    case Menu::Search:
      items = search_items();
      break;
    case Menu::Looseness:
      for (int i = 0; i < kBrushLoosenessCount; ++i) {
        UiItem item;
        item.label = std::string("Looseness: ") + kBrushLoosenessNames[i];
        item.icon = brush_looseness_icon(i);
        item.active = brush_looseness_index_ == i;
        item.press = [this, i] {
          brush_looseness_index_ = i;
          KINFO("Brush looseness: {}.", kBrushLoosenessNames[i]);
        };
        items.push_back(item);
      }
      break;
    case Menu::Opacity:
      for (int i = 0; i < kOpacityCount; ++i) {
        UiItem item;
        item.label = std::string("Opacity: ") + kOpacityNames[i];
        item.swatch = true;
        item.colour = coat_colour();
        item.opacity = kOpacityLevels[i];
        item.active = opacity_index_ == i;
        item.press = [this, i] {
          opacity_index_ = i;
          KINFO("Spray opacity: {}.", kOpacityNames[i]);
        };
        items.push_back(item);
      }
      break;
    case Menu::Guides: {
      auto toggle = [&](const char *name, const char *icon, bool *flag) {
        UiItem item;
        item.label = std::string(name) + (*flag ? ": On" : ": Off");
        item.icon = icon;
        item.active = *flag;
        item.press = [this, name, flag] {
          *flag = !*flag;
          guide_object_ = -1; // start any held move afresh
          KINFO("{}: {}.", name, *flag ? "on" : "off");
        };
        return item;
      };
      items.push_back(toggle("Grid Snap", "ui/toolbar/grid.png", &grid_snap_));
      UiItem size;
      size.label = std::string("Grid Size: ") + kGridSizeNames[grid_size_index_] +
                   (grid_size_index_ == 4 ? " unit" : " units");
      size.icon = "ui/toolbar/grid_size.png";
      size.active = shades_open_;
      size.press = [this] {
        shades_open_ = !shades_open_;
        if (shades_open_) {
          shades_progress_ = 0.0f;
        }
      };
      items.push_back(size);
      items.push_back(toggle("Upright", "ui/toolbar/upright.png", &upright_));
      UiItem angles = toggle("90 Degree Angles", "ui/toolbar/angle90.png",
                             &right_angles_);
      // Only means anything while Upright is on (turns about the vertical).
      angles.enabled = upright_;
      angles.active = upright_ && right_angles_;
      if (!upright_) {
        angles.label = "90 Degree Angles (turn on Upright first)";
        angles.press = [] {};
      }
      items.push_back(angles);
      items.push_back(
          toggle("Precise Move", "ui/toolbar/precise.png", &precise_move_));
      // Surface Snap / Tentacle Snap: one or the other.
      auto snap = [&](const char *name, const char *icon, bool *flag, bool *other) {
        UiItem item = toggle(name, icon, flag);
        item.press = [this, name, flag, other] {
          *flag = !*flag;
          if (*flag) {
            *other = false;
          }
          guide_object_ = -1;
          KINFO("{}: {}.", name, *flag ? "on" : "off");
        };
        return item;
      };
      items.push_back(snap("Surface Snap", "ui/toolbar/surface_snap.png",
                           &surface_snap_, &tentacle_snap_));
      items.push_back(snap("Tentacle Snap", "ui/toolbar/tentacle_snap.png",
                           &tentacle_snap_, &surface_snap_));
      items.push_back(toggle("Mirror", "ui/toolbar/mirror.png", &mirror_));
      UiItem kaleidoscope =
          toggle("Kaleidoscope", "ui/toolbar/kaleidoscope.png", &kaleidoscope_);
      if (kaleidoscope_) {
        kaleidoscope.label = "Kaleidoscope: " + std::to_string(kaleidoscope_count_) +
                             " copies (D-pad left/right)";
        kaleidoscope.shortcut[0] = "dpad_left";
        kaleidoscope.shortcut[1] = "dpad_right";
      }
      items.push_back(kaleidoscope);
      break;
    }
    case Menu::Tools: // base mode: Move is all it has
      items.push_back(move_tool_item());
      break;
    default:
      break;
    }
    return items;
  }

  // The Grid Size sub-row (shown in the shades layer, under Guides).
  std::vector<UiItem> grid_size_items() {
    std::vector<UiItem> items;
    for (int i = 0; i < kGridSizeCount; ++i) {
      UiItem item;
      item.label = std::string(kGridSizeNames[i]) + (i == 4 ? " unit" : " units");
      item.icon = kGridSizeIcons[i];
      item.active = grid_size_index_ == i;
      item.press = [this, i] {
        grid_size_index_ = i;
        guide_object_ = -1;
        KINFO("Grid size: {}.", kGridSizes[i]);
      };
      items.push_back(item);
    }
    return items;
  }

  // The third layer: the colour's shades, or (under Guides) grid sizes.
  std::vector<UiItem> sub_items() {
    if (shown_menu_ == Menu::Guides) {
      return grid_size_items();
    }
    return shade_items();
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
    return (shown_menu_ == Menu::Colour || shown_menu_ == Menu::Guides) &&
           shades_progress_ > 0.0f;
  }

  void toggle_menu(Menu menu) {
    if (open_menu_ == menu) {
      open_menu_ = Menu::None; // close; the row animates away
      shades_open_ = false;
    } else {
      if (menu == Menu::Search) {
        refresh_library();
      }
      open_menu_ = menu;
      shown_menu_ = menu;
      menu_progress_ = 0.0f; // drop the new row in from the top
      shades_open_ = false;
      shades_progress_ = 0.0f;
    }
  }

  void select_mode(int index) {
    if (index == 0) {
      enter_sculpt();
      return;
    }
    if (index >= 2 && index <= 4) { // Coat, Style, Effects
      move_tool_ = false;
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
    if (mode_ == EditMode::Sculpt) {
      leave_sculpt();
    }
    mode_ = EditMode::Default;
    move_tool_ = false;
    open_menu_ = Menu::None;
    shades_open_ = false;
  }

  const char *mode_name() const {
    switch (mode_) {
    case EditMode::Coat: return "coat";
    case EditMode::Style: return "style";
    case EditMode::Effects: return "effects";
    case EditMode::Sculpt: return "sculpt";
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

  // L1 + Y in sculpt mode: Smear from Stamp, else Stamp.
  void toggle_stamp_smear() {
    sculpt_tool_ = !move_tool_ && sculpt_tool_ == SculptTool::Stamp
                       ? SculptTool::Smear
                       : SculptTool::Stamp;
    move_tool_ = false;
    stroke_active_ = false;
    KINFO("Sculpt tool: {}.", kSculptToolNames[static_cast<int>(sculpt_tool_)]);
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
  // The shades row sits under the last line of the row above it (a long
  // row wraps, see item_center()), a label's height lower so the names fit.
  f32 shades_home_y() const {
    const f32 line_step = 2.0f * kButtonRadius + kMenuRowGap + kTipFontPixels + 6.0f;
    return row_home_y() + line_step * static_cast<f32>(shaded_row_lines());
  }

  // How many lines the row under a shades row takes: only Colour wraps.
  int shaded_row_lines() const {
    const int count = shown_menu_ == Menu::Colour   ? kCoatColourCount + 1
                      : shown_menu_ == Menu::Guides ? kGuideCount
                                                    : 1;
    return (count + kRowWrap - 1) / kRowWrap;
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
    if (layer == kLayerSide || layer == kLayerSideSub) {
      return side_item_center(layer, index, count, out_shown);
    }
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

  // --- Shape Options panel ----------------------------------------------------
  //
  // The Shape Options side button opens a vertical panel beside the side
  // column (Precise Scale, Smoothness, Section, Reset), sliding out to the
  // left; Smoothness and Section open a second column further left, centred
  // on the button that opened it. It is independent of the top bar, so a
  // top-bar row (Shapes, say) can stay open with it.

  // Precise Scale, Smoothness, Section, Reset (see shape_option_items()).
  static constexpr int kShapeOptionCount = 5;

  f32 side_step() const { return 2.0f * kButtonRadius + kButtonGap; }
  f32 side_panel_x() const { return side_home_x() - side_step() - kButtonGap; }

  glm::vec2 side_item_center(int layer, int index, int count, f32 *out_shown) const {
    const bool sub = layer == kLayerSideSub;
    const f32 shown = staggered(sub ? side_sub_progress_ : side_progress_, index, count);
    *out_shown = shown;
    const f32 step = side_step();
    const f32 offset = step * (static_cast<f32>(index) - 0.5f * static_cast<f32>(count - 1));
    f32 centre_y = 0.5f * screen_height_;
    if (sub) {
      // Centred on the button that opened it, kept inside the reach.
      const int panel_count = kShapeOptionCount;
      centre_y += step * (static_cast<f32>(side_sub_anchor_) -
                          0.5f * static_cast<f32>(panel_count - 1));
      const f32 reach_y = 0.5f * kScreenEdge * screen_height_;
      const f32 half = 0.5f * step * static_cast<f32>(count - 1);
      const f32 lo = 0.5f * screen_height_ - reach_y + kButtonMargin + kButtonRadius + half;
      const f32 hi = 0.5f * screen_height_ + reach_y - kButtonMargin - kButtonRadius - half;
      centre_y = lo <= hi ? std::clamp(centre_y, lo, hi) : 0.5f * screen_height_;
    }
    const f32 from = sub ? side_panel_x() : side_home_x();
    const f32 to = sub ? side_panel_x() - step - kButtonGap : side_panel_x();
    return {glm::mix(from, to, ease_out(shown)), centre_y + offset};
  }

  void toggle_side_panel() {
    side_open_ = !side_open_;
    if (side_open_) {
      side_progress_ = 0.0f;
    } else {
      side_sub_open_ = false;
    }
  }

  void close_side_panel() {
    side_open_ = false;
    side_sub_open_ = false;
  }

  void update_side_panel(f32 dt) {
    const f32 total = kMenuDropTime + kMenuDropStagger * 6.0f;
    side_progress_ = side_open_ ? std::min(side_progress_ + dt / total, 1.0f)
                                : std::max(side_progress_ - dt / total, 0.0f);
    side_sub_progress_ = side_sub_open_
                             ? std::min(side_sub_progress_ + dt / total, 1.0f)
                             : std::max(side_sub_progress_ - dt / total, 0.0f);
  }

  bool side_panel_visible() const { return side_progress_ > 0.0f; }
  bool side_sub_visible() const { return side_sub_progress_ > 0.0f; }

  std::vector<UiItem> side_sub_items() {
    switch (sub_row_) {
    case SubRow::Smoothness: return smoothness_items();
    case SubRow::Section: return section_items();
    default: return variation_items();
    }
  }

  // The items of hover layer `layer`.
  std::vector<UiItem> layer_items(int layer) {
    switch (layer) {
    case kLayerTop: return top_items();
    case kLayerRow: return row_items(shown_menu_);
    case kLayerSide: return shape_option_items();
    case kLayerSideSub: return side_sub_items();
    default: return sub_items();
    }
  }

  void update_menu(f32 dt) {
    update_side_panel(dt);
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
        // (Nearly: the last item of a row ends its drop exactly as the
        // row does, and rounding can leave it a hair short of 1.)
        if (shown >= 0.99f && glm::length(reticle - c) <= reach) {
          return layer * kLayerStride + i;
        }
      }
      return -1;
    };
    if (mode_ != EditMode::Default &&
        glm::length(reticle - exit_button_center()) <= reach) {
      return kLayerExit * kLayerStride;
    }
    if (side_sub_visible()) {
      if (int id = test(kLayerSideSub, static_cast<int>(side_sub_items().size()));
          id >= 0) {
        return id;
      }
    }
    if (side_panel_visible()) {
      if (int id = test(kLayerSide, static_cast<int>(shape_option_items().size()));
          id >= 0) {
        return id;
      }
    }
    if (shades_visible()) {
      if (int id = test(kLayerShades, static_cast<int>(sub_items().size()));
          id >= 0) {
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
    std::vector<UiItem> items = layer_items(layer);
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
                          glm::vec2(size), glm::vec4(item.colour, alpha * item.opacity));
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
    if (shown_menu_ != Menu::None) {
      draw_layer(kLayerRow, row_items(shown_menu_));
    }
    if (shades_visible()) {
      draw_layer(kLayerShades, sub_items());
    }
    if (side_panel_visible()) {
      draw_layer(kLayerSide, shape_option_items());
    }
    if (side_sub_visible()) {
      draw_layer(kLayerSideSub, side_sub_items());
    }
  }

  // --- Coat --------------------------------------------------------------------

  // One of a coat's materials: its slot and the look that differs between
  // them (colour, finish, fleck style). See ObjectCoat::palette.
  struct SlotLook {
    i32 slot = kPlainSlot;
    glm::vec3 colour = kPlainColour;
    f32 roughness = kCoatFinishes[0].roughness;
    f32 metallic = 0.0f;
    f32 waxy = 0.0f;
    i32 fleck_style = kDefaultFleckStyle;
  };

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
    // More materials, each on its own slot: a sculpture's stamps can each
    // have their own colour, finish and fleck (the fields above are the
    // first). The rest of the coat (glow, looseness, comb, ..., effects)
    // is shared by all of them, and the coat tools change every one.
    std::vector<SlotLook> palette;

    // Every material slot of the coat, the main one first.
    std::vector<i32> slots() const {
      std::vector<i32> all = {slot};
      for (const SlotLook &look : palette) {
        all.push_back(look.slot);
      }
      return all;
    }
  };

  // Calls fn(colour, roughness, metallic, waxy, fleck_style) for each of
  // the coat's materials, the main one first.
  template <typename Fn> static void for_each_look(ObjectCoat &coat, Fn fn) {
    fn(coat.colour, coat.roughness, coat.metallic, coat.waxy, coat.fleck_style);
    for (SlotLook &look : coat.palette) {
      fn(look.colour, look.roughness, look.metallic, look.waxy, look.fleck_style);
    }
  }

  static void apply_coat_material(const ObjectCoat &coat) {
    auto apply = [&](i32 slot, glm::vec3 colour, f32 roughness, f32 metallic,
                     f32 waxy, i32 fleck_style) {
      renderer_sculpt_set_material(slot, colour, roughness, metallic,
                                   waxy > 0.5f);
      renderer_sculpt_set_material_glow(slot, coat.glow);
      renderer_sculpt_set_material_flecks(slot, coat.looseness, fleck_style,
                                          coat.comb, coat.ruffle,
                                          coat.impasto, coat.neatness);
      renderer_sculpt_set_material_effects(slot, coat.boil, coat.flow,
                                           coat.wave, coat.evaporate,
                                           coat.throb);
    };
    apply(coat.slot, coat.colour, coat.roughness, coat.metallic, coat.waxy,
          coat.fleck_style);
    for (const SlotLook &look : coat.palette) {
      apply(look.slot, look.colour, look.roughness, look.metallic, look.waxy,
            look.fleck_style);
    }
  }

  // --- Search: the sculpture library -----------------------------------------
  //
  // Search (default mode's top bar) opens a row: Save, then every .sculpt
  // file found on the computer (see find_sculptures()), newest first, as
  // thumbnails. Save stores the selection (or, with nothing selected,
  // everything) as assets/library/sculpture_NNN.sculpt plus a .png thumbnail
  // drawn from the current view. Picking one imports
  // it onto the reticle; R2 stamps a copy into the scene each press, D-pad
  // up/down resizes it, B puts it away.
  //
  // A .sculpt file is an ordinary .stamps file under another name (so the
  // Qt editor and --controller can open it too) with the look in comment
  // lines:
  //   #look OBJECT SLOT r g b roughness metallic waxy fleck_style
  //       one per material the object uses, its main one first; SLOT is the
  //       slot its stamps carry in the file, remapped to fresh ones on load
  //   #style OBJECT glow looseness comb ruffle impasto neatness boil flow
  //       wave evaporate throb
  //   #coat OBJECT r g b roughness metallic waxy
  //       the older one-look form (scenes/make_face.py writes it)

  static constexpr const char *kLibraryDir = "assets/library";
  static constexpr const char *kSculptExtension = ".sculpt";
  static constexpr int kLibraryShown = 15; // with Save, two full rows
  static constexpr int kThumbnailSize = 96;

  struct LibraryEntry {
    std::string stem; // file name without extension
    std::string stamps_path;
    std::string thumbnail; // absolute path of the .png beside it, or empty
  };

  // How a file says one object looks (see the format above).
  struct FileCoat {
    std::vector<SlotLook> looks; // .slot = the slot in the file (-1: unknown)
    bool has_style = false;
    f32 glow = 0.0f, looseness = 0.0f, comb = 0.0f, ruffle = 0.5f;
    f32 impasto = 0.0f, neatness = 0.0f;
    f32 boil = 0.0f, flow = 0.0f, wave = 0.0f, evaporate = 0.0f, throb = 0.0f;
  };

  // The look lines of a stamps file, by file object id.
  // A file's weld lines ("#weld file_id key"), given fresh keys here.
  void adopt_file_welds(const std::string &path,
                        const std::unordered_map<i32, i32> &engine_id_for) {
    std::ifstream in(path);
    std::string line;
    std::unordered_map<i32, i32> fresh; // file key -> key here
    while (std::getline(in, line)) {
      if (line.rfind("#weld ", 0) != 0) {
        continue;
      }
      std::istringstream fields(line.substr(6));
      i32 file_id = -1, key = 0;
      if (!(fields >> file_id >> key)) {
        continue;
      }
      auto object = engine_id_for.find(file_id);
      if (object == engine_id_for.end()) {
        continue;
      }
      auto [it, added] = fresh.try_emplace(key, next_weld_key_);
      if (added) {
        ++next_weld_key_;
      }
      welds_[object->second] = it->second;
    }
  }

  static std::unordered_map<i32, FileCoat> read_file_coats(const std::string &path) {
    std::unordered_map<i32, FileCoat> coats;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
      std::istringstream fields;
      i32 object = -1;
      if (line.rfind("#look ", 0) == 0) {
        fields.str(line.substr(6));
        SlotLook look;
        if (fields >> object >> look.slot >> look.colour.r >> look.colour.g >>
            look.colour.b >> look.roughness >> look.metallic >> look.waxy >>
            look.fleck_style) {
          coats[object].looks.push_back(look);
        }
      } else if (line.rfind("#coat ", 0) == 0) {
        fields.str(line.substr(6));
        SlotLook look;
        look.slot = -1;
        if (fields >> object >> look.colour.r >> look.colour.g >> look.colour.b >>
            look.roughness >> look.metallic >> look.waxy) {
          coats[object].looks.insert(coats[object].looks.begin(), look);
        }
      } else if (line.rfind("#style ", 0) == 0) {
        fields.str(line.substr(7));
        FileCoat style;
        if (fields >> object >> style.glow >> style.looseness >> style.comb >>
            style.ruffle >> style.impasto >> style.neatness >> style.boil >>
            style.flow >> style.wave >> style.evaporate >> style.throb) {
          FileCoat &coat = coats[object];
          style.looks = std::move(coat.looks);
          style.has_style = true;
          coat = std::move(style);
        }
      }
    }
    return coats;
  }

  // Gives a just-loaded object the look its file describes, on fresh
  // material slots of its own (its stamps' file slots remapped onto them).
  void adopt_file_coat(i32 object, const FileCoat &file) {
    if (file.looks.empty()) {
      renderer_sculpt_set_object_material(&object, 1, kPlainSlot);
      return;
    }
    ObjectCoat coat;
    if (file.has_style) {
      coat.glow = file.glow;
      coat.looseness = file.looseness;
      coat.comb = file.comb;
      coat.ruffle = file.ruffle;
      coat.impasto = file.impasto;
      coat.neatness = file.neatness;
      coat.boil = file.boil;
      coat.flow = file.flow;
      coat.wave = file.wave;
      coat.evaporate = file.evaporate;
      coat.throb = file.throb;
    }
    std::vector<i32> old_slots, new_slots;
    for (size_t i = 0; i < file.looks.size(); ++i) {
      const i32 slot = allocate_coat_slot();
      if (slot < 0) {
        break; // out of slots: what's left stays as it is
      }
      SlotLook look = file.looks[i];
      old_slots.push_back(look.slot);
      new_slots.push_back(slot);
      look.slot = slot;
      if (i == 0) {
        coat.slot = slot;
        coat.colour = look.colour;
        coat.roughness = look.roughness;
        coat.metallic = look.metallic;
        coat.waxy = look.waxy;
        coat.fleck_style = look.fleck_style;
      } else {
        coat.palette.push_back(look);
      }
    }
    if (new_slots.empty()) {
      renderer_sculpt_set_object_material(&object, 1, kPlainSlot);
      return;
    }
    coats_[object] = coat;
    apply_coat_material(coat);
    if (old_slots[0] < 0) { // one look, slot not recorded: the whole object
      renderer_sculpt_set_object_material(&object, 1, new_slots[0]);
    } else {
      renderer_sculpt_remap_object_materials(&object, 1, old_slots.data(),
                                             new_slots.data(),
                                             static_cast<i32>(old_slots.size()));
    }
  }

  // Every .sculpt file under the library and the home folder, newest first.
  // Hidden folders (".cache", ".local", ...) and node_modules are skipped;
  // `cancel` stops it early (the app is closing).
  static std::vector<LibraryEntry> find_sculptures(const std::atomic<bool> &cancel) {
    namespace fs = std::filesystem;
    std::vector<fs::path> roots = {fs::path(kLibraryDir)};
    const char *home = std::getenv("HOME");
    if (home == nullptr || *home == '\0') {
      home = std::getenv("USERPROFILE"); // Windows
    }
    if (home != nullptr && *home != '\0') {
      roots.emplace_back(home);
    }
    std::set<fs::path> seen; // the library is usually under home too
    std::vector<std::pair<fs::file_time_type, LibraryEntry>> found;
    for (const fs::path &root : roots) {
      std::error_code error;
      fs::recursive_directory_iterator it(
          root, fs::directory_options::skip_permission_denied, error);
      for (; !error && it != fs::recursive_directory_iterator();
           it.increment(error)) {
        if (cancel) {
          return {};
        }
        const fs::path &path = it->path();
        const std::string name = path.filename().string();
        std::error_code type_error;
        if (it->is_directory(type_error)) {
          if ((!name.empty() && name[0] == '.') || name == "node_modules") {
            it.disable_recursion_pending();
          }
          continue;
        }
        if (path.extension() != kSculptExtension ||
            !it->is_regular_file(type_error)) {
          continue;
        }
        std::error_code path_error;
        const fs::path absolute = fs::weakly_canonical(path, path_error);
        if (path_error || !seen.insert(absolute).second) {
          continue;
        }
        LibraryEntry entry;
        entry.stem = absolute.stem().string();
        entry.stamps_path = absolute.string();
        fs::path png = absolute;
        png.replace_extension(".png");
        if (fs::exists(png, path_error)) {
          entry.thumbnail = png.string(); // absolute: drawn from where it is
        }
        found.emplace_back(it->last_write_time(path_error), std::move(entry));
      }
    }
    std::sort(found.begin(), found.end(),
              [](const auto &a, const auto &b) { return a.first > b.first; });
    std::vector<LibraryEntry> sculptures;
    for (auto &[time, entry] : found) {
      sculptures.push_back(std::move(entry));
    }
    return sculptures;
  }

  // A search of the computer for sculptures, on its own thread so a slow
  // disk never stalls a frame. Closing the app cancels and waits for it.
  struct LibraryScan {
    std::thread thread;
    std::atomic<bool> cancel{false};
    std::atomic<bool> done{false};
    std::vector<LibraryEntry> result; // written by the thread before `done`
    ~LibraryScan() {
      cancel = true;
      if (thread.joinable()) {
        thread.join();
      }
    }
  };

  // Starts a new search, unless one is already under way. The current list
  // stays shown until it finishes.
  void refresh_library() {
    if (library_scan_ != nullptr) {
      return;
    }
    library_scan_ = std::make_unique<LibraryScan>();
    LibraryScan *scan = library_scan_.get();
    scan->thread = std::thread([scan] {
      scan->result = find_sculptures(scan->cancel);
      scan->done = true;
    });
  }

  // Each frame: takes a finished search's results.
  void poll_library_scan() {
    if (library_scan_ == nullptr || !library_scan_->done) {
      return;
    }
    library_ = std::move(library_scan_->result);
    library_scan_.reset(); // joins a thread that has already finished
    KINFO("Search: found {} sculpture(s).", library_.size());
  }

  // "sculpture_007" -> "Sculpture 7"; other names as they are.
  static std::string library_label(const std::string &stem) {
    if (stem.rfind("sculpture_", 0) == 0) {
      return "Sculpture " + std::to_string(std::atoi(stem.c_str() + 10));
    }
    return stem;
  }

  std::vector<i32> all_object_ids() const {
    const i32 count = renderer_sculpt_object_ids(nullptr, 0);
    std::vector<i32> ids(static_cast<size_t>(std::max(count, 0)));
    renderer_sculpt_object_ids(ids.data(), count);
    return ids;
  }

  // Search -> Save.
  void save_to_library() {
    const std::vector<i32> objects =
        !selection_.empty() ? selection_ : all_object_ids();
    if (objects.empty()) {
      KINFO("Save: there's nothing to save.");
      return;
    }
    std::error_code error;
    std::filesystem::create_directories(kLibraryDir, error);
    std::string stem;
    for (int n = 1; n < 100000; ++n) {
      char name[32];
      std::snprintf(name, sizeof(name), "sculpture_%03d", n);
      if (!std::filesystem::exists(
              std::string(kLibraryDir) + "/" + name + kSculptExtension, error)) {
        stem = name;
        break;
      }
    }
    const std::string base = std::string(kLibraryDir) + "/" + stem;
    std::ofstream out(base + kSculptExtension);
    if (!out) {
      KERROR("Save: could not write '{}{}'.", base, kSculptExtension);
      return;
    }
    out << "# modelling_software_v2 sculpt stamps (v4/objects)\n"
           "# x y z shape_type p0 p1 p2 p3 smoothness op material_slot "
           "looseness qx qy qz qw object_id\n";
    size_t stamp_count = 0;
    for (size_t i = 0; i < objects.size(); ++i) {
      const i32 file_id = static_cast<i32>(i) + 1;
      if (auto it = coats_.find(objects[i]); it != coats_.end()) {
        ObjectCoat &coat = it->second;
        for_each_slot_look(coat, [&](const SlotLook &look) {
          out << "#look " << file_id << ' ' << look.slot << ' ' << look.colour.r
              << ' ' << look.colour.g << ' ' << look.colour.b << ' '
              << look.roughness << ' ' << look.metallic << ' ' << look.waxy
              << ' ' << look.fleck_style << '\n';
        });
        if (auto weld = welds_.find(objects[i]); weld != welds_.end()) {
          out << "#weld " << file_id << ' ' << weld->second << '\n';
        }
        out << "#style " << file_id << ' ' << coat.glow << ' ' << coat.looseness
            << ' ' << coat.comb << ' ' << coat.ruffle << ' ' << coat.impasto
            << ' ' << coat.neatness << ' ' << coat.boil << ' ' << coat.flow
            << ' ' << coat.wave << ' ' << coat.evaporate << ' ' << coat.throb
            << '\n';
      }
      std::vector<SculptStampInfo> stamps(static_cast<size_t>(
          std::max(renderer_sculpt_object_stamps(objects[i], nullptr, 0), 0)));
      renderer_sculpt_object_stamps(objects[i], stamps.data(),
                                    static_cast<i32>(stamps.size()));
      for (const SculptStampInfo &s : stamps) {
        out << s.center.x << ' ' << s.center.y << ' ' << s.center.z << ' '
            << static_cast<int>(s.shape_type) << ' ' << s.params.x << ' '
            << s.params.y << ' ' << s.params.z << ' ' << s.params.w << ' '
            << s.smoothness << ' ' << static_cast<int>(s.op) << ' '
            << s.material_slot << ' ' << s.looseness << ' ' << s.rotation.x
            << ' ' << s.rotation.y << ' ' << s.rotation.z << ' '
            << s.rotation.w << ' ' << file_id << '\n';
      }
      stamp_count += stamps.size();
    }
    out.close();
    render_thumbnail(objects, base + ".png");
    LibraryEntry saved;
    saved.stem = stem;
    std::error_code path_error;
    saved.stamps_path =
        std::filesystem::weakly_canonical(base + kSculptExtension, path_error).string();
    const std::filesystem::path png =
        std::filesystem::weakly_canonical(base + ".png", path_error);
    if (std::filesystem::exists(png, path_error)) {
      saved.thumbnail = png.string();
    }
    library_.insert(library_.begin(), saved); // shown now; the search confirms it
    refresh_library();
    KINFO("Saved {} object(s), {} stamps, as '{}'.", objects.size(), stamp_count,
          stem);
  }

  // The main look then the palette, each as a SlotLook.
  template <typename Fn> static void for_each_slot_look(const ObjectCoat &coat, Fn fn) {
    SlotLook main;
    main.slot = coat.slot;
    main.colour = coat.colour;
    main.roughness = coat.roughness;
    main.metallic = coat.metallic;
    main.waxy = coat.waxy;
    main.fleck_style = coat.fleck_style;
    fn(main);
    for (const SlotLook &look : coat.palette) {
      fn(look);
    }
  }

  // A small picture of `objects` as seen from the current view direction:
  // rays cast on the CPU against their stamps (renderer_sculpt_raycast_
  // objects), lit simply, in each object's main colour.
  void render_thumbnail(const std::vector<i32> &objects, const std::string &path) {
    bool any = false;
    glm::vec3 lo_all(0.0f), hi_all(0.0f);
    for (i32 object : objects) {
      glm::vec3 lo, hi;
      if (renderer_sculpt_object_bounds(object, &lo, &hi)) {
        lo_all = any ? glm::min(lo_all, lo) : lo;
        hi_all = any ? glm::max(hi_all, hi) : hi;
        any = true;
      }
    }
    if (!any) {
      return;
    }
    const glm::vec3 centre = 0.5f * (lo_all + hi_all);
    const f32 radius = std::max(0.5f * glm::length(hi_all - lo_all), 0.01f);
    const glm::vec3 forward = camera_.forward();
    const glm::vec3 right = camera_.right();
    const glm::vec3 up = screen_up();
    const glm::vec3 light = glm::normalize(-forward + 0.8f * up + 0.4f * right);
    const int n = kThumbnailSize;
    // Bounds are generous; frame a bit inside them.
    const f32 half = 0.8f * radius;
    std::vector<f32> depth(static_cast<size_t>(n * n), -1.0f);
    std::vector<i32> hit(static_cast<size_t>(n * n), -1);
    for (int y = 0; y < n; ++y) {
      for (int x = 0; x < n; ++x) {
        const f32 u = (2.0f * (static_cast<f32>(x) + 0.5f) / n - 1.0f) * half;
        const f32 v = (1.0f - 2.0f * (static_cast<f32>(y) + 0.5f) / n) * half;
        const glm::vec3 origin = centre - forward * (2.0f * radius) + right * u + up * v;
        f32 t = 0.0f;
        const i32 object = renderer_sculpt_raycast_objects(
            origin, forward, 4.0f * radius, objects.data(),
            static_cast<i32>(objects.size()), &t);
        if (object >= 0) {
          depth[static_cast<size_t>(y * n + x)] = t;
          hit[static_cast<size_t>(y * n + x)] = object;
        }
      }
    }
    QImage image(n, n, QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    const f32 pixel = 2.0f * half / static_cast<f32>(n);
    auto depth_at = [&](int x, int y, f32 fallback) {
      if (x < 0 || y < 0 || x >= n || y >= n) {
        return fallback;
      }
      const f32 d = depth[static_cast<size_t>(y * n + x)];
      return d < 0.0f ? fallback : d;
    };
    for (int y = 0; y < n; ++y) {
      for (int x = 0; x < n; ++x) {
        const size_t i = static_cast<size_t>(y * n + x);
        if (hit[i] < 0) {
          continue;
        }
        const f32 d = depth[i];
        // Normal from the depth slope across neighbouring pixels.
        const f32 dx = depth_at(x + 1, y, d) - depth_at(x - 1, y, d);
        const f32 dy = depth_at(x, y - 1, d) - depth_at(x, y + 1, d);
        const glm::vec3 normal = glm::normalize(-forward * (2.0f * pixel) -
                                                right * dx - up * dy);
        glm::vec3 colour = kPlainColour;
        if (auto it = coats_.find(hit[i]); it != coats_.end()) {
          colour = it->second.colour;
        }
        const f32 lit = 0.3f + 0.7f * std::max(glm::dot(normal, light), 0.0f);
        // Linear to roughly sRGB for the picture.
        const glm::vec3 shown = glm::pow(glm::clamp(colour * lit * 1.3f, 0.0f, 1.0f),
                                         glm::vec3(1.0f / 2.2f));
        image.setPixel(x, y, qRgba(static_cast<int>(shown.r * 255.0f),
                                   static_cast<int>(shown.g * 255.0f),
                                   static_cast<int>(shown.b * 255.0f), 255));
      }
    }
    if (!image.save(QString::fromStdString(path))) {
      KWARN("Save: could not write the thumbnail '{}'.", path);
    }
  }

  // Picking a library entry: load it as new objects and pick them up.
  void import_from_library(const LibraryEntry &entry) {
    end_placing();
    SculptDocument document;
    if (!document.load(entry.stamps_path)) {
      KERROR("Import: could not read '{}'.", entry.stamps_path);
      return;
    }
    const std::unordered_map<i32, i32> engine_id_for =
        sculpt_shared::replay_document(document);
    adopt_file_welds(entry.stamps_path, engine_id_for);
    const std::unordered_map<i32, FileCoat> coats = read_file_coats(entry.stamps_path);
    std::vector<i32> objects;
    for (const auto &[file_id, engine_id] : engine_id_for) {
      auto it = coats.find(file_id);
      adopt_file_coat(engine_id, it != coats.end() ? it->second : FileCoat{});
      objects.push_back(engine_id);
    }
    if (objects.empty()) {
      return;
    }
    std::sort(objects.begin(), objects.end());
    if (objects.size() > 1) {
      groups_.push_back({next_group_id_++, objects});
    }
    placing_ = objects;
    placing_center_ = group_center(objects);
    // In the middle of the view, far enough away to see all of it.
    glm::vec3 lo, hi, lo_all(1e30f), hi_all(-1e30f);
    for (i32 object : objects) {
      if (renderer_sculpt_object_bounds(object, &lo, &hi)) {
        lo_all = glm::min(lo_all, lo);
        hi_all = glm::max(hi_all, hi);
      }
    }
    const f32 radius = 0.5f * glm::length(hi_all - lo_all);
    brush_ = glm::vec3(0.0f, 0.0f, std::clamp(2.5f * radius, 2.0f, kMaxGrabDistance));
    open_menu_ = Menu::None;
    KINFO("Imported '{}' ({} object(s)): R2 stamps it, B puts it away.",
          entry.stem, objects.size());
  }

  bool placing() const { return !placing_.empty(); }

  // Puts the floating copy away.
  void end_placing() {
    if (!placing()) {
      return;
    }
    const std::vector<i32> floating = placing_;
    placing_.clear();
    delete_objects(floating);
    history_dirty_ = true; // the stamped copies become one undo step
  }

  // Each frame while placing: the copy follows the reticle; R2 leaves a
  // copy where it is; D-pad up/down resizes it.
  void update_placing(f32 dt, bool r2_pressed) {
    if (!placing()) {
      return;
    }
    const i32 count = static_cast<i32>(placing_.size());
    const glm::quat identity(1.0f, 0.0f, 0.0f, 0.0f);
    const glm::vec3 target = brush_world();
    const glm::vec3 delta = target - placing_center_;
    if (glm::length(delta) > 1e-4f &&
        renderer_sculpt_transform_objects(placing_.data(), count, placing_center_,
                                          identity, delta, 1.0f)) {
      placing_center_ = target;
    }
    const f32 resize = (held(GamepadButton::DpadUp) ? 1.0f : 0.0f) -
                       (held(GamepadButton::DpadDown) ? 1.0f : 0.0f);
    if (resize != 0.0f) {
      renderer_sculpt_transform_objects(
          placing_.data(), count, placing_center_, identity, glm::vec3(0.0f),
          std::exp2(resize * dt / kResizeDoublingTime));
    }
    if (r2_pressed && hovered_button_ < 0 && hovered_top_ < 0) {
      const std::vector<i32> copies = clone_objects(placing_);
      KINFO("Stamped {} object(s) into the scene.", copies.size());
    }
  }

  std::vector<UiItem> search_items() {
    std::vector<UiItem> items;
    UiItem save;
    save.icon = "ui/toolbar/save.png";
    save.label = selection_.empty() ? "Save Everything" : "Save Selection";
    save.press = [this] { save_to_library(); };
    items.push_back(save);
    if (library_.empty() && library_scan_ != nullptr) {
      UiItem searching;
      searching.icon = "ui/toolbar/search.png";
      searching.label = "Searching...";
      searching.enabled = false;
      items.push_back(searching);
    }
    const int shown = std::min(static_cast<int>(library_.size()), kLibraryShown);
    for (int i = 0; i < shown; ++i) {
      const LibraryEntry entry = library_[static_cast<size_t>(i)];
      UiItem item;
      item.icon = entry.thumbnail.empty() ? "ui/toolbar/dummy.png" : entry.thumbnail;
      item.label = library_label(entry.stem);
      item.press = [this, entry] { import_from_library(entry); };
      items.push_back(item);
    }
    return items;
  }

  // Every object starts plain: one shared slot, set up once.
  void make_everything_plain() {
    ObjectCoat plain;
    apply_coat_material(plain);
    const i32 count = renderer_sculpt_object_ids(nullptr, 0);
    std::vector<i32> ids(static_cast<size_t>(count));
    renderer_sculpt_object_ids(ids.data(), count);
    std::erase_if(ids, [&](i32 id) { return coats_.count(id) != 0; });
    if (!ids.empty()) {
      renderer_sculpt_set_object_material(ids.data(),
                                          static_cast<i32>(ids.size()), kPlainSlot);
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

  // How many objects' coats use `slot`.
  int coat_slot_users(i32 slot) const {
    int users = 0;
    for (const auto &[object, coat] : coats_) {
      const std::vector<i32> slots = coat.slots();
      users += std::find(slots.begin(), slots.end(), slot) != slots.end() ? 1 : 0;
    }
    return users;
  }

  // The object's coat, on a slot of its own (moving it onto one -- a re-run
  // of its region -- the first time it is coated, or when it still shares
  // its slot with the object it was cloned from). Null if out of slots.
  ObjectCoat *own_coat(i32 object) {
    auto it = coats_.find(object);
    ObjectCoat coat = it != coats_.end() ? it->second : ObjectCoat{};
    const std::vector<i32> old_slots = coat.slots();
    const bool owned =
        coat.slot != kPlainSlot &&
        std::all_of(old_slots.begin(), old_slots.end(),
                    [&](i32 slot) { return coat_slot_users(slot) <= 1; });
    if (owned) {
      return &coats_[object];
    }
    std::vector<i32> new_slots;
    for (size_t i = 0; i < old_slots.size(); ++i) {
      const i32 slot = allocate_coat_slot();
      if (slot < 0) {
        for (i32 taken : new_slots) {
          free_coat_slots_.push_back(taken);
        }
        if (!coat_slots_warned_) {
          KWARN("Coat: out of material slots; more objects can't be coated.");
          coat_slots_warned_ = true;
        }
        return nullptr;
      }
      new_slots.push_back(slot);
    }
    coat.slot = new_slots[0];
    for (size_t i = 0; i < coat.palette.size(); ++i) {
      coat.palette[i].slot = new_slots[i + 1];
    }
    apply_coat_material(coat);
    if (coat.palette.empty()) {
      renderer_sculpt_set_object_material(&object, 1, coat.slot);
    } else { // each material onto its own new slot
      renderer_sculpt_remap_object_materials(
          &object, 1, old_slots.data(), new_slots.data(),
          static_cast<i32>(old_slots.size()));
    }
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
      const std::vector<i32> slots = it->second.slots();
      release_glow_light(it->second);
      coats_.erase(it);
      for (i32 slot : slots) {
        if (slot != kPlainSlot && coat_slot_users(slot) == 0) {
          free_coat_slots_.push_back(slot);
        }
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

  // Looseness stamped with the sculpt brush sits on the shapes, and the
  // renderer draws the larger of that and the object's own, so style mode
  // could only ever raise it. The first time style mode's Looseness (or
  // Revert) touches such an object, its shapes' looseness becomes the
  // object's (the loosest of them) and the shapes go back to 0 -- from
  // then on style mode has it all. Once per object per R2 hold.
  void take_stamped_looseness(const std::vector<i32> &objects) {
    std::vector<i32> flatten;
    for (i32 id : objects) {
      if (!style_checked_.insert(id).second) {
        continue;
      }
      std::vector<SculptStampInfo> stamps(static_cast<size_t>(
          std::max(renderer_sculpt_object_stamps(id, nullptr, 0), 0)));
      renderer_sculpt_object_stamps(id, stamps.data(), static_cast<i32>(stamps.size()));
      f32 loosest = 0.0f;
      for (const SculptStampInfo &stamp : stamps) {
        if (stamp.op != SculptBrushOp::ObjectPaint) { // paint: opacity
          loosest = std::max(loosest, stamp.looseness);
        }
      }
      if (loosest <= 0.0f) {
        continue;
      }
      if (ObjectCoat *coat = own_coat(id)) {
        coat->looseness = std::max(coat->looseness, loosest);
        apply_coat_material(*coat);
      }
      flatten.push_back(id);
    }
    if (!flatten.empty()) {
      renderer_sculpt_set_object_looseness(flatten.data(),
                                           static_cast<i32>(flatten.size()), 0.0f);
      history_dirty_ = true;
      KINFO("Style: {} object(s) took their stamped looseness.", flatten.size());
    }
  }

  // R2 held in style mode: one frame of the current fleck tool on the
  // object (or group) under the reticle.
  void update_style(f32 dt) {
    if (!r2_.down) {
      style_checked_.clear();
    }
    if (mode_ != EditMode::Style || move_tool_ || !r2_.down || hovered_button_ >= 0 ||
        hovered_top_ >= 0) {
      return;
    }
    const i32 object = pick_object_under_reticle();
    if (object < 0) {
      return;
    }
    const f32 step = dt / kCoatFullTime;
    const f32 target = coat_subtract_ ? 0.0f : 1.0f;
    if (style_tool_ == StyleTool::Looseness || style_tool_ == StyleTool::Revert) {
      take_stamped_looseness(group_for(object));
    }
    for (i32 id : group_for(object)) {
      ObjectCoat *coat = own_coat(id);
      if (!coat) {
        continue;
      }
      switch (style_tool_) {
      case StyleTool::FleckStyle:
        for_each_look(*coat, [&](glm::vec3 &, f32 &, f32 &, f32 &, i32 &style) {
          style = coat_subtract_ ? kDefaultFleckStyle : fleck_style_index_;
        });
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
        for_each_look(*coat, [&](glm::vec3 &, f32 &, f32 &, f32 &, i32 &style) {
          style = plain.fleck_style;
        });
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
    if (mode_ != EditMode::Effects || move_tool_ || !r2_.down || hovered_button_ >= 0 ||
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
    if (mode_ != EditMode::Coat || move_tool_ || !r2_.down || hovered_button_ >= 0 ||
        hovered_top_ >= 0) {
      coating_ = false;
      return;
    }
    const i32 object = pick_object_under_reticle();
    const bool was_coating = coating_;
    coating_ = object >= 0;
    if (object < 0) {
      return;
    }
    const f32 step = dt / kCoatFullTime;
    const CoatFinish &finish = kCoatFinishes[finish_index_];
    const bool revert = coat_tool_ == CoatTool::Revert;
    const bool subtract = coat_subtract_ || revert;
    const std::vector<i32> group = group_for(object);
    // Colour Variation: each object coated (a new press, or the reticle onto
    // another object or group) takes the next steps of the set, one per
    // member.
    const bool varied = tool_uses_colour() && !subtract && variation_active();
    if (varied && (!was_coating || group.front() != coat_variation_key_)) {
      coat_variation_key_ = group.front();
      coat_variation_base_ = coat_variation_step_;
      coat_variation_step_ += static_cast<int>(group.size());
    }
    for (size_t member = 0; member < group.size(); ++member) {
      const i32 id = group[member];
      const glm::vec3 picked =
          varied ? variation_step_colour(coat_variation_base_ + static_cast<int>(member))
                 : coat_colour();
      ObjectCoat *coat = own_coat(id);
      if (!coat) {
        continue;
      }
      const bool all = revert;
      // Every material of the coat (a sculpture can have several).
      for_each_look(*coat, [&](glm::vec3 &colour, f32 &roughness,
                               f32 &metallic, f32 &waxy, i32 &) {
        if (coat_tool_ == CoatTool::Tint || coat_tool_ == CoatTool::Hue || all) {
          glm::vec3 target = kPlainColour;
          if (!subtract && coat_tool_ == CoatTool::Tint) {
            target = picked;
          } else if (!subtract) { // Hue: the picked hue/saturation, own brightness
            const glm::vec3 hsv = rgb_to_hsv(picked);
            target = hsv_to_rgb({hsv.x, hsv.y, rgb_to_hsv(colour).z});
          }
          colour = move_toward(colour, target, step);
        }
        if (coat_tool_ == CoatTool::Finish || all) {
          const CoatFinish &to = subtract ? kCoatFinishes[0] : finish;
          roughness = move_toward(roughness, to.roughness, step);
          metallic = move_toward(metallic, to.metallic, step);
          waxy = move_toward(waxy, to.waxy ? 1.0f : 0.0f, step);
        }
      });
      if (coat_tool_ == CoatTool::Glow || all) {
        coat->glow = move_toward(coat->glow, subtract ? 0.0f : 1.0f, step);
      }
      history_dirty_ = true;
      apply_coat_material(*coat);
    }
  }

  // --- Sculpt mode -------------------------------------------------------------

  // Sculpt mode: a new, empty sculpture to build -- or, scoped into an
  // object or a group, that object or group, to carry on editing -- in
  // focus; the tool is Smear and the Shapes row is open.
  //
  // A sculpture is a group of strokes: its first addition goes into its
  // first object (sculpture_), and every later one -- each Stamp press,
  // each Smear stroke, adding -- is an object of its own, joined to the
  // sculpture's group. So scoped into the sculpture afterwards, each stroke
  // can be picked, moved, turned or deleted on its own. What changes the
  // material that's there -- carving, Spray Paint, Crop -- works on every
  // stroke of the sculpture. The strokes share the sculpture's materials
  // (sync_stroke_coats()) until one is coated on its own.
  void enter_sculpt() {
    selection_.clear();
    if (scoped_object_ >= 0) {
      sculpture_ = scoped_object_;
      sculpture_members_ = {sculpture_};
      KINFO("Entered sculpt mode: editing object {}.", sculpture_);
    } else if (const Group *group = scoped_group(); group && !group->members.empty()) {
      sculpture_members_ = group->members;
      sculpture_ = sculpture_members_.front();
      KINFO("Entered sculpt mode: editing group {} ({} objects).", group->id,
            sculpture_members_.size());
    } else {
      sculpture_ = renderer_sculpt_begin_object();
      sculpture_members_ = {sculpture_};
      KINFO("Entered sculpt mode: new sculpture {}.", sculpture_);
    }
    stroke_object_ = -1;
    mode_ = EditMode::Sculpt;
    move_tool_ = false;
    sculpt_tool_ = SculptTool::Smear;
    stroke_active_ = false;
    open_menu_ = Menu::None;
    toggle_menu(Menu::Shapes);
  }

  void leave_sculpt() {
    renderer_sculpt_preview_brush(false, glm::vec3(0.0f),
                                  SculptBrushShape::Sphere, glm::vec4(0.0f),
                                  SculptBrushOp::Union, 0);
    sculpture_ = -1;
    sculpture_members_.clear();
    stroke_object_ = -1;
    stroke_active_ = false;
    precise_scale_ = false;
    drag_handle_ = -1;
  }

  // The sculpture's group (the one its first object is in), if it has one.
  Group *sculpture_group() {
    const Group *group = group_of(sculpture_);
    return group ? find_group(group->id) : nullptr;
  }

  // --- Welding (see renderer_sculpt_set_weld_keys()) --------------------------
  // Objects with the same key -- the strokes of one sculpture -- blend into
  // each other where they meet. Sent to the engine whenever it changes.
  void sync_welds() {
    if (welds_ == sent_welds_) {
      return;
    }
    std::vector<i32> ids, keys;
    for (const auto &[object, key] : welds_) {
      ids.push_back(object);
      keys.push_back(key);
    }
    renderer_sculpt_set_weld_keys(ids.data(), keys.data(),
                                  static_cast<i32>(ids.size()));
    sent_welds_ = welds_;
  }

  // Each weld among `originals` -> `copies` (same order) as a new weld of
  // its own, so copies blend with each other but not with what they copy.
  void copy_welds(const std::vector<std::pair<i32, i32>> &copies) {
    std::unordered_map<i32, i32> fresh; // old key -> new key
    for (const auto &[original, copy] : copies) {
      auto it = welds_.find(original);
      if (it == welds_.end()) {
        continue;
      }
      auto [key, added] = fresh.try_emplace(it->second, next_weld_key_);
      if (added) {
        ++next_weld_key_;
      }
      welds_[copy] = key->second;
    }
  }

  // Where an addition goes: an empty sculpture's own first object, or else
  // a new object, joined to the sculpture's group (made now, if this is its
  // second stroke).
  i32 begin_addition() {
    glm::vec3 lo, hi;
    if (sculpture_ >= 0 && !renderer_sculpt_object_bounds(sculpture_, &lo, &hi)) {
      return sculpture_;
    }
    const i32 stroke = renderer_sculpt_begin_object();
    if (stroke < 0) {
      return sculpture_;
    }
    sculpture_members_.push_back(stroke);
    // The strokes of a sculpture weld: each blends into the others with
    // its own Smoothness, as shapes within one stroke do.
    auto key = welds_.find(sculpture_);
    if (key == welds_.end()) {
      key = welds_.emplace(sculpture_, next_weld_key_++).first;
    }
    welds_[stroke] = key->second;
    if (Group *group = sculpture_group()) {
      group->members.push_back(stroke);
    } else {
      groups_.push_back({next_group_id_++, {sculpture_, stroke}});
    }
    sync_stroke_coats();
    return stroke;
  }

  // The strokes take the sculpture's materials (its first object's coat):
  // each gets a copy of it, sharing its slots -- as a clone does -- unless
  // it's been coated on its own since (its main slot is its own).
  void sync_stroke_coats() {
    const auto root = coats_.find(sculpture_);
    if (root == coats_.end()) {
      return;
    }
    for (i32 member : sculpture_members_) {
      if (member == sculpture_) {
        continue;
      }
      auto it = coats_.find(member);
      if (it == coats_.end() || it->second.slot == root->second.slot) {
        const i32 light = it != coats_.end() ? it->second.light : -1;
        ObjectCoat copy = root->second;
        copy.light = light; // keeps its own glow light, if it has one
        coats_[member] = copy;
      }
    }
  }

  // The sculpture's objects that have shape (what carving and painting
  // work on).
  std::vector<i32> sculpture_shaped_members() const {
    std::vector<i32> shaped;
    for (i32 member : sculpture_members_) {
      glm::vec3 lo, hi;
      if (renderer_sculpt_object_bounds(member, &lo, &hi)) {
        shaped.push_back(member);
      }
    }
    return shaped;
  }

  // The shape's params at brush size `size` (about its radius). Each shape
  // gets proportions that read as that shape, centred on the brush point
  // (Pyramid: on its base); icons/make_shape_icons.py draws the same.
  static glm::vec4 sculpt_params(SculptBrushShape shape, f32 size) {
    switch (shape) {
    case SculptBrushShape::Sphere: return {size, 0.0f, 0.0f, 0.0f};
    case SculptBrushShape::Box: return {0.8f * size, 0.8f * size, 0.8f * size, 0.0f};
    case SculptBrushShape::Torus: return {0.75f * size, 0.3f * size, 0.0f, 0.0f};
    case SculptBrushShape::CappedCylinder: return {0.7f * size, 0.9f * size, 0.0f, 0.0f};
    case SculptBrushShape::CappedCone:
      return {0.9f * size, 0.85f * size, 0.2f * size, 0.0f};
    case SculptBrushShape::RoundBox:
      return {0.8f * size, 0.8f * size, 0.8f * size, 0.25f * size};
    case SculptBrushShape::BoxFrame:
      return {0.8f * size, 0.8f * size, 0.8f * size, 0.12f * size};
    case SculptBrushShape::Octahedron: return {1.2f * size, 0.0f, 0.0f, 0.0f};
    case SculptBrushShape::Pyramid: return {1.3f * size, 0.85f * size, 0.0f, 0.0f};
    case SculptBrushShape::HexPrism: return {0.8f * size, 0.75f * size, 0.0f, 0.0f};
    case SculptBrushShape::RoundCone:
      return {0.75f * size, 0.3f * size, 0.75f * size, 0.0f};
    case SculptBrushShape::Capsule: return {0.55f * size, 0.65f * size, 0.0f, 0.0f};
    case SculptBrushShape::Link:
      return {0.45f * size, 0.5f * size, 0.18f * size, 0.0f};
    case SculptBrushShape::Ellipsoid:
      return {1.15f * size, 0.7f * size, 0.8f * size, 0.0f};
    default: return {size, 0.0f, 0.0f, 0.0f};
    }
  }

  enum class SubRow { Smoothness, Section, Variation };

  // --- Shape options (sculpt mode) -------------------------------------------
  //
  // The side button opens the Shape row: Precise Scale, Smoothness,
  // Section and Reset. They shape what every sculpt tool places. What each
  // shape supports is below; the rest are greyed out. Smoothness rounds a
  // shape's edges and Section keeps a half, quarter or eighth of it -- both
  // ride in shape params the engine otherwise leaves unused (see the
  // modifier notes in Builtin.BrushShapes.inc.glsl).

  using Shape = SculptBrushShape;
  // Shapes with separate sizes along their own axes, so pulling one face
  // means something. (A sphere is the same size every way.)
  static bool shape_scalable(Shape s) {
    switch (s) {
    case Shape::Box: case Shape::RoundBox: case Shape::BoxFrame:
    case Shape::Ellipsoid: case Shape::CappedCylinder: case Shape::Capsule:
    case Shape::HexPrism: case Shape::CappedCone: case Shape::RoundCone:
    case Shape::Pyramid:
      return true;
    default:
      return false;
    }
  }
  // Must match brush_shape_sectionable() (Builtin.BrushShapes.inc.glsl).
  static bool shape_sectionable(Shape s) {
    const int i = static_cast<int>(s);
    return i == 0 || i == 3 || i == 4 || i == 5 || (i >= 8 && i <= 14);
  }
  // Round in X and Z (one radius): pulling either side face changes both.
  static bool shape_radial(Shape s) {
    return s == Shape::CappedCylinder || s == Shape::Capsule ||
           s == Shape::HexPrism || s == Shape::CappedCone ||
           s == Shape::RoundCone || s == Shape::Pyramid;
  }

  Shape current_shape() const { return kSculptShapes[shape_index_].shape; }

  // The current shape's params: its base proportions at the brush size,
  // stretched by Precise Scale, then rounded and sectioned.
  glm::vec4 shaped_params() const {
    const Shape s = current_shape();
    glm::vec4 p = sculpt_params(s, brush_size_);
    const glm::vec3 sc = shape_scalable(s) ? shape_scale_ : glm::vec3(1.0f);
    switch (s) {
    case Shape::Box: case Shape::Ellipsoid:
      p.x *= sc.x; p.y *= sc.y; p.z *= sc.z;
      break;
    case Shape::RoundBox: case Shape::BoxFrame:
      p.x *= sc.x; p.y *= sc.y; p.z *= sc.z;
      p.w *= std::min(sc.x, std::min(sc.y, sc.z));
      break;
    case Shape::CappedCylinder: case Shape::Capsule: case Shape::HexPrism:
      p.x *= sc.x; p.y *= sc.y;
      break;
    case Shape::CappedCone: // half height, bottom radius, top radius
      p.x *= sc.y; p.y *= sc.x; p.z *= sc.x;
      break;
    case Shape::RoundCone: // bottom radius, top radius, half height
      p.x *= sc.x; p.y *= sc.x; p.z *= sc.y;
      break;
    case Shape::Pyramid: // height, base half-width
      p.x *= sc.y; p.y *= sc.x;
      break;
    default:
      break;
    }
    if (shape_sectionable(s)) {
      p.w = static_cast<f32>(section_index_);
    }
    return p;
  }

  // The shape's extent in its own frame (before rotation): lo..hi per axis.
  static void local_bounds(Shape s, glm::vec4 p, glm::vec3 *lo, glm::vec3 *hi) {
    glm::vec3 h(p.x, p.y, p.z);
    switch (s) {
    case Shape::CappedCylinder: case Shape::HexPrism:
      h = glm::vec3(p.x, p.y, p.x);
      break;
    case Shape::Capsule:
      h = glm::vec3(p.x, p.y + p.x, p.x);
      break;
    case Shape::CappedCone: {
      const f32 r = std::max(p.y, p.z);
      h = glm::vec3(r, p.x, r);
      break;
    }
    case Shape::RoundCone: { // spheres of r1 at -hh and r2 at +hh
      const f32 r = std::max(p.x, p.y);
      *lo = glm::vec3(-r, -p.z - p.x, -r);
      *hi = glm::vec3(r, p.z + p.y, r);
      return;
    }
    case Shape::Pyramid: // base at y = 0, apex at y = h
      *lo = glm::vec3(-p.y, 0.0f, -p.y);
      *hi = glm::vec3(p.y, p.x, p.y);
      return;
    default:
      break;
    }
    *lo = -h;
    *hi = h;
  }

  // Precise Scale handles: 0..5 = axis * 2 + (1 for the + face).
  glm::vec3 handle_world(int handle) const {
    glm::vec3 lo, hi;
    local_bounds(current_shape(), shaped_params(), &lo, &hi);
    const int axis = handle / 2;
    glm::vec3 local(0.0f);
    local[axis] = (handle % 2) ? hi[axis] : lo[axis];
    return pinned_center_ + brush_rotation_ * local;
  }

  int handle_under_reticle() const {
    if (!precise_scale_ || brush_.z <= 0.01f) {
      return -1;
    }
    const glm::vec2 reticle = reticle_screen();
    int best = -1;
    f32 best_distance = kHandleReach;
    for (int h = 0; h < 6; ++h) {
      glm::vec2 at;
      if (project(handle_world(h), &at)) {
        const f32 d = glm::length(at - reticle);
        if (d < best_distance) {
          best = h;
          best_distance = d;
        }
      }
    }
    return best;
  }

  // Whether the current tool puts the brush shape into the scene (every
  // sculpt tool does: Stamp, Smear, Spray Paint, Crop, Cut Out) -- the
  // tools Precise Scale goes with.
  bool tool_uses_shape() const {
    return mode_ == EditMode::Sculpt && !move_tool_;
  }

  void begin_precise_scale() {
    if (!tool_uses_shape() || !shape_scalable(current_shape())) {
      return;
    }
    precise_scale_ = true;
    // In the middle of the view at the brush's depth -- the reticle is on
    // a button right now, so where it is says nothing.
    brush_ = glm::vec3(0.0f, 0.0f, brush_.z);
    pinned_center_ = placement_point();
    drag_handle_ = -1;
    open_menu_ = Menu::None; // out of the way of the handles
    shades_open_ = false;
    side_sub_open_ = false;
    KINFO("Precise scale: on.");
  }

  void end_precise_scale() {
    if (!precise_scale_) {
      return;
    }
    precise_scale_ = false;
    drag_handle_ = -1;
    // The shape stays where it was pinned: the brush picks it up there.
    brush_ = to_camera_space(pinned_center_);
    KINFO("Precise scale: off.");
  }

  // R2 on a handle drags that face along its own axis to wherever the line
  // of sight through the reticle passes closest; the opposite face stays
  // put. With Grid Snap on, the size goes in whole grid cells.
  void update_scale_drag(bool r2_pressed) {
    if (!r2_.down) {
      drag_handle_ = -1;
      return;
    }
    if (r2_pressed) {
      drag_handle_ = hovered_handle_;
    }
    if (drag_handle_ < 0) {
      return;
    }
    const Shape s = current_shape();
    const int axis = drag_handle_ / 2;
    const bool plus = drag_handle_ % 2;
    glm::vec3 lo, hi;
    local_bounds(s, shaped_params(), &lo, &hi);
    glm::vec3 unit(0.0f);
    unit[axis] = 1.0f;
    const glm::vec3 e = brush_rotation_ * unit;
    const f32 face = plus ? hi[axis] : lo[axis];
    const f32 opposite = plus ? lo[axis] : hi[axis];
    // Closest point on the face's axis line to the reticle's line of sight.
    const glm::vec3 eye = camera_.position();
    const glm::vec3 ray = glm::normalize(brush_world() - eye);
    const f32 b = glm::dot(e, ray);
    const f32 denom = 1.0f - b * b;
    if (denom < 1e-3f) {
      return; // looking straight down the axis: no way to tell how far
    }
    const glm::vec3 w0 = pinned_center_ + e * face - eye;
    const f32 u = (b * glm::dot(ray, w0) - glm::dot(e, w0)) / denom;
    f32 size = plus ? (face + u) - opposite : opposite - (face + u);
    size = std::max(size, kMinShapeSize);
    if (grid_snap_) {
      size = std::max(grid_size(), std::round(size / grid_size()) * grid_size());
    }
    if (std::fabs(size - (hi[axis] - lo[axis])) < 1e-5f) {
      return;
    }
    // Scale the axis until its size matches (a few rounds, since a
    // capsule's or round cone's ends don't scale with it exactly).
    const int scale_axis = shape_radial(s) && axis == 2 ? 0 : axis;
    for (int round = 0; round < 4; ++round) {
      local_bounds(s, shaped_params(), &lo, &hi);
      const f32 current = hi[axis] - lo[axis];
      if (current <= 1e-6f) {
        break;
      }
      shape_scale_[scale_axis] = std::clamp(
          shape_scale_[scale_axis] * size / current, kMinShapeScale, kMaxShapeScale);
    }
    // Keep the opposite face where it was.
    local_bounds(s, shaped_params(), &lo, &hi);
    const f32 new_opposite = plus ? lo[axis] : hi[axis];
    pinned_center_ += e * (opposite - new_opposite);
  }

  static const char *axis_size_name(int axis) {
    return axis == 0 ? "Width" : axis == 1 ? "Height" : "Depth";
  }

  // The pinned shape's bounding box (faint), its six face handles in the
  // axis colours, and the size of the face under the reticle or being
  // dragged.
  void draw_scale_handles() {
    if (!precise_scale_) {
      return;
    }
    glm::vec3 lo, hi;
    local_bounds(current_shape(), shaped_params(), &lo, &hi);
    // Box edges: corners differing in exactly one axis.
    for (int c = 0; c < 8; ++c) {
      for (int axis = 0; axis < 3; ++axis) {
        if (c & (1 << axis)) {
          continue;
        }
        const int d = c | (1 << axis);
        auto corner = [&](int k) {
          return pinned_center_ +
                 brush_rotation_ * glm::vec3((k & 1) ? hi.x : lo.x,
                                             (k & 2) ? hi.y : lo.y,
                                             (k & 4) ? hi.z : lo.z);
        };
        glm::vec2 a, b;
        if (project(corner(c), &a) && project(corner(d), &b)) {
          renderer_draw_line(a, b, glm::vec4(1.0f, 1.0f, 1.0f, 0.3f));
        }
      }
    }
    const int shown = drag_handle_ >= 0 ? drag_handle_ : hovered_handle_;
    for (int h = 0; h < 6; ++h) {
      glm::vec2 at;
      if (!project(handle_world(h), &at)) {
        continue;
      }
      const bool lit = h == shown;
      const f32 size = lit ? 22.0f : 14.0f;
      glm::vec4 colour = kAxisColours[h / 2];
      colour.a = lit ? 1.0f : 0.85f;
      renderer_draw_image("ui/coat/swatch.png", at - glm::vec2(0.5f * size),
                          glm::vec2(size), colour);
    }
    if (shown >= 0) {
      glm::vec2 at;
      if (project(handle_world(shown), &at)) {
        const int axis = shown / 2;
        char text[48];
        std::snprintf(text, sizeof(text), "%s %.2f", axis_size_name(axis),
                      hi[axis] - lo[axis]);
        // Above the handle, clear of the tooltip beside the reticle.
        draw_shadowed_text(text,
                           at + glm::vec2(-0.5f * text_width(text),
                                          -18.0f - kTipFontPixels),
                           kAxisColours[axis], 1.0f);
      }
    }
  }

  void reset_shape_options() {
    shape_scale_ = glm::vec3(1.0f);
    smoothness_index_ = kDefaultSmoothness;
    section_index_ = 0;
    KINFO("Shape options reset.");
  }

  std::vector<UiItem> shape_option_items() {
    std::vector<UiItem> items;
    const Shape s = current_shape();
    const std::string name = kSculptShapes[shape_index_].name;
    UiItem scale;
    scale.icon = "ui/toolbar/precise_scale.png";
    const bool shaping = tool_uses_shape();
    scale.enabled = shaping && shape_scalable(s);
    scale.active = precise_scale_;
    scale.label = !shaping ? std::string("Precise Scale (for tools that place a shape)")
                  : scale.enabled ? std::string("Precise Scale")
                                  : "Precise Scale (not for a " + name + ")";
    scale.press = [this, enabled = scale.enabled] {
      if (!enabled) {
        return;
      }
      if (precise_scale_) {
        end_precise_scale();
      } else {
        begin_precise_scale();
      }
    };
    items.push_back(scale);
    UiItem smooth;
    smooth.icon = "ui/toolbar/smoothness.png";
    smooth.active = side_sub_open_ && sub_row_ == SubRow::Smoothness;
    smooth.label = std::string("Smoothness: ") + kSmoothnessNames[smoothness_index_];
    smooth.press = [this] { toggle_sub_row(SubRow::Smoothness); };
    items.push_back(smooth);
    UiItem section;
    section.icon = kSectionIcons[shape_sectionable(s) ? section_index_ : 0];
    section.enabled = shape_sectionable(s);
    section.active = side_sub_open_ && sub_row_ == SubRow::Section;
    section.label = section.enabled
                        ? std::string("Section: ") + kSectionNames[section_index_]
                        : "Section (not for a " + name + ")";
    section.press = [this, enabled = section.enabled] {
      if (enabled) {
        toggle_sub_row(SubRow::Section);
      }
    };
    items.push_back(section);
    items.push_back(variation_button());
    UiItem reset;
    reset.icon = "ui/toolbar/revert.png";
    reset.label = "Reset Shape";
    reset.press = [this] { reset_shape_options(); };
    items.push_back(reset);
    return items;
  }

  void toggle_sub_row(SubRow row) {
    if (side_sub_open_ && sub_row_ == row) {
      side_sub_open_ = false;
      return;
    }
    sub_row_ = row;
    side_sub_anchor_ = row == SubRow::Smoothness ? 1   // its panel button
                       : row == SubRow::Section  ? 2
                                                 : 3;
    side_sub_open_ = true;
    side_sub_progress_ = 0.0f;
  }

  // --- Colour Variation -----------------------------------------------------
  //
  // A set of colours that every tool using the picked colour goes through
  // in place of it: Switch changes to the next colour, Blend fades from each
  // into the next in kVariationBlendSteps steps; either way round the set
  // and back to the start. Where the tool is along the set:
  //   Smear, Spray   how far along the stroke (a colour per
  //                  kVariationLength brush sizes)
  //   Stamp          a clock (kStampVariationRate): each shape takes the
  //                  colour the set is on as it's placed
  //   Tint, Hue      one step per object coated (each member of a group its
  //                  own step)
  // Its column (opened from the Colour row, or the side panel): the mode
  // (pressing cycles Off / Switch / Blend), Add (the colour picked in the
  // Colour row), then the set's colours (pressing one takes it out). Needs
  // two colours or more; otherwise the picked colour is used as usual.

  static constexpr const char *kVariationNames[3] = {"Off", "Switch", "Blend"};
  static constexpr const char *kVariationIcons[3] = {
      "ui/toolbar/variation_off.png", "ui/toolbar/variation_switch.png",
      "ui/toolbar/variation_blend.png"};

  bool variation_active() const {
    return variation_mode_ != 0 && variation_colours_.size() >= 2;
  }

  // Whether the current tool uses the picked colour (and so can vary it).
  bool tool_uses_colour() const {
    if (move_tool_) {
      return false;
    }
    if (mode_ == EditMode::Sculpt) {
      return sculpt_tool_ == SculptTool::Stamp ||
             sculpt_tool_ == SculptTool::Smear ||
             sculpt_tool_ == SculptTool::Spray;
    }
    if (mode_ == EditMode::Coat) {
      return coat_tool_ == CoatTool::Tint || coat_tool_ == CoatTool::Hue;
    }
    return false;
  }

  // The Colour Variation button (in the Colour row and the side panel):
  // opens the side panel at its column.
  UiItem variation_button() {
    UiItem variation;
    const bool usable = tool_uses_colour();
    variation.icon = kVariationIcons[variation_mode_];
    variation.enabled = usable;
    variation.active = variation_active() ||
                       (side_sub_open_ && sub_row_ == SubRow::Variation);
    variation.label = usable ? std::string("Colour Variation: ") +
                                   kVariationNames[variation_mode_]
                             : std::string("Colour Variation (not for this tool)");
    variation.press = [this, usable] {
      if (!usable) {
        return;
      }
      if (!side_open_) {
        toggle_side_panel();
      }
      toggle_sub_row(SubRow::Variation);
    };
    return variation;
  }

  // The colour at `travel` (world units along the stroke).
  glm::vec3 variation_colour(f32 travel) const {
    return variation_colour_along(travel / (kVariationLength * brush_size_));
  }

  // The colour `step` steps into the set (Stamp, Tint, Hue): Switch moves on
  // a colour each step, Blend a fade step.
  glm::vec3 variation_step_colour(int step) const {
    const int per = variation_mode_ == 1 ? 1 : kVariationBlendSteps;
    return variation_colour_along(static_cast<f32>(step) / static_cast<f32>(per));
  }

  // Stamp: the colour the clock is on now.
  glm::vec3 stamp_variation_colour() const {
    return variation_colour_along(variation_clock_ * kStampVariationRate);
  }

  // `along`: in colours of the set (1 = the next colour).
  glm::vec3 variation_colour_along(f32 along) const {
    const int n = static_cast<int>(variation_colours_.size());
    const int i = static_cast<int>(std::floor(along)) % n;
    if (variation_mode_ == 1) {
      return variation_colours_[static_cast<size_t>(i)];
    }
    const f32 step = std::round((along - std::floor(along)) *
                                static_cast<f32>(kVariationBlendSteps)) /
                     static_cast<f32>(kVariationBlendSteps);
    return glm::mix(variation_colours_[static_cast<size_t>(i)],
                    variation_colours_[static_cast<size_t>((i + 1) % n)], step);
  }

  std::vector<UiItem> variation_items() {
    std::vector<UiItem> items;
    UiItem mode;
    mode.icon = kVariationIcons[variation_mode_];
    mode.active = variation_mode_ != 0;
    mode.label = std::string("Mode: ") + kVariationNames[variation_mode_] +
                 (variation_mode_ != 0 && variation_colours_.size() < 2
                      ? " (add two colours or more)"
                      : "");
    mode.press = [this] {
      variation_mode_ = (variation_mode_ + 1) % 3;
      KINFO("Colour variation: {}.", kVariationNames[variation_mode_]);
    };
    items.push_back(mode);
    UiItem add;
    add.icon = "ui/toolbar/plus.png";
    const bool full = static_cast<int>(variation_colours_.size()) >= kMaxVariationColours;
    add.enabled = !full;
    add.label = full ? std::string("Add Colour (the set is full)")
                     : std::string("Add the picked colour");
    add.press = [this, full] {
      if (full) {
        return;
      }
      variation_colours_.push_back(coat_colour());
      if (variation_mode_ == 0 && variation_colours_.size() >= 2) {
        variation_mode_ = 2; // a set of two: start blending
      }
      KINFO("Colour variation: {} colour(s).", variation_colours_.size());
    };
    items.push_back(add);
    for (size_t i = 0; i < variation_colours_.size(); ++i) {
      UiItem colour;
      colour.swatch = true;
      colour.colour = variation_colours_[i];
      colour.label = "Colour " + std::to_string(i + 1) + " (press to take out)";
      colour.press = [this, i] {
        if (i < variation_colours_.size()) {
          variation_colours_.erase(variation_colours_.begin() +
                                   static_cast<std::ptrdiff_t>(i));
        }
      };
      items.push_back(colour);
    }
    return items;
  }

  std::vector<UiItem> smoothness_items() {
    std::vector<UiItem> items;
    for (int i = 0; i < kSmoothnessCount; ++i) {
      UiItem item;
      item.label = kSmoothnessNames[i];
      item.icon = "ui/toolbar/blend_" + std::to_string(i) + ".png";
      item.active = smoothness_index_ == i;
      item.press = [this, i] {
        smoothness_index_ = i;
        KINFO("Smoothness: {}.", kSmoothnessNames[i]);
      };
      items.push_back(item);
    }
    return items;
  }

  std::vector<UiItem> section_items() {
    std::vector<UiItem> items;
    for (int i = 0; i < kSectionCount; ++i) {
      UiItem item;
      item.label = kSectionNames[i];
      item.icon = kSectionIcons[i];
      item.active = section_index_ == i;
      item.press = [this, i] {
        section_index_ = i;
        KINFO("Section: {}.", kSectionNames[i]);
      };
      items.push_back(item);
    }
    return items;
  }

  // The look picked in the top bar (or, plain, the default one).
  SlotLook picked_look(bool plain) const {
    SlotLook look;
    if (!plain) {
      const CoatFinish &finish = kCoatFinishes[finish_index_];
      look.colour = coat_colour();
      look.roughness = finish.roughness;
      look.metallic = finish.metallic;
      look.waxy = finish.waxy ? 1.0f : 0.0f;
      look.fleck_style = fleck_style_index_;
    }
    return look;
  }

  // The sculpture's material slot with `look`, added to its coat if it
  // doesn't have one yet (the first look makes the coat). -1 if out of
  // slots.
  i32 sculpture_slot(const SlotLook &wanted) {
    auto same = [&](glm::vec3 colour, f32 roughness, f32 metallic, f32 waxy,
                    i32 fleck_style) {
      return glm::length(colour - wanted.colour) < 1e-3f &&
             std::fabs(roughness - wanted.roughness) < 1e-3f &&
             std::fabs(metallic - wanted.metallic) < 1e-3f &&
             (waxy > 0.5f) == (wanted.waxy > 0.5f) &&
             fleck_style == wanted.fleck_style;
    };
    auto it = coats_.find(sculpture_);
    if (it != coats_.end()) {
      ObjectCoat &coat = it->second;
      if (same(coat.colour, coat.roughness, coat.metallic, coat.waxy,
               coat.fleck_style)) {
        return coat.slot;
      }
      for (const SlotLook &look : coat.palette) {
        if (same(look.colour, look.roughness, look.metallic, look.waxy,
                 look.fleck_style)) {
          return look.slot;
        }
      }
    }
    const i32 slot = allocate_coat_slot();
    if (slot < 0) {
      if (!coat_slots_warned_) {
        KWARN("Sculpt: out of material slots; new looks reuse the first one.");
        coat_slots_warned_ = true;
      }
      return it != coats_.end() ? it->second.slot : kPlainSlot;
    }
    if (it == coats_.end()) {
      ObjectCoat coat;
      coat.slot = slot;
      coat.colour = wanted.colour;
      coat.roughness = wanted.roughness;
      coat.metallic = wanted.metallic;
      coat.waxy = wanted.waxy;
      coat.fleck_style = wanted.fleck_style;
      it = coats_.emplace(sculpture_, coat).first;
    } else {
      SlotLook look = wanted;
      look.slot = slot;
      it->second.palette.push_back(look);
    }
    apply_coat_material(it->second);
    sync_stroke_coats();
    return slot;
  }

  // Whether the sculpture has any shape in it yet.
  bool sculpture_has_shape() const {
    return !sculpture_shaped_members().empty();
  }

  // The current shape at `at`, as a stamp with `op` and material `slot`.
  SculptStampInfo sculpt_stamp(glm::vec3 at, SculptBrushOp op, i32 slot) const {
    SculptStampInfo stamp;
    stamp.center = at;
    stamp.shape_type = kSculptShapes[shape_index_].shape;
    stamp.params = shaped_params();
    stamp.smoothness = op == SculptBrushOp::ObjectUnion ||
                               op == SculptBrushOp::ObjectSubtraction
                           ? kSmoothnessLevels[smoothness_index_] * brush_size_
                           : 0.0f;
    stamp.op = op;
    stamp.material_slot = slot;
    stamp.rotation = brush_rotation_;
    if (op == SculptBrushOp::ObjectUnion) {
      stamp.looseness = kBrushLoosenessLevels[brush_looseness_index_];
    }
    if (op == SculptBrushOp::ObjectPaint) {
      // Paint carries its opacity in looseness: the stroke's seed plus
      // 1 - opacity (see paint_covers() in Builtin.SculptChunkRecompute).
      const f32 opacity = kOpacityLevels[opacity_index_];
      stamp.looseness =
          opacity >= 1.0f ? 0.0f : static_cast<f32>(paint_seed_) + (1.0f - opacity);
    }
    return stamp;
  }

  // What the current tool places: its op and material, false if it places
  // nothing at the brush.
  bool sculpt_tool_stamp(SculptBrushOp *op, i32 *slot) {
    switch (sculpt_tool_) {
    case SculptTool::Stamp:
    case SculptTool::Smear:
      *op = coat_subtract_ ? SculptBrushOp::ObjectSubtraction
                           : SculptBrushOp::ObjectUnion;
      *slot = coat_subtract_ ? kPlainSlot : sculpture_slot(picked_look(false));
      return true;
    case SculptTool::Spray:
      *op = SculptBrushOp::ObjectPaint;
      *slot = sculpture_slot(picked_look(coat_subtract_));
      return true;
    case SculptTool::Crop:
    case SculptTool::CutOut:
      *op = SculptBrushOp::ObjectIntersection;
      *slot = kPlainSlot;
      return true;
    default:
      return false;
    }
  }

  // One frame of sculpt mode: brush size, the preview, and R2's tool.
  void update_sculpt(f32 dt, bool r2_pressed) {
    if (mode_ != EditMode::Sculpt) {
      return;
    }
    if (move_tool_) { // no brush: the controls are base mode's
      renderer_sculpt_preview_brush(false, glm::vec3(0.0f),
                                    SculptBrushShape::Sphere, glm::vec4(0.0f),
                                    SculptBrushOp::Union, 0);
      stroke_active_ = false;
      end_precise_scale();
      return;
    }
    if (hovered_button_ < 0 && hovered_top_ < 0) {
      update_surface_brush(r2_pressed);
    }
    const f32 resize = (held(GamepadButton::DpadUp) ? 1.0f : 0.0f) -
                       (held(GamepadButton::DpadDown) ? 1.0f : 0.0f);
    if (resize != 0.0f) {
      brush_size_ = std::clamp(
          brush_size_ * std::exp2(resize * dt / kResizeDoublingTime),
          kSculptMinSize, kSculptMaxSize);
    }

    // The preview: the shape as it would be placed, in the picked look
    // (it has its own material slot, so it never touches a real one).
    const bool over_ui = hovered_button_ >= 0 || hovered_top_ >= 0;
    SculptBrushOp preview_op = SculptBrushOp::ObjectUnion;
    switch (sculpt_tool_) {
    case SculptTool::Spray: preview_op = SculptBrushOp::ObjectPaint; break;
    case SculptTool::Crop:
    case SculptTool::CutOut: preview_op = SculptBrushOp::ObjectIntersection; break;
    default:
      preview_op = coat_subtract_ ? SculptBrushOp::ObjectSubtraction
                                  : SculptBrushOp::ObjectUnion;
      break;
    }
    SlotLook look = picked_look(coat_subtract_ && sculpt_tool_ == SculptTool::Spray);
    if (sculpt_varies()) {
      look.colour = sculpt_tool_ == SculptTool::Stamp
                        ? stamp_variation_colour()
                        : variation_colour(stroke_active_ ? stroke_travel_ : 0.0f);
    }
    renderer_sculpt_set_material(kPreviewSlot, look.colour, look.roughness,
                                 look.metallic, look.waxy > 0.5f);
    // Precise Scale only goes with a tool that places a shape.
    if (precise_scale_ && !tool_uses_shape()) {
      end_precise_scale();
    }
    hovered_handle_ = handle_under_reticle();
    const SculptStampInfo preview = sculpt_stamp(
        precise_scale_ ? pinned_center_ : placement_point(), preview_op,
        kPreviewSlot);
    // An adding brush shows the blend it would make with what's there, a
    // carving one the hollow it would make (with the same smoothness).
    renderer_sculpt_set_preview_smoothness(
        preview.op == SculptBrushOp::ObjectUnion ||
                preview.op == SculptBrushOp::ObjectSubtraction
            ? preview.smoothness
            : 0.0f);
    renderer_sculpt_preview_brush(precise_scale_ ||
                                      (!over_ui && !framing_ &&
                                       brush_.z > 0.01f),
                                  preview.center, preview.shape_type,
                                  preview.params, preview.op, kPreviewSlot,
                                  preview.rotation);

    if (precise_scale_) {
      stroke_active_ = false;
      update_scale_drag(r2_pressed); // R2 drags handles here, never stamps
      return;
    }
    if (!r2_.down) {
      stroke_active_ = false;
      return;
    }
    if (over_ui && !stroke_active_) {
      return; // R2 over a button doesn't sculpt
    }
    switch (sculpt_tool_) {
    case SculptTool::Stamp:
      if (r2_pressed) {
        place_sculpt_stamps({placement_point()});
      }
      break;
    case SculptTool::Smear:
    case SculptTool::Spray:
      update_stroke(r2_pressed);
      break;
    case SculptTool::Crop:
      if (r2_pressed) {
        if (sculpture_has_shape()) {
          place_sculpt_stamps({placement_point()});
          KINFO("Cropped sculpture {}.", sculpture_);
        }
      }
      break;
    case SculptTool::CutOut:
      if (r2_pressed) {
        cut_out();
      }
      break;
    }
  }

  // Smear / spray: a shape where R2 went down, then one every
  // kSculptStrokeSpacing brush sizes along the brush's path.
  void update_stroke(bool r2_pressed) {
    const glm::vec3 at = placement_point();
    if (r2_pressed || !stroke_active_) {
      stroke_active_ = true;
      paint_seed_ = paint_seed_ % 4000u + 1u; // a new spray pattern each stroke
      stroke_travel_ = 0.0f; // Colour Variation starts at its first colour
      stroke_first_stamp_ = true;
      stroke_object_ = -1; // an adding stroke is an object of its own
      stroke_last_ = at;
      place_sculpt_stamps({at});
      return;
    }
    const f32 spacing = kSculptStrokeSpacing * brush_size_;
    const glm::vec3 delta = at - stroke_last_;
    const f32 distance = glm::length(delta);
    if (distance < spacing) {
      return;
    }
    const int count =
        std::min(static_cast<int>(distance / spacing), kSculptMaxStrokeStamps);
    std::vector<glm::vec3> points;
    for (int i = 1; i <= count; ++i) {
      points.push_back(stroke_last_ + delta * (static_cast<f32>(i) * spacing / distance));
    }
    stroke_last_ = points.back();
    place_sculpt_stamps(points);
  }

  // Whether what the sculpt tool places now goes through Colour Variation
  // (subtracting carves, or sprays plain: no colour).
  bool sculpt_varies() const {
    return tool_uses_colour() && !coat_subtract_ && variation_active();
  }

  // The current tool's shape at each of `points`, into the sculpture.
  void place_sculpt_stamps(const std::vector<glm::vec3> &points) {
    // Only adding makes something from nothing; carving, cropping or
    // painting nothing changes nothing.
    const bool adds = (sculpt_tool_ == SculptTool::Stamp ||
                       sculpt_tool_ == SculptTool::Smear) &&
                      !coat_subtract_;
    if (!adds && !sculpture_has_shape()) {
      return;
    }
    SculptBrushOp op;
    i32 slot;
    if (!sculpt_tool_stamp(&op, &slot)) {
      return;
    }
    std::vector<SculptStampInfo> stamps;
    // Colour Variation: each shape of a stroke takes the colour of where it
    // falls along the stroke; each stamp, the next step of the set.
    const bool varied = sculpt_varies();
    for (glm::vec3 point : points) {
      if (varied) {
        SlotLook look = picked_look(false);
        if (sculpt_tool_ == SculptTool::Stamp) {
          look.colour = stamp_variation_colour();
        } else {
          if (stroke_first_stamp_) {
            stroke_first_stamp_ = false; // the stroke's first shape is at 0
          } else {
            stroke_travel_ += kSculptStrokeSpacing * brush_size_;
          }
          look.colour = variation_colour(stroke_travel_);
        }
        slot = sculpture_slot(look);
      }
      stamps.push_back(sculpt_stamp(point, op, slot));
    }
    if (symmetry_on() && !stamps.empty()) {
      stamps = with_symmetry(stamps, symmetry_point(stamps.front().center));
    }
    if (adds) {
      // A new object per Stamp press or Smear stroke (see enter_sculpt()).
      if (sculpt_tool_ == SculptTool::Stamp || stroke_object_ < 0 ||
          std::find(sculpture_members_.begin(), sculpture_members_.end(),
                    stroke_object_) == sculpture_members_.end()) {
        stroke_object_ = begin_addition();
      }
      if (renderer_sculpt_add_stamps(stroke_object_, stamps.data(),
                                     static_cast<i32>(stamps.size()))) {
        history_dirty_ = true;
      }
      return;
    }
    // Carving, painting: every stroke of the sculpture the shape reaches
    // (its bounds against the shape's, a sphere round it). Cropping: every
    // stroke, since it takes away all of it outside the shape.
    const bool crops = op == SculptBrushOp::ObjectIntersection;
    std::vector<i32> owners;
    std::vector<SculptStampInfo> all;
    for (i32 member : sculpture_shaped_members()) {
      glm::vec3 lo, hi;
      renderer_sculpt_object_bounds(member, &lo, &hi);
      for (const SculptStampInfo &stamp : stamps) {
        const f32 reach = 2.0f * brush_size_ * std::max(
            shape_scale_.x, std::max(shape_scale_.y, shape_scale_.z));
        const glm::vec3 nearest = glm::clamp(stamp.center, lo, hi);
        if (!crops && glm::length(stamp.center - nearest) > reach) {
          continue;
        }
        owners.push_back(member);
        all.push_back(stamp);
      }
    }
    if (!all.empty() &&
        renderer_sculpt_add_stamps_to_objects(owners.data(), all.data(),
                                              static_cast<i32>(all.size()))) {
      history_dirty_ = true;
    }
  }

  // Cut Out: the part of the sculpture inside the shape becomes a sculpture
  // of its own (a copy of it, cropped to the shape) and is carved out of
  // this one. The piece keeps the sculpture's look.
  // With several strokes, each one the shape takes part of gives a piece;
  // the pieces are grouped.
  void cut_out() {
    if (!sculpture_has_shape()) {
      return;
    }
    const SculptStampInfo crop =
        sculpt_stamp(placement_point(), SculptBrushOp::ObjectIntersection, kPlainSlot);
    std::vector<i32> pieces;
    for (i32 member : sculpture_shaped_members()) {
      std::vector<SculptStampInfo> stamps(
          static_cast<size_t>(renderer_sculpt_object_stamps(member, nullptr, 0)));
      renderer_sculpt_object_stamps(member, stamps.data(),
                                    static_cast<i32>(stamps.size()));
      stamps.push_back(crop);
      const i32 piece = renderer_sculpt_begin_object();
      renderer_sculpt_add_stamps(piece, stamps.data(), static_cast<i32>(stamps.size()));
      glm::vec3 lo, hi;
      if (!renderer_sculpt_object_bounds(piece, &lo, &hi)) {
        renderer_sculpt_delete_objects(&piece, 1); // the shape missed it
        continue;
      }
      SculptStampInfo cutter = crop;
      cutter.op = SculptBrushOp::ObjectSubtraction;
      renderer_sculpt_add_stamps(member, &cutter, 1);
      if (auto coat = coats_.find(member); coat != coats_.end()) {
        coats_[piece] = coat->second; // shares its slots until either is coated
        coats_[piece].light = -1;
      }
      pieces.push_back(piece);
    }
    if (pieces.empty()) {
      KINFO("Cut out: the shape isn't on the sculpture.");
      return;
    }
    if (pieces.size() >= 2) {
      groups_.push_back({next_group_id_++, pieces});
      const i32 key = next_weld_key_++;
      for (i32 piece : pieces) {
        welds_[piece] = key; // blended like the strokes they came from
      }
    }
    history_dirty_ = true;
    KINFO("Cut {} piece(s) out of sculpture {}.", pieces.size(), sculpture_);
  }

  // L2 held in sculpt mode: turn the brush shape, with the same stick
  // mapping as turning an object (update_rotate()).
  void update_brush_rotate(f32 dt) {
    const f32 lx = stick_x(GamepadAxis::LeftX);
    const f32 ly = stick_up(GamepadAxis::LeftY);
    const f32 rx = stick_x(GamepadAxis::RightX);
    const f32 ry = stick_up(GamepadAxis::RightY);
    const f32 about_x = std::clamp(ly + ry, -1.0f, 1.0f);
    if (about_x == 0.0f && lx == 0.0f && rx == 0.0f) {
      return;
    }
    const f32 step = kRotateRate * dt;
    brush_rotation_ = glm::normalize(
        glm::angleAxis(about_x * step, camera_.right()) *
        glm::angleAxis(lx * step, screen_up()) *
        glm::angleAxis(rx * step, camera_.forward()) * brush_rotation_);
  }

  // L3 in sculpt mode: tip the brush shape 90 degrees toward the left
  // stick's direction (the same turn begin_flip() gives an object).
  void flip_brush() {
    const f32 x = stick_x(GamepadAxis::LeftX);
    const f32 up = stick_up(GamepadAxis::LeftY);
    if (std::max(std::fabs(x), std::fabs(up)) < kFlipStickThreshold) {
      return;
    }
    const glm::vec3 axis =
        std::fabs(up) >= std::fabs(x)
            ? camera_.right() * (up > 0.0f ? -1.0f : 1.0f)
            : camera_.forward() * (x > 0.0f ? 1.0f : -1.0f);
    brush_rotation_ =
        glm::normalize(glm::angleAxis(kFlipAngle, axis) * brush_rotation_);
  }

  void draw_toolbar() {
    for (int i = 0; i < side_count(); ++i) {
      if (button_shown(i) <= 0.0f) {
        continue;
      }
      const ToolbarButton button = shown_side_[i];
      const glm::vec2 centre = button_center(i);
      const bool hovered = hovered_button_ == static_cast<int>(button);
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

    // The reticle lives in 3D, at the brush point's depth: each arm is cut
    // into pieces, and a piece with geometry in front of it is hidden
    // (drawn as a faint ghost, so it's never lost). Over the UI it's flat.
    const bool over_ui = hovered_button_ >= 0 || hovered_top_ >= 0;
    bool everything = false;
    const std::vector<i32> occluders =
        over_ui ? std::vector<i32>{} : reticle_occluders(&everything);
    const bool can_hide = everything || !occluders.empty();
    const glm::vec4 ghost(colour.r, colour.g, colour.b,
                          colour.a * kReticleHiddenAlpha);
    const f32 piece = kReticleArm / static_cast<f32>(kReticlePieces);
    static constexpr glm::vec2 kArms[4] = {{-1.0f, 0.0f}, {1.0f, 0.0f},
                                           {0.0f, -1.0f}, {0.0f, 1.0f}};
    for (const glm::vec2 &arm : kArms) {
      for (int i = 0; i < kReticlePieces; ++i) {
        const f32 from = kReticleGap + piece * static_cast<f32>(i);
        const glm::vec2 a = centre + arm * from;
        const glm::vec2 b = centre + arm * (from + piece);
        const bool hidden =
            can_hide && reticle_point_hidden(0.5f * (a + b), occluders, everything);
        renderer_draw_line(a, b, hidden ? ghost : colour);
      }
    }
  }

  // What can hide the reticle: what's in focus -- the sculpture in sculpt
  // mode, the scoped objects when scoped in, otherwise everything (then
  // *everything is set and the list is empty) -- less whatever is held,
  // since the reticle rides on that.
  std::vector<i32> reticle_occluders(bool *everything) const {
    *everything = false;
    std::vector<i32> objects;
    if (mode_ == EditMode::Sculpt) {
      objects = sculpture_members_;
    } else if (scoped()) {
      objects = scope_members();
    } else if (held_group_.empty()) {
      *everything = true;
      return {};
    } else {
      objects = all_object_ids();
    }
    std::erase_if(objects, [&](i32 id) {
      return std::find(held_group_.begin(), held_group_.end(), id) !=
             held_group_.end();
    });
    return objects;
  }

  // Whether geometry (from `occluders`, or all of it) lies between the
  // camera and the reticle's depth along the line of sight through `pixel`.
  bool reticle_point_hidden(glm::vec2 pixel, const std::vector<i32> &occluders,
                            bool everything) const {
    // The inverse of the reticle's projection (see reticle_screen()).
    const glm::vec3 c((pixel.x - 0.5f * screen_width_) / screen_height_ * brush_.z,
                      (0.5f * screen_height_ - pixel.y) / screen_height_ * brush_.z,
                      brush_.z);
    const glm::vec3 origin = camera_.position();
    const glm::vec3 at = origin + camera_.right() * c.x + screen_up() * c.y +
                         camera_.forward() * c.z;
    const f32 distance = glm::length(at - origin);
    if (distance <= 1e-4f) {
      return false;
    }
    const glm::vec3 direction = (at - origin) / distance;
    // A hair short of the reticle, so a surface it sits on doesn't hide it.
    const f32 reach = distance * (1.0f - kReticleDepthSlack);
    f32 t = 0.0f;
    const i32 hit =
        everything ? renderer_sculpt_raycast_object(origin, direction, reach, &t)
                   : renderer_sculpt_raycast_objects(
                         origin, direction, reach, occluders.data(),
                         static_cast<i32>(occluders.size()), &t);
    return hit >= 0 && t < reach;
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
      switch (button) {
      case ToolbarButton::Group:
        rows.push_back({{"dpad_up", nullptr}, "Shortcut"});
        break;
      case ToolbarButton::Scope:
        rows.push_back({{"dpad_down", nullptr}, "Shortcut"});
        if (scoped()) {
          rows.push_back({{"b", nullptr}, "Shortcut"});
        }
        break;
      case ToolbarButton::Delete: // Y on any selected object
        rows.push_back({{"y", nullptr}, "Shortcut (on a selected object)"});
        break;
      default:
        break;
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
      const std::vector<UiItem> items = layer_items(layer);
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
          {{"r1", "ls"}, "Push/pull"},
          {{"dpad_updown", nullptr}, "Resize"},
          {{"y", nullptr}, "Snap"},
          {{"r2", nullptr}, "Drop"},
      };
    }
    if (grabbed_object_ >= 0) {
      return {
          {{"ls", nullptr}, "Move"},
          {{"r1", "ls"}, "Push/pull"},
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
    if (placing()) {
      return {
          {{"r2", nullptr}, "Stamp"},
          {{"dpad_updown", nullptr}, "Size"},
          {{"b", nullptr}, "Done"},
          {{"ls", nullptr}, "Move"},
          {{"rs", nullptr}, "Look"},
      };
    }
    if (mode_ == EditMode::Sculpt && !move_tool_ && precise_scale_ && !l2_.down) {
      std::vector<TipRow> rows;
      if (drag_handle_ >= 0 || hovered_handle_ >= 0) {
        rows.push_back({{"r2", nullptr}, "Drag face"});
      }
      rows.push_back({{"b", nullptr}, "Done"});
      rows.push_back({{"dpad_updown", nullptr}, "Size"});
      rows.push_back({{"l2", nullptr}, "Turn shape"});
      return rows;
    }
    if (mode_ == EditMode::Sculpt && !move_tool_) {
      if (l2_.down) {
        return {
            {{"ls", nullptr}, "Spin shape"},
            {{"rs", nullptr}, "Roll shape"},
            {{"l2", nullptr}, "Done"},
        };
      }
      const bool subtract = coat_subtract_;
      std::string tool = kSculptToolNames[static_cast<int>(sculpt_tool_)];
      if (sculpt_tool_ != SculptTool::Crop && sculpt_tool_ != SculptTool::CutOut) {
        tool += subtract ? " -" : " +";
      }
      return {
          {{"r2", nullptr}, tool},
          {{"y", nullptr}, subtract ? "Add" : "Subtract"},
          {{"dpad_updown", nullptr}, "Size"},
          {{"l2", nullptr}, "Turn shape"},
          {{"ls", nullptr}, "Move"},
          {{"rs", nullptr}, "Look"},
          {{"r1", nullptr}, "Orbit"},
          {{"l1", "b"}, "Exit"},
      };
    }
    if (hovered_object_ >= 0 && !move_tool_ &&
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
    if (hovered_object_ >= 0 && !move_tool_ && mode_ == EditMode::Coat) {
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
          {{"l1", "ls"}, "Forward/back"},
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
        {{"l1", "ls"}, "Forward/back"},
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
    std::unordered_map<i32, i32> welds;
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
    snap.welds = welds_;
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
    if (!history_dirty_ || placing() || grabbed_object_ >= 0 || rotating_object_ >= 0 ||
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
      // Rebuilt from the list in one go (exact for object-scoped stamps).
      const std::vector<SculptStampInfo> &stamps = target.objects[i].stamps;
      renderer_sculpt_add_stamps(id, stamps.data(), static_cast<i32>(stamps.size()));
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
      // The sculpture being made, if it was re-stamped.
      if (auto it = new_id.find(sculpture_); it != new_id.end()) {
        sculpture_ = it->second;
      }
      for (i32 &member : sculpture_members_) {
        if (auto it = new_id.find(member); it != new_id.end()) {
          member = it->second;
        }
      }
      if (auto it = new_id.find(scoped_object_); it != new_id.end()) {
        scoped_object_ = it->second;
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
    welds_ = target.welds;
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
    if (mode_ == EditMode::Sculpt) {
      // Strokes undone are gone; the first object stays (it may be empty).
      std::erase_if(sculpture_members_, [&](i32 member) {
        return member != sculpture_ && !object_exists(member);
      });
      stroke_object_ = -1;
    }
    if (scoped_group_ >= 0 && !scoped_group()) {
      scoped_group_ = -1;
    }
    if (scoped_object_ >= 0 && !object_exists(scoped_object_)) {
      scoped_object_ = -1;
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
    std::unordered_map<i32, i32> welds;
    for (const auto &[id, key] : snap.welds) {
      welds[map(id)] = key;
    }
    snap.welds = std::move(welds);
  }

  std::string stamps_path_;
  SculptBench bench_;
  std::chrono::steady_clock::time_point frame_begin_;
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
  // Object -> weld key (see sync_welds()); sent_welds_ is what the engine
  // has.
  std::unordered_map<i32, i32> welds_;
  std::unordered_map<i32, i32> sent_welds_;
  i32 next_weld_key_ = 1;

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
  i32 scoped_object_ = -1; // the scoped-in object (alone or in that group), or -1
  int hovered_button_ = -1; // toolbar button under the reticle, or -1
  f32 toolbar_slide_ = 0.0f; // 0 = slid out, 1 = in (see button_shown())
  // Top bar: the menu asked for, the one whose row is on screen (kept while
  // it animates closed), its drop progress, and the hovered top-bar id.
  EditMode mode_ = EditMode::Default;
  bool move_tool_ = false; // Move picked in a mode's Tools (see modeless())
  // Coat mode state: the tool, add/subtract, the picked colour (index +
  // shade) and finish, each object's coat, and the material slots.
  CoatTool coat_tool_ = CoatTool::Tint;
  StyleTool style_tool_ = StyleTool::Looseness;
  EffectsTool effects_tool_ = EffectsTool::Boil;
  // Sculpt mode: the sculpture being made (-1 outside sculpt mode), the
  // tool and shape, the brush's size and orientation (shapes are modelled
  // y-up; world +Y is down on screen, so by default they're turned over to
  // stand upright), and the smear/spray stroke under way.
  std::unordered_map<i32, FileCoat> loaded_coats_; // engine object -> coat
  // Search: the library listing (newest first) and the imported copy
  // following the reticle, if any (see update_placing()).
  std::vector<LibraryEntry> library_;
  std::unique_ptr<LibraryScan> library_scan_; // a search under way, if any
  std::vector<i32> placing_;
  glm::vec3 placing_center_{0.0f};
  i32 sculpture_ = -1;                  // the sculpture's first object
  std::vector<i32> sculpture_members_;  // all its objects (strokes), first first
  i32 stroke_object_ = -1;              // the object this adding stroke fills
  SculptTool sculpt_tool_ = SculptTool::Smear;
  int shape_index_ = 0;
  f32 brush_size_ = kSculptDefaultSize;
  glm::quat brush_rotation_ =
      glm::angleAxis(3.14159265f, glm::vec3(1.0f, 0.0f, 0.0f));
  bool stroke_active_ = false;
  glm::vec3 stroke_last_{0.0f};
  // Undo / redo (see capture_scene()).
  SceneSnapshot committed_;               // the scene as of the last step
  std::vector<SceneSnapshot> undo_stack_; // older steps, newest last
  std::vector<SceneSnapshot> redo_stack_; // undone steps, newest last
  bool history_dirty_ = false;            // an action has changed things
  std::unordered_map<i32, f32> loaded_looseness_; // see adopt_loaded_looseness()
  std::unordered_set<i32> style_checked_; // see take_stamped_looseness()
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
  // Guides (see the Guides menu). guide_* track the held object's
  // unsnapped anchor position and yaw, so snapped moves accumulate the
  // stick's motion instead of losing it to rounding each frame;
  // guide_object_ is the object they belong to (-1: start afresh).
  // Shape options (see shaped_params()): per-axis stretch from Precise
  // Scale, the smoothness and section picked, and Precise Scale's state --
  // the shape pinned in place, and the face handle under the reticle /
  // being dragged (-1: none).
  glm::vec3 shape_scale_{1.0f};
  int smoothness_index_ = kDefaultSmoothness;
  int section_index_ = 0;
  int opacity_index_ = kDefaultOpacity; // Spray Paint's (kOpacityLevels)
  int brush_looseness_index_ = 0;       // Stamp / Smear (kBrushLoosenessLevels)
  u32 paint_seed_ = 0; // the spray stroke's pattern (see sculpt_stamp())
  SubRow sub_row_ = SubRow::Smoothness;
  // The Shape Options panel (see side_item_center()).
  bool side_open_ = false;
  f32 side_progress_ = 0.0f;
  bool side_sub_open_ = false;
  f32 side_sub_progress_ = 0.0f;
  int side_sub_anchor_ = 1; // the panel button its column is centred on
  // Colour Variation (see variation_items()): 0 Off, 1 Switch, 2 Blend; the
  // set; how far the current smear stroke has gone.
  int variation_mode_ = 0;
  std::vector<glm::vec3> variation_colours_;
  f32 variation_clock_ = 0.0f;   // seconds, for Stamp (kStampVariationRate)
  int coat_variation_step_ = 0;  // Tint / Hue: the next object's step
  int coat_variation_base_ = 0;  // ...and the one being coated now's
  i32 coat_variation_key_ = -1;  // (its group's first member)
  f32 stroke_travel_ = 0.0f;
  bool stroke_first_stamp_ = true;
  bool precise_scale_ = false;
  glm::vec3 pinned_center_{0.0f};
  int hovered_handle_ = -1;
  int drag_handle_ = -1;
  // The side column's buttons as drawn now (they change only while it is
  // slid out -- see update_toolbar_slide()).
  std::vector<ToolbarButton> shown_side_;
  bool grid_snap_ = false;
  int grid_size_index_ = kDefaultGridSize;
  bool upright_ = false;
  bool right_angles_ = false;
  bool precise_move_ = false;
  bool surface_snap_ = false;
  bool tentacle_snap_ = false;
  i32 tentacle_object_ = -1; // the held object Tentacle Snap has let go of
  i32 snap_stamp_limit_ = -1; // the scene's stamp count as the stroke began
  bool mirror_ = false;
  bool kaleidoscope_ = false;
  int kaleidoscope_count_ = kKaleidoscopeDefault;
  i32 guide_object_ = -1;
  glm::vec3 guide_target_{0.0f};
  f32 guide_yaw_ = 0.0f;
  // Precise move: where the held object's centre was when it was grabbed.
  i32 move_start_object_ = -1;
  glm::vec3 move_start_{0.0f};
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
