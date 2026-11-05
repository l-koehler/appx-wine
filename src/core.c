/* Generic WinRT object model: every interface from the metadata gets a vtable of
 * logging thunks with correct stdcall stack sizes; selected methods are overridden. */
#include "shim.h"
#include <stdio.h>
#include <stdarg.h>
#include <tlhelp32.h>

static const GUID IID_IInspectable_ = {0xaf86e2e0, 0xb12d, 0x4c6a, {0x9c, 0x5a, 0xd7, 0xaa, 0x65, 0x10, 0x1e, 0x90}};
static const GUID IID_IAgileObject_ = {0x94ea2b94, 0xe9cc, 0x49e0, {0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90}};
static const GUID IID_IWeakReferenceSource_ = {0x00000038, 0, 0, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};
static const GUID IID_IActivationFactory_ = {0x00000035, 0, 0, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};

static CRITICAL_SECTION log_cs;
static int log_verbose = 1;
static HANDLE log_file = INVALID_HANDLE_VALUE;

void shim_log(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    DWORD w;
    int n = snprintf(buf, sizeof(buf), "[shim %04lx] ", GetCurrentThreadId());
    va_start(ap, fmt);
    n += vsnprintf(buf + n, sizeof(buf) - n - 2, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof(buf) - 2) n = sizeof(buf) - 2;
    buf[n++] = '\n';
    EnterCriticalSection(&log_cs);
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), buf, n, &w, NULL);
    if (log_file != INVALID_HANDLE_VALUE) WriteFile(log_file, buf, n, &w, NULL);
    LeaveCriticalSection(&log_cs);
}

static const char *guid_str(const GUID *g)
{
    static char buf[4][40];
    static int i;
    char *b = buf[i++ & 3];
    sprintf(b, "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", g->Data1, g->Data2, g->Data3,
            g->Data4[0], g->Data4[1], g->Data4[2], g->Data4[3], g->Data4[4], g->Data4[5], g->Data4[6], g->Data4[7]);
    return b;
}

/* ---- metadata lookup ---- */

/* the generator emits names sorted by code point, so strcmp order holds */
int find_iface(const char *name)
{
    int lo = 0, hi = meta_n_ifaces - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2, c = strcmp(meta_ifaces[mid].name, name);
        if (!c) return mid;
        if (c < 0) lo = mid + 1;
        else hi = mid - 1;
    }
    return -1;
}

int find_iface_iid(const GUID *iid)
{
    for (int i = 0; i < meta_n_ifaces; i++)
        if (IsEqualGUID(&meta_ifaces[i].iid, iid)) return i;
    return -1;
}

int find_class(const char *name)
{
    int lo = 0, hi = meta_n_classes - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2, c = strcmp(meta_classes[mid].name, name);
        if (!c) return mid;
        if (c < 0) lo = mid + 1;
        else hi = mid - 1;
    }
    return -1;
}

static int base_slots(int iface)
{
    return meta_ifaces[iface].delegate ? 3 : 6;
}

int method_slot(const char *iface, const char *meth)
{
    return method_slot_idx(find_iface(iface), meth);
}

int method_slot_idx(int i, const char *meth)
{
    /* "Name#2" selects the second overload */
    int nth = 1, len = strlen(meth);
    const char *hash = strchr(meth, '#');
    if (i < 0) return -1;
    if (hash) {
        nth = atoi(hash + 1);
        len = hash - meth;
    }
    for (int m = 0; m < meta_ifaces[i].nmeth; m++) {
        const char *n = meta_meths[meta_ifaces[i].first + m].name;
        if (!strncmp(n, meth, len) && !n[len] && !--nth) return base_slots(i) + m;
    }
    return -1;
}

/* ---- vtables ---- */

HRESULT __cdecl generic_impl(const struct meth_info *mi, DWORD *args);
extern char generic_entry[];
__asm__(".text\n"
        ".globl _generic_entry\n"
        "_generic_entry:\n"
        "  push %eax\n"
        "  lea 8(%esp), %ecx\n"
        "  push %ecx\n"
        "  push %eax\n"
        "  call _generic_impl\n"
        "  add $8, %esp\n"
        "  pop %ecx\n"
        "  mov 4(%ecx), %ecx\n"
        "  pop %edx\n"
        "  add %ecx, %esp\n"
        "  jmp *%edx\n");

