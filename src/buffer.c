/* IBuffer, hashing and CryptographicBuffer */
#include "shim.h"
#include <stdio.h>
#include <wincrypt.h>

static const GUID IID_IBufferByteAccess = {0x905a0fef, 0xbc53, 0x11df, {0x8c, 0x49, 0x00, 0x1e, 0x4f, 0xc6, 0x86, 0xda}};
static int ibuffer_idx;

struct buf {
    BYTE *data;
    UINT32 cap, len;
};

static struct buf *buf_of(struct obj *o)
{
    if (!o->priv) o->priv = calloc(1, sizeof(struct buf));
    return o->priv;
}

static HRESULT WINAPI bba_buffer(struct tear *t, BYTE **out)
{
    struct buf *b = buf_of(t->obj);
    if (!b->data) b->data = calloc(1, b->cap + 1);
    *out = b->data;
    return S_OK;
}
static void *bba_vtbl[4];

static void attach_buffer(struct obj *o)
{
    buf_of(o);
    obj_add_custom(o, &IID_IBufferByteAccess, bba_vtbl);
}

struct obj *buffer_new(const void *data, UINT32 len)
{
    struct obj *o = obj_new_named("Windows.Storage.Streams.Buffer");
    struct buf *b;
    attach_buffer(o);
    b = o->priv;
    b->cap = b->len = len;
    b->data = calloc(1, len + 1);
    if (data) memcpy(b->data, data, len);
    return o;
}

/* returns pointer and length of any IBuffer (ours or foreign) */
static BYTE *buffer_data(IUnknown *ib, UINT32 *len)
{
    struct obj *o = obj_from(ib);
    void *bba = NULL;
    BYTE *p = NULL;
    *len = 0;
    if (!ib) return NULL;
    if (o && o->priv) {
        struct buf *b = o->priv;
        *len = b->len;
        return b->data;
    }
    VCALL(ib, method_slot("Windows.Storage.Streams.IBuffer", "get_Length"), HRESULT(WINAPI *)(void *, UINT32 *))(ib, len);
    if (SUCCEEDED(IUnknown_QueryInterface(ib, &IID_IBufferByteAccess, &bba))) {
        VCALL(bba, 3, HRESULT(WINAPI *)(void *, BYTE **))(bba, &p);
        IUnknown_Release((IUnknown *)bba);
    }
    return p;
}

static HRESULT WINAPI buf_get_cap(struct tear *t, UINT32 *v)
{
    *v = buf_of(t->obj)->cap;
    return S_OK;
}

static HRESULT WINAPI buf_get_len(struct tear *t, UINT32 *v)
{
    *v = buf_of(t->obj)->len;
    return S_OK;
}

static HRESULT WINAPI buf_put_len(struct tear *t, UINT32 v)
{
    struct buf *b = buf_of(t->obj);
    if (v > b->cap) return E_INVALIDARG;
    b->len = v;
    return S_OK;
}

static HRESULT WINAPI buf_create(struct tear *t, UINT32 cap, void **out)
{
    struct obj *o = buffer_new(NULL, cap);
    ((struct buf *)o->priv)->len = 0;
    *out = obj_default(o);
    return S_OK;
}

/* ---- hashing ---- */

struct hash {
    ALG_ID alg;
    BYTE *data;
    UINT32 len;
};

static ALG_ID alg_from_name(HSTRING name)
{
    const WCHAR *n = WindowsGetStringRawBuffer(name, NULL);
    if (!wcsicmp(n, L"MD5")) return CALG_MD5;
    if (!wcsicmp(n, L"SHA1")) return CALG_SHA1;
    if (!wcsicmp(n, L"SHA256")) return CALG_SHA_256;
    if (!wcsicmp(n, L"SHA384")) return CALG_SHA_384;
    if (!wcsicmp(n, L"SHA512")) return CALG_SHA_512;
    shim_log("hash: unknown algorithm %ls", n);
    return CALG_MD5;
}

static struct obj *do_hash(ALG_ID alg, const BYTE *data, UINT32 len)
{
    HCRYPTPROV prov;
    HCRYPTHASH h;
    BYTE out[64];
    DWORD outlen = sizeof(out);
    CryptAcquireContextW(&prov, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT);
    CryptCreateHash(prov, alg, 0, 0, &h);
    CryptHashData(h, data, len, 0);
    CryptGetHashParam(h, HP_HASHVAL, out, &outlen, 0);
    CryptDestroyHash(h);
    CryptReleaseContext(prov, 0);
    return buffer_new(out, outlen);
}

