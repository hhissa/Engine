// Scene-state switching: the per-chapter staging table below (which room
// .sdf each of the 40 SceneState values shows, from which camera stations,
// with which parts of the subject censored), apply_scene_state() which
// applies one of its entries, and register_scene_states()/
// register_chapter_scene_states(), which wire .conversation tag= names to
// it -- see game.h's "game_scene_state.cpp" section.

#include "game.h"

#include "text_wrap.h"

#include <audio/audio.h>
#include <renderer/renderer_frontend.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>

namespace {

// Every room below is an interior, so the sky box is only ever seen
// through a window -- one shared choice rather than a per-state one. Set
// from code (not from each .sdf's own `skybox=` line) because an .sdf that
// doesn't name one means "unspecified", not "none": letting the files
// decide would leave whichever sky box the previous scene happened to set
// in place for five of the six rooms.
constexpr std::string_view kSkyBox = "skybox_to_equirect_2";

// A camera station authored the way it's actually reasoned about -- "stand
// here, look at that" -- rather than as the raw (yaw, pitch) pair
// CameraPose wants. Every eye/target below is a literal world-space
// coordinate readable straight out of the room's own .sdf file (the
// subject's head sphere, the middle of the bed, the studio's backdrop
// panel), so a station can be checked, and re-aimed, against the geometry
// without having to picture what a given yaw in radians points at.
//
// Note the rooms are authored -Y up (a floor slab sits at y = 0 and the
// ceiling above it at, say, y = -2.4), which is what makes every eye/
// target height below negative.
struct Station {
  glm::vec3 eye;
  glm::vec3 target;
  f32 max_pan;      // see CameraPose::max_pan
  f32 max_zoom_out; // see CameraPose -- both are distances from `eye`, and
  f32 max_zoom_in;  // are sized per room: these interiors range from a
                    // 2m-across bedroom to an office authored ~2.5x life
                    // size, so CameraPose's own defaults (1.5/3.0) would
                    // dolly straight through a wall in half of them.
};

// world_scale is the room's own ChapterStaging::world_scale -- the station
// is authored in room coordinates, so every length here scales with it.
// yaw/pitch don't: a uniform scale about the origin leaves directions
// alone. Neither does max_pan, which is an angle in radians.
CameraPose pose_from(const Station &station, f32 world_scale) {
  const glm::vec3 to_target = station.target - station.eye;
  const f32 distance = glm::length(to_target);

  CameraPose pose;
  pose.position = station.eye * world_scale;
  // Inverts Camera::forward()'s (cos(pitch)sin(yaw), sin(pitch),
  // cos(pitch)cos(yaw)). A station whose target is directly above or below
  // the eye leaves yaw at 0 (atan2(0, 0)), which is harmless -- there's no
  // meaningful heading for a straight-down look anyway.
  pose.yaw = std::atan2(to_target.x, to_target.z);
  pose.pitch = distance > 0.0f ? std::asin(to_target.y / distance) : 0.0f;
  pose.max_pan = station.max_pan;
  pose.max_zoom_out = station.max_zoom_out * world_scale;
  pose.max_zoom_in = station.max_zoom_in * world_scale;
  return pose;
}

constexpr size_t kRoomsPerChapter = 5;

// The tag= names every ch*.conversation file marks its questions with, in
// the order their chapter's five SceneState values are declared in -- see
// register_chapter_scene_states().
constexpr std::array<std::string_view, kRoomsPerChapter> kRoomTags = {
    "Room_01", "Room_02", "Room_03", "Room_04", "Room_05"};

// One chapter's worth of staging: the room it plays in, and the camera
// stations for each of its five "Room_01".."Room_05" states. The five are
// deliberately the same room from progressively closer, more oblique
// stations rather than five different sets of geometry -- the tags mark
// how deep into an interview the player has gotten, so the room stops
// being the subject and the person in it starts being it. Room_01 opens
// wide from the doorway end with three stations to cut between; Room_05 is
// one or two near-locked close-ups with barely any pan or zoom left.
// A figure stood into a room that doesn't author one of its own, placed
// the way SceneRef's chained transforms work -- scale about the world
// origin first, then translate -- so `translate` is a plain world-space
// position and `scale` is the factor that makes the model life-sized for
// that particular room. An empty scene_path means the room is already
// complete, which is the case for the five that have their subject
// modelled into the .sdf itself.
struct Figure {
  std::string_view scene_path;
  f32 scale = 1.0f;
  glm::vec3 translate{0.0f};
};

struct ChapterStaging {
  std::string_view scene_path;
  // How much to blow this room up on load. The chunked field's finest clip
  // level has a FIXED voxel size of COARSE_CELL_SIZE/CHUNK_BRICK_DIM =
  // 0.25/16 = 0.015625 world units (see Builtin.SdfFieldConfig.inc.glsl),
  // and it is world-anchored, not screen-anchored -- so how much detail a
  // model resolves to depends only on how big it is in world units, and
  // walking the camera up to it buys nothing once it is already on level
  // 0. These rooms are authored at wildly different scales (Mel's head is
  // a 0.06-radius sphere, Diego's a 0.3-radius one), so left at 1:1 the
  // small ones render a head across ~8 voxels and look faceted no matter
  // where the camera stands. Each factor below brings its room up to the
  // same ~0.6-unit head Diego's office already has, i.e. ~38 voxels
  // across, which is what the level-0 voxel size can actually deliver.
  //
  // Everything else in this entry -- every station, the figure, the censor
  // points -- is authored in the room's OWN coordinates and multiplied by
  // this at use, so the numbers below stay readable against the .sdf file
  // and this stays the single knob for a room's detail. Raising it is
  // close to free on the brick pool (a measured peak of 2173 of 81920
  // bricks, 3%, at the largest factor here) but does push the far walls
  // out of level 0's 12-unit window onto coarser levels -- which is the
  // clipmap working, and still finer per room-unit than 1:1 was.
  f32 world_scale = 1.0f;
  Figure figure;
  std::array<std::vector<Station>, kRoomsPerChapter> stations;
  // Where this chapter's subject's face and lap are, in world space -- see
  // censor_points_for() and its caller in game_playing.cpp. Empty for a
  // chapter whose subject isn't modelled yet.
  std::vector<CensorPoint> censor_points;
};

// The PhotoStudio is the one room with no subject of its own, so the three
// chapters shot in it stand assets/scenes/props/man.sdf on its shooting
// stage instead. man.sdf's origin sits at the head with the body running
// towards +y (scenes are -Y up) and it measures 6.29 units head to heel,
// so 0.165 makes him 1.04 tall, and the translate drops his heels onto the
// stage's top surface at y = -0.30. The z of 0.69 lands his body's centre
// at z = 0.71 (his own primitives sit ~0.11 forward of his origin),
// between the platform's front edge and its backdrop panel.
//
// Sized against the studio's own props rather than against the room's
// 2.02 of clear height: the backdrop panel he stands in front of is 1.0
// tall, its frame 1.4, and the light stands 0.65, so the 0.29 this used
// to be -- 1.82 tall, filling 90% of the room and overshooting the
// backdrop by most of a head -- read as a giant in a doll's house. At
// 0.165 he tops out level with the frame's upper edge (y = -1.34).
//
// He faces -z (his toes and the bump of his nose both run that way), which
// is where every station in all three chapters shoots from, so he needs no
// rotation.
constexpr Figure kStudioFigure{"assets/scenes/props/man.sdf", 0.165f,
                               glm::vec3(0.0f, -1.22f, 0.69f)};

// man.sdf's head sphere (local y -0.32, radius 0.4) and lap (local y 2.75,
// the same point the pre-rooms build censored) put through kStudioFigure's
// own scale/translate.
const std::vector<CensorPoint> kStudioCensorPoints = {
    {{0.00f, -1.27f, 0.71f}, 0.08f},
    {{0.01f, -0.77f, 0.71f}, 0.07f},
};

// Indexed by chapter, in exactly kChapters' order (game_menus.cpp), which
// is also the order SceneState's five-enumerator blocks are declared in --
// the static_asserts below are what keeps that true. Three chapters share
// PhotoStudio.sdf (it's the one room more than one person is interviewed
// in), and differ by which side of it their stations favour: Thiago's sit
// camera-left and low, Alex's camera-right and high, and the
// photographer's own dead on the lens axis, since chapter 8 is him sitting
// in his own chair.
const std::array<ChapterStaging, 8> kStaging = {{
    // --- Chapter 1: Theo -------------------------------------------------
    // 4 x 6.8 room, floor y = 0, ceiling y = -4.81. Theo is sat up on the
    // bed platform along the back wall (bed surface y = -0.97, z >= 1.2),
    // head at (-1.28, -1.95, 2.53), legs out towards +x. Stations stay on
    // the open floor at z < 1.2 so none of them is inside the bed.
    {"assets/scenes/rooms/TheosRoom.sdf",
     /*world_scale=*/1.5f, // Theo's head sphere is radius 0.2
     /*figure=*/{},
     {{
         {{{0.45f, -2.5f, -2.6f}, {-1.1f, -1.75f, 2.45f}, 0.1f, 0.62f, 1.8f},
          {{-1.2f, -2.4f, -2.3f}, {-1.1f, -1.75f, 2.45f}, 0.1f, 0.86f, 1.8f},
          {{-0.3f, -3.6f, -2.8f}, {-1.15f, -1.85f, 2.45f}, 0.08f, 0.43f, 1.6f}},
         {{{0.35f, -2.3f, -1.2f}, {-1.15f, -1.8f, 2.5f}, 0.12f, 1.0f, 1.4f},
          {{-1.15f, -2.3f, -1.0f}, {-1.15f, -1.8f, 2.5f}, 0.12f, 1.0f, 1.4f},
          {{-0.4f, -1.05f, -1.5f}, {-1.2f, -1.85f, 2.5f}, 0.1f, 0.8f, 1.2f}},
         {{{0.25f, -2.2f, -0.1f}, {-1.2f, -1.85f, 2.52f}, 0.1f, 0.8f, 1.0f},
          {{-1.1f, -2.2f, 0.05f}, {-1.2f, -1.85f, 2.52f}, 0.1f, 0.8f, 1.0f},
          {{-0.45f, -2.85f, -0.4f}, {-1.2f, -1.88f, 2.52f}, 0.12f, 0.8f, 1.0f}},
         {{{0.05f, -2.1f, 0.75f}, {-1.25f, -1.88f, 2.53f}, 0.08f, 0.6f, 0.7f},
          {{-1.05f, -2.1f, 0.85f}, {-1.25f, -1.88f, 2.53f}, 0.08f, 0.6f, 0.7f}},
         {{{-0.55f, -2.0f, 1.1f}, {-1.28f, -1.92f, 2.53f}, 0.05f, 0.5f, 0.5f},
          {{-0.1f, -1.55f, 1.05f}, {-1.28f, -1.9f, 2.53f}, 0.05f, 0.5f, 0.5f}},
     }},
     {{{-1.28f, -1.93f, 2.53f}, 0.26f}, {{-0.45f, -1.20f, 2.35f}, 0.25f}}},

    // --- Chapter 2: Thiago (PhotoStudio) ---------------------------------
    // 6.4 x 4.2 studio, floor y = -0.14, ceiling y = -2.36. The shooting
    // stage is the low platform at x[-0.7, 0.7], z[0.25, 1.25] (top
    // y = -0.30) with its backdrop panel at z = 1.05; kStudioFigure stands
    // on it at z = 0.71 and every target below is a point on him -- they
    // climb his body as the five states tighten, from waist height in
    // Room_01 to his face (y = -1.27) in Room_05.
    {"assets/scenes/rooms/PhotoStudio.sdf",
     /*world_scale=*/4.5f, // kStudioFigure's head lands at radius 0.297
     kStudioFigure,
     {{
         {{{-2.2f, -1.5f, -1.3f}, {-0.05f, -0.93f, 0.7f}, 0.1f, 0.42f, 1.5f},
          {{1.59f, -1.5f, -1.38f}, {-0.05f, -0.93f, 0.7f}, 0.1f, 0.25f, 1.5f},
          {{-0.1f, -2.03f, -1.51f}, {-0.05f, -0.93f, 0.7f}, 0.08f, 0.07f, 1.4f}},
         {{{-1.7f, -1.48f, -0.7f}, {-0.05f, -1.03f, 0.7f}, 0.12f, 0.9f, 1.2f},
          {{0.95f, -1.5f, -0.95f}, {-0.05f, -1.03f, 0.7f}, 0.12f, 0.7f, 1.2f},
          {{-0.3f, -0.62f, -1.1f}, {-0.05f, -1.03f, 0.7f}, 0.1f, 0.49f, 1.0f}},
         {{{-1.3f, -1.55f, -0.25f}, {-0.05f, -1.12f, 0.7f}, 0.1f, 0.7f, 0.9f},
          {{0.7f, -1.55f, -0.35f}, {-0.05f, -1.12f, 0.7f}, 0.1f, 0.7f, 0.57f},
          {{-0.55f, -1.35f, -0.6f}, {-0.05f, -1.12f, 0.7f}, 0.14f, 0.7f, 0.74f}},
         {{{-0.95f, -1.6f, 0.1f}, {-0.05f, -1.21f, 0.7f}, 0.08f, 0.5f, 0.44f},
          {{0.55f, -1.6f, 0.0f}, {-0.05f, -1.21f, 0.7f}, 0.08f, 0.5f, 0.21f}},
         {{{-0.35f, -1.58f, 0.05f}, {-0.05f, -1.27f, 0.7f}, 0.05f, 0.4f, 0.07f},
          {{-0.7f, -0.95f, 0.15f}, {-0.05f, -1.27f, 0.7f}, 0.05f, 0.4f, 0.4f}},
     }},
     kStudioCensorPoints},

    // --- Chapter 3: Alex (PhotoStudio) -----------------------------------
    // Same studio as chapter 2, worked from the opposite side and from
    // slightly above, so Alex's five states don't replay Thiago's.
    {"assets/scenes/rooms/PhotoStudio.sdf",
     /*world_scale=*/4.5f, // kStudioFigure's head lands at radius 0.297
     kStudioFigure,
     {{
         {{{2.29f, -1.58f, -1.34f}, {0.05f, -0.93f, 0.7f}, 0.1f, 0.36f, 1.5f},
          {{-1.48f, -1.51f, -1.42f}, {0.05f, -0.93f, 0.7f}, 0.1f, 0.19f, 1.5f},
          {{0.2f, -2.03f, -1.56f}, {0.05f, -0.93f, 0.7f}, 0.08f, 0.02f, 1.4f}},
         {{{1.75f, -1.6f, -0.75f}, {0.05f, -1.03f, 0.7f}, 0.12f, 0.9f, 1.2f},
          {{-0.9f, -1.55f, -1.0f}, {0.05f, -1.03f, 0.7f}, 0.12f, 0.64f, 1.2f},
          {{0.4f, -1.98f, -1.15f}, {0.05f, -1.03f, 0.7f}, 0.1f, 0.44f, 1.0f}},
         {{{1.35f, -1.65f, -0.3f}, {0.05f, -1.12f, 0.7f}, 0.1f, 0.7f, 0.9f},
          {{-0.65f, -1.6f, -0.4f}, {0.05f, -1.12f, 0.7f}, 0.1f, 0.7f, 0.58f},
          {{0.6f, -1.88f, -0.65f}, {0.05f, -1.12f, 0.7f}, 0.14f, 0.7f, 0.79f}},
         {{{1.0f, -1.7f, 0.05f}, {0.05f, -1.21f, 0.7f}, 0.08f, 0.5f, 0.5f},
          {{-0.5f, -1.65f, -0.05f}, {0.05f, -1.21f, 0.7f}, 0.08f, 0.5f, 0.21f}},
         {{{0.4f, -1.65f, 0.05f}, {0.05f, -1.27f, 0.7f}, 0.05f, 0.4f, 0.09f},
          {{0.74f, -1.97f, 0.16f}, {0.05f, -1.27f, 0.7f}, 0.05f, 0.36f, 0.22f}},
     }},
     kStudioCensorPoints},

    // --- Chapter 4: Diego ------------------------------------------------
    // A cubicle corner (partitions at x = 0 and z = 0.43, desk surface at
    // y = -1.95) standing on an effectively infinite office floor, and the
    // one room authored well above life size -- Diego's head sphere alone
    // has radius 0.3 and sits 3.64 above the floor, so every distance and
    // zoom limit here is ~2.5x the other rooms'. He's sat facing -z with
    // his legs out that way, which is where all the stations are.
    {"assets/scenes/rooms/DiegosOffice.sdf",
     /*world_scale=*/1.0f, // already ~2.5x life: head radius 0.3
     /*figure=*/{},
     {{
         {{{-2.4f, -3.3f, -8.5f}, {-2.54f, -2.9f, -2.12f}, 0.1f, 3.0f, 4.5f},
          {{-7.1f, -3.2f, -6.2f}, {-2.54f, -2.9f, -2.12f}, 0.1f, 1.98f, 4.5f},
          {{-2.3f, -5.0f, -7.6f}, {-2.54f, -3.0f, -2.12f}, 0.08f, 2.29f, 4.0f}},
         {{{-2.45f, -3.4f, -6.6f}, {-2.54f, -3.1f, -2.12f}, 0.12f, 2.5f, 3.0f},
          {{-5.6f, -3.35f, -5.4f}, {-2.54f, -3.1f, -2.12f}, 0.12f, 2.5f, 3.0f},
          {{-1.05f, -2.1f, -5.4f}, {-2.54f, -3.2f, -2.12f}, 0.1f, 2.0f, 2.5f}},
         {{{-2.5f, -3.5f, -5.3f}, {-2.54f, -3.3f, -2.12f}, 0.1f, 2.0f, 2.0f},
          {{-4.55f, -3.5f, -4.9f}, {-2.54f, -3.3f, -2.12f}, 0.1f, 2.0f, 2.0f},
          {{-0.95f, -3.45f, -4.7f}, {-2.54f, -3.3f, -2.12f}, 0.1f, 1.37f, 2.0f}},
         {{{-2.55f, -3.55f, -4.4f}, {-2.54f, -3.45f, -2.12f}, 0.07f, 1.4f, 1.4f},
          {{-3.85f, -3.55f, -4.2f}, {-2.54f, -3.45f, -2.12f}, 0.07f, 1.4f, 1.4f}},
         {{{-2.54f, -3.6f, -3.8f}, {-2.54f, -3.56f, -2.12f}, 0.05f, 1.0f, 0.9f},
          {{-1.7f, -3.05f, -3.9f}, {-2.54f, -3.52f, -2.12f}, 0.05f, 1.0f, 0.9f}},
     }},
     {{{-2.54f, -3.60f, -2.12f}, 0.40f}, {{-2.55f, -1.45f, -2.25f}, 0.35f}}},

    // --- Chapter 5: Mel --------------------------------------------------
    // The smallest room in the game -- 2.1 x 3.0, floor y = -0.07, ceiling
    // y = -2.04, open at -z where the fourth wall would be, which is the
    // only place a camera fits. Mel is sat on the edge of the mattress
    // facing -z, head at (0.04, -0.91, 0.00). Authored about 0.65x life
    // size, hence the very short zoom limits.
    {"assets/scenes/rooms/MelsRoom.sdf",
     /*world_scale=*/5.0f, // the smallest room and the smallest subject: head radius 0.06
     /*figure=*/{},
     {{
         {{{0.73f, -1.04f, -1.26f}, {0.04f, -0.72f, 0.0f}, 0.1f, 0.05f, 0.6f},
          {{-0.83f, -1.04f, -1.22f}, {0.04f, -0.72f, 0.0f}, 0.1f, 0.11f, 0.6f},
          {{0.0f, -1.58f, -1.15f}, {0.04f, -0.78f, 0.0f}, 0.08f, 0.19f, 0.6f}},
         {{{0.6f, -1.0f, -0.95f}, {0.04f, -0.78f, 0.0f}, 0.12f, 0.4f, 0.5f},
          {{-0.69f, -1.0f, -0.89f}, {0.04f, -0.78f, 0.0f}, 0.12f, 0.36f, 0.5f},
          {{0.05f, -0.46f, -1.03f}, {0.04f, -0.84f, 0.0f}, 0.1f, 0.29f, 0.5f}},
         {{{0.5f, -1.0f, -0.65f}, {0.04f, -0.84f, 0.0f}, 0.1f, 0.3f, 0.35f},
          {{-0.55f, -1.0f, -0.6f}, {0.04f, -0.84f, 0.0f}, 0.1f, 0.3f, 0.35f},
          {{0.0f, -1.35f, -0.75f}, {0.04f, -0.86f, 0.0f}, 0.1f, 0.3f, 0.35f}},
         {{{0.35f, -0.95f, -0.5f}, {0.04f, -0.88f, 0.0f}, 0.07f, 0.25f, 0.25f},
          {{-0.4f, -0.95f, -0.45f}, {0.04f, -0.88f, 0.0f}, 0.07f, 0.25f, 0.25f}},
         {{{0.05f, -0.93f, -0.38f}, {0.04f, -0.91f, 0.0f}, 0.05f, 0.2f, 0.08f},
          {{0.28f, -0.7f, -0.42f}, {0.04f, -0.9f, 0.0f}, 0.05f, 0.2f, 0.15f}},
     }},
     {{{0.04f, -0.91f, 0.00f}, 0.09f}, {{0.04f, -0.32f, 0.00f}, 0.10f}}},

    // --- Chapter 6: Pat --------------------------------------------------
    // Floor y = 0, ceiling y = -1.69 -- a low room, so even the "high"
    // station in each set only gets to y = -1.5. Pat is sat at the -x end
    // facing +x, head at (-1.19, -1.06, -0.01), so the camera works down
    // the room towards him from +x. Also ~0.65x life size.
    {"assets/scenes/rooms/PatsRoom.sdf",
     /*world_scale=*/4.3f, // head radius 0.07
     /*figure=*/{},
     {{
         {{{1.23f, -1.05f, -0.59f}, {-1.08f, -0.82f, 0.0f}, 0.1f, 0.22f, 0.7f},
          {{1.18f, -1.05f, 0.6f}, {-1.08f, -0.82f, 0.0f}, 0.1f, 0.27f, 0.7f},
          {{0.87f, -1.49f, 0.02f}, {-1.08f, -0.88f, 0.0f}, 0.08f, 0.12f, 0.7f}},
         {{{0.55f, -1.05f, -0.55f}, {-1.12f, -0.88f, 0.0f}, 0.12f, 0.4f, 0.5f},
          {{0.5f, -1.05f, 0.55f}, {-1.12f, -0.88f, 0.0f}, 0.12f, 0.4f, 0.5f},
          {{0.34f, -0.31f, 0.0f}, {-1.12f, -0.94f, 0.0f}, 0.1f, 0.34f, 0.5f}},
         {{{0.1f, -1.05f, -0.45f}, {-1.15f, -0.94f, 0.0f}, 0.1f, 0.3f, 0.35f},
          {{0.05f, -1.05f, 0.45f}, {-1.15f, -0.94f, 0.0f}, 0.1f, 0.3f, 0.35f},
          {{0.0f, -1.4f, 0.0f}, {-1.15f, -0.97f, 0.0f}, 0.1f, 0.3f, 0.35f}},
         {{{-0.35f, -1.05f, -0.35f}, {-1.17f, -1.0f, 0.0f}, 0.07f, 0.25f, 0.25f},
          {{-0.4f, -1.05f, 0.35f}, {-1.17f, -1.0f, 0.0f}, 0.07f, 0.25f, 0.25f}},
         {{{-0.62f, -1.04f, 0.0f}, {-1.19f, -1.05f, -0.01f}, 0.05f, 0.2f, 0.15f},
          {{-0.55f, -0.72f, 0.15f}, {-1.19f, -1.03f, -0.01f}, 0.05f, 0.2f, 0.15f}},
     }},
     {{{-1.19f, -1.05f, -0.01f}, 0.10f}, {{-1.22f, -0.55f, 0.00f}, 0.09f}}},

    // --- Chapter 7: Theresa ----------------------------------------------
    // Floor y = -0.43, ceiling y = -2.04. Theresa is on the chair at
    // (-0.04, ~-1.0, 0.05) facing -z, head at (-0.04, -1.44, 0.06). The
    // desk fills x > 0.5, z[-1.1, -0.1], so every station stays left of
    // x = 0.45 to keep out of it.
    {"assets/scenes/rooms/TheresasRoom.sdf",
     /*world_scale=*/3.3f, // head radius 0.09
     /*figure=*/{},
     {{
         {{{0.39f, -1.44f, -1.81f}, {-0.04f, -1.12f, 0.05f}, 0.1f, 0.07f, 0.8f},
          {{-0.84f, -1.45f, -1.68f}, {-0.04f, -1.12f, 0.05f}, 0.1f, 0.23f, 0.8f},
          {{-0.15f, -1.86f, -1.9f}, {-0.04f, -1.18f, 0.05f}, 0.08f, 0.03f, 0.8f}},
         {{{0.36f, -1.42f, -1.35f}, {-0.04f, -1.18f, 0.05f}, 0.12f, 0.5f, 0.6f},
          {{-0.75f, -1.42f, -1.25f}, {-0.04f, -1.18f, 0.05f}, 0.12f, 0.42f, 0.6f},
          {{-0.1f, -0.71f, -1.44f}, {-0.04f, -1.24f, 0.05f}, 0.1f, 0.32f, 0.5f}},
         {{{0.3f, -1.4f, -0.95f}, {-0.04f, -1.26f, 0.05f}, 0.1f, 0.35f, 0.4f},
          {{-0.62f, -1.4f, -0.88f}, {-0.04f, -1.26f, 0.05f}, 0.1f, 0.35f, 0.4f},
          {{-0.05f, -1.75f, -1.04f}, {-0.04f, -1.3f, 0.05f}, 0.1f, 0.33f, 0.4f}},
         {{{0.24f, -1.4f, -0.62f}, {-0.04f, -1.34f, 0.05f}, 0.07f, 0.25f, 0.25f},
          {{-0.48f, -1.4f, -0.58f}, {-0.04f, -1.34f, 0.05f}, 0.07f, 0.25f, 0.25f}},
         {{{-0.04f, -1.42f, -0.45f}, {-0.04f, -1.42f, 0.06f}, 0.05f, 0.2f, 0.15f},
          {{0.2f, -1.05f, -0.5f}, {-0.04f, -1.4f, 0.06f}, 0.05f, 0.2f, 0.15f}},
     }},
     {{{-0.04f, -1.42f, 0.06f}, 0.12f}, {{-0.05f, -0.80f, 0.06f}, 0.11f}}},

    // --- Chapter 8: the photographer (PhotoStudio) -----------------------
    // The studio again, but shot straight down its own lens axis: he has
    // turned the camera around and sat down in front of it, so the
    // stations are centred rather than favouring a side, and Room_05 is a
    // single locked frame -- there is nobody left to cut away to.
    {"assets/scenes/rooms/PhotoStudio.sdf",
     /*world_scale=*/4.5f, // kStudioFigure's head lands at radius 0.297
     kStudioFigure,
     {{
         {{{0.0f, -1.54f, -1.39f}, {0.0f, -0.93f, 0.7f}, 0.12f, 0.18f, 1.4f},
          {{-2.59f, -1.5f, -0.6f}, {0.0f, -0.93f, 0.7f}, 0.1f, 0.36f, 1.6f},
          {{2.59f, -1.5f, -0.6f}, {0.0f, -0.93f, 0.7f}, 0.1f, 0.38f, 1.6f}},
         {{{0.0f, -1.55f, -1.1f}, {0.0f, -1.03f, 0.7f}, 0.1f, 0.47f, 1.1f},
          {{-1.9f, -1.48f, -0.2f}, {0.0f, -1.03f, 0.7f}, 0.1f, 0.9f, 1.2f},
          {{1.9f, -1.48f, -0.2f}, {0.0f, -1.03f, 0.7f}, 0.1f, 0.9f, 1.2f}},
         {{{0.0f, -1.58f, -0.6f}, {0.0f, -1.12f, 0.7f}, 0.08f, 0.7f, 0.6f},
          {{-1.2f, -1.58f, -0.3f}, {0.0f, -1.12f, 0.7f}, 0.08f, 0.7f, 0.86f},
          {{1.2f, -1.58f, -0.3f}, {0.0f, -1.12f, 0.7f}, 0.08f, 0.7f, 0.86f}},
         {{{0.0f, -1.6f, -0.2f}, {0.0f, -1.21f, 0.7f}, 0.06f, 0.5f, 0.22f},
          {{-0.7f, -1.55f, -0.1f}, {0.0f, -1.21f, 0.7f}, 0.06f, 0.5f, 0.39f}},
         {{{0.0f, -1.62f, 0.1f}, {0.0f, -1.27f, 0.7f}, 0.04f, 0.35f, 0.05f}},
     }},
     kStudioCensorPoints},
}};

// kStaging is indexed by SceneState's own numbering rather than by a
// parallel table of enumerator names, so these pin the one property that
// makes that legal: the enum is exactly eight consecutive blocks of five,
// in kChapters' order. Adding a chapter means appending a block to
// SceneState *and* an entry to kStaging -- if the two ever drift, this is
// what fails, at compile time, instead of chapter N quietly staging
// chapter N+1's room.
static_assert(static_cast<size_t>(SceneState::TheoRoom1) == 0);
static_assert(static_cast<size_t>(SceneState::ThiagoRoom1) == 1 * kRoomsPerChapter);
static_assert(static_cast<size_t>(SceneState::AlexRoom1) == 2 * kRoomsPerChapter);
static_assert(static_cast<size_t>(SceneState::DiegoRoom1) == 3 * kRoomsPerChapter);
static_assert(static_cast<size_t>(SceneState::MelRoom1) == 4 * kRoomsPerChapter);
static_assert(static_cast<size_t>(SceneState::PatRoom1) == 5 * kRoomsPerChapter);
static_assert(static_cast<size_t>(SceneState::TheresaRoom1) == 6 * kRoomsPerChapter);
static_assert(static_cast<size_t>(SceneState::PhotographerRoom1) ==
              7 * kRoomsPerChapter);
static_assert(static_cast<size_t>(SceneState::PhotographerRoom5) + 1 ==
              kStaging.size() * kRoomsPerChapter);

size_t chapter_of(SceneState state) {
  return static_cast<size_t>(state) / kRoomsPerChapter;
}

// 0 for a chapter's Room_01 state, 4 for its Room_05.
size_t room_of(SceneState state) {
  return static_cast<size_t>(state) % kRoomsPerChapter;
}

// Whether two chapters put the exact same set of scenes in the world, so a
// transition between them has nothing to load. Compares the figure as well
// as the room: chapters 2, 3 and 8 all play in the PhotoStudio, and a
// future one could share a room while standing a different person in it.
// Loads one chapter's room (and its figure, if it has one), appending the
// handles to out.
void load_staging(size_t chapter, std::vector<SceneHandle> &out) {
  const ChapterStaging &staging = kStaging[chapter];
  // Loaded untransformed apart from the scale: each room .sdf is authored
  // in world space already, lights included.
  out.push_back(
      renderer_load_scene(staging.scene_path).scale(staging.world_scale));
  if (!staging.figure.scene_path.empty()) {
    // scale() before translate(): both are about the world origin, so
    // scaling afterwards would scale the placement too -- see SceneRef.
    // Both factors carry the room's world_scale, since the figure is
    // authored against the room's own coordinates like everything else.
    out.push_back(renderer_load_scene(staging.figure.scene_path)
                      .scale(staging.figure.scale * staging.world_scale)
                      .translate(staging.figure.translate * staging.world_scale));
  }
}

bool same_staging(const ChapterStaging &a, const ChapterStaging &b) {
  return a.scene_path == b.scene_path && a.world_scale == b.world_scale &&
         a.figure.scene_path == b.figure.scene_path &&
         a.figure.scale == b.figure.scale &&
         a.figure.translate == b.figure.translate;
}

} // namespace

