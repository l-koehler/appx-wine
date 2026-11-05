/* IVector / IVectorView / IIterable / IIterator / IMap / IMapView / IKeyValuePair for
 * every generic instance with pointer-sized elements */
#include "shim.h"
#include <stdio.h>

struct coll {
    int n, cap;
    void **items;
    HSTRING *keys; /* maps only */
    int vkind;     /* PK_* of the element / value type */
};

static int elem_kind(const char *iface_name)
{
    const char *arg = strrchr(iface_name, '<');
    const char *last = strrchr(iface_name, ' ');
    static const char *prims[] = {"Boolean", "Char16", "Int8", "UInt8", "Int16", "UInt16", "Int32", "UInt32", "Single"};
    if (!arg) return PK_OBJECT;
    arg = last && last > arg ? last + 1 : arg + 1;
    if (!strncmp(arg, "String>", 7)) return PK_HSTRING;
    for (int i = 0; i < 9; i++)
        if (!strncmp(arg, prims[i], strlen(prims[i])) && arg[strlen(prims[i])] == '>') return PK_VALUE;
    return PK_OBJECT;
}

static struct coll *coll_of(struct tear *t)
{
    struct obj *o = t->obj;
    if (!o->coll) {
        o->coll = calloc(1, sizeof(struct coll));
        o->coll->vkind = t->iface >= 0 ? elem_kind(meta_ifaces[t->iface].name) : PK_OBJECT;
    }
    return o->coll;
}

static void hold(struct coll *c, void *v)
{
    if (c->vkind == PK_OBJECT && v) IUnknown_AddRef((IUnknown *)v);
}

static void *dup_out(struct coll *c, void *v)
{
    HSTRING s;
    if (c->vkind == PK_OBJECT && v) IUnknown_AddRef((IUnknown *)v);
    if (c->vkind == PK_HSTRING) {
        WindowsDuplicateString(v, &s);
        return s;
    }
    return v;
}

static void *store(struct coll *c, void *v)
{
    HSTRING s;
    if (c->vkind == PK_HSTRING) {
        WindowsDuplicateString(v, &s);
        return s;
    }
    hold(c, v);
    return v;
}

static void insert_at(struct coll *c, int i, void *v)
{
    if (c->n == c->cap) {
        c->cap = c->cap ? c->cap * 2 : 8;
        c->items = realloc(c->items, c->cap * sizeof(void *));
        c->keys = realloc(c->keys, c->cap * sizeof(HSTRING));
    }
    memmove(c->items + i + 1, c->items + i, (c->n - i) * sizeof(void *));
    memmove(c->keys + i + 1, c->keys + i, (c->n - i) * sizeof(HSTRING));
    c->items[i] = store(c, v);
    c->keys[i] = NULL;
    c->n++;
}

static void remove_at(struct coll *c, int i)
{
    memmove(c->items + i, c->items + i + 1, (c->n - i - 1) * sizeof(void *));
    memmove(c->keys + i, c->keys + i + 1, (c->n - i - 1) * sizeof(HSTRING));
    c->n--;
}

/* related generic instance: same type arguments, different generic interface */
static int sibling(int iface, const char *generic)
{
    char buf[512];
    const char *args = strchr(meta_ifaces[iface].name, '<');
    snprintf(buf, sizeof(buf), "Windows.Foundation.Collections.%s%s", generic, args);
    return find_iface(buf);
}

static struct obj *view_of(struct tear *t, const char *generic)
{
    int idx = sibling(t->iface, generic);
    struct obj *v;
    if (idx < 0) {
        shim_log("collections: no %s for %s", generic, meta_ifaces[t->iface].name);
        return NULL;
    }
    v = obj_new_iface(idx);
    v->coll = coll_of(t);
    return v;
}

/* ---- vector ---- */

static HRESULT WINAPI v_get_at(struct tear *t, UINT32 i, void **out)
{
    struct coll *c = coll_of(t);
    if (i >= (UINT32)c->n) return E_BOUNDS;
    *out = dup_out(c, c->items[i]);
    return S_OK;
}

static HRESULT WINAPI v_size(struct tear *t, UINT32 *n)
{
    *n = coll_of(t)->n;
    return S_OK;
}

static HRESULT WINAPI v_get_view(struct tear *t, void **out)
{
    struct obj *v = view_of(t, "IVectorView`1");
    *out = v ? obj_default(v) : NULL;
    return v ? S_OK : E_NOTIMPL;
}

static HRESULT WINAPI v_index_of(struct tear *t, void *item, UINT32 *index, boolean *found)
{
    struct coll *c = coll_of(t);
    *found = 0;
    *index = 0;
    for (int i = 0; i < c->n; i++)
        if (c->items[i] == item) {
            *found = 1;
            *index = i;
            break;
        }
    return S_OK;
}

static HRESULT WINAPI v_set_at(struct tear *t, UINT32 i, void *item)
{
    struct coll *c = coll_of(t);
    if (i >= (UINT32)c->n) return E_BOUNDS;
    c->items[i] = store(c, item);
    return S_OK;
}

