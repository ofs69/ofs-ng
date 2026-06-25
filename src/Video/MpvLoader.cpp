#include "MpvLoader.h"
#include "Util/Log.h"
#include <SDL3/SDL.h>

namespace ofs {
static SDL_SharedObject *mpvHandle = nullptr;

mpv_create_FUNC MpvLoader::mpv_create = nullptr;
mpv_initialize_FUNC MpvLoader::mpv_initialize = nullptr;
mpv_wait_event_FUNC MpvLoader::mpv_wait_event = nullptr;
mpv_observe_property_FUNC MpvLoader::mpv_observe_property = nullptr;
mpv_render_context_update_FUNC MpvLoader::mpv_render_context_update = nullptr;
mpv_render_context_render_FUNC MpvLoader::mpv_render_context_render = nullptr;
mpv_set_option_string_FUNC MpvLoader::mpv_set_option_string = nullptr;
mpv_set_property_string_FUNC MpvLoader::mpv_set_property_string = nullptr;
mpv_set_property_FUNC MpvLoader::mpv_set_property = nullptr;
mpv_request_log_messages_FUNC MpvLoader::mpv_request_log_messages = nullptr;
mpv_command_async_FUNC MpvLoader::mpv_command_async = nullptr;
mpv_render_context_create_FUNC MpvLoader::mpv_render_context_create = nullptr;
mpv_set_wakeup_callback_FUNC MpvLoader::mpv_set_wakeup_callback = nullptr;
mpv_render_context_set_update_callback_FUNC MpvLoader::mpv_render_context_set_update_callback = nullptr;
mpv_set_property_async_FUNC MpvLoader::mpv_set_property_async = nullptr;
mpv_render_context_free_FUNC MpvLoader::mpv_render_context_free = nullptr;
mpv_destroy_FUNC MpvLoader::mpv_destroy = nullptr;
mpv_render_context_report_swap_FUNC MpvLoader::mpv_render_context_report_swap = nullptr;
mpv_terminate_destroy_FUNC MpvLoader::mpv_terminate_destroy = nullptr;

#define LOAD_FUNCTION(name)                                                                                            \
    name = (name##_FUNC)SDL_LoadFunction(mpvHandle, #name);                                                            \
    if (!(name)) {                                                                                                     \
        OFS_CORE_ERROR("Failed to load mpv function: {}", #name);                                                      \
        return false;                                                                                                  \
    }

bool MpvLoader::load() noexcept {
    if (mpvHandle)
        return true;

#if defined(WIN32)
    mpvHandle = SDL_LoadObject("libmpv-2.dll");
#elif defined(__APPLE__)
    mpvHandle = SDL_LoadObject("libmpv.dylib");
#else
    mpvHandle = SDL_LoadObject("libmpv.so.2");
    if (!mpvHandle) {
        mpvHandle = SDL_LoadObject("libmpv.so.1");
    }
#endif

    if (!mpvHandle) {
        OFS_CORE_ERROR("Failed to load mpv library: {}", SDL_GetError());
        return false;
    }

    LOAD_FUNCTION(mpv_create);
    LOAD_FUNCTION(mpv_initialize);
    LOAD_FUNCTION(mpv_wait_event);
    LOAD_FUNCTION(mpv_observe_property);
    LOAD_FUNCTION(mpv_render_context_update);
    LOAD_FUNCTION(mpv_render_context_render);
    LOAD_FUNCTION(mpv_set_option_string);
    LOAD_FUNCTION(mpv_set_property_string);
    LOAD_FUNCTION(mpv_set_property);
    LOAD_FUNCTION(mpv_request_log_messages);
    LOAD_FUNCTION(mpv_command_async);
    LOAD_FUNCTION(mpv_render_context_create);
    LOAD_FUNCTION(mpv_set_wakeup_callback);
    LOAD_FUNCTION(mpv_render_context_set_update_callback);
    LOAD_FUNCTION(mpv_set_property_async);
    LOAD_FUNCTION(mpv_render_context_free);
    LOAD_FUNCTION(mpv_destroy);
    LOAD_FUNCTION(mpv_render_context_report_swap);
    LOAD_FUNCTION(mpv_terminate_destroy);

    return true;
}
} // namespace ofs
