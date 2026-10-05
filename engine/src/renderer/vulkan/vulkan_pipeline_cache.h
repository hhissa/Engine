#pragma once
#include "vulkan_types.inl"

// A VkPipelineCache persisted to disk across runs.
//
// Every pipeline this engine creates is a compute or graphics pipeline over
// a large SPIR-V module, and the driver's back-end compiler is what turns
// that into machine code. That compile is not cheap here: measured on a
// Quadro T2000, Builtin.RaymarchVoxelize alone takes ~18s and
// Builtin.ChunkVoxelize longer still, so a cold start spends MINUTES inside
// vkCreateComputePipelines before the first frame -- with no output, which
// reads as a hang rather than as work.
//
// A pipeline cache is the API's answer: hand it to every
// vkCreate*Pipelines() call, and the driver stores its compiled result in
// it; save the blob at shutdown and reload it next launch and the same
// pipeline comes back without recompiling. The first run still pays in
// full. Every run after it, for the same driver and the same shaders,
// is effectively free.
//
// Enabled by default (there is no correctness risk -- see below), at
// $KENGINE_PIPELINE_CACHE if set, otherwise
// $XDG_CACHE_HOME/kengine/pipeline_cache.bin (~/.cache/... when that isn't
// set). KENGINE_NO_PIPELINE_CACHE disables the whole mechanism, including
// the save.
//
// Safety: a cache blob is only ever a HINT. The driver revalidates it and
// silently ignores anything it cannot use, and this loader additionally
// refuses a blob whose header doesn't match this physical device (vendor,
// device, and the driver's own pipelineCacheUUID, which changes with every
// driver update) -- so a stale or foreign file costs one cold rebuild, not
// a wrong pipeline. Shader edits need no invalidation at all: the cache is
// keyed by the SPIR-V that goes in, so a changed shader simply misses.
void vulkan_pipeline_cache_create(VulkanContext &context);

// Writes the cache back out (unless disabled) and destroys the object.
// Called before the logical device it belongs to is destroyed.
void vulkan_pipeline_cache_destroy(VulkanContext &context);
