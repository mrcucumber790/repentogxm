/* MSVC x86 C++ exception handling for the recompiled guest (GUEST_CXX_EH=1).
 *
 * The PE was built with /EHsc against VCRUNTIME140: `throw` is
 * `_CxxThrowException(object, ThrowInfo)`, every function with destructors
 * or try blocks links a registration node into the fs:[0] chain whose
 * handler thunk loads its FuncInfo and jumps to `__CxxFrameHandler3`.  On
 * Windows, RaiseException walks that chain twice (search, unwind).  Here the
 * same two phases are executed directly in C over the guest chain, with the
 * __CxxFrameHandler3 rules (vcruntime frame.cpp, x86 FH3):
 *
 *   search   for each C++ frame: try blocks whose [tryLow, tryHigh] holds the
 *            frame's state, in table order; each catch in order; each
 *            CatchableType of the thrown object in order (TypeMatch).
 *            Nothing found: the throw stops exactly where it stopped before
 *            this file existed (guest_fault at the _CxxThrowException import
 *            with the CPU untouched), so the bad_alloc diagnostics still see
 *            the thrower's frame.
 *   unwind   build the catch object, unwind every nested frame to state -1
 *            (destructor funclets, EBP = frame), unlink it from fs:[0],
 *            unwind the catching frame to tryLow, state = tryHigh + 1, run
 *            the catch funclet (EBP = frame, on the thrower's deep stack), then
 *            destroy the exception object and resume: fs:[0] = RN,
 *            EBP = RN + 0Ch, ESP = [RN - 4], EIP = the funclet's EAX.
 *
 * Resuming needs the native C activation of the generated function that
 * owns RN.  The generator (recomp/cxx_eh.py) gives every such body a
 * guest_eh_site registered at its `push __ehhandler$F` and a setjmp whose
 * second return dispatches on the continuation; this file longjmps there.
 * Funclets are ordinary translated roots reached through guest_call.
 *
 * Rethrow (`throw;` = _CxxThrowException(NULL, NULL)) reuses the innermost
 * exception whose catch funclet is running.  A catch block left by a new
 * exception destroys its own exception object (unless that object is the
 * one propagating), as vcruntime does when it unwinds a catch block.
 *
 * Out of scope (fails loudly): SEH __try/__finally frames that are active
 * while a C++ exception unwinds through them, dynamic exception
 * specifications, and C++ exceptions crossing a native Lua callback
 * boundary (Lua's own frames cannot be unwound by a guest catch). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "guest.h"

#if GUEST_CXX_EH

#if defined(ISAAC_VITA_HAS_RUNTIME) && ISAAC_VITA_HAS_RUNTIME
#include "host_vita_heap.h"
#define GUEST_EH_MALLOC(size) isaac_vita_guest_malloc(size)
#define GUEST_EH_FREE(pointer) isaac_vita_guest_free(pointer)
#else
#define GUEST_EH_MALLOC(size) malloc(size)
#define GUEST_EH_FREE(pointer) (free(pointer), 1)
#endif

#if defined(ISAAC_VITA_LUA) && ISAAC_VITA_LUA
/* host_vita_lua.c: guest ESP saved by the innermost native Lua callback
 * trampoline of this CPU (the guest stack above it belongs to frames that
 * called into native Lua); 0 when no callback is active. */
uint32_t isaac_vita_lua_callback_boundary_esp(CPU *c);
#define GUEST_EH_LUA_BOUNDARY(c) isaac_vita_lua_callback_boundary_esp(c)
#else
#define GUEST_EH_LUA_BOUNDARY(c) 0U
#endif

#define EH_END_OF_CHAIN        0xFFFFFFFFU
#define EH_FRAME_OFFSET        0x0CU     /* EBP = RN + 0Ch */
#define EH_FUNCLET_RETURN      0xFFF7EC00U  /* synthetic guest return word */

