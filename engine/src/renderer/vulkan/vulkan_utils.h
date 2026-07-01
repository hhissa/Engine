#pragma once
#include "vulkan_types.inl"

// Returns the string representation of result.
// get_extended: also include the extended (human-readable) description.
// Defaults to success for unknown result types.
const char *vulkan_result_string(VkResult result, b8 get_extended);

// Indicates if the passed result is a success or an error, as defined by
// the Vulkan spec. Defaults to true for unknown result types.
b8 vulkan_result_is_success(VkResult result);