std::vector<CensorPoint> censor_points_for(SceneState state) {
  const ChapterStaging &staging = kStaging[chapter_of(state)];
  std::vector<CensorPoint> scaled = staging.censor_points;
  for (CensorPoint &point : scaled) {
    point.world_position *= staging.world_scale;
    point.world_half_size *= staging.world_scale;
  }
  return scaled;
}

void SHGame::apply_scene_state(SceneState state, size_t first_station) {
  if (current_state_ == state && current_station_ == first_station) {
    return; // already there -- e.g. re-selecting a dialogue option that
            // maps to the state already active
  }

  const ChapterStaging &staging = kStaging[chapter_of(state)];

  // Within one chapter all five states are the same room from different
  // stations, so the common transition -- the interview moving from
  // Room_02 to Room_03 -- is a camera change and nothing else. Reloading
  // the .sdf for it would cost a full device-idle re-bake to produce a
  // byte-identical scene, so only the transitions that actually change
  // what's loaded -- a different room or a differently-placed figure, i.e.
  // starting a chapter, plus the fallbacks in register_scene_states()
  // after an intertitle has cleared everything -- tear anything down.
  // current_state_ being nullopt means the scene was cleared out from
  // under us (show_intertitle(), setup_menus()'s title backdrop), so that
  // always counts as a change.
  const bool same_scenery =
      current_state_.has_value() &&
      same_staging(kStaging[chapter_of(*current_state_)], staging);
  if (!same_scenery) {
    for (SceneHandle handle : loaded_scenes_) {
      renderer_remove_scene(handle);
    }
    loaded_scenes_.clear();
    renderer_enable_sky_box(kSkyBox);
    load_staging(chapter_of(state), loaded_scenes_);
  }

  // Rotated so first_station opens the shot -- set_poses() always starts
  // on the list's first entry, and Tab carries on round the rest in their
  // authored order from there.
  const std::vector<Station> &stations = staging.stations[room_of(state)];
  first_station %= stations.size();
  std::vector<CameraPose> poses;
  poses.reserve(stations.size());
  for (size_t i = 0; i < stations.size(); ++i) {
    poses.push_back(pose_from(
        stations[(first_station + i) % stations.size()], staging.world_scale));
  }
  cameras_.set_poses(std::move(poses));

  current_state_ = state;
  current_station_ = first_station;
}