#define EH_MAGIC_LOW           0x19930520U
#define EH_MAGIC_HIGH          0x19930522U
#define FI_EHNOEXCEPT_FLAG     0x4U

#define HT_IS_CONST            0x01U
#define HT_IS_VOLATILE         0x02U
#define HT_IS_UNALIGNED        0x04U
#define HT_IS_REFERENCE        0x08U
#define HT_IS_STD_DOT_DOT      0x40U
#define HT_IS_BADALLOC_COMPAT  0x80U

#define CT_IS_SIMPLE_TYPE      0x01U
#define CT_BY_REFERENCE_ONLY   0x02U
#define CT_HAS_VIRTUAL_BASE    0x04U
#define CT_IS_STD_BAD_ALLOC    0x10U

#define TI_IS_CONST            0x01U
#define TI_IS_VOLATILE         0x02U
#define TI_IS_UNALIGNED        0x04U

#define CXX_THROW_NAME   "VCRUNTIME140.dll!_CxxThrowException"
#define CXX_COPY_NAME    "VCRUNTIME140.dll!__std_exception_copy"
#define CXX_DESTROY_NAME "VCRUNTIME140.dll!__std_exception_destroy"
#define CXX_FRAME_NAME   "VCRUNTIME140.dll!__CxxFrameHandler3"
#define EH4_COMMON_NAME  "VCRUNTIME140.dll!_except_handler4_common"

/* One exception whose catch funclet is running (c->eh_handling chain). */
typedef struct guest_eh_handling {
    struct guest_eh_handling *previous;
    uint32_t object;
    uint32_t throw_info;
    uint32_t catch_esp;     /* guest ESP the catch funclet was called with */
} guest_eh_handling;

typedef struct guest_eh_match {
    uint32_t rn;
    uint32_t funcinfo;
    uint32_t try_entry;     /* guest TryBlockMapEntry* */
    uint32_t handler_type;  /* guest HandlerType* */
    uint32_t catchable;     /* guest CatchableType* */
    guest_eh_site *site;
} guest_eh_match;

enum { EH_FRAME_FOREIGN = 0, EH_FRAME_CXX = 1, EH_FRAME_EH4 = 2 };

static unsigned s_eh_throw_log_budget = 64U;

/* ------------------------------------------------------- landing sites --- */

void guest_eh_frame_enter(CPU *__restrict c, guest_eh_site *site,
                          uint32_t handler)
{
    if (!c || !site) abort();
    if (site->active) {
        /* The same prologue instruction can only run again in this
         * activation through a guest loop, which MSVC never emits. */
        guest_fault(c, handler, "C++ EH frame registered twice");
        return;
    }
    site->cpu = c;
    site->registration = c->esp - 4U;  /* the next push stores old fs:[0] */
    site->handler = handler;
    site->continuation = 0U;
    site->previous = c->eh_sites;
    site->active = 1U;
    c->eh_sites = site;
}

void guest_eh_frame_cleanup(guest_eh_site *site)
{
    CPU *c;
    guest_eh_site *p;

    if (!site || !site->active)
        return;
    site->active = 0U;
    c = site->cpu;
    if (!c)
        return;
    if (c->eh_sites == site) {
        c->eh_sites = site->previous;
        return;
    }
    /* Pruned by a nonlocal transfer: already gone.  Found below a live site
     * instead: a broken LIFO, and later native jumps would be unsafe. */
    for (p = c->eh_sites; p; p = p->previous) {
        if (p == site) {
            fprintf(stderr, "guest: C++ EH landing site %08x left out of "
                            "order\n", (unsigned)site->registration);
            abort();
        }
    }
}

void guest_eh_prune(CPU *__restrict c, uint32_t guest_esp)
{
    guest_eh_handling *h;

    if (!c)
        return;
    while (c->eh_sites && c->eh_sites->registration < guest_esp) {
        c->eh_sites->active = 0U;
        c->eh_sites = c->eh_sites->previous;
    }
    h = (guest_eh_handling *)c->eh_handling;
    while (h && h->catch_esp < guest_esp)
        h = h->previous;
    c->eh_handling = h;
}

