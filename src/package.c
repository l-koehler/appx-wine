/* Package identity from AppxManifest.xml next to the executable */
#include "shim.h"
#include <stdio.h>
#include <wincrypt.h>

struct package package;

static char *read_manifest(void)
{
    WCHAR path[MAX_PATH];
    HANDLE f;
    DWORD size, rd;
    char *buf;
    swprintf(path, MAX_PATH, L"%ls\\AppxManifest.xml", package.dir);
    f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return NULL;
    size = GetFileSize(f, NULL);
    buf = calloc(1, size + 1);
    ReadFile(f, buf, size, &rd, NULL);
    CloseHandle(f);
    return buf;
}

/* value of attribute `name` inside the first <tag ...> element */
static void xml_attr(const char *xml, const char *tag, const char *name, WCHAR *out, int outlen)
{
    char open[64], key[64];
    const char *p, *end, *v, *e;
    snprintf(open, sizeof(open), "<%s ", tag);
    snprintf(key, sizeof(key), " %s=\"", name);
    if (!(p = strstr(xml, open)) || !(end = strchr(p, '>'))) return;
    for (v = strstr(p, key); v && v < end; v = NULL) {
        v += strlen(key);
        e = strchr(v, '"');
        MultiByteToWideChar(CP_UTF8, 0, v, e - v, out, outlen - 1);
        out[min(e - v, outlen - 1)] = 0;
    }
}

/* text content of the first <tag> element */
static void xml_text(const char *xml, const char *tag, WCHAR *out, int outlen)
{
    char open[64], close[64];
    const char *p, *e;
    snprintf(open, sizeof(open), "<%s>", tag);
    snprintf(close, sizeof(close), "</%s>", tag);
    if (!(p = strstr(xml, open)) || !(e = strstr(p, close))) return;
    p += strlen(open);
    MultiByteToWideChar(CP_UTF8, 0, p, e - p, out, outlen - 1);
    out[min(e - p, outlen - 1)] = 0;
}

/* PublisherId: Crockford base32 of the first 8 bytes of SHA-256(UTF-16LE publisher) */
static void publisher_id(const WCHAR *publisher, WCHAR *out)
{
    static const char alphabet[] = "0123456789abcdefghjkmnpqrstvwxyz";
    HCRYPTPROV prov;
    HCRYPTHASH h;
    BYTE hash[32];
    DWORD len = sizeof(hash);
    UINT64 bits = 0;
    CryptAcquireContextW(&prov, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT);
    CryptCreateHash(prov, CALG_SHA_256, 0, 0, &h);
    CryptHashData(h, (const BYTE *)publisher, wcslen(publisher) * sizeof(WCHAR), 0);
    CryptGetHashParam(h, HP_HASHVAL, hash, &len, 0);
    CryptDestroyHash(h);
    CryptReleaseContext(prov, 0);
    for (int i = 0; i < 8; i++) bits = bits << 8 | hash[i];
    /* 64 bits padded with one zero bit -> 13 five-bit groups */
    for (int i = 0; i < 13; i++) {
        int shift = 59 - 5 * i;
        out[i] = alphabet[(shift >= 0 ? bits >> shift : bits << -shift) & 31];
    }
    out[13] = 0;
}

void package_init(void)
{
    char *xml;
    WCHAR arch[16] = L"neutral", ver[32] = L"1.0.0.0";
    GetModuleFileNameW(NULL, package.dir, MAX_PATH);
    *wcsrchr(package.dir, '\\') = 0;
    if (!(xml = read_manifest())) {
        shim_log("no AppxManifest.xml in %ls", package.dir);
        wcscpy(package.name, L"UnknownPackage");
        wcscpy(package.display_name, L"UnknownPackage");
    } else {
        xml_attr(xml, "Identity", "Name", package.name, 128);
        xml_attr(xml, "Identity", "Publisher", package.publisher, 256);
        xml_attr(xml, "Identity", "Version", ver, 32);
        xml_attr(xml, "Identity", "ProcessorArchitecture", arch, 16);
        xml_text(xml, "DisplayName", package.display_name, 128);
        free(xml);
    }
    /* ms-resource: names would need resources.pri */
    if (!package.display_name[0] || !wcsncmp(package.display_name, L"ms-resource:", 12))
        wcscpy(package.display_name, package.name);
    swscanf(ver, L"%hu.%hu.%hu.%hu", &package.version[0], &package.version[1], &package.version[2], &package.version[3]);
    package.arch = !wcscmp(arch, L"x86") ? 0 : !wcscmp(arch, L"arm") ? 5 : !wcscmp(arch, L"x64") ? 9 : 11;
    publisher_id(package.publisher, package.publisher_id);
    swprintf(package.family_name, 256, L"%ls_%ls", package.name, package.publisher_id);
    swprintf(package.full_name, 256, L"%ls_%ls_%ls__%ls", package.name, ver, arch, package.publisher_id);
    shim_log("package %ls (%ls)", package.full_name, package.display_name);
}
