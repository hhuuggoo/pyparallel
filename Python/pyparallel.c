#include "Python.h"

#ifdef __cpplus
extern "C" {
#endif

#include <fcntl.h>

#include "pyparallel_private.h"
#include "fileio.h"
#include "frameobject.h"
#include "structmember.h"

/* XXX TODO:
 *      - Either investigate why DisconnectEx/TF_REUSE seems to suck or just
 *        drop it altogether (currently dropped).
 *      - Finish inlining exception/error handlers in IOLoop.
 *      - Implement cleanup routines and bind to thread destroy.
 *      - Finish IOLoop logic.
 */

#define CS_SOCK_SPINCOUNT 4

Py_CACHE_ALIGN
Py_TLS Context *ctx = NULL;
Py_TLS TLS tls;
Py_TLS PyThreadState *TSTATE;
Py_TLS HANDLE heap_override;
Py_TLS void *last_heap_override_malloc_addr;
Py_TLS void *last_context_heap_malloc_addr;


Py_TLS static int _PxNewThread = 1;

Py_CACHE_ALIGN
long Py_MainThreadId  = -1;
long Py_MainProcessId = -1;
long Py_ParallelContextsEnabled = -1;
size_t _PxObjectSignature = -1;
size_t _PxSocketSignature = -1;
size_t _PxSocketBufSignature = -1;
int _PxBlockingCallsThreshold = 20;

int _Py_CtrlCPressed = 0;
int _Py_InstalledCtrlCHandler = 0;

int _PyParallel_Finalized = 0;

int _PxSocketServer_PreallocatedSockets = 64;
int _PxSocket_MaxSyncSendAttempts = 3;
int _PxSocket_MaxSyncRecvAttempts = 3;
int _PxSocket_MaxRecvBufSize = 65536;
int _PxSocket_CriticalSectionSpinCount = CS_SOCK_SPINCOUNT;
int _PxSocket_ListenBacklog = SOMAXCONN;

int _PyTLSHeap_DefaultSize = Px_DEFAULT_TLS_HEAP_SIZE;

int _PxSocket_SendListSize = 30;

volatile int _PxSocket_ActiveHogs = 0;
volatile int _PxSocket_ActiveIOLoops = 0;
int _PyParallel_NumCPUs = 0;

static PyObject *PyExc_AsyncError;
static PyObject *PyExc_ProtectionError;
static PyObject *PyExc_UnprotectedError;
static PyObject *PyExc_AssignmentError;
static PyObject *PyExc_NoWaitersError;
static PyObject *PyExc_WaitError;
static PyObject *PyExc_WaitTimeoutError;
static PyObject *PyExc_AsyncIOBuffersExhaustedError;
static PyObject *PyExc_PersistenceError;
static PyObject *PyExc_LowMemory;
static PyObject *PyExc_SoftMemoryLoadLimitHit;
static PyObject *PyExc_HardMemoryLoadLimitHit;

Py_CACHE_ALIGN
/* Point at which socket servers will start aggressively disconnecting
   idle clients in order to free up more sockets. */
static short _PyParallel_SoftMemoryLoadLimit = 75;
/* Point at which no more accepts/connects can be posted. */
static short _PyParallel_HardMemoryLoadLimit = 90;
static short _PyParallel_CurrentMemoryLoad;

static BOOL _PyParallel_LowMemory = FALSE;

static MEMORYSTATUSEX _memory_status;

/* 0 on error */
static
char
_PyParallel_RefreshMemoryLoad(void)
{
    static HANDLE low_mem;
    MEMORYSTATUSEX *ms;
    Py_GUARD

    ms = &_memory_status;

    if (!low_mem)
        low_mem = CreateMemoryResourceNotification(
            LowMemoryResourceNotification
        );

    if (!QueryMemoryResourceNotification(low_mem, &_PyParallel_LowMemory)) {
        PyErr_SetFromWindowsErr(0);
        return 0;
    }

    ms->dwLength = sizeof(_memory_status);
    if (!GlobalMemoryStatusEx(ms)) {
        PyErr_SetFromWindowsErr(0);
        return 0;
    }

    _PyParallel_CurrentMemoryLoad = (short)ms->dwMemoryLoad;

    return 1;
}

static
int
_PyParallel_HitSoftMemoryLimit(void)
{
    return (
        _PyParallel_CurrentMemoryLoad >= _PyParallel_SoftMemoryLoadLimit &&
        _PyParallel_CurrentMemoryLoad <  _PyParallel_HardMemoryLoadLimit
    );
}

static
int
_PyParallel_HitHardMemoryLimit(void)
{
    return _PyParallel_CurrentMemoryLoad >= _PyParallel_HardMemoryLoadLimit;
}

void *_PyHeap_Malloc(Context *c, size_t n, size_t align, int no_realloc);
void *_PyTLSHeap_Malloc(size_t n, size_t align);

static
char
_PyParallel_IsHeapOverrideActive(void)
{
    return (heap_override != NULL);
}

static
void
_PyParallel_SetHeapOverride(HANDLE heap_handle)
{
    assert(!_PyParallel_IsHeapOverrideActive());
    heap_override = heap_handle;
}

static
HANDLE
_PyParallel_GetHeapOverride(void)
{
    return heap_override;
}


static
void
_PyParallel_RemoveHeapOverride(void)
{
    assert(_PyParallel_IsHeapOverrideActive());
    heap_override = NULL;
}

/* tls-oriented stuff */
int
_PyParallel_DoesContextHaveActiveHeapSnapshot(void)
{
    return (!Py_PXCTX ? 0 : (ctx->snapshot_id == 0));
}

#define Px_TLS_HEAP_ACTIVE (tls.heap_depth > 0)

int
_PyParallel_IsTLSHeapActive(void)
{
    return (!Py_PXCTX ? 0 : Px_TLS_HEAP_ACTIVE);
}

int
_PyParallel_GetTLSHeapDepth(void)
{
    return (!Py_PXCTX ? 0 : tls.heap_depth);
}

void
_PyParallel_EnableTLSHeap(void)
{
    TLS     *t = &tls;
    Context *c = ctx;

    if (!Py_PXCTX)
        return;

    if (++t->heap_depth > 1)
        /* Heap already active. */
        return;

    if (t->heap_depth < 0)
        Py_FatalError("PyParallel_EnableTLSHeap(): heap depth overflow");

    assert(t->heap_depth == 1);
    assert(!t->ctx_heap);
    assert(c->h != t->h);

    t->ctx_heap = c->h;
    c->h = t->h;
}

void
_PyParallel_DisableTLSHeap(void)
{
    TLS     *t = &tls;
    Context *c = ctx;

    if (!Py_PXCTX)
        return;

    if (--t->heap_depth > 0)
        return;

    if (t->heap_depth < 0)
        Py_FatalError("PyParallel_DisableTLSHeap: negative heap depth");

    assert(t->heap_depth == 0);
    assert(t->ctx_heap);
    assert(c->h == t->h);
    assert(c->h != t->ctx_heap);

    c->h = t->ctx_heap;
    t->ctx_heap = NULL;
}

#define _TMPBUF_SIZE 1024

#ifdef _WIN64
#define _tls_bitscan_fwd        _BitScanForward64
#define _tls_bitscan_rev        _BitScanReverse64
#define _tls_interlocked_or     _InterlockedOr64
#define _tls_interlocked_and    _InterlockedAnd64
#define _tls_popcnt             _Py_popcnt_u64
#else
#define _tls_bitscan_fwd        _BitScanForward
#define _tls_bitscan_rev        _BitScanReverse
#define _tls_interlocked_or     _InterlockedOr
#define _tls_interlocked_and    _InterlockedAnd
#define _tls_popcnt             _Py_popcnt_u32
#endif

static
PyThreadState *
get_main_thread_state(void)
{
    PyThreadState *tstate;

    tstate = (PyThreadState *)_Py_atomic_load_relaxed(&_PyThreadState_Current);
    if (!tstate)
        tstate = TSTATE;
    //assert(tstate);
    return tstate;
}

PyThreadState *
_PyParallel_GetCurrentThreadState(void)
{
    return get_main_thread_state();
}

static
PyThreadState *
get_main_thread_state_old(void)
{
    PyThreadState *tstate;

    tstate = (PyThreadState *)_Py_atomic_load_relaxed(&_PyThreadState_Current);
    if (!tstate) {
        _Py_lfence();
        _Py_clflush(&_PyThreadState_Current);
        tstate = (PyThreadState *)_PyThreadState_Current._value;
    }
    if (!tstate) {
        OutputDebugString(L"get_main_thread_state(2): !tstate\n");
        tstate = PyGILState_GetThisThreadState();
        if (!tstate) {
            OutputDebugString(L"get_main_thread_state(3): !tstate\n");

            if (Py_PXCTX) {
                PyThreadState *ts1, *ts2;
                ts1 = tls.px->tstate;
                ts2 = ctx->px->tstate;
                assert(ts1 == ts2);
                tstate = ts1;
                _PyThreadState_Current._value = tstate;
            }
        }
    }
    if (!tstate) {
        OutputDebugString(L"get_main_thread_state(4): !tstate!\n");
        assert(TSTATE);
        tstate = TSTATE;
    }
    /*
    assert(tstate);
    */
    return tstate;
    /*
    return (PyThreadState *)_Py_atomic_load_relaxed(&_PyThreadState_Current);
    */
}


static
PxState *
PXSTATE(void)
{
    PxState *px;
    PyThreadState *pstate = get_main_thread_state();
    if (!pstate) {
        OutputDebugString(L"_PyThreadState_Current == NULL!\n");
        if (Py_PXCTX)
            px = ctx->px;
    } else
        px = (PxState *)pstate->px;
    if (!px) {
        OutputDebugString(L"!px");
    }
    if ((Px_PTR(px) & 0x0000000800000000) == 0x0000000800000000) {
        OutputDebugString(L"px -> 0x8..\n");
    }
    /*
    assert(px);
    assert((Px_PTR(px) & 0x0000000800000000) != 0x0000000800000000);
    */
    return px;
}

Heap *
PxContext_HeapSnapshot(Context *c, Heap *prev)
{
    Heap *h = NULL;
    Px_UINTPTR bitmap = Px_PTR(c->snapshots_bitmap);
    unsigned long i = 0;

    assert(!prev || (prev->ctx == c));

    EnterCriticalSection(&c->heap_cs);
    EnterCriticalSection(&c->snapshots_cs);

    if (_tls_bitscan_fwd(&i, bitmap))
        h = c->snapshots[i];

    if (h) {
        assert(h->bitmap_index == i);
        _tls_interlocked_and(&c->snapshots_bitmap, ~(Px_UINTPTR_1 << i));
    }

    LeaveCriticalSection(&c->snapshots_cs);

    if (!h)
        Py_FatalError("Context heap snapshots exhausted!");

    memcpy(h, c->h, PxHeap_SNAPSHOT_COPY_SIZE);
    h->snapshot_id = ++c->snapshot_id;
    if (prev) {
        h->sle_prev = prev;
        h->sle_next = NULL;
        prev->sle_next = h;
    }
    h->sle_next = NULL;

    LeaveCriticalSection(&c->heap_cs);

    return h;
}

void
PxContext_RollbackHeap(Context *c, Heap **snapshot)
{
    Heap *h1, *h2;
    void *tstart, *hstart, *next;
    Px_UINTPTR bitmap = 0;
    size_t size;
    size_t bitmap_popcount;

    h1 = *snapshot;
    assert(h1->ctx == c);
    assert(ctx == c);

    EnterCriticalSection(&c->heap_cs);
    EnterCriticalSection(&c->snapshots_cs);

    h2 = h1->sle_next;

    if (!h2) {
        unsigned long i = 0;
        bitmap = Px_PTR(c->snapshots_bitmap);
        if (_tls_bitscan_fwd(&i, bitmap)) {
            if (i == h1->bitmap_index+1)
                goto rollback;
            else if (!i) {
                /* I think this can only happen if a heap snapshot hasn't been
                 * taken yet. */
                __debugbreak();
            } else
                __debugbreak();
        } else
            __debugbreak();

        /* You know what... I admit it... I have no idea what this code is
         * meant to be doing on any given debugging session. */
        __debugbreak();
        //assert(0);
        //h2 = PxContext_HeapSnapshot(c, h1);
    }

    assert(h2);
    assert(!h2->sle_next);
    assert(h2->sle_prev == h1);
    assert(h1->sle_next == h2);

    assert(h1->ctx == h2->ctx);

    if (h1->snapshot_id == h2->snapshot_id-1)
        goto rollback;

    bitmap_popcount = _tls_popcnt(~c->snapshots_bitmap);

    /* Should this be changed to 3 now that we're taking snapshots as soon as
     * a context is created? */
    if (bitmap_popcount == 2)
        goto rollback;
    else
        /* Do we ever have more than 2 snapshots?  (Or 3 as the case may be if
         * the above comment holds.) */
        __debugbreak();

    if (h1->id == h2->id && h1->id == c->h->id) {
        if (h2->allocated == c->h->allocated)
            goto rollback;
        else
            __debugbreak();
    } else
        __debugbreak();

    /* xxx todo */
    __debugbreak();
    assert(0);

rollback:
    /* xxx todo: HeapFree() extra heaps if h1->id != h2->id. */

    next = (h2 ? h2->next : c->h->next);
    size = _Py_PTR(next) - _Py_PTR(h1->next);
    if (!size)
        /* This happens when we're rolling back to the first snapshot. */
        goto post_rollback;
    assert(size < Px_LARGE_PAGE_SIZE);
    memset(h1->next, 0, size);

    /* skip sle_prev and sle_next */
    tstart = _Py_CAST_FWD(c->h, void *, Heap, base);
    hstart = _Py_CAST_FWD(h1,   void *, Heap, base);
    size = PxHeap_SNAPSHOT_COPY_SIZE - _Py_PTR_SUB(tstart, c->h);
    memcpy(tstart, hstart, size);

post_rollback:

    bitmap_popcount = _tls_popcnt(~c->snapshots_bitmap);

    /* Don't clear/reset anything if we're the first snapshot. */
    if (*snapshot == &c->snapshot[0]) {
        /* First snapshot! */
        if (bitmap_popcount != 1)
            /* So the popcount should be one. */
            __debugbreak();

        goto end;
    }

    if (bitmap_popcount == 1) {
        /* If this is 1, what is *snapshot pointing to if not the first
         * snapshot? */
        __debugbreak();
    }

    if (snapshot == &c->snapshots[bitmap_popcount]) {
        /* Also, er, don't clear the pointer if it's pointing to the context's
         * actual snapshot pointer array. */
        __debugbreak();
        goto end;
    }

    /* Clear this bit in the bitmap and the corresponding snapshot pointer,
     * which completes the "rollback". */
    bitmap = (Px_UINTPTR_1 << h1->bitmap_index);
    if (h2)
        bitmap |= (Px_UINTPTR_1 << h2->bitmap_index);

    _tls_interlocked_or(&c->snapshots_bitmap, bitmap);

    *snapshot = NULL;

end:
    LeaveCriticalSection(&c->snapshots_cs);
    LeaveCriticalSection(&c->heap_cs);

    return;
}

void
PxContext_ResetHeapToFirstSnapshot(Context *c)
{
    size_t bitmap_popcount;

    while ((bitmap_popcount = _tls_popcnt(~c->snapshots_bitmap)) > 1)
        PxContext_RollbackHeap(c, &c->snapshots[bitmap_popcount]);

    /* That'll roll us back to the first snapshot, which we have to roll back
     * outside of the loop as the bitmap won't be affected by the rollback
     * because it's the first snapshot.
     */

    /* (I probably could have worded that a bit better.) */
    PxContext_RollbackHeap(c, &c->snapshots[0]);
}

PyObject *
create_pxsocket(
    PyObject *args,
    PyObject *kwds,
    int flags,
    PxSocket *parent,
    Context *use_this_context
);

/* Called from PxSocket_IOLoop when a socket has encountered an error and
 * needs to close.  It will close the socket and associated resources,
 * rollback the context to the first snapshot, and create a new socket
 * based off the details of the closed one.
 */
void
PxSocket_Recycle(PxSocket **sp)
{
    PxSocket *s = *sp;
    Context *c = s->ctx;
    PxSocket *parent = s->parent;
    TP_IO *tp_io = s->tp_io;
    int flags;

    OutputDebugString(L"recycling...\n");

    if (PxSocket_IS_SERVERCLIENT(s))
        flags = Px_SOCKFLAGS_SERVERCLIENT;
    else if (PxSocket_IS_CLIENT(s))
        flags = Px_SOCKFLAGS_CLIENT;
    else if (PxSocket_IS_SERVER(s))
        flags = Px_SOCKFLAGS_SERVER;
    else
        assert(0);

    if ((PxSocket *)c->io_obj != s)
        __debugbreak();

    if (s->tp_io) {
        //CancelThreadpoolIo(s->tp_io);
        CloseThreadpoolIo(s->tp_io);
        s->tp_io = NULL;
    }
    if (s->sock_fd != INVALID_SOCKET) {
        int retval = closesocket(s->sock_fd);
        s->sock_fd = INVALID_SOCKET;
    }

    if (s->rbuf && s->rbuf->snapshot)
        PxContext_RollbackHeap(c, &s->rbuf->snapshot);

    PxContext_ResetHeapToFirstSnapshot(c);

    s = (PxSocket *)create_pxsocket(NULL, NULL, flags, parent, c);
    if (!s)
        /* Eh, what should we do here? */
        __debugbreak();

    *sp = s;

    ++c->times_recycled;

    return;
}

#define TLS_BUF_SPINCOUNT 8

/* 0 on failure, 1 on success */
static
int
PyObject2WSABUF(PyObject *o, WSABUF *w)
{
    int result = 1;
    if (PyBytes_Check(o)) {
        w->len = (ULONG)((PyVarObject *)o)->ob_size;
        w->buf = (char *)((PyBytesObject *)o)->ob_sval;
    } else if (PyByteArray_Check(o)) {
        w->len = (ULONG)((PyByteArrayObject *)o)->ob_alloc;
        w->buf = ((PyByteArrayObject *)o)->ob_bytes;
    } else if (PyUnicode_Check(o)) {
        Py_ssize_t nbytes;
        char *buf = PyUnicode_AsUTF8AndSize(o, &nbytes);
        if (buf) {
            w->buf = buf;
            /* xxx todo: range check */
            w->len = (DWORD)nbytes;
        } else
            result = 0;
    } else
        result = 0;

    return result;
}

/* 0 on failure, 1 on success */
/* Failure will be for one of two reasons:
 *  - PyObject2WSABUF() failed to convert `PyObject *o` into something
 *    sendable.  PyErr_Occurred() will not be set.
 *  - Out of mem.  PyErr_Occurred() will be set to PyErr_NoMemory().
 * Caller should rollback heap on failure.
 */
int
PxSocket_NEW_SBUF(
    Context *c,
    PxSocket *s,
    Heap *snapshot,
    DWORD *len,
    char *buf,
    PyObject *o,
    SBUF **sbuf,
    int copy_buf
)
{
    DWORD  nbytes;
    WSABUF w;
    SBUF *b;
    assert(!*sbuf);
    assert(snapshot);
    assert(!snapshot->sle_prev);
    assert(!snapshot->sle_next);
    assert(c == ctx);
    assert(s->ctx == c);
    assert(c->io_obj == (PyObject *)s);
    assert(
        (!len && !buf &&  o) ||
        ( len &&  buf && !o && (*len > 0))
    );

    if (!o) {
        w.len = *len;
        w.buf =  buf;
    } else if (!PyObject2WSABUF(o, &w))
        return 0;

    nbytes = Px_PTR_ALIGN(sizeof(SBUF));
    if (copy_buf)
        nbytes += w.len;

    *sbuf = (SBUF *)_PyHeap_Malloc(c, nbytes, 0, 0);
    if (!*sbuf)
        return 0;

    b = *sbuf;

    b->last_thread_id = _Py_get_current_thread_id();
    b->snapshot = snapshot;
    b->s = s;
    s->sbuf = b;

    b->w.len = w.len;

    if (!copy_buf)
        b->w.buf = w.buf;
    else {
        b->w.buf = (char *)(_Py_PTR_ADD(b, Px_PTR_ALIGN(sizeof(SBUF))));
        memcpy(b->w.buf, w.buf, w.len);
    }

    return 1;
}

static
int
PxOverlapped_IsNull(void *ol)
{
    static OVERLAPPED _null;
    return memcmp(ol, &_null, sizeof(OVERLAPPED));
}

static
void
PxOverlapped_Reset(void *ol)
{
    memset(ol, 0, sizeof(OVERLAPPED));
}

int _PyObject_GenericSetAttr(PyObject *o, PyObject *n, PyObject *v);
int _PyObject_SetAttrString(PyObject *, char *, PyObject *w);

PyObject *_PyObject_GenericGetAttr(PyObject *o, PyObject *n);
PyObject *_PyObject_GetAttrString(PyObject *, char *);

void
_PyParallel_DisassociateCurrentThreadFromCallback(void)
{
    Context *c = ctx;
    if (Px_CTX_IS_DISASSOCIATED(c))
        return;
    DisassociateCurrentThreadFromCallback((PTP_CALLBACK_INSTANCE)c->instance);
    Px_CTXFLAGS(c) |= Px_CTXFLAGS_DISASSOCIATED;
}

void
_PyParallel_BlockingCall(void)
{
    Context *c = ctx;
    Stats   *s = STATS(c);
    Px_GUARD

    if (Px_CTX_IS_DISASSOCIATED(c))
        return;

    if (s && ++s->blocking_calls > _PxBlockingCallsThreshold)
        _PyParallel_DisassociateCurrentThreadFromCallback();
}


void *
_PyHeap_MemAlignedMalloc(Context *c, size_t n)
{
    return _PyHeap_Malloc(c, n, Px_MEM_ALIGN_SIZE, 0);
}


PxListItem *
_PyHeap_NewListItem(Context *c)
{
    return (PxListItem *)_PyHeap_MemAlignedMalloc(c, sizeof(PxListItem));
}


PxListHead *
_PyHeap_NewList(Context *c)
{
    PxListHead *l;

    l = (PxListHead *)_PyHeap_MemAlignedMalloc(c, sizeof(PxListHead));
    if (l)
        InitializeSListHead(l);

    return l;
}


int
_Py_PXCTX(void)
{
    int active = (int)(Py_MainThreadId != _Py_get_current_thread_id());
    assert(Py_MainThreadId > 0);
    assert(Py_MainProcessId != -1);
    return active;
}

void
_PyObject_Dealloc(PyObject *o)
{
    PyTypeObject *tp;
    PyMappingMethods *mm;
    //PySequenceMethods *sm;
    destructor d;

    Py_GUARD_OBJ(o);
    Py_GUARD

    assert(Py_ORIG_TYPE(o));

    if (Py_HAS_EVENT(o))
        PyEvent_DESTROY(o);

    tp = Py_TYPE(o);
    mm = tp->tp_as_mapping;
    //sm = tp->tp_as_sequence;
    d = Py_ORIG_TYPE_CAST(o)->tp_dealloc;
    Py_TYPE(o) = Py_ORIG_TYPE(o);
    Py_ORIG_TYPE(o) = NULL;
    (*d)(o);
    PyMem_FREE(tp);
    PyMem_FREE(mm);
    //PyMem_FREE(sm);
}


char
_protected(PyObject *obj)
{
    PyObject **dp;
    dp = _PyObject_GetDictPtr(obj);
    return (!dp ? Px_ISPROTECTED(obj) :
                  Px_ISPROTECTED(obj) && Px_ISPROTECTED(*dp));
}

PyObject *
_async_protected(PyObject *self, PyObject *obj)
{
    Py_INCREF(obj);
    Py_RETURN_BOOL(_protected(obj));
}


void
_unprotect(PyObject *obj)
{
    PyObject **dp;
    if (_protected(obj)) {
        Py_PXFLAGS(obj) &= ~Py_PXFLAGS_RWLOCK;
        obj->srw_lock = NULL;
    }
    dp = _PyObject_GetDictPtr(obj);
    if (dp && _protected(*dp)) {
        (*dp)->px_flags &= ~Py_PXFLAGS_RWLOCK;
        (*dp)->srw_lock = NULL;
    }
}

PyObject *
_async_unprotect(PyObject *self, PyObject *obj)
{
    Py_INCREF(obj);
    if (Py_ISPX(obj)) {
        PyErr_SetNone(PyExc_ProtectionError);
        return NULL;
    }
    _unprotect(obj);
    Py_RETURN_NONE;
}

PyObject *
_async_read_lock(PyObject *self, PyObject *obj)
{
    Px_PROTECTION_GUARD(obj);
    Py_INCREF(obj);
    return _read_lock(obj);
}

PyObject *
_async_read_unlock(PyObject *self, PyObject *obj)
{
    Px_PROTECTION_GUARD(obj);
    Py_INCREF(obj);
    return _read_unlock(obj);
}

PyObject *
_async_try_read_lock(PyObject *self, PyObject *obj)
{
    Px_PROTECTION_GUARD(obj);
    Py_INCREF(obj);
    Py_RETURN_BOOL(_try_read_lock(obj));
}

PyObject *
_async_write_lock(PyObject *self, PyObject *obj)
{
    Px_PROTECTION_GUARD(obj);
    Py_INCREF(obj);
    return _write_lock(obj);
}

PyObject *
_async_write_unlock(PyObject *self, PyObject *obj)
{
    Px_PROTECTION_GUARD(obj);
    Py_INCREF(obj);
    return _write_unlock(obj);
}

PyObject *
_async_try_write_lock(PyObject *self, PyObject *obj)
{
    Px_PROTECTION_GUARD(obj);
    Py_INCREF(obj);
    Py_RETURN_BOOL(_try_write_lock(obj));
}


Object *
_PyHeap_NewObject(Context *c)
{
    return (Object *)_PyHeap_Malloc(c, sizeof(Object), 0, 0);
}

#define _Px_READ_LOCK(o)    if (Py_HAS_RWLOCK(o)) _read_lock(o)
#define _Px_READ_UNLOCK(o)  if (Py_HAS_RWLOCK(o)) _read_unlock(o)
#define _Px_WRITE_LOCK(o)   if (Py_HAS_RWLOCK(o)) _write_lock(o)
#define _Px_WRITE_UNLOCK(o) if (Py_HAS_RWLOCK(o)) _write_unlock(o)

#define Py_PXCB (_PyParallel_ExecutingCallbackFromMainThread())
#define Px_XISPX(o) ((o) ? Px_ISPX(o) : 0)


int
_Px_TryPersist(PyObject *o)
{
    BOOL pending;
    Context  *c;
    PxObject *x;
    if (!o || (!(Px_ISPX(o))) || Px_PERSISTED(o))
        return 1;

    x = Py_ASPX(o);
    if (!InitOnceBeginInitialize(&(x->persist), 0, &pending, NULL)) {
        PyErr_SetFromWindowsErr(0);
        return 0;
    }
    if (!pending)
        return 1;

    assert(!(Px_PERSISTED(o)));

    c = x->ctx;
    if (!Px_CTX_IS_PERSISTED(c))
        Px_CTXFLAGS(c) |= Px_CTXFLAGS_IS_PERSISTED;

    c->persisted_count++;

    Py_PXFLAGS(o) |= Py_PXFLAGS_PERSISTED;
    Py_REFCNT(o) = 1;

    Px_CTXFLAGS(c) |= Px_CTXFLAGS_IS_PERSISTED;

    if (!InitOnceComplete(&(x->persist), 0, NULL)) {
        PyErr_SetFromWindowsErr(0);
        return 0;
    }

    return 1;
}


int
_Px_objobjargproc_ass(PyObject *o, PyObject *k, PyObject *v)
{
    if (!Py_PXCTX && !Py_PXCB) {
        assert(!Px_ISPX(o));
        assert(!Px_ISPX(k));
        assert(!Px_XISPX(v));
    }
    if (!Px_ISPY(o) || (!Py_PXCTX && !Py_PXCB))
        return 1;

    return !(!_Px_TryPersist(k) || !_Px_TryPersist(v));
}


int
_PyObject_GenericSetAttr(PyObject *o, PyObject *n, PyObject *v)
{
    PyTypeObject *tp;
    int result;
    assert(Py_ORIG_TYPE(o));

    _Px_WRITE_LOCK(o);
    tp = Py_ORIG_TYPE_CAST(o);
    if (tp->tp_setattro)
        result = (*tp->tp_setattro)(o, n, v);
    else
        result = PyObject_GenericSetAttr(o, n, v);
    _Px_WRITE_UNLOCK(o);
    if (result == -1 || !_Px_objobjargproc_ass(o, n, v))
        return -1;

    return result;
}

PyObject *
_PyObject_GenericGetAttr(PyObject *o, PyObject *n)
{
    PyTypeObject *tp;
    PyObject *result;
    assert(Py_ORIG_TYPE(o));

    _Px_READ_LOCK(o);
    tp = Py_ORIG_TYPE_CAST(o);
    if (tp->tp_getattro)
        result = (*tp->tp_getattro)(o, n);
    else
        result = PyObject_GenericGetAttr(o, n);
    _Px_READ_UNLOCK(o);

    return result;
}

int
_PyObject_SetAttrString(PyObject *o, char *n, PyObject *v)
{
    PyTypeObject *tp;
    int result;
    assert(Py_ORIG_TYPE(o));

    _Px_WRITE_LOCK(o);
    tp = Py_ORIG_TYPE_CAST(o);
    if (tp->tp_setattr)
        result = (*tp->tp_setattr)(o, n, v);
    else {
        PyObject *s;
        s = PyUnicode_InternFromString(n);
        if (!s)
            return -1;
        result = PyObject_GenericSetAttr(o, s, v);
        Py_DECREF(s);
    }
    _Px_WRITE_UNLOCK(o);
    if (result != -1 && !_Px_objobjargproc_ass(o, NULL, v))
        result = -1;

    return result;
}

PyObject *
_PyObject_GetAttrString(PyObject *o, char *n)
{
    PyTypeObject *tp;
    PyObject *result;
    assert(Py_ORIG_TYPE(o));

    _Px_READ_LOCK(o);
    tp = Py_ORIG_TYPE_CAST(o);
    if (tp->tp_getattr)
        result = (*tp->tp_getattr)(o, n);
    else {
        PyObject *s;
        s = PyUnicode_InternFromString(n);
        if (!s)
            return NULL;
        result = PyObject_GenericGetAttr(o, s);
        Py_DECREF(s);
    }
    _Px_READ_UNLOCK(o);

    return result;
}

/* MappingMethods */
Py_ssize_t
_Px_mp_length(PyObject *o)
{
    Py_ssize_t result;
    assert(Py_ORIG_TYPE(o));
    _Px_READ_LOCK(o);
    result = Py_ORIG_TYPE_CAST(o)->tp_as_mapping->mp_length(o);
    _Px_READ_UNLOCK(o);
    return result;
}

PyObject *
_Px_mp_subcript(PyObject *o, PyObject *k)
{
    PyObject *result;
    assert(Py_ORIG_TYPE(o));
    _Px_READ_LOCK(o);
    result = Py_ORIG_TYPE_CAST(o)->tp_as_mapping->mp_subscript(o, k);
    _Px_READ_UNLOCK(o);
    return result;
}

int
_Px_mp_ass_subscript(PyObject *o, PyObject *k, PyObject *v)
{
    int result;
    assert(Py_ORIG_TYPE(o));
    _Px_WRITE_LOCK(o);
    result = Py_ORIG_TYPE_CAST(o)->tp_as_mapping->mp_ass_subscript(o, k, v);
    _Px_WRITE_UNLOCK(o);
    if (result == -1 || !_Px_objobjargproc_ass(o, k, v))
        return -1;
    return result;
}

/* SequenceMethods */
Py_ssize_t
_Px_sq_length(PyObject *o)
{
    Py_ssize_t result;
    assert(Py_ORIG_TYPE(o));
    _Px_READ_LOCK(o);
    result = Py_ORIG_TYPE_CAST(o)->tp_as_sequence->sq_length(o);
    _Px_READ_UNLOCK(o);
    return result;
}

char
_PyObject_PrepOrigType(PyObject *o, PyObject *kwds)
{
    PyMappingMethods *old_mm, *new_mm;
    /*PySequenceMethods *old_sm, *new_sm;*/

    if (!Py_ORIG_TYPE(o)) {
        PyTypeObject *type = Py_TYPE(o), *tp;
        size_t size;
        void *offset = type;
        void *m;
        int is_heap = PyType_HasFeature(type, Py_TPFLAGS_HEAPTYPE);
        int is_gc = PyType_IS_GC(type);
        int is_tracked = 0;

        if (is_heap)
            size = sizeof(PyHeapTypeObject);
        else
            size = sizeof(PyTypeObject);

        if (is_gc && is_heap) {
            size += sizeof(PyGC_Head);
            offset = _Py_AS_GC(type);
            is_tracked = _PyObject_GC_IS_TRACKED(type);
        }

        m = PyMem_MALLOC(size);
        if (!m) {
            PyErr_NoMemory();
            return 0;
        }

        if (is_tracked)
            _PyObject_GC_UNTRACK(type);

        memcpy(m, offset, size);

        if (is_gc && is_heap)
            tp = (PyTypeObject *)_Py_FROM_GC(m);
        else
            tp = (PyTypeObject *)m;

        if (is_tracked)
            _PyObject_GC_TRACK(tp);

        Py_ORIG_TYPE(o) = type;
        Py_TYPE(o) = tp;
        tp->tp_dealloc  = _PyObject_Dealloc;
        tp->tp_setattro = _PyObject_GenericSetAttr;
        tp->tp_getattro = _PyObject_GenericGetAttr;
        tp->tp_setattr  = _PyObject_SetAttrString;
        tp->tp_getattr  = _PyObject_GetAttrString;

        old_mm = type->tp_as_mapping;
        if (old_mm && old_mm->mp_subscript) {
            size = sizeof(PyMappingMethods);
            new_mm = (PyMappingMethods *)PyMem_MALLOC(size);
            if (!new_mm)
                goto free_m;

            memcpy(new_mm, old_mm, size);

            new_mm->mp_subscript = _Px_mp_subcript;

            new_mm->mp_length = (old_mm->mp_length ? _Px_mp_length : 0);
            new_mm->mp_ass_subscript = (
                old_mm->mp_ass_subscript ? _Px_mp_ass_subscript : 0
            );

            tp->tp_as_mapping = new_mm;
        }

        /*
        old_sm = type->tp_as_sequence;
        if (old_sm) {
            assert(old_sm->sq_item);
            size = sizeof(PySequenceMethods);
            new_sm = (PySequenceMethods *)PyMem_MALLOC(size);
            if (!new_sm)
                goto free_new_mm;

            memcpy(new_sm, old_sm, size);

            new_sm->sq_item = _Px_sq_item;

            new_sm->sq_length = (old_sm->sq_length ? _Px_sq_length : 0);
            new_sm->sq_concat = (old_sm->sq_concat ? _Px_sq_concat : 0);
            new_sm->sq_repeat = (old_sm->sq_repeat ? _Px_sq_repeat : 0);
            new_sm->sq_ass_item = (old_sm->sq_ass_item ? _Px_sq_ass_item : 0);
            new_sm->sq_contains = (old_sm->sq_contains ? _Px_sq_contains : 0);
            new_sm->sq_inplace_concat = (
                old_sm->sq_inplace_concat ? _Px_sq_inplace_concat : 0
            );
            new_sm->sq_inplace_repeat = (
                old_sm->sq_inplace_repeat ? _Px_sq_inplace_repeat : 0
            );

            tp->tp_as_sequence = new_sm;
        }
        */

        goto check_invariants;

        /*
    free_new_mm:
        PyMem_FREE(new_mm);
        */

    free_m:
        PyMem_FREE(m);

        PyErr_NoMemory();
        goto error;

    }

check_invariants:
    assert(Py_ORIG_TYPE(o));
    assert(Py_ORIG_TYPE_CAST(o)->tp_dealloc);
    assert(Py_ORIG_TYPE_CAST(o)->tp_dealloc != _PyObject_Dealloc);
    assert(Py_TYPE(o)->tp_dealloc  == _PyObject_Dealloc);
    assert(Py_TYPE(o)->tp_setattro == _PyObject_GenericSetAttr);
    assert(Py_TYPE(o)->tp_getattro == _PyObject_GenericGetAttr);
    assert(Py_TYPE(o)->tp_setattr  == _PyObject_SetAttrString);
    assert(Py_TYPE(o)->tp_getattr  == _PyObject_GetAttrString);
    old_mm = (Py_ORIG_TYPE_CAST(o)->tp_as_mapping);
    if (old_mm && old_mm->mp_subscript) {
        new_mm = Py_TYPE(o)->tp_as_mapping;
        assert(new_mm->mp_subscript == _Px_mp_subcript);
        assert(old_mm->mp_length ? (new_mm->mp_length == _Px_mp_length) : 1);
        assert(
            !old_mm->mp_ass_subscript ? 1 : (
                new_mm->mp_ass_subscript == _Px_mp_ass_subscript
            )
        );
    }
    /*
    old_sm = (Py_ORIG_TYPE_CAST(o)->tp_as_sequence);
    if (old_sm) {
        new_sm = Py_TYPE(o)->tp_as_sequence;
        assert(new_sm->sq_item);
        assert(new_sm->sq_item == _Px_sq_item);
        assert(old_sm->sq_length ? (new_sm->sq_length == _Px_sq_length) : 1);
        assert(old_sm->sq_concat ? (new_sm->sq_concat == _Px_sq_concat) : 1);
        assert(old_sm->sq_repeat ? (new_sm->sq_repeat == _Px_sq_repeat) : 1);
        assert(old_sm->sq_ass_item ? (new_sm->sq_ass_item == _Px_sq_ass_item) : 1);
        assert(old_sm->sq_contains ? (new_sm->sq_contains == _Px_sq_contains) : 1);
        assert(
            !old_sm->sq_inplace_concat ? 1 : (
                new_sm->sq_inplace_concat == _Px_sq_inplace_concat
            )
        );
        assert(
            !old_sm->sq_inplace_repeat ? 1 : (
                new_sm->sq_inplace_repeat == _Px_sq_inplace_repeat
            )
        );
    }
    */
    return 1;
error:
    assert(PyErr_Occurred());
    return 0;
}

char
_PyEvent_TryCreate(PyObject *o)
{
    char success = 1;
    assert(Py_HAS_RWLOCK(o));
    _write_lock(o);
    if (!Py_HAS_EVENT(o)) {
        success = 0;
        if (Py_ISPX(o))
            PyErr_SetNone(PyExc_WaitError);
        else if (!_PyObject_PrepOrigType(o, 0))
            goto done;
        else if (!PyEvent_CREATE(o))
            PyErr_SetFromWindowsErr(0);
        else {
            Py_PXFLAGS(o) |= Py_PXFLAGS_EVENT;
            success = 1;
        }
    }
done:
    if (!success)
        assert(PyErr_Occurred());
    _write_unlock(o);
    return success;
}

PyObject *
_async_wait(PyObject *self, PyObject *o)
{
    DWORD result;
    Px_PROTECTION_GUARD(o);
    Py_INCREF(o);
    if (!_PyEvent_TryCreate(o))
        return NULL;

    result = WaitForSingleObject((HANDLE)o->event, INFINITE);

    if (result == WAIT_OBJECT_0)
        Py_RETURN_NONE;

    else if (result == WAIT_ABANDONED)
        PyErr_SetString(PyExc_SystemError, "wait abandoned");

    else if (result == WAIT_TIMEOUT)
        PyErr_SetString(PyExc_SystemError, "infinite wait timed out?");

    else if (result == WAIT_FAILED)
        PyErr_SetFromWindowsErr(0);

    else
        PyErr_SetString(PyExc_SystemError, "unexpected result from wait");

    return NULL;
}

PyObject *
_async_prewait(PyObject *self, PyObject *o)
{
    Px_PROTECTION_GUARD(o);
    Py_INCREF(o);
    if (!_PyEvent_TryCreate(o))
        return NULL;

    Py_INCREF(o);
    return o;
}

PyObject *
_async_signal(PyObject *self, PyObject *o)
{
    PyObject *result = NULL;
    Px_PROTECTION_GUARD(o);
    Py_INCREF(o);
    _write_lock(o);

    if (!Py_HAS_EVENT(o))
        PyErr_SetNone(PyExc_NoWaitersError);
    else if (!PyEvent_SIGNAL(o))
        PyErr_SetFromWindowsErr(0);
    else
        result = Py_None;

    _write_unlock(o);
    Py_XINCREF(result);
    return result;
}

PyObject *
_async_signal_and_wait(PyObject *self, PyObject *args)
{
    PyObject *s, *w;
    PyObject *result = NULL;
    DWORD wait_result;

    if (!PyArg_UnpackTuple(args, "signal_and_wait", 2, 2, &s, &w))
        goto done;

    Px_PROTECTION_GUARD(s);
    Px_PROTECTION_GUARD(w);

    if (s == w) {
        PyErr_SetString(PyExc_WaitError,
                        "signal and wait objects must differ");
        goto done;
    }

    if (!_PyEvent_TryCreate(w))
        goto done;

    if (!Py_HAS_EVENT(s)) {
        PyErr_SetNone(PyExc_NoWaitersError);
        goto done;
    }

    Py_INCREF(s);
    Py_INCREF(w);

    wait_result = SignalObjectAndWait(Py_EVENT(s), Py_EVENT(w), INFINITE, 0);

    if (wait_result == WAIT_OBJECT_0 || wait_result == WAIT_IO_COMPLETION)
        result = Py_None;

    else if (wait_result == WAIT_ABANDONED)
        PyErr_SetString(PyExc_SystemError, "wait abandoned");

    else if (wait_result == WAIT_TIMEOUT)
        PyErr_SetString(PyExc_SystemError, "infinite wait timed out?");

    else if (wait_result == WAIT_FAILED)
        PyErr_SetFromWindowsErr(0);

    else
        PyErr_SetString(PyExc_SystemError, "unexpected result from wait");

    Py_DECREF(s);
    Py_DECREF(w);

done:
    if (!result)
        assert(PyErr_Occurred());
    else
        Py_INCREF(result);

    return result;
}


PyObject *
_protect(PyObject *obj)
{
    PyObject **dp;
    if (!obj)
        return NULL;

    if (!_protected(obj)) {
        if (!_PyObject_PrepOrigType(obj, 0))
            return NULL;
        InitializeSRWLock((PSRWLOCK)&(obj->srw_lock));
        Py_PXFLAGS(obj) |= Py_PXFLAGS_RWLOCK;
    }

    dp = _PyObject_GetDictPtr(obj);
    if (dp && !_protected(*dp)) {
        if (!_PyObject_PrepOrigType(*dp, 0)) {
            /* Manually undo the protection we applied above. */
            Py_PXFLAGS(obj) &= ~Py_PXFLAGS_RWLOCK;
            obj->srw_lock = NULL;
            return NULL;
        }
        InitializeSRWLock((PSRWLOCK)&((*dp)->srw_lock));
        Py_PXFLAGS((*dp)) |= Py_PXFLAGS_RWLOCK;
    }
    return obj;
}

PyObject *
_async_protect(PyObject *self, PyObject *obj)
{
    Py_INCREF(obj);
    if (Py_ISPX(obj)) {
        PyErr_SetNone(PyExc_ProtectionError);
        return NULL;
    }
    Py_INCREF(obj);
    return _protect(obj);
}

PyObject *
_async__rawfile(PyObject *self, PyObject *obj)
{
    PyObject *raw;
    fileio   *f;

    Py_INCREF(obj);
    if (PyFileIO_Check(obj))
        return obj;

    raw = PyObject_GetAttrString(obj, "raw");
    if (!raw) {
        PyErr_SetString(PyExc_ValueError, "not an io file object");
        return NULL;
    }

    if (!PyFileIO_Check(raw)) {
        PyErr_SetString(PyExc_ValueError, "invalid type for raw attribute");
        return NULL;
    }

    f = (fileio *)raw;
    Py_INCREF(f);
    f->owner = obj;
    return (PyObject *)f;
}

int
_PxPages_LookupHeapPage(PxPages *pages, Px_UINTPTR *value, void *p)
{
    PxPages *x;
    HASH_FIND_INT(pages, value, x);
    if (x) {
        Heap *h1, *h2;
        assert(x->count >= 1 && x->count <= 2);
        h1 = x->heaps[0];
        h2 = (x->count == 2 ? x->heaps[1] : NULL);
        if (Px_PTR_IN_HEAP(p, h1) || (h2 && Px_PTR_IN_HEAP(p, h2)))
            return 1;
    }
    return 0;
}

int
PxPages_Find(PxPages *pages, void *p)
{
    int found;
    Px_UINTPTR lower, upper;

    lower = Px_PAGESIZE_ALIGN_DOWN(p, Px_PAGE_SIZE);
    upper = Px_PAGESIZE_ALIGN_UP(p, Px_PAGE_SIZE);

    found = _PxPages_LookupHeapPage(pages, &lower, p);
    if (!found && lower != upper)
        found = _PxPages_LookupHeapPage(pages, &upper, p);

    return found;
}

void
_PxPages_AddHeapPage(PxPages **pages, Px_UINTPTR *value, Heap *h)
{
    PxPages *x;
    HASH_FIND_INT(*pages, value, x);
    if (x) {
        /* Original:
        assert(x->count == 1);
        x->heaps[1] = h;
        x->count++; */
        x->heaps[x->count++] = h;
    } else {
        x = (PxPages *)malloc(sizeof(PxPages));
        x->heaps[0] = h;
        x->count = 1;
        x->base = *value;
        HASH_ADD_INT(*pages, base, x);
    }
}

void
_PxPages_RemoveHeapPage(PxPages **pages, Px_UINTPTR *value, Heap *h)
{
    PxPages *x;
    HASH_FIND_INT(*pages, value, x);
    assert(x);
    if (x->count == 1) {
        assert(x->heaps[0] == h);
        HASH_DEL(*pages, x);
        free(x);
    } else {
        assert(x->count >= 1 && x->count <= 2);
        if (x->heaps[0] == h)
            x->heaps[0] = x->heaps[1];
        else
            assert(x->heaps[1] == h);
        x->heaps[1] = NULL;
        x->count = 1;
    }
}

void
PxPages_Dump(PxPages *pages)
{
    PxPages *x, *t;
    int i = 0;
    HASH_ITER(hh, pages, x, t) {
        i++;
        printf("[%d] base: 0x%llx, count: %d\n", i, x->base, x->count);
    }
}


void
_PxState_InitPxPages(PxState *px)
{
    Py_GUARD

    InitializeSRWLock(&px->pages_srwlock);
}

void
_PxState_RegisterHeap(PxState *px, Heap *h, Context *c)
{
    int i;

    AcquireSRWLockExclusive(&px->pages_srwlock);

    assert((h->size % h->page_size) == 0);

    for (i = 0; i < h->pages; i++) {
        void *p;
        Px_UINTPTR lower, upper;

        p = Px_PTR_ADD(h->base, (i * h->page_size));

        lower = Px_PAGESIZE_ALIGN_DOWN(p, h->page_size);
        upper = Px_PAGESIZE_ALIGN_UP(p, h->page_size);

        _PxPages_AddHeapPage(&(px->pages), &lower, h);
        if (lower != upper)
            _PxPages_AddHeapPage(&(px->pages), &upper, h);
    }
    ReleaseSRWLockExclusive(&px->pages_srwlock);
}

void
_PxContext_UnregisterHeaps(Context *c)
{
    Heap    *h;
    Stats   *s;
    PxState *px;
    int      i, heap_count = 0;

    Py_GUARD

    px =  c->px;
    s  = &c->stats;

    AcquireSRWLockExclusive(&px->pages_srwlock);

    h = &c->heap;
    while (h) {
        heap_count++;

        assert((h->size % h->page_size) == 0);

        for (i = 0; i < h->pages; i++) {
            void *p;
            Px_UINTPTR lower, upper;

            p = Px_PTR_ADD(h->base, (i * h->page_size));

            lower = Px_PAGESIZE_ALIGN_DOWN(p, h->page_size);
            upper = Px_PAGESIZE_ALIGN_UP(p, h->page_size);

            _PxPages_RemoveHeapPage(&(px->pages), &lower, h);
            if (lower != upper)
                _PxPages_RemoveHeapPage(&(px->pages), &upper, h);
        }

        h = h->sle_next;
        if (h->size == 0)
            break;
    }
    ReleaseSRWLockExclusive(&px->pages_srwlock);
    assert(heap_count == s->heaps);
}

#define _MEMSIG_INVALID     (0UL)
#define _MEMSIG_NOT_READY   (1UL)
#define _MEMSIG_NULL        (1UL << 1)
#define _MEMSIG_UNKNOWN     (1UL << 2)
#define _MEMSIG_PY          (1UL << 3)
#define _MEMSIG_PX          (1UL << 4)

#define _OBJSIG_INVALID     (0UL)
#define _OBJSIG_NULL        (1UL << 1)
#define _OBJSIG_UNKNOWN     (1UL << 2)  /* 4  */
#define _OBJSIG_PY          (1UL << 3)  /* 8  */
#define _OBJSIG_PX          (1UL << 4)  /* 16 */

#define _SIG_INVALID        (0UL)
#define _SIG_NULL           (1UL << 1)
#define _SIG_UNKNOWN        (1UL << 2)
#define _SIG_PY             (1UL << 3)
#define _SIG_PX             (1UL << 4)

Py_TLS int _Px_ObjectSignature_CallDepth = 0;
Py_TLS int _Px_SafeObjectSignatureTest_CallDepth = 0;

unsigned long
_Px_MemorySignature(void *m)
{
    PxState *px;
    unsigned long signature;

    if (!m)
        return _MEMSIG_NULL;

    px = PXSTATE();
    if (!px)
        return (_PyMem_InRange(m) ? _MEMSIG_PY : _MEMSIG_NOT_READY);

    signature = _MEMSIG_UNKNOWN;

    AcquireSRWLockShared(&px->pages_srwlock);
    if (PxPages_Find(px->pages, m))
        signature = _MEMSIG_PX;
    ReleaseSRWLockShared(&px->pages_srwlock);

    if (signature == _MEMSIG_UNKNOWN && _PyMem_InRange(m))
        signature = _MEMSIG_PY;

    return signature;
}

unsigned long
_Px_ObjectSignature(void *m)
{
    PyObject     *y;
    Py_uintptr_t  s;
    unsigned long signature;

    if (!m)
        return _OBJSIG_NULL;

    assert(_Px_ObjectSignature_CallDepth == 0);
    _Px_ObjectSignature_CallDepth++;

    y = (PyObject *)m;

    __try {
        s = ((Py_uintptr_t)(y->is_px));
    } __except(
        GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ?
            EXCEPTION_EXECUTE_HANDLER :
            EXCEPTION_CONTINUE_SEARCH
    ) {
        s = (Py_uintptr_t)NULL;
    }

    if (!s) {
        signature = _OBJSIG_UNKNOWN;
        goto done;
    }

    if (s == (Py_uintptr_t)_Py_NOT_PARALLEL) {
        assert(y->px == _Py_NOT_PARALLEL);
        signature = _OBJSIG_PY;
        goto done;
    }

    if (s == (Py_uintptr_t)_Py_IS_PARALLEL) {
        assert(y->px != NULL);
        assert(Py_ASPX(y)->signature == _PxObjectSignature);
        signature = _OBJSIG_PX;
        goto done;
    }

    /* We'll hit this if m is a valid pointer (i.e. dereferencing m->is_px
     * doesn't trigger the SEH), but it doesn't point to something with a
     * valid object signature.
     */
    signature = _OBJSIG_UNKNOWN;
done:
    _Px_ObjectSignature_CallDepth--;
    return signature;
}

unsigned long
_Px_SafeObjectSignatureTest(void *m)
{
    PyObject     *y;
    PxObject     *x;
    Py_uintptr_t  s;
    int is_py;
    int is_px;
    unsigned long signature;

    if (!m)
        return _OBJSIG_NULL;

    y = (PyObject *)m;

    assert(_Px_SafeObjectSignatureTest_CallDepth == 0);
    _Px_SafeObjectSignatureTest_CallDepth++;

    __try {
        s = ((Py_uintptr_t)(y->is_px));
    } __except(
        GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ?
            EXCEPTION_EXECUTE_HANDLER :
            EXCEPTION_CONTINUE_SEARCH
    ) {
        s = (Py_uintptr_t)NULL;
    }

    if (!s) {
        signature = _OBJSIG_UNKNOWN;
        goto done;
    }

    is_py = (s == (Py_uintptr_t)_Py_NOT_PARALLEL);

    if (is_py) {
        signature = _OBJSIG_PY;
        goto done;
    }

    is_px = -1;
    x = Py_ASPX(y);
    __try {
        is_px = (x->signature == _PxObjectSignature);
    } __except(
        GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ?
            EXCEPTION_EXECUTE_HANDLER :
            EXCEPTION_CONTINUE_SEARCH
    ) {
        is_px = 0;
    }

    assert(is_px != -1);

    signature = (is_px ? _OBJSIG_PX : _OBJSIG_UNKNOWN);
done:
    _Px_SafeObjectSignatureTest_CallDepth--;
    return signature;
}

int
_PyParallel_GuardObj(const char *function,
                     const char *filename,
                     int lineno,
                     void *m,
                     unsigned int flags)
{
    unsigned long s, o;

    assert(_OBJTEST(flags));

    if (_PyParallel_Finalized)
        return (_PYTEST(flags) ? 1 : 0);

    if (m) {
        o = _Px_SafeObjectSignatureTest(m);
        s = _Px_MemorySignature(m);
        if (s & _MEMSIG_NOT_READY || (o > s))
            s = o;

        if (s & (_OBJSIG_UNKNOWN))
            s = _OBJSIG_PY;

        assert(s & (_OBJSIG_UNKNOWN | _OBJSIG_PX | _OBJSIG_PY));
    }

    if (flags & (_PXOBJ_TEST | _PY_ISPX_TEST)) {

        if (!m)
            return 0;

        if (flags & _PY_ISPX_TEST) {
            /* Special case for Py_ISPX(o); o must be a valid object. */
            assert(s & (_OBJSIG_PY | _OBJSIG_PX));
            return (Py_PXCTX ? 1 : ((s & _OBJSIG_PX) == _OBJSIG_PX) ? 1 : 0);
        }

        return ((s & _OBJSIG_PX) == _OBJSIG_PX);

    } else if (flags & _PYOBJ_TEST) {

        if (!m)
            return 0;

        return ((s & _OBJSIG_PY) == _OBJSIG_PY);

    } else {
        assert(m);

        assert(flags & (_PYOBJ_GUARD | _PXOBJ_GUARD));

        if (flags & _PYOBJ_GUARD)
            assert(s & _OBJSIG_PY);
        else
            assert(s & _OBJSIG_PX);

        return 0;
    }
}

void
_PxWarn_PyMemUnknown(void)
{
    PySys_FormatStderr(
        "WARNING! expected _MEMSIG_PY but got _MEMSIG_UNKNOWN\n"
    );
}

int
_PyParallel_GuardMem(const char *function,
                     const char *filename,
                     int lineno,
                     void *m,
                     unsigned int flags)
{
    unsigned long s = 0;
    unsigned long o;

    assert(_MEMTEST(flags));

    if (_PyParallel_Finalized)
        return (_PYTEST(flags) ? 1 : 0);

    if (m) {
        if (_PyParallel_IsHeapOverrideActive()) {
            if (m == last_heap_override_malloc_addr)
                s = _MEMSIG_PX;
        }
        if (!s) {
            if (m == last_context_heap_malloc_addr)
                s = _MEMSIG_PX;
        }
        if (!s) {
            o = _Px_SafeObjectSignatureTest(m);
            s = _Px_MemorySignature(m);
            if (s & _MEMSIG_NOT_READY || (o > s))
                s = o;
        }
    }

    if (flags & (_PYMEM_TEST | _PXMEM_TEST)) {

        if (!m)
            return 0;

        return (flags & _PYMEM_TEST ? s & _MEMSIG_PY : s & _MEMSIG_PX);

    } else {
        assert(m);

        assert(flags & (_PYMEM_GUARD | _PXMEM_GUARD));

        if (flags & _PYMEM_GUARD) {
            if (s & _MEMSIG_UNKNOWN) {
                //printf("expected _MEMSIG_PY but got _MEMSIG_UNKNOWN\n");
                return 0;
            }
            assert(s & _MEMSIG_PY);
        } else {
            if (!(s & _MEMSIG_PX)) {
                PxState *px = PXSTATE();
                AcquireSRWLockShared(&px->pages_srwlock);
                printf("\ncouldn't find ptr: 0x%llx\n", m);
                PxPages_Dump(px->pages);
                ReleaseSRWLockExclusive(&px->pages_srwlock);
            } else {
                //printf("found ptr 0x%llx\n", m);
            }
            assert(s & _MEMSIG_PX);
        }

        return 0;
    }
}

int
_PyParallel_Guard(const char *function,
                  const char *filename,
                  int lineno,
                  void *m,
                  unsigned int flags)
{
    assert(_Py_UINT32_BITS_SET(flags) == 1);

    if (_OBJTEST(flags))
        return _PyParallel_GuardObj(function, filename, lineno, m, flags);
    else {
        assert(_MEMTEST(flags));
        return _PyParallel_GuardMem(function, filename, lineno, m, flags);
    }
}

int
_Px_TEST(void *p)
{
    unsigned long o, m, s;
    if (!p)
        return _SIG_NULL;

    o = _Px_SafeObjectSignatureTest(p);
    m = _Px_MemorySignature(p);
    s = Px_MAX(o, m);
    return (s & _SIG_PX);
}

void
_PyParallel_ContextGuardFailure(const char *function,
                                const char *filename,
                                int lineno,
                                int was_px_ctx)
{
    int err;
    char buf[128], *fmt;
    memset((void *)buf, 0, sizeof(buf));

    if (was_px_ctx)
        fmt = "%s called outside of parallel context (%s:%d)";
    else
        fmt = "%s called from within parallel context (%s:%d)";

    err = snprintf(buf, sizeof(buf), fmt, function, filename, lineno);
    if (err == -1)
        Py_FatalError("_PyParallel_ContextGuardFailure: snprintf failed");
    else {
        char *guard_override = getenv("PYPARALLEL_PY_GUARD");
        if (!guard_override)
            Py_FatalError(buf);

        if (!strcmp(guard_override, "warn")) {
            PyErr_Warn(PyExc_RuntimeWarning, buf);
        } else if (!strcmp(guard_override, "break")) {
            __debugbreak();
        }
    }
}
/*
#endif
*/
#define Px_SIZEOF_HEAP        Px_CACHE_ALIGN(sizeof(Heap))
#define Px_USEABLE_HEAP_SIZE (Px_PAGE_ALIGN_SIZE - Px_SIZEOF_HEAP)
#define Px_NEW_HEAP_SIZE(n)  Px_PAGE_ALIGN((Py_MAX(n, Px_USEABLE_HEAP_SIZE)))

void *
Heap_Init(Context *c, size_t n, int page_size)
{
    Heap  *h;
    Stats *s = &(c->stats);
    size_t size;
    int flags;

    assert(!Px_TLS_HEAP_ACTIVE);

    if (!page_size)
        page_size = Px_PAGE_SIZE;

    assert(
        page_size == Px_PAGE_SIZE ||
        page_size == Px_LARGE_PAGE_SIZE
    );


    if (n < Px_DEFAULT_HEAP_SIZE)
        size = Px_DEFAULT_HEAP_SIZE;
    else
        size = n;

    size = Px_PAGESIZE_ALIGN_UP(size, page_size);

    assert((size % page_size) == 0);

    if (!c->h) {
        /* First init. */
        h = &(c->heap);
        h->id = 1;
    } else {
        h = c->h->sle_next;
        h->sle_prev = c->h;
        h->id = h->sle_prev->id + 1;
    }

    assert(h);

    h->page_size = page_size;
    h->pages = size / page_size;

    h->size = size;
    flags = HEAP_ZERO_MEMORY;
    h->base = h->next = HeapAlloc(c->heap_handle, flags, h->size);
    if (!h->base)
        return PyErr_SetFromWindowsErr(0);
    h->next_alignment = Px_GET_ALIGNMENT(h->base);
    h->remaining = size;
    s->remaining = size;
    s->size += size;
    s->heaps++;
    c->h = h;
    h->ctx = c;
    h->sle_next = (Heap *)_PyHeap_Malloc(c, sizeof(Heap), 0, 0);
    assert(h->sle_next);
    _PxState_RegisterHeap(c->px, h, c);
    return h;
}

/* 0 = failure, 1 = success */
int
_PyTLSHeap_Init(size_t n, int page_size)
{
    TLS *t = &tls;
    Heap *h;
    Stats *s = &(t->stats);
    size_t size;
    int flags;

    if (!page_size)
        page_size = Px_PAGE_SIZE;

    assert(
        page_size == Px_PAGE_SIZE ||
        page_size == Px_LARGE_PAGE_SIZE
    );

    if (n < _PyTLSHeap_DefaultSize)
        size = _PyTLSHeap_DefaultSize;
    else
        size = n;

    size = Px_PAGESIZE_ALIGN_UP(size, page_size);

    assert((size % page_size) == 0);

    if (!t->h) {
        /* First init. */
        h = &(t->heap);
        h->id = 1;
    } else {
        h = t->h->sle_next;
        h->sle_prev = t->h;
        h->id = h->sle_prev->id + 1;
    }

    assert(h);

    h->page_size = page_size;
    h->pages = size / page_size;

    h->size = size;
    flags = HEAP_ZERO_MEMORY;
    h->base = h->next = HeapAlloc(t->handle, flags, h->size);
    if (!h->base)
        return (int)PyErr_SetFromWindowsErr(0);
    h->next_alignment = Px_GET_ALIGNMENT(h->base);
    h->remaining = size;
    s->remaining = size;
    s->size += size;
    s->heaps++;
    t->h = h;
    h->tls = t;
    h->sle_next = (Heap *)_PyTLSHeap_Malloc(sizeof(Heap), 0);
    assert(h->sle_next);
    _PxState_RegisterHeap(t->px, h, 0);
    return 1;
}

void *
_PyHeap_Init(Context *c, Py_ssize_t n)
{
    return Heap_Init(c, n, 0);
}

void *
_PyTLSHeap_Malloc(size_t n, size_t align)
{
    void  *next;
    Heap  *h;
    TLS   *t = &tls;
    Stats *s = &t->stats;
    size_t alignment_diff;
    size_t alignment = align;
    size_t requested_size = n;
    size_t aligned_size;

    assert(t->heap_depth > 0 || _PxNewThread);

    if (!alignment)
        alignment = Px_PTR_ALIGN_SIZE;
begin:
    h = t->h;

    if (alignment > h->next_alignment)
        alignment_diff = Px_PTR_ALIGN(alignment - h->next_alignment);
    else
        alignment_diff = 0;

    aligned_size = Px_ALIGN(n, alignment);

    if (aligned_size < (h->remaining-alignment_diff)) {
        if (alignment_diff) {
            h->remaining -= alignment_diff;
            s->remaining -= alignment_diff;
            h->allocated += alignment_diff;
            s->allocated += alignment_diff;
            h->alignment_mismatches++;
            s->alignment_mismatches++;
            h->bytes_wasted += alignment_diff;
            s->bytes_wasted += alignment_diff;
            h->next = Px_PTR_ADD(h->next, alignment_diff);
            assert(Px_PTR_ADD(h->base, h->allocated) == h->next);
        }

        h->allocated += aligned_size;
        s->allocated += aligned_size;

        h->remaining -= aligned_size;
        s->remaining -= aligned_size;

        h->mallocs++;
        s->mallocs++;

        h->bytes_wasted += (aligned_size - requested_size);
        s->bytes_wasted += (aligned_size - requested_size);

        next = h->next;
        h->next = Px_PTR_ADD(h->next, aligned_size);
        h->next_alignment = Px_GET_ALIGNMENT(h->next);

        assert(Px_PTR_ADD(h->base, h->allocated) == h->next);
        assert(_Py_IS_ALIGNED(h->base, alignment));
        assert(Px_GET_ALIGNMENT(next) >= alignment);
        return next;
    }

    t->h = h->sle_next;

    if (!t->h->size && !_PyTLSHeap_Init(Px_NEW_HEAP_SIZE(aligned_size), 0)) {
        return _aligned_malloc(aligned_size, alignment);
    }

    goto begin;
}

void *
_PyHeapOverride_Malloc(size_t n, size_t align)
{
    void *p;
    HANDLE h;
    int flags = HEAP_ZERO_MEMORY;
    size_t aligned_size = Px_ALIGN(n, Px_MAX(align, Px_PTR_ALIGN_SIZE));
    assert(_PyParallel_IsHeapOverrideActive());

    h = _PyParallel_GetHeapOverride();

    p = HeapAlloc(h, HEAP_ZERO_MEMORY, aligned_size);
    if (!p)
        PyErr_SetFromWindowsErr(0);

    last_heap_override_malloc_addr = p;

    return p;
}

void *
_PyHeap_Malloc(Context *c, size_t n, size_t align, int no_realloc)
{
    void  *next;
    Heap  *h;
    Stats *s;
    size_t alignment_diff;
    size_t alignment = align;
    size_t requested_size = n;
    size_t aligned_size;

    if (Px_TLS_HEAP_ACTIVE)
        return _PyTLSHeap_Malloc(n, align);

    if (_PyParallel_IsHeapOverrideActive())
        return _PyHeapOverride_Malloc(n, align);

    s = &c->stats;
    if (!alignment)
        alignment = Px_PTR_ALIGN_SIZE;

begin:
    h = c->h;

    if (alignment > h->next_alignment)
        alignment_diff = Px_PTR_ALIGN(alignment - h->next_alignment);
    else
        alignment_diff = 0;

    aligned_size = Px_ALIGN(n, alignment);

    if (aligned_size < (h->remaining-alignment_diff)) {
        if (alignment_diff) {
            h->remaining -= alignment_diff;
            s->remaining -= alignment_diff;
            h->allocated += alignment_diff;
            s->allocated += alignment_diff;
            h->alignment_mismatches++;
            s->alignment_mismatches++;
            h->bytes_wasted += alignment_diff;
            s->bytes_wasted += alignment_diff;
            h->next = Px_PTR_ADD(h->next, alignment_diff);
            assert(Px_PTR_ADD(h->base, h->allocated) == h->next);
        }

        h->allocated += aligned_size;
        s->allocated += aligned_size;

        h->remaining -= aligned_size;
        s->remaining -= aligned_size;

        h->mallocs++;
        s->mallocs++;

        h->bytes_wasted += (aligned_size - requested_size);
        s->bytes_wasted += (aligned_size - requested_size);

        next = h->next;
        h->next = Px_PTR_ADD(h->next, aligned_size);
        h->next_alignment = Px_GET_ALIGNMENT(h->next);

        assert(Px_PTR_ADD(h->base, h->allocated) == h->next);
        assert(_Py_IS_ALIGNED(h->base, alignment));
        assert(Px_GET_ALIGNMENT(next) >= alignment);
        last_context_heap_malloc_addr = next;
        return next;
    }

    if (no_realloc)
        NULL;

    /* Force a resize. */
    if (!_PyHeap_Init(c, Px_NEW_HEAP_SIZE(aligned_size)))
        return PyErr_NoMemory();

    goto begin;
}


void
_PyHeap_FastFree(Heap *h, Stats *s, void *p)
{
    h->frees++;
    s->frees++;
}

void *
_PyTLSHeap_Realloc(void *p, size_t n)
{
    void *r = _PyTLSHeap_Malloc(n, 0);
    if (!r)
        return NULL;

    if (p)
        memcpy(r, p, n);

    return r;
}

void *
_PyHeap_Realloc(Context *c, void *p, size_t n)
{
    void  *r;
    Heap  *h;
    Stats *s;

    if (Px_TLS_HEAP_ACTIVE)
        return _PyTLSHeap_Realloc(p, n);

    h = c->h;
    s = &c->stats;
    r = _PyHeap_Malloc(c, n, 0, 0);
    if (!r)
        return NULL;
    if (!p)
        return r;
    h->mem_reallocs++;
    s->mem_reallocs++;
    memcpy(r, p, n);
    return r;
}

void
_PyHeap_Free(Context *c, void *p)
{
    Heap  *h;
    Stats *s;

    if (Px_TLS_HEAP_ACTIVE)
        return;

    Px_GUARD_MEM(p);

    h = c->h;
    s = &c->stats;

    h->frees++;
    s->frees++;
}

#define _Px_X_OFFSET(n) (Px_PTR_ALIGN(n))
#define _Px_O_OFFSET(n) \
    (Px_PTR_ALIGN((_Px_X_OFFSET(n)) + (Px_PTR_ALIGN(sizeof(PxObject)))))

#define _Px_X_PTR(p, n) \
    ((PxObject *)(Px_PTR_ALIGN(Px_PTR_ALIGNED_ADD((p), _Px_X_OFFSET((n))))))

#define _Px_O_PTR(p, n) \
    ((Object *)(Px_PTR_ALIGN(Px_PTR_ALIGNED_ADD((p), _Px_O_OFFSET((n))))))

#define _Px_SZ(n) (Px_PTR_ALIGN(      \
    Px_PTR_ALIGN(n)                 + \
    Px_PTR_ALIGN(sizeof(PxObject))  + \
    Px_PTR_ALIGN(sizeof(Object))      \
))