/* --------------------------------------------------------- guest reads --- */

static uint32_t eh_arg(CPU *__restrict c, uint32_t index)
{
    return ld32(guest_stack_address(c, c->esp + 4U + index * 4U, 4U, 0U));
}

static int eh_image_contains(uint32_t va, uint32_t size)
{
    return va >= (uint32_t)GUEST_IMAGE_BASE &&
           va - (uint32_t)GUEST_IMAGE_BASE < 0x00900000U - size;
}

/* The import named by a `jmp dword ptr [slot]` linker thunk at `va`. */
static const char *eh_thunk_import(uint32_t va)
{
    if (!eh_image_contains(va, 6U) || ld8(va) != 0xFFU || ld8(va + 1U) != 0x25U)
        return NULL;
    return guest_import_name(ld32(va + 2U));
}

static uint32_t eh_rel32_target(uint32_t at)
{
    return at + 5U + ld32(at + 1U);
}

typedef struct eh_handler_cache_entry {
    uint32_t handler;
    uint32_t funcinfo;
    uint32_t kind;
} eh_handler_cache_entry;

#define EH_HANDLER_CACHE 1024U
static eh_handler_cache_entry s_handler_cache[EH_HANDLER_CACHE];

/* Classify a registration's handler by its code, like the generator's
 * discovery.  An `__ehhandler$` thunk is optional NOP padding, then either
 * the tail directly or the /GS cookie checks (`mov edx, [esp+8]` ...), and
 * ends in `mov eax, offset FuncInfo ; jmp <__CxxFrameHandler3 thunk>`.
 * `_except_handler4` calls the `_except_handler4_common` thunk.  The window
 * is matched byte-wise (no decoder here), so no RET/INT3 byte may end the
 * scan early: such bytes occur inside displacements (`8B 4A CC`).  The
 * C++ tail must also name a FuncInfo with a valid magic. */
static uint32_t eh_classify_handler(uint32_t handler, uint32_t *funcinfo)
{
    eh_handler_cache_entry *entry =
        &s_handler_cache[(handler >> 2) % EH_HANDLER_CACHE];
    uint32_t kind = EH_FRAME_FOREIGN, fi = 0U, i, start = 0U;

    if (entry->handler == handler && handler) {
        *funcinfo = entry->funcinfo;
        return entry->kind;
    }
    if (eh_image_contains(handler, 80U)) {
        while (start < 16U && ld8(handler + start) == 0x90U)
            ++start;
        if (ld8(handler + start) == 0xB8U ||
            ld32(handler + start) == 0x0824548BU) {
            for (i = start; i + 10U <= start + 64U; ++i) {
                uint32_t at = handler + i;
                const char *name;
                uint32_t magic;
                if (ld8(at) != 0xB8U || ld8(at + 5U) != 0xE9U)
                    continue;
                name = eh_thunk_import(eh_rel32_target(at + 5U));
                if (!name || strcmp(name, CXX_FRAME_NAME) != 0)
                    continue;
                fi = ld32(at + 1U);
                if (!eh_image_contains(fi, 36U))
                    break;
                magic = ld32(fi);
                if (magic >= EH_MAGIC_LOW && magic <= EH_MAGIC_HIGH)
                    kind = EH_FRAME_CXX;
                break;
            }
            if (kind != EH_FRAME_CXX)
                fi = 0U;
        } else {
            for (i = start; i + 5U <= start + 64U; ++i) {
                const char *name;
                if (ld8(handler + i) != 0xE8U)
                    continue;
                name = eh_thunk_import(eh_rel32_target(handler + i));
                if (name && strcmp(name, EH4_COMMON_NAME) == 0) {
                    kind = EH_FRAME_EH4;
                    break;
                }
            }
        }
    }
    entry->handler = handler;
    entry->funcinfo = fi;
    entry->kind = kind;
    *funcinfo = fi;
    return kind;
}