static void ***vtables;
static void *base_vtbl[6];
static void *inner_vtbl[6];
static int *call_counts;

static BYTE *thunk_alloc(int n)
{
    return VirtualAlloc(NULL, n * 10, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
}

static void **get_vtbl(int iface)
{
    const struct iface_info *ii = &meta_ifaces[iface];
    int bs = base_slots(iface);
    void **vt;
    BYTE *th;
    if (vtables[iface]) return vtables[iface];
    vt = calloc(bs + ii->nmeth, sizeof(void *));
    memcpy(vt, base_vtbl, bs * sizeof(void *));
    th = thunk_alloc(ii->nmeth);
    for (int m = 0; m < ii->nmeth; m++) {
        BYTE *p = th + m * 10;
        const struct meth_info *mi = &meta_meths[ii->first + m];
        p[0] = 0xb8;
        *(const void **)(p + 1) = mi;
        p[5] = 0xe9;
        *(DWORD *)(p + 6) = (DWORD)(generic_entry - (char *)(p + 10));
        vt[bs + m] = p;
    }
    vtables[iface] = vt;
    return vt;
}

void override(const char *iface, const char *meth, void *fn)
{
    int i = find_iface(iface), s;
    if (i < 0 || (s = method_slot_idx(i, meth)) < 0) {
        shim_log("override: %s.%s not found", iface, meth);
        return;
    }
    get_vtbl(i)[s] = fn;
}

/* ---- objects ---- */

static int list_has(int first, int n, int iface, int depth)
{
    for (int i = 0; i < n; i++) {
        int x = meta_lists[first + i];
        if (x == iface) return 1;
        if (depth < 6 && list_has(meta_ifaces[x].req_first, meta_ifaces[x].nreq, iface, depth + 1)) return 1;
    }
    return 0;
}

static int iface_allowed(struct obj *o, int iface)
{
    if (o->is_factory) {
        const struct class_info *c = &meta_classes[o->cls];
        return list_has(c->st_first, c->st_n, iface, 0) || list_has(c->fa_first, c->fa_n, iface, 0) ||
               list_has(c->co_first, c->co_n, iface, 0);
    }
    for (int c = o->cls; c >= 0; c = meta_classes[c].base)
        if (list_has(meta_classes[c].if_first, meta_classes[c].if_n, iface, 0)) return 1;
    if (o->iface >= 0) {
        if (o->iface == iface) return 1;
        return list_has(meta_ifaces[o->iface].req_first, meta_ifaces[o->iface].nreq, iface, 0);
    }
    return 0;
}

static struct tear *add_tear(struct obj *o, int iface, const GUID *iid, void **vtbl)
{
    struct tear *t = calloc(1, sizeof(*t));
    t->vtbl = vtbl;
    t->obj = o;
    t->iface = iface;
    t->iid = *iid;
    if (o->ntears == o->maxtears) {
        o->maxtears = o->maxtears ? o->maxtears * 2 : 8;
        o->tears = realloc(o->tears, o->maxtears * sizeof(*o->tears));
    }
    o->tears[o->ntears++] = t;
    return t;
}

static struct tear *find_tear(struct obj *o, const GUID *iid)
{
    for (int i = 0; i < o->ntears; i++)
        if (IsEqualGUID(&o->tears[i]->iid, iid)) return o->tears[i];
    return NULL;
}

void *obj_iface_idx(struct obj *o, int iface)
{
    struct tear *t;
    if (iface < 0) return NULL;
    t = find_tear(o, &meta_ifaces[iface].iid);
    if (!t) t = add_tear(o, iface, &meta_ifaces[iface].iid, get_vtbl(iface));
    return t;
}

void *obj_iface(struct obj *o, const char *iface)
{
    int i = find_iface(iface);
    if (i < 0) shim_log("obj_iface: unknown %s", iface);
    return obj_iface_idx(o, i);
}

void obj_add_custom(struct obj *o, const GUID *iid, void **vtbl)
{
    add_tear(o, -1, iid, vtbl);
}

void *obj_default(struct obj *o)
{
    if (o->cls >= 0 && meta_classes[o->cls].dflt >= 0) return obj_iface_idx(o, meta_classes[o->cls].dflt);
    if (o->iface >= 0) return obj_iface_idx(o, o->iface);
    return &o->inner;
}

struct obj *obj_from(void *p)
{
    struct tear *t = p;
    if (!p) return NULL;
    if (t->vtbl == inner_vtbl || (t->vtbl && t->vtbl[0] == base_vtbl[0])) return t->obj;
    return NULL;
}

static struct obj *obj_alloc(void)
{
    struct obj *o = calloc(1, sizeof(*o));
    o->ref = 1;
    o->cls = o->iface = -1;
    o->inner.vtbl = inner_vtbl;
    o->inner.obj = o;
    o->inner.iface = -1;
    return o;
}

struct obj *obj_new_class(int cls)
{
    struct obj *o = obj_alloc();
    o->cls = cls;
    snprintf(o->name, sizeof(o->name), "%s", meta_classes[cls].name);
    return o;
}

struct obj *obj_new_named(const char *cls)
{
    int c = find_class(cls);
    if (c < 0) {
        shim_log("obj_new_named: unknown class %s", cls);
        return NULL;
    }
    return obj_new_class(c);
}

struct obj *obj_new_iface(int iface)
{
    struct obj *o = obj_alloc();
    o->iface = iface;
    snprintf(o->name, sizeof(o->name), "%s", meta_ifaces[iface].name);
    return o;
}

/* ---- IUnknown / IInspectable ---- */

static HRESULT nd_qi(struct obj *o, REFIID iid, void **out)
{
    struct tear *t;
    int idx;
    if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_IInspectable_) ||
        IsEqualGUID(iid, &IID_IAgileObject_)) {
        *out = &o->inner;
        InterlockedIncrement(&o->ref);
        return S_OK;
    }
    if (IsEqualGUID(iid, &IID_IMarshal)) {
        if (!o->ftm) CoCreateFreeThreadedMarshaler((IUnknown *)&o->inner, &o->ftm);
        if (o->ftm) return IUnknown_QueryInterface(o->ftm, iid, out);
    }
    if ((t = find_tear(o, iid))) {
        *out = t;
        IUnknown_AddRef((IUnknown *)t);
        return S_OK;
    }
    idx = find_iface_iid(iid);
    if (idx >= 0 && iface_allowed(o, idx)) {
        *out = obj_iface_idx(o, idx);
        IUnknown_AddRef((IUnknown *)*out);
        return S_OK;
    }
    if (!IsEqualGUID(iid, &IID_IWeakReferenceSource_))
        shim_log("QI miss on %s: %s (%s)", o->name, guid_str(iid), idx >= 0 ? meta_ifaces[idx].name : "?");
    *out = NULL;
    return E_NOINTERFACE;
}