static HRESULT WINAPI names_md5(struct tear *t, HSTRING *s) { return WindowsCreateString(L"MD5", 3, s); }
static HRESULT WINAPI names_sha1(struct tear *t, HSTRING *s) { return WindowsCreateString(L"SHA1", 4, s); }
static HRESULT WINAPI names_sha256(struct tear *t, HSTRING *s) { return WindowsCreateString(L"SHA256", 6, s); }

static HRESULT WINAPI hap_open(struct tear *t, HSTRING name, void **out)
{
    struct obj *o = obj_new_named("Windows.Security.Cryptography.Core.HashAlgorithmProvider");
    o->priv = (void *)(UINT_PTR)alg_from_name(name);
    prop_set_str(o, "AlgorithmName", WindowsGetStringRawBuffer(name, NULL));
    *out = obj_default(o);
    return S_OK;
}

static HRESULT WINAPI hap_hash_data(struct tear *t, IUnknown *data, void **out)
{
    UINT32 len;
    BYTE *p = buffer_data(data, &len);
    *out = obj_default(do_hash((ALG_ID)(UINT_PTR)t->obj->priv, p, len));
    return S_OK;
}

static HRESULT WINAPI hap_create_hash(struct tear *t, void **out)
{
    struct obj *o = obj_new_named("Windows.Security.Cryptography.Core.CryptographicHash");
    struct hash *h = calloc(1, sizeof(*h));
    h->alg = (ALG_ID)(UINT_PTR)t->obj->priv;
    o->priv = h;
    *out = obj_default(o);
    return S_OK;
}

static HRESULT WINAPI hash_append(struct tear *t, IUnknown *data)
{
    struct hash *h = t->obj->priv;
    UINT32 len;
    BYTE *p = buffer_data(data, &len);
    h->data = realloc(h->data, h->len + len + 1);
    memcpy(h->data + h->len, p, len);
    h->len += len;
    return S_OK;
}

static HRESULT WINAPI hash_value(struct tear *t, void **out)
{
    struct hash *h = t->obj->priv;
    *out = obj_default(do_hash(h->alg, h->data, h->len));
    h->len = 0;
    return S_OK;
}

/* ---- CryptographicBuffer ---- */

static HRESULT WINAPI cb_hex(struct tear *t, IUnknown *b, HSTRING *out)
{
    UINT32 len;
    BYTE *p = buffer_data(b, &len);
    WCHAR *s = calloc(len * 2 + 1, sizeof(WCHAR));
    for (UINT32 i = 0; i < len; i++) swprintf(s + i * 2, 3, L"%02x", p[i]);
    WindowsCreateString(s, len * 2, out);
    free(s);
    return S_OK;
}

static HRESULT WINAPI cb_base64(struct tear *t, IUnknown *b, HSTRING *out)
{
    UINT32 len;
    DWORD n = 0;
    BYTE *p = buffer_data(b, &len);
    WCHAR *s;
    CryptBinaryToStringW(p, len, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, NULL, &n);
    s = calloc(n + 1, sizeof(WCHAR));
    CryptBinaryToStringW(p, len, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, s, &n);
    WindowsCreateString(s, wcslen(s), out);
    free(s);
    return S_OK;
}

static HRESULT WINAPI cb_from_bytes(struct tear *t, UINT32 n, BYTE *data, void **out)
{
    *out = obj_default(buffer_new(data, n));
    return S_OK;
}

static HRESULT WINAPI cb_to_bytes(struct tear *t, IUnknown *b, UINT32 *n, BYTE **out)
{
    UINT32 len;
    BYTE *p = buffer_data(b, &len);
    *out = CoTaskMemAlloc(len + 1);
    memcpy(*out, p, len);
    *n = len;
    return S_OK;
}

static HRESULT WINAPI cb_from_string(struct tear *t, HSTRING s, int encoding, void **out)
{
    UINT32 wl;
    const WCHAR *w = WindowsGetStringRawBuffer(s, &wl);
    if (encoding == 0) { /* Utf8 */
        int n = WideCharToMultiByte(CP_UTF8, 0, w, wl, NULL, 0, NULL, NULL);
        struct obj *o = buffer_new(NULL, n);
        WideCharToMultiByte(CP_UTF8, 0, w, wl, (char *)((struct buf *)o->priv)->data, n, NULL, NULL);
        *out = obj_default(o);
    } else *out = obj_default(buffer_new(w, wl * 2));
    return S_OK;
}

