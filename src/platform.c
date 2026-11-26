#include <fcntl.h>
#include <glib-unix.h>
#include <libevdev/libevdev.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <wpe/webkit.h>
#include <wpe/wpe-platform.h>

#define START "gizlet://start/"

enum Action { IGNORE, UP, DOWN, LEFT, RIGHT, FOLLOW, BACK, FORWARD, ADDRESS, RELOAD, DELETE };

extern int browser_button(uint16_t code, bool swap, bool on_start);

typedef struct {
    GMainLoop *loop;
    WebKitWebView *web;
    WPEView *view;
    const char *page;
    GPtrArray *pads;
    gboolean smoke;
    gboolean swap;
    int status;
} Browser;

static gboolean on_start(Browser *browser)
{
    return g_strcmp0(webkit_web_view_get_uri(browser->web), START) == 0;
}

static char *web_address(const char *text)
{
    g_autofree char *clean = g_strdup(text);
    g_strstrip(clean);
    if (!*clean || strpbrk(clean, " \t\r\n"))
        return NULL;
    g_autofree char *address = strstr(clean, "://") ? g_strdup(clean) : g_strconcat("https://", clean, NULL);
    g_autoptr(GUri) uri = g_uri_parse(address, G_URI_FLAGS_NONE, NULL);
    if (!uri || !g_uri_get_host(uri) || !*g_uri_get_host(uri) || g_uri_get_userinfo(uri))
        return NULL;
    const char *scheme = g_uri_get_scheme(uri);
    if (g_strcmp0(scheme, "http") != 0 && g_strcmp0(scheme, "https") != 0)
        return NULL;
    return g_steal_pointer(&address);
}

static void serve_start(WebKitURISchemeRequest *request, gpointer data)
{
    Browser *browser = data;
    const char *uri = webkit_uri_scheme_request_get_uri(request);
    if (g_strcmp0(uri, START) == 0) {
        g_autoptr(GInputStream) stream = g_memory_input_stream_new_from_data(browser->page, -1, NULL);
        webkit_uri_scheme_request_finish(request, stream, strlen(browser->page), "text/html; charset=utf-8");
        return;
    }
    g_autoptr(GError) error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Unknown browser page");
    webkit_uri_scheme_request_finish_error(request, error);
}

static gboolean decide_policy(WebKitWebView *view, WebKitPolicyDecision *decision,
                              WebKitPolicyDecisionType type, Browser *browser)
{
    if (type == WEBKIT_POLICY_DECISION_TYPE_RESPONSE) {
        if (!webkit_response_policy_decision_is_mime_type_supported(WEBKIT_RESPONSE_POLICY_DECISION(decision))) {
            webkit_policy_decision_ignore(decision);
            return TRUE;
        }
        return FALSE;
    }
    if (type != WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION) {
        webkit_policy_decision_ignore(decision);
        return TRUE;
    }
    WebKitNavigationAction *action = webkit_navigation_policy_decision_get_navigation_action(
        WEBKIT_NAVIGATION_POLICY_DECISION(decision));
    const char *uri = webkit_uri_request_get_uri(webkit_navigation_action_get_request(action));
    if (g_str_has_prefix(uri, "gizlet://open/?address=") && on_start(browser)) {
        const char *encoded = uri + strlen("gizlet://open/?address=");
        g_autofree char *decoded = g_uri_unescape_string(encoded, NULL);
        g_autofree char *address = decoded ? web_address(decoded) : NULL;
        webkit_policy_decision_ignore(decision);
        if (address)
            webkit_web_view_load_uri(view, address);
        else
            webkit_web_view_evaluate_javascript(view, "showAddressError()", -1, NULL, NULL, NULL, NULL, NULL);
        return TRUE;
    }
    if (g_strcmp0(uri, START) == 0 || g_str_has_prefix(uri, "https://") || g_str_has_prefix(uri, "http://"))
        return FALSE;
    webkit_policy_decision_ignore(decision);
    return TRUE;
}

static gboolean deny_permission(WebKitWebView *view, WebKitPermissionRequest *request, gpointer data)
{
    (void)view;
    (void)data;
    webkit_permission_request_deny(request);
    return TRUE;
}

static gboolean load_failed(WebKitWebView *view, WebKitLoadEvent event, const char *uri, GError *error, Browser *browser)
{
    (void)event;
    if (g_error_matches(error, WEBKIT_NETWORK_ERROR, WEBKIT_NETWORK_ERROR_CANCELLED))
        return TRUE;
    g_printerr("Cannot load %s: %s\n", uri, error->message);
    if (browser->smoke || g_strcmp0(uri, START) == 0) {
        browser->status = 1;
        g_main_loop_quit(browser->loop);
    } else {
        webkit_web_view_load_alternate_html(view,
            "<meta name=viewport content='width=device-width,initial-scale=1'>"
            "<body style='background:#151725;color:#f5f3ff;font:20px sans-serif;padding:32px'>"
            "<h1>Page unavailable</h1><p>Press X or Ctrl+L to enter an address.</p>",
            uri, NULL);
    }
    return TRUE;
}

