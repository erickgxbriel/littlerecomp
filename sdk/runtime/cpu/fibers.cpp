// Cooperative guest fibers.
//
// The game runs several guest threads' worth of work as cooperative fibers on one host thread. The switch function
// (sub_003094E8 in Scarface) saves esp into the current descriptor, loads esp from the target descriptor, pops the
// target's callee saved registers and returns. On the real console that return lands in a different guest stack.
//
// Recompiled code mirrors the guest call chain with host frames, so switching only ctx.esp would leave the host
// return address pointing back into the switch function's C++ caller. Instead each guest descriptor gets a host
// fiber. Switching guest stacks switches host stacks too, which keeps every fiber's recompiled call chain intact.
//
// The recompiler detects the switch function and replaces its ret with x86_pivot_ret. At that ret ctx.esp is already
// the target stack, ctx.eax is the target descriptor and ctx.ecx the source descriptor.

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <unordered_map>

#ifndef _WIN32
#include <sys/mman.h>
#include <unistd.h>
#endif

#include "core/log.h"
#include "cpu/x86_ops.h"

namespace dispatch {
X86Func* Find(uint32_t guest);
}

namespace {

#ifdef _WIN32

// Host frames are larger than the guest's, and a fiber can be suspended as deep as its guest stack allows.
constexpr SIZE_T kFiberStack = 8u << 20;

struct FiberState {
    uint32_t desc = 0;       // guest descriptor address
    uint32_t entry = 0;      // first guest address of a new fiber; always the trampoline
    X86Context* ctx = nullptr;
    uint8_t* base = nullptr;
    bool started = false;    // the host fiber has run, so a switch to it resumes a host stack
    void* handle = nullptr;  // CreateFiber/ConvertThreadToFiber handle
};

thread_local std::unordered_map<uint32_t, FiberState*> g_fibers;
thread_local FiberState* g_current = nullptr;

void CALLBACK FiberEntry(void* param) {
    FiberState* s = static_cast<FiberState*>(param);
    g_current = s;
    LOGI("fiber %08X starts at %08X (esp %08X)", s->desc, s->entry, s->ctx->esp);
    if (X86Func* f = dispatch::Find(s->entry)) f(*s->ctx, s->base);
    x86_trap(*s->ctx, s->entry, "guest fiber function returned");
}

// the first switch on a host thread: make the thread itself a fiber so guest fibers can switch back to it
void EnsureCurrent(X86Context& ctx, uint8_t* base) {
    if (g_current) return;
    void* handle = ConvertThreadToFiber(nullptr);
    if (!handle) {
        DWORD err = GetLastError();
        if (err == ERROR_ALREADY_FIBER) handle = GetCurrentFiber();
        if (!handle) {
            LOGE("fiber: ConvertThreadToFiber failed (%lu)", (unsigned long)err);
            x86_trap(ctx, 0, "ConvertThreadToFiber failed");
        }
    }
    FiberState* s = new FiberState;
    s->desc = ctx.ecx;  // the source descriptor at the pivot is the current guest fiber
    s->ctx = &ctx;
    s->base = base;
    s->started = true;
    s->handle = handle;
    if (s->desc) g_fibers[s->desc] = s;
    g_current = s;
    LOGI("fiber %08X is the host thread", s->desc);
}

FiberState* EnsureFiber(uint32_t desc, X86Context& ctx, uint8_t* base) {
    auto it = g_fibers.find(desc);
    if (it != g_fibers.end()) return it->second;
    if (!desc) {
        LOGE("fiber: switch to descriptor 0 from %08X", g_current ? g_current->desc : 0);
        x86_trap(ctx, 0, "switch to a null fiber descriptor");
    }
    FiberState* s = new FiberState;
    s->desc = desc;
    s->ctx = &ctx;
    s->base = base;
    s->handle = CreateFiber(kFiberStack, FiberEntry, s);
    if (!s->handle) {
        LOGE("fiber: CreateFiber failed (%lu)", (unsigned long)GetLastError());
        x86_trap(ctx, 0, "CreateFiber failed");
    }
    g_fibers[desc] = s;
    LOGI("fiber %08X created", desc);
    return s;
}

#else  // !_WIN32

constexpr size_t kGuardSize = 64u << 10;  // covers both 4 KB and 16 KB Android page sizes
constexpr size_t kFiberStack = 8u << 20;

struct alignas(16) FiberCtx {
#if defined(__aarch64__)
    uint64_t x19, x20, x21, x22, x23, x24, x25, x26, x27, x28, x29, x30;
    uint64_t sp, pad;
    uint64_t d8, d9, d10, d11, d12, d13, d14, d15;
#elif defined(__x86_64__)
    uint64_t rbx, rbp, r12, r13, r14, r15, rsp, rip;
    uint32_t mxcsr;
    uint16_t fcw;
    uint16_t pad;
#else
#error "Unsupported architecture for cooperative fibers"
#endif
};

#if defined(__aarch64__)
static_assert(offsetof(FiberCtx, x19) == 0x00);
static_assert(offsetof(FiberCtx, x21) == 0x10);
static_assert(offsetof(FiberCtx, x23) == 0x20);
static_assert(offsetof(FiberCtx, x25) == 0x30);
static_assert(offsetof(FiberCtx, x27) == 0x40);
static_assert(offsetof(FiberCtx, x29) == 0x50);
static_assert(offsetof(FiberCtx, sp) == 0x60);
static_assert(offsetof(FiberCtx, d8) == 0x70);
static_assert(offsetof(FiberCtx, d10) == 0x80);
static_assert(offsetof(FiberCtx, d12) == 0x90);
static_assert(offsetof(FiberCtx, d14) == 0xA0);
static_assert(sizeof(FiberCtx) == 0xB0);
#elif defined(__x86_64__)
static_assert(offsetof(FiberCtx, rbx) == 0x00);
static_assert(offsetof(FiberCtx, rbp) == 0x08);
static_assert(offsetof(FiberCtx, r12) == 0x10);
static_assert(offsetof(FiberCtx, r13) == 0x18);
static_assert(offsetof(FiberCtx, r14) == 0x20);
static_assert(offsetof(FiberCtx, r15) == 0x28);
static_assert(offsetof(FiberCtx, rsp) == 0x30);
static_assert(offsetof(FiberCtx, rip) == 0x38);
static_assert(offsetof(FiberCtx, mxcsr) == 0x40);
static_assert(offsetof(FiberCtx, fcw) == 0x44);
static_assert(sizeof(FiberCtx) == 0x50);
#endif

extern "C" {
void x86_fiber_switch(FiberCtx* from, const FiberCtx* to);
[[noreturn]] void x86_fiber_trampoline();
}

struct FiberState {
    uint32_t desc = 0;
    uint32_t entry = 0;
    X86Context* ctx = nullptr;
    uint8_t* base = nullptr;
    bool started = false;
    void* stack_mem = nullptr;
    size_t stack_total = 0;
    void (*test_fn)(void*) = nullptr;
    void* test_arg = nullptr;
    bool* test_caught_outer = nullptr;
    FiberState* test_return = nullptr;
    FiberCtx fctx{};
};

thread_local std::unordered_map<uint32_t, FiberState*> g_fibers;
thread_local FiberState* g_current = nullptr;

void FiberEntry(void* param) {
    FiberState* s = static_cast<FiberState*>(param);
    g_current = s;
    if (s->test_fn) {
        try {
            s->test_fn(s->test_arg);
        } catch (...) {
            if (s->test_caught_outer) *s->test_caught_outer = true;
            if (s->test_return) {
                g_current = s->test_return;
                x86_fiber_switch(&s->fctx, &s->test_return->fctx);
            }
        }
        return;
    }
    LOGI("fiber %08X starts at %08X (esp %08X)", s->desc, s->entry, s->ctx->esp);
    try {
        if (X86Func* f = dispatch::Find(s->entry)) f(*s->ctx, s->base);
        x86_trap(*s->ctx, s->entry, "guest fiber function returned");
    } catch (const X86Resume& r) {
        LOGE("fiber %08X uncaught X86Resume target=%08X esp=%08X", s->desc, r.target, r.esp);
        x86_trap(*s->ctx, s->entry, "uncaught X86Resume on guest fiber");
    } catch (...) {
        LOGE("fiber %08X uncaught C++ exception", s->desc);
        x86_trap(*s->ctx, s->entry, "uncaught C++ exception on guest fiber");
    }
}

bool InitFiberStack(FiberState* s, void (*entry)(void*), void* arg) {
    size_t page = size_t(sysconf(_SC_PAGESIZE));
    size_t guard = std::max(kGuardSize, page);
    size_t total = guard + kFiberStack;
    void* mem = mmap(nullptr, total, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) return false;
    if (mprotect(static_cast<uint8_t*>(mem) + guard, kFiberStack, PROT_READ | PROT_WRITE) != 0) {
        munmap(mem, total);
        return false;
    }
    s->stack_mem = mem;
    s->stack_total = total;
    uintptr_t top = (reinterpret_cast<uintptr_t>(mem) + total) & ~uintptr_t(15);
    // Reserve 32 zeroed bytes at the top of the stack as a null terminator frame (fp=0, lr=0).
    top -= 32;
    std::memset(reinterpret_cast<void*>(top), 0, 32);
    std::memset(&s->fctx, 0, sizeof(s->fctx));
#if defined(__aarch64__)
    s->fctx.sp = static_cast<uint64_t>(top);
    s->fctx.x29 = 0;
    s->fctx.x30 = reinterpret_cast<uint64_t>(&x86_fiber_trampoline);
    s->fctx.x19 = reinterpret_cast<uint64_t>(entry);
    s->fctx.x20 = reinterpret_cast<uint64_t>(arg);
#elif defined(__x86_64__)
    s->fctx.rsp = static_cast<uint64_t>(top);
    s->fctx.rbp = 0;
    s->fctx.rip = reinterpret_cast<uint64_t>(&x86_fiber_trampoline);
    s->fctx.r12 = reinterpret_cast<uint64_t>(entry);
    s->fctx.r13 = reinterpret_cast<uint64_t>(arg);
    s->fctx.mxcsr = 0x1F80;
    s->fctx.fcw = 0x037F;
#endif
    return true;
}

void DestroyFiber(FiberState* s) {
    if (!s) return;
    if (s->stack_mem) munmap(s->stack_mem, s->stack_total);
    delete s;
}

void EnsureCurrent(X86Context& ctx, uint8_t* base) {
    if (g_current) return;
    FiberState* s = new FiberState;
    s->desc = ctx.ecx;
    s->ctx = &ctx;
    s->base = base;
    s->started = true;
    if (s->desc) g_fibers[s->desc] = s;
    g_current = s;
    LOGI("fiber %08X is the host thread", s->desc);
}

FiberState* EnsureFiber(uint32_t desc, X86Context& ctx, uint8_t* base) {
    auto it = g_fibers.find(desc);
    if (it != g_fibers.end()) return it->second;
    if (!desc) {
        LOGE("fiber: switch to descriptor 0 from %08X", g_current ? g_current->desc : 0);
        x86_trap(ctx, 0, "switch to a null fiber descriptor");
    }
    FiberState* s = new FiberState;
    s->desc = desc;
    s->ctx = &ctx;
    s->base = base;
    if (!InitFiberStack(s, FiberEntry, s)) {
        delete s;
        LOGE("fiber: InitFiberStack failed");
        x86_trap(ctx, 0, "InitFiberStack failed");
    }
    g_fibers[desc] = s;
    LOGI("fiber %08X created", desc);
    return s;
}

#endif  // _WIN32

}  // namespace