static int eh_names_equal(uint32_t a, uint32_t b)
{
    uint32_t i;
    for (i = 0U; i < 4096U; ++i) {
        uint8_t x = ld8(a + i), y = ld8(b + i);
        if (x != y)
            return 0;
        if (!x)
            return 1;
    }
    return 0;
}

static void eh_copy_name(char *out, size_t size, uint32_t type_descriptor)
{
    size_t i;
    if (!type_descriptor) {
        snprintf(out, size, "...");
        return;
    }
    for (i = 0; i + 1 < size; ++i) {
        out[i] = (char)ld8(type_descriptor + 8U + (uint32_t)i);
        if (!out[i])
            return;
    }
    out[size - 1] = '\0';
}

/* ----------------------------------------------------------- funclets --- */

/* _CallSettingFrame: EBP = frame, near call, the funclet's own RET pops the
 * synthetic return word.  Returns the funclet's EAX. */
static uint32_t eh_call_funclet(CPU *__restrict c, uint32_t rn,
                                uint32_t funclet)
{
    uint32_t esp = c->esp;
    c->ebp = rn + EH_FRAME_OFFSET;
    gpush(c, EH_FUNCLET_RETURN);
    guest_call(c, funclet);
    if (c->esp != esp) {
        guest_fault(c, funclet, "C++ EH funclet did not balance the stack");
        abort();
    }
    return c->eax;
}

/* __thiscall member call with up to two stack arguments, callee-cleaned. */
static void eh_call_member(CPU *__restrict c, uint32_t function,
                           uint32_t self, uint32_t argc,
                           uint32_t arg0, uint32_t arg1)
{
    uint32_t esp = c->esp;
    if (argc > 1U)
        gpush(c, arg1);
    if (argc > 0U)
        gpush(c, arg0);
    c->ecx = self;
    gpush(c, EH_FUNCLET_RETURN);
    guest_call(c, function);
    if (c->esp != esp) {
        guest_fault(c, function,
                    "C++ EH member call did not balance the stack");
        abort();
    }
}

static void eh_destroy_object(CPU *__restrict c, uint32_t object,
                              uint32_t throw_info)
{
    uint32_t dtor;
    if (!object || !throw_info)
        return;
    dtor = ld32(throw_info + 4U);           /* ThrowInfo.pmfnUnwind */
    if (dtor)
        eh_call_member(c, dtor, object, 0U, 0U, 0U);
}

static int eh_object_in_handling(const guest_eh_handling *h, uint32_t object)
{
    for (; h; h = h->previous)
        if (h->object == object)
            return 1;
    return 0;
}

/* ------------------------------------------------------------ matching --- */

/* __AdjustPointer with a PMD {mdisp, pdisp, vdisp}. */
static uint32_t eh_adjust(uint32_t pointer, uint32_t pmd)
{
    int32_t mdisp = (int32_t)ld32(pmd);
    int32_t pdisp = (int32_t)ld32(pmd + 4U);
    int32_t vdisp = (int32_t)ld32(pmd + 8U);
    uint32_t result = pointer + (uint32_t)mdisp;
    if (pdisp >= 0) {
        uint32_t vbtable = ld32(pointer + (uint32_t)pdisp);
        result += ld32(vbtable + (uint32_t)vdisp) + (uint32_t)pdisp;
    }
    return result;
}

