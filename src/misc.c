/* Package objects, storage folders, display properties, device info and Wine combase gaps */
#include "shim.h"
#include <stdio.h>
#include <shlobj.h>
#include <roapi.h>

static WCHAR data_dir[MAX_PATH];

static HRESULT WINAPI ro_capture_error_context(HRESULT hr)
{
    return S_OK;
}

static void WINAPI ro_fail_fast(HRESULT hr)
{
    shim_log("RoFailFastWithErrorContext(%08lx)", hr);
    ExitProcess(hr);
}

static struct obj *folder(const WCHAR *path)
{
    struct obj *o = obj_new_named("Windows.Storage.StorageFolder");
    prop_set_str(o, "Path", path);
    prop_set_str(o, "Name", wcsrchr(path, '\\') ? wcsrchr(path, '\\') + 1 : path);
    CreateDirectoryW(path, NULL);
    return o;
}

static HRESULT WINAPI ret_folder(void **out, const WCHAR *sub)
{
    WCHAR p[MAX_PATH];
    swprintf(p, MAX_PATH, L"%ls\\%ls", data_dir, sub);
    *out = obj_default(folder(p));
    return S_OK;
}

static HRESULT WINAPI ad_local(struct tear *t, void **out) { return ret_folder(out, L"LocalState"); }
static HRESULT WINAPI ad_roaming(struct tear *t, void **out) { return ret_folder(out, L"RoamingState"); }
static HRESULT WINAPI ad_temp(struct tear *t, void **out) { return ret_folder(out, L"TempState"); }

static HRESULT WINAPI pkg_installed(struct tear *t, void **out)
{
    *out = obj_default(folder(package.dir));
    return S_OK;
}

static HRESULT WINAPI pkg_id(struct tear *t, void **out)
{
    struct obj *o = obj_new_named("Windows.ApplicationModel.PackageId");
    prop_set_str(o, "Name", package.name);
    prop_set_str(o, "FullName", package.full_name);
    prop_set_str(o, "FamilyName", package.family_name);
    prop_set_str(o, "Publisher", package.publisher);
    prop_set_str(o, "PublisherId", package.publisher_id);
    prop_set_str(o, "ResourceId", L"");
    prop_set_raw(o, "Version", package.version, 8);
    prop_set_raw(o, "Architecture", &package.arch, 4);
    *out = obj_default(o);
    return S_OK;
}

static HRESULT WINAPI dpi(struct tear *t, float *v)
{
    *v = 96.0f;
    return S_OK;
}

static HRESULT WINAPI landscape(struct tear *t, int *v)
{
    *v = 1;
    return S_OK;
}

static HRESULT WINAPI scale100(struct tear *t, int *v)
{
    *v = 100;
    return S_OK;
}

/* Wine stubs IContextCallback::ContextCallback; PPL tasks use it to resume continuations */
typedef HRESULT(__stdcall *context_fn)(void *data);
static HRESULT WINAPI context_callback(IUnknown *this, context_fn fn, void *data, REFIID riid, int method, IUnknown *unk)
{
    return fn(data);
}

static void patch_context_callback(void)
{
    static const GUID IID_IContextCallback_ = {0x000001da, 0, 0, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};
    IUnknown *ctx = NULL;
    void **vt;
    DWORD old;
    if (FAILED(CoGetObjectContext(&IID_IContextCallback_, (void **)&ctx)) || !ctx) {
        shim_log("no IContextCallback to patch");
        return;
    }
    vt = *(void ***)ctx;
    VirtualProtect(&vt[3], 4, PAGE_READWRITE, &old);
    vt[3] = context_callback;
    VirtualProtect(&vt[3], 4, old, &old);
    shim_log("patched IContextCallback::ContextCallback");
}

IInspectable *box_string(const WCHAR *s)
{
    static void *statics;
    HSTRING cls, str;
    IInspectable *out = NULL;
    if (!statics) {
        static const GUID IID_IPropertyValueStatics = {0x629bdbc8, 0xd932, 0x4ff4, {0x96, 0xb9, 0x8d, 0x96, 0xc5, 0xc1, 0xe8, 0x58}};
        WindowsCreateString(L"Windows.Foundation.PropertyValue", 32, &cls);
        RoGetActivationFactory(cls, &IID_IPropertyValueStatics, &statics);
    }
    WindowsCreateString(s, wcslen(s), &str);
    if (statics)
        VCALL(statics, method_slot("Windows.Foundation.IPropertyValueStatics", "CreateString"),
              HRESULT(WINAPI *)(void *, HSTRING, IInspectable **))(statics, str, &out);
    return out;
}

static IInspectable *box_guid(const GUID *g)
{
    void *statics = NULL;
    HSTRING cls;
    IInspectable *out = NULL;
    static const GUID IID_IPropertyValueStatics = {0x629bdbc8, 0xd932, 0x4ff4, {0x96, 0xb9, 0x8d, 0x96, 0xc5, 0xc1, 0xe8, 0x58}};
    WindowsCreateString(L"Windows.Foundation.PropertyValue", 32, &cls);
    RoGetActivationFactory(cls, &IID_IPropertyValueStatics, &statics);
    if (statics)
        VCALL(statics, method_slot("Windows.Foundation.IPropertyValueStatics", "CreateGuid"),
              HRESULT(WINAPI *)(void *, GUID, IInspectable **))(statics, *g, &out);
    return out;
}