#define _Px_VSZ(t, n) (Px_PTR_ALIGN( \
    ((!((t)->tp_itemsize)) ?         \
        _PyObject_SIZE(t) :          \
        _PyObject_VAR_SIZE(t, n))))

PyObject *
init_object(Context *c, PyObject *p, PyTypeObject *tp, Py_ssize_t nitems)
{
    /* Main use cases:
     *  1.  Redirect from PyObject_NEW/PyObject_NEW_VAR.  p will be NULL.
     *  2.  PyObject_MALLOC/PyObject_INIT combo (PyUnicode* does this).
     *      p will pass Px_GUARD_MEM(p) and everything will be null.  We
     *      need to allocate x & o manually.
     *  3.  Redirect from PyObject_GC_Resize.  p shouldn't be NULL, although
     *      it might not be a PX allocation -- we could be resizing a PY
     *      allocation.  That's fine, as our realloc doesn't actually free
     *      anything, so we're, in effect, just copying the existing object.
     */
    TLS      *t = &tls;
    PyObject *n;
    PxObject *x;
    Object   *o;
    Stats    *s;
    size_t    object_size;
    size_t    total_size;
    size_t    bytes_to_copy;
    int       init_type;
    int       is_varobj = -1;
    int       is_heap_override_active = _PyParallel_IsHeapOverrideActive();

#define _INIT_NEW       1
#define _INIT_INIT      2
#define _INIT_RESIZE    3

    s = (Px_TLS_HEAP_ACTIVE ? &t->stats : &c->stats);

    if (!p) {
        /* Case 1: PyObject_NEW/NEW_VAR (via (Object|VarObject)_New). */
        init_type = _INIT_NEW;
        assert(tp);

        object_size = _Px_VSZ(tp, nitems);
        total_size  = _Px_SZ(object_size);
        n = (PyObject *)_PyHeap_Malloc(c, total_size, 0, 0);
        if (!n)
            return PyErr_NoMemory();
        x = _Px_X_PTR(n, object_size);
        o = _Px_O_PTR(n, object_size);

        Py_TYPE(n) = tp;
        if (is_varobj = (tp->tp_itemsize > 0)) {
            s->varobjs++;
            Py_SIZE(n) = nitems;
        } else
            s->objects++;

    } else {
        if (tp) {
            /* Case 2: PyObject_INIT/INIT_VAR called against manually
             * allocated memory (i.e. not allocated via PyObject_NEW). */
            init_type = _INIT_INIT;
            assert(tp);
            Px_GUARD_MEM(p);

            /* Need to manually allocate x + o storage. */
            x = (PxObject *)_PyHeap_Malloc(c, _Px_SZ(0), 0, 0);
            if (!x)
                return PyErr_NoMemory();

            o = _Px_O_PTR(x, 0);
            n = p;

            Py_TYPE(n) = tp;

            if (is_varobj = (tp->tp_itemsize > 0))
                Py_SIZE(n) = nitems;

            if (!Px_ISMIMIC(n)) {
                if (is_varobj)
                    s->varobjs++;
                else
                    s->objects++;
            }

        } else {
            /* Case 3: PyObject_GC_Resize called.  Object to resize may or may
             * not be from a parallel context.  Doesn't matter either way as
             * we don't really realloc anything behind the scenes -- we just
             * malloc another, larger chunk from our heap and copy over the
             * previous data. */
            init_type = _INIT_RESIZE;
            assert(!tp);
            assert(Py_TYPE(p) != NULL);
            is_varobj = 1;

            object_size = _Px_VSZ(tp, nitems);
            total_size  = _Px_SZ(object_size);
            n = (PyObject *)_PyHeap_Malloc(c, total_size, 0, 0);
            if (!n)
                return PyErr_NoMemory();
            x = _Px_X_PTR(n, object_size);
            o = _Px_O_PTR(n, object_size);

            /* Just do a blanket copy of everything rather than trying to
             * isolate the underlying VarObject ob_items.  It doesn't matter
             * if we pick up old pointers and whatnot (i.e. old px/is_px refs)
             * as all that stuff is initialized in the next section. */
            bytes_to_copy = _PyObject_VAR_SIZE(tp, Py_SIZE(p));

            assert(bytes_to_copy < object_size);
            assert(Py_SIZE(p) < nitems);

            memcpy(n, p, bytes_to_copy);

            Py_TYPE(n) = tp;
            Py_SIZE(n) = nitems;

            if (Py_PXOBJ(p)) {
                /* XXX do we really need to do this?  (Original line of
                 * thinking was that we might need to treat the object
                 * differently down the track (i.e. during cleanup) if
                 * it was resized.) */
                Py_ASPX(p)->resized_to = n;
                x->resized_from = p;
            }

            c->h->resizes++;
            s->resizes++;
        }
    }
    Py_REFCNT(n) = 1;

    assert(tp);
    assert(Py_TYPE(n) == tp);
    assert(is_varobj == 0 || is_varobj == 1);

    Py_EVENT(n) = NULL;
    Py_PXFLAGS(n) |= Py_PXFLAGS_ISPX;
    Py_ORIG_TYPE(n) = NULL;

    if (is_varobj)
        assert(Py_SIZE(n) == nitems);

    n->px = x;
    n->is_px = _Py_IS_PARALLEL;
    n->srw_lock = NULL;

    x->ctx = c;
    x->signature = _PxObjectSignature;

    if (Px_ISMIMIC(n))
        goto end;

    if (Px_TLS_HEAP_ACTIVE)
        goto end;

    if (is_heap_override_active) {
        Py_PXFLAGS(n) |= Py_PXFLAGS_CLONED;
        goto end;
    }

    o->op = n;
    append_object((is_varobj ? &c->varobjs : &c->objects), o);

    if (!c->ob_first) {
        c->ob_first = n;
        c->ob_last  = n;
        n->_ob_next = NULL;
        n->_ob_prev = NULL;
    } else {
        PyObject *last;
        assert(!c->ob_first->_ob_prev);
        assert(!c->ob_last->_ob_next);
        last = c->ob_last;
        last->_ob_next = n;
        n->_ob_prev = last;
        n->_ob_next = NULL;
        c->ob_last = n;
    }

end:
    return n;
}


