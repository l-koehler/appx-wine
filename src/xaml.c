/* Windows.UI.Xaml / Windows.UI.Core on top of a plain Win32 window */
#include "shim.h"
#include <stdio.h>

HWND main_hwnd;
struct obj *core_window, *xaml_window, *dispatcher, *app_obj, *comp_target;
static int quit;

/* ---- dispatcher queue ---- */

struct work {
    struct work *next;
    IUnknown *handler;
    struct obj *action;
};
static struct work *queue_head, **queue_tail = &queue_head;
static CRITICAL_SECTION queue_cs;

static void run_queue(void)
{
    struct work *w;
    EnterCriticalSection(&queue_cs);
    w = queue_head;
    queue_head = NULL;
    queue_tail = &queue_head;
    LeaveCriticalSection(&queue_cs);
    while (w) {
        struct work *next = w->next;
        struct prop *p;
        {
            static int ran;
            int k = ran++ < 20;
            HRESULT hr;
            if (k) shim_log("run_queue: invoking %p on thread %lu", w->handler, GetCurrentThreadId());
            hr = invoke0(w->handler);
            if (k) shim_log("run_queue: handler %p returned %08lx", w->handler, hr);
        }
        IUnknown_Release(w->handler);
        prop_set_raw(w->action, "Status", &(int){1}, 4);
        if ((p = prop_find(w->action, "Completed")) && p->v.unk)
            invoke2(p->v.unk, obj_default(w->action), (void *)1);
        free(w);
        w = next;
    }
}

static HRESULT WINAPI disp_run_async(struct tear *t, int prio, IUnknown *handler, void **action)
{
    struct work *w = calloc(1, sizeof(*w));
    static int logged;
    if (logged++ < 20) shim_log("Dispatcher.RunAsync prio %d from thread %lu", prio, GetCurrentThreadId());
    w->handler = handler;
    IUnknown_AddRef(handler);
    w->action = obj_new_iface(find_iface("Windows.Foundation.IAsyncAction"));
    prop_set_raw(w->action, "Status", &(int){0}, 4);
    *action = obj_default(w->action);
    IUnknown_AddRef((IUnknown *)*action);
    EnterCriticalSection(&queue_cs);
    *queue_tail = w;
    queue_tail = &w->next;
    LeaveCriticalSection(&queue_cs);
    return S_OK;
}

static HRESULT WINAPI disp_has_access(struct tear *t, boolean *v)
{
    *v = 1;
    return S_OK;
}

static void pump(void)
{
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) quit = 1;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    run_queue();
}

static HRESULT WINAPI disp_process_events(struct tear *t, int option)
{
    /* 0 = ProcessOneAndAllPending, 1 = ProcessOneIfPresent, 2 = ProcessUntilQuit, 3 = ProcessAllIfPresent */
    if (option == 2)
        while (!quit) {
            pump();
            Sleep(1);
        }
    else pump();
    return S_OK;
}

/* ---- window geometry ---- */

static void client_size(float *w, float *h)
{
    RECT r;
    GetClientRect(main_hwnd, &r);
    *w = r.right - r.left;
    *h = r.bottom - r.top;
}

static HRESULT WINAPI cw_get_bounds(struct tear *t, float *rect)
{
    rect[0] = rect[1] = 0;
    client_size(&rect[2], &rect[3]);
    return S_OK;
}

static HRESULT WINAPI true_getter(struct tear *t, boolean *v)
{
    *v = 1;
    return S_OK;
}

/* ICoreWindowInterop */
static const GUID IID_ICoreWindowInterop = {0x45d64a29, 0xa63e, 0x4cb6, {0xb4, 0x98, 0x57, 0x81, 0xd2, 0x98, 0xcb, 0x4f}};
static HRESULT WINAPI cwi_get_hwnd(struct tear *t, HWND *h)
{
    *h = main_hwnd;
    return S_OK;
}
static HRESULT WINAPI cwi_put_msg(struct tear *t, boolean v)
{
    return S_OK;
}
static void *cwi_vtbl[8];

static void fire_element_sizes(void);


static void fire_size_changed(void)
{
    struct obj *args = obj_new_named("Windows.UI.Core.WindowSizeChangedEventArgs");
    float sz[2];
    client_size(&sz[0], &sz[1]);
    prop_set_raw(args, "Size", sz, 8);
    event_fire(core_window, "SizeChanged", obj_iface(core_window, "Windows.UI.Core.ICoreWindow"), obj_default(args));
    event_fire(xaml_window, "SizeChanged", obj_identity(xaml_window), obj_default(args));
    fire_element_sizes();
}

/* ---- element sizes: every element fills the window ---- */

static struct obj *sized[64];
static int nsized;

/* every element covers the whole window at its origin, so transforms between them are the identity */
struct point { float x, y; };
struct rect { float x, y, w, h; };