static int eh_type_match(uint32_t handler_type, uint32_t catchable,
                         uint32_t throw_info)
{
    uint32_t adjectives = ld32(handler_type);
    uint32_t catch_type = ld32(handler_type + 4U);
    uint32_t props = ld32(catchable);
    uint32_t thrown_type = ld32(catchable + 4U);
    uint32_t attributes = ld32(throw_info);

    if (!catch_type || ld8(catch_type + 8U) == 0U)
        return 1;                             /* catch (...) */
    if ((adjectives & HT_IS_BADALLOC_COMPAT) && (props & CT_IS_STD_BAD_ALLOC))
        return 1;
    if (catch_type != thrown_type && !eh_names_equal(catch_type + 8U,
                                                     thrown_type + 8U))
        return 0;
    return (!(props & CT_BY_REFERENCE_ONLY) ||
            (adjectives & HT_IS_REFERENCE)) &&
           (!(attributes & TI_IS_CONST) || (adjectives & HT_IS_CONST)) &&
           (!(attributes & TI_IS_UNALIGNED) ||
            (adjectives & HT_IS_UNALIGNED)) &&
           (!(attributes & TI_IS_VOLATILE) || (adjectives & HT_IS_VOLATILE));
}

static guest_eh_site *eh_find_site(CPU *__restrict c, uint32_t rn,
                                   uint32_t handler)
{
    guest_eh_site *site;
    for (site = c->eh_sites; site; site = site->previous)
        if (site->active && site->registration == rn &&
            site->handler == handler)
            return site;
    return NULL;
}

/* FindHandler for one C++ frame.  Returns 1 with *match filled. */
static int eh_find_in_frame(CPU *__restrict c, uint32_t rn, uint32_t fi,
                            uint32_t throw_info, guest_eh_match *match)
{
    int32_t max_state = (int32_t)ld32(fi + 4U);
    int32_t state = (int32_t)ld32(rn + 8U);
    uint32_t n_try = ld32(fi + 12U);
    uint32_t try_map = ld32(fi + 16U);
    uint32_t types = ld32(throw_info + 12U);  /* CatchableTypeArray* */
    uint32_t n_types = types ? ld32(types) : 0U;
    uint32_t t, k, j;

    if (state < -1 || state >= max_state) {
        guest_fault(c, rn, "C++ EH frame state is out of range");
        abort();
    }
    for (t = 0U; t < n_try; ++t) {
        uint32_t entry = try_map + t * 20U;
        int32_t low = (int32_t)ld32(entry);
        int32_t high = (int32_t)ld32(entry + 4U);
        uint32_t n_catch = ld32(entry + 12U);
        uint32_t handlers = ld32(entry + 16U);
        if (state < low || state > high)
            continue;
        for (k = 0U; k < n_catch; ++k) {
            uint32_t ht = handlers + k * 16U;
            for (j = 0U; j < n_types; ++j) {
                uint32_t catchable = ld32(types + 4U + j * 4U);
                if (!eh_type_match(ht, catchable, throw_info))
                    continue;
                match->rn = rn;
                match->funcinfo = fi;
                match->try_entry = entry;
                match->handler_type = ht;
                match->catchable = catchable;
                return 1;
            }
        }
    }
    return 0;
}

/* __FrameUnwindToState */
static void eh_unwind_frame(CPU *__restrict c, uint32_t rn, uint32_t fi,
                            int32_t target)
{
    int32_t max_state = (int32_t)ld32(fi + 4U);
    uint32_t unwind_map = ld32(fi + 8U);
    int32_t state = (int32_t)ld32(rn + 8U);

    while (state != target) {
        int32_t next;
        uint32_t action;
        if (state < 0 || state >= max_state) {
            guest_fault(c, rn, "C++ EH unwind state is out of range");
            abort();
        }
        next = (int32_t)ld32(unwind_map + (uint32_t)state * 8U);
        action = ld32(unwind_map + (uint32_t)state * 8U + 4U);
        if (action) {
            st32(rn + 8U, (uint32_t)next);
            (void)eh_call_funclet(c, rn, action);
        }
        state = next;
    }
    st32(rn + 8U, (uint32_t)state);
}

