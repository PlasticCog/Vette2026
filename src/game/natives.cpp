#include "game/natives.h"

#include "game/math3d.h"
#include "game/projection.h"

namespace vette::game {
namespace {

// name, image segment, offset, far, ret n, touches VRAM, FLAGS mask, native body
constexpr host::NativeFunction kFunctions[] = {
    {"sincos_deg", 0x3009, 0x4E3C, false, 0, false, 0xFFFF, &sincos_deg},
    {"camera_matrix_from_angles", 0x3009, 0x3F2D, false, 0, false, 0xFFFF, &camera_matrix_from_angles},
    {"vec_mul_mat3", 0x3009, 0x3D51, false, 0, false, 0xFFFF, &vec_mul_mat3},
    {"points_rel_camera", 0x3009, 0x3D8C, false, 0, false, 0xFFFF, &points_rel_camera},
    {"xform_points_to_camera", 0x3009, 0x3D2F, false, 0, false, 0xFFFF, &xform_points_to_camera},
    {"world_to_camera_point", 0x3009, 0x3917, false, 0, false, 0xFFFF, &world_to_camera_point},
    {"build_axis_table", 0x3009, 0x39B9, false, 0, false, 0xFFFF, &build_axis_table},
    {"project_vertices", 0x3009, 0xA685, false, 0, false, 0xFFFF, &project_vertices},
};

} // namespace

std::span<const host::NativeFunction> native_functions() { return kFunctions; }

const host::NativeFunction* find_native(std::string_view name) {
    for (const auto& fn : kFunctions) {
        if (name == fn.name) {
            return &fn;
        }
    }
    return nullptr;
}

} // namespace vette::game