static IInspectable *pnp_value(const WCHAR *key)
{
    static const GUID display_class = {0x4d36e968, 0xe325, 0x11ce, {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};
    if (!wcsicmp(key, L"{A45C254E-DF1C-4EFD-8020-67D146A850E0},10")) return box_guid(&display_class); /* ClassGuid */
    if (!wcsicmp(key, L"{A8B865DD-2E3D-4094-AD97-E593A70C75D6},3")) return box_string(L"10.0.0.0"); /* DriverVersion */
    return box_string(L"Wine");
}

/* one fake device carrying every requested property */
static HRESULT WINAPI pnp_find_all(struct tear *t, int type, IUnknown *props, void **out)
{
    struct obj *op = obj_new_iface(find_iface("Windows.Foundation.IAsyncOperation`1<Windows.Devices.Enumeration.Pnp.PnpObjectCollection>"));
    struct obj *coll = obj_new_named("Windows.Devices.Enumeration.Pnp.PnpObjectCollection");
    struct obj *dev = obj_new_named("Windows.Devices.Enumeration.Pnp.PnpObject");
    struct obj *map = obj_new_iface(find_iface("Windows.Foundation.Collections.IMapView`2<String, Object>"));
    IUnknown *it = NULL;
    boolean has = 0;
    prop_set_str(dev, "Id", L"wine-device");
    prop_set_raw(dev, "Type", &type, 4);
    if (props && SUCCEEDED(VCALL(props, 6, HRESULT(WINAPI *)(void *, IUnknown **))(props, &it)) && it) {
        VCALL(it, 7, HRESULT(WINAPI *)(void *, boolean *))(it, &has);
        while (has) {
            HSTRING key = NULL;
            VCALL(it, 6, HRESULT(WINAPI *)(void *, HSTRING *))(it, &key);
            shim_log("PnpObject property %ls", WindowsGetStringRawBuffer(key, NULL));
            {
                /* Windows canonicalizes "{guid},pid"; store the spellings games use */
                const WCHAR *k = WindowsGetStringRawBuffer(key, NULL), *comma = wcschr(k, ',');
                IInspectable *v = pnp_value(k);
                WCHAR alt[100];
                map_put(map, "Windows.Foundation.Collections.IMapView`2<String, Object>", k, v);
                if (comma) {
                    swprintf(alt, 100, L"%.*ls%ls", (int)(comma - k), k, comma + 1);
                    map_put(map, "Windows.Foundation.Collections.IMapView`2<String, Object>", alt, v);
                    swprintf(alt, 100, L"%.*ls %ls", (int)(comma - k), k, comma + 1);
                    map_put(map, "Windows.Foundation.Collections.IMapView`2<String, Object>", alt, v);
                }
            }
            VCALL(it, 8, HRESULT(WINAPI *)(void *, boolean *))(it, &has);
        }
    }
    prop_set_unk(dev, "Properties", obj_default(map));
    coll_append(coll, "Windows.Foundation.Collections.IVectorView`1<Windows.Devices.Enumeration.Pnp.PnpObject>", obj_default(dev));
    prop_set_unk(op, "#GetResults", obj_default(coll));
    *out = obj_default(op);
    return S_OK;
}

static HRESULT WINAPI pnp_find_all2(struct tear *t, int type, IUnknown *props, HSTRING filter, void **out)
{
    return pnp_find_all(t, type, props, out);
}

void misc_init(void)
{
    WCHAR local[MAX_PATH];
    SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, local);
    swprintf(data_dir, MAX_PATH, L"%ls\\Packages", local);
    CreateDirectoryW(data_dir, NULL);
    wcscat(data_dir, L"\\");
    wcscat(data_dir, package.family_name);
    CreateDirectoryW(data_dir, NULL);
    shim_log("data dir %ls", data_dir);

    patch_import("RoCaptureErrorContext", ro_capture_error_context);
    patch_import("RoFailFastWithErrorContext", ro_fail_fast);
    patch_context_callback();

    override("Windows.Storage.IApplicationData", "get_LocalFolder", ad_local);
    override("Windows.Storage.IApplicationData", "get_RoamingFolder", ad_roaming);
    override("Windows.Storage.IApplicationData", "get_TemporaryFolder", ad_temp);
    override("Windows.ApplicationModel.IPackage", "get_InstalledLocation", pkg_installed);
    override("Windows.ApplicationModel.IPackage", "get_Id", pkg_id);
    override("Windows.Devices.Enumeration.Pnp.IPnpObjectStatics", "FindAllAsync", pnp_find_all);
    override("Windows.Devices.Enumeration.Pnp.IPnpObjectStatics", "FindAllAsync#2", pnp_find_all2);
    override("Windows.Graphics.Display.IDisplayPropertiesStatics", "get_LogicalDpi", dpi);
    override("Windows.Graphics.Display.IDisplayPropertiesStatics", "get_CurrentOrientation", landscape);
    override("Windows.Graphics.Display.IDisplayPropertiesStatics", "get_NativeOrientation", landscape);
    override("Windows.Graphics.Display.IDisplayPropertiesStatics", "get_ResolutionScale", scale100);
}
