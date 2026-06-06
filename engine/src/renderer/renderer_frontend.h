#pragma once

#include "renderer_types.inl"
#include <string_view>

struct static_mesh_data;
class PlatformLayer;
b8 renderer_initialize(const std::string_view, struct PlatformLayer &platform);
void renderer_shutdown();

void renderer_on_resized(u16 width, u16 height);

b8 renderer_draw_frame(struct render_packet *packet);