/* BuildCatchObject */
static void eh_build_catch_object(CPU *__restrict c, const guest_eh_match *m,
                                  uint32_t object)
{
    uint32_t adjectives = ld32(m->handler_type);
    uint32_t catch_type = ld32(m->handler_type + 4U);
    int32_t disp = (int32_t)ld32(m->handler_type + 8U);
    uint32_t props, pmd, size, copy, buffer, source, i;

    if ((adjectives & HT_IS_STD_DOT_DOT) || !catch_type ||
        ld8(catch_type + 8U) == 0U || disp == 0)
        return;
    buffer = m->rn + EH_FRAME_OFFSET + (uint32_t)disp;
    props = ld32(m->catchable);
    pmd = m->catchable + 8U;
    size = ld32(m->catchable + 20U);
    copy = ld32(m->catchable + 24U);
    if (adjectives & HT_IS_REFERENCE) {
        st32(buffer, eh_adjust(object, pmd));
    } else if (props & CT_IS_SIMPLE_TYPE) {
        for (i = 0U; i < size; ++i)
            st8(buffer + i, ld8(object + i));
        if (size == 4U && ld32(buffer))
            st32(buffer, eh_adjust(ld32(buffer), pmd));
    } else if (!copy) {
        source = eh_adjust(object, pmd);
        for (i = 0U; i < size; ++i)
            st8(buffer + i, ld8(source + i));
    } else if (props & CT_HAS_VIRTUAL_BASE) {
        eh_call_member(c, copy, buffer, 2U, eh_adjust(object, pmd), 1U);
    } else {
        eh_call_member(c, copy, buffer, 1U, eh_adjust(object, pmd), 0U);
    }
}

/* ----------------------------------------------------------- dispatch --- */

static void eh_describe(CPU *__restrict c, uint32_t object,
                        uint32_t throw_info, char *type, size_t type_size,
                        char *what, size_t what_size)
{
    uint32_t types = ld32(throw_info + 12U);
    uint32_t n = types ? ld32(types) : 0U, j;
    (void)c;
    type[0] = '\0';
    what[0] = '\0';
    if (n)
        eh_copy_name(type, type_size, ld32(ld32(types + 4U) + 4U));
    for (j = 0U; j < n; ++j) {
        uint32_t catchable = ld32(types + 4U + j * 4U);
        uint32_t descriptor = ld32(catchable + 4U);
        char name[64];
        eh_copy_name(name, sizeof name, descriptor);
        if (strcmp(name, ".?AVexception@std@@") == 0) {
            /* std::exception: vftable, __std_exception_data {what, free} */
            uint32_t self = eh_adjust(object, catchable + 8U);
            uint32_t text = ld32(self + 4U);
            size_t i;
            if (!text) {
                snprintf(what, what_size, "(null)");
                return;
            }
            for (i = 0; i + 1 < what_size; ++i) {
                what[i] = (char)ld8(text + (uint32_t)i);
                if (!what[i])
                    return;
            }
            what[what_size - 1] = '\0';
            return;
        }
    }
}

static GUEST_NORETURN void eh_unhandled(CPU *__restrict c, uint32_t addr,
                                        const char *why, uint32_t object,
                                        uint32_t throw_info)
{
    char type[96], what[160];
    eh_describe(c, object, throw_info, type, sizeof type, what, sizeof what);
    fprintf(stderr, "guest: C++ exception %s not handled (%s)%s%s%s\n",
            type[0] ? type : "?", why, what[0] ? ": what() = \"" : "",
            what, what[0] ? "\"" : "");
    /* Stop exactly as the unimplemented import did: same address and name,
     * CPU still at the throw call (no funclet has run). */
    guest_fault(c, addr, CXX_THROW_NAME);
    abort();
}