PyObject *
Object_Init(PyObject *op, PyTypeObject *tp, Context *c)
{
    assert(tp->tp_itemsize == 0);
    return init_object(c, op, tp, 0);
}


PyObject *
Object_New(PyTypeObject *tp, Context *c)
{
    return init_object(c, NULL, tp, 0);
}


PyVarObject *
VarObject_Init(PyVarObject *v, PyTypeObject *tp, Py_ssize_t nitems, Context *c)
{
    assert(tp->tp_itemsize > 0);
    return (PyVarObject *)init_object(c, (PyObject *)v, tp, nitems);
}


PyVarObject *
VarObject_New(PyTypeObject *tp, Py_ssize_t nitems, Context *c)
{
    return (PyVarObject *)init_object(c, NULL, tp, nitems);
}

PyVarObject *
VarObject_Resize(PyObject *v, Py_ssize_t nitems, Context *c)
{
    return (PyVarObject *)init_object(c, v, NULL, nitems);
}

#ifdef Py_DEBUG
void *
_PxObject_Realloc(void *p, size_t nbytes)
{
    unsigned long o, m, s;
    Context *c = ctx;
    o = _Px_SafeObjectSignatureTest(p);
    m = _Px_MemorySignature(p);
    s = Px_MAX(o, m);
    if (Py_PXCTX) {
        void *r;
        if (s & _SIG_PY)
            printf("\n_PxObject_Realloc(Py_PXCTX && p = _SIG_PY)\n");
        r = _PyHeap_Realloc(c, p, nbytes);
        return r;
    } else {
        if (s & _SIG_PX)
            printf("\n_PxObject_Realloc(!Py_PXCTX && p = _SIG_PX)\n");
        return PyObject_Realloc(p, nbytes);
    }
    assert(0);
}

void
_PxObject_Free(void *p)
{
    unsigned long o, m, s;
    Context *c = ctx;
    if (!p)
        return;
    o = _Px_SafeObjectSignatureTest(p);
    m = _Px_MemorySignature(p);
    s = Px_MAX(o, m);
    if (Py_PXCTX) {
        if (!(s & _SIG_PX))
            printf("\n_PxObject_Free(Py_PXCTX && p != _SIG_PX)\n");
        else
            _PyHeap_Free(c, p);
    } else {
        if (s & _SIG_PX)
            printf("\n_PxObject_Free(!Py_PXCTX && p = _SIG_PX)\n");
        else
            PyObject_Free(p);
    }
}

#else
void *
_PxObject_Realloc(void *p, size_t nbytes)
{
    Px_GUARD
    return _PyHeap_Realloc(ctx, p, nbytes);
}

void
_PxObject_Free(void *p)
{
    Px_GUARD
    if (!p)
        return;
    _PyHeap_Free(ctx, p);
}
#endif


int
null_with_exc_or_non_none_return_type(PyObject *op, PyThreadState *tstate)
{
    if (!op && tstate->curexc_type)
        return 1;

    assert(!tstate->curexc_type);

    if ((!op && !tstate->curexc_type) || op == Py_None)
        return 0;

    Py_DECREF(op);
    PyErr_SetString(PyExc_ValueError, "non-None return value detected");
    return 1;
}


int
null_or_non_none_return_type(PyObject *op)
{
    if (!op)
        return 1;

    if (op == Py_None)
        return 0;

    Py_DECREF(op);
    PyErr_SetString(PyExc_ValueError, "non-None return value detected");
    return 1;
}



void
Px_INCCTX(Context *c)
{
    InterlockedIncrement(&(c->refcnt));
}

void
_PxState_ReleaseContext(PxState *px, Context *c)
{
    Context *last;
    assert(c->refcnt == 0);
    if (c->persisted_count > 0) {

        assert(Px_CTX_IS_PERSISTED(c));

        InterlockedIncrement(&(px->contexts_persisted));
        InterlockedDecrement(&(px->active));
        InterlockedDecrement(&(px->contexts_active));

        Px_CTXFLAGS(c) &= ~Px_CTXFLAGS_IS_PERSISTED;
        Px_CTXFLAGS(c) |=  Px_CTXFLAGS_WAS_PERSISTED;

        return;
    }

    assert(c->ttl >= 1 && c->ttl <= 4);
    assert(c->next == NULL);
    assert(c->prev == NULL);
    if (!px->ctx_first) {
        assert(!px->ctx_last);
        px->ctx_first = c;
        px->ctx_last = c;
        c->next = NULL;
    } else {
        assert(!px->ctx_first->prev);
        assert(!px->ctx_last->next);
        last = px->ctx_last;
        last->next = c;
        c->prev = last;
        c->next = NULL;
        px->ctx_last = c;
    }
}


long
Px_DECCTX(Context *c)
{
    PxState *px = c->px;
    InterlockedDecrement(&(c->refcnt));
    assert(c->refcnt >= 0);

    if (c->refcnt > 0)
        return c->refcnt;

    assert(c->refcnt == 0);
    _PxState_ReleaseContext(px, c);
    return 0;
}

int
_PxState_AllocIOBufs(PxState *px, Context *c, int count, int size)
{
    size_t   nbufs;
    size_t   bufsize;
    size_t   heapsize;
    size_t   all_io;
    size_t   all_bufs;
    size_t   iosize;
    void    *io_first;
    void    *buf_first;
    int      i;
    int      result = 0;
    PxIO    *io;
    char    *buf;

    assert(px);

    nbufs = count;
    bufsize = size;
    iosize = Px_MEM_ALIGN(sizeof(PxIO));

    all_io = nbufs * iosize;
    all_bufs = nbufs * bufsize;

    heapsize = all_io + all_bufs;

    c->heap_handle = HeapCreate(HEAP_NO_SERIALIZE, heapsize, 0);
    if (!c->heap_handle) {
        PyErr_SetFromWindowsErr(0);
        goto done;
    }

    if (!_PyHeap_Init(c, heapsize))
        goto free_heap;

    io_first = _PyHeap_Malloc(c, all_io, Px_MEM_ALIGN_SIZE, 1);
    if (!io_first)
        goto free_heap;

    do {
        buf_first = _PyHeap_Malloc(c, all_bufs, Px_MEM_ALIGN_SIZE, 1);
        if (buf_first)
            break;

        all_bufs -= bufsize;
        nbufs--;

    } while (all_bufs > 0);

    for (i = 0; i < nbufs; i++) {
        io =  (PxIO *)Px_PTR_ADD(io_first,  (i * iosize));
        buf = (char *)Px_PTR_ADD(buf_first, (i * bufsize));

        assert(Px_PTR(io) ==  Px_ALIGN(io,  Px_MEM_ALIGN_SIZE));
        assert(Px_PTR(buf) == Px_ALIGN(buf, Px_MEM_ALIGN_SIZE));

        io->size = (int)bufsize;
        io->buf  = buf;

        PxList_Push(px->io_free, E2I(io));
    }

    assert(PxList_QueryDepth(px->io_free) == nbufs);

    result = 1;
    goto done;

free_heap:
    HeapDestroy(c->heap_handle);

done:
    if (!result)
        assert(PyErr_Occurred());

    return result;
}

void *
_PyParallel_CreatedNewThreadState(PyThreadState *tstate)
{
    PxState *px;

    TSTATE = tstate;

    _PyParallel_RefreshMemoryLoad();

    px = (PxState *)malloc(sizeof(PxState));
    if (!px)
        return PyErr_NoMemory();

    memset((void *)px, 0, sizeof(PxState));

    px->errors = PxList_New();
    if (!px->errors)
        goto free_px;

    px->completed_callbacks = PxList_New();
    if (!px->completed_callbacks)
        goto free_errors;

    px->completed_errbacks = PxList_New();
    if (!px->completed_errbacks)
        goto free_completed_callbacks;

    px->new_threadpool_work = PxList_New();
    if (!px->new_threadpool_work)
        goto free_completed_errbacks;

    px->incoming = PxList_New();
    if (!px->incoming)
        goto free_new_threadpool_work;

    px->finished = PxList_New();
    if (!px->finished)
        goto free_incoming;

    px->finished_sockets = PxList_New();
    if (!px->finished_sockets)
        goto free_finished;

    px->io_free = PxList_New();
    if (!px->io_free)
        goto free_finished_sockets;

    px->work_ready = PxList_New();
    if (!px->work_ready)
        goto free_io_free;

    px->io_free_wakeup = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!px->io_free_wakeup)
        goto free_work_ready;

    px->wakeup = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!px->wakeup)
        goto free_io_wakeup;

    _PxState_InitPxPages(px);

    InitializeCriticalSectionAndSpinCount(&(px->cs), 12);

    tstate->px = px;
    px->tstate = tstate;

    tstate->is_parallel_thread = 0;
    px->ctx_ttl = 1;

    goto done;

free_io_wakeup:
    CloseHandle(px->io_free_wakeup);

free_work_ready:
    PxList_FreeListHead(px->work_ready);

free_io_free:
    PxList_FreeListHead(px->io_free);

free_finished_sockets:
    PxList_FreeListHead(px->finished_sockets);

free_finished:
    PxList_FreeListHead(px->finished);

free_incoming:
    PxList_FreeListHead(px->incoming);

free_new_threadpool_work:
    PxList_FreeListHead(px->new_threadpool_work);

free_completed_errbacks:
    PxList_FreeListHead(px->completed_errbacks);

free_completed_callbacks:
    PxList_FreeListHead(px->completed_callbacks);

free_errors:
    PxList_FreeListHead(px->errors);

free_px:
    free(px);
    px = NULL;

done:
    if (!px)
        PyErr_SetFromWindowsErr(0);

    return px;
}

void
_PyParallel_ClearingThreadState(PyThreadState *tstate)
{

}

PyObject* _async_run(PyObject *, PyObject *);

void
_PyParallel_DeletingThreadState(PyThreadState *tstate)
{
    PxState *px = (PxState *)tstate->px;

    assert(px);

    if (px->contexts_active > 0) {
        printf("_PyParallel_DeletingThreadState(): px->contexts_active: %d\n",
               px->contexts_active);
    }
}

void
_PyParallel_DeletingInterpreterState(PyInterpreterState *interp)
{

}

void
_PyParallel_InitializedThreadState(PyThreadState *pstate)
{
    //if (Py_MainThreadId != _Py_get_current_thread_id())
    //    PyEval_RestoreThread(pstate);
}


int
_PyParallel_ExecutingCallbackFromMainThread(void)
{
    PyThreadState *tstate;
    PxState       *px;
    Py_GUARD

    tstate = PyThreadState_GET();

    assert(!tstate->is_parallel_thread);
    px = (PxState *)tstate->px;
    return (px->processing_callback == 1);
}

PyObject *
PxState_SetError(Context *c)
{
    PxState *px = c->px;
    PyThreadState *pstate = c->pstate;
    assert(pstate->curexc_type != NULL);
    PxList_TimestampItem(c->error);
    c->error->from = c;
    c->error->p1 = pstate->curexc_type;
    c->error->p2 = pstate->curexc_value;
    c->error->p3 = pstate->curexc_traceback;
    InterlockedExchange(&(c->done), 1);
    /*
    InterlockedIncrement64(done);
    InterlockedDecrement(inflight);
    */
    PxList_Push(px->errors, c->error);
    SetEvent(px->wakeup);
    return NULL;
}

void
PxContext_HandleError(Context *c)
{
    PyObject *args, *r;
    PxState *px = c->px;
    PyThreadState *pstate = c->pstate;

    assert(pstate->curexc_type != NULL);

    if (c->errback) {
        PyObject *exc;
        assert(pstate->curexc_type);
        exc = PyTuple_Pack(3, pstate->curexc_type,
                              pstate->curexc_value,
                              pstate->curexc_traceback);
        if (!exc)
            goto error;

        PyErr_Clear();
        args = Py_BuildValue("(O)", exc);
        r = PyObject_CallObject(c->errback, args);
        if (!null_with_exc_or_non_none_return_type(r, pstate)) {
            c->errback_completed->from = c;
            PxList_TimestampItem(c->errback_completed);
            InterlockedExchange(&(c->done), 1);
            /*
            InterlockedIncrement64(done);
            InterlockedDecrement(inflight);
            */
            PxList_Push(px->completed_errbacks, c->errback_completed);
            SetEvent(px->wakeup);
            return;
        }
    }

error:
    PxState_SetError(c);
}

int
_PyParallel_InitTLS(void)
{
    int   i;
    TLS  *t = &tls;
    assert(_PxNewThread != 0);
    assert(!t->h);

    t->handle = HeapCreate(HEAP_NO_SERIALIZE, _PyTLSHeap_DefaultSize, 0);
    if (!t->handle)
        Py_FatalError("_PyParallel_InitTLSHeap:HeapCreate");

    TSTATE = ctx->tstate;
    t->px = (PxState *)TSTATE->px;
    assert(t->px);

    InitializeCriticalSectionAndSpinCount(&t->sbuf_cs, TLS_BUF_SPINCOUNT);
    InitializeCriticalSectionAndSpinCount(&t->rbuf_cs, TLS_BUF_SPINCOUNT);
    InitializeCriticalSectionAndSpinCount(&t->snapshots_cs, TLS_BUF_SPINCOUNT);

    if (!_PyTLSHeap_Init(0, 0))
        return 0;

    for (i = 0; i < Px_NUM_TLS_WSABUFS; i++) {
        Heap   *h  = &t->snapshot[i];
        TLSBUF *sb = &t->sbuf[i];
        TLSBUF *rb = &t->sbuf[i];
        WSABUF *sw = T2W(sb);
        WSABUF *rw = T2W(rb);

        h->bitmap_index  = i;
        sb->bitmap_index = i;
        rb->bitmap_index = i;

        h->tls  = t;
        sb->tls = t;
        rb->tls = t;

        assert(&sb->w == sw);
        assert(&rb->w == rw);

        t->sbufs[i]     = sw;
        t->rbufs[i]     = rw;
        t->snapshots[i] = h;
    }

    t->sbuf_bitmap      = ~0;
    t->rbuf_bitmap      = ~0;
    t->snapshots_bitmap = ~0;

    t->thread_id = _Py_get_current_thread_id();
    t->snapshot_id = 0;

    return 1;
}

void
_PyParallel_EnteredCallback(Context *c, PTP_CALLBACK_INSTANCE instance)
{
    Stats *s;
    ctx = c;

    if (_PxNewThread) {
        if (!_PyParallel_InitTLS())
            Py_FatalError("TLS heap initialization failed");
        _PxNewThread = 0;
    }

    assert(
        c->error                &&
        c->pstate               &&
        c->decrefs              &&
        c->outgoing             &&
        c->errback_completed    &&
        c->callback_completed
    );

    s = &(c->stats);
    s->entered = _Py_rdtsc();
    assert(c->tstate);
    assert(c->heap_handle);

    if (instance)
        c->instance = instance;

    c->pstate->thread_id = _Py_get_current_thread_id();
}

void
_PyParallel_EnteredIOCallback(
    Context *c,
    PTP_CALLBACK_INSTANCE instance,
    void *overlapped,
    ULONG io_result,
    ULONG_PTR nbytes,
    TP_IO *tp_io
)
{
    _PyParallel_EnteredCallback(c, instance);

    c->io_result = io_result;
    c->io_nbytes = nbytes;
}

void
_PyParallel_ExitingCallback(Context *c)
{
    c->stats.exited = _Py_rdtsc();
}

void
_PyParallel_ExitingIOCallback(Context *c)
{
    _PyParallel_ExitingCallback(c);
}

void
NTAPI
_PyParallel_WorkCallback(PTP_CALLBACK_INSTANCE instance, void *context)
{
    Context  *c = (Context *)context;
    Stats    *s;
    PxState  *px;
    PyObject *r;
    PyObject *func = NULL,
             *args = NULL,
             *kwds = NULL,
             *callback = NULL,
             *callback_args = NULL,
             *callback_kwds = NULL,
             *errback = NULL;

    PyThreadState *pstate;

    volatile long       *pending;
    volatile long       *inflight;
    volatile long long  *done;

    _PyParallel_EnteredCallback(c, instance);

    s = &(c->stats);
    px = (PxState *)c->tstate->px;

    if (c->tp_wait) {
        pending = &(px->waits_pending);
        inflight = &(px->waits_inflight);
        done = &(px->waits_done);
    } else if (c->tp_io) {
        pending = &(px->io_pending);
        inflight = &(px->io_inflight);
        done = &(px->io_done);
    } else if (c->tp_timer) {
        pending = &(px->timers_pending);
        inflight = &(px->timers_inflight);
        done = &(px->timers_done);
    } else {
        pending = &(px->pending);
        inflight = &(px->inflight);
        done = &(px->done);
    }

    InterlockedDecrement(pending);
    InterlockedIncrement(inflight);

    pstate = c->pstate;

    func = c->func;
    args = c->args;
    kwds = c->kwds;

    callback = c->callback;
    errback = c->errback;

    if (c->tp_wait) {
        assert(
            c->wait_result == WAIT_OBJECT_0 ||
            c->wait_result == WAIT_TIMEOUT  ||
            c->wait_result == WAIT_ABANDONED_0
        );
        if (c->wait_result == WAIT_OBJECT_0)
            goto start;
        else if (c->wait_result == WAIT_TIMEOUT) {
            PyErr_SetNone(PyExc_WaitTimeoutError);
            goto errback;
        } else {
            PyErr_SetFromWindowsErr(0);
            goto errback;
        }
    } /* else if (c->tp_io && c->io_type == Px_IOTYPE_FILE) {
        PxListItem *item;
        PyObject *obj;
        PxIO *io;
        ULONG_PTR nbytes;
        assert(
            (c->io_type & (PyAsync_IO_WRITE)) &&
            c->overlapped != NULL &&
            c->io != NULL
        );
        io = c->io;
        obj = io->obj;
        nbytes = c->io_nbytes;
        if (PxIO_IS_ONDEMAND(io))
            PxList_Push(px->io_ondemand, E2I(io));
        else {
            assert(PxIO_IS_PREALLOC(io));
            memset(&(io->overlapped), 0, sizeof(OVERLAPPED));
            io->obj = NULL;
            PxList_Push(px->io_free, E2I(io));
            SetEvent(px->io_free_wakeup);
        }

        item = _PyHeap_NewListItem(c);
        if (!item) {
            PyErr_NoMemory();
            goto error;
        }
        item->p1 = obj;
        PxList_Push(c->decrefs, item);

        if (c->io_result == NO_ERROR) {
            if (!c->callback)
                goto after_callback;
            args = PyTuple_Pack(2, obj, nbytes);
            goto start_callback;
        } else {
            PyErr_SetFromWindowsErr(c->io_result);
            goto errback;
        }
        assert(0);
    } else if (c->tp_io && c->io_type == Px_IOTYPE_SOCKET) {
        PxSocket *s = (PxSocket *)c->io_obj;
        switch (s->io_op) {
            case PxSocket_IO_CONNECT:
                READ_LOCK(s);
                func = s->connection_made;
                READ_UNLOCK(s);
                args = Py_BuildValue("(O)", s);
                break;
            case PxSocket_IO_READ:
                func = s->data_received;
                if (!func)
                    break;
        }
        READ_UNLOCK(s);

    } */ else if (c->tp_timer) {
        assert(0);
    }

start:
    s->start = _Py_rdtsc();
    c->result = PyObject_Call(c->func, c->args, c->kwds);
    s->end = _Py_rdtsc();

    if (c->result) {
        assert(!pstate->curexc_type);
        if (c->callback) {
            if (!args)
                args = Py_BuildValue("(O)", c->result);
            r = PyObject_CallObject(c->callback, args);
            if (null_with_exc_or_non_none_return_type(r, pstate))
                goto errback;
        }
        c->callback_completed->from = c;
        PxList_TimestampItem(c->callback_completed);
        InterlockedExchange(&(c->done), 1);
        InterlockedIncrement64(done);
        InterlockedDecrement(inflight);
        PxList_Push(px->completed_callbacks, c->callback_completed);
        SetEvent(px->wakeup);
        goto end;
    }

errback:
    if (c->errback) {
        PyObject *exc;
        assert(pstate->curexc_type);
        exc = PyTuple_Pack(3, pstate->curexc_type,
                              pstate->curexc_value,
                              pstate->curexc_traceback);
        if (!exc)
            goto error;

        PyErr_Clear();
        args = Py_BuildValue("(O)", exc);
        r = PyObject_CallObject(c->errback, args);
        if (!null_with_exc_or_non_none_return_type(r, pstate)) {
            c->errback_completed->from = c;
            PxList_TimestampItem(c->errback_completed);
            InterlockedExchange(&(c->done), 1);
            InterlockedIncrement64(done);
            InterlockedDecrement(inflight);
            PxList_Push(px->completed_errbacks, c->errback_completed);
            SetEvent(px->wakeup);
            goto end;
        }
    }

error:
    assert(pstate->curexc_type != NULL);
    PxList_TimestampItem(c->error);
    c->error->from = c;
    c->error->p1 = pstate->curexc_type;
    c->error->p2 = pstate->curexc_value;
    c->error->p3 = pstate->curexc_traceback;
    InterlockedExchange(&(c->done), 1);
    InterlockedIncrement64(done);
    InterlockedDecrement(inflight);
    PxList_Push(px->errors, c->error);
    SetEvent(px->wakeup);
end:
    _PyParallel_ExitingCallback(c);
}

void
NTAPI
_PyParallel_WaitCallback(
    PTP_CALLBACK_INSTANCE instance,
    void *context,
    PTP_WAIT wait,
    TP_WAIT_RESULT wait_result)
{
    Context  *c = (Context *)context;

    assert(wait == c->tp_wait);
    c->wait_result = wait_result;

    _PyParallel_WorkCallback(instance, c);
}

void
NTAPI
_PyParallel_IOCallback(
    PTP_CALLBACK_INSTANCE instance,
    void *context,
    void *overlapped,
    ULONG io_result,
    ULONG_PTR nbytes,
    TP_IO *tp_io
)
{
    Context *c = (Context *)context;
    assert(tp_io == c->tp_io);
    c->io_result = io_result;
    c->io_nbytes = nbytes;
    assert(overlapped == &(c->overlapped));
    /* c->io = OL2PxIO(c->overlapped);*/
    _PyParallel_WorkCallback(instance, c);
}

PyDoc_STRVAR(_async_cpu_count_doc,
"cpu_count() -> integer\n\n\
Return an integer representing the number of online logical CPUs,\n\
or -1 if this value cannot be established.");

#if defined(__DragonFly__) || \
    defined(__OpenBSD__)   || \
    defined(__FreeBSD__)   || \
    defined(__NetBSD__)    || \
    defined(__APPLE__)
int
_bsd_cpu_count(void)
{
    int err = -1;
    int ncpu = -1;
    int mib[4];
    size_t len = sizeof(int);
    mib[0] = CTL_HW;
    mib[1] = HW_NCPU;
    err = sysctl(mib, 2, &ncpu, &len, NULL, 0);
    if (!err)
        return ncpu;
    else
        return -1;
}
#endif

int
_cpu_count(void)
{
#ifdef MS_WINDOWS
    SYSTEM_INFO sysinfo;
    GetSystemInfo(&sysinfo);
    return sysinfo.dwNumberOfProcessors;
#elif __hpux
    return mpctl(MPC_GETNUMSPUS, NULL, NULL);
#ifndef _SC_NPROCESSORS_ONLN
#ifdef _SC_NPROC_ONLN /* IRIX */
#define _SC_NPROCESSORS_ONLN _SC_NPROC_ONLN
#endif
#endif /* ! defined(_SC_NPROCESSORS_ONLN) */
#elif defined(HAVE_SYSCONF) && defined(_SC_NPROCESSORS_ONLN)
    return sysconf(_SC_NPROCESSORS_ONLN);
#elif __APPLE__
    int err = -1;
    int ncpu = -1;
    size_t len = sizeof(int);
    err = sysctlnametomib("hw.logicalcpu", &ncpu, &len, NULL, 0);
    if (!err)
        return ncpu;
    else
        return _bsd_cpu_count();
#elif defined(__DragonFly__) || \
      defined(__OpenBSD__)   || \
      defined(__FreeBSD__)   || \
      defined(__NetBSD__)
    return _bsd_cpu_count();
#else
    return -1;
#endif
}

PyObject *
_async_cpu_count(PyObject *self)
{
    return PyLong_FromLong(_cpu_count());
}

void
_PyParallel_Init(void)
{
    _Py_sfence();

    if (Py_MainProcessId == -1) {
        if (Py_MainThreadId != -1)
            Py_FatalError("_PyParallel_Init: invariant failed: "  \
                          "Py_MainThreadId should also be -1 if " \
                          "Py_MainProcessId is -1.");
    }
    if (Py_MainThreadId == -1) {
        if (Py_MainProcessId != -1)
            Py_FatalError("_PyParallel_Init: invariant failed: "   \
                          "Py_MainProcessId should also be -1 if " \
                          "Py_MainThreadId is -1.");
    }

    if (Py_MainProcessId == -1) {
        Py_MainProcessId = GetCurrentProcessId();
        if (Py_MainProcessId != _Py_get_current_process_id())
            Py_FatalError("_PyParallel_Init: intrinsics failure: " \
                          "_Py_get_current_process_id() != "       \
                          "GetCurrentProcessId()");
    }

    if (Py_MainThreadId == -1) {
        Py_MainThreadId = GetCurrentThreadId();
        if (Py_MainThreadId != _Py_get_current_thread_id())
            Py_FatalError("_PyParallel_Init: intrinsics failure: " \
                          "_Py_get_current_thread_id() != "        \
                          "GetCurrentThreadId()");
    }

    _PxObjectSignature = (Px_PTR(_Py_rdtsc()) ^ Px_PTR(&_PxObjectSignature));
    _PxSocketSignature = (Px_PTR(_Py_rdtsc()) ^ Px_PTR(&_PxSocketSignature));
    _PxSocketBufSignature = (
        Px_PTR(_Py_rdtsc()) ^
        Px_PTR(&_PxSocketBufSignature)
    );

    Py_ParallelContextsEnabled = 0;
    _Py_lfence();
    _Py_clflush(&Py_MainThreadId);

    _PyParallel_NumCPUs = _cpu_count();
    if (!_PyParallel_NumCPUs)
        Py_FatalError("_PyParallel_Init: GetActiveProcessorCount() failed");

}

void
_PyParallel_Finalize(void)
{
    PxState *px = PXSTATE();

    assert(px);

    if (px->contexts_active > 0) {
        printf("_PyParallel_Finalize(): px->contexts_active: %d\n",
               px->contexts_active);
    }

    _PyParallel_Finalized = 1;
}

void
_PyParallel_ClearMainThreadId(void)
{
    _Py_sfence();
    Py_MainThreadId = 0;
    _Py_lfence();
    _Py_clflush(&Py_MainThreadId);
    //TSTATE = NULL;
}

void
_PyParallel_CreatedGIL(void)
{
    //_PyParallel_ClearMainThreadId();
}

void
_PyParallel_AboutToDropGIL(void)
{
    //_PyParallel_ClearMainThreadId();
}

void
_PyParallel_DestroyedGIL(void)
{
    //_PyParallel_ClearMainThreadId();
}

void
_PyParallel_JustAcquiredGIL(void)
{
    char buf[128], *fmt;

    _Py_lfence();

    if (Py_MainThreadId != 0) {
        fmt = "_PyParallel_JustAcquiredGIL: invariant failed: "   \
              "expected Py_MainThreadId to have value 0, actual " \
              "value: %d";
        (void)snprintf(buf, sizeof(buf), fmt, Py_MainThreadId);
        /*Py_FatalError(buf);*/
    }

    if (Py_MainProcessId == -1)
        Py_FatalError("_PyParallel_JustAcquiredGIL: Py_MainProcessId == -1");

    _Py_sfence();
    Py_MainThreadId = _Py_get_current_thread_id();
    _Py_lfence();
    _Py_clflush(&Py_MainThreadId);
    //TSTATE = \
    //    (PyThreadState *)_Py_atomic_load_relaxed(&_PyThreadState_Current);
}

void
_PyParallel_SetMainProcessId(long id)
{
    _Py_sfence();
    Py_MainProcessId = id;
    _Py_lfence();
    _Py_clflush(&Py_MainThreadId);
}

void
_PyParallel_ClearMainProcessId(void)
{
    _PyParallel_SetMainProcessId(0);
}

void
_PyParallel_RestoreMainProcessId(void)
{
    _PyParallel_SetMainProcessId(_Py_get_current_process_id());
}

void
_PyParallel_EnableParallelContexts(void)
{
    _Py_sfence();
    Py_ParallelContextsEnabled = 1;
    _Py_lfence();
    _Py_clflush(&Py_MainThreadId);
}

void
_PyParallel_DisableParallelContexts(void)
{
    _Py_sfence();
    Py_ParallelContextsEnabled = 0;
    _Py_lfence();
    _Py_clflush(&Py_MainThreadId);
}

void
_PyParallel_NewThreadState(PyThreadState *tstate)
{
    return;
}

/* mod _parallel */
PyObject *
_parallel_map(PyObject *self, PyObject *args)
{
    return NULL;
}

PyDoc_STRVAR(_parallel_doc,
"_parallel module.\n\
\n\
Functions:\n\
\n\
map()\n");

PyDoc_STRVAR(_parallel_map_doc,
"map(callable, iterable) -> list\n\
\n\
Calls ``callable`` with each item in ``iterable``.\n\
Returns a list of results.");

#define _METHOD(m, n, a) {#n, (PyCFunction)##m##_##n##, a, m##_##n##_doc }
#define _PARALLEL(n, a) _METHOD(_parallel, n, a)
#define _PARALLEL_N(n) _PARALLEL(n, METH_NOARGS)
#define _PARALLEL_O(n) _PARALLEL(n, METH_O)
#define _PARALLEL_V(n) _PARALLEL(n, METH_VARARGS)
static PyMethodDef _parallel_methods[] = {
    _PARALLEL_V(map),

    { NULL, NULL } /* sentinel */
};

static struct PyModuleDef _parallelmodule = {
    PyModuleDef_HEAD_INIT,
    "_parallel",
    _parallel_doc,
    -1, /* multiple "initialization" just copies the module dict. */
    _parallel_methods,
    NULL,
    NULL,
    NULL,
    NULL
};

PyObject *
_PyParallel_ModInit(void)
{
    PyObject *m;

    m = PyModule_Create(&_parallelmodule);
    if (m == NULL)
        return NULL;

    return m;
}

/* xlist */
void
xlist_dealloc(PyXListObject *xlist)
{
    HeapDestroy(xlist->heap_handle);
    free(xlist);
}

