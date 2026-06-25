#pragma once

#include <mpv/client.h>
#include <mpv/render_gl.h>

namespace ofs {
typedef mpv_handle *(*mpv_create_FUNC)();

typedef int (*mpv_initialize_FUNC)(mpv_handle *ctx);

typedef mpv_event *(*mpv_wait_event_FUNC)(mpv_handle *ctx, double timeout);

typedef int (*mpv_observe_property_FUNC)(mpv_handle *mpv, uint64_t reply_userdata, const char *name, mpv_format format);

typedef uint64_t (*mpv_render_context_update_FUNC)(mpv_render_context *ctx);

typedef int (*mpv_render_context_render_FUNC)(mpv_render_context *ctx, mpv_render_param *params);

typedef int (*mpv_set_option_string_FUNC)(mpv_handle *ctx, const char *name, const char *data);

typedef int (*mpv_set_property_string_FUNC)(mpv_handle *ctx, const char *name, const char *data);

typedef int (*mpv_set_property_FUNC)(mpv_handle *ctx, const char *name, mpv_format format, void *data);

typedef int (*mpv_request_log_messages_FUNC)(mpv_handle *ctx, const char *min_level);

typedef int (*mpv_command_async_FUNC)(mpv_handle *ctx, uint64_t reply_userdata, const char **args);

typedef int (*mpv_render_context_create_FUNC)(mpv_render_context **res, mpv_handle *mpv, mpv_render_param *params);

typedef void (*mpv_set_wakeup_callback_FUNC)(mpv_handle *ctx, void (*cb)(void *d), void *d);

typedef void (*mpv_render_context_set_update_callback_FUNC)(mpv_render_context *ctx, mpv_render_update_fn callback,
                                                            void *callback_ctx);

typedef int (*mpv_set_property_async_FUNC)(mpv_handle *ctx, uint64_t reply_userdata, const char *name,
                                           mpv_format format, void *data);

typedef void (*mpv_render_context_free_FUNC)(mpv_render_context *ctx);

typedef void (*mpv_destroy_FUNC)(mpv_handle *ctx);

typedef void (*mpv_render_context_report_swap_FUNC)(mpv_render_context *ctx);

typedef void (*mpv_terminate_destroy_FUNC)(mpv_handle *ctx);

struct MpvLoader {
    static mpv_create_FUNC mpv_create;
    static mpv_initialize_FUNC mpv_initialize;
    static mpv_wait_event_FUNC mpv_wait_event;
    static mpv_observe_property_FUNC mpv_observe_property;
    static mpv_render_context_update_FUNC mpv_render_context_update;
    static mpv_render_context_render_FUNC mpv_render_context_render;
    static mpv_set_option_string_FUNC mpv_set_option_string;
    static mpv_set_property_string_FUNC mpv_set_property_string;
    static mpv_set_property_FUNC mpv_set_property;
    static mpv_request_log_messages_FUNC mpv_request_log_messages;
    static mpv_command_async_FUNC mpv_command_async;
    static mpv_render_context_create_FUNC mpv_render_context_create;
    static mpv_set_wakeup_callback_FUNC mpv_set_wakeup_callback;
    static mpv_render_context_set_update_callback_FUNC mpv_render_context_set_update_callback;
    static mpv_set_property_async_FUNC mpv_set_property_async;
    static mpv_render_context_free_FUNC mpv_render_context_free;
    static mpv_destroy_FUNC mpv_destroy;
    static mpv_render_context_report_swap_FUNC mpv_render_context_report_swap;
    static mpv_terminate_destroy_FUNC mpv_terminate_destroy;

    static bool load() noexcept;
};
} // namespace ofs