static HRESULT WINAPI gt_transform_point(struct tear *t, struct point p, void *out)
{
    memcpy(out, &p, sizeof(p));
    return S_OK;
}

static HRESULT WINAPI gt_try_transform(struct tear *t, struct point p, void *out, boolean *ok)
{
    memcpy(out, &p, sizeof(p));
    *ok = 1;
    return S_OK;
}

static HRESULT WINAPI gt_transform_bounds(struct tear *t, struct rect r, void *out)
{
    memcpy(out, &r, sizeof(r));
    return S_OK;
}

static HRESULT WINAPI fe_get_actual_size(struct tear *t, double *v)
{
    float w, h;
    client_size(&w, &h);
    *v = w;
    return S_OK;
}

static HRESULT WINAPI fe_get_actual_height(struct tear *t, double *v)
{
    float w, h;
    client_size(&w, &h);
    *v = h;
    return S_OK;
}

static HRESULT WINAPI fe_add_size_changed(struct tear *t, IUnknown *h, INT64 *token)
{
    event_add(t->obj, "SizeChanged", h, token);
    if (nsized < 64) sized[nsized++] = t->obj;
    return S_OK;
}

static void fire_element_sizes(void)
{
    for (int i = 0; i < nsized; i++) {
        struct obj *args = obj_new_named("Windows.UI.Xaml.SizeChangedEventArgs");
        float sz[2];
        client_size(&sz[0], &sz[1]);
        prop_set_raw(args, "NewSize", sz, 8);
        prop_set_raw(args, "PreviousSize", sz, 8);
        event_fire(sized[i], "SizeChanged", obj_identity(sized[i]), obj_default(args));
    }
}

static void fire_visibility(int visible)
{
    struct obj *args = obj_new_named("Windows.UI.Core.VisibilityChangedEventArgs");
    boolean v = visible;
    prop_set_raw(args, "Visible", &v, 1);
    event_fire(core_window, "VisibilityChanged", obj_iface(core_window, "Windows.UI.Core.ICoreWindow"), obj_default(args));
    event_fire(xaml_window, "VisibilityChanged", obj_identity(xaml_window), obj_default(args));
}

static void fire_activated(int state)
{
    struct obj *args = obj_new_named("Windows.UI.Core.WindowActivatedEventArgs");
    prop_set_raw(args, "WindowActivationState", &state, 4);
    event_fire(core_window, "Activated", obj_iface(core_window, "Windows.UI.Core.ICoreWindow"), obj_default(args));
    event_fire(xaml_window, "Activated", obj_identity(xaml_window), obj_default(args));
}

/* ---- loaded events ---- */

static struct obj *pending_loaded[64];
static int npending_loaded;

static void queue_loaded(struct obj *o)
{
    if (o && npending_loaded < 64) pending_loaded[npending_loaded++] = o;
}

static void fire_loaded(void)
{
    while (npending_loaded) {
        struct obj *o = pending_loaded[--npending_loaded];
        struct obj *args = obj_new_named("Windows.UI.Xaml.RoutedEventArgs");
        for (int i = 0; i < o->nchildren; i++) queue_loaded(o->children[i]);
        event_fire(o, "Loaded", obj_identity(o), obj_default(args));
    }
}

/* ---- IWindow ---- */

static HRESULT WINAPI win_put_content(struct tear *t, IUnknown *content)
{
    struct obj *o;
    void *fe = NULL;
    prop_set_unk(t->obj, "Content", content);
    if (content && SUCCEEDED(IUnknown_QueryInterface(content, &meta_ifaces[find_iface("Windows.UI.Xaml.IFrameworkElement")].iid, &fe)) &&
        (o = obj_from(fe))) {
        shim_log("window content = %s", o->name);
        queue_loaded(o);
    }
    if (fe) IUnknown_Release((IUnknown *)fe);
    return S_OK;
}

static HRESULT WINAPI win_activate(struct tear *t)
{
    shim_log("Window.Activate");
    ShowWindow(main_hwnd, SW_SHOW);
    fire_activated(0);
    fire_visibility(1);
    return S_OK;
}

static HRESULT WINAPI win_statics_current(struct tear *t, void **out)
{
    *out = obj_default(xaml_window);
    IUnknown_AddRef((IUnknown *)*out);
    return S_OK;
}

static HRESULT WINAPI cw_statics_current(struct tear *t, void **out)
{
    *out = obj_iface(core_window, "Windows.UI.Core.ICoreWindow");
    IUnknown_AddRef((IUnknown *)*out);
    return S_OK;
}

/* ---- XAML loading ---- */