static HRESULT WINAPI t_qi(struct tear *t, REFIID iid, void **out)
{
    struct obj *o = t->obj;
    if (o->outer) return IUnknown_QueryInterface(o->outer, iid, out);
    return nd_qi(o, iid, out);
}

static ULONG WINAPI t_addref(struct tear *t)
{
    if (t->obj->outer) return IUnknown_AddRef(t->obj->outer);
    return InterlockedIncrement(&t->obj->ref);
}

static ULONG WINAPI t_release(struct tear *t)
{
    /* objects are never freed: lifetime bugs in the mocks must not crash the game */
    if (t->obj->outer) return IUnknown_Release(t->obj->outer);
    return InterlockedDecrement(&t->obj->ref);
}

static HRESULT WINAPI t_getiids(struct tear *t, ULONG *n, IID **iids)
{
    struct obj *o = t->obj;
    *n = 0;
    *iids = CoTaskMemAlloc(sizeof(IID) * (o->ntears + 1));
    for (int i = 0; i < o->ntears; i++) (*iids)[(*n)++] = o->tears[i]->iid;
    return S_OK;
}

static HRESULT WINAPI t_classname(struct tear *t, HSTRING *name)
{
    WCHAR buf[128];
    MultiByteToWideChar(CP_UTF8, 0, t->obj->name, -1, buf, 128);
    return WindowsCreateString(buf, wcslen(buf), name);
}

