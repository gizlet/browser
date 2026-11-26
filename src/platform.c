#include <errno.h>
#include <stdarg.h>
#include <fcntl.h>
#include <glib-unix.h>
#include <libevdev/libevdev.h>
#include <libsoup/soup.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <wpe/drm/wpe-drm.h>
#include <wpe/headless/wpe-headless.h>
#include <wpe/webkit.h>
#include <wpe/wpe-platform.h>

#define START "gizlet://start/"
#define WORLD "gizlet"
#define SEARCH "https://www.bing.com/search?q="
#define MOBILE_USER_AGENT "Mozilla/5.0 (iPhone; CPU iPhone OS 18_6 like Mac OS X) " \
    "AppleWebKit/605.1.15 (KHTML, like Gecko) Version/26.0 Mobile/15E148 Safari/604.1"
#define TICK_MS 16
#define SMOKE_RETRY_MS 250
#define SMOKE_TRIES 16
#define REPEAT_DELAY_MS 300
#define REPEAT_MS 65
#define DPAD_POINTER_STEP 8
#define SCROLL_RAMP_SECONDS 0.35
#define TOUCHPAD_SCALE 2.5
#define MEMORY_LIMIT_MB 420
#define LOAD_PROGRESS_HOLD_US (400 * G_TIME_SPAN_MILLISECOND)

enum Button {
    BUTTON_NONE, BUTTON_UP, BUTTON_DOWN, BUTTON_LEFT, BUTTON_RIGHT, BUTTON_A, BUTTON_B, BUTTON_X,
    BUTTON_Y, BUTTON_L1, BUTTON_R1, BUTTON_L2, BUTTON_R2, BUTTON_SELECT, BUTTON_START
};

static const char *const button_names[] = {
    NULL, "up", "down", "left", "right", "a", "b", "x", "y", "l1", "r1", "l2", "r2", "select", "start"
};

extern int browser_button(uint16_t code, bool swap);

typedef struct Browser Browser;

typedef struct {
    Browser *browser;
    struct libevdev *device;
    guint source;
    uint16_t hat[2];
} Pad;

struct Browser {
    GMainLoop *loop;
    WebKitWebView *web;
    WPEView *view;
    const char *page;
    GPtrArray *pads;
    double stick[4];
    double pointer_x;
    double pointer_y;
    gint64 scroll_started_at;
    gboolean loading;
    gint64 load_finished_at;
    guint tick;
    guint repeat;
    uint16_t repeat_code;
    enum Button repeat_button;
    gboolean keyboard;
    gboolean swap;
    gboolean chinese;
    gboolean smoke;
    guint smoke_port;
    guint smoke_step;
    guint smoke_tries;
    guint smoke_timer;
    guint smoke_response_timer;
    SoupServerMessage *smoke_delayed_response;
    int status;
};

static void sync_view(Browser *browser);

static gboolean on_start(Browser *browser)
{
    const char *uri = webkit_web_view_get_uri(browser->web);
    return uri && g_str_has_prefix(uri, START);
}

static gboolean is_start_uri(const char *uri)
{
    return g_str_has_prefix(uri, START) && (uri[strlen(START)] == '\0' || uri[strlen(START)] == '?');
}

static gboolean web_uri(const char *uri)
{
    return g_str_has_prefix(uri, "https://") || g_str_has_prefix(uri, "http://");
}

static char *web_address(const char *text)
{
    g_autofree char *clean = g_strstrip(g_strdup(text));
    if (!*clean)
        return NULL;
    if (strstr(clean, "://")) {
        g_autoptr(GUri) uri = g_uri_parse(clean, G_URI_FLAGS_NONE, NULL);
        if (!uri || !web_uri(clean) || !g_uri_get_host(uri) || !*g_uri_get_host(uri) || g_uri_get_userinfo(uri))
            return NULL;
        return g_steal_pointer(&clean);
    }
    if (!strpbrk(clean, " \t\r\n")) {
        g_autofree char *address = g_strconcat("https://", clean, NULL);
        g_autoptr(GUri) uri = g_uri_parse(address, G_URI_FLAGS_NONE, NULL);
        const char *host = uri ? g_uri_get_host(uri) : NULL;
        if (host && *host && !g_uri_get_userinfo(uri) && host[strlen(host) - 1] != '.'
            && (strchr(host, '.') || g_strcmp0(host, "localhost") == 0))
            return g_steal_pointer(&address);
    }
    g_autofree char *query = g_uri_escape_string(clean, NULL, FALSE);
    return g_strconcat(SEARCH, query, NULL);
}

static void overlay(Browser *browser, const char *script)
{
    webkit_web_view_evaluate_javascript(browser->web, script, -1, WORLD, NULL, NULL, NULL, NULL);
}

static void overlay_call(Browser *browser, const char *format, ...) G_GNUC_PRINTF(2, 3);

static void overlay_call(Browser *browser, const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    g_autofree char *call = g_strdup_vprintf(format, arguments);
    va_end(arguments);
    g_autofree char *script = g_strconcat("globalThis.gizlet && gizlet.", call, NULL);
    overlay(browser, script);
}

static void sync_load_progress(Browser *browser)
{
    int percent = -1;
    if (browser->loading)
        percent = CLAMP((int)(100 * webkit_web_view_get_estimated_load_progress(browser->web)), 0, 99);
    else if (browser->load_finished_at && g_get_monotonic_time() - browser->load_finished_at < LOAD_PROGRESS_HOLD_US)
        percent = 100;
    overlay_call(browser, "progress(%d)", percent);
}