static GUEST_NORETURN void eh_dispatch(CPU *__restrict c, uint32_t addr,
                                       uint32_t object, uint32_t throw_info)
{
    guest_eh_match match;
    guest_eh_handling handling, *h;
    uint32_t fs, rn, boundary, fi, handler, kind, continuation, esp;
    int found = 0;

    memset(&match, 0, sizeof match);
    fs = guest_fs_base(c);
    if (!fs)
        eh_unhandled(c, addr, "no guest TIB", object, throw_info);
    if (!throw_info)
        eh_unhandled(c, addr, "null ThrowInfo", object, throw_info);
    boundary = GUEST_EH_LUA_BOUNDARY(c);

    /* ---- phase 1: search (no side effects) ---- */
    for (rn = ld32(fs); rn != EH_END_OF_CHAIN; rn = ld32(rn)) {
        if (!guest_stack_contains(c, rn, 12U) || rn < c->esp)
            eh_unhandled(c, addr, "corrupt fs:[0] chain", object, throw_info);
        if (boundary && rn >= boundary)
            eh_unhandled(c, addr, "would cross a native Lua callback",
                         object, throw_info);
        handler = ld32(rn + 4U);
        kind = eh_classify_handler(handler, &fi);
        if (kind != EH_FRAME_CXX)
            continue;                   /* SEH filters never take C++ here */
        if (eh_find_in_frame(c, rn, fi, throw_info, &match)) {
            found = 1;
            break;
        }
        if (ld32(fi) >= 0x19930522U && (ld32(fi + 32U) & FI_EHNOEXCEPT_FLAG)) {
            eh_unhandled(c, addr, "std::terminate: escaped a noexcept function",
                         object, throw_info);
        }
    }
    if (!found)
        eh_unhandled(c, addr, "no matching catch", object, throw_info);
    match.site = eh_find_site(c, match.rn, ld32(match.rn + 4U));
    if (!match.site)
        eh_unhandled(c, addr, "catching frame has no native landing site",
                     object, throw_info);

    if (s_eh_throw_log_budget) {
        char type[96], what[160];
        --s_eh_throw_log_budget;
        eh_describe(c, object, throw_info, type, sizeof type,
                    what, sizeof what);
        fprintf(stderr, "guest: C++ exception %s caught by frame %08x "
                        "(handler %08x)%s%s%s\n",
                type[0] ? type : "?", (unsigned)match.rn,
                (unsigned)ld32(match.rn + 4U),
                what[0] ? ": what() = \"" : "", what, what[0] ? "\"" : "");
    }

    /* ---- phase 2: unwind ---- */
    eh_build_catch_object(c, &match, object);

    /* Catch blocks running in frames below the target are left by this
     * exception: their exception objects end here (unless propagating). */
    h = (guest_eh_handling *)c->eh_handling;
    while (h && h->catch_esp < match.rn) {
        guest_eh_handling *left = h;
        h = h->previous;
        c->eh_handling = h;
        if (left->object != object && !eh_object_in_handling(h, left->object))
            eh_destroy_object(c, left->object, left->throw_info);
    }

    for (rn = ld32(fs); rn != match.rn; rn = ld32(fs)) {
        uint32_t next = ld32(rn);
        handler = ld32(rn + 4U);
        kind = eh_classify_handler(handler, &fi);
        if (kind == EH_FRAME_CXX) {
            eh_unwind_frame(c, rn, fi, -1);
        } else if (kind == EH_FRAME_EH4) {
            if (ld32(rn + 12U) != 0xFFFFFFFEU) {
                guest_fault(c, rn, "C++ exception unwinds an active SEH "
                                   "__try scope (unsupported)");
                abort();
            }
        } else {
            guest_fault(c, handler, "C++ exception unwinds a foreign SEH "
                                    "frame (unsupported)");
            abort();
        }
        st32(fs, next);
    }
    eh_unwind_frame(c, match.rn, match.funcinfo,
                    (int32_t)ld32(match.try_entry));
    st32(match.rn + 8U, ld32(match.try_entry + 4U) + 1U);

    handling.previous = (guest_eh_handling *)c->eh_handling;
    handling.object = object;
    handling.throw_info = throw_info;
    handling.catch_esp = c->esp;
    c->eh_handling = &handling;
    continuation = eh_call_funclet(c, match.rn,
                                   ld32(match.handler_type + 12U));
    c->eh_handling = handling.previous;
    if (!eh_object_in_handling(handling.previous, object))
        eh_destroy_object(c, object, throw_info);

    /* ---- resume at the continuation (_JumpToContinuation) ---- */
    st32(fs, match.rn);
    c->ebp = match.rn + EH_FRAME_OFFSET;
    esp = ld32(match.rn - 4U);
    if (!guest_stack_set(c, esp, continuation))
        abort();
    c->eax = continuation;
    if (eh_find_site(c, match.rn, ld32(match.rn + 4U)) != match.site) {
        guest_fault(c, match.rn, "C++ EH landing site vanished during unwind");
        abort();
    }
    guest_eh_prune(c, match.rn);
    if (c->eh_sites != match.site) {
        guest_fault(c, match.rn, "C++ EH landing sites are out of order");
        abort();
    }
    while (c->jump_sites && c->jump_sites->guest_esp < esp)
        c->jump_sites = c->jump_sites->previous;
    match.site->continuation = continuation;
    longjmp(match.site->native_env, 1);
}