namespace {

// Who has a room to be interviewed in, for a chapter staged from its
// script's speakers (see SHGame::on_script_line()) -- each name is a
// [Name] a .conversation line can use, mapped to the first of the five
// SceneState values that stage that person's room. Alex has no room of
// her own; she gets the PhotoStudio worked from her side of it, the same
// staging her old chapter used.
struct SpeakerRoom {
  std::string_view speaker;
  SceneState room;
};

constexpr std::array<SpeakerRoom, 6> kSpeakerRooms = {{
    {"Mel", SceneState::MelRoom1},
    {"Diego", SceneState::DiegoRoom1},
    {"Theo", SceneState::TheoRoom1},
    {"Pat", SceneState::PatRoom1},
    {"Alex", SceneState::AlexRoom1},
    {"Theresa", SceneState::TheresaRoom1},
}};

// One camera angle in a room's rotation: which of its five states, and
// which station within that state.
struct Angle {
  size_t room = 0;
  size_t station = 0;
};

// Every station a room has across its five states, ordered so each visit
// lands somewhere new: round r takes station r of Room_01, then of
// Room_02, ... Room_05, so consecutive visits walk in from wide to close
// and each new round comes back out from a different side. Wraps once
// every station has been used -- the rooms have 12 or 13 each, and a
// chapter of Chapter 1's length returns to each person about 18 times.
std::vector<Angle> angle_rotation(const ChapterStaging &staging) {
  std::vector<Angle> rotation;
  size_t widest = 0;
  for (const std::vector<Station> &stations : staging.stations) {
    widest = std::max(widest, stations.size());
  }
  for (size_t round = 0; round < widest; ++round) {
    for (size_t room = 0; room < kRoomsPerChapter; ++room) {
      if (round < staging.stations[room].size()) {
        rotation.push_back({room, round});
      }
    }
  }
  return rotation;
}

} // namespace