static void serve_start(WebKitURISchemeRequest *request, gpointer data)
{
    Browser *browser = data;
    if (is_start_uri(webkit_uri_scheme_request_get_uri(request))) {
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
    (void)browser;
    if (type == WEBKIT_POLICY_DECISION_TYPE_RESPONSE) {
        if (!webkit_response_policy_decision_is_mime_type_supported(WEBKIT_RESPONSE_POLICY_DECISION(decision))) {
            webkit_policy_decision_ignore(decision);
            return TRUE;
        }
        return FALSE;
    }
    WebKitNavigationAction *action = webkit_navigation_policy_decision_get_navigation_action(
        WEBKIT_NAVIGATION_POLICY_DECISION(decision));
    const char *uri = webkit_uri_request_get_uri(webkit_navigation_action_get_request(action));
    if (type == WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION) {
        g_autofree char *destination = web_uri(uri) && webkit_navigation_action_is_user_gesture(action) ? g_strdup(uri) : NULL;
        webkit_policy_decision_ignore(decision);
        if (destination)
            webkit_web_view_load_uri(view, destination);
        return TRUE;
    }
    WebKitNavigationType kind = webkit_navigation_action_get_navigation_type(action);
    gboolean from_browser = kind == WEBKIT_NAVIGATION_TYPE_OTHER || kind == WEBKIT_NAVIGATION_TYPE_BACK_FORWARD
        || kind == WEBKIT_NAVIGATION_TYPE_RELOAD;
    if (web_uri(uri) || (is_start_uri(uri) && from_browser))
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

static void show_problem(Browser *browser, const char *uri, const char *english, const char *chinese)
{
    g_autofree char *message = g_markup_escape_text(browser->chinese ? chinese : english, -1);
    const char *hint = browser->chinese ? "按 X 输入网址，按 Y 重新加载，按 B 返回。" : "Press X for an address, Y to reload or B to go back.";
    g_autofree char *html = g_strdup_printf(
        "<meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
        "<body style='margin:0;background:#181522;color:#faf7ff;font:18px/1.4 system-ui,\"Noto Sans CJK SC\",sans-serif;padding:28px'>"
        "<h1 style='font-size:24px;margin:0 0 10px'>%s</h1><p style='color:#cdbde6;margin:0'>%s</p>", message, hint);
    webkit_web_view_load_alternate_html(browser->web, html, uri, NULL);
}

static gboolean load_failed(WebKitWebView *view, WebKitLoadEvent event, const char *uri, GError *error, Browser *browser)
{
    (void)view;
    (void)event;
    browser->loading = FALSE;
    browser->load_finished_at = 0;
    sync_load_progress(browser);
    if (g_error_matches(error, WEBKIT_NETWORK_ERROR, WEBKIT_NETWORK_ERROR_CANCELLED)
        || g_error_matches(error, WEBKIT_POLICY_ERROR, WEBKIT_POLICY_ERROR_FRAME_LOAD_INTERRUPTED_BY_POLICY_CHANGE))
        return TRUE;
    g_printerr("Cannot load %s: %s\n", uri, error->message);
    if (browser->smoke || is_start_uri(uri)) {
        browser->status = 1;
        g_main_loop_quit(browser->loop);
    } else {
        show_problem(browser, uri, "This page could not be opened", "无法打开此网页");
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
    g_printerr("WebKit web process stopped (%d)\n", reason);
    browser->loading = FALSE;
    browser->load_finished_at = 0;
    if (browser->smoke) {
        browser->status = 1;
        g_main_loop_quit(browser->loop);
        return;
    }
    const char *uri = webkit_web_view_get_uri(view);
    if (reason == WEBKIT_WEB_PROCESS_EXCEEDED_MEMORY_LIMIT)
        show_problem(browser, uri, "This page needed more memory than the handheld has", "此网页需要的内存超过了掌机的容量");
    else
        show_problem(browser, uri, "This page stopped working", "此网页已停止运行");
}

static guint32 now_ms(void)
{
    return (guint32)(g_get_monotonic_time() / 1000);
}

static void dispatch(Browser *browser, WPEEvent *event)
{
    wpe_view_event(browser->view, event);
    wpe_event_unref(event);
}

static void key(Browser *browser, guint symbol, guint scan, WPEModifiers modifiers)
{
    if (!browser->view)
        return;
    guint32 tick = now_ms();
    dispatch(browser, wpe_event_keyboard_new(WPE_EVENT_KEYBOARD_KEY_DOWN, browser->view,
        WPE_INPUT_SOURCE_KEYBOARD, tick, modifiers, scan + 8, symbol));
    dispatch(browser, wpe_event_keyboard_new(WPE_EVENT_KEYBOARD_KEY_UP, browser->view,
        WPE_INPUT_SOURCE_KEYBOARD, tick, modifiers, scan + 8, symbol));
}

static void type_text(Browser *browser, const char *text)
{
    for (const char *at = text; *at; at = g_utf8_next_char(at)) {
        gunichar character = g_utf8_get_char_validated(at, -1);
        if (character == (gunichar)-1 || character == (gunichar)-2)
            return;
        key(browser, character < 0x100 ? character : 0x01000000 | character, 0, 0);
    }
}

static gboolean position_pointer(Browser *browser, double dx, double dy)
{
    if (!browser->view)
        return FALSE;
    int width = wpe_view_get_width(browser->view);
    int height = wpe_view_get_height(browser->view);
    if (width <= 0 || height <= 0)
        return FALSE;
    if (browser->pointer_x < 0) {
        browser->pointer_x = width / 2.0;
        browser->pointer_y = height / 2.0;
    }
    browser->pointer_x = CLAMP(browser->pointer_x + dx, 0, width - 1);
    browser->pointer_y = CLAMP(browser->pointer_y + dy, 0, height - 1);
    char x[G_ASCII_DTOSTR_BUF_SIZE], y[G_ASCII_DTOSTR_BUF_SIZE];
    overlay_call(browser, "setPointer(%s, %s)",
        g_ascii_dtostr(x, sizeof(x), browser->pointer_x / width),
        g_ascii_dtostr(y, sizeof(y), browser->pointer_y / height));
    return TRUE;
}

static void move_pointer(Browser *browser, double dx, double dy)
{
    double old_x = browser->pointer_x, old_y = browser->pointer_y;
    if (!position_pointer(browser, dx, dy))
        return;
    double x = browser->pointer_x, y = browser->pointer_y;
    dispatch(browser, wpe_event_pointer_move_new(WPE_EVENT_POINTER_MOVE, browser->view, WPE_INPUT_SOURCE_MOUSE,
        now_ms(), 0, x, y, old_x < 0 ? dx : x - old_x, old_y < 0 ? dy : y - old_y));
}

static void click(Browser *browser)
{
    if (!position_pointer(browser, 0, 0))
        return;
    guint32 tick = now_ms();
    dispatch(browser, wpe_event_pointer_button_new(WPE_EVENT_POINTER_DOWN, browser->view, WPE_INPUT_SOURCE_MOUSE,
        tick, 0, 1, browser->pointer_x, browser->pointer_y, 1));
    dispatch(browser, wpe_event_pointer_button_new(WPE_EVENT_POINTER_UP, browser->view, WPE_INPUT_SOURCE_MOUSE,
        tick, WPE_MODIFIER_POINTER_BUTTON1, 1, browser->pointer_x, browser->pointer_y, 0));
}

static void scroll(Browser *browser, double dx, double dy, gboolean stop)
{
    if (!browser->view)
        return;
    double x = browser->pointer_x >= 0 ? browser->pointer_x : wpe_view_get_width(browser->view) / 2.0;
    double y = browser->pointer_y >= 0 ? browser->pointer_y : wpe_view_get_height(browser->view) / 2.0;
    dispatch(browser, wpe_event_scroll_new(browser->view, WPE_INPUT_SOURCE_TOUCHPAD, now_ms(), 0,
        -dx / TOUCHPAD_SCALE, -dy / TOUCHPAD_SCALE, TRUE, stop, x, y));
}

static void scroll_stick(Browser *browser, double horizontal, double vertical, double seconds, double held_seconds)
{
    double dx = (horizontal > 0) - (horizontal < 0);
    double dy = (vertical > 0) - (vertical < 0);
    double speed = 480 + 1020 * MIN(1, held_seconds / SCROLL_RAMP_SECONDS);
    double distance = speed * seconds / hypot(dx, dy);
    scroll(browser, dx * distance, dy * distance, FALSE);
}

static void set_zoom(Browser *browser, double level)
{
    webkit_web_view_set_zoom_level(browser->web, level);
    g_autofree char *value = g_strdup_printf("%.3f", level);
    for (char *at = value; *at; ++at)
        if (*at == ',')
            *at = '.';
    overlay_call(browser, "setZoom(%s)", value);
}

static void zoom(Browser *browser, int step)
{
    static const double levels[] = { 0.5, 0.67, 0.8, 0.9, 1.0, 1.1, 1.25, 1.5, 1.75, 2.0 };
    double current = webkit_web_view_get_zoom_level(browser->web);
    guint index = 4;
    for (guint candidate = 0; candidate < G_N_ELEMENTS(levels); ++candidate)
        if (fabs(levels[candidate] - current) < fabs(levels[index] - current))
            index = candidate;
    if (step)
        index = (guint)CLAMP((int)index + step, 0, (int)G_N_ELEMENTS(levels) - 1);
    else
        index = 4;
    set_zoom(browser, levels[index]);
    overlay_call(browser, "toast('%d%%')", (int)lround(levels[index] * 100));
}

static void go_back(Browser *browser)
{
    if (webkit_web_view_can_go_back(browser->web))
        webkit_web_view_go_back(browser->web);
    else if (!on_start(browser))
        webkit_web_view_load_uri(browser->web, START);
}

static void open_address_screen(Browser *browser)
{
    if (on_start(browser))
        return;
    const char *uri = webkit_web_view_get_uri(browser->web);
    g_autofree char *current = uri && web_uri(uri) ? g_uri_escape_string(uri, NULL, FALSE) : NULL;
    g_autofree char *target = current ? g_strconcat(START "?u=", current, NULL) : g_strdup(START);
    webkit_web_view_load_uri(browser->web, target);
}

static void press(Browser *browser, enum Button button)
{
    sync_view(browser);
    if (button == BUTTON_NONE)
        return;
    if (browser->keyboard) {
        overlay_call(browser, "button('%s')", button_names[button]);
        return;
    }
    switch (button) {
    case BUTTON_UP: move_pointer(browser, 0, -DPAD_POINTER_STEP); break;
    case BUTTON_DOWN: move_pointer(browser, 0, DPAD_POINTER_STEP); break;
    case BUTTON_LEFT: move_pointer(browser, -DPAD_POINTER_STEP, 0); break;
    case BUTTON_RIGHT: move_pointer(browser, DPAD_POINTER_STEP, 0); break;
    case BUTTON_A: click(browser); break;
    case BUTTON_B: go_back(browser); break;
    case BUTTON_X: open_address_screen(browser); break;
    case BUTTON_Y: webkit_web_view_reload(browser->web); break;
    case BUTTON_L1: key(browser, WPE_KEY_Page_Up, KEY_PAGEUP, 0); break;
    case BUTTON_R1: key(browser, WPE_KEY_Page_Down, KEY_PAGEDOWN, 0); break;
    case BUTTON_L2: zoom(browser, -1); break;
    case BUTTON_R2: zoom(browser, 1); break;
    case BUTTON_SELECT: zoom(browser, 0); break;
    case BUTTON_START: webkit_web_view_go_forward(browser->web); break;
    case BUTTON_NONE: break;
    }
}

static gboolean repeats(Browser *browser, enum Button button)
{
    switch (button) {
    case BUTTON_UP: case BUTTON_DOWN: case BUTTON_LEFT: case BUTTON_RIGHT:
        return TRUE;
    case BUTTON_B: case BUTTON_L1: case BUTTON_R1:
        return browser->keyboard;
    default:
        return FALSE;
    }
}

static void stop_repeat(Browser *browser)
{
    g_clear_handle_id(&browser->repeat, g_source_remove);
    browser->repeat_code = 0;
}

static gboolean repeat_again(gpointer data)
{
    Browser *browser = data;
    press(browser, browser->repeat_button);
    return G_SOURCE_CONTINUE;
}

static gboolean repeat_begin(gpointer data)
{
    Browser *browser = data;
    press(browser, browser->repeat_button);
    browser->repeat = g_timeout_add(REPEAT_MS, repeat_again, browser);
    return G_SOURCE_REMOVE;
}

static void button_event(Browser *browser, uint16_t code, int value)
{
    enum Button button = browser_button(code, browser->swap);
    if (button == BUTTON_NONE)
        return;
    if (value == 1) {
        stop_repeat(browser);
        press(browser, button);
        if (repeats(browser, button)) {
            browser->repeat_code = code;
            browser->repeat_button = button;
            browser->repeat = g_timeout_add(REPEAT_DELAY_MS, repeat_begin, browser);
        }
    } else if (value == 0 && code == browser->repeat_code) {
        stop_repeat(browser);
    }
}

static gboolean tick(gpointer data)
{
    Browser *browser = data;
    double seconds = TICK_MS / 1000.0;
    gboolean active = FALSE;
    double lx = browser->stick[0], ly = browser->stick[1];
    double rx = browser->stick[2], ry = browser->stick[3];
    double left = hypot(lx, ly);
    if (left > 0 && !browser->keyboard) {
        double speed = 70 + 830 * pow(MIN(1, left), 2.2);
        move_pointer(browser, lx / left * speed * seconds, ly / left * speed * seconds);
        active = TRUE;
    }
    if ((rx || ry) && !browser->keyboard) {
        gint64 now = g_get_monotonic_time();
        if (!browser->scroll_started_at)
            browser->scroll_started_at = now;
        double held_seconds = (now - browser->scroll_started_at) / (double)G_USEC_PER_SEC;
        scroll_stick(browser, rx, ry, seconds, held_seconds);
        active = TRUE;
    } else if (browser->scroll_started_at) {
        scroll(browser, 0, 0, TRUE);
        browser->scroll_started_at = 0;
    }
    if (active)
        return G_SOURCE_CONTINUE;
    browser->tick = 0;
    return G_SOURCE_REMOVE;
}

static double axis_value(struct libevdev *device, unsigned code, int value)
{
    const struct input_absinfo *info = libevdev_get_abs_info(device, code);
    if (!info || info->maximum <= info->minimum)
        return 0;
    double middle = (info->maximum + info->minimum) / 2.0;
    double half = (info->maximum - info->minimum) / 2.0;
    double dead = MAX(info->flat, half * 0.18);
    double offset = value - middle;
    if (fabs(offset) <= dead)
        return 0;
    return copysign(MIN(1.0, (fabs(offset) - dead) / (half - dead)), offset);
}

static void axis_event(Pad *pad, uint16_t code, int value)
{
    Browser *browser = pad->browser;
    if (code == ABS_HAT0X || code == ABS_HAT0Y) {
        guint index = code == ABS_HAT0Y;
        if (pad->hat[index])
            button_event(browser, pad->hat[index], 0);
        pad->hat[index] = value ? (uint16_t)(index ? (value < 0 ? 0x220 : 0x221) : (value < 0 ? 0x222 : 0x223)) : 0;
        if (pad->hat[index])
            button_event(browser, pad->hat[index], 1);
        return;
    }
    int slot = code == ABS_X ? 0 : code == ABS_Y ? 1 : code == ABS_RX ? 2 : code == ABS_RY ? 3 : -1;
    if (slot < 0)
        return;
    browser->stick[slot] = axis_value(pad->device, code, value);
    if (!browser->tick && (browser->stick[0] || browser->stick[1] || browser->stick[2] || browser->stick[3]))
        browser->tick = g_timeout_add(TICK_MS, tick, browser);
}

static void pad_lost(Pad *pad)
{
    Browser *browser = pad->browser;
    pad->source = 0;
    stop_repeat(browser);
    memset(pad->hat, 0, sizeof(pad->hat));
    memset(browser->stick, 0, sizeof(browser->stick));
}

static gboolean pad_ready(int fd, GIOCondition condition, gpointer data)
{
    (void)fd;
    Pad *pad = data;
    if (condition & (G_IO_ERR | G_IO_HUP | G_IO_NVAL)) {
        pad_lost(pad);
        return G_SOURCE_REMOVE;
    }
    unsigned flags = LIBEVDEV_READ_FLAG_NORMAL;
    struct input_event event;
    int status;
    while ((status = libevdev_next_event(pad->device, flags, &event)) >= 0) {
        if (status == LIBEVDEV_READ_STATUS_SYNC)
            flags = LIBEVDEV_READ_FLAG_SYNC;
        if (event.type == EV_KEY)
            button_event(pad->browser, event.code, event.value);
        else if (event.type == EV_ABS)
            axis_event(pad, event.code, event.value);
    }
    if (status != -EAGAIN) {
        pad_lost(pad);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void close_pad(gpointer data)
{
    Pad *pad = data;
    if (pad->source)
        g_source_remove(pad->source);
    close(libevdev_get_fd(pad->device));
    libevdev_free(pad->device);
    g_free(pad);
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
        struct libevdev *device = NULL;
        if (libevdev_new_from_fd(fd, &device) != 0) {
            close(fd);
            continue;
        }
        if (!libevdev_has_event_code(device, EV_KEY, BTN_SOUTH)) {
            libevdev_free(device);
            close(fd);
            continue;
        }
        Pad *pad = g_new0(Pad, 1);
        pad->browser = browser;
        pad->device = device;
        pad->source = g_unix_fd_add(fd, G_IO_IN | G_IO_ERR | G_IO_HUP, pad_ready, pad);
        g_ptr_array_add(browser->pads, pad);
    }
    g_dir_close(directory);
}

static void script_message(WebKitUserContentManager *manager, JSCValue *value, Browser *browser)
{
    (void)manager;
    g_autofree char *message = jsc_value_to_string(value);
    if (!message)
        return;
    if (g_str_has_prefix(message, "open:") && on_start(browser)) {
        g_autofree char *address = web_address(message + strlen("open:"));
        if (address)
            webkit_web_view_load_uri(browser->web, address);
        else
            overlay_call(browser, "say('invalid')");
    } else if (g_str_equal(message, "cancel") && on_start(browser)) {
        if (webkit_web_view_can_go_back(browser->web))
            webkit_web_view_go_back(browser->web);
    } else if (g_str_equal(message, "submit")) {
        key(browser, WPE_KEY_Return, KEY_ENTER, 0);
    } else if (g_str_has_prefix(message, "keyboard:")) {
        browser->keyboard = message[strlen("keyboard:")] == '1';
        stop_repeat(browser);
    } else if (g_str_equal(message, "pointer:ready") && !on_start(browser)) {
        position_pointer(browser, 0, 0);
    } else if (g_str_equal(message, "progress:ready")) {
        sync_load_progress(browser);
    } else if (g_str_equal(message, "frame:text-click") && !browser->keyboard) {
        overlay_call(browser, "openKeyboard(true)");
    } else if (g_str_has_prefix(message, "type:") && browser->keyboard) {
        type_text(browser, message + strlen("type:"));
    } else if (g_str_equal(message, "erase") && browser->keyboard) {
        key(browser, WPE_KEY_BackSpace, KEY_BACKSPACE, 0);
    } else if (g_str_has_prefix(message, "caret:") && browser->keyboard) {
        gboolean back = message[strlen("caret:")] == '-';
        key(browser, back ? WPE_KEY_Left : WPE_KEY_Right, back ? KEY_LEFT : KEY_RIGHT, 0);
    }
}

static void load_progress(WebKitWebView *view, GParamSpec *spec, Browser *browser)
{
    (void)view;
    (void)spec;
    if (browser->loading)
        sync_load_progress(browser);
}

static gboolean keyboard_shortcut(WPEView *view, WPEEvent *event, Browser *browser)
{
    (void)view;
    if (wpe_event_get_event_type(event) != WPE_EVENT_KEYBOARD_KEY_DOWN
        || wpe_event_get_input_source(event) != WPE_INPUT_SOURCE_KEYBOARD)
        return FALSE;
    guint symbol = wpe_event_keyboard_get_keyval(event);
    WPEModifiers modifiers = wpe_event_get_modifiers(event);
    if ((modifiers & WPE_MODIFIER_KEYBOARD_CONTROL) && symbol == WPE_KEY_l)
        open_address_screen(browser);
    else if ((modifiers & WPE_MODIFIER_KEYBOARD_ALT) && symbol == WPE_KEY_Left)
        go_back(browser);
    else if ((modifiers & WPE_MODIFIER_KEYBOARD_ALT) && symbol == WPE_KEY_Right)
        webkit_web_view_go_forward(browser->web);
    else if (symbol == WPE_KEY_F5)
        webkit_web_view_reload(browser->web);
    else
        return FALSE;
    return TRUE;
}

static void sync_view(Browser *browser)
{
    WPEView *view = webkit_web_view_get_wpe_view(browser->web);
    if (view == browser->view)
        return;
    if (browser->view)
        g_signal_handlers_disconnect_by_data(browser->view, browser);
    g_set_object(&browser->view, view);
    if (view) {
        g_signal_connect(view, "event", G_CALLBACK(keyboard_shortcut), browser);
        wpe_view_set_visible(view, TRUE);
        wpe_view_focus_in(view);
    }
}

static void load_changed(WebKitWebView *view, WebKitLoadEvent event, Browser *browser)
{
    sync_view(browser);
    if (event == WEBKIT_LOAD_STARTED) {
        browser->loading = TRUE;
        browser->load_finished_at = 0;
        overlay_call(browser, "progress(0)");
    } else if (event == WEBKIT_LOAD_COMMITTED) {
        browser->keyboard = FALSE;
        stop_repeat(browser);
    } else if (event == WEBKIT_LOAD_FINISHED) {
        if (browser->loading) {
            browser->loading = FALSE;
            browser->load_finished_at = g_get_monotonic_time();
        }
        sync_load_progress(browser);
        set_zoom(browser, webkit_web_view_get_zoom_level(view));
        overlay_call(browser, "sync()");
        if (!on_start(browser))
            position_pointer(browser, 0, 0);
    }
}

static const char *const smoke_checks[] = {
    "document.title === 'Gizlet Browser' && gizlet.isOpen() && gizlet.keyCount() > 40 && document.activeElement.id === 'address'",
    "location.pathname === '/first' && document.title === 'Browser smoke test' && "
    "document.body.dataset.mobileUa === 'yes' && "
    "navigator.userAgent.includes('(iPhone;') && navigator.userAgent.includes(' Mobile/') && "
    "gizlet.pointerPosition() !== null",
    "gizlet.pointerPosition()?.every((coordinate, index) => Math.abs(coordinate - [64, 44][index]) < 2)",
    "location.hash === '#b' && gizlet.pointerPosition() !== null",
    "scrollY === 0 && gizlet.pointerPosition()?.every((coordinate, index) => Math.abs(coordinate - [72, 52][index]) < 2)",
    "scrollX === 400 && scrollY === 500 && "
    "gizlet.pointerPosition()?.every((coordinate, index) => Math.abs(coordinate - [64, 44][index]) < 2)",
    "scrollY > 580",
    "Math.abs(scrollY - 500) < 12",
    "scrollX > 480",
    "Math.abs(scrollX - 400) < 12",
    "gizlet.pointerPosition()?.every((coordinate, index) => Math.abs(coordinate - [100, 130][index]) < 2)",
    "document.activeElement.id === 'q' && gizlet.isOpen() && gizlet.pointerPosition() === null",
    "document.getElementById('q').value === 'hi' && !gizlet.isOpen() && gizlet.pointerPosition() !== null",
    "document.activeElement.tagName === 'IFRAME' && gizlet.isOpen() && gizlet.pointerPosition() === null",
    "document.body.dataset.frame === 'ok'",
    "!gizlet.isOpen() && gizlet.pointerPosition() !== null",
    "location.protocol === 'gizlet:' && gizlet.isOpen()",
    "location.protocol === 'gizlet:' && gizlet.loadingProgress() >= 0 && gizlet.loadingProgress() < 100",
    "location.hostname === 'localhost' && location.pathname === '/second'",
    "location.protocol === 'gizlet:' && gizlet.isOpen() && gizlet.loadingProgress() === 100",
    "gizlet.loadingProgress() === -1 && / $/.test(document.getElementById('address').value) && "
    "(globalThis.unzoomed = gizlet.panelHeight()) > 100",
    "gizlet.isOpen() && Math.abs(gizlet.panelHeight() * 2 - unzoomed) < 3",
};

static const char *const smoke_names[] = {
    "address screen and keyboard", "mobile page shows a pointer", "the pointer moves to a link", "A clicks the link",
    "D-pad down and right move the pointer", "D-pad up and left move the pointer",
    "right stick scrolls down", "right stick scrolls up by the same distance",
    "right stick scrolls right", "right stick scrolls left by the same distance", "the pointer moves to a text field",
    "clicking a text field shows the keyboard and hides the pointer", "typing and closing restores the pointer",
    "clicking a cross-origin frame field opens the keyboard", "keyboard types into the frame", "Select closes the keyboard",
    "X opens the address screen", "progress is visible while navigating", "second origin from the address screen",
    "back to the address screen shows completed progress", "progress clears and the keyboard owns the buttons on return",
    "keyboard keeps its size at 200% zoom",
};

static gboolean smoke_navigates(guint step)
{
    return step == 0 || step == 1 || step == 16 || step == 18 || step == 19;
}

static void smoke_result(GObject *source, GAsyncResult *result, gpointer data);

static void smoke_check(Browser *browser)
{
    webkit_web_view_evaluate_javascript(browser->web, smoke_checks[browser->smoke_step], -1, WORLD, NULL, NULL,
        smoke_result, browser);
}

static gboolean smoke_delay(gpointer data)
{
    Browser *browser = data;
    browser->smoke_timer = 0;
    smoke_check(browser);
    return G_SOURCE_REMOVE;
}

static void smoke_act(Browser *browser)
{
    switch (browser->smoke_step) {
    case 1:
        overlay_call(browser, "type('http://127.0.0.1:%u/first'), gizlet.button('start')", browser->smoke_port);
        break;
    case 2: move_pointer(browser, 64 - browser->pointer_x, 44 - browser->pointer_y); break;
    case 3: case 11: press(browser, BUTTON_A); break;
    case 4:
        press(browser, BUTTON_DOWN);
        press(browser, BUTTON_RIGHT);
        break;
    case 5:
        press(browser, BUTTON_UP);
        press(browser, BUTTON_LEFT);
        overlay(browser, "scrollTo(400, 500)");
        break;
    case 6: scroll_stick(browser, 0, 0.45, 0.14, SCROLL_RAMP_SECONDS); break;
    case 7: scroll_stick(browser, 0, -0.9, 0.14, SCROLL_RAMP_SECONDS); break;
    case 8: scroll_stick(browser, 0.45, 0, 0.14, SCROLL_RAMP_SECONDS); break;
    case 9: scroll_stick(browser, -0.9, 0, 0.14, SCROLL_RAMP_SECONDS); break;
    case 10: move_pointer(browser, 100 - browser->pointer_x, 130 - browser->pointer_y); break;
    case 12: overlay_call(browser, "type('hi'), gizlet.button('select')"); break;
    case 13:
        move_pointer(browser, 100 - browser->pointer_x, 230 - browser->pointer_y);
        press(browser, BUTTON_A);
        break;
    case 14: overlay_call(browser, "type('ok')"); break;
    case 15: overlay_call(browser, "button('select')"); break;
    case 16: press(browser, BUTTON_X); break;
    case 17:
        overlay_call(browser, "type('http://localhost:%u/second'), gizlet.button('start')", browser->smoke_port);
        break;
    case 19: press(browser, BUTTON_B); break;
    case 20: press(browser, BUTTON_Y); break;
    case 21: set_zoom(browser, 2.0); break;
    }
    if (!smoke_navigates(browser->smoke_step))
        browser->smoke_timer = g_timeout_add(600, smoke_delay, browser);
}

static void smoke_result(GObject *source, GAsyncResult *result, gpointer data)
{
    Browser *browser = data;
    g_autoptr(GError) error = NULL;
    g_autoptr(JSCValue) value = webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(source), result, &error);
    if (!value || !jsc_value_to_boolean(value) || !browser->view) {
        if (browser->view && ++browser->smoke_tries < SMOKE_TRIES) {
            browser->smoke_timer = g_timeout_add(SMOKE_RETRY_MS, smoke_delay, browser);
            return;
        }
        g_printerr("Smoke test failed at step %u (%s): %s\n", browser->smoke_step, smoke_names[browser->smoke_step],
            error ? error->message : webkit_web_view_get_uri(browser->web));
        g_main_loop_quit(browser->loop);
        return;
    }
    browser->smoke_tries = 0;
    g_print("ok %u %s\n", browser->smoke_step, smoke_names[browser->smoke_step]);
    if (++browser->smoke_step == G_N_ELEMENTS(smoke_checks)) {
        browser->status = 0;
        g_print("WebKit passed the keyboard, pointer, frame, scrolling and navigation progress checks\n");
        g_main_loop_quit(browser->loop);
        return;
    }
    smoke_act(browser);
}

static void smoke_loaded(WebKitWebView *view, WebKitLoadEvent event, Browser *browser)
{
    (void)view;
    if (event == WEBKIT_LOAD_FINISHED && smoke_navigates(browser->smoke_step)) {
        g_clear_handle_id(&browser->smoke_timer, g_source_remove);
        smoke_check(browser);
    }
}

static gboolean smoke_resume_response(gpointer data)
{
    Browser *browser = data;
    g_autoptr(SoupServerMessage) message = g_steal_pointer(&browser->smoke_delayed_response);
    browser->smoke_response_timer = 0;
    soup_server_message_unpause(message);
    return G_SOURCE_REMOVE;
}

static void smoke_response(SoupServer *server, SoupServerMessage *message, const char *path,
                           GHashTable *query, gpointer data)
{
    (void)server;
    (void)query;
    Browser *browser = data;
    const char *agent = soup_message_headers_get_one(soup_server_message_get_request_headers(message), "User-Agent");
    g_autofree char *page = g_str_equal(path, "/frame")
        ? g_strdup("<meta charset=utf-8><body style='margin:0'><input id=f aria-label=frame "
                   "style='box-sizing:border-box;width:100%;height:60px;margin:0'>"
                   "<script>f.addEventListener('input', () => parent.postMessage(f.value, '*'))</script>")
        : g_strdup_printf("<meta charset=utf-8><meta name=viewport content='width=device-width'>"
                          "<title>Browser smoke test</title><body data-mobile-ua='%s'>"
                          "<a href='#b' style='position:fixed;left:24px;top:24px;width:120px;height:40px'>Link</a>"
                          "<input id=q aria-label=query style='position:fixed;left:40px;top:110px;height:40px'>"
                          "<iframe src='http://localhost:%u/frame' tabindex=-1 "
                          "style='position:fixed;left:0;top:200px;width:300px;height:60px;border:0'></iframe>"
                          "<div style='width:1800px;height:4000px'></div>"
                          "<script>addEventListener('message', event => { document.body.dataset.frame = event.data })</script>",
                          g_strcmp0(agent, MOBILE_USER_AGENT) == 0 ? "yes" : "no", browser->smoke_port);
    gsize length = strlen(page);
    soup_server_message_set_status(message, SOUP_STATUS_OK, NULL);
    soup_server_message_set_response(message, "text/html", SOUP_MEMORY_TAKE, g_steal_pointer(&page), length);
    if (g_str_equal(path, "/second") && !browser->smoke_response_timer) {
        soup_server_message_pause(message);
        browser->smoke_delayed_response = g_object_ref(message);
        browser->smoke_response_timer = g_timeout_add(2500, smoke_resume_response, browser);
    }
}

static void configure(WebKitSettings *settings)
{
    webkit_settings_set_enable_media(settings, FALSE);
    webkit_settings_set_enable_webrtc(settings, FALSE);
    webkit_settings_set_enable_webgl(settings, FALSE);
    webkit_settings_set_javascript_can_open_windows_automatically(settings, FALSE);
    webkit_settings_set_enable_spatial_navigation(settings, FALSE);
    webkit_settings_set_enable_page_cache(settings, TRUE);
    webkit_settings_set_user_agent(settings, MOBILE_USER_AGENT);
}

int browser_run(const char *page, const char *overlay_source, const char *address, int smoke)
{
    if (geteuid() == 0) {
        g_printerr("Gizlet Browser must run as a non-root user\n");
        return 1;
    }
    g_setenv("WEBKIT_SKIA_ENABLE_CPU_RENDERING", "1", FALSE);
    g_autoptr(WPEDisplay) display = smoke ? wpe_display_headless_new() : wpe_display_drm_new();
    g_autoptr(GError) display_error = NULL;
    if (!wpe_display_connect(display, &display_error)) {
        g_printerr("WPE cannot open the %s display: %s\n", smoke ? "headless" : "DRM",
            display_error ? display_error->message : "unknown error");
        return 1;
    }
    gboolean chinese = g_str_has_prefix(g_getenv("GIZLET_LANG") ? g_getenv("GIZLET_LANG") : "", "zh");
    g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);
    g_autoptr(WebKitNetworkSession) session = webkit_network_session_new_ephemeral();
    WebKitMemoryPressureSettings *memory = webkit_memory_pressure_settings_new();
    webkit_memory_pressure_settings_set_memory_limit(memory, MEMORY_LIMIT_MB);
    g_autoptr(WebKitWebContext) context = g_object_new(WEBKIT_TYPE_WEB_CONTEXT, "memory-pressure-settings", memory, NULL);
    webkit_memory_pressure_settings_free(memory);
    g_autoptr(WebKitUserContentManager) content = webkit_user_content_manager_new();
    g_autofree char *script_source = g_strconcat("globalThis.GIZLET_LANG = \"", chinese ? "zh-CN" : "en", "\";\n",
        overlay_source, NULL);
    WebKitUserScript *script = webkit_user_script_new_for_world(script_source, WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
        WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, WORLD, NULL, NULL);
    webkit_user_content_manager_add_script(content, script);
    webkit_user_script_unref(script);
    webkit_user_content_manager_register_script_message_handler(content, "gizlet", WORLD);
    g_autoptr(WebKitWebView) web = g_object_new(WEBKIT_TYPE_WEB_VIEW, "display", display, "web-context", context,
        "network-session", session, "user-content-manager", content, NULL);
    g_autoptr(GPtrArray) pads = g_ptr_array_new_with_free_func(close_pad);
    g_autoptr(SoupServer) server = NULL;
    Browser browser = { .loop = loop, .web = web, .page = page, .pads = pads, .smoke = smoke, .status = smoke, .pointer_x = -1,
        .chinese = chinese, .swap = g_strcmp0(g_getenv("GIZLET_SWAP_AB"), "1") == 0 };
    if (smoke) {
        server = soup_server_new(NULL, NULL);
        g_autoptr(GError) error = NULL;
        if (!soup_server_listen_local(server, 0, SOUP_SERVER_LISTEN_IPV4_ONLY, &error)) {
            g_printerr("Cannot start the local smoke test server: %s\n", error->message);
            return 1;
        }
        GSList *uris = soup_server_get_uris(server);
        browser.smoke_port = g_uri_get_port(uris->data);
        g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);
        soup_server_add_handler(server, NULL, smoke_response, &browser, NULL);
    }
    webkit_web_context_register_uri_scheme(context, "gizlet", serve_start, &browser, NULL);
    configure(webkit_web_view_get_settings(web));
    g_signal_connect(content, "script-message-received::gizlet", G_CALLBACK(script_message), &browser);
    g_signal_connect(web, "decide-policy", G_CALLBACK(decide_policy), &browser);
    g_signal_connect(web, "permission-request", G_CALLBACK(deny_permission), &browser);
    g_signal_connect(web, "load-failed", G_CALLBACK(load_failed), &browser);
    g_signal_connect(web, "web-process-terminated", G_CALLBACK(web_process_failed), &browser);
    g_signal_connect(web, "load-changed", G_CALLBACK(load_changed), &browser);
    g_signal_connect(web, "notify::estimated-load-progress", G_CALLBACK(load_progress), &browser);
    sync_view(&browser);
    guint sources[3] = { g_unix_signal_add(SIGTERM, quit_loop, loop), g_unix_signal_add(SIGINT, quit_loop, loop), 0 };
    if (smoke) {
        g_signal_connect(web, "load-changed", G_CALLBACK(smoke_loaded), &browser);
        sources[2] = g_timeout_add_seconds(90, quit_loop, loop);
    } else {
        open_pads(&browser);
    }
    g_autofree char *target = *address ? web_address(address) : NULL;
    webkit_web_view_load_uri(web, target ? target : START);
    g_main_loop_run(loop);
    for (guint index = 0; index < G_N_ELEMENTS(sources); ++index)
        if (sources[index])
            g_source_remove(sources[index]);
    g_clear_handle_id(&browser.tick, g_source_remove);
    g_clear_handle_id(&browser.smoke_timer, g_source_remove);
    if (browser.smoke_response_timer) {
        g_source_remove(browser.smoke_response_timer);
        smoke_resume_response(&browser);
    }
    stop_repeat(&browser);
    g_ptr_array_set_size(pads, 0);
    g_signal_handlers_disconnect_by_data(content, &browser);
    g_signal_handlers_disconnect_by_data(web, &browser);
    if (browser.view)
        g_signal_handlers_disconnect_by_data(browser.view, &browser);
    g_clear_object(&browser.view);
    return browser.status;
}