static gboolean quit_loop(gpointer data)
{
    g_main_loop_quit(data);
    return G_SOURCE_CONTINUE;
}

static void web_process_failed(WebKitWebView *view, WebKitWebProcessTerminationReason reason, Browser *browser)
{
    (void)view;
    g_printerr("WebKit web process stopped (%d)\n", reason);
    browser->status = 1;
    g_main_loop_quit(browser->loop);
}

static void smoke_result(GObject *source, GAsyncResult *result, gpointer data)
{
    Browser *browser = data;
    g_autoptr(GError) error = NULL;
    g_autoptr(JSCValue) value = webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(source), result, &error);
    if (value && jsc_value_to_boolean(value)) {
        browser->status = 0;
        g_print("WPE rendered and ran the Gizlet address screen\n");
    } else {
        g_printerr("WPE address screen failed: %s\n", error ? error->message : "missing controls");
    }
    g_main_loop_quit(browser->loop);
}

static void smoke_loaded(WebKitWebView *view, WebKitLoadEvent event, Browser *browser)
{
    if (event == WEBKIT_LOAD_FINISHED)
        webkit_web_view_evaluate_javascript(view,
            "document.title === 'Gizlet Browser' && document.querySelectorAll('#keys button').length > 30",
            -1, NULL, NULL, NULL, smoke_result, browser);
}

static void key(Browser *browser, guint symbol, guint scan, WPEModifiers modifiers)
{
    guint32 tick = (guint32)(g_get_monotonic_time() / 1000);
    WPEEvent *press = wpe_event_keyboard_new(WPE_EVENT_KEYBOARD_KEY_DOWN, browser->view,
        WPE_INPUT_SOURCE_KEYBOARD, tick, modifiers, scan + 8, symbol);
    WPEEvent *release = wpe_event_keyboard_new(WPE_EVENT_KEYBOARD_KEY_UP, browser->view,
        WPE_INPUT_SOURCE_KEYBOARD, tick, modifiers, scan + 8, symbol);
    wpe_view_event(browser->view, press);
    wpe_view_event(browser->view, release);
    wpe_event_unref(press);
    wpe_event_unref(release);
}

static void perform(Browser *browser, enum Action action)
{
    switch (action) {
    case UP: key(browser, WPE_KEY_Up, KEY_UP, 0); break;
    case DOWN: key(browser, WPE_KEY_Down, KEY_DOWN, 0); break;
    case LEFT:
        if (on_start(browser))
            key(browser, WPE_KEY_Left, KEY_LEFT, 0);
        else
            key(browser, WPE_KEY_ISO_Left_Tab, KEY_TAB, WPE_MODIFIER_KEYBOARD_SHIFT);
        break;
    case RIGHT:
        if (on_start(browser))
            key(browser, WPE_KEY_Right, KEY_RIGHT, 0);
        else
            key(browser, WPE_KEY_Tab, KEY_TAB, 0);
        break;
    case FOLLOW: key(browser, WPE_KEY_Return, KEY_ENTER, 0); break;
    case DELETE: key(browser, WPE_KEY_BackSpace, KEY_BACKSPACE, 0); break;
    case BACK:
        if (webkit_web_view_can_go_back(browser->web))
            webkit_web_view_go_back(browser->web);
        else
            webkit_web_view_load_uri(browser->web, START);
        break;
    case FORWARD: webkit_web_view_go_forward(browser->web); break;
    case ADDRESS: webkit_web_view_load_uri(browser->web, START); break;
    case RELOAD: webkit_web_view_reload(browser->web); break;
    case IGNORE: break;
    }
}

static gboolean keyboard_shortcut(WPEView *view, WPEEvent *event, Browser *browser)
{
    (void)view;
    if (wpe_event_get_event_type(event) != WPE_EVENT_KEYBOARD_KEY_DOWN)
        return FALSE;
    guint symbol = wpe_event_keyboard_get_keyval(event);
    WPEModifiers modifiers = wpe_event_get_modifiers(event);
    if ((modifiers & WPE_MODIFIER_KEYBOARD_CONTROL) && symbol == WPE_KEY_l)
        perform(browser, ADDRESS);
    else if ((modifiers & WPE_MODIFIER_KEYBOARD_ALT) && symbol == WPE_KEY_Left)
        perform(browser, BACK);
    else if ((modifiers & WPE_MODIFIER_KEYBOARD_ALT) && symbol == WPE_KEY_Right)
        perform(browser, FORWARD);
    else if (symbol == WPE_KEY_F5)
        perform(browser, RELOAD);
    else
        return FALSE;
    return TRUE;
}

static void close_pad(gpointer data)
{
    struct libevdev *pad = data;
    close(libevdev_get_fd(pad));
    libevdev_free(pad);
}

