/* Mocked async operations complete immediately */
#include "shim.h"

static HRESULT WINAPI put_completed(struct tear *t, IUnknown *handler)
{
    HRESULT hr;
    struct prop *st = prop_find(t->obj, "Status");
    prop_set_unk(t->obj, "Completed", handler);
    /* dispatcher work items set Status 0 until they have run */
    if (!handler || (st && !*(int *)st->v.raw)) return S_OK;
    /* AsyncStatus.Completed = 1, Error = 3 */
    shim_log("async %s: completion handler, status %d", t->obj->name, st ? *(int *)st->v.raw : 1);
    hr = invoke2(handler, t, (void *)(st ? (UINT_PTR) * (int *)st->v.raw : 1));
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
    struct prop *p = prop_find(t->obj, "ErrorCode");
    *hr = p ? *(HRESULT *)p->v.raw : S_OK;
    return S_OK;
}

/* an operation that has already failed with hr */
static struct obj *failed_op(const char *iface, HRESULT hr)
{
    struct obj *op = obj_new_iface(find_iface(iface));
    prop_set_raw(op, "Status", &(int){3}, 4);
    prop_set_raw(op, "ErrorCode", &hr, 4);
    return op;
}

static HRESULT WINAPI failed_get_results(struct tear *t, void **out)
{
    struct prop *p = prop_find(t->obj, "ErrorCode");
    *out = NULL;
    return p ? *(HRESULT *)p->v.raw : S_OK;
}

#define ENDPOINT_PAIRS_OP \
    "Windows.Foundation.IAsyncOperation`1<Windows.Foundation.Collections.IVectorView`1<Windows.Networking.EndpointPair>>"
#define HRESULT_HOST_NOT_FOUND ((HRESULT)0x80072AF9)

/* there is no network: name resolution fails like on a machine that is offline */
static HRESULT WINAPI endpoint_pairs(struct tear *t, void *host, HSTRING service, void **op)
{
    void *ihost = NULL;
    HSTRING raw = NULL;
    if (host && SUCCEEDED(IUnknown_QueryInterface((IUnknown *)host, &meta_ifaces[find_iface("Windows.Networking.IHostName")].iid, &ihost))) {
        VCALL(ihost, method_slot("Windows.Networking.IHostName", "get_RawName"), HRESULT(WINAPI *)(void *, HSTRING *))(ihost, &raw);
        IUnknown_Release((IUnknown *)ihost);
    }
    shim_log("GetEndpointPairsAsync host %ls service %ls", raw ? WindowsGetStringRawBuffer(raw, NULL) : L"?",
             service ? WindowsGetStringRawBuffer(service, NULL) : L"");
    *op = obj_default(failed_op(ENDPOINT_PAIRS_OP, HRESULT_HOST_NOT_FOUND));
    return S_OK;
}

static HRESULT WINAPI endpoint_pairs_sorted(struct tear *t, void *host, HSTRING service, int sort, void **op)
{
    return endpoint_pairs(t, host, service, op);
}

#define PUSH_CHANNEL_OP "Windows.Foundation.IAsyncOperation`1<Windows.Networking.PushNotifications.PushNotificationChannel>"
#define HRESULT_NETWORK_UNREACHABLE ((HRESULT)0x800704CF)

/* push channels need the notification service, which is not reachable offline */
static HRESULT WINAPI push_channel(struct tear *t, void **op)
{
    shim_log("CreatePushNotificationChannelForApplicationAsync: failing (offline)");
    *op = obj_default(failed_op(PUSH_CHANNEL_OP, HRESULT_NETWORK_UNREACHABLE));
    return S_OK;
}

static HRESULT WINAPI push_channel_id(struct tear *t, HSTRING id, void **op)
{
    return push_channel(t, op);
}

/* ---- ThreadPoolTimer ---- */

struct pool_timer {
    HANDLE handle;
    IUnknown *handler, *self;
    IUnknown *destroyed;  /* TimerDestroyedHandler, claimed by whoever runs it */
    int periodic;
};

static void run_destroyed(struct pool_timer *pt)
{
    IUnknown *d = InterlockedExchangePointer((void **)&pt->destroyed, NULL);
    if (!d) return;
    shim_log("ThreadPoolTimer destroyed (handler %p)", d);
    VCALL(d, 3, HRESULT(WINAPI *)(void *, void *))(d, pt->self);
    IUnknown_Release(d);
}

static void CALLBACK timer_fired(void *arg, BOOLEAN timed_out)
{
    struct pool_timer *pt = arg;
    static volatile LONG n;
    if (InterlockedIncrement(&n) <= 20) shim_log("ThreadPoolTimer fired (handler %p)", pt->handler);
    VCALL(pt->handler, 3, HRESULT(WINAPI *)(void *, void *))(pt->handler, pt->self);
    if (!pt->periodic) run_destroyed(pt);
}

