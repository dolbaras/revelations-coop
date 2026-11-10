/*
 * The detour stubs of native/rolebarrier1.c executed as 32-bit code (the game is a 32-bit process): this test is compiled for an x86 target. A fake "engine function" is built for
 * each hook from the REAL first bytes of the function (the bytes the hook steals) followed by a small body written by hand; the hook is applied with rbk_apply on live memory and the
 * function is called with the engine's calling convention (thiscall, callee pops the arguments). Checked: the callback sees the registers and the stack arguments, can change an
 * argument in place, can make the function return early (ret 4 / ret 0xC, stack balanced, callee-saved registers untouched, original body NOT run), an observer cannot make the function
 * return early, the original runs with all its registers when the callback lets it, and the POST filter hands the callback `this` and the result and returns what the callback says.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

#include "../native/rolebarrier1.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static int m_read(void *u, uint32_t a, void *out, uint32_t n) { (void)u; memcpy(out, (void *)(uintptr_t)a, n); return 1; }
static int m_write(void *u, uint32_t a, const void *p, uint32_t n) { (void)u; memcpy((void *)(uintptr_t)a, p, n); return 1; }
static uint32_t m_alloc(void *u, uint32_t n) { (void)u; return (uint32_t)(uintptr_t)VirtualAlloc(NULL, n, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE); }
static void m_log(void *u, const char *l) { (void)u; (void)l; }
static const rbk_mem MEM = {m_read, m_write, m_alloc, m_log, NULL};

static volatile uint32_t marker;   /* the fake original bodies set it: "the original ran" */
static rbk_frame seen;             /* what the last PRE callback saw */
static uint32_t seen_this, seen_result;
static int cb_mode;                /* what the callback does: 0 let go, 1 hold, 2 change args and let go, 3 hold + change */
static uint32_t post_ret;

static uint32_t __attribute__((force_align_arg_pointer)) pre_cb(rbk_frame *f)
{
    seen = *f;
    if (cb_mode == 2 || cb_mode == 3) {
        f->arg[0] += 1000, f->arg[1] = 7, f->arg[2] *= 2;
    }
    return cb_mode == 1 || cb_mode == 3;
}
static rb_t FB;                    /* a filter of the real module, without a session */
static uint32_t raw_unhooked_expect;
static int fb_session(void *u, rb_sinfo *o) { (void)u; (void)o; return 0; }
static uint32_t fb_now(void *u) { (void)u; return 0; }
static uint32_t __attribute__((force_align_arg_pointer)) post_cb(uint32_t self, uint32_t result)
{
    seen_this = self, seen_result = result;
    if (post_ret == 0xFFFFFFFEu) return rb_pcs_filter(&FB, result);
    return post_ret == 0xFFFFFFFFu ? result : post_ret;
}