static HRESULT WINAPI t_trust(struct tear *t, TrustLevel *l)
{
    *l = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI inner_qi(struct tear *t, REFIID iid, void **out)
{
    return nd_qi(t->obj, iid, out);
}

static ULONG WINAPI inner_addref(struct tear *t)
{
    return InterlockedIncrement(&t->obj->ref);
}

static ULONG WINAPI inner_release(struct tear *t)
{
    return InterlockedDecrement(&t->obj->ref);
}

/* ---- properties and events ---- */

struct prop *prop_find(struct obj *o, const char *name)
{
    for (struct prop *p = o->props; p; p = p->next)
        if (!strcmp(p->name, name)) return p;
    return NULL;
}

static struct prop *prop_get(struct obj *o, const char *name)
{
    struct prop *p = prop_find(o, name);
    if (p) {
        if (p->kind == PK_OBJECT && p->v.unk) IUnknown_Release(p->v.unk);
        if (p->kind == PK_HSTRING) WindowsDeleteString(p->v.str);
        memset(&p->v, 0, sizeof(p->v));
        return p;
    }
    p = calloc(1, sizeof(*p));
    snprintf(p->name, sizeof(p->name), "%s", name);
    p->next = o->props;
    o->props = p;
    return p;
}

void prop_set_unk(struct obj *o, const char *name, IUnknown *unk)
{
    struct prop *p = prop_get(o, name);
    p->kind = PK_OBJECT;
    p->size = 4;
    p->v.unk = unk;
    if (unk) IUnknown_AddRef(unk);
}

void prop_set_str(struct obj *o, const char *name, const WCHAR *s)
{
    struct prop *p = prop_get(o, name);
    p->kind = PK_HSTRING;
    p->size = 4;
    WindowsCreateString(s, wcslen(s), &p->v.str);
}

void prop_set_raw(struct obj *o, const char *name, const void *data, int size)
{
    struct prop *p = prop_get(o, name);
    p->kind = PK_VALUE;
    p->size = size > 16 ? 16 : size;
    memcpy(p->v.raw, data, p->size);
}

static INT64 next_token = 1;

static void event_add(struct obj *o, const char *name, IUnknown *h, INT64 *token)
{
    struct event *e = calloc(1, sizeof(*e));
    snprintf(e->name, sizeof(e->name), "%s", name);
    e->handler = h;
    if (h) IUnknown_AddRef(h);
    e->token = next_token++;
    e->next = o->events;
    o->events = e;
    if (token) *token = e->token;
}

static void event_remove(struct obj *o, INT64 token)
{
    for (struct event **e = &o->events; *e; e = &(*e)->next)
        if ((*e)->token == token) {
            *e = (*e)->next;
            return;
        }
}

int event_count(struct obj *o, const char *name)
{
    int n = 0;
    for (struct event *e = o->events; e; e = e->next)
        if (!strcmp(e->name, name)) n++;
    return n;
}

HRESULT invoke0(IUnknown *d)
{
    return VCALL(d, 3, HRESULT(WINAPI *)(void *))(d);
}

HRESULT invoke1(IUnknown *d, void *a)
{
    return VCALL(d, 3, HRESULT(WINAPI *)(void *, void *))(d, a);
}

HRESULT invoke2(IUnknown *d, void *a, void *b)
{
    return VCALL(d, 3, HRESULT(WINAPI *)(void *, void *, void *))(d, a, b);
}

void event_fire(struct obj *o, const char *name, void *sender, void *args)
{
    struct event *e, *next;
    for (e = o->events; e; e = next) {
        next = e->next;
        if (!strcmp(e->name, name) && e->handler) {
            HRESULT hr = invoke2(e->handler, sender, args);
            if (FAILED(hr)) shim_log("event %s.%s handler failed %08lx", o->name, name, hr);
        }
    }
}

/* ---- generic method implementation ---- */

void (*compose_hook)(struct obj *o);
void (*auto_hook)(struct obj *o);

struct obj *compose(int cls, IUnknown *outer, void **inner, void **result)
{
    struct obj *no = obj_new_class(cls);
    no->outer = outer;
    if (inner) *inner = &no->inner;
    else no->ref = 0;
    *result = obj_default(no);
    if (outer) IUnknown_AddRef(outer);
    else InterlockedIncrement(&no->ref);
    shim_log("composed %s (outer %p)", no->name, outer);
    if (compose_hook) compose_hook(no);
    return no;
}

static void auto_object(struct obj *o, const struct meth_info *mi, const char *key, void **out)
{
    struct prop *p = prop_find(o, key);
    struct obj *n;
    if (p && p->kind == PK_OBJECT) {
        *out = p->v.unk;
        if (p->v.unk) IUnknown_AddRef(p->v.unk);
        return;
    }
    if (mi->rk == RK_CLASS) n = obj_new_class(mi->rtype);
    else n = obj_new_iface(mi->rtype);
    if (auto_hook) auto_hook(n);
    prop_set_unk(o, key, obj_default(n));
    *out = obj_default(n);
    IUnknown_AddRef((IUnknown *)*out);
}

static volatile LONG call_total;
static DWORD main_tid;

HRESULT __cdecl generic_impl(const struct meth_info *mi, DWORD *args)
{
    struct tear *t = (struct tear *)args[0];
    struct obj *o = t->obj;
    const char *n = mi->name;
    int k = mi->argbytes / 4 - 1; /* number of params */
    int idx = mi - meta_meths;
    void **outp = mi->rk == RK_VOID ? NULL : (void **)args[k];
    const char *iname = meta_ifaces[mi->iface].name;

    if (GetCurrentThreadId() == main_tid) InterlockedIncrement(&call_total);
    if (log_verbose && (call_counts[idx]++ < 3 || call_counts[idx] % 1000 == 0))
        shim_log("call %s.%s on %s%s (#%d)", iname, n, o->name, o->is_factory ? " [factory]" : "", call_counts[idx]);

    if (o->is_factory) {
        const struct class_info *c = &meta_classes[o->cls];
        for (int i = 0; i < c->co_n; i++)
            if (meta_lists[c->co_first + i] == mi->iface) {
                compose(o->cls, (IUnknown *)args[k - 2], (void **)args[k - 1], outp);
                return S_OK;
            }
        for (int i = 0; i < c->fa_n; i++)
            if (meta_lists[c->fa_first + i] == mi->iface) {
                struct obj *no = obj_new_class(o->cls);
                *outp = obj_default(no);
                return S_OK;
            }
    }
    if (!strncmp(n, "add_", 4)) {
        event_add(o, n + 4, (IUnknown *)args[1], (INT64 *)args[2]);
        return S_OK;
    }
    if (!strncmp(n, "remove_", 7)) {
        event_remove(o, *(INT64 *)&args[1]);
        return S_OK;
    }
    if (!strncmp(n, "put_", 4)) {
        if (mi->pk == PK_OBJECT) prop_set_unk(o, n + 4, (IUnknown *)args[1]);
        else if (mi->pk == PK_HSTRING) {
            struct prop *p = prop_get(o, n + 4);
            p->kind = PK_HSTRING;
            WindowsDuplicateString((HSTRING)args[1], &p->v.str);
        } else prop_set_raw(o, n + 4, &args[1], mi->argbytes - 4);
        return S_OK;
    }
    if (!outp) return S_OK;
    if (mi->rk == RK_ARRAY) {
        *(DWORD *)args[k - 1] = 0;
        *(void **)args[k] = NULL;
        return S_OK;
    }
    if (!strncmp(n, "get_", 4)) {
        struct prop *p = prop_find(o, n + 4);
        if (p) {
            if (p->kind == PK_OBJECT) {
                *outp = p->v.unk;
                if (p->v.unk) IUnknown_AddRef(p->v.unk);
            } else if (p->kind == PK_HSTRING) {
                WindowsDuplicateString(p->v.str, (HSTRING *)outp);
            } else {
                memset(outp, 0, mi->rsize);
                memcpy(outp, p->v.raw, min(p->size, mi->rsize));
            }
            return S_OK;
        }
    }
    if (mi->rk == RK_CLASS || mi->rk == RK_IFACE) {
        char key[70];
        snprintf(key, sizeof(key), "%s%s", strncmp(n, "get_", 4) ? "#" : "", strncmp(n, "get_", 4) ? n : n + 4);
        auto_object(o, mi, key, outp);
        return S_OK;
    }
    memset(outp, 0, mi->rsize ? mi->rsize : 4);
    return S_OK;
}

/* ---- activation factories ---- */

static struct obj **factories;

static HRESULT WINAPI af_activate(struct tear *t, IInspectable **out)
{
    struct obj *o = obj_new_class(t->obj->cls);
    shim_log("ActivateInstance %s", o->name);
    *out = obj_default(o);
    return S_OK;
}

static void *af_vtbl[7];

struct obj *factory_for(int cls)
{
    if (!factories[cls]) {
        struct obj *o = obj_new_class(cls);
        o->is_factory = 1;
        snprintf(o->name, sizeof(o->name), "%s", meta_classes[cls].name);
        obj_add_custom(o, &IID_IActivationFactory_, af_vtbl);
        factories[cls] = o;
    }
    return factories[cls];
}

/* ---- import patching ---- */

void patch_import(const char *func, void *repl)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
    MODULEENTRY32 me = {sizeof(me)};
    if (snap == INVALID_HANDLE_VALUE) return;
    for (BOOL ok = Module32First(snap, &me); ok; ok = Module32Next(snap, &me)) {
        BYTE *base = me.modBaseAddr;
        IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
        IMAGE_DATA_DIRECTORY *dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (!dir->VirtualAddress) continue;
        for (IMAGE_IMPORT_DESCRIPTOR *d = (void *)(base + dir->VirtualAddress); d->Name; d++) {
            IMAGE_THUNK_DATA *names = (void *)(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
            IMAGE_THUNK_DATA *addrs = (void *)(base + d->FirstThunk);
            for (; names->u1.AddressOfData; names++, addrs++) {
                DWORD old;
                if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
                if (strcmp((char *)((IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData))->Name, func)) continue;
                VirtualProtect(&addrs->u1.Function, 4, PAGE_READWRITE, &old);
                addrs->u1.Function = (DWORD)repl;
                VirtualProtect(&addrs->u1.Function, 4, old, &old);
                shim_log("patched %s in %s", func, me.szModule);
            }
        }
    }
    CloseHandle(snap);
}

/* ---- crash diagnostics ---- */

static const char *addr_name(DWORD a)
{
    static char buf[4][300];
    static int i;
    char *b = buf[i++ & 3], mod[MAX_PATH];
    HMODULE m;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)a, &m) && GetModuleFileNameA(m, mod, sizeof(mod))) {
        snprintf(b, 300, "%s+%lx", strrchr(mod, '\\') ? strrchr(mod, '\\') + 1 : mod, a - (DWORD)m);
        return b;
    }
    return NULL;
}