static const GUID IID_ISwapChainBackgroundPanelNative = {0x43bebd4e, 0xadd5, 0x4035, {0x8f, 0x85, 0x56, 0x08, 0xd0, 0x8e, 0x9d, 0xc9}};
static const GUID IID_ISwapChainPanelNative = {0xf92f19d2, 0x3ade, 0x45a6, {0xa2, 0x0c, 0xf6, 0xf1, 0xea, 0x90, 0x55, 0x4b}};
static void *scbp_vtbl[4];
static HRESULT WINAPI scbp_set_swapchain(struct tear *t, IUnknown *sc)
{
    shim_log("SwapChainBackgroundPanel.SetSwapChain(%p)", sc);
    if (sc) IUnknown_AddRef(sc);
    t->obj->priv = sc;
    return S_OK;
}

static struct obj *xaml_element(const char *tag)
{
    static const char *nss[] = {"Windows.UI.Xaml.Controls.", "Windows.UI.Xaml.", "Windows.UI.Xaml.Shapes.",
                                "Windows.UI.Xaml.Controls.Primitives.", "Windows.UI.Xaml.Documents."};
    char buf[200];
    const char *colon = strchr(tag, ':');
    if (colon) tag = colon + 1;
    for (int i = 0; i < 5; i++) {
        int c;
        snprintf(buf, sizeof(buf), "%s%s", nss[i], tag);
        if ((c = find_class(buf)) >= 0) {
            struct obj *o = obj_new_class(c);
            if (!strcmp(tag, "SwapChainBackgroundPanel")) obj_add_custom(o, &IID_ISwapChainBackgroundPanelNative, scbp_vtbl);
            return o;
        }
    }
    shim_log("xaml: unknown element %s", tag);
    return obj_new_named("Windows.UI.Xaml.Controls.Grid");
}

static int attr(const char *tag, const char *end, const char *name, char *out, int outlen)
{
    int nl = strlen(name);
    for (const char *p = tag; p + nl + 2 < end; p++) {
        if ((p == tag || p[-1] == ' ' || p[-1] == '\n' || p[-1] == '\t' || p[-1] == '\r') && !strncmp(p, name, nl) &&
            p[nl] == '=' && (p[nl + 1] == '"' || p[nl + 1] == '\'')) {
            const char *s = p + nl + 2, *e = s;
            while (e < end && *e != p[nl + 1]) e++;
            snprintf(out, outlen, "%.*s", (int)(e - s), s);
            return 1;
        }
    }
    return 0;
}

struct media_pending {
    struct obj *o;
    DWORD when;
};
static struct media_pending media[8];
static int nmedia;

/* a MediaElement created in code ends right away, like those from XAML */
static HRESULT WINAPI me_add_media_ended(struct tear *t, IUnknown *h, INT64 *token)
{
    int i;
    event_add(t->obj, "MediaEnded", h, token);
    for (i = 0; i < nmedia && media[i].o; i++) {}
    if (i < 8) {
        media[i].o = t->obj;
        media[i].when = GetTickCount() + 500;
        if (i == nmedia) nmedia++;
    }
    return S_OK;
}

static HRESULT load_xaml(IInspectable *component, const WCHAR *path)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    DWORD size, rd;
    char *buf, *p;
    struct obj *root, *stack[32];
    int depth = 0, seen_root = 0;
    void *fe = NULL, *conn = NULL;
    int connector = find_iface("Windows.UI.Xaml.Markup.IComponentConnector");

    if (f == INVALID_HANDLE_VALUE) {
        shim_log("LoadComponent: cannot open %ls", path);
        return S_OK;
    }
    size = GetFileSize(f, NULL);
    buf = calloc(1, size + 1);
    ReadFile(f, buf, size, &rd, NULL);
    CloseHandle(f);

    IUnknown_QueryInterface((IUnknown *)component, &meta_ifaces[find_iface("Windows.UI.Xaml.IFrameworkElement")].iid, &fe);
    root = obj_from(fe);
    if (!root) {
        IUnknown_QueryInterface((IUnknown *)component, &meta_ifaces[find_iface("Windows.UI.Xaml.IApplication")].iid, &fe);
        root = obj_from(fe);
    }
    if (!root) {
        shim_log("LoadComponent: component is not ours");
        return S_OK;
    }
    IUnknown_QueryInterface((IUnknown *)component, &meta_ifaces[connector].iid, &conn);
    shim_log("LoadComponent %ls into %s (connector %p)", path, root->name, conn);

    for (p = buf; (p = strchr(p, '<'));) {
        char *end = strchr(p, '>'), tag[100], val[100];
        int selfclose, n;
        struct obj *o;
        if (!end) break;
        if (p[1] == '?' || p[1] == '!') {
            p = end;
            continue;
        }
        if (p[1] == '/') {
            if (depth) depth--;
            p = end;
            continue;
        }
        selfclose = end[-1] == '/';
        n = strcspn(p + 1, " \t\r\n/>");
        snprintf(tag, sizeof(tag), "%.*s", n, p + 1);
        if (strchr(tag, '.')) { /* property element like <Grid.RowDefinitions> */
            if (!selfclose) {
                stack[depth] = depth ? stack[depth - 1] : root;
                depth++;
            }
            p = end;
            continue;
        }
        if (!seen_root) {
            o = root;
            seen_root = 1;
        } else {
            o = xaml_element(tag);
            struct obj *parent = depth ? stack[depth - 1] : root;
            if (parent->nchildren < 16) parent->children[parent->nchildren++] = o;
            if (parent == root && !prop_find(root, "Content")) prop_set_unk(root, "Content", obj_default(o));
        }
        if (attr(p, end, "x:Name", val, sizeof(val)) && o != root) {
            struct named *nm = calloc(1, sizeof(*nm));
            MultiByteToWideChar(CP_UTF8, 0, val, -1, nm->name, 64);
            nm->obj = o;
            nm->next = root->names;
            root->names = nm;
            prop_set_str(o, "Name", nm->name);
        }
        if (!strcmp(tag, "MediaElement") && nmedia < 8) {
            media[nmedia].o = o;
            media[nmedia++].when = GetTickCount() + 500;
        }
        if (attr(p, end, "x:ConnectionId", val, sizeof(val)) && conn) {
            HRESULT hr = VCALL(conn, 6, HRESULT(WINAPI *)(void *, int, void *))(conn, atoi(val), obj_default(o));
            shim_log("xaml: Connect(%d, %s) -> %08lx", atoi(val), o->name, hr);
        }
        if (!selfclose && depth < 32) stack[depth++] = o;
        p = end;
    }
    free(buf);
    return S_OK;
}