static HRESULT WINAPI v_insert_at(struct tear *t, UINT32 i, void *item)
{
    struct coll *c = coll_of(t);
    if (i > (UINT32)c->n) return E_BOUNDS;
    insert_at(c, i, item);
    return S_OK;
}

static HRESULT WINAPI v_remove_at(struct tear *t, UINT32 i)
{
    struct coll *c = coll_of(t);
    if (i >= (UINT32)c->n) return E_BOUNDS;
    remove_at(c, i);
    return S_OK;
}

static HRESULT WINAPI v_append(struct tear *t, void *item)
{
    struct coll *c = coll_of(t);
    insert_at(c, c->n, item);
    return S_OK;
}

static HRESULT WINAPI v_remove_end(struct tear *t)
{
    struct coll *c = coll_of(t);
    if (!c->n) return E_BOUNDS;
    c->n--;
    return S_OK;
}

static HRESULT WINAPI v_clear(struct tear *t)
{
    coll_of(t)->n = 0;
    return S_OK;
}

static HRESULT WINAPI v_get_many(struct tear *t, UINT32 start, UINT32 cap, void **items, UINT32 *actual)
{
    struct coll *c = coll_of(t);
    UINT32 i;
    for (i = 0; i < cap && start + i < (UINT32)c->n; i++) items[i] = dup_out(c, c->items[start + i]);
    *actual = i;
    return S_OK;
}

static HRESULT WINAPI v_replace_all(struct tear *t, UINT32 n, void **items)
{
    struct coll *c = coll_of(t);
    c->n = 0;
    for (UINT32 i = 0; i < n; i++) insert_at(c, c->n, items[i]);
    return S_OK;
}

/* ---- iterable / iterator ---- */

static HRESULT WINAPI it_first(struct tear *t, void **out)
{
    struct obj *v = view_of(t, "IIterator`1");
    if (!v) return E_NOTIMPL;
    v->pos = 0;
    *out = obj_default(v);
    return S_OK;
}

static int kvp_iface(struct tear *t)
{
    /* IIterator`1<Windows.Foundation.Collections.IKeyValuePair`2<String, Object>> -> IKeyValuePair`2<String, Object> */
    char buf[512];
    const char *a = strchr(meta_ifaces[t->iface].name, '<');
    snprintf(buf, sizeof(buf), "%s", a + 1);
    buf[strlen(buf) - 1] = 0;
    return strncmp(buf, "Windows.Foundation.Collections.IKeyValuePair`2<", 47) ? -1 : find_iface(buf);
}

static void *iter_item(struct tear *t, int i)
{
    struct coll *c = coll_of(t);
    int kvp = kvp_iface(t);
    if (kvp >= 0) {
        struct obj *p = obj_new_iface(kvp);
        p->coll = calloc(1, sizeof(struct coll));
        p->coll->vkind = c->vkind;
        insert_at(p->coll, 0, c->items[i]);
        WindowsDuplicateString(c->keys[i], &p->coll->keys[0]);
        return obj_default(p);
    }
    return dup_out(c, c->items[i]);
}

static HRESULT WINAPI iter_current(struct tear *t, void **out)
{
    struct coll *c = coll_of(t);
    if (t->obj->pos >= c->n) return E_BOUNDS;
    *out = iter_item(t, t->obj->pos);
    return S_OK;
}

static HRESULT WINAPI iter_has_current(struct tear *t, boolean *v)
{
    *v = t->obj->pos < coll_of(t)->n;
    return S_OK;
}

static HRESULT WINAPI iter_move_next(struct tear *t, boolean *v)
{
    t->obj->pos++;
    *v = t->obj->pos < coll_of(t)->n;
    return S_OK;
}

static HRESULT WINAPI iter_get_many(struct tear *t, UINT32 cap, void **items, UINT32 *actual)
{
    struct coll *c = coll_of(t);
    UINT32 i;
    for (i = 0; i < cap && t->obj->pos < c->n; i++, t->obj->pos++) items[i] = iter_item(t, t->obj->pos);
    *actual = i;
    return S_OK;
}

/* ---- map with HSTRING keys ---- */

static int map_find(struct coll *c, HSTRING key)
{
    INT32 r;
    for (int i = 0; i < c->n; i++)
        if (SUCCEEDED(WindowsCompareStringOrdinal(c->keys[i], key, &r)) && !r) return i;
    return -1;
}

static HRESULT WINAPI m_lookup(struct tear *t, HSTRING key, void **out)
{
    struct coll *c = coll_of(t);
    int i = map_find(c, key);
    if (i < 0) {
        shim_log("map lookup miss: %ls", WindowsGetStringRawBuffer(key, NULL));
        *out = NULL;
        return E_BOUNDS;
    }
    *out = dup_out(c, c->items[i]);
    return S_OK;
}

static HRESULT WINAPI m_has_key(struct tear *t, HSTRING key, boolean *v)
{
    *v = map_find(coll_of(t), key) >= 0;
    return S_OK;
}

static HRESULT WINAPI m_get_view(struct tear *t, void **out)
{
    struct obj *v = view_of(t, "IMapView`2");
    *out = v ? obj_default(v) : NULL;
    return v ? S_OK : E_NOTIMPL;
}

