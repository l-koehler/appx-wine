#ifndef SHIM_H
#define SHIM_H

#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <inspectable.h>
#include <activation.h>
#include <winstring.h>

/* ---- generated metadata (winrt_meta.c) ---- */
enum { RK_VOID, RK_VALUE, RK_HSTRING, RK_CLASS, RK_IFACE, RK_OBJECT, RK_GENERIC, RK_ARRAY };
enum { PK_VALUE, PK_OBJECT, PK_HSTRING };

struct meth_info {
    const char *name;
    int argbytes; /* stack bytes including this and out params */
    int rk;       /* RK_* */
    int rsize;    /* size of the returned value */
    int rtype;    /* class (RK_CLASS) or interface (RK_IFACE) index */
    int iface;
    int pk;       /* PK_* of the first parameter */
};

struct iface_info {
    const char *name;
    GUID iid;
    int first, nmeth;
    int req_first, nreq;
    int delegate;
};

struct class_info {
    const char *name;
    int base;
    int if_first, if_n;
    int st_first, st_n;
    int fa_first, fa_n;
    int co_first, co_n;
    int activatable;
    int dflt;
};

extern const struct meth_info meta_meths[];
extern const struct iface_info meta_ifaces[];
extern const struct class_info meta_classes[];
extern const unsigned short meta_lists[];
extern const int meta_n_ifaces, meta_n_classes;

/* ---- object model (core.c) ---- */
struct obj;
struct coll;

struct tear {
    void **vtbl;
    struct obj *obj;
    int iface;    /* index into meta_ifaces, -1 for custom */
    GUID iid;
};

struct prop {
    struct prop *next;
    char name[64];
    int kind;     /* PK_* */
    int size;
    union { BYTE raw[16]; IUnknown *unk; HSTRING str; } v;
};

struct event {
    struct event *next;
    char name[64];
    INT64 token;
    IUnknown *handler;
};

struct named {
    struct named *next;
    WCHAR name[64];
    struct obj *obj;
};

struct obj {
    LONG ref;
    int cls;              /* meta_classes index or -1 */
    int iface;            /* for interface-only mocks */
    char name[128];
    IUnknown *outer;      /* controlling unknown when aggregated */
    struct tear inner;    /* non-delegating IInspectable */
    int is_factory;
    int ntears, maxtears;
    struct tear **tears;
    struct prop *props;
    struct event *events;
    struct named *names;  /* XAML namescope */
    IUnknown *ftm;        /* free-threaded marshaler */
    struct coll *coll;    /* collections.c */
    int pos;
    struct obj *children[16];
    int nchildren;
    void *priv;
};

void shim_log(const char *fmt, ...);
void shim_init(void);

int find_iface(const char *name);
int find_iface_iid(const GUID *iid);
int find_class(const char *name);
int method_slot(const char *iface, const char *meth);
int method_slot_idx(int iface, const char *meth);

struct obj *obj_new_class(int cls);
struct obj *obj_new_iface(int iface);
struct obj *obj_new_named(const char *cls);
void *obj_iface(struct obj *o, const char *iface);       /* no AddRef */
void *obj_iface_idx(struct obj *o, int iface);
void *obj_default(struct obj *o);
void obj_add_custom(struct obj *o, const GUID *iid, void **vtbl);
struct obj *obj_from(void *iface_ptr);                   /* NULL if not ours */
struct obj *factory_for(int cls);
struct obj *compose(int cls, IUnknown *outer, void **inner, void **result);
extern void (*compose_hook)(struct obj *o);
extern void (*auto_hook)(struct obj *o); /* called for objects created by generic getters */
IInspectable *obj_identity(struct obj *o); /* outer if aggregated, else inner; no AddRef */

void override(const char *iface, const char *meth, void *fn);

struct prop *prop_find(struct obj *o, const char *name);
void prop_set_unk(struct obj *o, const char *name, IUnknown *unk);
void prop_set_str(struct obj *o, const char *name, const WCHAR *s);
void prop_set_raw(struct obj *o, const char *name, const void *data, int size);

void event_fire(struct obj *o, const char *name, void *sender, void *args);
int event_count(struct obj *o, const char *name);

HRESULT invoke0(IUnknown *delegate);
HRESULT invoke1(IUnknown *delegate, void *a);
HRESULT invoke2(IUnknown *delegate, void *a, void *b);
#define VCALL(p, slot, type) ((type)((*(void ***)(p))[slot]))

void patch_import(const char *func, void *repl);

/* ---- package identity (package.c) ---- */
struct package {
    WCHAR dir[MAX_PATH];  /* install location (directory of the executable) */
    WCHAR name[128], publisher[256], publisher_id[16], display_name[128];
    WCHAR family_name[256], full_name[256];
    USHORT version[4];
    int arch;             /* Windows.System.ProcessorArchitecture */
};
extern struct package package;
void package_init(void);

/* ---- implementations ---- */
void xaml_init(void);
void dxgi_init(void);
void misc_init(void);
void buffer_init(void);
void collections_init(void);
void async_init(void);
extern void (*map_change_hook)(struct obj *o);
void coll_append(struct obj *o, const char *iface, void *item);
void map_put(struct obj *o, const char *iface, const WCHAR *key, void *val);
IInspectable *box_string(const WCHAR *s);
struct obj *buffer_new(const void *data, UINT32 len);

extern HWND main_hwnd;
extern struct obj *core_window, *xaml_window, *dispatcher, *app_obj, *comp_target;

#define TOKEN_STRUCT EventRegistrationToken
#endif