void x86_pivot_ret(X86Context& ctx, uint8_t* base, int n) {
    EnsureCurrent(ctx, base);
    // the switch function has already loaded the target stack; finish its `ret n`
    const uint32_t target = X86_MEM32(ctx.esp);
    ctx.esp += uint32_t(4 + n);
    FiberState* self = g_current;
    FiberState* dst = EnsureFiber(ctx.eax, ctx, base);
    if (dst == self) return;
    dst->ctx = &ctx;
    dst->base = base;
    if (!dst->started) {
        // a brand new fiber starts at the return address its initial frame was built with (the trampoline)
        dst->entry = target;
        dst->started = true;
    }
    g_current = dst;
#ifdef _WIN32
    SwitchToFiber(dst->handle);
#else
    x86_fiber_switch(&self->fctx, &dst->fctx);
#endif
    // another fiber switched back: g_current is self again and ctx already holds this fiber's restored state
}

namespace fibers {

#ifndef _WIN32
namespace {

__attribute__((noinline)) void ThrowResumeHelper(uint32_t target, uint32_t esp) {
    throw X86Resume{target, esp};
}

__attribute__((noinline)) void NestedThrowFrame(uint32_t target, uint32_t esp) {
    ThrowResumeHelper(target, esp);
}

struct FiberTestArgs {
    FiberState* main_fiber = nullptr;
    FiberState* self_fiber = nullptr;
    FiberState* next_fiber = nullptr;
    bool throw_uncaught = false;
    bool ok = false;
};

__attribute__((noinline)) void TestFiberARunner(void* p) {
    auto* a = static_cast<FiberTestArgs*>(p);
    bool pre_ok = false;
    bool post_ok = false;
    try {
        NestedThrowFrame(0x12345678u, 0xABCD0000u);
    } catch (const X86Resume& r) {
        pre_ok = (r.target == 0x12345678u && r.esp == 0xABCD0000u);
    }
    // Switch from Fiber A -> Fiber B, then resume back into Fiber A and throw again.
    g_current = a->next_fiber;
    x86_fiber_switch(&a->self_fiber->fctx, &a->next_fiber->fctx);
    g_current = a->self_fiber;

    try {
        NestedThrowFrame(0x22334455u, 0xCAFEBABEu);
    } catch (const X86Resume& r) {
        post_ok = (r.target == 0x22334455u && r.esp == 0xCAFEBABEu);
    }
    a->ok = pre_ok && post_ok;
    g_current = a->main_fiber;
    x86_fiber_switch(&a->self_fiber->fctx, &a->main_fiber->fctx);
}

__attribute__((noinline)) void TestFiberBRunner(void* p) {
    auto* b = static_cast<FiberTestArgs*>(p);
    bool ok = false;
    try {
        NestedThrowFrame(0x87654321u, 0xDEAD0004u);
    } catch (const X86Resume& r) {
        ok = (r.target == 0x87654321u && r.esp == 0xDEAD0004u);
    }
    b->ok = ok;
    g_current = b->next_fiber;
    x86_fiber_switch(&b->self_fiber->fctx, &b->next_fiber->fctx);
}

__attribute__((noinline)) void TestFiberCRunner(void* p) {
    auto* c = static_cast<FiberTestArgs*>(p);
    if (c->throw_uncaught) {
        NestedThrowFrame(0x55667788u, 0xBEEF0008u);
    }
}

}  // namespace

bool RunSelfTest() {
    FiberState main_fiber{};
    main_fiber.started = true;
    FiberState* prev_current = g_current;
    g_current = &main_fiber;

    auto* fiber_a = new FiberState;
    auto* fiber_b = new FiberState;
    auto* fiber_c = new FiberState;

    FiberTestArgs args_a{};
    args_a.main_fiber = &main_fiber;
    args_a.self_fiber = fiber_a;
    args_a.next_fiber = fiber_b;

    FiberTestArgs args_b{};
    args_b.main_fiber = &main_fiber;
    args_b.self_fiber = fiber_b;
    args_b.next_fiber = fiber_a;

    bool caught_outer = false;
    FiberTestArgs args_c{};
    args_c.main_fiber = &main_fiber;
    args_c.self_fiber = fiber_c;
    args_c.throw_uncaught = true;

    fiber_a->test_fn = TestFiberARunner;
    fiber_a->test_arg = &args_a;
    fiber_b->test_fn = TestFiberBRunner;
    fiber_b->test_arg = &args_b;
    fiber_c->test_fn = TestFiberCRunner;
    fiber_c->test_arg = &args_c;
    fiber_c->test_caught_outer = &caught_outer;
    fiber_c->test_return = &main_fiber;

    bool init_ok = InitFiberStack(fiber_a, FiberEntry, fiber_a) && InitFiberStack(fiber_b, FiberEntry, fiber_b) &&
                   InitFiberStack(fiber_c, FiberEntry, fiber_c);
    if (!init_ok) {
        DestroyFiber(fiber_a);
        DestroyFiber(fiber_b);
        DestroyFiber(fiber_c);
        g_current = prev_current;
        return false;
    }

    // Test 1 & 2: Main -> Fiber A -> Fiber B -> Fiber A -> Main with nested throws on both stacks.
    g_current = fiber_a;
    x86_fiber_switch(&main_fiber.fctx, &fiber_a->fctx);

    // Test 3: Throw across multiple frames all the way to FiberEntry's outer catch(...) boundary.
    g_current = fiber_c;
    x86_fiber_switch(&main_fiber.fctx, &fiber_c->fctx);

    g_current = prev_current;
    DestroyFiber(fiber_a);
    DestroyFiber(fiber_b);
    DestroyFiber(fiber_c);
    return args_a.ok && args_b.ok && caught_outer;
}
#else
bool RunSelfTest() { return true; }
#endif

}  // namespace fibers