static void open_pads(Browser *browser)
{
    GDir *directory = g_dir_open("/dev/input", 0, NULL);
    if (!directory)
        return;
    const char *name;
    while ((name = g_dir_read_name(directory))) {
        if (!g_str_has_prefix(name, "event"))
            continue;
        g_autofree char *path = g_build_filename("/dev/input", name, NULL);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            continue;
        struct libevdev *pad = NULL;
        if (libevdev_new_from_fd(fd, &pad) != 0) {
            close(fd);
            continue;
        }
        if (libevdev_has_event_code(pad, EV_KEY, BTN_SOUTH))
            g_ptr_array_add(browser->pads, pad);
        else
            close_pad(pad);
    }
    g_dir_close(directory);
}

static gboolean poll_pads(gpointer data)
{
    Browser *browser = data;
    for (guint index = 0; index < browser->pads->len; ++index) {
        struct libevdev *pad = g_ptr_array_index(browser->pads, index);
        struct input_event event;
        for (unsigned count = 0; count < 32; ++count) {
            int status = libevdev_next_event(pad, LIBEVDEV_READ_FLAG_NORMAL, &event);
            if (status < 0)
                break;
            if (status == LIBEVDEV_READ_STATUS_SUCCESS && event.type == EV_KEY && event.value > 0)
                perform(browser, browser_button(event.code, browser->swap, on_start(browser)));
            if (status == LIBEVDEV_READ_STATUS_SUCCESS && event.type == EV_ABS && event.value) {
                if (event.code == ABS_HAT0X)
                    perform(browser, event.value < 0 ? LEFT : RIGHT);
                if (event.code == ABS_HAT0Y)
                    perform(browser, event.value < 0 ? UP : DOWN);
            }
        }
    }
    return G_SOURCE_CONTINUE;
}

int browser_run(const char *page, const char *address, int smoke)
{
    if (geteuid() == 0) {
        g_printerr("Gizlet Browser must run as a non-root user\n");
        return 1;
    }
    g_setenv("WPE_PLATFORM", smoke ? "headless" : "drm", TRUE);
    WPEDisplay *display = wpe_display_get_default();
    if (!display) {
        g_printerr("WPE cannot open the %s display\n", smoke ? "headless" : "DRM");
        return 1;
    }
    g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);
    g_autoptr(WebKitNetworkSession) session = webkit_network_session_new_ephemeral();
    g_autoptr(WebKitWebContext) context = webkit_web_context_new();
    g_autoptr(WebKitWebView) web = g_object_new(WEBKIT_TYPE_WEB_VIEW,
        "display", display, "web-context", context, "network-session", session, NULL);
    g_autoptr(GPtrArray) pads = g_ptr_array_new_with_free_func(close_pad);
    Browser browser = { .loop = loop, .web = web, .view = webkit_web_view_get_wpe_view(web),
        .page = page, .pads = pads, .smoke = smoke, .status = smoke,
        .swap = g_strcmp0(g_getenv("GIZLET_SWAP_AB"), "1") == 0 };
    if (!browser.view)
        return 1;
    webkit_web_context_register_uri_scheme(context, "gizlet", serve_start, &browser, NULL);
    WebKitSettings *settings = webkit_web_view_get_settings(web);
    webkit_settings_set_enable_media(settings, FALSE);
    webkit_settings_set_enable_webrtc(settings, FALSE);
    webkit_settings_set_enable_webgl(settings, FALSE);
    webkit_settings_set_javascript_can_open_windows_automatically(settings, FALSE);
    g_signal_connect(web, "decide-policy", G_CALLBACK(decide_policy), &browser);
    g_signal_connect(web, "permission-request", G_CALLBACK(deny_permission), &browser);
    g_signal_connect(web, "load-failed", G_CALLBACK(load_failed), &browser);
    g_signal_connect(web, "web-process-terminated", G_CALLBACK(web_process_failed), &browser);
    g_signal_connect(browser.view, "event", G_CALLBACK(keyboard_shortcut), &browser);
    wpe_view_set_visible(browser.view, TRUE);
    wpe_view_focus_in(browser.view);
    guint sources[3] = { g_unix_signal_add(SIGTERM, quit_loop, loop), g_unix_signal_add(SIGINT, quit_loop, loop), 0 };
    if (smoke) {
        g_signal_connect(web, "load-changed", G_CALLBACK(smoke_loaded), &browser);
        sources[2] = g_timeout_add_seconds(20, quit_loop, loop);
    } else {
        open_pads(&browser);
        if (pads->len > 0)
            sources[2] = g_timeout_add(30, poll_pads, &browser);
    }
    g_autofree char *target = web_address(address);
    webkit_web_view_load_uri(web, target ? target : START);
    g_main_loop_run(loop);
    for (guint index = 0; index < G_N_ELEMENTS(sources); ++index)
        if (sources[index])
            g_source_remove(sources[index]);
    g_signal_handlers_disconnect_by_data(web, &browser);
    g_signal_handlers_disconnect_by_data(browser.view, &browser);
    return browser.status;
}