void SHGame::cut_to_speaker(const std::string &speaker) {
  auto it = std::find_if(
      kSpeakerRooms.begin(), kSpeakerRooms.end(),
      [&](const SpeakerRoom &entry) { return entry.speaker == speaker; });
  if (it == kSpeakerRooms.end()) {
    return; // the photographer, or nobody with a room -- see the comment
  }

  const size_t chapter = chapter_of(it->room);
  const std::vector<Angle> rotation = angle_rotation(kStaging[chapter]);
  const Angle angle = rotation[visits_[speaker]++ % rotation.size()];
  apply_scene_state(
      static_cast<SceneState>(chapter * kRoomsPerChapter + angle.room),
      angle.station);

  shot_ = Shot{Shot::Kind::Room, speaker, {}};
  cut_cover_ = true;
  cut_cover_frames_ = 0;
  cut_cover_seconds_ = 0.0f;
}

namespace {
// Frames each room stays loaded on the Preparing screen: one for the load
// to reach the renderer (scene changes are applied at the start of the
// next frame) and one for the pre-warm it arms to run there.
constexpr size_t kPrepareFramesPerRoom = 2;
} // namespace

void SHGame::prepare_speaker_rooms(std::function<void()> on_finish) {
  prepare_chapters_.clear();
  for (const SpeakerRoom &entry : kSpeakerRooms) {
    prepare_chapters_.push_back(chapter_of(entry.room));
  }
  prepare_frame_ = 0;
  prepare_on_finish_ = std::move(on_finish);
  screen_ = AppScreen::Preparing;
}

