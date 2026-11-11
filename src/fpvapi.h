// libfpv.dll, loaded at runtime (on the first F7 or F8) so a missing or broken
// DLL never stops the game, or the rest of the mod, from starting.
#pragma once

#include <libfpv.h>

namespace fpvapi {

// The mod ships the libfpv it was built with, so everything here is required.
#define FPV_FUNCTIONS(X)                                                                          \
    X(fpv_global_config_defaults) X(fpv_global_init) X(fpv_last_error) X(fpv_result_string)       \
    X(fpv_version_string) X(fpv_world_set_raycast_callback) X(fpv_material_set)                   \
    X(fpv_drone_desc_defaults) X(fpv_drone_create) X(fpv_drone_delete) X(fpv_drone_reset)         \
    X(fpv_drone_repair) X(fpv_drone_tick) X(fpv_drone_damage_props) X(fpv_drone_set_pilot_position) \
    X(fpv_drone_push) X(fpv_cargo_defaults) X(fpv_drone_set_cargo) X(fpv_portals_set)             \
    X(fpv_input_key) X(fpv_input_get_status) X(fpv_input_set_channels) X(fpv_input_clear_channels)                            \
    X(fpv_controls_set) X(fpv_control_poll)                                                       \
    X(fpv_texture_get) X(fpv_overlay_is_open) X(fpv_overlay_wants_input) X(fpv_overlay_set_display) \
    X(fpv_overlay_mouse_move) X(fpv_overlay_mouse_button) X(fpv_overlay_mouse_wheel)              \
    X(fpv_overlay_render) X(fpv_overlay_set_action) X(fpv_overlay_poll_action) X(fpv_osd_draw_data) \
    X(fpv_audio_configure) X(fpv_audio_read) X(fpv_audio_set_listener) X(fpv_set_view)            \
    X(fpv_gps_set_home) X(fpv_wind_defaults) X(fpv_wind_set) X(fpv_wind_set_sources)              \
    X(fpv_wind_add_blast) X(fpv_shared_data_dir) X(fpv_pilot_set_support) X(fpv_pilot_get_config)

// Local kamikaze builds only (src/kamikaze.h).
#if defined(LIBFPV_KAMIKAZE)
#define FPV_KAMIKAZE_FUNCTIONS(X) \
    X(fpv_kamikaze_poll_detonation) X(fpv_kamikaze_get_config) X(fpv_kamikaze_set_ending_support) \
    X(fpv_kamikaze_set_installed) X(fpv_kamikaze_is_installed) X(fpv_kamikaze_detonate)
#else
#define FPV_KAMIKAZE_FUNCTIONS(X)
#endif

struct Api {
#define FPV_FN(name) decltype(&::name) name = nullptr;
    FPV_FUNCTIONS(FPV_FN)
    FPV_KAMIKAZE_FUNCTIONS(FPV_FN)
#undef FPV_FN
};

extern Api fpv;

// Loads and initialises libfpv once; later calls return the first result.
// A libfpv already running in the game (a core reload) is picked up, or
// replaced if a newer build is on disk (libfpv-live\, see fpvapi.cpp).
bool load(fpv_log_fn log);
bool loaded();
// Some libfpv runs in this process (loaded and initialised by an earlier core).
bool running();

// Portal 2's world: X forward (north), Z up, Y left; inches.
constexpr float kUnitsPerMeter = 39.37f;

}  // namespace fpvapi