/* harnesses: set ebx/ebp/esi/edi to sentinels, call fn(thiscall; ecx = self, n stack args, callee pops), return eax, report the four registers afterwards */
__attribute__((naked)) static uint32_t call1(void *fn, void *self, uint32_t a0, uint32_t out[4])
{
    __asm__ volatile(
        "pushl %ebx\n pushl %ebp\n pushl %esi\n pushl %edi\n"
        "movl 20(%esp), %eax\n movl 24(%esp), %ecx\n movl 28(%esp), %edx\n"
        "pushl %edx\n"
        "movl $0x11111111, %ebx\n movl $0x22222222, %ebp\n movl $0x33333333, %esi\n movl $0x44444444, %edi\n"
        "call *%eax\n"
        "movl 32(%esp), %edx\n"
        "movl %ebx, 0(%edx)\n movl %ebp, 4(%edx)\n movl %esi, 8(%edx)\n movl %edi, 12(%edx)\n"
        "popl %edi\n popl %esi\n popl %ebp\n popl %ebx\n ret\n");
}
__attribute__((naked)) static uint32_t call2(void *fn, void *self, uint32_t a0, uint32_t a1, uint32_t out[4])
{
    __asm__ volatile(
        "pushl %ebx\n pushl %ebp\n pushl %esi\n pushl %edi\n"
        "movl 20(%esp), %eax\n movl 24(%esp), %ecx\n"
        "pushl 32(%esp)\n pushl 32(%esp)\n"   /* a1 then a0: after the first push a0 sits at 32 again... */
        "movl $0x11111111, %ebx\n movl $0x22222222, %ebp\n movl $0x33333333, %esi\n movl $0x44444444, %edi\n"
        "call *%eax\n"
        "movl 36(%esp), %edx\n"
        "movl %ebx, 0(%edx)\n movl %ebp, 4(%edx)\n movl %esi, 8(%edx)\n movl %edi, 12(%edx)\n"
        "popl %edi\n popl %esi\n popl %ebp\n popl %ebx\n ret\n");
}
__attribute__((naked)) static uint32_t call3(void *fn, void *self, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t out[4])
{
    __asm__ volatile(
        "pushl %ebx\n pushl %ebp\n pushl %esi\n pushl %edi\n"
        "movl 20(%esp), %eax\n movl 24(%esp), %ecx\n"
        "pushl 36(%esp)\n pushl 36(%esp)\n pushl 36(%esp)\n"   /* a2, a1, a0 (each push moves the next one back to the same offset) */
        "movl $0x11111111, %ebx\n movl $0x22222222, %ebp\n movl $0x33333333, %esi\n movl $0x44444444, %edi\n"
        "call *%eax\n"
        "movl 40(%esp), %edx\n"
        "movl %ebx, 0(%edx)\n movl %ebp, 4(%edx)\n movl %esi, 8(%edx)\n movl %edi, 12(%edx)\n"
        "popl %edi\n popl %esi\n popl %ebp\n popl %ebx\n ret\n");
}
/* the POST harness: a function without stack arguments, ecx = self, edx = 0x55555555; out[4] / out[5] = ecx / edx after the call (the real getter leaves both alone) */
__attribute__((naked)) static uint32_t call0(void *fn, void *self, uint32_t out[6])
{
    __asm__ volatile(
        "pushl %ebx\n pushl %ebp\n pushl %esi\n pushl %edi\n"
        "movl 20(%esp), %eax\n movl 24(%esp), %ecx\n"
        "movl $0x11111111, %ebx\n movl $0x22222222, %ebp\n movl $0x33333333, %esi\n movl $0x44444444, %edi\n movl $0x55555555, %edx\n"
        "call *%eax\n"
        "pushl %edx\n pushl %ecx\n"
        "movl 36(%esp), %ecx\n"                                   /* out (28 + the two pushes) */
        "movl %ebx, 0(%ecx)\n movl %ebp, 4(%ecx)\n movl %esi, 8(%ecx)\n movl %edi, 12(%ecx)\n"
        "popl %edx\n movl %edx, 16(%ecx)\n"                        /* ecx after the call */
        "popl %edx\n movl %edx, 20(%ecx)\n"                        /* edx after the call */
        "popl %edi\n popl %esi\n popl %ebp\n popl %ebx\n ret\n");
}