PyObject *
PyXList_New(void)
{
    PyXListObject *xlist;

    if (Py_PXCTX) {
        PyErr_SetString(PyExc_RuntimeError,
                        "xlist objects cannot be "
                        "created from parallel threads");
        return NULL;
    }

    xlist = (PyXListObject *)malloc(sizeof(PyXListObject));
    if (!xlist)
        return PyErr_NoMemory();

    memset(xlist, 0, sizeof(PyXListObject));

    xlist->heap_handle = HeapCreate(0, 0, 0);
    if (!xlist->heap_handle) {
        PyErr_SetFromWindowsErr(0);
        free(xlist);
        return NULL;
    }

    xlist->head = PxList_NewFromHeap(xlist->heap_handle);
    if (!xlist->head) {
        free(xlist);
        return NULL;
    }

    /* Manually initialize the type. */
    xlist->ob_base.ob_type = &PyXList_Type;
    xlist->ob_base.ob_refcnt = 1;

    InitializeCriticalSectionAndSpinCount(&(xlist->cs), 4);

    return (PyObject *)xlist;
}

PyObject *
xlist_new(PyTypeObject *tp, PyObject *args, PyObject *kwds)
{
    assert(tp == &PyXList_Type);
    return PyXList_New();
}

PyObject *
xlist_alloc(PyTypeObject *tp, Py_ssize_t nitems)
{
    assert(nitems == 0);
    assert(tp == &PyXList_Type);

    return PyXList_New();
}

PyObject *
xlist_pop(PyObject *self, PyObject *args)
{
    PyXListObject *xlist = (PyXListObject *)self;
    PxListItem *item;
    PyObject *obj;
    assert(args == NULL);
    Py_GUARD
    /*Py_INCREF(xlist);*/
    item = PxList_Pop(xlist->head);
    obj = (item ? I2O(item) : NULL);
    if (obj)
        Py_REFCNT(obj) = 1;
    return obj;
}

PyObject *
PyXList_Pop(PyObject *xlist)
{
    return xlist_pop(xlist, NULL);
}

PyObject *
PyObject_Clone(PyObject *src, const char *errmsg)
{
    int valid_type;
    PyObject *result = NULL;
    PyTypeObject *tp;

    tp = Py_TYPE(src);

    valid_type = (
        PyBytes_CheckExact(src)         ||
        PyByteArray_CheckExact(src)     ||
        PyUnicode_CheckExact(src)       ||
        PyLong_CheckExact(src)          ||
        PyFloat_CheckExact(src)
    );

    if (!valid_type) {
        PyErr_Format(PyExc_ValueError, errmsg, tp->tp_name);
        return NULL;
    }

    assert(_PyParallel_IsHeapOverrideActive());

    if (PyLong_CheckExact(src)) {
        result = _PyLong_Copy((PyLongObject *)src);

    } else if (PyFloat_CheckExact(src)) {
        result = _PxObject_Init(NULL, &PyFloat_Type);
        if (!result)
            return NULL;
        PyFloat_AS_DOUBLE(result) = PyFloat_AS_DOUBLE(src);

    } else if (PyUnicode_CheckExact(src)) {
        result = _PyUnicode_Copy(src);

    } else {
        assert(0);
    }

    assert(result);
    assert(Px_CLONED(result));

    return result;
}

PyObject *
xlist_push(PyObject *obj, PyObject *src)
{
    PyXListObject *xlist = (PyXListObject *)obj;
    assert(src);

    /*Py_INCREF(xlist);*/
    /*Py_INCREF(src);*/

    if (!Py_PXCTX)
        PxList_PushObject(xlist->head, src);
    else {
        PyObject *dst;
        _PyParallel_SetHeapOverride(xlist->heap_handle);
        dst = PyObject_Clone(src, "objects of type %s cannot "
                                  "be pushed to xlists");
        _PyParallel_RemoveHeapOverride();
        if (!dst)
            return NULL;

        PxList_PushObject(xlist->head, dst);
    }

    /*
    if (Px_CV_WAITERS(xlist))
        ConditionVariableWakeOne(&(xlist->cv));
    */

    Py_RETURN_NONE;
}

PyObject *
xlist_flush(PyObject *self, PyObject *arg)
{
    Py_RETURN_NONE;
}

Py_ssize_t
PyXList_Length(PyObject *self)
{
    PyXListObject *xlist = (PyXListObject *)self;
    Py_INCREF(xlist);
    return PxList_QueryDepth(xlist->head);
}

PyDoc_STRVAR(xlist_pop_doc,   "XXX TODO\n");
PyDoc_STRVAR(xlist_push_doc,  "XXX TODO\n");
PyDoc_STRVAR(xlist_size_doc,  "XXX TODO\n");
PyDoc_STRVAR(xlist_flush_doc, "XXX TODO\n");
#define _XLIST(n, a) _METHOD(xlist, n, a)
#define _XLIST_N(n) _XLIST(n, METH_NOARGS)
#define _XLIST_O(n) _XLIST(n, METH_O)
#define _XLIST_V(n) _XLIST(n, METH_VARARGS)
#define _XLIST_K(n) _XLIST(n, METH_VARARGS | METH_KEYWORDS)
static PyMethodDef xlist_methods[] = {
    _XLIST_N(pop),
    _XLIST_O(push),
    _XLIST_N(flush),
    { NULL, NULL }
};

static PySequenceMethods xlist_as_sequence = {
    (lenfunc)PyXList_Length,                    /* sq_length */
    0,                                          /* sq_concat */
    0,                                          /* sq_repeat */
    0,                                          /* sq_item */
    0,                                          /* sq_slice */
    0,                                          /* sq_ass_item */
    0,                                          /* sq_ass_slice */
    0,                                          /* sq_contains */
    0,                                          /* sq_inplace_concat */
    0,                                          /* sq_inplace_repeat */
};


static PyTypeObject PyXList_Type = {
    PyObject_HEAD_INIT(0)
    "xlist",
    sizeof(PyXListObject),
    0,
    (destructor)xlist_dealloc,                  /* tp_dealloc */
    0,                                          /* tp_print */
    0,                                          /* tp_getattr */
    0,                                          /* tp_setattr */
    0,                                          /* tp_reserved */
    0,                                          /* tp_repr */
    0,                                          /* tp_as_number */
    &xlist_as_sequence,                         /* tp_as_sequence */
    0,                                          /* tp_as_mapping */
    0,                                          /* tp_hash */
    0,                                          /* tp_call */
    0,                                          /* tp_str */
    PyObject_GenericGetAttr,                    /* tp_getattro */
    0,                                          /* tp_setattro */
    0,                                          /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT,                         /* tp_flags */
    "Interlocked List Object",                  /* tp_doc */
    0,                                          /* tp_traverse */
    0,                                          /* tp_clear */
    0,                                          /* tp_richcompare */
    0,                                          /* tp_weaklistoffset */
    0,                                          /* tp_iter */
    0,                                          /* tp_iternext */
    xlist_methods,                              /* tp_methods */
    0,                                          /* tp_members */
    0,                                          /* tp_getset */
    0,                                          /* tp_base */
    0,                                          /* tp_dict */
    0,                                          /* tp_descr_get */
    0,                                          /* tp_descr_set */
    0,                                          /* tp_dictoffset */
    0,/*(initproc)xlist_init,*/                 /* tp_init */
    xlist_alloc,                                /* tp_alloc */
    xlist_new,                                  /* tp_new */
    0,                                          /* tp_free */
};


/* mod _async */

int
_is_active_ex(void)
{
    PyThreadState *tstate = get_main_thread_state();
    PxState *px = (PxState *)tstate->px;
    int rv;

    if (!TryEnterCriticalSection(&(px->cs)))
        return 1;

    rv = !(px->ctx_first == NULL &&
           px->done == px->last_done_count &&
           px->submitted == px->last_submitted_count &&
           px->pending == 0 &&
           px->inflight == 0 &&
           px->sync_wait_submitted == px->last_sync_wait_submitted_count &&
           px->sync_wait_pending == 0 &&
           px->sync_wait_inflight == 0 &&
           px->sync_wait_done == px->last_sync_wait_done_count &&
           px->sync_nowait_submitted == px->last_sync_nowait_submitted_count &&
           px->sync_nowait_pending == 0 &&
           px->sync_nowait_inflight == 0 &&
           px->sync_nowait_done == px->last_sync_nowait_done_count &&
           PxList_QueryDepth(px->errors)   == 0 &&
           PxList_QueryDepth(px->finished) == 0 &&
           PxList_QueryDepth(px->incoming) == 0 &&
           PxList_QueryDepth(px->completed_errbacks) == 0 &&
           PxList_QueryDepth(px->completed_callbacks) == 0);

    LeaveCriticalSection(&(px->cs));
    return rv;
}


int
_is_parallel_thread(void)
{
    return PyThreadState_GET()->is_parallel_thread;
}

PyObject *
_async_is_parallel_thread(void)
{
    PyObject *r = (PyObject *)(_is_parallel_thread() ? Py_True : Py_False);
    Py_INCREF(r);
    return r;
}


unsigned long long
_rdtsc(void)
{
    return _Py_rdtsc();
}

PyObject *
_async_rdtsc(void)
{
    return PyLong_FromUnsignedLongLong(_Py_rdtsc());
}


long
_is_active(void)
{
    return PXSTATE()->active;
}

PyObject *
_async_is_active(PyObject *self, PyObject *args)
{
    PyObject *r = (PyObject *)(_is_active() ? Py_True : Py_False);
    Py_INCREF(r);
    return r;
}

PyObject *
_async_is_active_ex(PyObject *self, PyObject *args)
{
    PyObject *r = (PyObject *)(_is_active_ex() ? Py_True : Py_False);
    Py_INCREF(r);
    return r;
}


PyObject *
_async_active_count(PyObject *self, PyObject *args)
{
    return PyLong_FromLong(PXSTATE()->active);
}

PyObject *
_async_active_contexts(PyObject *self, PyObject *args)
{
    return PyLong_FromLong(PXSTATE()->contexts_active);
}

PyObject *
_async_persisted_contexts(PyObject *self, PyObject *args)
{
    return PyLong_FromLong(PXSTATE()->contexts_persisted);
}


void
incref_args(Context *c)
{
    Py_INCREF(c->func);
    Py_XINCREF(c->args);
    Py_XINCREF(c->kwds);
    Py_XINCREF(c->callback);
    Py_XINCREF(c->errback);
}


void
incref_waitobj_args(Context *c)
{
    Py_INCREF(c->waitobj);
    Py_INCREF(c->waitobj_timeout);
    incref_args(c);
}



void
decref_args(Context *c)
{
    Py_XDECREF(c->func);
    Py_XDECREF(c->args);
    Py_XDECREF(c->kwds);
    Py_XDECREF(c->callback);
    Py_XDECREF(c->errback);
}


void
decref_waitobj_args(Context *c)
{
    Py_DECREF(c->waitobj);
    Py_DECREF(c->waitobj_timeout);
}

Context *
_PxState_FreeContext(PxState *px, Context *c)
{
    Heap *h;
    Stats *s;
    Object *o;
    PxListItem *item;
    Context *prev, *next;

    assert(c->px == px);

    prev = c->prev;
    next = c->next;

    if (px->ctx_first == c)
        px->ctx_first = next;

    if (px->ctx_last == c)
        px->ctx_last = prev;

    if (prev)
        prev->next = next;

    if (next)
        next->prev = prev;

    /* xxx todo: check refcnts of func/args/kwds etc? */
    decref_args(c);

    if (c->tp_wait)
        decref_waitobj_args(c);

    h = c->h;
    s = &(c->stats);
    _PyHeap_FastFree(h, s, c->error);
    _PyHeap_FastFree(h, s, c->errback_completed);
    _PyHeap_FastFree(h, s, c->callback_completed);
    _PyHeap_FastFree(h, s, c->outgoing);

    if (c->last_leak)
        free(c->last_leak);

    if (c->errors_tuple)
        _PyHeap_FastFree(h, s, c->errors_tuple);

    item = PxList_Flush(c->decrefs);
    while (item) {
        PxListItem *next = PxList_Next(item);
        Py_XDECREF((PyObject *)item->p1);
        Py_XDECREF((PyObject *)item->p2);
        Py_XDECREF((PyObject *)item->p3);
        Py_XDECREF((PyObject *)item->p4);
        _PyHeap_Free(c, item);
        item = next;
    }

    for (o = c->events.first; o; o = o->next) {
        assert(Py_HAS_EVENT(o));
        assert(Py_EVENT(o));
        PyEvent_DESTROY(o);
    }

    px->contexts_destroyed++;

    if (!Px_CTX_WAS_PERSISTED(c)) {
        InterlockedDecrement(&(px->active));
        InterlockedDecrement(&(px->contexts_active));
    }

    /*
    if (c->io_obj) {
        if (Py_TYPE(c->io_obj) == &PxSocket_Type) {
            PxSocket *s = (PxSocket *)c->io_obj;
            if (c->tp_io)
                CancelThreadpoolIo(c->tp_io);
        }
        Py_DECREF(c->io_obj);
    }
    */

    _PxContext_UnregisterHeaps(c);

    HeapDestroy(c->heap_handle);
    free(c);
    return next;
}

int
_PxState_PurgeContexts(PxState *px)
{
    Context *c;
    int destroyed = 0;

    if (!px->ctx_first)
        return 0;

    c = px->ctx_first;
    while (c) {
        if (c->ttl > 0) {
            --(c->ttl);
            c = c->next;
            continue;
        }
        assert(c->ttl == 0);

        c = _PxState_FreeContext(px, c);
        destroyed++;
    }

    return destroyed;
}

BOOL
_Py_HandleCtrlC(DWORD ctrltype)
{
    if (ctrltype == CTRL_C_EVENT) {
        _Py_sfence();
        _Py_CtrlCPressed = 1;
        _Py_lfence();
        _Py_clflush(&_Py_CtrlCPressed);
        return TRUE;
    }
    return FALSE;
}
PHANDLER_ROUTINE _Py_CtrlCHandlerRoutine = (PHANDLER_ROUTINE)_Py_HandleCtrlC;

int
_Py_CheckCtrlC(void)
{
    Py_GUARD

    if (_Py_CtrlCPressed) {
        _Py_CtrlCPressed = 0;
        PyErr_SetNone(PyExc_KeyboardInterrupt);
        return 1;
    }

    return 0;
}

void
_PyParallel_SchedulePyNoneDecref(long refs)
{
    PxState *px = PXSTATE();
    assert(refs > 0);
    InterlockedAdd(&(px->incoming_pynone_decrefs), refs);
}

PyObject *
_async_run_once(PyObject *self, PyObject *args)
{
    int err = 0;
    int wait = -1;
    int purged = 0;
    unsigned short depth_hint = 0;
    unsigned int waited = 0;
    unsigned int depth = 0;
    unsigned int events = 0;
    unsigned int errors = 0;
    unsigned int processed_errors = 0;
    unsigned int processed_finished = 0;
    unsigned int processed_incoming = 0;
    unsigned int processed_errbacks = 0;
    unsigned int processed_callbacks = 0;
    PyObject *result = NULL;
    Context *c;
    PxState *px;
    PxListItem *item = NULL;
    PyThreadState *tstate;
    PyFrameObject *old_frame;
    Py_GUARD

    if (PyErr_CheckSignals() || _Py_CheckCtrlC())
        return NULL;

    if (!_Py_InstalledCtrlCHandler) {
        if (!SetConsoleCtrlHandler(_Py_CtrlCHandlerRoutine, TRUE)) {
            PyErr_SetFromWindowsErr(0);
            return NULL;
        }
        _Py_InstalledCtrlCHandler = 1;
    }

    _PyParallel_RefreshMemoryLoad();

    tstate = get_main_thread_state();

    px = (PxState *)tstate->px;

    if (px->submitted == 0 &&
        px->waits_submitted == 0 &&
        px->persistent == 0 &&
        px->contexts_persisted == 0 &&
        px->contexts_active == 0)
    {
        PyErr_SetNone(PyExc_AsyncRunCalledWithoutEventsError);
        return NULL;
    }

    if (px->incoming_pynone_decrefs) {
        long r = InterlockedExchange(&(px->incoming_pynone_decrefs), 0);
        assert(r >= 0);
        if (r > 0) {
            PyObject *o = Py_None;
            assert((Py_REFCNT(o) - r) > 0);
            o->ob_refcnt -= r;
        }
    }

    px->last_done_count = px->done;
    px->last_submitted_count = px->submitted;

    px->last_sync_wait_done_count = px->sync_wait_done;;
    px->last_sync_wait_submitted_count = px->sync_wait_submitted;

    px->last_sync_nowait_done_count = px->sync_nowait_done;;
    px->last_sync_nowait_submitted_count = px->sync_nowait_submitted;

    purged = _PxState_PurgeContexts(px);

    item = PxList_Flush(px->finished);
    while (item) {
        ++processed_finished;
        c = (Context *)item->from;
        c->times_finished++;

        assert(!c->io_obj);

        item = (Px_DECCTX(c) ?
            PxList_Transfer(px->finished, item) :
            PxList_SeverFromNext(item)
        );
    }

start:
    if (PyErr_CheckSignals())
        return NULL;
    /* First error wins. */
    item = PxList_Pop(px->errors);
    if (item) {
        c = item->from;
        assert(PyExceptionClass_Check((PyObject *)item->p1));
        PyErr_Restore((PyObject *)item->p1,
                      (PyObject *)item->p2,
                      (PyObject *)item->p3);

        /* Ugh, so hacky.  If our originating context is an I/O object, don't
         * treat the context as 'finished'. */
        if (!c->io_obj) {
            PxList_Transfer(px->finished, item);
            InterlockedIncrement64(&(px->done));
        }
        return NULL;
    }

    /* New threadpool work. */
    while (item = PxList_Pop(px->new_threadpool_work)) {
        PTP_SIMPLE_CALLBACK cb;
        c = (Context *)item->from;
        cb = (PTP_SIMPLE_CALLBACK)item->p1;

        if (!TrySubmitThreadpoolCallback(cb, c, NULL)) {
            PyErr_SetFromWindowsErr(0);
            Py_XDECREF(item->p2);
            Py_XDECREF(item->p3);
            Py_XDECREF(item->p4);
            return NULL;
        }
    }

    assert(px->processing_callback == 0);
    /* Process incoming work items. */
    old_frame = ((PyFrameObject *)(tstate->frame));
    ((PyFrameObject *)(tstate->frame)) = NULL;
    while (item = PxList_Pop(px->incoming)) {
        HANDLE wait;
        PyObject *func, *args, *kwds, *result;

        px->processing_callback = 1;

        func = (PyObject *)item->p1;
        args = (PyObject *)item->p2;
        kwds = (PyObject *)item->p3;
        wait = (HANDLE)item->p4;
        c = (Context *)item->from;

        if (wait) {
            InterlockedDecrement(&(px->sync_wait_pending));
            InterlockedIncrement(&(px->sync_wait_inflight));
        } else {
            InterlockedDecrement(&(px->sync_nowait_pending));
            InterlockedIncrement(&(px->sync_nowait_inflight));
        }

        if (kwds)
            assert(PyDict_CheckExact(kwds));

        result = PyObject_Call(func, args, kwds);

        ++processed_incoming;

        if (wait) {
            PxListItem *d;
            d = c->decref;
            assert(
                d &&
                d->p1 == NULL &&
                d->p2 == NULL &&
                d->p3 == NULL &&
                d->p4 == NULL
            );

            if (!result) {
                PyObject *exc_type, *exc_value, *exc_tb;

                assert(tstate->curexc_type);

                PyErr_Fetch(&exc_type, &exc_value, &exc_tb);

                assert(exc_type);
                assert(exc_value);
                assert(exc_tb);

                item->p1 = d->p1 = exc_type;
                item->p2 = d->p2 = exc_value;
                item->p3 = d->p3 = exc_tb;

                PyErr_Clear();

            } else {
                item->p1 = NULL;
                item->p2 = d->p1 = result;
                item->p3 = NULL;
            }
            PxList_Push(c->decrefs, d);
            c->decref = NULL;
            SetEvent(wait);
        } else {
            InterlockedDecrement(&(px->sync_nowait_inflight));
            InterlockedIncrement64(&(px->sync_nowait_done));

            if (!result) {
                assert(tstate->curexc_type != NULL);
            } else if (result != Py_None) {
                char *msg = "async call from main thread returned non-None";
                PyErr_WarnEx(PyExc_RuntimeWarning, msg, 1);
            }
            Py_XDECREF(result);

            c = (Context *)item->from;
            /* More hacks to persist socket/IO objects. */
            if (c->io_obj)
                continue;

            Px_DECCTX(c);
            _PyHeap_Free(c, item);

            if (!result) {
                px->processing_callback = 0;
                return NULL;
            }
        }
    }
    px->processing_callback = 0;
    ((PyFrameObject *)(tstate->frame)) = old_frame;


    /* Process completed items. */
    item = PxList_Flush(px->completed_callbacks);
    if (item) {
        do {
            /* XXX TODO: update stats. */
            ++processed_callbacks;
            item = PxList_Transfer(px->finished, item);
        } while (item);
    }

    item = PxList_Flush(px->completed_errbacks);
    if (item) {
        do {
            /* XXX TODO: update stats. */
            ++processed_errbacks;
            item = PxList_Transfer(px->finished, item);
        } while (item);
    }

    if (px->contexts_active == 0 || purged)
        Py_RETURN_NONE;

    /* Return if we've done something useful... */
    if (processed_errors    ||
        processed_finished  ||
        processed_incoming  ||
        processed_errbacks  ||
        processed_callbacks)
            Py_RETURN_NONE;

    /* ...and wait for a second if we haven't. */
    err = WaitForSingleObject(px->wakeup, 1000);
    switch (err) {
        case WAIT_OBJECT_0:
            goto start;
        case WAIT_TIMEOUT:
            Py_RETURN_NONE;
        case WAIT_ABANDONED:
            PyErr_SetString(PyExc_SystemError, "wait abandoned");
            break;
        case WAIT_FAILED:
            PyErr_SetFromWindowsErr(0);
            break;
    }
    return NULL;
}

PyObject *
_async_map(PyObject *self, PyObject *args)
{
    PyObject *result = NULL;

    return result;
}

int
extract_args(PyObject *args, Context *c)
{
    if (!PyArg_UnpackTuple(
            args, "", 1, 5,
            &(c->func), &(c->args), &(c->kwds),
            &(c->callback), &(c->errback)))
        return 0;

    if (c->callback == Py_None) {
        Py_DECREF(c->callback);
        c->callback = NULL;
    }

    if (c->errback == Py_None) {
        Py_DECREF(c->errback);
        c->errback = NULL;
    }

    if (c->args == Py_None) {
        Py_DECREF(c->args);
        c->args = Py_BuildValue("()");
    }

    if (c->kwds == Py_None) {
        Py_DECREF(c->kwds);
        c->kwds = NULL;
    }

    if (c->args && !PyTuple_Check(c->args)) {
        PyObject *tmp = c->args;
        c->args = Py_BuildValue("(O)", c->args);
        Py_DECREF(tmp);
    }

    return 1;
}

int
extract_waitobj_args(PyObject *args, Context *c)
{
    if (!PyArg_UnpackTuple(
            args, "", 2, 7,
            &(c->waitobj),
            &(c->waitobj_timeout),
            &(c->func),
            &(c->args),
            &(c->kwds),
            &(c->callback),
            &(c->errback)))
        return 0;

    if (c->waitobj_timeout != Py_None) {
        PyErr_SetString(PyExc_ValueError, "non-None value for timeout");
        return 0;
    }

    if (c->callback == Py_None) {
        Py_DECREF(c->callback);
        c->callback = NULL;
    }

    if (c->errback == Py_None) {
        Py_DECREF(c->errback);
        c->errback = NULL;
    }

    if (c->args == Py_None) {
        Py_DECREF(c->args);
        c->args = Py_BuildValue("()");
    }

    if (c->kwds == Py_None) {
        Py_DECREF(c->kwds);
        c->kwds = NULL;
    }

    if (c->args && !PyTuple_Check(c->args)) {
        PyObject *tmp = c->args;
        c->args = Py_BuildValue("(O)", c->args);
        Py_DECREF(tmp);
    }

    return 1;
}


int
submit_work(Context *c)
{
    int retval;
    PTP_SIMPLE_CALLBACK cb = _PyParallel_WorkCallback;
    if (Py_PXCTX) {
        assert(c->instance);
        cb(c->instance, c);
        return 1;
    } else {
        retval = TrySubmitThreadpoolCallback(cb, c, NULL);
        if (!retval)
            PyErr_SetFromWindowsErr(0);
        return retval;
    }
}

Context *
new_context(size_t heapsize, int init_heap_snapshots)
{
    int i;
    PxState  *px;
    Stats *s;
    PyThreadState *pstate;
    Context *c;

    if (_PyParallel_HitHardMemoryLimit())
        return (Context *)PyErr_NoMemory();

    c = (Context *)malloc(sizeof(Context));

    if (!c)
        return (Context *)PyErr_NoMemory();

    memset((void *)c, 0, sizeof(Context));

    c->heap_handle = HeapCreate(HEAP_NO_SERIALIZE, Px_DEFAULT_HEAP_SIZE, 0);
    if (!c->heap_handle) {
        PyErr_SetFromWindowsErr(0);
        goto free_context;
    }

    c->tstate = get_main_thread_state();

    assert(c->tstate);
    px = c->px = (PxState *)c->tstate->px;

    if (!_PyHeap_Init(c, heapsize))
        goto free_heap;

    c->refcnt = 1;
    c->ttl = px->ctx_ttl;

    pstate = (PyThreadState *)_PyHeap_Malloc(c, sizeof(PyThreadState), 0, 0);
    c->pstate = pstate;

    c->error = _PyHeap_NewListItem(c);
    c->errback_completed = _PyHeap_NewListItem(c);
    c->callback_completed = _PyHeap_NewListItem(c);

    c->outgoing = _PyHeap_NewList(c);
    c->decrefs  = _PyHeap_NewList(c);

    if (!(c->error                &&
          c->pstate               &&
          c->decrefs              &&
          c->outgoing             &&
          c->errback_completed    &&
          c->callback_completed))
            goto free_heap;

    pstate->px = c;
    pstate->is_parallel_thread = 1;
    pstate->interp = c->tstate->interp;

    //c->px->contexts_created++;
    InterlockedIncrement64(&(c->px->contexts_created));
    InterlockedIncrement(&(c->px->contexts_active));

    if (!init_heap_snapshots)
        goto done;

    InitializeCriticalSectionAndSpinCount(&c->heap_cs, TLS_BUF_SPINCOUNT);
    InitializeCriticalSectionAndSpinCount(&c->snapshots_cs, TLS_BUF_SPINCOUNT);

    for (i = 0; i < Px_INTPTR_BITS; i++) {
        Heap *h  = &c->snapshot[i];
        h->bitmap_index  = i;
        c->snapshots[i] = h;
    }

    c->snapshots_bitmap = ~0;

done:
    s = &(c->stats);
    s->startup_size = s->allocated;

    return c;

free_heap:
    HeapDestroy(c->heap_handle);

free_context:
    free(c);

    return NULL;
}

PyObject *
_async_submit_work(PyObject *self, PyObject *args)
{
    PyObject *result = NULL;
    Context  *c;
    PxState  *px;
    PxListItem *item;

    c = new_context(0, 0);
    if (!c)
        return NULL;

    px = c->px;

    if (!extract_args(args, c))
        goto free_context;

    item = _PyHeap_NewListItem(c);
    if (!item)
        goto free_context;

    item->from = c;

    InterlockedIncrement64(&(px->submitted));
    InterlockedIncrement(&(px->pending));
    InterlockedIncrement(&(px->active));
    c->stats.submitted = _Py_rdtsc();

    if (!submit_work(c))
        goto error;

    incref_args(c);

    result = (Py_INCREF(Py_None), Py_None);
    goto done;

error:
    InterlockedDecrement(&(c->px->contexts_active));
    InterlockedDecrement(&(px->pending));
    InterlockedDecrement(&(px->active));
    InterlockedIncrement64(&(px->done));
    decref_args(c);

free_context:
    HeapDestroy(c->heap_handle);
    free(c);

done:
    if (!result)
        assert(PyErr_Occurred());
    return result;
}

/*
PyObject *
_async_socket(PyObject *self, PyObject *args)
{
    PyObject *result = NULL;
    PySocketSockObject *sock;

    if (!PyArg_ParseTuple(args, "O!:socket", PySocketModule.Sock_Type, &sock))
        return NULL;

    return sock;
}
*/

PyObject *
_async_enabled(PyObject *self, PyObject *args)
{
    /*
    if (!PyArg_ParseTuple(args, "O!:socket", PySocketModule.Sock_Type, &sock))
        return NULL;
        */
    return NULL;
}

/*
PyObject *
_async_socket(PyObject *self, PyObject *args)
{
    PyObject *result = NULL;
    return result;
}
*/


PyObject *
_async_run(PyObject *self, PyObject *args)
{
    PyThreadState *tstate = get_main_thread_state();
    PxState *px = PXSTATE();
    int i = 0;
    long active_contexts = 0;
    long persisted_contexts = 0;
    do {
        i++;
        active_contexts = px->contexts_active;
        persisted_contexts = px->contexts_persisted;
        if (Py_VerboseFlag)
            PySys_FormatStdout("_async.run(%d) [%d/%d] "
                               "(hogs: %d, ioloops: %d)\n",
                               i, active_contexts, persisted_contexts,
                               _PxSocket_ActiveHogs, _PxSocket_ActiveIOLoops);
        assert(active_contexts >= 0);
        if (active_contexts == 0)
            break;
        if (!_async_run_once(NULL, NULL)) {
            assert(tstate->curexc_type != NULL);
            if (Py_VerboseFlag)
                PySys_FormatStdout("_async.run_once raised "
                                   "exception, returning...\n");
            return NULL;
        }
    } while (1);

    if (Py_VerboseFlag)
        PySys_FormatStdout("_async.run(): no more events, returning...\n");
    Py_RETURN_NONE;
}

PyObject *
_async_submit_wait(PyObject *self, PyObject *args)
{
    PyObject *result = NULL;
    Context  *c;
    PxState  *px;
    PTP_WAIT_CALLBACK cb;

    c = new_context(0, 0);
    if (!c)
        return NULL;

    px = c->px;

    if (!extract_waitobj_args(args, c))
        goto free_context;

    cb = _PyParallel_WaitCallback;
    c->tp_wait = CreateThreadpoolWait(cb, c, NULL);
    if (!c->tp_wait) {
        PyErr_SetFromWindowsErr(0);
        goto free_context;
    }

    if (!_PyEvent_TryCreate(c->waitobj))
        goto free_context;

    SetThreadpoolWait(c->tp_wait, Py_EVENT(c->waitobj), NULL);

    InterlockedIncrement64(&(px->waits_submitted));
    InterlockedIncrement(&(px->waits_pending));
    InterlockedIncrement(&(px->active));
    c->stats.submitted = _Py_rdtsc();

    incref_waitobj_args(c);

    result = (Py_INCREF(Py_None), Py_None);
    goto done;

free_context:
    InterlockedDecrement(&(c->px->contexts_active));
    HeapDestroy(c->heap_handle);
    free(c);

done:
    if (!result)
        assert(PyErr_Occurred());
    return result;
}

PyObject *
_async_submit_timer(PyObject *self, PyObject *args)
{
    PyObject *result = NULL;

    return result;
}

PyObject *
_async_submit_io(PyObject *self, PyObject *args)
{
    return NULL;
}

/*
PxIO *
get_pxio(int size)
{
    PxIO *io = NULL;
    PxState *px = (Py_PXCTX ? ctx->px : PXSTATE());
    short io_attempt = 0;
    if (size > PyAsync_IO_BUFSIZE) {
alloc_io:
        io = (PxIO *)PxList_Malloc(sizeof(PxIO));
        if (!io)
            return PyErr_NoMemory();
        io->flags = PxIO_ONDEMAND;
        io->buf = (char *)malloc(size);
        if (!io->buf) {
            PxList_Free(io);
            return PyErr_NoMemory();
        }
        io->size = size;
    } else {
try_io:
        io_attempt++;
        io = (PxIO *)PxList_Pop(px->io_free);
        if (!io) {
            if (io_attempt > 1)
                goto alloc_io;
            else {
                int r;
                InterlockedIncrement64(&(px->io_stalls));
                r = WaitForSingleObject(px->io_free_wakeup, 100);
                if (r == WAIT_OBJECT_0 || r == WAIT_TIMEOUT)
                    goto try_io;
                else {
                    PyErr_SetExcFromWindowsErr(
                        PyExc_AsyncIOBuffersExhaustedError, 0);
                    goto free_context;
                }
            }
        }
        assert(io);
        assert(PxIO_IS_PREALLOC(io));
    }
    assert(io);
    return io;
} */

#define PxSocketIO_Check(o) (0)