static HRESULT make_timer(IUnknown *handler, INT64 ticks, int periodic, IUnknown *destroyed, void **out)
{
    struct obj *o = obj_new_named("Windows.System.Threading.ThreadPoolTimer");
    struct pool_timer *pt = calloc(1, sizeof(*pt));
    DWORD ms = ticks / 10000 ? ticks / 10000 : 1;
    shim_log("ThreadPoolTimer %s %lu ms%s", periodic ? "periodic" : "once", ms, destroyed ? " (with destroyed handler)" : "");
    prop_set_raw(o, periodic ? "Period" : "Delay", &ticks, 8);
    IUnknown_AddRef(handler);
    pt->handler = handler;
    if (destroyed) IUnknown_AddRef(destroyed);
    pt->destroyed = destroyed;
    pt->periodic = periodic;
    pt->self = (IUnknown *)obj_default(o);
    o->priv = pt;
    *out = obj_default(o);
    IUnknown_AddRef((IUnknown *)*out);
    CreateTimerQueueTimer(&pt->handle, NULL, timer_fired, pt, ms, periodic ? ms : 0, WT_EXECUTEDEFAULT);
    return S_OK;
}

static HRESULT WINAPI timer_create_periodic(struct tear *t, IUnknown *h, INT64 period, void **out)
{
    return make_timer(h, period, 1, NULL, out);
}

static HRESULT WINAPI timer_create_once(struct tear *t, IUnknown *h, INT64 delay, void **out)
{
    return make_timer(h, delay, 0, NULL, out);
}

static HRESULT WINAPI timer_create_periodic2(struct tear *t, IUnknown *h, INT64 period, IUnknown *destroyed, void **out)
{
    return make_timer(h, period, 1, destroyed, out);
}

static HRESULT WINAPI timer_create_once2(struct tear *t, IUnknown *h, INT64 delay, IUnknown *destroyed, void **out)
{
    return make_timer(h, delay, 0, destroyed, out);
}

struct timer_delete {
    struct pool_timer *pt;
    HANDLE done;
};

/* the destroyed handler runs once no timer callback is in flight any more */
static DWORD WINAPI timer_deleted(void *arg)
{
    struct timer_delete *td = arg;
    WaitForSingleObject(td->done, INFINITE);
    CloseHandle(td->done);
    run_destroyed(td->pt);
    free(td);
    return 0;
}

static HRESULT WINAPI timer_cancel(struct tear *t)
{
    struct pool_timer *pt = t->obj->priv;
    struct timer_delete *td;
    HANDLE h;
    if (!pt || !(h = InterlockedExchangePointer(&pt->handle, NULL))) return S_OK;
    shim_log("ThreadPoolTimer cancel (handler %p)", pt->handler);
    if (!pt->destroyed) {
        DeleteTimerQueueTimer(NULL, h, NULL);
        return S_OK;
    }
    /* Cancel may be called from the timer's own callback, so do not block here */
    td = malloc(sizeof(*td));
    td->pt = pt;
    td->done = CreateEventW(NULL, TRUE, FALSE, NULL);
    DeleteTimerQueueTimer(NULL, h, td->done);
    QueueUserWorkItem(timer_deleted, td, WT_EXECUTEDEFAULT);
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
    override("Windows.System.Threading.IThreadPoolTimerStatics", "CreatePeriodicTimer", timer_create_periodic);
    override("Windows.System.Threading.IThreadPoolTimerStatics", "CreateTimer", timer_create_once);
    override("Windows.System.Threading.IThreadPoolTimerStatics", "CreatePeriodicTimer#2", timer_create_periodic2);
    override("Windows.System.Threading.IThreadPoolTimerStatics", "CreateTimer#2", timer_create_once2);
    override("Windows.System.Threading.IThreadPoolTimer", "Cancel", timer_cancel);
    override(ENDPOINT_PAIRS_OP, "GetResults", failed_get_results);
    override(PUSH_CHANNEL_OP, "GetResults", failed_get_results);
    override("Windows.Networking.PushNotifications.IPushNotificationChannelManagerStatics",
             "CreatePushNotificationChannelForApplicationAsync", push_channel);
    override("Windows.Networking.PushNotifications.IPushNotificationChannelManagerStatics",
             "CreatePushNotificationChannelForApplicationAsync#2", push_channel_id);
    override("Windows.Networking.PushNotifications.IPushNotificationChannelManagerStatics",
             "CreatePushNotificationChannelForSecondaryTileAsync", push_channel_id);
    override("Windows.Networking.Sockets.IDatagramSocketStatics", "GetEndpointPairsAsync", endpoint_pairs);
    override("Windows.Networking.Sockets.IDatagramSocketStatics", "GetEndpointPairsAsync#2", endpoint_pairs_sorted);
}