static void map_changed(struct tear *t, int change, HSTRING key);

static HRESULT WINAPI m_insert(struct tear *t, HSTRING key, void *val, boolean *replaced)
{
    struct coll *c = coll_of(t);
    int i = map_find(c, key);
    *replaced = i >= 0;
    if (i >= 0) c->items[i] = store(c, val);
    else {
        insert_at(c, c->n, val);
        WindowsDuplicateString(key, &c->keys[c->n - 1]);
    }
    map_changed(t, i >= 0 ? 3 : 1, key);
    return S_OK;
}

static HRESULT WINAPI m_remove(struct tear *t, HSTRING key)
{
    struct coll *c = coll_of(t);
    int i = map_find(c, key);
    if (i < 0) return E_BOUNDS;
    remove_at(c, i);
    map_changed(t, 2, key);
    return S_OK;
}

static HRESULT WINAPI m_split(struct tear *t, void **a, void **b)
{
    *a = *b = NULL;
    return S_OK;
}

static HRESULT WINAPI kvp_key(struct tear *t, HSTRING *out)
{
    return WindowsDuplicateString(coll_of(t)->keys[0], out);
}

static HRESULT WINAPI kvp_value(struct tear *t, void **out)
{
    struct coll *c = coll_of(t);
    *out = dup_out(c, c->items[0]);
    return S_OK;
}

void (*map_change_hook)(struct obj *o);

static void map_changed(struct tear *t, int change, HSTRING key)
{
    if (map_change_hook) map_change_hook(t->obj);
}

/* ---- registration ---- */

static int pointer_sized(int iface, const char *meth, int expect)
{
    for (int m = 0; m < meta_ifaces[iface].nmeth; m++) {
        const struct meth_info *mi = &meta_meths[meta_ifaces[iface].first + m];
        if (!strcmp(mi->name, meth)) return mi->argbytes == expect;
    }
    return 0;
}

#define OV(m, f) override(n, m, f)

void collections_init(void)
{
    int nv = 0, nm = 0;
    for (int i = 0; i < meta_n_ifaces; i++) {
        const char *n = meta_ifaces[i].name;
        if (!strncmp(n, "Windows.Foundation.Collections.IVector`1<", 41) && pointer_sized(i, "Append", 8)) {
            OV("GetAt", v_get_at);
            OV("get_Size", v_size);
            OV("GetView", v_get_view);
            OV("IndexOf", v_index_of);
            OV("SetAt", v_set_at);
            OV("InsertAt", v_insert_at);
            OV("RemoveAt", v_remove_at);
            OV("Append", v_append);
            OV("RemoveAtEnd", v_remove_end);
            OV("Clear", v_clear);
            OV("GetMany", v_get_many);
            OV("ReplaceAll", v_replace_all);
            nv++;
        } else if (!strncmp(n, "Windows.Foundation.Collections.IVectorView`1<", 45) && pointer_sized(i, "GetAt", 12)) {
            OV("GetAt", v_get_at);
            OV("get_Size", v_size);
            OV("IndexOf", v_index_of);
            OV("GetMany", v_get_many);
        } else if (!strncmp(n, "Windows.Foundation.Collections.IIterable`1<", 43)) {
            OV("First", it_first);
        } else if (!strncmp(n, "Windows.Foundation.Collections.IIterator`1<", 43) && pointer_sized(i, "get_Current", 8)) {
            OV("get_Current", iter_current);
            OV("get_HasCurrent", iter_has_current);
            OV("MoveNext", iter_move_next);
            OV("GetMany", iter_get_many);
        } else if (!strncmp(n, "Windows.Foundation.Collections.IMap`2<String, ", 46) && pointer_sized(i, "Lookup", 12)) {
            OV("Lookup", m_lookup);
            OV("get_Size", v_size);
            OV("HasKey", m_has_key);
            OV("GetView", m_get_view);
            OV("Insert", m_insert);
            OV("Remove", m_remove);
            OV("Clear", v_clear);
            nm++;
        } else if (!strncmp(n, "Windows.Foundation.Collections.IMapView`2<String, ", 50) && pointer_sized(i, "Lookup", 12)) {
            OV("Lookup", m_lookup);
            OV("get_Size", v_size);
            OV("HasKey", m_has_key);
            OV("Split", m_split);
        } else if (!strncmp(n, "Windows.Foundation.Collections.IKeyValuePair`2<String, ", 55) && pointer_sized(i, "get_Value", 8)) {
            OV("get_Key", kvp_key);
            OV("get_Value", kvp_value);
        }
    }
    shim_log("collections: %d vector and %d map instances", nv, nm);
}

/* helpers for building collections from C */
void coll_append(struct obj *o, const char *iface, void *item)
{
    struct coll *c = coll_of(obj_iface(o, iface));
    insert_at(c, c->n, item);
}

void map_put(struct obj *o, const char *iface, const WCHAR *key, void *val)
{
    struct coll *c = coll_of(obj_iface(o, iface));
    insert_at(c, c->n, val);
    WindowsCreateString(key, wcslen(key), &c->keys[c->n - 1]);
}