static HRESULT WINAPI cb_to_string(struct tear *t, int encoding, IUnknown *b, HSTRING *out)
{
    UINT32 len;
    BYTE *p = buffer_data(b, &len);
    if (encoding == 0) {
        int n = MultiByteToWideChar(CP_UTF8, 0, (char *)p, len, NULL, 0);
        WCHAR *w = calloc(n + 1, sizeof(WCHAR));
        MultiByteToWideChar(CP_UTF8, 0, (char *)p, len, w, n);
        WindowsCreateString(w, n, out);
        free(w);
    } else WindowsCreateString((WCHAR *)p, len / 2, out);
    return S_OK;
}

static HRESULT WINAPI cb_random(struct tear *t, UINT32 n, void **out)
{
    struct obj *o = buffer_new(NULL, n);
    for (UINT32 i = 0; i < n; i++) ((struct buf *)o->priv)->data[i] = rand();
    *out = obj_default(o);
    return S_OK;
}

static HRESULT WINAPI cb_random_number(struct tear *t, UINT32 *v)
{
    *v = ((UINT32)rand() << 16) ^ rand();
    return S_OK;
}

static HRESULT WINAPI hwtoken_id(struct tear *t, void **out)
{
    static const BYTE id[16] = "wine-hardware-id";
    *out = obj_default(buffer_new(id, sizeof(id)));
    return S_OK;
}

static void on_auto(struct obj *o)
{
    if (o->iface == ibuffer_idx) attach_buffer(o);
}

void buffer_init(void)
{
    ibuffer_idx = find_iface("Windows.Storage.Streams.IBuffer");
    memcpy(bba_vtbl, *(void ***)obj_default(obj_new_iface(ibuffer_idx)), 3 * sizeof(void *));
    bba_vtbl[3] = bba_buffer;
    auto_hook = on_auto;
    override("Windows.Storage.Streams.IBuffer", "get_Capacity", buf_get_cap);
    override("Windows.Storage.Streams.IBuffer", "get_Length", buf_get_len);
    override("Windows.Storage.Streams.IBuffer", "put_Length", buf_put_len);
    override("Windows.Storage.Streams.IBufferFactory", "Create", buf_create);
    override("Windows.Security.Cryptography.Core.IHashAlgorithmNamesStatics", "get_Md5", names_md5);
    override("Windows.Security.Cryptography.Core.IHashAlgorithmNamesStatics", "get_Sha1", names_sha1);
    override("Windows.Security.Cryptography.Core.IHashAlgorithmNamesStatics", "get_Sha256", names_sha256);
    override("Windows.Security.Cryptography.Core.IHashAlgorithmProviderStatics", "OpenAlgorithm", hap_open);
    override("Windows.Security.Cryptography.Core.IHashAlgorithmProvider", "HashData", hap_hash_data);
    override("Windows.Security.Cryptography.Core.IHashAlgorithmProvider", "CreateHash", hap_create_hash);
    override("Windows.Security.Cryptography.Core.IHashComputation", "Append", hash_append);
    override("Windows.Security.Cryptography.Core.IHashComputation", "GetValueAndReset", hash_value);
    override("Windows.Security.Cryptography.ICryptographicBufferStatics", "EncodeToHexString", cb_hex);
    override("Windows.Security.Cryptography.ICryptographicBufferStatics", "EncodeToBase64String", cb_base64);
    override("Windows.Security.Cryptography.ICryptographicBufferStatics", "CreateFromByteArray", cb_from_bytes);
    override("Windows.Security.Cryptography.ICryptographicBufferStatics", "CopyToByteArray", cb_to_bytes);
    override("Windows.Security.Cryptography.ICryptographicBufferStatics", "ConvertStringToBinary", cb_from_string);
    override("Windows.Security.Cryptography.ICryptographicBufferStatics", "ConvertBinaryToString", cb_to_string);
    override("Windows.Security.Cryptography.ICryptographicBufferStatics", "GenerateRandom", cb_random);
    override("Windows.Security.Cryptography.ICryptographicBufferStatics", "GenerateRandomNumber", cb_random_number);
    override("Windows.System.Profile.IHardwareToken", "get_Id", hwtoken_id);
}