/* ------------------------------------------------------------ imports --- */

static GUEST_NORETURN void eh_throw(CPU *__restrict c, uint32_t addr)
{
    uint32_t object = eh_arg(c, 0U);
    uint32_t throw_info = eh_arg(c, 1U);

    if (!object && !throw_info) {
        /* `throw;` rethrows the exception of the innermost running catch. */
        guest_eh_handling *h = (guest_eh_handling *)c->eh_handling;
        if (!h)
            eh_unhandled(c, addr, "std::terminate: rethrow with no "
                                  "exception being handled", 0U, 0U);
        object = h->object;
        throw_info = h->throw_info;
    }
    eh_dispatch(c, addr, object, throw_info);
}

/* struct __std_exception_data { char const *_What; bool _DoFree; } */
static void eh_std_exception_copy(CPU *__restrict c)
{
    uint32_t from = eh_arg(c, 0U);
    uint32_t to = eh_arg(c, 1U);
    uint32_t what = ld32(from);
    int do_free = ld8(from + 4U) != 0U;

    if (!do_free || !what) {
        st32(to, what);
        st8(to + 4U, 0U);
    } else {
        uint32_t length = 0U, i;
        void *buffer;
        while (ld8(what + length))
            ++length;
        buffer = GUEST_EH_MALLOC((size_t)length + 1U);
        if (buffer) {
            uint32_t copy = guest_addr(buffer);
            for (i = 0U; i <= length; ++i)
                st8(copy + i, ld8(what + i));
            st32(to, copy);
            st8(to + 4U, 1U);
        }
        /* Allocation failure leaves `to` as the caller zeroed it, exactly
         * like vcruntime. */
    }
    (void)gpop(c);                                  /* cdecl */
}

static void eh_std_exception_destroy(CPU *__restrict c)
{
    uint32_t data = eh_arg(c, 0U);
    if (ld8(data + 4U)) {
        uint32_t what = ld32(data);
        if (what && !GUEST_EH_FREE(guest_ptr(what))) {
            guest_fault(c, what, "__std_exception_destroy received a "
                                 "foreign guest pointer");
            return;
        }
    }
    st32(data, 0U);
    st8(data + 4U, 0U);
    (void)gpop(c);                                  /* cdecl */
}

int guest_cxx_eh_import(CPU *__restrict c, uint32_t addr, const char *name)
{
    if (!c || !name)
        return 0;
    if (strcmp(name, CXX_THROW_NAME) == 0) {
        ++g_host_import_calls;
        eh_throw(c, addr);
    }
    if (strcmp(name, CXX_COPY_NAME) == 0) {
        ++g_host_import_calls;
        eh_std_exception_copy(c);
        return 1;
    }
    if (strcmp(name, CXX_DESTROY_NAME) == 0) {
        ++g_host_import_calls;
        eh_std_exception_destroy(c);
        return 1;
    }
    return 0;
}

#endif /* GUEST_CXX_EH */