PyObject *
_async_submit_write_io(PyObject *self, PyObject *args)
{
    PyObject *o, *cb, *eb, *result = NULL;
    Py_buffer pybuf;
    fileio   *f;
    Context  *c;
    PxState  *px;
    PxIO     *io;
    char     *buf;
    char      success;
    int is_file = 0;
    int is_socket = 0;
    int io_attempt = 0;
    PTP_WIN32_IO_CALLBACK callback;

    if (!PyArg_ParseTuple(args, "Oy*OO", &o, &pybuf, &cb, &eb))
        return NULL;

    is_file = PyFileIO_Check(o);
    is_socket = PxSocketIO_Check(o);
    assert(!(is_file && is_socket));

    if (is_socket) {
        PyErr_SetString(PyExc_ValueError, "sockets not supported yet");
        return NULL;
    } else {
        assert(is_file);
        f = (fileio *)o;
        if (!f->native) {
            PyErr_SetString(PyExc_ValueError,
                            "file was not opened with async.open()");
            return NULL;
        }
    }

    Px_PROTECTION_GUARD(o);

    if (!_PyEvent_TryCreate(o))
        return NULL;

    c = new_context(0, 0);
    if (!c)
        return NULL;

    px = c->px;

    c->callback = (cb == Py_None ? NULL : cb);
    c->errback  = (eb == Py_None ? NULL : eb);
    c->func = NULL;
    c->args = NULL;
    c->kwds = NULL;

    Py_XINCREF(c->callback);
    Py_XINCREF(c->errback);

    callback = _PyParallel_IOCallback;
    c->tp_io = CreateThreadpoolIo(f->h, callback, c, NULL);
    if (!c->tp_io) {
        PyErr_SetFromWindowsErr(0);
        goto free_context;
    }
    c->io_type = Px_IOTYPE_FILE;

    if (pybuf.len > PyAsync_IO_BUFSIZE) {
alloc_io:
        io = (PxIO *)PxList_Malloc(sizeof(PxIO));
        if (!io) {
            PyErr_NoMemory();
            goto free_context;
        }
        io->flags = PxIO_ONDEMAND;
        buf = (char *)malloc(pybuf.len);
        if (!buf) {
            PxList_Free(io);
            PyErr_NoMemory();
            goto free_context;
        }
        io->buf = buf;
    } else {
try_io:
        io_attempt++;
        io = (PxIO *)PxList_Pop(px->io_free);
        if (!io) {
            if (io_attempt > 1)
                goto alloc_io;
            else {
                int r;
                InterlockedIncrement64(&(px->io_stalls));
                /* XXX TODO create more buffers or wait for existing buffers. */
                /* xxx todo: convert to submit_wait */
                r = WaitForSingleObject(px->io_free_wakeup, 100);
                if (r == WAIT_OBJECT_0 || r == WAIT_TIMEOUT)
                    goto try_io;
                else {
                    PyErr_SetExcFromWindowsErr(
                        PyExc_AsyncIOBuffersExhaustedError, 0);
                    goto free_context;
                }
            }
        }
        assert(io);
        assert(PxIO_IS_PREALLOC(io));
    }
    assert(io);

    /* Ugh.  This is ass-backwards.  Need to refactor PxIO to support
     * Py_buffer natively. */
    io->len = (ULONG)pybuf.len;
    memcpy(io->buf, pybuf.buf, pybuf.len);
    PyBuffer_Release(&pybuf);

    io->obj = o;
    c->io_type = PyAsync_IO_WRITE;

    Py_INCREF(io->obj);
    _write_lock(o);
    io->overlapped.Offset = f->write_offset.LowPart;
    io->overlapped.OffsetHigh = f->write_offset.HighPart;
    f->write_offset.QuadPart += io->size;
    _write_unlock(o);

    StartThreadpoolIo(c->tp_io);

    InterlockedIncrement64(&(px->io_submitted));
    InterlockedIncrement(&(px->io_pending));
    InterlockedIncrement(&(px->active));
    c->stats.submitted = _Py_rdtsc();
    c->px->contexts_created++;
    InterlockedIncrement(&(c->px->contexts_active));

    success = WriteFile(f->h, io->buf, io->size, NULL, &(io->overlapped));
    if (!success) {
        int last_error = GetLastError();
        if (last_error != ERROR_IO_PENDING) {
            CancelThreadpoolIo(c->tp_io);
            InterlockedDecrement(&(px->io_pending));
            InterlockedDecrement(&(px->active));
            InterlockedIncrement64(&(px->done));
            if (c->errback)
                PyErr_Warn(PyExc_RuntimeWarning,
                           "file io errbacks not yet supported");
            PyErr_SetFromWindowsErr(0);
            goto free_io;
        } else {
            result = Py_None;
            goto done;
        }
    } else {
        CancelThreadpoolIo(c->tp_io);
        InterlockedDecrement(&(px->io_pending));
        InterlockedDecrement(&(px->active));
        InterlockedIncrement64(&(px->done));
        InterlockedIncrement64(&(px->async_writes_completed_synchronously));
        PySys_WriteStdout("_async.write() completed synchronously\n");
        result = Py_True;
        if (c->callback)
            PyErr_Warn(PyExc_RuntimeWarning,
                       "file io callbacks not yet supported");
        goto free_io;
    }

    assert(0); /* unreachable */

free_io:
    if (PxIO_IS_ONDEMAND(io)) {
        free(io->buf);
        PxList_Free(io);
    }

free_context:
    InterlockedDecrement(&(px->contexts_active));
    px->contexts_destroyed++;
    free(c);

done:
    if (!result)
        assert(PyErr_Occurred());
    else {
        assert(result == Py_None || result == Py_True);
        Py_INCREF(result);
    }
    return result;
}

PyObject *
_async_submit_read_io(PyObject *self, PyObject *args)
{
    //return _async_submit_io(self, args, 0);
    return NULL;
}

PyObject *
_async_submit_server(PyObject *self, PyObject *args)
{
    PyObject *result = NULL;

    return result;
}

PyObject *
_async_submit_client(PyObject *self, PyObject *args)
{
    PyObject *result = NULL;

    return result;
}

PyObject *
_async_submit_class(PyObject *self, PyObject *args)
{
    PyObject *result = NULL;

    return result;
}

PyObject *
_call_from_main_thread(PyObject *self, PyObject *targs, int wait)
{
    int err;
    Context *c;
    PyObject *result = NULL;
    PxListItem *item;
    PxState *px;
    PyObject *func, *arg, *args, *kwds, *tmp;

    Px_GUARD

    func = arg = args = kwds = tmp = NULL;

    c = ctx;
    assert(!c->pstate->curexc_type);

    item = _PyHeap_NewListItem(c);
    if (!item)
        return PyErr_NoMemory();

    if (wait) {
        assert(c->decref == NULL);
        c->decref = _PyHeap_NewListItem(c);
        if (!c->decref) {
            PyErr_NoMemory();
            goto error;
        }
        c->decref->from = c;
    }

    if (!PyArg_UnpackTuple(targs, "call_from_main_thread",
                           1, 3, &func, &arg, &kwds))
        goto error;

    assert(func);
    if (func == Py_None || !PyCallable_Check(func)) {
        PyErr_SetString(PyExc_TypeError, "parameter 1 must be callable");
        goto error;
    }

    if (kwds && kwds == Py_None)
        kwds = NULL;

    if (kwds) {
        if (!PyDict_CheckExact(kwds)) {
            PyErr_SetString(PyExc_TypeError, "param 3 must be None or dict");
            goto error;
        }
    }

    if (arg) {
        if (PyTuple_Check(arg))
            args = arg;
        else {
            args = Py_BuildValue("(O)", arg);
            if (!args)
                goto error;
        }
    } else {
        args = PyTuple_New(0);
        if (!args)
            goto error;
    }

    item->p1 = func;
    item->p2 = args;
    item->p3 = kwds;

    if (wait) {
        item->p4 = (void *)CreateEvent(NULL, FALSE, FALSE, NULL);
        if (!item->p4) {
            PyErr_SetFromWindowsErr(0);
            goto error;
        }
    } else
        assert(item->p4 == NULL);

    px = c->px;

    if (wait) {
        InterlockedIncrement64(&(px->sync_wait_submitted));
        InterlockedIncrement(&(px->sync_wait_pending));
    } else {
        Px_INCCTX(c);
        PxList_Push(c->outgoing, item);
        InterlockedIncrement64(&(px->sync_nowait_submitted));
        InterlockedIncrement(&(px->sync_nowait_pending));
    }

    //InterlockedIncrement(&(px->active));
    item->from = c;
    PxList_TimestampItem(item);
    PxList_Push(px->incoming, item);
    SetEvent(px->wakeup);
    if (!wait)
        return Py_None;

    _PyParallel_DisassociateCurrentThreadFromCallback();
    err = WaitForSingleObject(item->p4, INFINITE);
    switch (err) {
        case WAIT_ABANDONED:
            PyErr_SetString(PyExc_SystemError, "wait abandoned");
            goto cleanup;
        case WAIT_TIMEOUT:
            PyErr_SetString(PyExc_SystemError, "infinite wait timed out?");
            goto cleanup;
        case WAIT_FAILED:
            PyErr_SetFromWindowsErr(0);
            goto cleanup;
    }
    assert(err == WAIT_OBJECT_0);

    if (item->p1 && PyExceptionClass_Check((PyObject *)item->p1)) {
        PyErr_Restore((PyObject *)item->p1,
                      (PyObject *)item->p2,
                      (PyObject *)item->p3);
        goto cleanup;
    }

    assert(item->p1 == NULL);
    assert(item->p2 != NULL);
    assert(item->p3 == NULL);
    result = (PyObject *)item->p2;

cleanup:
    assert(c->decref == NULL);
    InterlockedDecrement(&(px->sync_wait_inflight));
    InterlockedIncrement64(&(px->sync_wait_done));

    CloseHandle(item->p4);
    item->p4 = NULL;
error:
    _PyHeap_Free(c, item);

    return result;
}

PyObject *
_async_call_from_main_thread(PyObject *self, PyObject *args)
{
    return _call_from_main_thread(self, args, 0);
}

PyObject *
_async_call_from_main_thread_and_wait(PyObject *self, PyObject *args)
{
    return _call_from_main_thread(self, args, 1);
}

PyObject *
_async_filecloser(PyObject *self, PyObject *args)
{
    fileio *f;
    PyObject *o, *result = NULL;

    if (!PyTuple_Check(args)) {
        PyErr_BadInternalCall();
        return NULL;
    }

    if (!PyArg_ParseTuple(args, "O!", &PyFileIO_Type, &f))
        return NULL;

    if (!f->h || f->h == INVALID_HANDLE_VALUE) {
        PyErr_BadInternalCall();
        return NULL;
    }

    o = (PyObject *)f;

    _write_lock(o);
    if (f->size > 0 && f->writable) {
        LPCWSTR n;
        Py_UNICODE *u;
        LARGE_INTEGER i;

        u = f->name;
        n = (LPCWSTR)u;

        Px_PROTECTION_GUARD(o);
        /* Close the file and re-open without FILE_FLAG_NO_BUFFERING in order
         * to set the EOF marker to the correct position (as opposed to the
         * sector-aligned position we set it to as part of _async_fileopener).
         */
        CloseHandle(f->h);
        f->h = CreateFile(n, GENERIC_WRITE,
                          FILE_SHARE_READ | FILE_SHARE_WRITE,
                          NULL, OPEN_EXISTING, 0, NULL);

        if (!f->h || f->h == INVALID_HANDLE_VALUE) {
            PyErr_SetFromWindowsErrWithUnicodeFilename(0, u);
            goto done;
        }

        i.QuadPart = f->size;
        if (!SetFilePointerEx(f->h, i, NULL, FILE_BEGIN)) {
            PyErr_SetFromWindowsErrWithUnicodeFilename(0, u);
            goto done;
        }

        if (!SetEndOfFile(f->h)) {
            PyErr_SetFromWindowsErrWithUnicodeFilename(0, u);
            goto done;
        }
    }

    CloseHandle(f->h);
    f->fd = -1;
    result = Py_True;

done:
    _write_unlock(o);
    if (!result)
        assert(PyErr_Occurred());

    Py_XINCREF(result);
    return result;
}


PyObject *
_async__close(PyObject *self, PyObject *obj)
{
    fileio   *f;

    Py_INCREF(obj);
    if (!PyFileIO_Check(obj)) {
        PyErr_SetString(PyExc_ValueError, "not an io file object");
        return NULL;
    }

    f = (fileio *)obj;
    Py_DECREF(f->owner);

    Py_RETURN_NONE;
}

PyObject *
_async_fileopener(PyObject *self, PyObject *args)
{
    LPCWSTR name;
    Py_UNICODE *uname;
    int namelen;
    int flags;
    int caching_behavior;

    int access = 0;
    int share = 0;
    char notif_flags;

    int create_flags = 0;
    int file_flags = FILE_FLAG_OVERLAPPED;

    int exists = 1;

    Py_ssize_t size = 0;

    PyObject *templ;
    PyObject *result = NULL;
    PyObject *fileobj;
    fileio   *f;

    HANDLE h;
    WIN32_FIND_DATA d;

    if (!PyArg_ParseTuple(args, "inOu#iO:fileopener", &caching_behavior,
                          &size, &templ, &uname, &namelen, &flags, &fileobj))
        return NULL;

    name = (LPCWSTR)uname;

    h = FindFirstFile(name, &d);
    if (h && h != INVALID_HANDLE_VALUE) {
        if (!FindClose(h)) {
            PyErr_SetFromWindowsErrWithUnicodeFilename(0, uname);
            goto done;
        }
    } else
        exists = 0;

    if (exists && (flags & O_EXCL)) {
        assert(!(flags & O_APPEND));
        PyErr_SetExcFromWindowsErrWithUnicodeFilename(
            PyExc_OSError,
            EEXIST,
            uname
        );
        goto done;
    } else if (!exists && (flags & O_RDONLY)) {
        assert(!(flags & O_APPEND));
        PyErr_SetExcFromWindowsErrWithUnicodeFilename(
            PyExc_OSError,
            ENOENT,
            uname
        );
        goto done;
    }

    if (flags & O_RDONLY)
        file_flags |= FILE_ATTRIBUTE_READONLY;

    if (flags & (O_RDWR | O_RDONLY)) {
        access |= GENERIC_READ;
        share  |= FILE_SHARE_READ;
    }

    if (flags & (O_RDWR | O_WRONLY)) {
        access |= GENERIC_WRITE;
        share  |= FILE_SHARE_WRITE;
    }

    if (flags & O_APPEND) {
        access |= FILE_APPEND_DATA;
        share  |= FILE_SHARE_WRITE;
    }

    /* There's not a 1:1 mapping between create flags and POSIX flags, so
     * the following code is a bit fiddly. */
    if (flags & O_RDONLY)
        create_flags = OPEN_EXISTING;

    else if (flags & O_WRONLY)
        create_flags = (exists ? TRUNCATE_EXISTING : CREATE_ALWAYS);

    else if ((flags & O_RDWR) || (flags & O_APPEND))
        create_flags = (exists ? OPEN_EXISTING : CREATE_ALWAYS);

    else if (flags & O_EXCL)
        create_flags = CREATE_NEW;

    else if (flags & O_APPEND)
        create_flags = (exists ? OPEN_EXISTING : CREATE_ALWAYS);

    else {
        PyErr_SetString(PyExc_ValueError, "unexpected value for flags");
        goto done;
    }

    switch (caching_behavior) {
        case PyAsync_CACHING_DEFAULT:
            file_flags |= FILE_FLAG_NO_BUFFERING;
            break;

        case PyAsync_CACHING_BUFFERED:
            break;

        case PyAsync_CACHING_RANDOMACCESS:
            file_flags |= FILE_FLAG_RANDOM_ACCESS;
            break;

        case PyAsync_CACHING_SEQUENTIALSCAN:
            file_flags |= FILE_FLAG_SEQUENTIAL_SCAN;
            break;

        case PyAsync_CACHING_WRITETHROUGH:
            file_flags |= FILE_FLAG_WRITE_THROUGH;
            break;

        case PyAsync_CACHING_TEMPORARY:
            file_flags |= FILE_ATTRIBUTE_TEMPORARY;
            break;

        default:
            PyErr_Format(PyExc_ValueError,
                         "invalid caching behavior: %d",
                         caching_behavior);
            goto done;
    }

    h = CreateFile(name, access, share, 0, create_flags, file_flags, 0);
    if (!h || h == INVALID_HANDLE_VALUE) {
        PyErr_SetFromWindowsErrWithUnicodeFilename(0, uname);
        goto done;
    } else if (size > 0) {
        LARGE_INTEGER i;
        i.QuadPart = Px_PAGE_ALIGN(size);
        if (!SetFilePointerEx(h, i, NULL, FILE_BEGIN)) {
            CloseHandle(h);
            PyErr_SetFromWindowsErrWithUnicodeFilename(0, uname);
            goto done;
        }
        if (!SetEndOfFile(h)) {
            CloseHandle(h);
            PyErr_SetFromWindowsErrWithUnicodeFilename(0, uname);
            goto done;
        }
    }

    notif_flags = (
        FILE_SKIP_COMPLETION_PORT_ON_SUCCESS |
        FILE_SKIP_SET_EVENT_ON_HANDLE
    );
    if (!SetFileCompletionNotificationModes(h, notif_flags)) {
        CloseHandle(h);
        PyErr_SetFromWindowsErrWithUnicodeFilename(0, uname);
    }

    if (!_protect(fileobj))
        goto done;

    f = (fileio *)fileobj;
    f->h = h;
    f->native = 1;
    f->istty = 0;
    f->name = uname;
    f->caching = caching_behavior;

    result = PyLong_FromVoidPtr(h);
done:
    if (!result)
        assert(PyErr_Occurred());

    return result;
}

PyObject *
_async__post_open(PyObject *self, PyObject *args)
{
    fileio     *f;
    PyObject   *o;
    Py_ssize_t  size;
    int         caching;
    int         is_write;

    if (!PyArg_ParseTuple(args, "Onii", &o, &caching, &size, &is_write))
        return NULL;

    f = (fileio *)o;
    f->size = size;
    f->caching = caching;

    if (is_write)
        assert(f->writable);

    if (size > 0 && !is_write) {
        PyErr_SetString(PyExc_ValueError,
                        "non-zero size invalid for read-only files");
            return NULL;
    }

    Py_RETURN_NONE;
}

PyObject *
_async__address(PyObject *self, PyObject *o)
{
    Py_INCREF(o);
    return PyLong_FromVoidPtr(o);
}

PyObject *
_async__dbg_address(PyObject *self, PyObject *addr)
{
    PyObject *o;
    Py_INCREF(addr);
    o = (PyObject *)PyLong_AsVoidPtr(addr);
    PySys_FormatStdout("address: 0x%x, refcnt: %d\n", o, Py_REFCNT(o));
    Py_RETURN_NONE;
}

/* Helper inline function that the macros use (allowing breakpoints to be set
 * at object creation time, which is useful when debugging). */

PyObject *
_wrap(PyTypeObject *tp, PyObject *args, PyObject *kwds)
{
    PyObject *self;
    Py_GUARD
    if (!(self = _protect(tp->tp_new(tp, args, kwds))))
        return NULL;

    if (kwds && !args)
        args = PyTuple_New(0);

    if (args && tp->tp_init(self, args, kwds)) {
        Py_DECREF(self);
        return NULL;
    }

    return self;
}

#define _ASYNC_WRAP(name, type)                                                \
PyDoc_STRVAR(_async_##name##_doc,                                              \
"Helper function for creating an async-protected instance of type '##name'\n");\
PyObject *                                                                     \
_async_##name(PyObject *self, PyObject *args, PyObject *kwds)                  \
{                                                                              \
    return _wrap(&type, args, kwds);                                           \
}

_ASYNC_WRAP(list, PyList_Type)
_ASYNC_WRAP(dict, PyDict_Type)

PyDoc_STRVAR(_async_doc,
"_async module.\n\
\n\
Functions:\n\
\n\
run()\n\
map(callable, iterable[, chunksize[, callback[, errback]]])\n\
submit_work(func[, args[, kwds[, callback[, errback]]]])\n\
submit_wait(wait, func[, args[, kwds[, callback[, errback]]]])\n\
submit_timer(timer, func[, args[, kwds[, callback[, errback]]]])\n\
submit_io(func[, args[, kwds[, callback[, errback]]]])\n\
submit_server(obj)\n\
submit_client(obj)\n\
\n\
Socket IO functions:\n\
connect(sock, (host, port)[, buf[, callback[, errback]]])\n\
");

PyDoc_STRVAR(_async_run_doc,
"run() -> None\n\
\n\
Runs the _async event loop.");


PyDoc_STRVAR(_async_unregister_doc,
"unregister(object) -> None\n\
\n\
Unregisters an asynchronous object.");


PyDoc_STRVAR(_async_read_doc, "XXX TODO\n");
PyDoc_STRVAR(_async_open_doc, "XXX TODO\n");
PyDoc_STRVAR(_async_pipe_doc, "XXX TODO\n");
PyDoc_STRVAR(_async_write_doc, "XXX TODO\n");
PyDoc_STRVAR(_async_fileopener_doc, "XXX TODO\n");
PyDoc_STRVAR(_async_filecloser_doc, "XXX TODO\n");
PyDoc_STRVAR(_async__address_doc, "XXX TODO\n");
PyDoc_STRVAR(_async__dbg_address_doc, "XXX TODO\n");
PyDoc_STRVAR(_async__close_doc, "XXX TODO\n");
PyDoc_STRVAR(_async__rawfile_doc,"XXX TODO\n");
PyDoc_STRVAR(_async__post_open_doc,"XXX TODO\n");
PyDoc_STRVAR(_async_submit_write_io_doc, "XXX TODO\n");

PyDoc_STRVAR(_async_map_doc, "XXX TODO\n");
PyDoc_STRVAR(_async_wait_doc, "XXX TODO\n");
PyDoc_STRVAR(_async_rdtsc_doc, "XXX TODO\n");
PyDoc_STRVAR(_async_client_doc, "XXX TODO\n");
PyDoc_STRVAR(_async_server_doc, "XXX TODO\n");
PyDoc_STRVAR(_async_signal_doc, "XXX TODO\n");
PyDoc_STRVAR(_async_prewait_doc, "XXX TODO\n");
PyDoc_STRVAR(_async_protect_doc, "XXX TODO\n");
PyDoc_STRVAR(_async_run_once_doc, "XXX TODO\n");
PyDoc_STRVAR(_async_unprotect_doc, "XXX TODO\n");
PyDoc_STRVAR(_async_protected_doc, "XXX TODO\n");
PyDoc_STRVAR(_async_is_active_doc, "XXX TODO\n");
PyDoc_STRVAR(_async_read_lock_doc, "XXX TODO\n");
PyDoc_STRVAR(_async_read_unlock_doc, "XXX TODO\n");
PyDoc_STRVAR(_async_try_read_lock_doc, "XXX TODO\n");
PyDoc_STRVAR(_async_write_lock_doc, "XXX TODO\n");
PyDoc_STRVAR(_async_write_unlock_doc, "XXX TODO\n");
PyDoc_STRVAR(_async_try_write_lock_doc, "XXX TODO\n");
PyDoc_STRVAR(_async_submit_io_doc, "XXX TODO\n");
PyDoc_STRVAR(_async_submit_work_doc, "XXX TODO\n");
PyDoc_STRVAR(_async_submit_wait_doc, "XXX TODO\n");
PyDoc_STRVAR(_async_is_active_ex_doc, "XXX TODO\n");
PyDoc_STRVAR(_async_active_count_doc, "XXX TODO\n");
PyDoc_STRVAR(_async_submit_timer_doc, "XXX TODO\n");
PyDoc_STRVAR(_async_submit_class_doc, "XXX TODO\n");
PyDoc_STRVAR(_async_submit_client_doc, "XXX TODO\n");
PyDoc_STRVAR(_async_submit_server_doc, "XXX TODO\n");
PyDoc_STRVAR(_async_active_contexts_doc, "XXX TODO\n");
PyDoc_STRVAR(_async_persisted_contexts_doc, "XXX TODO\n");
PyDoc_STRVAR(_async_signal_and_wait_doc, "XXX TODO\n");
PyDoc_STRVAR(_async_is_parallel_thread_doc, "XXX TODO\n");
PyDoc_STRVAR(_async_call_from_main_thread_doc, "XXX TODO\n");
PyDoc_STRVAR(_async_call_from_main_thread_and_wait_doc, "XXX TODO\n");

/* And now for the exported symbols... */
PyThreadState *
_PyParallel_GetThreadState(void)
{
    Px_GUARD
    assert(ctx->pstate);
    assert(ctx->pstate != ctx->tstate);
    return ctx->pstate;
}

void
_Px_NewReference(PyObject *op)
{
    Px_GUARD
    Px_GUARD_MEM(op);

#ifdef Py_DEBUG
    if (!_Px_TEST(op))
        printf("\n_Px_NewReference(op) -> failed _Px_TEST!\n");
    if (!Py_ASPX(op))
        printf("\n_Px_NewReference: no px object!\n");
#endif

    assert(op->is_px != _Py_NOT_PARALLEL);

    if (op->is_px != _Py_IS_PARALLEL)
        op->is_px = _Py_IS_PARALLEL;

    assert(Py_TYPE(op));

    op->ob_refcnt = 1;
    ctx->stats.newrefs++;
}

void
_Px_ForgetReference(PyObject *op)
{
    Px_GUARD_OBJ(op);
    Px_GUARD
    ctx->stats.forgetrefs++;
}

void
_Px_Dealloc(PyObject *op)
{
    Px_GUARD_OBJ(op);
    Px_GUARD
    assert(Py_ASPX(op)->ctx == ctx);
    ctx->h->deallocs++;
    ctx->stats.deallocs++;
}

PyObject *
_PxObject_New(PyTypeObject *tp)
{
    return Object_New(tp, ctx);
}

PyVarObject *
_PxObject_NewVar(PyTypeObject *tp, Py_ssize_t nitems)
{
    return VarObject_New(tp, nitems, ctx);
}

PyObject *
_PxObject_Init(PyObject *op, PyTypeObject *tp)
{
    return Object_Init(op, tp, ctx);
}

PyVarObject *
_PxObject_InitVar(PyVarObject *op, PyTypeObject *tp, Py_ssize_t nitems)
{
    return VarObject_Init(op, tp, nitems, ctx);
}


PyVarObject *
_PxObject_Resize(PyVarObject *op, Py_ssize_t nitems)
{
    return VarObject_Resize((PyObject *)op, nitems, ctx);
}

void *
_PxMem_Malloc(size_t n)
{
    Px_GUARD
    return _PyHeap_Malloc(ctx, n, 0, 0);
}

void *
_PxMem_Realloc(void *p, size_t n)
{
    return _PxObject_Realloc(p, n);
}

void
_PxMem_Free(void *p)
{
    _PxObject_Free(p);
}

void
_Px_NegativePersistedCount(const char *fname, int lineno, Context *c, int count)
{
    char buf[300];

    PyOS_snprintf(buf, sizeof(buf),
                  "%s:%i context at %p has negative ref count "
                  "%" PY_FORMAT_SIZE_T "d",
                  fname, lineno, c, count);
    Py_FatalError(buf);
}


void
Px_DecRef(PyObject *o)
{
    assert(!Py_PXCTX);
    assert(Px_PERSISTED(o) || Px_CLONED(o));

    _Py_DEC_REFTOTAL;
    if ((--((PyObject *)(o))->ob_refcnt) != 0) {
        _Py_CHECK_REFCNT(o);
    } else {
        if (Px_PERSISTED(o)) {
            Context *c = Py_ASPX(o)->ctx;
            int count = InterlockedDecrement(&(c->persisted_count));
            if (count < 0)
                _Px_NegativePersistedCount(__FILE__, __LINE__, c, count);
            else if (count == 0) {
                InterlockedDecrement(&(c->px->contexts_persisted));
                _PxState_FreeContext(c->px, c);
            }
            return;
        } else {
            assert(Px_CLONED(o));
            /* xxx todo: decref parent's children count */

            return;
        }

        assert(0);
    }
}

const char *
PxSocket_GetRecvCallback(PxSocket *s)
{
    int lines_mode;
    READ_LOCK(s);
    lines_mode = PyObject_IsTrue(PxSocket_GET_ATTR("lines_mode"));
    READ_UNLOCK(s);
    return (lines_mode ? "lines_received" : "data_received");
}

/* 0 = failure, 1 = success */
static
int
PxSocket_UpdateConnectTime(PxSocket *s)
{
    int seconds;
    int bytes = sizeof(seconds);
    int result = 0;
    char *b = (char *)&seconds;
    int  *n = &bytes;
    SOCKET fd = s->sock_fd;

    if (getsockopt(fd, SOL_SOCKET, SO_CONNECT_TIME, b, n) != NO_ERROR)
        goto end;

    if (seconds != -1)
        s->connect_time = seconds;

    result = 1;

end:
    return result;
}

int PxSocket_InitInitialBytes(PxSocket *s);