void SHGame::update_preparing() {
  const size_t room = prepare_frame_ / kPrepareFramesPerRoom;
  if (room >= prepare_chapters_.size()) {
    // Every room has been through once. Clear the last one out -- the
    // chapter stages its own opening room next -- and hand over.
    for (SceneHandle handle : loaded_scenes_) {
      renderer_remove_scene(handle);
    }
    loaded_scenes_.clear();
    current_state_.reset();
    rooms_prepared_ = true;
    std::function<void()> on_finish = std::move(prepare_on_finish_);
    prepare_on_finish_ = nullptr;
    if (on_finish) {
      on_finish();
    }
    return;
  }
  if (prepare_frame_ % kPrepareFramesPerRoom == 0) {
    for (SceneHandle handle : loaded_scenes_) {
      renderer_remove_scene(handle);
    }
    loaded_scenes_.clear();
    load_staging(prepare_chapters_[room], loaded_scenes_);
    renderer_request_cache_prewarm();
  }
  ++prepare_frame_;
}

void SHGame::render_preparing() const {
  // Whatever room is being warmed is loaded behind this -- it isn't meant
  // to be seen, so the whole frame is covered.
  const f32 width = static_cast<f32>(width_);
  const f32 height = static_cast<f32>(height_);
  renderer_draw_backdrop_quad(glm::vec2(0.0f), glm::vec2(width, height),
                              glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
  const size_t room = std::min(prepare_frame_ / kPrepareFramesPerRoom + 1,
                               prepare_chapters_.size());
  const std::string text = "Preparing scenes " + std::to_string(room) + "/" +
                           std::to_string(prepare_chapters_.size());
  renderer_draw_text(
      text,
      glm::vec2((width - static_cast<f32>(text.size()) * kAverageCharWidth) /
                    2.0f,
                height / 2.0f),
      glm::vec4(0.5f, 0.5f, 0.5f, 1.0f));
}

void SHGame::on_script_line(const ScriptLine &line) {
  // Every [title] card lands with a ding -- the script's "black screen with
  // a ding sound effect when it first appears". This hook fires once per
  // line as it comes up, so a card dings once, not every frame it's shown.
  if (line.title) {
    audio_play_sound("ding");
  }
  if (!speaker_staged_) {
    return;
  }
  const std::string speaker = line.speaker.value_or(std::string());

  if (line.cutaway || line.photo) {
    // No cutaway scene or photo exists yet, and there's no way to draw an
    // image -- so both are a placeholder card naming the asset, drawn by
    // render_playing() over the speaker's room. See Shot.
    shot_ = Shot{line.cutaway ? Shot::Kind::Cutaway : Shot::Kind::Photo,
                 speaker, line.cutaway ? *line.cutaway : *line.photo};
    return;
  }

  // A line by whoever already owns the shot (or by the photographer, who
  // is never on screen) leaves it up -- including a cutaway/photo, which
  // stays until somebody else speaks.
  if (speaker.empty() || speaker == kPhotographerSpeaker ||
      speaker == shot_.owner) {
    return;
  }
  cut_to_speaker(speaker);
}

void SHGame::register_scene_states() {
  // Fallback for the stretch of the chain no tag has covered yet -- see
  // QASystem::set_base_scene_state()'s doc comment. set_on_returned_to_root
  // is a second, independent trigger: it fires once, specifically when a
  // nested follow-up branch is fully explored and the view pops all the
  // way back up to the top-level list. Both go through
  // current_chapter_base_state() (game_menus.cpp) rather than a single
  // hardcoded SceneState, so each chapter falls back to *its own* Room1
  // state no matter which chapter is actually playing.
  //
  // Neither applies to a speaker-staged chapter (see speaker_staged_):
  // there the shot follows whoever is talking, line by line, through
  // set_on_line_shown() below instead.
  qa_.set_base_scene_state([this] {
    if (!speaker_staged_) {
      apply_scene_state(current_chapter_base_state());
    }
  });
  qa_.set_on_returned_to_root([this] {
    if (!speaker_staged_) {
      apply_scene_state(current_chapter_base_state());
    }
  });
  qa_.set_on_line_shown([this](const ScriptLine &line) { on_script_line(line); });

  // Reaching a question marked `ending` (see resources/conversation.h)
  // stops dialogue navigation right where it is -- this is what actually
  // ends the chapter: a short outro beat (reusing the same black
  // click-through screen chapter intros use -- see show_intertitle() in
  // game_menus.cpp), using that specific ending's own ending_lines (see
  // resources/conversation.h's `ending_text=` lines), then straight on to
  // the next chapter -- see finish_chapter() (game_menus.cpp).
  qa_.set_on_ending_reached([this](const QASystem::Entry &entry) {
    finish_chapter(entry.ending_lines);
  });
}

void SHGame::register_chapter_scene_states(size_t chapter_index) {
  // Every ch*.conversation file tags its questions with the same five
  // names ("Room_01".."Room_05") -- see game.h's SceneState comment for
  // why that's fine despite QASystem::register_scene_state() otherwise
  // warning about a name colliding: begin_chapter_playing() always
  // unloads the previous conversation first, which clears every
  // previously-registered tag name (QASystem::clear_scene_states()),
  // before this re-registers the same five names against chapter_index's
  // own block of five SceneState values.
  //
  // Those five are derived from chapter_index rather than listed in a
  // table of their own, for the same reason kStaging is indexed that way
  // (see its static_asserts): SceneState is exactly eight consecutive
  // blocks of five in chapter order, so a second hand-written list of the
  // enumerators would only be one more thing to keep in step.
  for (size_t room = 0; room < kRoomsPerChapter; ++room) {
    const SceneState state = static_cast<SceneState>(
        chapter_index * kRoomsPerChapter + room);
    qa_.register_scene_state(kRoomTags[room],
                             [this, state] { apply_scene_state(state); });
  }
}
