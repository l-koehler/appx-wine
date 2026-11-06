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

static HRESULT WINAPI float_one(struct tear *t, float *v)
{
    *v = 1.0f;
    return S_OK;
}

static HRESULT WINAPI double_one(struct tear *t, double *v)
{
    *v = 1.0;
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

enum pnp_kind { PNP_DISPLAY, PNP_HAL, PNP_ROOT_CONTAINER };

/* values as a Windows 10 22H2 VM reports them (see NOTES.md) */
static IInspectable *pnp_value(const WCHAR *key, enum pnp_kind kind)
{
    static const GUID display_class = {0x4d36e968, 0xe325, 0x11ce, {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};
    static const GUID hal_class = {0x4d36e966, 0xe325, 0x11ce, {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};
    if (!wcsicmp(key, L"{A45C254E-DF1C-4EFD-8020-67D146A850E0},10")) /* ClassGuid */
        return box_guid(kind == PNP_HAL ? &hal_class : &display_class);
    if (!wcsicmp(key, L"{A8B865DD-2E3D-4094-AD97-E593A70C75D6},3")) /* DriverVersion; the HAL's is the OS build */
        return box_string(kind == PNP_HAL ? L"10.0.19041.1" : L"10.0.0.0");
    if (!wcsicmp(key, L"{78C34FC8-104A-4ACA-9EA4-524D52996E57},97"))
        return kind == PNP_ROOT_CONTAINER ? box_string(L"Computer") : NULL;
    return box_string(L"Wine");
}

/* a fake device carrying every requested property */
static struct obj *pnp_device(int type, const WCHAR *id, IUnknown *props, enum pnp_kind kind)
{
    struct obj *dev = obj_new_named("Windows.Devices.Enumeration.Pnp.PnpObject");
    struct obj *map = obj_new_iface(find_iface("Windows.Foundation.Collections.IMapView`2<String, Object>"));
    IUnknown *it = NULL;
    boolean has = 0;
    prop_set_str(dev, "Id", id);
    prop_set_raw(dev, "Type", &type, 4);
    if (props && SUCCEEDED(VCALL(props, 6, HRESULT(WINAPI *)(void *, IUnknown **))(props, &it)) && it) {
        VCALL(it, 7, HRESULT(WINAPI *)(void *, boolean *))(it, &has);
        while (has) {
            HSTRING key = NULL;
            VCALL(it, 6, HRESULT(WINAPI *)(void *, HSTRING *))(it, &key);
            if (kind != PNP_HAL) shim_log("PnpObject property %ls", WindowsGetStringRawBuffer(key, NULL));
            {
                /* Windows canonicalizes "{guid},pid"; store the spellings games use */
                const WCHAR *k = WindowsGetStringRawBuffer(key, NULL), *comma = wcschr(k, ',');
                IInspectable *v = pnp_value(k, kind);
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
    return dev;
}

static HRESULT WINAPI pnp_find_all(struct tear *t, int type, IUnknown *props, void **out)
{
    struct obj *op = obj_new_iface(find_iface("Windows.Foundation.IAsyncOperation`1<Windows.Devices.Enumeration.Pnp.PnpObjectCollection>"));
    struct obj *coll = obj_new_named("Windows.Devices.Enumeration.Pnp.PnpObjectCollection");
    coll_append(coll, "Windows.Foundation.Collections.IVectorView`1<Windows.Devices.Enumeration.Pnp.PnpObject>",
                obj_default(pnp_device(type, L"wine-display", props, PNP_DISPLAY)));
    coll_append(coll, "Windows.Foundation.Collections.IVectorView`1<Windows.Devices.Enumeration.Pnp.PnpObject>",
                obj_default(pnp_device(type, L"wine-hal", props, PNP_HAL)));
    prop_set_unk(op, "#GetResults", obj_default(coll));
    *out = obj_default(op);
    return S_OK;
}

static HRESULT WINAPI pnp_create_from_id(struct tear *t, int type, HSTRING id, IUnknown *props, void **out)
{
    struct obj *op = obj_new_iface(find_iface("Windows.Foundation.IAsyncOperation`1<Windows.Devices.Enumeration.Pnp.PnpObject>"));
    shim_log("PnpObject.CreateFromIdAsync type %d id %ls", type, WindowsGetStringRawBuffer(id, NULL));
    prop_set_unk(op, "#GetResults", obj_default(pnp_device(type, WindowsGetStringRawBuffer(id, NULL), props, PNP_ROOT_CONTAINER)));
    *out = obj_default(op);
    return S_OK;
}

static HRESULT WINAPI pnp_find_all2(struct tear *t, int type, IUnknown *props, HSTRING filter, void **out)
{
    return pnp_find_all(t, type, props, out);
}

static HRESULT WINAPI device_id(struct tear *t, GUID *id)
{
    static const GUID fixed = {0x6f1c9a52, 0x3d7e, 0x4b8a, {0x9c, 0x21, 0x5e, 0x40, 0xa8, 0x17, 0x2b, 0x93}};
    *id = fixed;
    return S_OK;
}

static HRESULT WINAPI access_allowed(struct tear *t, int *status)
{
    *status = 1;
    return S_OK;
}

/* ---- PasswordVault: credentials are kept in memory ---- */

#define CRED_VIEW "Windows.Foundation.Collections.IVectorView`1<Windows.Security.Credentials.PasswordCredential>"
#define CRED_MAX 256
static struct obj *creds[CRED_MAX];
static int ncreds;
static CRITICAL_SECTION vault_cs;

static const WCHAR *cred_str(struct obj *o, const char *name)
{
    struct prop *p = prop_find(o, name);
    return p && p->kind == PK_HSTRING && p->v.str ? WindowsGetStringRawBuffer(p->v.str, NULL) : L"";
}

static struct obj *cred_new(const WCHAR *resource, const WCHAR *user, const WCHAR *pass)
{
    struct obj *o = obj_new_named("Windows.Security.Credentials.PasswordCredential");
    prop_set_str(o, "Resource", resource);
    prop_set_str(o, "UserName", user);
    prop_set_str(o, "Password", pass);
    return o;
}

static HRESULT WINAPI cred_create(struct tear *t, HSTRING resource, HSTRING user, HSTRING pass, void **out)
{
    shim_log("CreatePasswordCredential %ls / %ls", WindowsGetStringRawBuffer(resource, NULL), WindowsGetStringRawBuffer(user, NULL));
    *out = obj_default(cred_new(WindowsGetStringRawBuffer(resource, NULL), WindowsGetStringRawBuffer(user, NULL),
                                WindowsGetStringRawBuffer(pass, NULL)));
    return S_OK;
}

/* Windows keeps the vault across launches (games store save data in it); here it is a file in the package
 * folder: one credential per line, resource / user name / password as hex-encoded UTF-16, separated by spaces */
static void vault_path(WCHAR *path, const WCHAR *suffix)
{
    swprintf(path, MAX_PATH, L"%ls\\shim-passwordvault%ls", data_dir, suffix);
}

static void put_hex(FILE *f, const WCHAR *s)
{
    if (!*s) fputc('-', f);
    for (; *s; s++) fprintf(f, "%04x", *s);
}

/* called with vault_cs held */
static void vault_save(void)
{
    WCHAR path[MAX_PATH], tmp[MAX_PATH];
    FILE *f;
    vault_path(path, L"");
    vault_path(tmp, L".tmp");
    if (!(f = _wfopen(tmp, L"wb"))) {
        shim_log("PasswordVault: cannot write %ls", tmp);
        return;
    }
    for (int i = 0; i < ncreds; i++) {
        put_hex(f, cred_str(creds[i], "Resource"));
        fputc(' ', f);
        put_hex(f, cred_str(creds[i], "UserName"));
        fputc(' ', f);
        put_hex(f, cred_str(creds[i], "Password"));
        fputc('\n', f);
    }
    if (fclose(f) || !MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        shim_log("PasswordVault: saving %ls failed", path);
}

static WCHAR *get_hex(const char *s, const char **end)
{
    size_t n = strcspn(s, " \r\n");
    WCHAR *w = calloc(n / 4 + 1, sizeof(WCHAR));
    for (size_t i = 0; s[0] != '-' && i + 4 <= n; i += 4) {
        unsigned v;
        sscanf(s + i, "%4x", &v);
        w[i / 4] = v;
    }
    *end = s + n + (s[n] == ' ');
    return w;
}

static void vault_load(void)
{
    WCHAR path[MAX_PATH];
    FILE *f;
    static char line[1 << 20];
    vault_path(path, L"");
    if (!(f = _wfopen(path, L"rb"))) return;
    while (ncreds < CRED_MAX && fgets(line, sizeof(line), f)) {
        const char *p = line;
        WCHAR *res, *user, *pass;
        if (strlen(line) < 3) continue;
        res = get_hex(p, &p);
        user = get_hex(p, &p);
        pass = get_hex(p, &p);
        creds[ncreds++] = cred_new(res, user, pass);
        free(res);
        free(user);
        free(pass);
    }
    fclose(f);
    shim_log("PasswordVault: loaded %d credentials from %ls", ncreds, path);
}

static HRESULT WINAPI vault_add(struct tear *t, IUnknown *cred)
{
    struct obj *o = obj_from(cred);
    int i;
    if (!o) {
        shim_log("PasswordVault.Add: %p is not a shim object", cred);
        return E_INVALIDARG;
    }
    EnterCriticalSection(&vault_cs);
    for (i = 0; i < ncreds; i++)
        if (!wcscmp(cred_str(creds[i], "Resource"), cred_str(o, "Resource")) &&
            !wcscmp(cred_str(creds[i], "UserName"), cred_str(o, "UserName")))
            break;
    if (i == ncreds && ncreds < CRED_MAX) ncreds++;
    if (i < ncreds) {
        IUnknown_AddRef(cred);
        creds[i] = o;
    }
    vault_save();
    LeaveCriticalSection(&vault_cs);
    shim_log("PasswordVault.Add %ls / %ls", cred_str(o, "Resource"), cred_str(o, "UserName"));
    return S_OK;
}

static HRESULT WINAPI vault_remove(struct tear *t, IUnknown *cred)
{
    struct obj *o = obj_from(cred);
    shim_log("PasswordVault.Remove");
    EnterCriticalSection(&vault_cs);
    for (int i = 0; o && i < ncreds; i++)
        if (!wcscmp(cred_str(creds[i], "Resource"), cred_str(o, "Resource")) &&
            !wcscmp(cred_str(creds[i], "UserName"), cred_str(o, "UserName"))) {
            creds[i] = creds[--ncreds];
            vault_save();
            break;
        }
    LeaveCriticalSection(&vault_cs);
    return S_OK;
}

static HRESULT WINAPI vault_retrieve(struct tear *t, HSTRING resource, HSTRING user, void **out)
{
    HRESULT hr = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    shim_log("PasswordVault.Retrieve %ls / %ls", WindowsGetStringRawBuffer(resource, NULL), WindowsGetStringRawBuffer(user, NULL));
    *out = NULL;
    EnterCriticalSection(&vault_cs);
    for (int i = 0; i < ncreds; i++)
        if (!wcscmp(cred_str(creds[i], "Resource"), WindowsGetStringRawBuffer(resource, NULL)) &&
            !wcscmp(cred_str(creds[i], "UserName"), WindowsGetStringRawBuffer(user, NULL))) {
            *out = obj_default(creds[i]);
            IUnknown_AddRef((IUnknown *)*out);
            hr = S_OK;
            break;
        }
    LeaveCriticalSection(&vault_cs);
    return hr;
}

/* field: NULL = all, otherwise the property to compare with value */
static HRESULT vault_list(const char *field, const WCHAR *value, void **out)
{
    struct obj *view = obj_new_iface(find_iface(CRED_VIEW));
    EnterCriticalSection(&vault_cs);
    for (int i = 0; i < ncreds; i++)
        if (!field || !wcscmp(cred_str(creds[i], field), value)) coll_append(view, CRED_VIEW, obj_default(creds[i]));
    LeaveCriticalSection(&vault_cs);
    *out = obj_default(view);
    return S_OK;
}

static HRESULT WINAPI vault_all(struct tear *t, void **out)
{
    shim_log("PasswordVault.RetrieveAll (%d stored)", ncreds);
    return vault_list(NULL, NULL, out);
}
static HRESULT WINAPI vault_by_resource(struct tear *t, HSTRING r, void **out)
{
    shim_log("PasswordVault.FindAllByResource %ls", WindowsGetStringRawBuffer(r, NULL));
    return vault_list("Resource", WindowsGetStringRawBuffer(r, NULL), out);
}
static HRESULT WINAPI vault_by_user(struct tear *t, HSTRING u, void **out)
{
    shim_log("PasswordVault.FindAllByUserName %ls", WindowsGetStringRawBuffer(u, NULL));
    return vault_list("UserName", WindowsGetStringRawBuffer(u, NULL), out);
}

/* the game always runs without network: like an offline Windows machine, there is no internet profile */
static HRESULT WINAPI no_internet_profile(struct tear *t, void **out)
{
    shim_log("NetworkInformation.GetInternetConnectionProfile -> none (offline)");
    *out = NULL;
    return S_OK;
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
    override("Windows.Devices.Enumeration.Pnp.IPnpObjectStatics", "CreateFromIdAsync", pnp_create_from_id);
    override("Windows.Security.ExchangeActiveSyncProvisioning.IEasClientDeviceInformation", "get_Id", device_id);
    override("Windows.Devices.Enumeration.IDeviceAccessInformation", "get_CurrentStatus", access_allowed);
    InitializeCriticalSection(&vault_cs);
    vault_load();
    override("Windows.Security.Credentials.ICredentialFactory", "CreatePasswordCredential", cred_create);
    override("Windows.Security.Credentials.IPasswordVault", "Add", vault_add);
    override("Windows.Security.Credentials.IPasswordVault", "Remove", vault_remove);
    override("Windows.Security.Credentials.IPasswordVault", "Retrieve", vault_retrieve);
    override("Windows.Security.Credentials.IPasswordVault", "FindAllByResource", vault_by_resource);
    override("Windows.Security.Credentials.IPasswordVault", "FindAllByUserName", vault_by_user);
    override("Windows.Security.Credentials.IPasswordVault", "RetrieveAll", vault_all);
    override("Windows.Graphics.Display.IDisplayPropertiesStatics", "get_LogicalDpi", dpi);
    override("Windows.Networking.Connectivity.INetworkInformationStatics", "GetInternetConnectionProfile", no_internet_profile);
    override("Windows.Graphics.Display.IDisplayPropertiesStatics", "get_CurrentOrientation", landscape);
    override("Windows.Graphics.Display.IDisplayPropertiesStatics", "get_NativeOrientation", landscape);
    override("Windows.Graphics.Display.IDisplayPropertiesStatics", "get_ResolutionScale", scale100);
    /* the Windows 8.1+ equivalents: an unscaled 96 dpi landscape display */
    override("Windows.Graphics.Display.IDisplayInformation", "get_LogicalDpi", dpi);
    override("Windows.Graphics.Display.IDisplayInformation", "get_RawDpiX", dpi);
    override("Windows.Graphics.Display.IDisplayInformation", "get_RawDpiY", dpi);
    override("Windows.Graphics.Display.IDisplayInformation", "get_CurrentOrientation", landscape);
    override("Windows.Graphics.Display.IDisplayInformation", "get_NativeOrientation", landscape);
    override("Windows.Graphics.Display.IDisplayInformation", "get_ResolutionScale", scale100);
    override("Windows.Graphics.Display.IDisplayInformation2", "get_RawPixelsPerViewPixel", double_one);
    override("Windows.UI.Xaml.Controls.ISwapChainPanel", "get_CompositionScaleX", float_one);
    override("Windows.UI.Xaml.Controls.ISwapChainPanel", "get_CompositionScaleY", float_one);
}