#if 1
#define OutputDebugString(s)
#endif
/* Hybrid sync/async IO loop. */
void
PxSocket_IOLoop(PxSocket *s)
{
    PyObject *func, *args, *result;
    PyBytesObject *bytes;
    TLS *t = &tls;
    Context *c = ctx;
    char *buf = NULL;
    DWORD err, wsa_error;
    SOCKET fd;
    WSABUF *w = NULL, *old_wsabuf = NULL;
    SBUF *sbuf = NULL;
    RBUF *rbuf = NULL;
    ULONG recv_avail = 0;
    ULONG rbuf_size = 0;
    DWORD recv_flags = 0;
    DWORD recv_nbytes = 0;
    Heap *snapshot = NULL;
    int i, n, success, flags = 0;
    TRANSMIT_FILE_BUFFERS *tf = NULL;
    int was_overlapped = 0;

    /* AcceptEx stuff */
    DWORD sockaddr_len, local_addr_len, remote_addr_len;
    LPSOCKADDR local_addr;
    LPSOCKADDR remote_addr;

    sockaddr_len = sizeof(SOCKADDR);
    s->acceptex_addr_len = sockaddr_len + 16;

    fd = s->sock_fd;

    assert(s->ctx == c);
    assert(c->io_obj == (PyObject *)s);

    InterlockedIncrement(&_PxSocket_ActiveIOLoops);

    s->last_thread_id = s->this_thread_id;
    s->this_thread_id = _Py_get_current_thread_id();
    s->last_cpuid = s->this_cpuid;
    __rdtscp(&(s->this_cpuid));
    s->last_io_op = s->this_io_op;
    s->this_io_op = 0;

    s->ioloops++;

    OutputDebugString(L"PxSocket_IOLoop()\n");

    if (s->next_io_op) {
        int op = s->next_io_op;
        s->next_io_op = 0;
        switch (op) {
            case PxSocket_IO_CONNECT:
                s->this_io_op = PxSocket_IO_CONNECT;
                goto do_connect;
            case PxSocket_IO_ACCEPT:
                goto do_accept;
            default:
                assert(0);
        }
    } else {
        was_overlapped = 1;
        assert(s->last_io_op);
        switch (s->last_io_op) {
            case PxSocket_IO_ACCEPT:
                //PxSocket_UpdateConnectTime(s);
                goto overlapped_acceptex_callback;

            case PxSocket_IO_CONNECT:
                //PxSocket_UpdateConnectTime(s);
                goto overlapped_connectex_callback;

            case PxSocket_IO_SEND:
                goto overlapped_send_callback;

            case PxSocket_IO_SENDFILE:
                goto overlapped_sendfile_callback;

            case PxSocket_IO_RECV:
                goto overlapped_recv_callback;

            case PxSocket_IO_DISCONNECT:
                goto overlapped_disconnectex_callback;

            default:
                ASSERT_UNREACHABLE();

        }
    }

    ASSERT_UNREACHABLE();

do_connect:
    OutputDebugString(L"do_connect:\n");
    assert(0);

do_accept:
    OutputDebugString(L"do_accept:\n");
    //rbuf = NULL;
    if (!PxSocket_InitInitialBytes(s))
        PxSocket_EXCEPTION();

    /*
     * It's handy knowing whether or not our protocol sends the first chunk of
     * data or whether we expect the client to send something first.  This
     * will be reflected in s->initial_bytes.len.  If that is greater than
     * zero, then we send first.  In which case, we set our AcceptEx() recv
     * buffer to 0, which means AcceptEx() will return as soon as a connection
     * has been established (without waiting for the other side to actually
     * send something).
     *
     * If the client sends something first, then we use a recv buffer.
     * AcceptEx() will return once the client has connected *and* sent
     * something.  If it connects and doesn't send anything, we won't waste
     * any time processing the connection.  To deal with clients that connect
     * and don't send anything, we periodically check socket connection time
     * in the PxSocketServer_Start() I/O loop.
     *
     * (Or at least we will when I implement that.  It's currently an xxx
     * todo.)
     */
    if (s->initial_bytes.len)
        s->acceptex_bufsize = 0;
    else
        s->acceptex_bufsize = (s->recvbuf_size - (s->acceptex_addr_len * 2));

    assert(s->acceptex_addr_len > 0);

    s->this_io_op = PxSocket_IO_ACCEPT;
    PxOverlapped_Reset(&s->overlapped_acceptex);
    StartThreadpoolIo(s->parent->tp_io);
    success = AcceptEx(s->parent->sock_fd,
                       s->sock_fd,
                       s->rbuf->w.buf,
                       s->acceptex_bufsize,
                       s->acceptex_addr_len,
                       s->acceptex_addr_len,
                       &(s->acceptex_recv_bytes),
                       &(s->overlapped_acceptex));
    if (success) {
        /* AcceptEx() completed synchronously.  No completion packet will be
         * queued.  The number of received bytes will live in
         * s->acceptex_recv_bytes.
         */
        s->last_io_op = PxSocket_IO_ACCEPT;
        s->num_bytes_just_received = s->acceptex_recv_bytes;
        goto accepted;
    } else {
        wsa_error = WSAGetLastError();
        if (wsa_error == WSA_IO_PENDING)
            /* Overlapped accept initiated successfully, a completion packet
             * will be posted when a new client connects. */
            goto end;
        else if (wsa_error == WSAECONNRESET) {
            /* MSDN: "If the error is WSAECONNRESET, an incoming connection
             * was indicated, but was subsequently terminated by the remote
             * peer prior to accepting the call."
             */
            PxSocket_RECYCLE(s);
        } else
            PxSocket_WSAERROR("AcceptEx");
    }

    assert(0);

overlapped_acceptex_callback:
    OutputDebugString(L"do_acceptex_callback:\n");
    if (c->io_result != NO_ERROR) {
        if (s->overlapped_acceptex.Internal == NO_ERROR)
            __debugbreak();

        if (s->wsa_error == NO_ERROR)
            __debugbreak();

        if (s->wsa_error == WSAECONNRESET)
            PxSocket_RECYCLE(s);
        else
            PxSocket_OVERLAPPED_ERROR("AcceptEx");
    }

    s->num_bytes_just_received = (DWORD)s->overlapped_acceptex.InternalHigh;

    /* Intentional follow-on to accepted */

accepted:
    OutputDebugString(L"accepted:\n");

    /* Update the socket context, */
    err = setsockopt(s->sock_fd,
                     SOL_SOCKET,
                     SO_UPDATE_ACCEPT_CONTEXT,
                     (char *)&(s->parent->sock_fd),
                     sizeof(SOCKET));
    if (err == SOCKET_ERROR)
        PxSocket_WSAERROR("setsockopt(SO_UPDATE_ACCEPT_CONTEXT)");

    GetAcceptExSockaddrs(
        s->rbuf->w.buf,
        s->acceptex_bufsize,
        s->acceptex_addr_len,
        s->acceptex_addr_len,
        &local_addr,
        &local_addr_len,
        &remote_addr,
        &remote_addr_len
    );

    memcpy(&(s->local_addr), local_addr, local_addr_len);
    memcpy(&(s->remote_addr), remote_addr, remote_addr_len);

    //InterlockedIncrement(&(s->parent->clients_connected));

    goto start;

overlapped_connectex_callback:
    OutputDebugString(L"overlapped_connectex_callback:\n");
    assert(0);

//connected:
//    assert(0);
//    goto maybe_shutdown_send_or_recv;

overlapped_disconnectex_callback:
    OutputDebugString(L"overlapped_disconnectex_callback:\n");
    /* Well... this seems like an easy one... just recycle the socket for now
     * without bothering to check for errors and whatnot. */
    PxSocket_RECYCLE(s);

start:
    OutputDebugString(L"start:\n");

    assert(s->protocol);

//maybe_shutdown_send_or_recv:
    if (!PxSocket_CAN_RECV(s)) {
        if (shutdown(s->sock_fd, SD_RECEIVE) == SOCKET_ERROR) {
            wsa_error = WSAGetLastError();
            switch (wsa_error) {
                case WSAECONNABORTED:
                case WSAECONNRESET:
                    /* In both of these cases, MSDN says "the application
                     * should close the socket as it is no longer usable".
                     */
                    PxSocket_RECYCLE(s);
            }
            PxSocket_WSAERROR("shutdown(SD_RECEIVE)");
        }
        Px_SOCKFLAGS(s) |= Px_SOCKFLAGS_RECV_SHUTDOWN;
        if (s->recv_shutdown) {
            assert(0 == "xxx todo: recv_shutdown");
        }
    }

    if (PxSocket_SHUTDOWN_SEND(s)) {
        if (shutdown(s->sock_fd, SD_SEND) == SOCKET_ERROR) {
            wsa_error = WSAGetLastError();
            switch (wsa_error) {
                case WSAECONNABORTED:
                case WSAECONNRESET:
                    /* In both of these cases, MSDN says "the application
                     * should close the socket as it is no longer usable".
                     */
                    PxSocket_RECYCLE(s);
            }
            PxSocket_WSAERROR("shutdown(SD_SEND)");
        }
        Px_SOCKFLAGS(s) |= Px_SOCKFLAGS_SEND_SHUTDOWN;
        if (s->send_shutdown) {
            assert(0 == "xxx todo: send_shutdown");
        }
    }

    if ((Px_SOCKFLAGS(s) & Px_SOCKFLAGS_RECV_SHUTDOWN) &&
        (Px_SOCKFLAGS(s) & Px_SOCKFLAGS_SEND_SHUTDOWN))
        goto do_disconnect;

    /* client and server entry point */
    if (PxSocket_IS_CLIENT(s))
        goto maybe_do_connection_made;

    /* server (accept socket) entry point */
    assert(PxSocket_IS_SERVERCLIENT(s));
    assert(!(Px_SOCKFLAGS(s) & Px_SOCKFLAGS_SENDING_INITIAL_BYTES));

    /* This will have been initialized at the do_accept: stage above. */
    if (s->initial_bytes_to_send) {
        DWORD *len;

        assert(!snapshot);
        snapshot = PxContext_HeapSnapshot(c, NULL);
        if (!PxSocket_LoadInitialBytes(s)) {
            PxContext_RollbackHeap(c, &snapshot);
            PxSocket_EXCEPTION();
        }

        Px_SOCKFLAGS(s) |= Px_SOCKFLAGS_SENDING_INITIAL_BYTES;

        w = &s->initial_bytes;
        len = &w->len;

        if (!PxSocket_NEW_SBUF(c, s, snapshot, len, w->buf, 0, &sbuf, 0)) {
            PxContext_RollbackHeap(c, &snapshot);
            if (!PyErr_Occurred())
                PyErr_SetString(PyExc_ValueError,
                                "failed to extract sendable object from "
                                "initial_bytes_to_send");
            PxSocket_EXCEPTION();
        }

        goto do_send;
    }

    /* Intentional follow-on to maybe_do_connection_made. */
maybe_do_connection_made:
    OutputDebugString(L"maybe_do_connection_made:\n");

    assert(
        s->last_io_op == PxSocket_IO_ACCEPT ||
        s->last_io_op == PxSocket_IO_CONNECT
    );

    if (s->connection_made &&
       !(Px_SOCKFLAGS(s) & Px_SOCKFLAGS_CALLED_CONNECTION_MADE))
        goto definitely_do_connection_made;

    goto try_recv;

definitely_do_connection_made:
    OutputDebugString(L"definitely_do_connection_made:\n");
    assert(!(Px_SOCKFLAGS(s) & Px_SOCKFLAGS_CALLED_CONNECTION_MADE));
    func = s->connection_made;
    assert(func);

    snapshot = PxContext_HeapSnapshot(c, NULL);

    /* xxx todo: add peer argument */
    args = PyTuple_Pack(1, s);
    if (!args) {
        PxContext_RollbackHeap(c, &snapshot);
        PxSocket_FATAL();
    }

    result = PyObject_CallObject(func, args);
    if (result)
        assert(!PyErr_Occurred());
    if (PyErr_Occurred())
        assert(!result);
    if (!result) {
        PxContext_RollbackHeap(c, &snapshot);
        PxSocket_EXCEPTION();
    }

    Px_SOCKFLAGS(s) |= Px_SOCKFLAGS_CALLED_CONNECTION_MADE;

    if (PxSocket_IS_SENDFILE_SCHEDULED(s)) {
        if (result != Py_None) {
            PyErr_SetString(PyExc_RuntimeError,
                            "data_received callback scheduled sendfile but "
                            "returned non-None data");
            PxSocket_EXCEPTION();
        }
    }

    if (result == Py_None) {
        if (PxSocket_IS_SENDFILE_SCHEDULED(s)) {
            s->sendfile_snapshot = snapshot;
            snapshot = NULL;
            goto do_sendfile;
        }
        PxContext_RollbackHeap(c, &snapshot);
        goto try_recv;
    }

    sbuf = NULL;
    if (!PxSocket_NEW_SBUF(c, s, snapshot, 0, 0, result, &sbuf, 0)) {
        PxContext_RollbackHeap(c, &snapshot);
        if (!PyErr_Occurred())
            PyErr_SetString(PyExc_ValueError,
                            "connection_made() did not return a sendable "
                            "object (bytes, bytearray or unicode)");
        PxSocket_EXCEPTION();
    }

    if (PyErr_Occurred())
        Px_SOCKFLAGS(s) |= Px_SOCKFLAGS_CLOSE_SCHEDULED;

    if (Px_SOCKFLAGS(s) & Px_SOCKFLAGS_CLOSE_SCHEDULED)
        goto do_disconnect;

    /* Intentional follow-on to do_send. */

do_send:
    OutputDebugString(L"do_send:\n");
    assert(sbuf);
    w = &sbuf->w;

    s->last_io_op = PxSocket_IO_SEND;

    if (!was_overlapped)
        goto do_async_send;

    if (PxSocket_THROUGHPUT(s)) {
        n = s->max_sync_send_attempts;
        goto try_synchronous_send;
    }

    n = 1;
    if (PxSocket_IS_HOG(s) && _PxSocket_ActiveHogs >= _PyParallel_NumCPUs-1)
        goto do_async_send;
    else if (_PxSocket_ActiveIOLoops >= _PyParallel_NumCPUs-1)
        goto do_async_send;
    else if (PxSocket_CONCURRENCY(s))
        goto do_async_send;

try_synchronous_send:
    OutputDebugString(L"try_synchronous_send:\n");
    s->send_id++;
    s->this_io_op = PxSocket_IO_SEND;

    err = SOCKET_ERROR;
    wsa_error = NO_ERROR;
    for (i = 1; i <= n; i++) {
        err = WSASend(s->sock_fd,
                      &sbuf->w,
                      1,        /* number of buffers, currently always 1 */
                      &s->num_bytes_just_sent,
                      0,        /* flags */
                      NULL,     /* overlapped */
                      NULL);    /* completion routine */
        if (err != SOCKET_ERROR)
            break;
        else {
            wsa_error = WSAGetLastError();
            if (wsa_error == WSAEWOULDBLOCK && i < n)
                Sleep(0);
            else
                break;
        }
    }

    if (err != SOCKET_ERROR) {
        /* Send completed synchronously. */

        /* Hmmm... what if num_bytes_just_sent doesn't equal w->len here?
         * That implies only some of the bytes were sent synchronously...
         * can that happen?  Will we need to do a resend dance?
         *
         * (I would think that... no, this shouldn't happen, but hey, when
         *  in doubt, assert!  Or __debugbreak() as the case may be.)
         */
        if (s->num_bytes_just_sent != sbuf->w.len)
            __debugbreak();

        s->total_bytes_sent += s->num_bytes_just_sent;
        snapshot = sbuf->snapshot;
        PxContext_RollbackHeap(c, &sbuf->snapshot);
        if (s->rbuf->snapshot == snapshot)
            s->rbuf->snapshot = NULL;
        w = NULL;
        sbuf = NULL;
        snapshot = NULL;
        goto send_completed;
    } else if (wsa_error == WSAEWOULDBLOCK) {
        s->send_id--;
        goto do_async_send;
    } else {
        s->send_id--;
        PxContext_RollbackHeap(c, &sbuf->snapshot);
        w = NULL;
        sbuf = NULL;
        snapshot = NULL;
        switch (wsa_error) {
            case WSAENETRESET:
            case WSAECONNABORTED:
            case WSAECONNRESET:
                PxSocket_RECYCLE(s);
        }
        PxSocket_WSAERROR("WSASend(synchronous)");
    }

    ASSERT_UNREACHABLE();

do_async_send:
    OutputDebugString(L"do_async_send:\n");
    /* There's some unavoidable code duplication between do_send: above and
     * do_async_send: below.  If you change one, check to see if you need to
     * change the other. */
    assert(sbuf);
    w = &sbuf->w;

    s->send_id++;
    s->this_io_op = PxSocket_IO_SEND;

    PxOverlapped_Reset(&sbuf->ol);
    StartThreadpoolIo(s->tp_io);

    err = WSASend(s->sock_fd,
                  &sbuf->w,
                  1,    /* number of buffers, currently always 1 */
                  NULL, /* number of bytes sent, must be null because we're
                           specifying an overlapped structure */
                  0,    /* flags (MSG_DONTROUTE, MSG_OOB, MSG_PARTIAL) */
                  &sbuf->ol,
                  NULL); /* completion routine; presumably null as we're
                          * using threadpool I/O instead */

    if (err == NO_ERROR) {
        /* Send completed synchronously.  Completion packet will be queued. */
        PxContext_RollbackHeap(c, &sbuf->snapshot);
        goto end;
    } else {
        wsa_error = WSAGetLastError();
        if (wsa_error == WSA_IO_PENDING)
            /* Overlapped IO successfully initiated; completion packet will
             * eventually get queued (when the IO completes or an error
             * occurs). */
            goto end;

        /* The overlapped send attempt failed.  No completion packet will
         * ever be queued, so we need to take care of cleanup here. */
        s->send_id--;
        PxContext_RollbackHeap(c, &sbuf->snapshot);
        switch (wsa_error) {
            case WSAENETRESET:
            case WSAECONNABORTED:
            case WSAECONNRESET:
                PxSocket_RECYCLE(s);
        }
        PxSocket_WSAERROR("WSASend");
    }

    assert(0);

overlapped_send_callback:
    OutputDebugString(L"overlapped_send_callback:\n");
    /* Entry point for an overlapped send. */

    sbuf = s->sbuf ? s->sbuf : (SBUF *)s->rbuf;

    if (c->io_result != NO_ERROR) {
        if (sbuf->ol.Internal == NO_ERROR)
            __debugbreak();

        if (s->wsa_error == NO_ERROR)
            __debugbreak();

        s->send_id--;

        switch (s->wsa_error) {
            case WSAENETRESET:
            case WSAECONNABORTED:
            case WSAECONNRESET:
                PxSocket_RECYCLE(s);
        }
        PxSocket_OVERLAPPED_ERROR("WSASend");
    }

    s->num_bytes_just_sent = (DWORD)sbuf->ol.InternalHigh;
    if (s->num_bytes_just_sent != sbuf->w.len)
        __debugbreak();

    snapshot = sbuf->snapshot;
    if (snapshot)
        PxContext_RollbackHeap(c, &sbuf->snapshot);

    if (s->rbuf->snapshot == snapshot)
        s->rbuf->snapshot = NULL;

    if (sbuf->snapshot)
        __debugbreak();

    if (s->rbuf->snapshot)
        __debugbreak();

    snapshot = NULL;

    s->total_bytes_sent += s->num_bytes_just_sent;

    /* Intentional follow-on to send_complete... */

send_completed:
    OutputDebugString(L"send_completed:\n");
    func = s->send_complete;
    if (!func)
        goto try_recv;

    snapshot = PxContext_HeapSnapshot(c, NULL);

    args = PyTuple_Pack(2, s, PyLong_FromSize_t(s->send_id));
    if (!args) {
        PxContext_RollbackHeap(c, &snapshot);
        PxSocket_FATAL();
    }

    result = PyObject_CallObject(func, args);
    if (result)
        assert(!PyErr_Occurred());
    if (PyErr_Occurred())
        assert(!result);
    if (!result) {
        PxContext_RollbackHeap(c, &snapshot);
        PxSocket_EXCEPTION();
    }

    if (PxSocket_IS_SENDFILE_SCHEDULED(s)) {
        if (result != Py_None) {
            PyErr_SetString(PyExc_RuntimeError,
                            "send_complete callback scheduled sendfile but "
                            "returned non-None data");
            PxSocket_EXCEPTION();
        }
    }

    if (result == Py_None) {
        if (PxSocket_IS_SENDFILE_SCHEDULED(s)) {
            s->sendfile_snapshot = snapshot;
            snapshot = NULL;
            goto do_sendfile;
        }
        PxContext_RollbackHeap(c, &snapshot);
        goto try_recv;
    }

    sbuf = NULL;
    if (!PxSocket_NEW_SBUF(c, s, snapshot, 0, 0, result, &sbuf, 0)) {
        PxContext_RollbackHeap(c, &snapshot);
        if (!PyErr_Occurred())
            PyErr_SetString(PyExc_ValueError,
                            "send_complete() did not return a sendable "
                            "object (bytes, bytearray or unicode)");
        PxSocket_EXCEPTION();
    }

    if (Px_SOCKFLAGS(s) & Px_SOCKFLAGS_SENDING_INITIAL_BYTES) {
        if (s->connection_made) {
            char *msg = "protocol's connection_made() callback "        \
                        "may never be called (because send_complete() " \
                        "is sending more data on the back of the "      \
                        "successful sending of the initial_bytes)";
            PyErr_WarnEx(PyExc_RuntimeWarning, msg, 1);
        }
        Px_SOCKFLAGS(s) &= ~Px_SOCKFLAGS_SENDING_INITIAL_BYTES;
    }

    if (!(Px_SOCKFLAGS(s) & Px_SOCKFLAGS_CHECKED_DR_UNREACHABLE)) {
        Px_SOCKFLAGS(s) |= Px_SOCKFLAGS_CHECKED_DR_UNREACHABLE;
        if (PxSocket_CAN_RECV(s)) {
            char *msg = "protocol has data_received|lines_received " \
                        "callback, but send_complete() is sending " \
                        "more data, so it may never be called";
            PyErr_WarnEx(PyExc_RuntimeWarning, msg, 1);
        }
    }

    if (!(PxSocket_IS_HOG(s))) {
        Px_SOCKFLAGS(s) |= Px_SOCKFLAGS_HOG;
        InterlockedIncrement(&_PxSocket_ActiveHogs);
    }

    goto do_send;

//send_failed:
//    /* Temporarily disabled... nothing is goto'ing here at the moment. */
//    assert(0);
//    assert(wsa_error);
//    func = s->send_failed;
//    if (func) {
//        /* xxx todo */
//        assert(0);
//    }
//
//    syscall = "WSASend";
//
//    goto handle_error;

eof_received:
do_disconnect:
    OutputDebugString(L"do_disconnect:\n");

    s->this_io_op = PxSocket_IO_DISCONNECT;

    /*
     * I'm not sure if StartThreadpoolIo(s->sock_fd) is suitable for use with
     * an overlapped DisconnectEx().  We may need to submit a threadpool wait
     * against the s->sock_fd handle... or event create a custom WSAEvent, set
     * overlapped_disconnectex.hEvent to it, and wait on that.
     */
    PxOverlapped_Reset(&s->overlapped_disconnectex);
    StartThreadpoolIo(s->tp_io);
    success = DisconnectEx(s->sock_fd,
                           &s->overlapped_disconnectex,
                           0,   /* flags */
                           0);  /* reserved */

    if (success) {
        int stop_sleeping = 0; /* You'd alter this in the debugger to stop
                                  sleeping... otherwise we're going to sleep
                                  forever. */
        int sleeps = 0;
        s->break_on_iocp_enter = 1;
        __debugbreak();
        while (!stop_sleeping && ++sleeps)
            Sleep(1);

        /* DisconnectEx() completed synchronously.  Absolutely NFI if a
         * completion packet will be queued... */
        Px_SOCKFLAGS(s) |= Px_SOCKFLAGS_CLEAN_DISCONNECT;

        s->last_io_op = PxSocket_IO_DISCONNECT;
        goto disconnected;
    } else {
        wsa_error = WSAGetLastError();
        if (wsa_error == WSA_IO_PENDING)
            /* Overlapped IO successfully initiated; completion packet will
             * eventually get queued (when the IO completes or an error
             * occurs). */
            goto end;

        switch (s->wsa_error) {
            case WSAENETRESET:
            case WSAECONNABORTED:
            case WSAECONNRESET:
                PxSocket_RECYCLE(s);
        }

        PxSocket_WSAERROR("DisconnectEx");
    }

    ASSERT_UNREACHABLE();

disconnected:
    OutputDebugString(L"disconnected:\n");
//connection_closed:
    Px_SOCKFLAGS(s) &= ~Px_SOCKFLAGS_CLOSE_SCHEDULED;
    Px_SOCKFLAGS(s) &= ~Px_SOCKFLAGS_CONNECTED;
    Px_SOCKFLAGS(s) |=  Px_SOCKFLAGS_CLOSED;

    if (PyErr_Occurred())
        PxSocket_EXCEPTION();

    func = s->connection_closed;
    if (func) {
        /* xxx todo */
        assert(0);
    }

    if (PxSocket_IS_SERVERCLIENT(s)) {
        /* xxx todo */
        PxSocket_RECYCLE(s);
        //__debugbreak();
        //PxServerSocket_ClientClosed(s);
    }

    goto end;

try_recv:
    OutputDebugString(L"try_recv:\n");

    if ((Px_SOCKFLAGS(s) & Px_SOCKFLAGS_CLOSE_SCHEDULED) ||
        (!(PxSocket_CAN_RECV(s))))
        goto do_disconnect;

    if (s->last_io_op == PxSocket_IO_ACCEPT) {
        /*
         * This code path will cover a newly connected client that's just sent
         * some data.
         */
        assert(!s->initial_bytes_to_send);
        goto process_data_received;
    }

do_recv:
    OutputDebugString(L"do_recv:\n");
    assert(!PxSocket_RECV_MORE(s));
    assert(PxSocket_CAN_RECV(s));

    assert(!rbuf);
    assert(!w);
    assert(!snapshot);

    rbuf = s->rbuf;
    if (rbuf->snapshot)
        __debugbreak();
    w = &rbuf->w;
    /* Reset our rbuf. */
    w->len = s->recvbuf_size;
    w->buf = (char *)rbuf->ob_sval;

    s->this_io_op = PxSocket_IO_RECV;
    c->io_result = NO_ERROR;

    if (!was_overlapped)
        goto do_async_recv;

    if (PxSocket_THROUGHPUT(s)) {
        n = s->max_sync_recv_attempts;
        goto try_synchronous_recv;
    }

    n = 1;
    if (_PxSocket_ActiveIOLoops >= _PyParallel_NumCPUs-1)
        goto do_async_recv;
    else if (PxSocket_CONCURRENCY(s))
        goto do_async_recv;

try_synchronous_recv:
    OutputDebugString(L"try_synchronous_recv:\n");
    s->recv_id++;

    assert(recv_flags == 0);

    err = SOCKET_ERROR;
    wsa_error = NO_ERROR;
    s->num_bytes_just_received = 0;

    for (i = 1; i <= n; i++) {
        err = WSARecv(s->sock_fd,
                      w,
                      1,
                      &s->num_bytes_just_received,
                      &recv_flags,
                      0,
                      0);
        if (err == SOCKET_ERROR) {
            wsa_error = WSAGetLastError();
            if (wsa_error == WSAEWOULDBLOCK) {
                if (s->num_bytes_just_received > 0) {
                    /* WSAEWOULDBLOCK is returned even when we received
                     * data if the amount was less than our socket buffer.
                     * (At least, I've seen it do this.)
                     */
                    if (s->num_bytes_just_received >= (DWORD)s->recvbuf_size)
                        /* Now *this* should never happen. */
                        __debugbreak();
                    else {
                        w = NULL;
                        rbuf = NULL;
                        goto process_data_received;
                    }
                }
                if (i < n) {
                    YieldProcessor();
                    continue;
                } else
                    break;
            } else if (wsa_error == WSA_IO_PENDING) {
                __debugbreak();
                break;
            }
        }
        else
            break;
    }

    if (err != SOCKET_ERROR) {
        /* Receive completed synchronously. */
        if (s->num_bytes_just_received == 0)
            goto eof_received;
        w = NULL;
        rbuf = NULL;
        goto process_data_received;
    } else if (wsa_error == WSAEWOULDBLOCK) {
        s->recv_id--;
        goto do_async_recv;
    } else {
        s->recv_id--;
        switch (wsa_error) {
            case WSAENETRESET:
            case WSAECONNABORTED:
            case WSAECONNRESET:
                PxSocket_RECYCLE(s);
        }
        PxSocket_WSAERROR("WSARecv");
    }

    ASSERT_UNREACHABLE();

do_async_recv:
    OutputDebugString(L"do_async_recv:\n");
    assert(rbuf);
    assert(w);

    s->recv_id++;
    StartThreadpoolIo(s->tp_io);
    PxOverlapped_Reset(&rbuf->ol);

    err = WSARecv(fd, w, 1, 0, &recv_flags, &rbuf->ol, NULL);
    if (err == NO_ERROR) {
        /* Recv completed synchronously.  Completion packet will be queued. */
        goto end;
    } else {
        wsa_error = WSAGetLastError();
        if (wsa_error == WSA_IO_PENDING)
            /* Overlapped IO successfully initiated; completion packet will be
             * queued once data is received or an error occurs. */
            goto end;

        /* Overlapped receive attempt failed.  No completion packet will be
         * queued, so we need to take care of cleanup here. */
        s->recv_id--;
        if (rbuf->snapshot)
            PxContext_RollbackHeap(c, &rbuf->snapshot);
        switch (wsa_error) {
            case WSAENETRESET:
            case WSAECONNABORTED:
            case WSAECONNRESET:
                PxSocket_RECYCLE(s);
        }
        PxSocket_WSAERROR("WSARecv");
    }

    ASSERT_UNREACHABLE();

overlapped_recv_callback:
    OutputDebugString(L"overlapped_recv_callback:\n");
    /* Entry point for an overlapped recv. */
    assert(!snapshot);
    rbuf = s->rbuf;

    if (c->io_result != NO_ERROR) {
        if (rbuf->ol.Internal == NO_ERROR)
            __debugbreak();

        if (s->wsa_error == NO_ERROR)
            __debugbreak();

        s->recv_id--;

        if (rbuf->snapshot)
            PxContext_RollbackHeap(c, &rbuf->snapshot);

        switch (s->wsa_error) {
            case WSAENETRESET:
            case WSAECONNABORTED:
            case WSAECONNRESET:
                PxSocket_RECYCLE(s);
        }
        PxSocket_OVERLAPPED_ERROR("WSARecv");
    }

    s->num_bytes_just_received = (DWORD)rbuf->ol.InternalHigh;

    /* do_data_received_callback: expects rbuf to be clear. */
    rbuf = NULL;

    if (s->num_bytes_just_received == 0)
        goto eof_received;

    /*
    if (recv_nbytes == 0)
        goto connection_closed;
     */

    /* Intentional follow-on to process_data_received... */

process_data_received:
    OutputDebugString(L"process_data_received:\n");
    /*
     * So, this is the point where we need to check the data we've received
     * for the sole purpose of seeing if we need to a) receive more data, or
     * b) invoke the protocol's (data|line)_received callback with the data.
     *
     * The former situation will occur when receive filters have been set on
     * the protocol, such as 'lines_mode' (we keep recv'ing until we find a
     * linebreak) or one of the 'expect_*' filters (expect_command, expect_
     * regex etc).  Or any number of other filters that allow us to determine
     * within C code (i.e. within this IO loop) whether or not we've received
     * enough data (without the need to call back into Python).
     *
     * Now, with all that being said, none of that functionality is
     * implemented yet, so the code below simply unsets the 'receive more'
     * flag and continues on to 'do data received callback'.
     *
     * (Which is why the next two lines look pointless.)
     *
     * Actually... here is the place we could also put some pre-Python
     * protocol helpers, like an HTTP request parser.
     */
    Px_SOCKFLAGS(s) &= ~Px_SOCKFLAGS_RECV_MORE;

    goto do_data_received_callback;

do_data_received_callback:
    OutputDebugString(L"do_data_received_callback:\n");

    //assert(recv_nbytes > 0);
    s->total_bytes_received += s->num_bytes_just_received;

    assert(!rbuf);
    rbuf = s->rbuf;

    if (s->num_bytes_just_received < (DWORD)s->recvbuf_size)
        rbuf->ob_sval[s->num_bytes_just_received] = 0;

    if (PxSocket_LINES_MODE_ACTIVE(s))
        goto do_lines_received_callback;

    assert(!rbuf->snapshot);
    rbuf->snapshot = PxContext_HeapSnapshot(c, NULL);

    func = s->data_received;
    assert(func);

    /* For now, num_rbufs should only ever be 1. */
    assert(s->num_rbufs == 1);
    if (s->num_rbufs == 1) {
        PyObject *n;
        PyObject *o;
        PyTypeObject *tp = &PyBytes_Type;
        bytes = R2B(rbuf);
        o = (PyObject *)bytes;
        Py_PXFLAGS(bytes) = Py_PXFLAGS_MIMIC;
        n = init_object(c, o, tp, s->num_bytes_just_received);
        assert(n == o);
        assert(Py_SIZE(bytes) == s->num_bytes_just_received);
        args = PyTuple_Pack(2, s, o);
        if (!args) {
            PxContext_RollbackHeap(c, &rbuf->snapshot);
            PxSocket_FATAL();
        }
    } else {
        /* xxx todo */
        assert(0);
    }

    result = PyObject_CallObject(func, args);
    if (result)
        assert(!PyErr_Occurred());
    if (PyErr_Occurred())
        assert(!result);
    if (!result) {
        PxContext_RollbackHeap(c, &rbuf->snapshot);
        PxSocket_EXCEPTION();
    }

    if (PxSocket_IS_SENDFILE_SCHEDULED(s)) {
        if (result != Py_None) {
            PyErr_SetString(PyExc_RuntimeError,
                            "data_received callback scheduled sendfile but "
                            "returned non-None data");
            PxSocket_EXCEPTION();
        }
    }

    if (result == Py_None) {
        if (PxSocket_IS_SENDFILE_SCHEDULED(s)) {
            s->sendfile_snapshot = rbuf->snapshot;
            rbuf->snapshot = NULL;
            goto do_sendfile;
        }
        PxContext_RollbackHeap(c, &rbuf->snapshot);
        if (Px_SOCKFLAGS(s) & Px_SOCKFLAGS_CLOSE_SCHEDULED)
            goto do_disconnect;
        /* Nothing to send, no close requested, so try recv again. */
        w = NULL;
        rbuf = NULL;
        snapshot = NULL;
        goto do_recv;
    }

    w = &rbuf->w;
    sbuf = (SBUF *)rbuf;
    if (Px_PTR(result) == Px_PTR(bytes)) {
        /* Special case for echo, we don't need to convert anything. */
        sbuf->w.len = s->num_bytes_just_received;
    } else {
        if (!PyObject2WSABUF(result, w)) {
            PyErr_SetString(PyExc_ValueError,
                            "data_received() did not return a sendable "
                            "object (bytes, bytearray or unicode)");
            PxSocket_EXCEPTION();
        }

        sbuf->w.len = w->len;
        sbuf->w.buf = w->buf;
    }

    w = NULL;
    rbuf = NULL;
    snapshot = NULL;
    goto do_send;

do_sendfile:
    OutputDebugString(L"do_sendfile:\n");

    s->send_id++;
    s->this_io_op = PxSocket_IO_SENDFILE;

    flags = 0;
    if (Px_SOCKFLAGS(s) & Px_SOCKFLAGS_CLOSE_SCHEDULED)
        flags = TF_DISCONNECT;

    StartThreadpoolIo(s->tp_io);
    PxOverlapped_Reset(&s->overlapped_sendfile);
    success = TransmitFile(s->sock_fd,
                           s->sendfile_handle,
                           0, /* bytes to write (0 = write whole file) */
                           0, /* bytes per send (0 = default) */
                           &(s->overlapped_sendfile),
                           &(s->sendfile_tfbuf),
                           flags);

    if (success) {
        /* TransmitFile completed synchronously.  No completion packet will
         * be queued.  If a close was requested, we can clean everything up
         * now and then kick-off a socket/context recycle.
         */

        int stop_sleeping = 0; /* You'd alter this in the debugger to stop
                                  sleeping... otherwise we're going to sleep
                                  forever. */
        int sleeps = 0;
        s->break_on_iocp_enter = 1;
        __debugbreak();
        while (!stop_sleeping && ++sleeps)
            Sleep(1);


        if (flags == TF_DISCONNECT) {
            /* Setting this to INVALID_SOCKET now ensures PxSocket_Recycle()
             * doesn't attempt to re-closesocket() the handle.
             */
            s->sock_fd = INVALID_SOCKET;

            CloseHandle(s->sendfile_handle);

            /* Not sure if rolling back the sendfile snapshot is necessary
             * anymore, given that we're about to rollback everything to the
             * first snapshot during PxSocket_Recycle().
             */
            if (s->sendfile_snapshot)
                PxContext_RollbackHeap(c, &s->sendfile_snapshot);

            PxSocket_RECYCLE(s);

        } else {
            /* Otherwise, if no close was requested, go straight to sendfile
             * completion processing.
             */
            goto send_completed;
        }

        ASSERT_UNREACHABLE();

    } else {

        wsa_error = WSAGetLastError();
        if (wsa_error == WSA_IO_PENDING)
            /* Overlapped transmit file request successfully initiated;
             * completion packet will be queued once transmission completes
             * (or an error occurs). */
            goto end;

        /* Overlapped transmit file attempt failed.  No completion packet will
         * be queued, so we need to take care of cleanup ourselves. */
        if (s->sendfile_snapshot)
            PxContext_RollbackHeap(c, &s->sendfile_snapshot);

        CloseHandle(s->sendfile_handle);

        s->send_id--;

        switch (wsa_error) {
            case WSAENETRESET:
            case WSAECONNABORTED:
            case WSAECONNRESET:
                PxSocket_RECYCLE(s);
        }
        PxSocket_WSAERROR("TransmitFile");
    }

    ASSERT_UNREACHABLE();

overlapped_sendfile_callback:
    OutputDebugString(L"overlapped_sendfile_callback:\n");
    /* Entry point for an overlapped TransmitFile */
    CloseHandle(s->sendfile_handle);

    if (s->sendfile_snapshot)
        PxContext_RollbackHeap(c, &s->sendfile_snapshot);

    if (c->io_result != NO_ERROR) {
        s->send_id--;
        if (s->overlapped_sendfile.Internal == NO_ERROR)
            __debugbreak();

        if (s->wsa_error == NO_ERROR)
            __debugbreak();

        /* xxx todo: call send(file?)_failed() if applicable */
        switch (s->wsa_error) {
            case WSAENETRESET:
            case WSAECONNABORTED:
            case WSAECONNRESET:
                PxSocket_RECYCLE(s);
        }

        PxSocket_OVERLAPPED_ERROR("TransmitFile");
    }

    s->total_bytes_sent += s->sendfile_nbytes;
    s->sendfile_nbytes = 0;
    s->sendfile_handle = 0;
    memset(&s->sendfile_tfbuf, 0, sizeof(TRANSMIT_FILE_BUFFERS));
    Px_SOCKFLAGS(s) &= ~Px_SOCKFLAGS_SENDFILE_SCHEDULED;

    goto send_completed;

do_lines_received_callback:
    OutputDebugString(L"do_lines_received_callback:\n");
    func = s->lines_received;
    assert(func);

    /* Not yet implemented. */
    assert(0);

end:
    OutputDebugString(L"end:\n");
    InterlockedDecrement(&_PxSocket_ActiveIOLoops);

    return;

}

PxSocketBuf *
new_pxsocketbuf(Context *c, size_t nbytes)
{
    size_t size;
    PxSocketBuf *sbuf;
    PyBytesObject *pbuf;

    size = nbytes + sizeof(PxSocketBuf);

    sbuf = (PxSocketBuf *)_PyHeap_Malloc(c, size, 0, 0);
    if (!sbuf)
        return NULL;

    pbuf = PxSocketBuf2PyBytesObject(sbuf);
    (void)init_object(c, (PyObject *)pbuf, &PyBytes_Type, nbytes);

    sbuf->signature = _PxSocketBufSignature;
    sbuf->w.len = (ULONG)nbytes;
    sbuf->w.buf = PyBytes_AsString((PyObject *)pbuf);
    return sbuf;
}

PxSocketBuf *
new_pxsocketbuf_from_bytes(Context *c, PyBytesObject *o)
{
    size_t size;
    Py_ssize_t nbytes;
    PxSocketBuf *sbuf;
    char *buf;

    if (PyBytes_AsStringAndSize((PyObject *)o, &buf, &nbytes) == -1)
        return NULL;

    size = sizeof(PxSocketBuf);

    sbuf = (PxSocketBuf *)_PyHeap_Malloc(c, size, 0, 0);
    if (!sbuf)
        return NULL;

    sbuf->w.len = (ULONG)nbytes;
    sbuf->w.buf = buf;
    return sbuf;
}

PxSocketBuf *
new_pxsocketbuf_from_unicode(Context *c, PyUnicodeObject *o)
{
    size_t size;
    Py_ssize_t nbytes;
    PxSocketBuf *sbuf;
    char *buf;

    assert(!PyErr_Occurred());
    buf = PyUnicode_AsUTF8AndSize((PyObject *)o, &nbytes);
    if (PyErr_Occurred())
        return NULL;

    size = sizeof(PxSocketBuf);

    sbuf = (PxSocketBuf *)_PyHeap_Malloc(c, size, 0, 0);
    if (!sbuf)
        return NULL;

    sbuf->w.len = (ULONG)nbytes;
    sbuf->w.buf = buf;
    return sbuf;
}

void
PxSocket_CallbackComplete(Context *c)
{
    c->callback_completed->from = c;
    PxList_TimestampItem(c->callback_completed);
    PxList_Push(c->px->completed_callbacks, c->callback_completed);
}

void
PxSocket_ErrbackComplete(Context *c)
{
    c->errback_completed->from = c;
    PxList_TimestampItem(c->errback_completed);
    PxList_Push(c->px->completed_errbacks, c->errback_completed);
}

void
PxSocket_HandleException(Context *c, const char *syscall, int fatal)
{
    PxSocket *s = (PxSocket *)c->io_obj;
    PyObject *exc, *args, *func, *result;
    PxState *px;
    PyThreadState *pstate;
    PxListItem *item;
    PxListHead *list;

    assert(PyErr_Occurred());

    pstate = c->pstate;
    px = c->px;

    if (fatal)
        goto error;

    READ_LOCK(s);
    func = s->exception_handler;
    READ_UNLOCK(s);

    if (!func)
        goto error;

    exc = PyTuple_Pack(3, pstate->curexc_type,
                          pstate->curexc_value,
                          pstate->curexc_traceback);
    if (!exc)
        goto error;

    PyErr_Clear();
    args = Py_BuildValue("(OsO)", s, syscall, exc);
    if (!args)
        goto error;

    result = PyObject_CallObject(func, args);
    if (null_with_exc_or_non_none_return_type(result, pstate))
        goto error;

    assert(!pstate->curexc_type);

    /* XXX TODO: ratify possible socket states. */
    goto end;

    list = px->completed_errbacks;
    item = c->errback_completed;
    goto done;

error:
    assert(pstate->curexc_type);
    list = px->errors;
    item = c->error;
    item->p1 = pstate->curexc_type;
    item->p2 = pstate->curexc_value;
    item->p3 = pstate->curexc_traceback;

    if (fatal) {
        if (s->sock_fd != INVALID_SOCKET) {
            closesocket(s->sock_fd);
            s->sock_fd = INVALID_SOCKET;
            if (s->tp_io) {
                CancelThreadpoolIo(s->tp_io);
                CloseThreadpoolIo(s->tp_io);
                s->tp_io = NULL;
            }
        }

        /*
        if (s->client_connected_tp_wait)
            CloseThreadpoolWait(s->client_connected_tp_wait);

        if (s->shutdown_tp_wait)
            CloseThreadpoolWait(s->shutdown_tp_wait);

        if (s->low_memory_tp_wait)
            CloseThreadpoolWait(s->low_memory_tp_wait);

        if (s->fd_accept_tp_wait)
            CloseThreadpoolWait(s->fd_accept_tp_wait);
         */

        if (s->client_connected)
            CloseHandle(s->client_connected);

        if (s->shutdown)
            CloseHandle(s->shutdown);

        if (s->low_memory)
            CloseHandle(s->low_memory);

        if (s->fd_accept)
            CloseHandle(s->fd_accept);

    }

done:
    InterlockedExchange(&(c->done), 1);
    item->from = c;
    PxList_TimestampItem(item);
    PxList_Push(list, item);
    SetEvent(px->wakeup);
end:
    return;
}

