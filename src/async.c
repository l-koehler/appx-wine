/* Mocked async operations complete immediately */
#include "shim.h"

static HRESULT WINAPI put_completed(struct tear *t, IUnknown *handler)
{
    HRESULT hr;
    struct prop *st = prop_find(t->obj, "Status");
    prop_set_unk(t->obj, "Completed", handler);
    /* dispatcher work items set Status 0 until they have run */
    if (!handler || (st && !*(int *)st->v.raw)) return S_OK;
    /* AsyncStatus.Completed */
    hr = invoke2(handler, t, (void *)1);
    if (FAILED(hr)) shim_log("async completion handler for %s failed %08lx", t->obj->name, hr);
    return S_OK;
}

static HRESULT WINAPI get_status(struct tear *t, int *status)
{
    struct prop *st = prop_find(t->obj, "Status");
    *status = st ? *(int *)st->v.raw : 1;
    return S_OK;
}

static HRESULT WINAPI get_error(struct tear *t, HRESULT *hr)
{
    *hr = S_OK;
    return S_OK;
}

void async_init(void)
{
    static const char *prefixes[] = {"Windows.Foundation.IAsyncOperation`1<", "Windows.Foundation.IAsyncOperationWithProgress`2<",
                                     "Windows.Foundation.IAsyncActionWithProgress`1<"};
    for (int i = 0; i < meta_n_ifaces; i++) {
        const char *n = meta_ifaces[i].name;
        for (int p = 0; p < 3; p++)
            if (!strncmp(n, prefixes[p], strlen(prefixes[p]))) override(n, "put_Completed", put_completed);
    }
    override("Windows.Foundation.IAsyncAction", "put_Completed", put_completed);
    override("Windows.Foundation.IAsyncInfo", "get_Status", get_status);
    override("Windows.Foundation.IAsyncInfo", "get_ErrorCode", get_error);
}