static void dump_stack(DWORD *sp, int words);

static LONG CALLBACK crash_handler(EXCEPTION_POINTERS *ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    CONTEXT *c = ep->ContextRecord;
    DWORD *sp = (DWORD *)c->Esp;
    const char *n;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != 0xc0000409) {
        if (code == 0xe06d7363) {
            int k = 0;
            char line[600] = "";
            for (int i = 0; i < 200 && k < 8; i++) {
                if (IsBadReadPtr(sp + i, 4)) break;
                if ((n = addr_name(sp[i])) && !strstr(n, "MSVCR") && !strstr(n, "MSVCP") && !strstr(n, "kernel") && !strstr(n, "ntdll") &&
                    !strstr(n, "+ffc00000") && strcmp(strchr(n, '+'), "+0")) {
                    snprintf(line + strlen(line), sizeof(line) - strlen(line), " %s", n);
                    k++;
                }
            }
            shim_log("C++ exception:%s", line);
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }
    shim_log("exception %08lx at %08lx (%s) addr %08lx eax %08lx ecx %08lx edx %08lx esi %08lx edi %08lx", code,
             c->Eip, addr_name(c->Eip) ?: "?", ep->ExceptionRecord->ExceptionInformation[1], c->Eax, c->Ecx, c->Edx,
             c->Esi, c->Edi);
    for (int i = 0; i < 256; i++) {
        if (IsBadReadPtr(sp + i, 4)) break;
        if ((n = addr_name(sp[i])) && !strstr(n, "ntdll") && !strstr(n, "kernel")) shim_log("  stack+%03x: %s", i * 4, n);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static void dump_stack(DWORD *sp, int words)
{
    const char *n;
    for (int i = 0; i < words; i++) {
        if (IsBadReadPtr(sp + i, 4)) break;
        if ((n = addr_name(sp[i])) && !strstr(n, "+ffc00000") && strcmp(strchr(n, '+'), "+0"))
            shim_log("  stack+%03x: %s", i * 4, n);
    }
}

/* SHIM_WATCHDOG=<seconds>: dump the main thread's stack whenever it makes no WinRT calls for that long */
static DWORD WINAPI watchdog(void *arg)
{
    int secs = (int)(INT_PTR)arg;
    HANDLE th = OpenThread(THREAD_ALL_ACCESS, FALSE, main_tid);
    LONG last = -1;
    for (;;) {
        Sleep(secs * 1000);
        if (call_total == last) {
            CONTEXT c = {.ContextFlags = CONTEXT_FULL};
            SuspendThread(th);
            GetThreadContext(th, &c);
            shim_log("watchdog: main thread idle, eip %08lx (%s)", c.Eip, addr_name(c.Eip) ?: "?");
            dump_stack((DWORD *)c.Esp, 512);
            ResumeThread(th);
        }
        last = call_total;
    }
    return 0;
}

static void WINAPI hook_exit_process(UINT code)
{
    DWORD *sp;
    __asm__ volatile("mov %%esp, %0" : "=r"(sp));
    shim_log("ExitProcess(%u)", code);
    dump_stack(sp, 512);
    ExitProcess(code);
}

static BOOL WINAPI hook_terminate_process(HANDLE h, UINT code)
{
    DWORD *sp;
    __asm__ volatile("mov %%esp, %0" : "=r"(sp));
    shim_log("TerminateProcess(%p, %u)", h, code);
    dump_stack(sp, 512);
    return TerminateProcess(h, code);
}

/* ---- entry points ---- */

static int initialized;

void shim_init(void)
{
    char path[MAX_PATH];
    if (initialized) return;
    initialized = 1;
    if (GetEnvironmentVariableA("SHIM_LOG", path, sizeof(path)))
        log_file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);
    if (GetEnvironmentVariableA("SHIM_QUIET", path, sizeof(path))) log_verbose = 0;
    vtables = calloc(meta_n_ifaces, sizeof(*vtables));
    call_counts = calloc(65536, sizeof(int));
    factories = calloc(meta_n_classes, sizeof(*factories));
    base_vtbl[0] = t_qi;
    base_vtbl[1] = t_addref;
    base_vtbl[2] = t_release;
    base_vtbl[3] = t_getiids;
    base_vtbl[4] = t_classname;
    base_vtbl[5] = t_trust;
    inner_vtbl[0] = inner_qi;
    inner_vtbl[1] = inner_addref;
    inner_vtbl[2] = inner_release;
    inner_vtbl[3] = t_getiids;
    inner_vtbl[4] = t_classname;
    inner_vtbl[5] = t_trust;
    memcpy(af_vtbl, base_vtbl, sizeof(base_vtbl));
    af_vtbl[6] = af_activate;
    AddVectoredExceptionHandler(1, crash_handler);
    main_tid = GetCurrentThreadId();
    patch_import("ExitProcess", hook_exit_process);
    patch_import("TerminateProcess", hook_terminate_process);
    if (GetEnvironmentVariableA("SHIM_WATCHDOG", path, sizeof(path)))
        CloseHandle(CreateThread(NULL, 0, watchdog, (void *)(INT_PTR)atoi(path), 0, NULL));
    shim_log("init: %d interfaces, %d classes", meta_n_ifaces, meta_n_classes);
    package_init();
    misc_init();
    buffer_init();
    collections_init();
    async_init();
    dxgi_init();
    xaml_init();
}

__declspec(dllexport) HRESULT WINAPI DllGetActivationFactory(HSTRING name, IActivationFactory **out)
{
    char buf[256];
    UINT32 len;
    const WCHAR *w = WindowsGetStringRawBuffer(name, &len);
    int c;
    shim_init();
    WideCharToMultiByte(CP_UTF8, 0, w, len + 1, buf, sizeof(buf), NULL, NULL);
    c = find_class(buf);
    shim_log("DllGetActivationFactory %s -> %d", buf, c);
    if (c < 0) return CLASS_E_CLASSNOTAVAILABLE;
    *out = (IActivationFactory *)find_tear(factory_for(c), &IID_IActivationFactory_);
    IUnknown_AddRef((IUnknown *)*out);
    return S_OK;
}

__declspec(dllexport) HRESULT WINAPI DllCanUnloadNow(void)
{
    return S_FALSE;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void *reserved)
{
    if (reason == DLL_PROCESS_ATTACH) {
        InitializeCriticalSection(&log_cs);
        DisableThreadLibraryCalls(inst);
    }
    return TRUE;
}

IInspectable *obj_identity(struct obj *o)
{
    return o->outer ? (IInspectable *)o->outer : (IInspectable *)&o->inner;
}
