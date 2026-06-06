#pragma once

#include "renderer_types.inl"
#include <memory>
class PlatformLayer;

class RendererBackend {
public:
  virtual ~RendererBackend() = default;

  virtual b8 initialize(std::string_view application_name,
                        PlatformLayer &plat_state) = 0;
  virtual void shutdown() = 0;
  virtual void on_resized(u16 width, u16 height) = 0;
  virtual b8 begin_frame(f32 delta_time) = 0;
  virtual b8 end_frame(f32 delta_time) = 0;

  u64 frame_number = 0;
};

std::unique_ptr<RendererBackend>
renderer_backend_create(renderer_backend_type type, PlatformLayer &plat_state);
