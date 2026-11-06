/* Redirect composition / CoreWindow swapchains to the shim's HWND */
#include "shim.h"
#include <dxgi1_2.h>
#include <d3d11.h>
#include <stdio.h>

static HRESULT(WINAPI *orig_for_hwnd)(IDXGIFactory2 *, IUnknown *, HWND, const DXGI_SWAP_CHAIN_DESC1 *,
                                     const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *, IDXGIOutput *, IDXGISwapChain1 **);

/* frame dumps: F12, or every SHIM_DUMP_EVERY seconds; written to SHIM_DUMP_DIR (default ..\\frames) */
static HRESULT(WINAPI *orig_present)(IDXGISwapChain *, UINT, UINT);
static HRESULT(WINAPI *orig_present1)(IDXGISwapChain1 *, UINT, UINT, const DXGI_PRESENT_PARAMETERS *);
static int dump_every, dump_count;
static DWORD next_dump;
static WCHAR dump_dir[MAX_PATH] = L"..\\frames";
volatile int dump_requested;

static void dump_frame(IDXGISwapChain *sc)
{
    ID3D11Texture2D *tex = NULL, *staging = NULL;
    ID3D11Device *dev = NULL;
    ID3D11DeviceContext *ctx = NULL;
    D3D11_TEXTURE2D_DESC desc;
    D3D11_MAPPED_SUBRESOURCE map;
    WCHAR path[MAX_PATH];
    HANDLE f;
    DWORD w;

    HRESULT hr;
    if (FAILED(hr = IDXGISwapChain_GetBuffer(sc, 0, &IID_ID3D11Texture2D, (void **)&tex))) {
        shim_log("frame dump: GetBuffer failed %08lx", hr);
        return;
    }
    ID3D11Texture2D_GetDevice(tex, &dev);
    ID3D11Device_GetImmediateContext(dev, &ctx);
    ID3D11Texture2D_GetDesc(tex, &desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.MiscFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(hr = ID3D11Device_CreateTexture2D(dev, &desc, NULL, &staging)))
        shim_log("frame dump: CreateTexture2D failed %08lx", hr);
    else {
        ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)staging, (ID3D11Resource *)tex);
        if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &map))) {
            BITMAPFILEHEADER fh = {0x4d42};
            BITMAPINFOHEADER ih = {sizeof(ih), desc.Width, -(LONG)desc.Height, 1, 32};
            int rgba = desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM || desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
            BYTE *row = malloc(desc.Width * 4);
            fh.bfOffBits = sizeof(fh) + sizeof(ih);
            fh.bfSize = fh.bfOffBits + desc.Width * desc.Height * 4;
            CreateDirectoryW(dump_dir, NULL);
            swprintf(path, MAX_PATH, L"%ls\\frame-%03d.bmp", dump_dir, dump_count++);
            f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
            WriteFile(f, &fh, sizeof(fh), &w, NULL);
            WriteFile(f, &ih, sizeof(ih), &w, NULL);
            for (UINT y = 0; y < desc.Height; y++) {
                memcpy(row, (BYTE *)map.pData + y * map.RowPitch, desc.Width * 4);
                for (UINT x = 0; x < desc.Width; x++) {
                    if (rgba) {
                        BYTE t = row[x * 4];
                        row[x * 4] = row[x * 4 + 2];
                        row[x * 4 + 2] = t;
                    }
                    row[x * 4 + 3] = 0xff;
                }
                WriteFile(f, row, desc.Width * 4, &w, NULL);
            }
            CloseHandle(f);
            free(row);
            ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)staging, 0);
            shim_log("dumped %ls", path);
        }
        ID3D11Texture2D_Release(staging);
    }
    ID3D11DeviceContext_Release(ctx);
    ID3D11Device_Release(dev);
    ID3D11Texture2D_Release(tex);
}

static void maybe_dump(IDXGISwapChain *sc)
{
    DWORD now = GetTickCount();
    static volatile LONG presents;
    LONG n = InterlockedIncrement(&presents);
    if (n == 1 || n % 600 == 0) shim_log("Present #%ld", n);
    if (dump_requested || (dump_every && (int)(now - next_dump) > 0)) {
        dump_requested = 0;
        next_dump = now + dump_every * 1000;
        dump_frame(sc);
    }
}