static HRESULT uri_to_path(IUnknown *uri, WCHAR *path)
{
    HSTRING s = NULL;
    const WCHAR *raw;
    struct obj *u = obj_from(uri);
    struct prop *p = u ? prop_find(u, "RawUri") : NULL;
    if (p) s = p->v.str;
    raw = WindowsGetStringRawBuffer(s, NULL);
    if (!wcsncmp(raw, L"ms-appx:///", 11)) raw += 11;
    swprintf(path, MAX_PATH, L"%ls\\%ls", package.dir, raw);
    for (WCHAR *c = path; *c; c++)
        if (*c == '/') *c = '\\';
    return S_OK;
}

static HRESULT WINAPI app_load_component(struct tear *t, IInspectable *component, IUnknown *uri)
{
    WCHAR path[MAX_PATH];
    uri_to_path(uri, path);
    return load_xaml(component, path);
}

static HRESULT WINAPI app_load_component2(struct tear *t, IInspectable *component, IUnknown *uri, int loc)
{
    return app_load_component(t, component, uri);
}

static HRESULT WINAPI fe_find_name(struct tear *t, HSTRING name, IInspectable **out)
{
    const WCHAR *n = WindowsGetStringRawBuffer(name, NULL);
    for (struct named *nm = t->obj->names; nm; nm = nm->next)
        if (!wcscmp(nm->name, n)) {
            *out = obj_default(nm->obj);
            IUnknown_AddRef((IUnknown *)*out);
            return S_OK;
        }
    shim_log("FindName(%ls) on %s: not found", n, t->obj->name);
    *out = NULL;
    return S_OK;
}

/* Windows.Foundation.Uri */
static HRESULT WINAPI uri_create(struct tear *t, HSTRING s, void **out)
{
    struct obj *o = obj_new_named("Windows.Foundation.Uri");
    struct prop *p;
    const WCHAR *raw = WindowsGetStringRawBuffer(s, NULL);
    prop_set_str(o, "RawUri", raw);
    prop_set_str(o, "AbsoluteUri", raw);
    prop_set_str(o, "DisplayUri", raw);
    if (!wcsncmp(raw, L"ms-appx:///", 11)) prop_set_str(o, "Path", raw + 10);
    shim_log("Uri(%ls)", raw);
    *out = obj_default(o);
    (void)p;
    return S_OK;
}

static HRESULT WINAPI uri_create2(struct tear *t, HSTRING base, HSTRING rel, void **out)
{
    WCHAR buf[1024];
    HSTRING s;
    swprintf(buf, 1024, L"%ls%ls", WindowsGetStringRawBuffer(base, NULL), WindowsGetStringRawBuffer(rel, NULL));
    WindowsCreateString(buf, wcslen(buf), &s);
    return uri_create(t, s, out);
}

/* ---- application ---- */

static void on_compose(struct obj *o)
{
    /* UWP apps create the SwapChainPanel in code; its native interface has the same layout */
    if (!strcmp(o->name, "Windows.UI.Xaml.Controls.SwapChainPanel")) obj_add_custom(o, &IID_ISwapChainPanelNative, scbp_vtbl);
    /* the framework keeps Application.Current alive */
    if (!strcmp(o->name, "Windows.UI.Xaml.Application")) {
        app_obj = o;
        IUnknown_AddRef(o->outer);
    }
}

