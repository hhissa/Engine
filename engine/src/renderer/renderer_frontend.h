#pragma once

#include "camera.h"
#include "renderer_types.inl"
#include <string_view>

struct static_mesh_data;
class PlatformLayer;
b8 renderer_initialize(const std::string_view, struct PlatformLayer &platform);
void renderer_shutdown();

void renderer_on_resized(u16 width, u16 height);

// Sets the camera used for the next frame's render. The game owns and
// drives the Camera (position/orientation from input); the renderer just
// forwards whatever it's given to the backend.
void renderer_set_camera(const Camera &camera);

b8 renderer_draw_frame(struct render_packet *packet);