void
pxsocket_dealloc(PxSocket *s)
{
    if (s->sock_fd != -1) {
        PyObject *exc, *val, *tb;
        Py_ssize_t old_refcount = Py_REFCNT(s);
        /* ++Py_REFCNT(self); */
        Py_INCREF(s);
        PyErr_Fetch(&exc, &val, &tb);
        if (PyErr_WarnFormat(PyExc_ResourceWarning, 1,
                             "unclosed %R", s))
            /* Spurious errors can appear at shutdown */
            if (PyErr_ExceptionMatches(PyExc_Warning))
                PyErr_WriteUnraisable((PyObject *) s);
        PyErr_Restore(exc, val, tb);
        closesocket(s->sock_fd);
        Py_REFCNT(s) = old_refcount;
    }
    if (s->ip)
        free(s->ip);

    Py_TYPE(s)->tp_free((PyObject *)s);
}

void
NTAPI
PxSocket_IOCallback(
    PTP_CALLBACK_INSTANCE instance,
    void *context,
    void *overlapped,
    ULONG io_result,
    ULONG_PTR nbytes,
    TP_IO *tp_io
);

PyObject *
create_pxsocket(
    PyObject *args,
    PyObject *kwds,
    int flags,
    PxSocket *parent,
    Context *use_this_context
)
{
    char *val;
    int len = sizeof(int);
    int nonblock = 1;
    PxSocket *s;
    SOCKET fd = INVALID_SOCKET;
    char *host;
    Context *c;
    Heap *old_heap = NULL;
    Py_ssize_t hostlen;
    int rbuf_size;
    RBUF *rbuf;
    int no_tcp_nodelay = 0;
    int no_exclusive_addr_use = 0;

    PyTypeObject *tp = &PxSocket_Type;

    /* xxx: we could support pipelining here by creating a context for ncpu;
     * then when we're in the PxSocket_IOLoop after we've AcceptEx()'d a call,
     * we'd post ncpu overlapped WSARecv()s, binding each one to one of our
     * contexts. */

    /* First step is to create a new context object that'll encapsulate the
     * socket for its entire lifetime.  (xxx: `use_this_context` is a quick
     * hack; PxSocketServer_CreateClientSocket() needs to create a context as
     * soon as it can in order to pass it to _PyParallel_EnterCallback(),
     * which needs to be done first because it's the point we do per-thread
     * TLS initialization if needed.)
     */
    if (use_this_context)
        c = use_this_context;
    else {
        if (_PyParallel_HitHardMemoryLimit())
            return PyErr_NoMemory();

        c = new_context(0, 1);
        /* Take a snapshot straight after creation such that we can roll back
         * to the original context state if we re-use the socket. */
        PxContext_HeapSnapshot(c, NULL);
    }

    if (!c)
        return NULL;

    c->io_type = Px_IOTYPE_SOCKET;

    s = (PxSocket *)_PyHeap_Malloc(c, sizeof(PxSocket), 0, 0);
    if (!s)
        return NULL;

    c->io_obj = (PyObject *)s;

    if (!init_object(c, c->io_obj, tp, 0))
        PxSocket_FATAL();

    s->ctx = c;

    s->flags = flags;

    if (parent) {
        assert(PxSocket_IS_SERVERCLIENT(s));

        /* Fast path for accept sockets; the initial bytes/protocol stuff will
         * have already been done by the listen socket, so we can just copy
         * what we need.
         */
        s->protocol_type = parent->protocol_type;
        s->protocol = parent->protocol;
        s->data_received = parent->data_received;
        s->lines_received = parent->lines_received;
        s->lines_mode = parent->lines_mode;
        s->exception_handler = parent->exception_handler;
        s->max_sync_send_attempts = parent->max_sync_send_attempts;
        s->max_sync_recv_attempts = parent->max_sync_recv_attempts;

        /* xxx: eh, we should be able to just copy the exact value for flags.
         * (We can't, because it's being overloaded with different
         * applications currently, i.e. indicating current protocol state as
         * well as protocol flags.)
         */
        if (PxSocket_CONCURRENCY(parent))
            Px_SOCKFLAGS(s) |= Px_SOCKFLAGS_CONCURRENCY;

        else if (PxSocket_THROUGHPUT(parent))
            Px_SOCKFLAGS(s) |= Px_SOCKFLAGS_THROUGHPUT;

        if (s->data_received || s->lines_received)
            Px_SOCKFLAGS(s) |= Px_SOCKFLAGS_CAN_RECV;

    } else {
        assert(
            ( PxSocket_IS_CLIENT(s) && !PxSocket_IS_SERVER(s)) ||
            (!PxSocket_IS_CLIENT(s) &&  PxSocket_IS_SERVER(s))
        );
    }

    s->sock_fd = fd;
    s->sock_timeout = -1.0;
    s->sock_family = AF_INET;
    s->sock_type = SOCK_STREAM;

    if (PxSocket_IS_SERVERCLIENT(s)) {
        assert(!PxSocket_IS_SERVER(s));
        assert(!PxSocket_IS_CLIENT(s));
        goto serverclient;
    }

    /* xxx todo: all of this Python-specific stuff should really be moved out
     * into a separate method, such that this method becomes a non-Python
     * specific implementation called PxSocket_Create() or something.
     */
    assert(args);
    //assert(kwds);

    if (!PyArg_ParseTupleAndKeywords(PxSocket_PARSE_ARGS))
        PxSocket_FATAL();

    if (s->sock_family != AF_INET) {
        PyErr_SetString(PyExc_ValueError, "family must be AF_INET");
        PxSocket_FATAL();
    }

    if (s->sock_type != SOCK_STREAM) {
        PyErr_SetString(PyExc_ValueError, "sock type must be SOCK_STREAM");
        PxSocket_FATAL();
    }

    if (s->port < 0 || s->port > 0xffff) {
        PyErr_SetString(PyExc_OverflowError, "socket: port must be 0-65535");
        PxSocket_FATAL();
    }

    if (host[0] >= '1' && host[0] <= '9') {
        int d1, d2, d3, d4;
        char ch;

        if (sscanf(host, "%d.%d.%d.%d%c", &d1, &d2, &d3, &d4, &ch) == 4 &&
            0 <= d1 && d1 <= 255 && 0 <= d2 && d2 <= 255 &&
            0 <= d3 && d3 <= 255 && 0 <= d4 && d4 <= 255)
        {
            struct sockaddr_in *sin;
            int *len;

            /* xxx todo: now that we've switched to having a context
             * encapsulate the socket, we should change these char[n]
             * arrays into pointers that are _PyHeap_Malloc'd with the
             * socket's context. */
            memset(&s->ip[0], 0, 16);
            strncpy(&s->ip[0], host, 15);
            assert(s->ip[15] == '\0');
            s->host = &(s->ip[0]);

            if (PxSocket_IS_CLIENT(s)) {
                sin = &(s->remote_addr.in);
                len = &(s->remote_addr_len);
            } else if (PxSocket_IS_SERVER(s)) {
                sin = &(s->local_addr.in);
                len = &(s->local_addr_len);
            } else
                assert(0);

            sin->sin_addr.s_addr = htonl(
                ((long)d1 << 24) | ((long)d2 << 16) |
                ((long)d3 << 8)  | ((long)d4 << 0)
            );

            sin->sin_family = AF_INET;
            sin->sin_port = htons((short)s->port);
            *len = sizeof(*sin);
        } else {
            PyErr_SetString(PyExc_ValueError, "invalid IPv4 address");
            PxSocket_FATAL();
        }
    } else {
        strncpy(s->host, host, hostlen);
        assert(!s->ip);
    }

serverclient:
    if (s->sock_fd != INVALID_SOCKET)
        goto setnonblock;

    s->sock_fd = socket(s->sock_family, s->sock_type, s->sock_proto);
    if (s->sock_fd == INVALID_SOCKET)
        PxSocket_WSAERROR("socket()");

setnonblock:
    fd = s->sock_fd;
    if (ioctlsocket(fd, FIONBIO, &nonblock) == SOCKET_ERROR)
        PxSocket_WSAERROR("ioctlsocket(FIONBIO)");

    val = (char *)&(s->recvbuf_size);
    if (getsockopt(fd, SOL_SOCKET, SO_RCVBUF, val, &len) == SOCKET_ERROR)
        PxSocket_WSAERROR("getsockopt(SO_RCVBUF)");

    assert(s->recvbuf_size >= 0 && s->recvbuf_size <= 65536);

    /* Every socket* gets a recv buf assigned to it.  This won't be transient;
     * i.e. it doesn't participate in the snapshot/rollback dance like the
     * send buffers do.
     *
     * [*] except listen sockets.
     */
    if (PxSocket_IS_SERVER(s))
        goto set_other_sockopts;

    rbuf_size = s->recvbuf_size + Px_PTR_ALIGN(sizeof(RBUF));
    rbuf = (RBUF *)_PyHeap_Malloc(s->ctx, rbuf_size, 0, 0);
    if (!rbuf)
        PxSocket_FATAL();

    rbuf->s = s;
    rbuf->ctx = s->ctx;
    rbuf->w.len = s->recvbuf_size;
    rbuf->w.buf = (char *)rbuf->ob_sval;
    s->num_rbufs = 1;
    s->rbuf = rbuf;

    /* Send buffers get allocated on demand via PxSocket_NEW_SBUF() during the
     * PxSocket_IOLoop(), so we don't explicitly reserve space for one like we
     * do for the receive buffer above. */
    val = (char *)&(s->sendbuf_size);
    if (getsockopt(fd, SOL_SOCKET, SO_SNDBUF, val, &len) == SOCKET_ERROR)
        PxSocket_WSAERROR("getsockopt(SO_SNDBUF)");

    assert(s->sendbuf_size >= 0 && s->sendbuf_size <= 65536);

set_other_sockopts:
    if (s->sock_type == SOCK_STREAM) {
        int i = 1;
        int err;

        if (!no_tcp_nodelay) {
            err = setsockopt(fd,
                             SOL_SOCKET,
                             TCP_NODELAY,
                             (char *)&i,
                             sizeof(int));
            if (err == SOCKET_ERROR)
                PxSocket_WSAERROR("setsockopt(TCP_NODELAY)");
        }

        if (PxSocket_IS_SERVER(s) && !no_exclusive_addr_use) {
            err = setsockopt(fd,
                             SOL_SOCKET,
                             SO_EXCLUSIVEADDRUSE,
                             (char *)&i,
                             sizeof(int));
            if (err == SOCKET_ERROR)
                PxSocket_WSAERROR("setsockopt(SO_EXCLUSIVEADDRUSE)");
        }
    }

    s->tp_io = CreateThreadpoolIo((HANDLE)s->sock_fd,
                                  PxSocket_IOCallback,
                                  s->ctx,
                                  NULL);
    if (!s->tp_io)
        PxSocket_SYSERROR("CreateThreadpoolIo(PyParallel_IOCallback)");

    if (PxSocket_IS_SERVERCLIENT(s))
        s->acceptex_addr_len = sizeof(SOCKADDR) + 16;

    InitializeCriticalSectionAndSpinCount(&(s->cs), CS_SOCK_SPINCOUNT);

    s->parent = parent;
    if (!s->parent) {
        /* Inherit our parent from the active context if applicable. */
        if (Py_PXCTX && ctx->io_obj && ctx->io_type == Px_IOTYPE_SOCKET)
            s->parent = (PxSocket *)ctx->io_obj;
    }

    if (s->parent && !PxSocket_IS_SERVER(s->parent))
        /* If our parent is not the listen socket, add a backref.  (xxx: I'm
         * not sure if this is right... can we have non 1:1 relationships
         * here?)
         */
        s->parent->child = s;

    return (PyObject *)s;

end:
    assert(PyErr_Occurred());
    if (PyErr_Occurred()) {
        assert(s->sock_fd == INVALID_SOCKET);
        assert(!s->tp_io);
    }
    return NULL;
}

PyDoc_STRVAR(pxsocket_accept_doc, "x\n");
PyDoc_STRVAR(pxsocket_bind_doc, "x\n");
PyDoc_STRVAR(pxsocket_connect_doc, "x\n");
PyDoc_STRVAR(pxsocket_close_doc, "x\n");
PyDoc_STRVAR(pxsocket_listen_doc, "x\n");
PyDoc_STRVAR(pxsocket_write_doc, "XXX TODO\n");

PyObject *
pxsocket_accept(PxSocket *s, PyObject *args)
{
    Py_RETURN_NONE;
}

void
PxSocket_HandleCallback(
    Context *c,
    const char *name,
    const char *format,
    ...
)
{
    va_list va;
    PyObject *func, *args, *result;
    PxSocket *s = (PxSocket *)c->io_obj;
    PyObject *o = (PyObject *)s;
    PyObject *protocol = s->protocol;

    if (!strcmp(name, "connection_made"))
        Px_SOCKFLAGS(s) |= Px_SOCKFLAGS_CALLED_CONNECTION_MADE;

    READ_LOCK(o);
    func = PxSocket_GET_ATTR(name);
    READ_UNLOCK(o);

    if (!func || func == Py_None)
        goto end;

    va_start(va, format);
    args = Py_VaBuildValue(format, va);
    va_end(va);

    /*
    if (!PxContext_Snapshot(c))
        PxSocket_EXCEPTION();
    */

    result = PyObject_CallObject(func, args);

    if (result)
        assert(!PyErr_Occurred());

    if (PyErr_Occurred())
        assert(!result);

    if (!result)
        PxSocket_EXCEPTION();

    if (result == Py_None)
        goto end;

end:
    return;
}

void
disabled_PxServerSocket_ClientClosed(PxSocket *o)
{
    Context  *x = o->ctx;
    PxSocket *s = o->parent;

    x->io_obj = NULL;

    if (PxSocket_IS_HOG(o)) {
        Px_SOCKFLAGS(s) &= ~Px_SOCKFLAGS_HOG;
        InterlockedDecrement(&_PxSocket_ActiveHogs);
    }

    /* temp stats for chargen */
    /*
    {
        size_t lines, lps;
        double Bs, KBs, MBs;
        SOCKET fd = o->sock_fd;

        lines = o->total_bytes_sent / 73;

        if (o->connect_time <= 0) {
            printf("[%d/%d/%d] client sent %d bytes (%d lines)\n",
                   s->nchildren, o->child_id, fd, o->total_bytes_sent, lines);
        } else {
            Bs = (double)o->total_bytes_sent / o->connect_time;
            KBs = Bs / 1024.0;
            MBs = KBs / 1024.0;
            lines = o->total_bytes_sent / 73;
            lps = lines / o->connect_time;

            printf("[%d/%d/%d] client sent %d bytes total, connect time: "
                   "%d seconds, %.3fb/s, %.3fKB/s, %.3fMB/s, "
                   "lines: %d, lps: %d\n",
                   s->nchildren, o->child_id, fd, o->total_bytes_sent,
                   o->connect_time, Bs, KBs, MBs, lines, lps);
        }
    }
    */

    PxSocket_CallbackComplete(x);

    o->ctx = NULL;

    if (!(Px_SOCKFLAGS(s) & Px_SOCKFLAGS_CLEAN_DISCONNECT))
        o->sock_fd = -1;

    //InterlockedDecrement(&(s->nchildren));

    //SetEvent(s->more_accepts);
}

PxSocketBuf *
_try_extract_something_sendable_from_object(
    Context *c,
    PyObject *o,
    int depth)
{
    PxSocketBuf *b;

    if (depth > 10) {
        PyErr_SetString(PyExc_ValueError, "call depth exceeded trying to "
                                          "extract sendable object");
        return NULL;
    }

    if (!o)
        return NULL;

    if (PyBytes_Check(o)) {
        b = PyBytesObject2PxSocketBuf(o);
        if (!b)
            b = new_pxsocketbuf_from_bytes(c, (PyBytesObject *)o);

    } else if (PyUnicode_Check(o)) {
        b = new_pxsocketbuf_from_unicode(c, (PyUnicodeObject *)o);

    } else if (PyCallable_Check(o)) {
        PyObject *r = PyObject_CallObject(o, NULL);
        if (r)
            b = _try_extract_something_sendable_from_object(c, r, depth+1);
    } else {
        PyObject *s = PyObject_Str(o);
        if (s)
            b = _try_extract_something_sendable_from_object(c, s, depth);
    }

    if (!b)
        assert(PyErr_Occurred());

    return b;
}

PxSocketBuf *
_pxsocket_initial_bytes_to_send(Context *c, PxSocket *s)
{
    PyObject *i = PxSocket_GET_ATTR("initial_bytes_to_send");
    if (i == Py_None)
        return NULL;
    return _try_extract_something_sendable_from_object(c, i, 0);
}

/* These stubs are useful when debugging as they allow you to quickly see what
 * overlapped entry point a given thread/breakpoint/callstack is in (rather
 * than trying to trace back through s->*_io_op).
 */

void PxSocket_IOLoop_Connect(PxSocket *s) { PxSocket_IOLoop(s); }
void PxSocket_IOLoop_Accept(PxSocket *s) { PxSocket_IOLoop(s); }
void PxSocket_IOLoop_OverlappedConnectEx(PxSocket *s) { PxSocket_IOLoop(s); }
void PxSocket_IOLoop_OverlappedAcceptEx(PxSocket *s) { PxSocket_IOLoop(s); }
void PxSocket_IOLoop_OverlappedWSASend(PxSocket *s) { PxSocket_IOLoop(s); }
void PxSocket_IOLoop_OverlappedWSARecv(PxSocket *s) { PxSocket_IOLoop(s); }
void PxSocket_IOLoop_OverlappedTransmitFile(PxSocket *s) {PxSocket_IOLoop(s);}
void PxSocket_IOLoop_OverlappedDisconnectEx(PxSocket *s) {PxSocket_IOLoop(s);}

void
NTAPI
PxSocket_IOCallback(
    PTP_CALLBACK_INSTANCE instance,
    void *context,
    void *overlapped,
    ULONG io_result,
    ULONG_PTR nbytes,
    TP_IO *tp_io
)
{
    Context *c = (Context *)context;
    PxSocket *s = (PxSocket *)c->io_obj;
    OVERLAPPED *ol = (OVERLAPPED *)overlapped;

    /* AcceptEx() callbacks are a little quirky as the threadpool I/O is
     * actually submitted against the listen socket, not the accept socket.
     * Easy fix though; detect if we're a listen socket, then switch over to
     * the accept socket if so.
     */
    if (s->this_io_op == PxSocket_IO_LISTEN) {
        PxSocket *listen_socket = s;
        s = _Py_CAST_BACK(ol, PxSocket *, PxSocket, overlapped_acceptex);
        c = s->ctx;

        if (s->parent != listen_socket)
            __debugbreak();

        if (s->this_io_op != PxSocket_IO_ACCEPT)
            __debugbreak();
    }

    ENTERED_IO_CALLBACK();

    if (s->break_on_iocp_enter)
        __debugbreak();

    /*
    if (!TryEnterCriticalSection(&(s->cs)))
        __debugbreak();
    */

    if (io_result != NO_ERROR) {
        int failed;
        int errval;
        int errlen = sizeof(int);
        failed = getsockopt(s->sock_fd,
                            SOL_SOCKET,
                            SO_ERROR,
                            (char *)&errval,
                            &errlen);
        if (failed)
            PxSocket_WSAERROR("getsockopt(SO_ERROR)");

        s->wsa_error = errval;
    }

    /* Would overlapped ever be null? */
    if (!ol)
        __debugbreak();
    else {
        /* And if not... would the Internal (status code) ever differ from
         * io_result, or InternalHigh (bytes transferred) ever differ from
         * nbytes?
         */
        if (ol->Internal != io_result) {
            /* Hrm, I saw instances of this where io_result was 64 and
             * ol->Internal was some random big number.  Always during
             * overlapped DisconnectEx() callbacks I believe.
             */
            if (io_result && io_result != 64)
                __debugbreak();
        }
        else if (ol->InternalHigh != nbytes)
            __debugbreak();

        /*
         * Or would it ever not match the overlapped struct corresponding to
         * the current I/O action (i.e. what s->this_io_op indicates).
         */
        /* xxx todo */
    }


    /* Would tp_io ever be null? */
    if (!tp_io)
        __debugbreak();
    else {
        /* And if not... would tp_io ever not equal s->tp_io, or
         * s->parent->tp_io if s is a server socket and s->this_io_op ==
         * PxSocket_IO_ACCEPT.
         */

        switch (s->this_io_op) {
            case PxSocket_IO_ACCEPT:
                if (!s->parent)
                    __debugbreak();
                break;
            case PxSocket_IO_LISTEN:
                if (tp_io != s->tp_io)
                    __debugbreak();
        }
    }

    EnterCriticalSection(&(s->cs));
    LeaveCriticalSectionWhenCallbackReturns(instance, &(s->cs));
    /* Heh.  All this duplication just for some useful names when viewing
     * stack traces. */
    if (s->next_io_op) {
        switch (s->next_io_op) {
            case PxSocket_IO_ACCEPT:
                PxSocket_IOLoop_Accept(s);
                break;

            case PxSocket_IO_CONNECT:
                PxSocket_IOLoop_Connect(s);
                break;

            default:
                ASSERT_UNREACHABLE();
        }
    } else {
        assert(s->this_io_op);
        switch (s->this_io_op) {
            case PxSocket_IO_ACCEPT:
                PxSocket_IOLoop_OverlappedAcceptEx(s);
                break;

            case PxSocket_IO_CONNECT:
                PxSocket_IOLoop_OverlappedConnectEx(s);
                break;

            case PxSocket_IO_SEND:
                PxSocket_IOLoop_OverlappedWSASend(s);
                break;

            case PxSocket_IO_SENDFILE:
                PxSocket_IOLoop_OverlappedTransmitFile(s);
                break;

            case PxSocket_IO_RECV:
                PxSocket_IOLoop_OverlappedWSARecv(s);
                break;

            case PxSocket_IO_DISCONNECT:
                PxSocket_IOLoop_OverlappedDisconnectEx(s);
                break;

            default:
                ASSERT_UNREACHABLE();

        }
    }
    LeaveCriticalSection(&(s->cs));
end:
    return;
}

void
NTAPI
PyParallel_IOCallback(
    PTP_CALLBACK_INSTANCE instance,
    void *context,
    void *overlapped,
    ULONG io_result,
    ULONG_PTR nbytes,
    TP_IO *tp_io
)
{
    Context *c = (Context *)context;

    ENTERED_IO_CALLBACK();

    /* xxx todo: could just use function pointers for this stuff instead of
     * hard-coding the type checks. */
    switch (c->io_type) {
        case Px_IOTYPE_SOCKET: {
            PxSocket *s = (PxSocket *)c->io_obj;
            EnterCriticalSection(&(s->cs));
            PxSocket_IOLoop(s);
            LeaveCriticalSection(&(s->cs));
            break;
        }

        default:
            assert(0);
    }
}

/* objects */
/* xxx: current thinking re: send_failed/recv_failed etc: need to tighten up
 * the error handling logic.  Do we need individual (send|recv)_failed calls?
 * Such failures will typically be one of WSAENOTCONN, WSAEABORT, WSAENETRESET
 * and the like... may make sense just to bundle all these up and refer to
 * them via 'connection_lost', whilst 'connection_closed' is used to indicate
 * a clean connection close. */

/* (Also, are we sure we want to be using _Py_IDENTIFIER here?  Not sure if
 *  there are some issues with the Py_TLS static stuff and the relationship
 *  between our server socket stuff (where the protocol is initialized at the
 *  listen socket level, then cloned in an ad-hoc fashion (in create_pxsocket)
 *  to the accept socket.)
 */
_Py_IDENTIFIER(send_failed);
_Py_IDENTIFIER(recv_failed);
_Py_IDENTIFIER(send_shutdown);
_Py_IDENTIFIER(recv_shutdown);
_Py_IDENTIFIER(send_complete);
_Py_IDENTIFIER(data_received);
_Py_IDENTIFIER(lines_received);
_Py_IDENTIFIER(connection_made);
_Py_IDENTIFIER(connection_closed);
_Py_IDENTIFIER(exception_handler);
_Py_IDENTIFIER(initial_bytes_to_send);

/* bools */
_Py_IDENTIFIER(lines_mode);
_Py_IDENTIFIER(throughput);
_Py_IDENTIFIER(concurrency);
_Py_IDENTIFIER(shutdown_send);

/* ints */
_Py_IDENTIFIER(max_sync_send_attempts);
_Py_IDENTIFIER(max_sync_recv_attempts);


/* 0 = failure, 1 = success */
int
PxSocket_InitProtocol(PxSocket *s)
{
    PyObject *p;
    assert(s->protocol_type);
    assert(!s->protocol);

    s->protocol = PyObject_CallObject(s->protocol_type, NULL);
    if (!s->protocol)
        return 0;

    p = s->protocol;

    assert(!PyErr_Occurred());

#define _PxSocket_RESOLVE_OBJECT(name) do {             \
    PyObject *o = _PyObject_GetAttrId(p, &PyId_##name); \
    if (!o)                                             \
        PyErr_Clear();                                  \
    else if (!PyCallable_Check(o)) {                    \
        PyErr_SetString(                                \
            PyExc_ValueError,                           \
            "protocol attribute '" #name "' "           \
            "is not a callable object"                  \
        );                                              \
        return 0;                                       \
    }                                                   \
    s->##name = o;                                      \
} while (0)

#define _PxSocket_RESOLVE_BOOL(name) do {               \
    int b = 0;                                          \
    PyObject *o = _PyObject_GetAttrId(p, &PyId_##name); \
    if (!o)                                             \
        PyErr_Clear();                                  \
    else                                                \
        b = PyObject_IsTrue(o);                         \
    if (b)                                              \
        Px_SOCKFLAGS(s) |= Px_SOCKFLAGS_##name;         \
    else                                                \
        Px_SOCKFLAGS(s) &= ~Px_SOCKFLAGS_##name;        \
} while (0)

#define _PxSocket_RESOLVE_INT(name) do {                \
    int i = 0;                                          \
    PyObject *o = _PyObject_GetAttrId(p, &PyId_##name); \
    if (!o) {                                           \
        PyErr_Clear();                                  \
        Px_SOCKFLAGS(s) &= ~Px_SOCKFLAGS_##name;        \
    } else {                                            \
        i = PyLong_AsLong(o);                           \
        if (PyErr_Occurred())                           \
            return 0;                                   \
        Px_SOCKFLAGS(s) |= Px_SOCKFLAGS_##name;         \
        s->##name = i;                                  \
    }                                                   \
} while (0)

    _PxSocket_RESOLVE_OBJECT(lines_mode);
    _PxSocket_RESOLVE_OBJECT(send_failed);
    _PxSocket_RESOLVE_OBJECT(recv_failed);
    _PxSocket_RESOLVE_OBJECT(send_shutdown);
    _PxSocket_RESOLVE_OBJECT(recv_shutdown);
    _PxSocket_RESOLVE_OBJECT(send_complete);
    _PxSocket_RESOLVE_OBJECT(data_received);
    _PxSocket_RESOLVE_OBJECT(lines_received);
    _PxSocket_RESOLVE_OBJECT(connection_made);
    _PxSocket_RESOLVE_OBJECT(connection_closed);
    _PxSocket_RESOLVE_OBJECT(exception_handler);
    _PxSocket_RESOLVE_OBJECT(initial_bytes_to_send);

    _PxSocket_RESOLVE_BOOL(throughput);
    _PxSocket_RESOLVE_BOOL(concurrency);
    _PxSocket_RESOLVE_BOOL(shutdown_send);

    _PxSocket_RESOLVE_INT(max_sync_send_attempts);
    _PxSocket_RESOLVE_INT(max_sync_recv_attempts);

    assert(!PyErr_Occurred());

    if (s->data_received || s->lines_received)
        Px_SOCKFLAGS(s) |= Px_SOCKFLAGS_CAN_RECV;

    if (s->lines_mode && !s->lines_received) {
        PyErr_SetString(PyExc_ValueError,
                        "protocol has 'lines_mode' set to True but no "
                        "'lines_received' callback");
        return 0;
    }

    if (s->lines_received && !s->lines_mode) {
        PyErr_SetString(PyExc_ValueError,
                        "protocol has 'lines_received' callback but "
                        "no 'lines_mode' attribute");
        return 0;
    }

    assert(!PyErr_Occurred());

    if (s->lines_mode && PyObject_IsTrue(s->lines_mode))
        Px_SOCKFLAGS(s) |= Px_SOCKFLAGS_LINES_MODE_ACTIVE;
    else
        Px_SOCKFLAGS(s) &= ~Px_SOCKFLAGS_LINES_MODE_ACTIVE;

    if (PyErr_Occurred())
        return 0;

    if (PxSocket_CONCURRENCY(s) && PxSocket_THROUGHPUT(s)) {
        PyErr_SetString(PyExc_ValueError,
                        "protocol has both 'concurrency' and "
                        "'throughput' set to True; only one or "
                        "the other is permitted");
        return 0;
    }

    if (!PxSocket_THROUGHPUT(s)) {
        if (PxSocket_MAX_SYNC_SEND_ATTEMPTS(s)) {
            PyErr_SetString(PyExc_ValueError,
                            "protocol has 'max_sync_send_attempts' "
                            "set without 'throughput' set to True");
            return 0;
        }
        if (PxSocket_MAX_SYNC_RECV_ATTEMPTS(s)) {
            PyErr_SetString(PyExc_ValueError,
                            "protocol has 'max_sync_recv_attempts' "
                            "set without 'throughput' set to True");
            return 0;
        }
    } else {
        if (!PxSocket_MAX_SYNC_SEND_ATTEMPTS(s))
            s->max_sync_send_attempts = _PxSocket_MaxSyncSendAttempts;
        else {
            if (s->max_sync_send_attempts < 0) {
                PyErr_SetString(PyExc_ValueError,
                                "protocol has 'max_sync_send_attempts' "
                                "set to a value less than 0");
                return 0;
            } else if (s->max_sync_send_attempts == 0)
                s->max_sync_send_attempts = INT_MAX;
        }

        if (!PxSocket_MAX_SYNC_RECV_ATTEMPTS(s))
            s->max_sync_recv_attempts = _PxSocket_MaxSyncSendAttempts;
        else {
            if (s->max_sync_recv_attempts < 0) {
                PyErr_SetString(PyExc_ValueError,
                                "protocol has 'max_sync_recv_attempts' "
                                "set to a value less than 0");
                return 0;
            } else if (s->max_sync_recv_attempts == 0)
                s->max_sync_recv_attempts = INT_MAX;
        }
    }

    return 1;
}


/* 0 = failure, 1 = success */
int
PxSocket_SetProtocolType(PxSocket *s, PyObject *protocol_type)
{
    if (!protocol_type) {
        PyErr_SetString(PyExc_ValueError, "missing protocol value");
        return 0;
    }

    if (!PyType_CheckExact(protocol_type)) {
        PyErr_SetString(PyExc_ValueError, "protocol must be a class");
        return 0;
    }

    s->protocol_type = protocol_type;
    return PxSocket_InitProtocol(s);
}

#define INVALID_INITIAL_BYTES                                           \
    "initial_bytes_to_send must be one of the following types: bytes, " \
    "unicode or callable"

/* 0 = failure (error will be set), 1 = no error occurred */
int
PxSocket_InitInitialBytes(PxSocket *s)
{
    Context *c = s->ctx;
    PyObject *o, *t = s->protocol;
    int is_static = 0;
    Heap *snapshot = NULL;

    assert(t);
    assert(!PyErr_Occurred());

    o = s->initial_bytes_to_send;

    if (!o || o == Py_None)
        return 1;

    is_static = (
        PyBytes_Check(o)        ||
        PyByteArray_Check(o)    ||
        PyUnicode_Check(o)
    );

    if (is_static)
        Px_SOCKFLAGS(s) &= ~Px_SOCKFLAGS_INITIAL_BYTES_CALLABLE;
    else if (PyCallable_Check(o))
        Px_SOCKFLAGS(s) |= Px_SOCKFLAGS_INITIAL_BYTES_CALLABLE;
    else {
        PyErr_SetString(PyExc_ValueError, INVALID_INITIAL_BYTES);
        return 0;
    }

    snapshot = PxContext_HeapSnapshot(c, NULL);

    if (Px_SOCKFLAGS(s) & Px_SOCKFLAGS_INITIAL_BYTES_CALLABLE) {
        WSABUF w;
        PyObject *r;
        int error = 0;
        r = PyObject_CallObject(o, NULL);
        if (!r) {
            PxContext_RollbackHeap(c, &snapshot);
            return 0;
        }
        if (!PyObject2WSABUF(r, &w)) {
            PxContext_RollbackHeap(c, &snapshot);
            PyErr_SetString(PyExc_ValueError,
                            "initial_bytes_to_send() callable did not return "
                            "a sendable object (bytes, bytearray or unicode)");
            return 0;
        }
        s->initial_bytes_callable = o;
    } else {
        s->initial_bytes_callable = NULL;
        if (!PxSocket_IS_SERVERCLIENT(s)) {
            assert(!s->initial_bytes.buf);

            if (!PyObject2WSABUF(o, &s->initial_bytes)) {
                PxContext_RollbackHeap(c, &snapshot);
                PyErr_SetString(PyExc_ValueError,
                                "initial_bytes_to_send is not a sendable "
                                "object (bytes, bytearray or unicode)");
                return 0;
            }

        } else {
            s->initial_bytes.len = s->parent->initial_bytes.len;
            s->initial_bytes.buf = s->parent->initial_bytes.buf;
        }

        assert(s->initial_bytes.buf);
    }

    PxContext_RollbackHeap(c, &snapshot);

    return 1;
}


/* 0 = failure, 1 = success */
int
PxSocket_LoadInitialBytes(PxSocket *s)
{
    Context *c = s->ctx;
    PyObject *o, *r;

    if (!s->initial_bytes_callable)
        return 1;

    assert(Px_SOCKFLAGS(s) & Px_SOCKFLAGS_INITIAL_BYTES_CALLABLE);
    o = s->initial_bytes_callable;

    r = PyObject_CallObject(o, NULL);
    if (!r)
        return 0;

    if (!PyObject2WSABUF(r, &s->initial_bytes)) {
        PyErr_SetString(PyExc_ValueError,
                        "initial_bytes_to_send() callable did not return "
                        "a sendable object (bytes, bytearray or unicode)");
        return 0;
    }

    return 1;
}