static HRESULT WINAPI app_statics_current(struct tear *t, void **out)
{
    if (!app_obj) {
        *out = NULL;
        return E_FAIL;
    }
    return IUnknown_QueryInterface(app_obj->outer, &meta_ifaces[find_iface("Windows.UI.Xaml.IApplication")].iid, out);
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
static DWORD autotap;

/* closing the window suspends the app first, like Windows does; games save their state then */
static volatile LONG open_deferrals;

static HRESULT WINAPI suspend_get_deferral(struct tear *t, void **out)
{
    InterlockedIncrement(&open_deferrals);
    *out = obj_default(obj_new_named("Windows.ApplicationModel.SuspendingDeferral"));
    return S_OK;
}

static HRESULT WINAPI suspend_complete(struct tear *t)
{
    if (!t->obj->priv) {
        t->obj->priv = t->obj;
        InterlockedDecrement(&open_deferrals);
    }
    return S_OK;
}

static void suspend_app(void)
{
    struct obj *args = obj_new_named("Windows.ApplicationModel.SuspendingEventArgs");
    struct obj *op = obj_new_named("Windows.ApplicationModel.SuspendingOperation");
    FILETIME ft;
    INT64 deadline;
    DWORD end = GetTickCount() + 5000;
    GetSystemTimeAsFileTime(&ft);
    deadline = ((INT64)ft.dwHighDateTime << 32 | ft.dwLowDateTime) + 5 * 10000000LL;
    prop_set_raw(op, "Deadline", &deadline, 8);
    prop_set_unk(args, "SuspendingOperation", obj_default(op));
    shim_log("suspending");
    event_fire(app_obj, "Suspending", obj_identity(app_obj), obj_default(args));
    /* deferred work may need the dispatcher; Windows allows about 5 seconds */
    while (open_deferrals > 0 && (int)(GetTickCount() - end) < 0) {
        pump();
        Sleep(5);
    }
    shim_log("suspended (%ld deferrals still open)", open_deferrals);
}

/* SHIM_TAPS="<s>:<x>,<y>;...": scripted clicks (seconds after start, client coordinates);
 * "<s>:key=<vk>" presses a virtual key instead */
static struct tap {
    DWORD when;
    int x, y, vk, stage;
} taps[64];
static int ntaps;

static void queue_tap(DWORD when, int x, int y)
{
    if (ntaps < 64) taps[ntaps++] = (struct tap){when, x, y, 0, 0};
}

static void parse_taps(void)
{
    char buf[1024], *p;
    DWORD start = GetTickCount();
    if (!GetEnvironmentVariableA("SHIM_TAPS", buf, sizeof(buf))) return;
    for (p = strtok(buf, ";"); p && ntaps < 64; p = strtok(NULL, ";")) {
        float t;
        int x, y, vk;
        if (sscanf(p, "%f:key=%i", &t, &vk) == 2) taps[ntaps++] = (struct tap){start + t * 1000, 0, 0, vk, 0};
        else if (sscanf(p, "%f:%d,%d", &t, &x, &y) == 3) queue_tap(start + t * 1000, x, y);
    }
}

static void run_taps(DWORD now)
{
    for (int i = 0; i < ntaps; i++) {
        struct tap *t = &taps[i];
        if (t->stage == 2 || (int)(now - t->when) < 0) continue;
        if (t->vk) {
            shim_log("scripted key %d %s", t->vk, t->stage ? "up" : "down");
            PostMessageW(main_hwnd, t->stage ? WM_KEYUP : WM_KEYDOWN, t->vk, t->stage ? 0xc0000001 : 1);
        } else {
            if (!t->stage) shim_log("scripted tap %d,%d", t->x, t->y);
            PostMessageW(main_hwnd, t->stage ? WM_LBUTTONUP : WM_LBUTTONDOWN, t->stage ? 0 : MK_LBUTTON,
                         MAKELPARAM(t->x, t->y));
        }
        t->stage++;
        t->when = now + (t->vk ? 1500 : 150);
    }
}
extern volatile int dump_requested;
static int pointer_type = 0; /* PointerDeviceType: 0 touch, 2 mouse */
static boolean buttons[3];

static void create_window(void)
{
    WNDCLASSW wc = {0};
    WCHAR buf[32];
    int w = 1280, h = 720;
    RECT r;
    if (GetEnvironmentVariableW(L"SHIM_WIDTH", buf, 32)) w = _wtoi(buf);
    if (GetEnvironmentVariableW(L"SHIM_HEIGHT", buf, 32)) h = _wtoi(buf);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = L"AppxWineWindow";
    wc.hbrBackground = GetStockObject(BLACK_BRUSH);
    RegisterClassW(&wc);
    r = (RECT){0, 0, w, h};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    main_hwnd = CreateWindowW(wc.lpszClassName, package.display_name, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                              r.right - r.left, r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
}

static HRESULT WINAPI app_start(struct tear *t, IUnknown *callback)
{
    struct obj *params, *args;
    void *overrides = NULL;
    HRESULT hr;
    int slot;

    shim_log("Application.Start");
    create_window();
    params = obj_new_named("Windows.UI.Xaml.ApplicationInitializationCallbackParams");
    hr = invoke1(callback, obj_default(params));
    shim_log("init callback -> %08lx, app %p", hr, app_obj);
    if (!app_obj) return E_FAIL;

    args = obj_new_named("Windows.ApplicationModel.Activation.LaunchActivatedEventArgs");
    prop_set_str(args, "Arguments", L"");
    prop_set_str(args, "TileId", L"App");
    prop_set_raw(args, "Kind", &(int){0}, 4);
    prop_set_raw(args, "PreviousExecutionState", &(int){0}, 4);
    obj_iface(args, "Windows.ApplicationModel.Activation.IActivatedEventArgs");
    hr = IUnknown_QueryInterface(app_obj->outer, &meta_ifaces[find_iface("Windows.UI.Xaml.IApplicationOverrides")].iid, &overrides);
    slot = method_slot("Windows.UI.Xaml.IApplicationOverrides", "OnLaunched");
    shim_log("IApplicationOverrides %08lx %p slot %d", hr, overrides, slot);
    if (overrides) {
        hr = VCALL(overrides, slot, HRESULT(WINAPI *)(void *, void *))(overrides, obj_iface(args, "Windows.ApplicationModel.Activation.ILaunchActivatedEventArgs"));
        shim_log("OnLaunched -> %08lx", hr);
    }
    fire_size_changed();

    {
        WCHAR buf[16];
        if (GetEnvironmentVariableW(L"SHIM_MOUSE", buf, 16)) pointer_type = 2;
        if (GetEnvironmentVariableW(L"SHIM_AUTOTAP", buf, 16)) autotap = GetTickCount() + _wtoi(buf) * 1000;
        parse_taps();
    }
    while (!quit) {
        DWORD now = GetTickCount();
        /* SHIM_AUTOTAP=<s>: click the window centre after <s> seconds (self-test without a user) */
        if (autotap && (int)(now - autotap) > 0) {
            RECT r;
            GetClientRect(main_hwnd, &r);
            queue_tap(now, r.right / 2, r.bottom / 2);
            autotap = 0;
        }
        run_taps(now);
        pump();
        if (npending_loaded) {
            fire_loaded();
            fire_size_changed();
        }
        for (int i = 0; i < nmedia; i++)
            if (media[i].o && (int)(now - media[i].when) > 0) {
                struct obj *a = obj_new_named("Windows.UI.Xaml.RoutedEventArgs");
                shim_log("skipping intro video: firing MediaEnded");
                event_fire(media[i].o, "MediaEnded", obj_default(media[i].o), obj_default(a));
                media[i].o = NULL;
            }
        if (event_count(comp_target, "Rendering")) {
            struct obj *a = obj_new_named("Windows.UI.Xaml.Media.RenderingEventArgs");
            event_fire(comp_target, "Rendering", NULL, obj_default(a));
            IUnknown_Release((IUnknown *)obj_default(a));
        } else Sleep(5);
    }
    shim_log("main loop exit");
    if (app_obj) {
        suspend_app();
        /* Windows terminates a suspended app that is closed; the game's own shutdown would wait for threads
         * it paused while suspending */
        shim_log("terminating");
        TerminateProcess(GetCurrentProcess(), 0);
    }
    return S_OK;
}

/* ---- input ---- */

/* typed characters go to the TextBox the game focused last (games use hidden ones for text entry) */
static struct obj *focused_textbox;

static HRESULT WINAPI control_focus(struct tear *t, int state, boolean *result)
{
    if (!strcmp(t->obj->name, "Windows.UI.Xaml.Controls.TextBox")) {
        struct obj *a = obj_new_named("Windows.UI.Xaml.RoutedEventArgs");
        shim_log("TextBox focused");
        focused_textbox = t->obj;
        event_fire(t->obj, "GotFocus", obj_identity(t->obj), obj_default(a));
    }
    if (result) *result = 1;
    return S_OK;
}

static int prop_bool(struct obj *o, const char *name, int dflt)
{
    struct prop *p = prop_find(o, name);
    return p ? p->v.raw[0] != 0 : dflt;
}

static void textbox_char(WCHAR c)
{
    struct obj *tb = focused_textbox;
    struct prop *p;
    WCHAR text[256] = L"";
    int len, max;
    if (!tb || prop_bool(tb, "IsReadOnly", 0) || !prop_bool(tb, "IsEnabled", 1)) return;
    if ((p = prop_find(tb, "Text")) && p->v.str) wcsncpy(text, WindowsGetStringRawBuffer(p->v.str, NULL), 255);
    len = wcslen(text);
    max = (p = prop_find(tb, "MaxLength")) ? *(int *)p->v.raw : 0;
    if (c == '\b') {
        if (!len) return;
        text[len - 1] = 0;
    } else if (c >= ' ' && len < 255 && (max <= 0 || len < max)) {
        text[len] = c;
        text[len + 1] = 0;
    } else return;
    prop_set_str(tb, "Text", text);
    event_fire(tb, "TextChanged", obj_identity(tb), obj_default(obj_new_named("Windows.UI.Xaml.Controls.TextChangedEventArgs")));
}



static void fire_pointer(const char *event, int x, int y, int wheel)
{
    struct obj *args = obj_new_named("Windows.UI.Core.PointerEventArgs");
    struct obj *pt = obj_new_named("Windows.UI.Input.PointerPoint");
    struct obj *props = obj_new_named("Windows.UI.Input.PointerPointProperties");
    struct obj *dev = obj_new_named("Windows.Devices.Input.PointerDevice");
    LARGE_INTEGER now, freq;
    float pos[2] = {x, y};
    UINT32 id = 1;
    UINT64 ts;
    boolean contact = buttons[0] || buttons[1] || buttons[2], yes = 1;

    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    ts = now.QuadPart * 1000000 / freq.QuadPart;
    prop_set_raw(dev, "PointerDeviceType", &pointer_type, 4);
    prop_set_raw(dev, "IsIntegrated", &yes, 1);
    prop_set_raw(dev, "MaxContacts", &id, 4);
    prop_set_raw(props, "IsLeftButtonPressed", &buttons[0], 1);
    prop_set_raw(props, "IsRightButtonPressed", &buttons[1], 1);
    prop_set_raw(props, "IsMiddleButtonPressed", &buttons[2], 1);
    prop_set_raw(props, "MouseWheelDelta", &wheel, 4);
    prop_set_raw(props, "IsPrimary", &yes, 1);
    prop_set_raw(props, "IsInRange", &yes, 1);
    prop_set_raw(props, "Pressure", &(float){0.5f}, 4);
    prop_set_unk(pt, "PointerDevice", obj_default(dev));
    prop_set_unk(pt, "Properties", obj_default(props));
    prop_set_raw(pt, "Position", pos, 8);
    prop_set_raw(pt, "RawPosition", pos, 8);
    prop_set_raw(pt, "PointerId", &id, 4);
    prop_set_raw(pt, "Timestamp", &ts, 8);
    prop_set_raw(pt, "IsInContact", &contact, 1);
    prop_set_unk(args, "CurrentPoint", obj_default(pt));
    event_fire(core_window, event, obj_iface(core_window, "Windows.UI.Core.ICoreWindow"), obj_default(args));
}

static void fire_key(const char *event, WPARAM vk, LPARAM lp)
{
    struct obj *args = obj_new_named("Windows.UI.Core.KeyEventArgs");
    /* CorePhysicalKeyStatus */
    struct { UINT32 repeat, scan; boolean extended, menu, was_down, released; } st = {
        LOWORD(lp), (lp >> 16) & 0xff, (lp >> 24) & 1, (lp >> 29) & 1, (lp >> 30) & 1, (lp >> 31) & 1};
    int key = vk;
    prop_set_raw(args, "VirtualKey", &key, 4);
    prop_set_raw(args, "KeyStatus", &st, sizeof(st));
    event_fire(core_window, event, obj_iface(core_window, "Windows.UI.Core.ICoreWindow"), obj_default(args));
}

static HRESULT WINAPI cw_get_key_state(struct tear *t, int vk, int *state)
{
    SHORT k = GetKeyState(vk);
    /* CoreVirtualKeyStates: Down = 1, Locked = 2 */
    *state = (k & 0x8000 ? 1 : 0) | (k & 1 ? 2 : 0);
    return S_OK;
}

/* ---- window procedure ---- */

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SIZE:
        if (core_window) fire_size_changed();
        break;
    case WM_CLOSE:
        quit = 1;
        return 0;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
        buttons[msg == WM_LBUTTONDOWN ? 0 : msg == WM_RBUTTONDOWN ? 1 : 2] = 1;
        SetCapture(hwnd);
        fire_pointer("PointerPressed", (short)LOWORD(lp), (short)HIWORD(lp), 0);
        return 0;
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    case WM_MBUTTONUP:
        buttons[msg == WM_LBUTTONUP ? 0 : msg == WM_RBUTTONUP ? 1 : 2] = 0;
        ReleaseCapture();
        fire_pointer("PointerReleased", (short)LOWORD(lp), (short)HIWORD(lp), 0);
        return 0;
    case WM_MOUSEMOVE:
        if (pointer_type == 2 || buttons[0]) fire_pointer("PointerMoved", (short)LOWORD(lp), (short)HIWORD(lp), 0);
        return 0;
    case WM_MOUSEWHEEL: {
        POINT p = {(short)LOWORD(lp), (short)HIWORD(lp)};
        ScreenToClient(hwnd, &p);
        fire_pointer("PointerWheelChanged", p.x, p.y, (short)HIWORD(wp));
        return 0;
    }
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (wp == VK_F12) {
            dump_requested = 1;
            return 0;
        }
        fire_key("KeyDown", wp, lp);
        if (msg == WM_SYSKEYDOWN && wp == VK_F4) break;
        return 0;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        fire_key("KeyUp", wp, lp);
        return 0;
    case WM_CHAR:
        textbox_char(wp);
        return 0;
    case WM_ACTIVATE:
        if (core_window) fire_activated(LOWORD(wp) == WA_INACTIVE ? 2 : 0);
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void xaml_init(void)
{
    InitializeCriticalSection(&queue_cs);
    compose_hook = on_compose;

    core_window = obj_new_named("Windows.UI.Core.CoreWindow");
    xaml_window = obj_new_named("Windows.UI.Xaml.Window");
    dispatcher = obj_new_named("Windows.UI.Core.CoreDispatcher");
    comp_target = factory_for(find_class("Windows.UI.Xaml.Media.CompositionTarget"));
    memcpy(cwi_vtbl, *(void ***)obj_default(core_window), 6 * sizeof(void *));
    cwi_vtbl[6] = cwi_get_hwnd;
    cwi_vtbl[7] = cwi_put_msg;
    obj_add_custom(core_window, &IID_ICoreWindowInterop, cwi_vtbl);
    memcpy(scbp_vtbl, *(void ***)obj_default(core_window), 3 * sizeof(void *));
    scbp_vtbl[3] = scbp_set_swapchain;
    prop_set_unk(core_window, "Dispatcher", obj_default(dispatcher));
    prop_set_unk(xaml_window, "CoreWindow", obj_iface(core_window, "Windows.UI.Core.ICoreWindow"));
    prop_set_unk(xaml_window, "Dispatcher", obj_default(dispatcher));

    override("Windows.UI.Xaml.IApplicationStatics", "Start", app_start);
    override("Windows.UI.Xaml.IApplicationStatics", "get_Current", app_statics_current);
    override("Windows.UI.Xaml.IApplicationStatics", "LoadComponent", app_load_component);
    override("Windows.UI.Xaml.IApplicationStatics", "LoadComponent#2", app_load_component2);
    override("Windows.UI.Xaml.IFrameworkElement", "FindName", fe_find_name);
    override("Windows.UI.Xaml.IWindowStatics", "get_Current", win_statics_current);
    override("Windows.UI.Xaml.IFrameworkElement", "get_ActualWidth", fe_get_actual_size);
    override("Windows.UI.Xaml.Media.IGeneralTransform", "TransformPoint", gt_transform_point);
    override("Windows.UI.Xaml.Controls.IControl", "Focus", control_focus);
    override("Windows.ApplicationModel.ISuspendingOperation", "GetDeferral", suspend_get_deferral);
    override("Windows.ApplicationModel.ISuspendingDeferral", "Complete", suspend_complete);
    override("Windows.UI.Xaml.Media.IGeneralTransform", "TryTransform", gt_try_transform);
    override("Windows.UI.Xaml.Media.IGeneralTransform", "TransformBounds", gt_transform_bounds);
    override("Windows.UI.Xaml.IFrameworkElement", "get_ActualHeight", fe_get_actual_height);
    override("Windows.UI.Xaml.IFrameworkElement", "add_SizeChanged", fe_add_size_changed);
    override("Windows.UI.Xaml.Controls.IMediaElement", "add_MediaEnded", me_add_media_ended);
    override("Windows.UI.Xaml.IWindow", "put_Content", win_put_content);
    override("Windows.UI.Xaml.IWindow", "Activate", win_activate);
    override("Windows.UI.Xaml.IWindow", "get_Bounds", cw_get_bounds);
    override("Windows.UI.Xaml.IWindow", "get_Visible", true_getter);
    override("Windows.UI.Core.ICoreWindowStatic", "GetForCurrentThread", cw_statics_current);
    override("Windows.UI.Core.ICoreWindow", "get_Bounds", cw_get_bounds);
    override("Windows.UI.Core.ICoreWindow", "get_Visible", true_getter);
    override("Windows.UI.Core.ICoreWindow", "GetKeyState", cw_get_key_state);
    override("Windows.UI.Core.ICoreWindow", "GetAsyncKeyState", cw_get_key_state);
    override("Windows.UI.Core.ICoreDispatcher", "RunAsync", disp_run_async);
    override("Windows.UI.Core.ICoreDispatcher", "ProcessEvents", disp_process_events);
    override("Windows.UI.Core.ICoreDispatcher", "get_HasThreadAccess", disp_has_access);
    override("Windows.Foundation.IUriRuntimeClassFactory", "CreateUri", uri_create);
    override("Windows.Foundation.IUriRuntimeClassFactory", "CreateWithRelativeUri", uri_create2);
}