static uint8_t *emit_fn(const rbk_def *d, const uint8_t *body, size_t n)
{
    uint8_t *p = (uint8_t *)VirtualAlloc(NULL, 256, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    memset(p, 0xCC, 256);
    memcpy(p, d->sig, (size_t)d->steal);
    memcpy(p + d->steal, body, n);
    return p;
}
static void put_marker(uint8_t *p) { p[0] = 0xC7, p[1] = 0x05; uint32_t a = (uint32_t)(uintptr_t)&marker; memcpy(p + 2, &a, 4); p[6] = 1, p[7] = p[8] = p[9] = 0; }   /* mov [marker],1 */

static rbk_def hooked(const rbk_def *proto, uint8_t *fn, rbk_state *st, uint32_t cb)
{
    rbk_def d = *proto;
    d.va = (uint32_t)(uintptr_t)fn;
    memset(st, 0, sizeof *st);
    CHECK(rbk_apply(st, &d, &MEM, cb) == 1);
    return d;
}

int main(void)
{
    uint32_t regs[6];
    CHECK(sizeof(void *) == 4);

    /* ---- restart: thiscall(this, area, a, b), ret 0Ch; the body returns a0 + 2*a1 + 4*a2 + this and sets the marker */
    {
        const rbk_def *proto = &rbk_defs[RBK_RESTART];
        uint8_t body[64], *b = body;
        static const uint8_t t1[] = {0x8B, 0x44, 0x24, 0x0C, 0x8B, 0x54, 0x24, 0x10, 0x8D, 0x04, 0x50, 0x8B, 0x54, 0x24, 0x14, 0x8D, 0x04, 0x90, 0x03, 0xC6};
        memcpy(b, t1, sizeof t1), b += sizeof t1;
        put_marker(b), b += 10;
        *b++ = 0x5E, *b++ = 0x59, *b++ = 0xC2, *b++ = 0x0C, *b++ = 0x00;
        uint8_t *fn = emit_fn(proto, body, (size_t)(b - body));
        typedef uint32_t(__attribute__((thiscall)) * f_t)(void *, uint32_t, uint32_t, uint32_t);
        f_t raw = (f_t)(void *)fn;
        CHECK(raw((void *)0x100, 1, 2, 3) == 1 + 4 + 12 + 0x100);   /* the fake is right before any hook */

        rbk_state st;
        rbk_def d = hooked(proto, fn, &st, (uint32_t)(uintptr_t)pre_cb);
        /* let go */
        cb_mode = 0, marker = 0;
        uint32_t r = call3(fn, (void *)0x100, 1, 2, 3, regs);
        CHECK(r == 1 + 4 + 12 + 0x100 && marker == 1);
        CHECK(regs[0] == 0x11111111 && regs[1] == 0x22222222 && regs[2] == 0x33333333 && regs[3] == 0x44444444);
        CHECK(seen.ecx == 0x100 && seen.arg[0] == 1 && seen.arg[1] == 2 && seen.arg[2] == 3 && seen.ret_addr != 0);
        CHECK(seen.ebx == 0x11111111 && seen.ebp == 0x22222222 && seen.esi == 0x33333333 && seen.edi == 0x44444444);
        /* change the arguments in place */
        cb_mode = 2, marker = 0;
        r = call3(fn, (void *)0x100, 1, 2, 3, regs);
        CHECK(r == 1001 + 14 + 24 + 0x100 && marker == 1);
        CHECK(regs[0] == 0x11111111 && regs[1] == 0x22222222 && regs[2] == 0x33333333 && regs[3] == 0x44444444);
        /* early return: the body does not run, eax = 0, the stack is balanced (the harness would crash otherwise), registers kept */
        cb_mode = 1, marker = 0;
        r = call3(fn, (void *)0x100, 1, 2, 3, regs);
        CHECK(r == 0 && marker == 0);
        CHECK(regs[0] == 0x11111111 && regs[1] == 0x22222222 && regs[2] == 0x33333333 && regs[3] == 0x44444444);
        cb_mode = 3, marker = 0;
        r = call3(fn, (void *)0x100, 5, 5, 5, regs);
        CHECK(r == 0 && marker == 0);
        /* the same through the stock calling convention */
        cb_mode = 0, marker = 0;
        CHECK(((f_t)(void *)fn)((void *)0x40, 10, 20, 30) == 10 + 40 + 120 + 0x40 && marker == 1);
        /* taken out again: the plain function */
        CHECK(rbk_revert(&st, &d, &MEM) == 1);
        cb_mode = 1, marker = 0;
        CHECK(raw((void *)0x100, 1, 2, 3) == 1 + 4 + 12 + 0x100);
        CHECK(marker == 1);
    }

    /* ---- section end: thiscall(this, ctx), ret 4; the body returns ctx and sets the marker */
    {
        const rbk_def *proto = &rbk_defs[RBK_EPEND];
        uint8_t body[32], *b = body;
        *b++ = 0x89, *b++ = 0xE8;                     /* mov eax,ebp (= the argument: the real function loads ebp from [esp+0Ch]) */
        put_marker(b), b += 10;
        *b++ = 0x5D, *b++ = 0x5B, *b++ = 0xC2, *b++ = 0x04, *b++ = 0x00;   /* pop ebp / pop ebx / ret 4 */
        uint8_t *fn = emit_fn(proto, body, (size_t)(b - body));
        rbk_state st;
        rbk_def d = hooked(proto, fn, &st, (uint32_t)(uintptr_t)pre_cb);
        cb_mode = 0, marker = 0;
        uint32_t r = call1(fn, (void *)0x55, 0x9999, regs);
        CHECK(r == 0x9999 && marker == 1 && seen.arg[0] == 0x9999 && seen.ecx == 0x55);
        CHECK(regs[0] == 0x11111111 && regs[1] == 0x22222222 && regs[2] == 0x33333333 && regs[3] == 0x44444444);
        cb_mode = 1, marker = 0;
        r = call1(fn, (void *)0x55, 0x9999, regs);
        CHECK(r == 0 && marker == 0);                                   /* held: ret 4 popped the argument, the body never ran */
        CHECK(regs[0] == 0x11111111 && regs[1] == 0x22222222 && regs[2] == 0x33333333 && regs[3] == 0x44444444);
        for (int i = 0; i < 5; i++) {                                   /* many frames in a row */
            cb_mode = i == 4 ? 0 : 1, marker = 0;
            r = call1(fn, (void *)0x55, 0x9999 + (uint32_t)i, regs);
            CHECK(i == 4 ? (r == 0x9999 + 4 && marker == 1) : (r == 0 && marker == 0));
        }
        CHECK(rbk_revert(&st, &d, &MEM) == 1);
    }

    /* ---- observers (cutscene / movie start): the callback may ask for a hold, nothing happens */
    {
        for (int which = 0; which < 2; which++) {
            const rbk_def *proto = which == 0 ? &rbk_defs[RBK_PCS] : &rbk_defs[RBK_MOVIE];
            uint8_t body[32], *b = body;
            *b++ = 0x89, *b++ = 0xC8;                                   /* mov eax,ecx */
            put_marker(b), b += 10;
            if (which == 0) { *b++ = 0x5F; *b++ = 0x83, *b++ = 0xC4, *b++ = 0x08; }   /* pop edi / add esp,8 (the real prologue pushed edi and took 8 bytes) */
            else { *b++ = 0x5B; *b++ = 0x83, *b++ = 0xC4, *b++ = 0x54; }   /* the real prologue: sub esp,54h / push ebx / xor ebx,ebx: pop ebx / add esp,54h */
            *b++ = 0xC2, *b++ = 0x08, *b++ = 0x00;                       /* ret 8 (two stack arguments in the harness; the real functions take 2 and 5, the stub does not care) */
            uint8_t *fn = emit_fn(proto, body, (size_t)(b - body));
            rbk_state st;
            rbk_def d = hooked(proto, fn, &st, (uint32_t)(uintptr_t)pre_cb);
            cb_mode = 1, marker = 0;
            uint32_t r = call2(fn, (void *)0x1234, 7, 8, regs);
            CHECK(r == 0x1234 && marker == 1);                          /* the hold request is ignored: the function ran */
            CHECK(seen.ecx == 0x1234 && seen.arg[0] == 7 && seen.arg[1] == 8);
            CHECK(regs[1] == 0x22222222 && regs[2] == 0x33333333 && regs[0] == 0x11111111 + (which == 0 ? 0u : 0u));
            (void)d;
        }
    }

    /* ---- the POST filter on the real getter code: 8B 41 28 83 F8 10 73 08 8A 84 08 EC 01 00 00 C3 B0 01 C3 */
    {
        const rbk_def *proto = &rbk_defs[RBK_PCSEND];
        static const uint8_t tail[] = {0x73, 0x08, 0x8A, 0x84, 0x08, 0xEC, 0x01, 0x00, 0x00, 0xC3, 0xB0, 0x01, 0xC3};
        uint8_t *fn = emit_fn(proto, tail, sizeof tail);
        static uint8_t obj[0x300];
        memset(obj, 0, sizeof obj);
        typedef uint32_t(__attribute__((thiscall)) * g_t)(void *);
        g_t raw = (g_t)(void *)fn;
        *(uint32_t *)(obj + 0x28) = 3, obj[0x1EC + 3] = 1;
        CHECK((raw(obj) & 0xFF) == 1);
        obj[0x1EC + 3] = 0;
        CHECK((raw(obj) & 0xFF) == 0);
        *(uint32_t *)(obj + 0x28) = 0xFFFFFFFFu;
        raw_unhooked_expect = raw(obj);                                  /* the stock answer without a handle */
        *(uint32_t *)(obj + 0x28) = 3;
        {
            rb_io io;
            memset(&io, 0, sizeof io);
            io.now = fb_now, io.session = fb_session;
            rb_init(&FB, &io, NULL);
            FB.enabled = 1;
        }
        rbk_state st;
        rbk_def d = hooked(proto, fn, &st, (uint32_t)(uintptr_t)post_cb);
        post_ret = 0xFFFFFFFFu;                                         /* hand the result on */
        obj[0x1EC + 3] = 1;
        uint32_t r = call0(fn, obj, regs);
        CHECK((r & 0xFF) == 1 && seen_this == (uint32_t)(uintptr_t)obj && seen_result == 1);
        CHECK(regs[0] == 0x11111111 && regs[1] == 0x22222222 && regs[2] == 0x33333333 && regs[3] == 0x44444444);
        obj[0x1EC + 3] = 0;
        r = call0(fn, obj, regs);
        CHECK((r & 0xFF) == 0 && seen_result == 0);
        post_ret = 0;                                                    /* the filter says "not ended" although it ended */
        obj[0x1EC + 3] = 1;
        r = call0(fn, obj, regs);
        CHECK((r & 0xFF) == 0 && seen_result == 1);
        post_ret = 1;                                                    /* ... or the other way round */
        obj[0x1EC + 3] = 0;
        r = call0(fn, obj, regs);
        CHECK((r & 0xFF) == 1 && seen_result == 0);
        *(uint32_t *)(obj + 0x28) = 0x20;                                /* slot out of range: the function answers 1 itself */
        post_ret = 0xFFFFFFFFu;
        r = call0(fn, obj, regs);
        CHECK(r == 1 && seen_result == 1);
        CHECK(regs[0] == 0x11111111 && regs[1] == 0x22222222 && regs[2] == 0x33333333 && regs[3] == 0x44444444);
        /* no handle (-1): the stock getter answers 0xFFFFFF01; the hooked one must hand on exactly that (the first version gave 1), and keep ecx / edx */
        *(uint32_t *)(obj + 0x28) = 0xFFFFFFFFu;
        CHECK(raw_unhooked_expect == 0xFFFFFF01u);
        r = call0(fn, obj, regs);
        CHECK(r == 0xFFFFFF01u && seen_result == 0xFFFFFF01u);
        CHECK(regs[4] == (uint32_t)(uintptr_t)obj && regs[5] == 0x55555555u);
        CHECK(regs[0] == 0x11111111 && regs[1] == 0x22222222 && regs[2] == 0x33333333 && regs[3] == 0x44444444);
        /* the real filter of rolebarrier1.c without a session: every answer bit for bit */
        post_ret = 0xFFFFFFFEu;   /* = use rb_pcs_filter */
        rb_pcs_begin(&FB, 0x1234, 1);
        for (uint32_t slot = 0; slot < 20; slot++) {
            for (int flag = 0; flag < 2; flag++) {
                static const uint32_t bigh[4] = {0xFFFFFFFFu, 0x00000110u, 0x12345678u, 0xFFFFFF10u};   /* handles >= 16: the getter answers (handle & ~0xFF) | 1 */
                uint32_t h = slot < 16 ? slot : bigh[slot - 16];
                *(uint32_t *)(obj + 0x28) = h;
                if (slot < 16) obj[0x1EC + slot] = (uint8_t)flag;
                r = call0(fn, obj, regs);
                uint32_t want = slot < 16 ? (uint32_t)flag : (h & ~0xFFu) | 1u;
                CHECK(r == want && regs[4] == (uint32_t)(uintptr_t)obj && regs[5] == 0x55555555u);
            }
        }
        CHECK(FB.pcs_changed == 0);
        post_ret = 0xFFFFFFFFu;
        CHECK(rbk_revert(&st, &d, &MEM) == 1);
        *(uint32_t *)(obj + 0x28) = 3, obj[0x1EC + 3] = 1;
        CHECK((raw(obj) & 0xFF) == 1);
    }

    printf("test_rolebarrier1_x86: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