void
PxSocket_InitExceptionHandler(PxSocket *s)
{
    Heap *old_heap = NULL;
    Context *c = s->ctx;
    PyObject *eh;
    assert(s->protocol);
    assert(!PyErr_Occurred());
    if (!s->exception_handler) {
        assert(Py_PXCTX);
        old_heap = ctx->h;
        ctx->h = s->ctx->h;
        eh = PyObject_GetAttrString(s->protocol, "exception_handler");
        if (eh && eh != Py_None)
            s->exception_handler = eh;
        else
            PyErr_Clear();
    }
    if (old_heap)
        ctx->h = old_heap;
    assert(!PyErr_Occurred());
}

void
NTAPI
PxSocket_Connect(PTP_CALLBACK_INSTANCE instance, void *context)
{
    /* This needs to be moved into PxSocket_IOLoop(), disable for now. */

    //Context *c = (Context *)context;
    //PxState *px;
    //PTP_WIN32_IO_CALLBACK cb;
    //BOOL success;
    //SOCKET fd;
    //struct sockaddr *sa;
    //int len;
    //PxSocketBuf *b;
    //char *cbuf = NULL;
    //size_t size = 0;
    //WSAOVERLAPPED *ol;
    //PyObject *result = NULL;
    //PxSocket *s = (PxSocket *)c->io_obj;
    //struct sockaddr_in *sin;

    //Px_GUARD


    //ENTERED_CALLBACK();

    /*
    if (!PxSocket_InitProtocol(c))
        goto end;
    */

//    assert(s->protocol);
//    assert(!PyErr_Occurred());
//
//    PxSocket_InitExceptionHandler(s);
//
//    b = _pxsocket_initial_bytes_to_send(c, s);
//    if (PyErr_Occurred())
//        PxSocket_EXCEPTION();
//
//    if (b) {
//        cbuf = b->w.buf;
//        size = b->w.len;
//    }
//
//    px = c->px;
//
//    sin = &(s->local_addr.in);
//    sin->sin_family = AF_INET;
//    sin->sin_addr.s_addr = INADDR_ANY;
//    sin->sin_port = 0;
//    if (bind(s->sock_fd, (struct sockaddr *)sin, sizeof(*sin)))
//        PxSocket_WSAERROR("bind");
//
//    c->io_type = Px_IOTYPE_SOCKET;
//    s->this_io_op = PxSocket_IO_CONNECT;
//
//    ol = &(s->overlapped_connectex);
//    PxOverlapped_Reset(ol);
//
//    sa = (struct sockaddr *)&(s->remote_addr.sa);
//    len = s->remote_addr_len;
//    fd = s->sock_fd;
//
//    StartThreadpoolIo(s->tp_io);
//    Px_SOCKFLAGS(s) |= Px_SOCKFLAGS_CONNECTED;
//    success = ConnectEx(fd, sa, len, cbuf, (DWORD)size, NULL, ol);
//    if (!success) {
//        if (WSAGetLastError() != WSA_IO_PENDING) {
//            Px_SOCKFLAGS(s) &= ~Px_SOCKFLAGS_CONNECTED;
//            s->exception_handler = NULL;
//            PxSocket_WSAERROR("ConnectEx");
//        }
//    } else {
//        PTP_CALLBACK_INSTANCE i = c->instance;
//        CancelThreadpoolIo(s->tp_io);
//        cb(NULL, c, ol, NO_ERROR, 0, NULL);
//    }
//
//end:
//    return;
}

void
NTAPI
PxSocketServer_CreateClientSocket(
    PTP_CALLBACK_INSTANCE instance,
    void *context
)
{
    Context  *c = (Context *)context;
    PxSocket *s = (PxSocket *)c->io_obj;

    PxSocket *o; /* client socket */
    Context  *x; /* client socket's context */
    int flags = Px_SOCKFLAGS_SERVERCLIENT;

    x = new_context(0, 1);
    if (!x)
        /* xxx: well... this is going to get silently ignored. */
        return;

    _PyParallel_EnteredCallback(x, instance);

    o = (PxSocket *)create_pxsocket(NULL, NULL, flags, s, x);

    if (!o) {
        PxSocket_CallbackComplete(x);
        return;
    }

    assert(PxSocket_IS_SERVERCLIENT(o));
    assert(s->protocol_type);
    assert(o->parent == s);

    assert(Px_PTR(x->io_obj) == Px_PTR(o));

    o->next_io_op = PxSocket_IO_ACCEPT;

    EnterCriticalSection(&(o->cs));
    PxSocket_IOLoop(o);
    LeaveCriticalSection(&(o->cs));
    return;
}

/* This is the entry point for a socket server.  It is called in a parallel
 * thread after an async.register(transport, protocol) call has been made.
 * It is responsible for:
 *      - Initializing the socket exception handlers.
 *      - Initializing the initial bytes.
 *      - Calling bind() against the socket.
 *      - Calling listen() against the socket.
 *      - Creating client sockets.
 *      - Submitting threadpool waits for the following:
 *          - Shutdown event.
 *          - Low system memory.
 *          - FD_ACCEPT.
 *          - Client connected event.
 * Once that is done, the method returns.  If an error occurs at any point,
 * one of the PxSocket_*ERROR() macros is used, which means an attempt will be
 * made to call the socket's registered exception handler, then the socket
 * will be closed if applicable, and the context pushed to the main thread's
 * callback complete list for subsequent cleanup.
 *
 * xxx: meh, for now, just keep using the WaitForMultipleObjects approach.
 */
void
NTAPI
PxSocketServer_Start(PTP_CALLBACK_INSTANCE instance, void *context)
{
    Context *c = (Context *)context;
    PxState *px;
    PTP_SIMPLE_CALLBACK work_cb = PxSocketServer_CreateClientSocket;
    char failed = 0;
    struct sockaddr *sa;
    int len;
    DWORD result;
    char *buf = NULL;
    PxSocket *s = (PxSocket *)c->io_obj;
    PyTypeObject *tp = &PxSocket_Type;
    int low_memory_wait = 0;
    int num_accepts_to_post;

    Px_GUARD

    ENTERED_CALLBACK();

    if (_PyParallel_HitHardMemoryLimit()) {
        PyErr_NoMemory();
        PxSocket_FATAL();
    }

    assert(s->protocol_type);
    assert(s->protocol);

    PxSocket_InitExceptionHandler(s);

    assert(PxSocket_IS_SERVER(s));

    assert(s->protocol);
    assert(!PyErr_Occurred());

    if (!PxSocket_InitInitialBytes(s))
        PxSocket_FATAL();

    px = c->px;
    s->this_io_op = PxSocket_IO_LISTEN;

    s->client_connected = CreateEvent(0, 0, 0, 0);
    if (!s->client_connected)
        PxSocket_SYSERROR("CreateEvent(s->client_connected)");

    /*
    s->client_connected_tp_wait = CreateThreadpoolWait(
        PxSocketServer_ClientConnectedWaitCallback,
        s->ctx,
        NULL
    );

    if (!s->client_connected_tp_wait)
        PxSocket_SYSERROR("CreateThreadpoolWait("
                          "PxSocketServer_ClientConnectedWaitCallback)");
    */

    s->shutdown = CreateEvent(0, 0, 0, 0);
    if (!s->shutdown)
        PxSocket_SYSERROR("CreateEvent(s->shutdown)");

    s->low_memory = CreateMemoryResourceNotification(
        LowMemoryResourceNotification
    );
    if (!s->low_memory)
        PxSocket_SYSERROR("CreateMemoryResourceNotification(LowMemory)");

    s->high_memory = CreateMemoryResourceNotification(
        HighMemoryResourceNotification
    );
    if (!s->high_memory)
        PxSocket_SYSERROR("CreateMemoryResourceNotification(HighMemory)");

    s->fd_accept = WSACreateEvent();
    if (!s->fd_accept)
        PxSocket_WSAERROR("WSACreateEvent(s->fd_accept)");

    s->wait_handles[0] = s->fd_accept;
    s->wait_handles[1] = s->client_connected;
    s->wait_handles[2] = s->shutdown;
    s->wait_handles[3] = s->low_memory;
    s->wait_handles[4] = s->high_memory;

    num_accepts_to_post = s->num_initial_accepts_to_post;

//do_bind:
    /* xxx todo: handle ADDRINUSE etc. */
    sa = (struct sockaddr *)&(s->local_addr.in);
    len = s->local_addr_len;
    if (bind(s->sock_fd, sa, len) == SOCKET_ERROR)
        PxSocket_WSAERROR("bind");

//do_listen:
    if (listen(s->sock_fd, SOMAXCONN) == SOCKET_ERROR)
        PxSocket_WSAERROR("listen");

select_fd_accept:
    if (WSAEventSelect(s->sock_fd, s->fd_accept, FD_ACCEPT) == SOCKET_ERROR)
        PxSocket_WSAERROR("WSAEventSelect(FD_ACCEPT)");

post_accepts:
    if (num_accepts_to_post <= 0)
        num_accepts_to_post = _PyParallel_NumCPUs * 2;

    /*
    while (num_accepts_to_post--) {
        s->total_accepts_attempted++;
        if (!TrySubmitThreadpoolCallback(work_cb, c, NULL))
            PxSocket_SYSERROR("TrySubmitThreadpoolCallback("
                              "PxSocketServer_CreateClientSocket)");
    }
    */
    while (num_accepts_to_post--) {
        s->total_accepts_attempted++;
        PxSocketServer_CreateClientSocket(NULL, c);
        ctx = c;
        if (PyErr_Occurred())
            PxSocket_EXCEPTION();
    }

wait:
    if (low_memory_wait)
        /* Only wait on shutdown and high memory events. */
        result = WaitForMultipleObjects(2, &(s->wait_handles[3]), 0, 5000);
    else
        /* Wait on everything except high memory. */
        result = WaitForMultipleObjects(4, &(s->wait_handles[0]), 0, 5000);

    switch (result) {
        case WAIT_OBJECT_0:
            /* fd_accept */
            num_accepts_to_post = s->num_additional_accepts_to_post;
            goto select_fd_accept;

        case WAIT_OBJECT_0 + 1:
            /* client_connected */
            num_accepts_to_post = 1;
            goto post_accepts;

        case WAIT_OBJECT_0 + 2:
            /* low memory */
            low_memory_wait = 1;
            goto low_memory;

        case WAIT_OBJECT_0 + 3:
            /* shutdown event */
            goto shutdown;

        case WAIT_OBJECT_0 + 4:
            /* high memory */
            low_memory_wait = 0;
            goto wait;

        case WAIT_TIMEOUT:
            goto timeout;

        case WAIT_ABANDONED_0:
        case WAIT_ABANDONED_0 + 1:
        case WAIT_ABANDONED_0 + 2:
        case WAIT_ABANDONED_0 + 3:
        case WAIT_ABANDONED_0 + 4:
            goto end;

        case WAIT_FAILED:
            PyErr_SetFromWindowsErr(0);
            PxSocket_HandleException(c, "WaitForMultipleObjects", 1);
            goto end;

        default:
            assert(0);
    }

shutdown:
    assert(0);

low_memory:
    /* xxx todo... close idle connections... potentially shut down the listen
     * socket? */
    goto wait;

timeout:
    /* Another xxx todo... close idle sockets... */
    goto wait;

end:
    if (s->sock_fd != INVALID_SOCKET) {
        closesocket(s->sock_fd);
        s->sock_fd = INVALID_SOCKET;
        if (s->tp_io) {
            CloseThreadpoolIo(s->tp_io);
            s->tp_io = NULL;
        }
    }
    PxSocket_CallbackComplete(s->ctx);
}

PyObject *
PxSocket_Register(PyObject *transport, PyObject *protocol_type)
{
    PxState *px = PXSTATE();
    PxSocket *s = (PxSocket *)transport;
    Context *c = s->ctx;
    PxListItem *item;
    PTP_SIMPLE_CALLBACK cb;

    assert(c);

    if (!PxSocket_SetProtocolType(s, protocol_type))
        return NULL;

    if (PxSocket_IS_CLIENT(s))
        cb = PxSocket_Connect;
    else
        cb = PxSocketServer_Start;

    if (Py_PXCTX) {
        /* If we're a parallel thread, we can just palm off the work to
         * TrySubmitThreadpoolCallback directly.
         */
        if (!TrySubmitThreadpoolCallback(cb, c, NULL))
            PxSocket_SYSERROR("TrySubmitThreadpoolCallback");

    } else {
        /* We can't call TrySubmitThreadpoolCallback() here; we need to
         * have it called after our calling Python code invokes async.run().
         * Otherwise, parallel contexts will start running immediately.
         */

        item = _PyHeap_NewListItem(c);
        if (!item)
            return NULL;

        item->from = c;
        item->p1 = cb;

        Py_INCREF(transport);
        Py_INCREF(protocol_type);
        /* _async_run_once() will decrement whatever's left in p2->p4 if the
         * TrySubmitThreadpoolCallback(cb, c, NULL) call fails. */
        item->p2 = transport;
        item->p3 = protocol_type;

        PxList_Push(px->new_threadpool_work, item);
    }

    Py_RETURN_NONE;

end:
    /* For PxSocket_SYSERROR's goto. */
    return NULL;
}


PyObject *
pxsocket_close(PxSocket *s, PyObject *args)
{
    WRITE_LOCK(s);
    Px_SOCKFLAGS(s) |= Px_SOCKFLAGS_CLOSE_SCHEDULED;
    WRITE_UNLOCK(s);
    Py_RETURN_NONE;
}

PyDoc_STRVAR(pxsocket_next_send_id_doc, "xxx todo\n");

PyObject *
pxsocket_next_send_id(PxSocket *s, PyObject *args)
{
    return PyLong_FromUnsignedLongLong(s->send_id+1);
}

PyDoc_STRVAR(pxsocket_sendfile_doc, "xxx todo\n");

PyObject *
pxsocket_sendfile(PxSocket *s, PyObject *args)
{
    PyObject *result = NULL;
    LPCWSTR name;
    Py_UNICODE *uname;
    int name_len;
    int access = GENERIC_READ;
    int share = FILE_SHARE_READ;
    int create_flags = OPEN_EXISTING;
    int file_flags = (
        FILE_FLAG_OVERLAPPED    |
        FILE_ATTRIBUTE_READONLY |
        FILE_FLAG_SEQUENTIAL_SCAN
    );
    HANDLE h;
    LARGE_INTEGER size;
    TRANSMIT_FILE_BUFFERS *tf;

    char *before_bytes = NULL, *after_bytes = NULL;
    int before_len = 0, after_len = 0;
    DWORD max_fsize = INT_MAX - 1;

    //Px_GUARD

    if (PxSocket_IS_SENDFILE_SCHEDULED(s)) {
        PyErr_SetString(PyExc_RuntimeError,
                        "sendfile already scheduled for this callback");
        goto done;
    }

    assert(!s->sendfile_handle);

    if (!PyArg_ParseTuple(args, "z#u#z#:sendfile",
                          &before_bytes, &before_len,
                          &uname, &name_len,
                          &after_bytes, &after_len))
        goto done;

    name = (LPCWSTR)uname;

    h = CreateFile(name, access, share, 0, create_flags, file_flags, 0);
    if (!h || (h == INVALID_HANDLE_VALUE)) {
        PyErr_SetFromWindowsErrWithUnicodeFilename(0, uname);
        goto done;
    }

    if (!GetFileSizeEx(h, &size)) {
        CloseHandle(h);
        PyErr_SetFromWindowsErrWithUnicodeFilename(0, uname);
        goto done;
    }

    /* Subtract before/after buffer sizes from maximum sendable file size. */
    max_fsize -= before_len;
    max_fsize -= after_len;

    if ((size.QuadPart > (long long)INT_MAX) || (size.LowPart > max_fsize)) {
        CloseHandle(h);
        PyErr_SetString(PyExc_ValueError,
                        "file is too large to send via sendfile()");
        goto done;
    }

    tf = &s->sendfile_tfbuf;
    memset(tf, 0, sizeof(TRANSMIT_FILE_BUFFERS));
    if (before_len) {
        tf->Head = before_bytes;
        tf->HeadLength = before_len;
    }
    if (after_len) {
        tf->Tail = after_bytes;
        tf->TailLength = after_len;
    }

    s->sendfile_nbytes = size.LowPart + before_len + after_len;
    s->sendfile_handle = h;
    Px_SOCKFLAGS(s) |= Px_SOCKFLAGS_SENDFILE_SCHEDULED;
    result = Py_None;

done:
    if (!result)
        assert(PyErr_Occurred());

    return result;
}

#define _PXSOCKET(n, a) _METHOD(pxsocket, n, a)
#define _PXSOCKET_N(n) _PXSOCKET(n, METH_NOARGS)
#define _PXSOCKET_O(n) _PXSOCKET(n, METH_O)
#define _PXSOCKET_V(n) _PXSOCKET(n, METH_VARARGS)

static PyMethodDef PxSocketMethods[] = {
    _PXSOCKET_N(close),
    _PXSOCKET_V(sendfile),
    _PXSOCKET_N(next_send_id),
    { NULL, NULL }
};

#define _MEMBER(n, t, c, f, d) {#n, t, offsetof(c, n), f, d}
#define _PXSOCKETMEM(n, t, f, d)  _MEMBER(n, t, PxSocket, f, d)
#define _PXSOCKET_CB(n)        _PXSOCKETMEM(n, T_OBJECT,    0, #n " callback")
#define _PXSOCKET_ATTR_O(n)    _PXSOCKETMEM(n, T_OBJECT_EX, 0, #n " callback")
#define _PXSOCKET_ATTR_OR(n)   _PXSOCKETMEM(n, T_OBJECT_EX, 1, #n " callback")
#define _PXSOCKET_ATTR_I(n)    _PXSOCKETMEM(n, T_INT,       0, #n " attribute")
#define _PXSOCKET_ATTR_IR(n)   _PXSOCKETMEM(n, T_INT,       1, #n " attribute")
#define _PXSOCKET_ATTR_B(n)    _PXSOCKETMEM(n, T_BOOL,      0, #n " attribute")
#define _PXSOCKET_ATTR_BR(n)   _PXSOCKETMEM(n, T_BOOL,      1, #n " attribute")
#define _PXSOCKET_ATTR_D(n)    _PXSOCKETMEM(n, T_DOUBLE,    0, #n " attribute")
#define _PXSOCKET_ATTR_DR(n)   _PXSOCKETMEM(n, T_DOUBLE,    1, #n " attribute")
#define _PXSOCKET_ATTR_S(n)    _PXSOCKETMEM(n, T_STRING,    0, #n " attribute")

static PyMemberDef PxSocketMembers[] = {
    _PXSOCKET_ATTR_S(ip),
    _PXSOCKET_ATTR_S(host),
    _PXSOCKET_ATTR_IR(port),

    /* underlying socket (readonly) */
    _PXSOCKET_ATTR_IR(sock_family),
    _PXSOCKET_ATTR_IR(sock_type),
    _PXSOCKET_ATTR_IR(sock_proto),
    _PXSOCKET_ATTR_DR(sock_timeout),
    /*_PXSOCKET_ATTR_IR(sock_backlog),*/

    ///* handler */
    //_PXSOCKET_ATTR_O(handler),

    ///* callbacks */
    //_PXSOCKET_CB(connection_made),
    //_PXSOCKET_CB(data_received),
    //_PXSOCKET_CB(lines_received),
    //_PXSOCKET_CB(eof_received),
    //_PXSOCKET_CB(connection_lost),
    //_PXSOCKET_CB(exception_handler),
    //_PXSOCKET_CB(initial_connection_error),

    ///* attributes */
    //_PXSOCKET_ATTR_S(eol),
    //_PXSOCKET_ATTR_B(lines_mode),
    //_PXSOCKET_ATTR_BR(is_client),
    //_PXSOCKET_ATTR_I(wait_for_eol),
    //_PXSOCKET_ATTR_BR(is_connected),
    //_PXSOCKET_ATTR_I(max_line_length),

    { NULL }
};

static PyTypeObject PxSocket_Type = {
    PyVarObject_HEAD_INIT(0, 0)
    "_async.socket",                            /* tp_name */
    sizeof(PxSocket),                           /* tp_basicsize */
    0,                                          /* tp_itemsize */
    (destructor)pxsocket_dealloc,               /* tp_dealloc */
    0,                                          /* tp_print */
    0,                                          /* tp_getattr */
    0,                                          /* tp_setattr */
    0,                                          /* tp_reserved */
    0,                                          /* tp_repr */
    0,                                          /* tp_as_number */
    0,                                          /* tp_as_sequence */
    0,                                          /* tp_as_mapping */
    0,                                          /* tp_hash */
    0,                                          /* tp_call */
    0,                                          /* tp_str */
    PyObject_GenericGetAttr,                    /* tp_getattro */
    PyObject_GenericSetAttr,                    /* tp_setattro */
    0,                                          /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT,                         /* tp_flags */
    "Asynchronous Socket Objects",              /* tp_doc */
    0,                                          /* tp_traverse */
    0,                                          /* tp_clear */
    0,                                          /* tp_richcompare */
    0,                                          /* tp_weaklistoffset */
    0,                                          /* tp_iter */
    0,                                          /* tp_iternext */
    PxSocketMethods,                            /* tp_methods */
    PxSocketMembers,                            /* tp_members */
    0,                                          /* tp_getset */
    0,                                          /* tp_base */
    0,                                          /* tp_dict */
    0,                                          /* tp_descr_get */
    0,                                          /* tp_descr_set */
    0,                                          /* tp_dictoffset */
    0,                                          /* tp_init */
    PyType_GenericAlloc,                        /* tp_alloc */
    0,                                          /* tp_new */
    0,                                          /* tp_free */
};

PyObject *
_async_client_or_server(PyObject *self, PyObject *args,
                        PyObject *kwds, char is_client)
{
    Py_RETURN_NONE;
}

/* addrinfo */



/* mod _async */
PyObject *
_async_client(PyObject *self, PyObject *args, PyObject *kwds)
{
    return create_pxsocket(args,
                           kwds,
                           Px_SOCKFLAGS_CLIENT,
                           NULL,  /* parent */
                           NULL); /* use_this_context hack */
}

PyObject *
_async_server(PyObject *self, PyObject *args, PyObject *kwds)
{
    return create_pxsocket(args,
                           kwds,
                           Px_SOCKFLAGS_SERVER,
                           NULL,  /* parent */
                           NULL); /* use_this_context hack */
}

PyObject *
_async_read(PyObject *self, PyObject *args, PyObject *kwds)
{
    return NULL;
}

PyObject *
_async_write(PyObject *self, PyObject *args, PyObject *kwds)
{
    return NULL;
}

PyDoc_STRVAR(_async_print_doc, "xxx todo\n");
PyObject *
_async_print(PyObject *self, PyObject *args)
{
    PyObject_Print(args, stdout, 0);
    Py_RETURN_NONE;
}

PyDoc_STRVAR(_async_stdout_doc, "xxx todo\n");
PyObject *
_async_stdout(PyObject *self, PyObject *o)
{
    Py_INCREF(o);
    PyObject_Print(o, stdout, Py_PRINT_RAW);
    Py_RETURN_NONE;
}

PyDoc_STRVAR(_async_stderr_doc, "xxx todo\n");
PyObject *
_async_stderr(PyObject *self, PyObject *o)
{
    Py_INCREF(o);
    PyObject_Print(o, stderr, Py_PRINT_RAW);
    Py_RETURN_NONE;
}

PyDoc_STRVAR(_async_register_doc,
"register(transport=object, protocol=object) -> None\n\
\n\
Register an asynchronous transport object with the given protocol.");
PyObject *
_async_register(PyObject *self, PyObject *args, PyObject *kwds)
{
    PyObject *transport;
    PyObject *protocol_type;

    static const char *kwlist[] = { "transport", "protocol", NULL };
    static const char *fmt = "OO:register";

    Py_GUARD

    if (!PyArg_ParseTupleAndKeywords(args, kwds, fmt, (char **)kwlist,
                                     &transport, &protocol_type))
        return NULL;

    if (PxSocket_Check(transport))
        return PxSocket_Register(transport, protocol_type);
    else {
        PyErr_SetString(PyExc_ValueError, "unsupported async object");
        return NULL;
    }
}

static PyObject *_asyncmodule_obj;

PyDoc_STRVAR(_async_refresh_memory_stats_doc, "xxx todo\n");
PyObject *
_async_refresh_memory_stats(PyObject *obj, PyObject *args)
{
    PyObject *m;
    MEMORYSTATUSEX ms;
    Py_GUARD

    m = _asyncmodule_obj;

    ms.dwLength = sizeof(ms);
    GlobalMemoryStatusEx(&ms);

    /* Erm, this is not the way this should be done (from a CPython
     * perspective).  (I'm sure I should be reflecting the C-struct
     * somehow.  Something to look into...)
     */

#define _refresh(n, ull)                        \
    do {                                        \
        PyObject *o = PyLong_FromLongLong(ull); \
        if (PyModule_AddObject(m, n, o))        \
            return NULL;                        \
    } while (0)

    _refresh("_memory_load", ms.dwMemoryLoad);
    _refresh("_memory_total_virtual", ms.ullTotalVirtual);
    _refresh("_memory_avail_virtual", ms.ullAvailVirtual);
    _refresh("_memory_total_physical", ms.ullTotalPhys);
    _refresh("_memory_avail_physical", ms.ullAvailPhys);
    _refresh("_memory_total_page_file", ms.ullTotalPageFile);
    _refresh("_memory_avail_page_file", ms.ullAvailPageFile);

    Py_RETURN_NONE;
}

#define _ASYNC(n, a) _METHOD(_async, n, a)
#define _ASYNC_N(n) _ASYNC(n, METH_NOARGS)
#define _ASYNC_O(n) _ASYNC(n, METH_O)
#define _ASYNC_V(n) _ASYNC(n, METH_VARARGS)
#define _ASYNC_K(n) _ASYNC(n, METH_VARARGS | METH_KEYWORDS)
PyMethodDef _async_methods[] = {
    _ASYNC_V(map),
    _ASYNC_N(run),
    _ASYNC_O(wait),
    _ASYNC_V(read),
    _ASYNC_K(dict),
    _ASYNC_K(list),
    /*_ASYNC_N(xlist),*/
    //_ASYNC_V(open),
    _ASYNC_V(print),
    _ASYNC_V(write),
    _ASYNC_N(rdtsc),
    { "stdout", (PyCFunction)_async_stdout, METH_O, _async_stdout_doc },
    { "stderr", (PyCFunction)_async_stderr, METH_O, _async_stderr_doc },
    _ASYNC_O(_close),
    _ASYNC_O(signal),
    _ASYNC_K(client),
    _ASYNC_K(server),
    _ASYNC_O(protect),
    //_ASYNC_O(wait_any),
    //_ASYNC_O(wait_all),
    _ASYNC_O(prewait),
    _ASYNC_O(_address),
    _ASYNC_O(_rawfile),
    _ASYNC_K(register),
    _ASYNC_N(run_once),
    _ASYNC_N(cpu_count),
    _ASYNC_O(unprotect),
    _ASYNC_O(protected),
    _ASYNC_N(is_active),
    _ASYNC_V(submit_io),
    _ASYNC_O(read_lock),
    _ASYNC_V(_post_open),
    _ASYNC_V(fileopener),
    _ASYNC_V(filecloser),
    _ASYNC_O(write_lock),
    _ASYNC_V(submit_work),
    _ASYNC_V(submit_wait),
    _ASYNC_O(read_unlock),
    _ASYNC_O(write_unlock),
    _ASYNC_N(is_active_ex),
    _ASYNC_N(active_count),
    _ASYNC_O(_dbg_address),
    _ASYNC_V(submit_timer),
    _ASYNC_O(submit_class),
    _ASYNC_O(submit_client),
    _ASYNC_O(submit_server),
    _ASYNC_O(try_read_lock),
    _ASYNC_O(try_write_lock),
    _ASYNC_V(submit_write_io),
    _ASYNC_V(signal_and_wait),
    _ASYNC_N(active_contexts),
    _ASYNC_N(is_parallel_thread),
    _ASYNC_N(persisted_contexts),
    _ASYNC_N(refresh_memory_stats),
    _ASYNC_V(call_from_main_thread),
    _ASYNC_V(call_from_main_thread_and_wait),

    { NULL, NULL } /* sentinel */
};

struct PyModuleDef _asyncmodule = {
    PyModuleDef_HEAD_INIT,
    "_async",
    _async_doc,
    -1, /* multiple "initialization" just copies the module dict. */
    _async_methods,
    NULL,
    NULL,
    NULL,
    NULL
};

PyObject *
_PyAsync_ModInit(void)
{
    PyObject *m;
    PySocketModule_APIObject *socket_api;

    if (!PyType_Ready(&PxSocket_Type) < 0)
        return NULL;

    if (!PyType_Ready(&PyXList_Type) < 0)
        return NULL;

    m = PyModule_Create(&_asyncmodule);
    if (m == NULL)
        return NULL;

    _asyncmodule_obj = m;
    _async_refresh_memory_stats(NULL, NULL);

    socket_api = PySocketModule_ImportModuleAndAPI();
    if (!socket_api)
        return NULL;
    PySocketModule = *socket_api;

    if (PyModule_AddIntConstant(m, "_bits", Px_INTPTR_BITS))
        return NULL;

    if (PyModule_AddIntConstant(m, "_mem_page_size", Px_PAGE_SIZE))
        return NULL;

    if (PyModule_AddIntConstant(m, "_mem_cache_line_size",
                                Px_CACHE_ALIGN_SIZE))
        return NULL;

    if (PyModule_AddIntConstant(m, "_mem_large_page_size",
                                Px_LARGE_PAGE_SIZE))
        return NULL;

    if (PyModule_AddIntConstant(m, "_mem_default_heap_size",
                                Px_DEFAULT_HEAP_SIZE))
        return NULL;

    if (PyModule_AddIntConstant(m, "_mem_default_tls_heap_size",
                                Px_DEFAULT_TLS_HEAP_SIZE))
        return NULL;

    if (PyModule_AddIntConstant(m, "_sizeof_Heap", sizeof(Heap)))
        return NULL;

    if (PyModule_AddIntConstant(m, "_sizeof_Context", sizeof(Context)))
        return NULL;

    if (PyModule_AddIntConstant(m, "_sizeof_ContextStats", sizeof(Stats)))
        return NULL;

    if (PyModule_AddIntConstant(m, "_sizeof_PxSocket", sizeof(PxSocket)))
        return NULL;

    if (PyModule_AddIntConstant(m, "_sizeof_PxSocketBuf", sizeof(PxSocketBuf)))
        return NULL;

    if (PyModule_AddIntConstant(m, "_sizeof_Context", sizeof(Context)))
        return NULL;

    if (PyModule_AddIntConstant(m, "_sizeof_PyObject", sizeof(PyObject)))
        return NULL;

    if (PyModule_AddIntConstant(m, "_sizeof_PxObject", sizeof(PxObject)))
        return NULL;

    if (PyModule_AddIntConstant(m, "_sizeof_PxState", sizeof(PxState)))
        return NULL;

    if (PyModule_AddIntConstant(m, "_sizeof_PxHeap", sizeof(PxHeap)))
        return NULL;

    if (PyModule_AddIntConstant(m, "_sizeof_PxPages", sizeof(PxPages)))
        return NULL;

    if (PyModule_AddIntConstant(m, "_sizeof_PxListItem", sizeof(PxListItem)))
        return NULL;

    if (PyModule_AddIntConstant(m, "_sizeof_PxListHead", sizeof(PxListHead)))
        return NULL;

    if (PyModule_AddIntConstant(m, "_sizeof_PxIO", sizeof(PxIO)))
        return NULL;

    if (PyModule_AddIntConstant(m, "_sizeof_TLS", sizeof(TLS)))
        return NULL;

    if (PyModule_AddIntConstant(m, "_sizeof_TLSBUF", sizeof(TLSBUF)))
        return NULL;

    if (PyModule_AddIntConstant(m, "_sizeof_SBUF", sizeof(SBUF)))
        return NULL;

    if (PyModule_AddIntConstant(m, "_sizeof_RBUF", sizeof(RBUF)))
        return NULL;

    if (PyModule_AddObject(m, "socket", (PyObject *)&PxSocket_Type))
        return NULL;

    if (PyModule_AddObject(m, "xlist", (PyObject *)&PyXList_Type))
        return NULL;

    PyExc_AsyncError = PyErr_NewException("_async.AsyncError", NULL, NULL);
    if (!PyExc_AsyncError)
        return NULL;

    PyExc_ProtectionError = \
        PyErr_NewException("_async.ProtectionError", PyExc_AsyncError, NULL);
    if (!PyExc_ProtectionError)
        return NULL;

    PyExc_UnprotectedError = \
        PyErr_NewException("_async.UnprotectedError", PyExc_AsyncError, NULL);
    if (!PyExc_UnprotectedError)
        return NULL;

    PyExc_AssignmentError = \
        PyErr_NewException("_async.AssignmentError", PyExc_AsyncError, NULL);
    if (!PyExc_AssignmentError)
        return NULL;

    PyExc_PersistenceError = \
        PyErr_NewException("_async.PersistenceError", PyExc_AsyncError, NULL);
    if (!PyExc_PersistenceError)
        return NULL;

    PyExc_NoWaitersError = \
        PyErr_NewException("_async.NoWaitersError", PyExc_AsyncError, NULL);
    if (!PyExc_NoWaitersError)
        return NULL;

    PyExc_WaitError = \
        PyErr_NewException("_async.WaitError", PyExc_AsyncError, NULL);
    if (!PyExc_WaitError)
        return NULL;

    PyExc_WaitTimeoutError = \
        PyErr_NewException("_async.WaitTimeoutError", PyExc_AsyncError, NULL);
    if (!PyExc_WaitTimeoutError)
        return NULL;

    PyExc_AsyncIOBuffersExhaustedError = \
        PyErr_NewException("_async.AsyncIOBuffersExhaustedError",
                           PyExc_AsyncError,
                           NULL);
    if (!PyExc_AsyncIOBuffersExhaustedError)
        return NULL;

    if (PyModule_AddObject(m, "AsyncError", PyExc_AsyncError))
        return NULL;

    if (PyModule_AddObject(m, "ProtectionError", PyExc_ProtectionError))
        return NULL;

    if (PyModule_AddObject(m, "UnprotectedError", PyExc_UnprotectedError))
        return NULL;

    if (PyModule_AddObject(m, "AssignmentError", PyExc_AssignmentError))
        return NULL;

    if (PyModule_AddObject(m, "PersistenceError", PyExc_PersistenceError))
        return NULL;

    if (PyModule_AddObject(m, "NoWaitersError", PyExc_NoWaitersError))
        return NULL;

    if (PyModule_AddObject(m, "WaitError", PyExc_WaitError))
        return NULL;

    if (PyModule_AddObject(m, "WaitTimeoutError", PyExc_WaitTimeoutError))
        return NULL;

    if (PyModule_AddObject(m, "AsyncIOBuffersExhaustedError",
                           PyExc_AsyncIOBuffersExhaustedError))
        return NULL;

    Py_INCREF(PyExc_AsyncError);
    Py_INCREF(PyExc_ProtectionError);
    Py_INCREF(PyExc_UnprotectedError);
    Py_INCREF(PyExc_AssignmentError);
    Py_INCREF(PyExc_PersistenceError);
    Py_INCREF(PyExc_NoWaitersError);
    Py_INCREF(PyExc_WaitError);
    Py_INCREF(PyExc_WaitTimeoutError);
    Py_INCREF(PyExc_AsyncIOBuffersExhaustedError);

    return m;
}

#ifdef __cpplus
}
#endif

/* vim:set ts=8 sw=4 sts=4 tw=78 et nospell: */