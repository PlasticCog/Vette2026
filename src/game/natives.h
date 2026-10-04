#pragma once
// Registry of every original function ported to native C++ (Phase 2), in port order.

#include <span>
#include <string_view>

#include "host/native.h"

namespace vette::game {

std::span<const host::NativeFunction> native_functions();
const host::NativeFunction* find_native(std::string_view name);

} // namespace vette::game