static HRESULT WINAPI hook_present(IDXGISwapChain *sc, UINT interval, UINT flags)
{
    maybe_dump(sc);
    return orig_present(sc, interval, flags);
}

static HRESULT WINAPI hook_present1(IDXGISwapChain1 *sc, UINT interval, UINT flags, const DXGI_PRESENT_PARAMETERS *p)
{
    maybe_dump((IDXGISwapChain *)sc);
    return orig_present1(sc, interval, flags, p);
}

static void hook_swapchain(IDXGISwapChain1 *sc)
{
    void **vt = *(void ***)sc;
    DWORD old;
    WCHAR buf[MAX_PATH];
    if (orig_present) return;
    if (GetEnvironmentVariableW(L"SHIM_DUMP_EVERY", buf, MAX_PATH)) dump_every = _wtoi(buf);
    if (GetEnvironmentVariableW(L"SHIM_DUMP_DIR", buf, MAX_PATH)) wcscpy(dump_dir, buf);
    next_dump = GetTickCount() + dump_every * 1000;
    VirtualProtect(&vt[8], 4, PAGE_READWRITE, &old);
    orig_present = vt[8];
    vt[8] = hook_present;
    VirtualProtect(&vt[8], 4, old, &old);
    VirtualProtect(&vt[22], 4, PAGE_READWRITE, &old);
    orig_present1 = vt[22];
    vt[22] = hook_present1;
    VirtualProtect(&vt[22], 4, old, &old);
}

static HRESULT make_swapchain(IDXGIFactory2 *f, IUnknown *dev, const DXGI_SWAP_CHAIN_DESC1 *desc, IDXGIOutput *out,
                              IDXGISwapChain1 **sc)
{
    DXGI_SWAP_CHAIN_DESC1 d = *desc;
    HRESULT hr;
    d.Scaling = DXGI_SCALING_STRETCH;
    d.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    hr = orig_for_hwnd(f, dev, main_hwnd, &d, NULL, out, sc);
    if (SUCCEEDED(hr)) hook_swapchain(*sc);
    shim_log("swapchain %ux%u fmt %u buffers %u effect %u -> %08lx", d.Width, d.Height, d.Format, d.BufferCount,
             d.SwapEffect, hr);
    return hr;
}

static HRESULT WINAPI hook_for_composition(IDXGIFactory2 *f, IUnknown *dev, const DXGI_SWAP_CHAIN_DESC1 *desc,
                                           IDXGIOutput *out, IDXGISwapChain1 **sc)
{
    return make_swapchain(f, dev, desc, out, sc);
}

static HRESULT WINAPI hook_for_corewindow(IDXGIFactory2 *f, IUnknown *dev, IUnknown *window,
                                          const DXGI_SWAP_CHAIN_DESC1 *desc, IDXGIOutput *out, IDXGISwapChain1 **sc)
{
    return make_swapchain(f, dev, desc, out, sc);
}

void dxgi_init(void)
{
    HMODULE dxgi = LoadLibraryA("dxgi.dll");
    HRESULT(WINAPI * create)(REFIID, void **) = (void *)GetProcAddress(dxgi, "CreateDXGIFactory1");
    IDXGIFactory2 *f = NULL;
    IDXGIFactory2Vtbl *vt;
    DWORD old;
    if (!create || FAILED(create(&IID_IDXGIFactory2, (void **)&f))) {
        shim_log("dxgi: no IDXGIFactory2");
        return;
    }
    vt = f->lpVtbl;
    VirtualProtect(vt, sizeof(*vt), PAGE_READWRITE, &old);
    orig_for_hwnd = vt->CreateSwapChainForHwnd;
    vt->CreateSwapChainForComposition = hook_for_composition;
    vt->CreateSwapChainForCoreWindow = hook_for_corewindow;
    VirtualProtect(vt, sizeof(*vt), old, &old);
    shim_log("dxgi: hooked factory vtable %p", vt);
}
