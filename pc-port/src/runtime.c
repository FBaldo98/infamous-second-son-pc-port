// SPDX-License-Identifier: GPL-2.0-or-later
#define _CRT_RAND_S
#include "runtime.h"
#include <Zydis/Zydis.h>
#include <intrin.h>
#include <malloc.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "api_names.h"

enum { MAX_MODULES = 8 };
typedef struct {
    void *allocations[MAX_MODULES];
    unsigned char *blocks[MAX_MODULES];
    uint64_t dtv[MAX_MODULES + 2];
    uint64_t *tcb;
} GuestTls;
static GuestModule layout[MAX_MODULES];
static uint64_t module_count, source_size, process_param, canary;
static unsigned char *source_image;
static DWORD tls_slot = TLS_OUT_OF_INDEXES;
static volatile LONG64 tls_faults;
static void **application_heap_api;
static _Thread_local int32_t guest_errno;
int32_t *runtime_errno(void) { return &guest_errno; }
// Layout/contract checked against shadPS4 libc_internal_memory.cpp (GPL-2.0+).
typedef struct { uint64_t size; uint32_t flags, segment_info; uint64_t *mask, *table; } HeapTraceInfo;
static uint64_t heap_trace_mask, heap_trace_table[64];

static void stop(const char *reason) {
    fprintf(stderr, "RUNTIME_ERROR: %s\n", reason);
    fflush(stderr);
    ExitProcess(23);
}
static uint64_t aligned(uint64_t value, uint64_t align) { return (value + align - 1) & ~(align - 1); }

void runtime_configure(unsigned char *image, uint64_t image_size, const GuestModule *modules,
                       uint64_t count, uint64_t procparam) {
    if (!count || count > MAX_MODULES || procparam >= image_size) stop("bad runtime metadata");
    if (tls_slot != TLS_OUT_OF_INDEXES) stop("runtime already configured");
    source_image = image; source_size = image_size; module_count = count;
    process_param = (uintptr_t)image + procparam;
    memcpy(layout, modules, (size_t)count * sizeof(*modules));
    for (uint64_t i = 0; i < count; ++i) {
        GuestModule *m = &layout[i];
        if (m->tls_id != i + 1 || m->tls_filesz > m->tls_memsz || m->tls_memsz > 1024 * 1024 ||
            !m->tls_align || m->tls_align > 4096 || (m->tls_align & (m->tls_align - 1)) ||
            m->tls_address > image_size || m->tls_filesz > image_size - m->tls_address)
            stop("bad TLS module metadata");
    }
    unsigned int halves[2];
    if (rand_s(&halves[0]) || rand_s(&halves[1])) stop("cannot generate stack canary");
    memcpy(&canary, halves, sizeof(canary));
    canary &= ~UINT64_C(255);
    tls_slot = TlsAlloc();
    if (tls_slot == TLS_OUT_OF_INDEXES) stop("cannot allocate host TLS slot");
}

void runtime_tls_attach(void) {
    if (tls_slot == TLS_OUT_OF_INDEXES) stop("TLS is not configured");
    if (TlsGetValue(tls_slot)) return;
    GuestTls *state = calloc(1, sizeof(*state));
    if (!state) stop("cannot allocate per-thread TLS state");
    for (uint64_t i = 0; i < module_count; ++i) {
        const GuestModule *m = &layout[i];
        uint64_t static_size = aligned(m->tls_memsz, m->tls_align);
        size_t bytes = (size_t)(static_size + (i == 0 ? 64 : 0));
        size_t alignment = (size_t)(m->tls_align < 64 ? 64 : m->tls_align);
        unsigned char *block = _aligned_malloc(bytes ? bytes : 1, alignment);
        if (!block) stop("cannot allocate per-module TLS block");
        memset(block, 0, bytes ? bytes : 1);
        if (m->tls_filesz) memcpy(block, source_image + m->tls_address, (size_t)m->tls_filesz);
        state->allocations[i] = block;
        state->blocks[i] = block;
        state->dtv[i + 2] = (uintptr_t)block;
        if (i == 0) state->tcb = (uint64_t *)(block + static_size);
    }
    state->dtv[0] = 1; state->dtv[1] = module_count;
    state->tcb[0] = (uintptr_t)state->tcb;
    state->tcb[1] = (uintptr_t)state->dtv;
    state->tcb[2] = (uintptr_t)state; // Thread handle services remain unimplemented.
    state->tcb[5] = canary;
    if (!TlsSetValue(tls_slot, state)) stop("cannot attach guest TLS");
}

void runtime_tls_detach(void) {
    if (tls_slot == TLS_OUT_OF_INDEXES) return;
    GuestTls *state = TlsGetValue(tls_slot);
    if (!state) return;
    TlsSetValue(tls_slot, NULL);
    for (uint64_t i = 0; i < module_count; ++i) _aligned_free(state->allocations[i]);
    free(state);
}
static GuestTls *current_tls(void) {
    runtime_tls_attach();
    return TlsGetValue(tls_slot);
}
void runtime_set_thread_handle(void *handle) { current_tls()->tcb[2] = (uintptr_t)handle; }
const char *runtime_api_name(const char *identity) {
    for (size_t i = 0; i < sizeof(api_names) / sizeof(*api_names); ++i)
        if (!strcmp(identity, api_names[i].identity)) return api_names[i].api;
    return "";
}
static void *GUEST_ABI guest_tls_get_addr(const uint64_t *index) {
    if (!index || !index[0] || index[0] > module_count || index[1] >= layout[index[0] - 1].tls_memsz)
        stop("invalid dynamic TLS module or offset");
    return current_tls()->blocks[index[0] - 1] + index[1];
}
static void *GUEST_ABI guest_procparam(void) { return (void *)(uintptr_t)process_param; }
static void GUEST_ABI guest_set_heap_api(void **api) {
    if (!api) stop("null application heap API");
    application_heap_api = api;
    printf("Registered native application heap API at 0x%llx\n", (unsigned long long)(uintptr_t)application_heap_api);
}
static void GUEST_ABI guest_heap_trace_info(HeapTraceInfo *info) {
    if (!info || info->size < sizeof(*info)) stop("invalid heap trace info size");
    info->segment_info = 0;
    info->mask = &heap_trace_mask;
    info->table = heap_trace_table;
}
static void *GUEST_ABI guest_memset(void *p,int value,uint64_t n){return memset(p,value,(size_t)n);}
static void *GUEST_ABI guest_memcpy(void *p,const void *s,uint64_t n){return memcpy(p,s,(size_t)n);}
static void *GUEST_ABI guest_memmove(void *p,const void *s,uint64_t n){return memmove(p,s,(size_t)n);}
static int GUEST_ABI guest_memcmp(const void *p,const void *s,uint64_t n){return memcmp(p,s,(size_t)n);}
static uint64_t GUEST_ABI guest_strlen(const char *p){return strlen(p);}
static int GUEST_ABI guest_strcmp(const char *p,const char *s){return strcmp(p,s);}
static int GUEST_ABI guest_strncmp(const char *p,const char *s,uint64_t n){return strncmp(p,s,(size_t)n);}
static int GUEST_ABI guest_neo_mode(void){const char *mode=getenv("ISS_NEO_MODE");return mode&&atoi(mode)==1;}

uintptr_t runtime_resolve(const char *identity, int data) {
    if (data) {
        if (!strcmp(identity, "f7uOxY9mM1U#libkernel#1#libkernel#257")) return (uintptr_t)&canary;
        return 0;
    }
    if(strstr(identity,"#libSceLibcInternal#1#libSceLibcInternal#257")){
        const char *api=runtime_api_name(identity);
        if(!strcmp(api,"memset"))return (uintptr_t)guest_memset;
        if(!strcmp(api,"memcpy"))return (uintptr_t)guest_memcpy;
        if(!strcmp(api,"memmove"))return (uintptr_t)guest_memmove;
        if(!strcmp(api,"memcmp"))return (uintptr_t)guest_memcmp;
        if(!strcmp(api,"strlen"))return (uintptr_t)guest_strlen;
        if(!strcmp(api,"strcmp"))return (uintptr_t)guest_strcmp;
        if(!strcmp(api,"strncmp"))return (uintptr_t)guest_strncmp;
    }
    if (!strcmp(identity, "vNe1w4diLCs#libkernel#1#libkernel#257")) return (uintptr_t)guest_tls_get_addr;
    if (!strcmp(runtime_api_name(identity), "__error") || !strcmp(runtime_api_name(identity), "__errno_location")) return (uintptr_t)runtime_errno;
    if (!strcmp(identity, "959qrazPIrg#libkernel#1#libkernel#257")) return (uintptr_t)guest_procparam;
    if (!strcmp(runtime_api_name(identity),"sceKernelIsNeoMode")) return (uintptr_t)guest_neo_mode;
    if (!strcmp(identity, "p5EcQeEeJAE#libkernel#1#libkernel#257")) return (uintptr_t)guest_set_heap_api;
    if (!strcmp(identity, "NWtTN10cJzE#libSceLibcInternalExt#1#libSceLibcInternal#257")) return (uintptr_t)guest_heap_trace_info;
    if (!strcmp(identity, "pO96TwzOm5E#libkernel#1#libkernel#257")) return (uintptr_t)memory_direct_size;
    if (!strcmp(identity, "rTXw65xmLIA#libkernel#1#libkernel#257")) return (uintptr_t)memory_allocate_direct;
    if (!strcmp(identity, "L-Q3LEjIbgA#libkernel#1#libkernel#257")) return (uintptr_t)memory_map_direct;
    uintptr_t fn = sync_resolve(identity);
    if (!fn) fn = thread_resolve(identity);
    if (!fn) fn = time_resolve(identity);
    if (!fn) fn = file_resolve(identity);
    if (!fn) fn = service_resolve(identity);
    if (!fn) fn = dialog_resolve(identity);
    if (!fn) fn = np_resolve(identity);
    if (!fn) fn = network_resolve(identity);
    if (!fn) fn = save_resolve(identity);
    if (!fn) fn = trophy_resolve(identity);
    if (!fn) fn = pad_resolve(identity);
    if (!fn) fn = audio_resolve(identity);
    if (!fn) fn = kernel_sync_resolve(identity);
    if (!fn) fn = http_resolve(identity);
    return fn ? fn : gpu_resolve(identity);
}

static DWORD64 *context_register(CONTEXT *c, ZydisRegister r) {
    switch (r) {
    case ZYDIS_REGISTER_RAX: return &c->Rax; case ZYDIS_REGISTER_RCX: return &c->Rcx;
    case ZYDIS_REGISTER_RDX: return &c->Rdx; case ZYDIS_REGISTER_RBX: return &c->Rbx;
    case ZYDIS_REGISTER_RSP: return &c->Rsp; case ZYDIS_REGISTER_RBP: return &c->Rbp;
    case ZYDIS_REGISTER_RSI: return &c->Rsi; case ZYDIS_REGISTER_RDI: return &c->Rdi;
    case ZYDIS_REGISTER_R8: return &c->R8; case ZYDIS_REGISTER_R9: return &c->R9;
    case ZYDIS_REGISTER_R10: return &c->R10; case ZYDIS_REGISTER_R11: return &c->R11;
    case ZYDIS_REGISTER_R12: return &c->R12; case ZYDIS_REGISTER_R13: return &c->R13;
    case ZYDIS_REGISTER_R14: return &c->R14; case ZYDIS_REGISTER_R15: return &c->R15;
    default: return NULL;
    }
}
static DWORD result_flags(uint64_t result) {
    unsigned char byte = (unsigned char)result;
    unsigned ones = 0;
    for (unsigned i = 0; i < 8; ++i) ones += (byte >> i) & 1;
    return (!result ? 0x40u : 0) | ((result >> 63) ? 0x80u : 0) | (!(ones & 1) ? 4u : 0);
}

// TLS site patching (ISS_TLS_PATCH=1). Every guest read of its TCB (fs:[0], the stack canary at
// fs:[0x28]...) faults on Windows and is emulated below: an exception, a decode and a system call
// each time, ~25-30% of the game threads in Seattle. Once such an instruction has faulted, it is
// rewritten into a jump to a trampoline doing the same read through the host TLS slot of the
// guest state (gs:[0x1480 + 8 * slot] in the TEB, for slots below 64):
//   MOV r, fs:[d]  ->  mov r, gs:[slot]; mov r, [r + tcb]; mov r, [r + d]; jmp back
//   XOR/CMP r, fs:[d] -> below the 128-byte red zone, a scratch register s is saved:
//      lea rsp,[rsp-136]; push s; mov s, gs:[slot]; mov s,[s+tcb]; op r,[s+d]; pop s;
//      lea rsp,[rsp+136]; jmp back      (lea/push/pop/mov leave the flags of op intact)
// Only the exact 9-byte form (64 REX.W op modrm=04 sib=25 disp32) is rewritten, and only after
// it faulted (a real instruction start). The site is patched int3 first, then the rel32, then
// the jmp opcode: a thread meeting the int3 meanwhile is sent to the trampoline here.
enum { TLS_SLOT_BYTES = 64, TLS_CODE_OFFSET = 16, TLS_POOL_BYTES = 4 << 20, TLS_SITES = 1 << 15 };
static unsigned char *tls_pool;
static size_t tls_pool_used;
static SRWLOCK tls_patch_lock = SRWLOCK_INIT;
static struct { volatile uint64_t site; uint64_t code; } tls_sites[TLS_SITES];
static volatile LONG tls_patched;
static int tls_patch_mode = -1;

static int tls_patch_enabled(void) {
    if (tls_patch_mode < 0) {
        char value[8] = {0};
        GetEnvironmentVariableA("ISS_TLS_PATCH", value, sizeof(value));
        tls_patch_mode = value[0] == '1' && tls_slot < 64;
        fprintf(stderr, "PORT_TUNING tls_patch=%d slot=%lu\n", tls_patch_mode, (unsigned long)tls_slot);
    }
    return tls_patch_mode;
}
static uint64_t tls_site_lookup(uint64_t site) {
    for (uint64_t i = (site * UINT64_C(0x9e3779b97f4a7c15)) >> 49, n = 0; n < TLS_SITES; ++n, i = (i + 1) & (TLS_SITES - 1)) {
        const uint64_t s = tls_sites[i].site;
        if (!s) return 0;
        if (s == site) { MemoryBarrier(); return tls_sites[i].code; }
    }
    return 0;
}
static int tls_site_insert(uint64_t site, uint64_t code) {
    for (uint64_t i = (site * UINT64_C(0x9e3779b97f4a7c15)) >> 49, n = 0; n < TLS_SITES; ++n, i = (i + 1) & (TLS_SITES - 1)) {
        if (!tls_sites[i].site) { tls_sites[i].code = code; MemoryBarrier(); tls_sites[i].site = site; return 1; }
    }
    return 0;
}
static unsigned char *tls_emit32(unsigned char *p, int32_t v) { memcpy(p, &v, 4); return p + 4; }
// op r, [b + disp32] (64-bit); op: 0x8B mov, 0x33 xor, 0x3B cmp.
static unsigned char *tls_emit_mem(unsigned char *p, unsigned char op, unsigned r, unsigned b, int32_t disp) {
    *p++ = (unsigned char)(0x48 | ((r >> 3) << 2) | (b >> 3));
    *p++ = op;
    *p++ = (unsigned char)(0x80 | ((r & 7) << 3) | (b & 7));
    if ((b & 7) == 4) *p++ = 0x24;
    return tls_emit32(p, disp);
}
// mov r, gs:[slot offset]
static unsigned char *tls_emit_slot(unsigned char *p, unsigned r) {
    *p++ = 0x65; *p++ = (unsigned char)(0x48 | ((r >> 3) << 2)); *p++ = 0x8B;
    *p++ = (unsigned char)(((r & 7) << 3) | 4); *p++ = 0x25;
    return tls_emit32(p, (int32_t)(0x1480 + 8 * tls_slot));
}
static unsigned char *tls_pool_reserve(void) {
    if (!tls_pool) {
        const uint64_t start = ((uint64_t)(uintptr_t)source_image + source_size + 0xffff) & ~UINT64_C(0xffff);
        for (uint64_t a = start; a < start + (UINT64_C(1) << 30) && !tls_pool; a += 0x10000)
            tls_pool = VirtualAlloc((void *)(uintptr_t)a, TLS_POOL_BYTES, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        if (!tls_pool) return NULL;
    }
    if (tls_pool_used + TLS_SLOT_BYTES > TLS_POOL_BYTES) return NULL;
    unsigned char *slot = tls_pool + tls_pool_used;
    tls_pool_used += TLS_SLOT_BYTES;
    return slot;
}
// Decodes the generated trampoline and checks every instruction against what it must be.
typedef struct { ZydisMnemonic mnemonic; int reg; int base; int segment_gs; int64_t disp; } TlsExpected;
static int tls_verify(const unsigned char *code, const TlsExpected *expected, int count, uint64_t back) {
    ZydisDecoder decoder;
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
    uint64_t at = (uint64_t)(uintptr_t)code;
    for (int i = 0; i <= count; ++i) {
        ZydisDecodedInstruction ins;
        ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
        if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, (const void *)(uintptr_t)at, 15, &ins, ops))) return 0;
        if (i == count) { // jmp back
            ZyanU64 target;
            return ins.mnemonic == ZYDIS_MNEMONIC_JMP &&
                   ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&ins, &ops[0], at, &target)) && target == back;
        }
        const TlsExpected *x = &expected[i];
        if (ins.mnemonic != x->mnemonic) return 0;
        if (x->reg >= 0 && (ops[0].type != ZYDIS_OPERAND_TYPE_REGISTER ||
                            ops[0].reg.value != (ZydisRegister)(ZYDIS_REGISTER_RAX + x->reg))) return 0;
        if (x->base != -2) { // memory operand checked
            const ZydisDecodedOperand *m = &ops[1];
            if (m->type != ZYDIS_OPERAND_TYPE_MEMORY || m->mem.disp.value != x->disp ||
                m->mem.index != ZYDIS_REGISTER_NONE ||
                (m->size != 64 && ins.mnemonic != ZYDIS_MNEMONIC_LEA) ||
                (x->segment_gs ? m->mem.segment != ZYDIS_REGISTER_GS || m->mem.base != ZYDIS_REGISTER_NONE
                               : m->mem.base != (ZydisRegister)(ZYDIS_REGISTER_RAX + x->base))) return 0;
        }
        at += ins.length;
    }
    return 0;
}
static void tls_patch_site(uint64_t site, const unsigned char *code, unsigned length, int32_t disp) {
    if (length != 9 || code[0] != 0x64 || (code[1] & 0xFB) != 0x48 ||
        (code[2] != 0x8B && code[2] != 0x33 && code[2] != 0x3B) || (code[3] & 0xC7) != 0x04 || code[4] != 0x25)
        return;
    const unsigned r = ((code[1] >> 2) & 1) << 3 | ((code[3] >> 3) & 7);
    const unsigned char op = code[2];
    if (r == 4) return; // rsp
    AcquireSRWLockExclusive(&tls_patch_lock);
    unsigned char *slot = NULL;
    if (!tls_site_lookup(site) && (slot = tls_pool_reserve()) != NULL) {
        const int32_t tcb = (int32_t)offsetof(GuestTls, tcb);
        unsigned char *p = slot + TLS_CODE_OFFSET;
        if (op == 0x8B) {
            slot[0] = 1;
            p = tls_emit_slot(p, r);
            p = tls_emit_mem(p, 0x8B, r, r, tcb);
            p = tls_emit_mem(p, 0x8B, r, r, disp);
        } else {
            const unsigned s = r == 11 ? 10 : 11;
            slot[0] = 2; slot[1] = (unsigned char)s;
            static const unsigned char down[] = {0x48, 0x8D, 0xA4, 0x24, 0x78, 0xFF, 0xFF, 0xFF};
            static const unsigned char up[] = {0x48, 0x8D, 0xA4, 0x24, 0x88, 0x00, 0x00, 0x00};
            memcpy(p, down, sizeof(down)); p += sizeof(down);
            *p++ = 0x41; *p++ = (unsigned char)(0x50 | (s & 7));
            p = tls_emit_slot(p, s);
            p = tls_emit_mem(p, 0x8B, s, s, tcb);
            p = tls_emit_mem(p, op, r, s, disp);
            *p++ = 0x41; *p++ = (unsigned char)(0x58 | (s & 7));
            memcpy(p, up, sizeof(up)); p += sizeof(up);
        }
        *p++ = 0xE9;
        p = tls_emit32(p, (int32_t)((int64_t)(site + 9) - (int64_t)(uintptr_t)(p + 4)));
        FlushInstructionCache(GetCurrentProcess(), slot, TLS_SLOT_BYTES);
        const uint64_t entry = (uint64_t)(uintptr_t)(slot + TLS_CODE_OFFSET);
        const int64_t rel = (int64_t)entry - (int64_t)(site + 5);
        const int64_t slot_disp = 0x1480 + 8 * (int64_t)tls_slot;
        const int tcb_disp = (int)offsetof(GuestTls, tcb);
        const ZydisMnemonic mn = op == 0x8B ? ZYDIS_MNEMONIC_MOV : op == 0x33 ? ZYDIS_MNEMONIC_XOR : ZYDIS_MNEMONIC_CMP;
        const int s = r == 11 ? 10 : 11;
        const TlsExpected expect_mov[] = {{ZYDIS_MNEMONIC_MOV, (int)r, -1, 1, slot_disp},
                                          {ZYDIS_MNEMONIC_MOV, (int)r, (int)r, 0, tcb_disp},
                                          {ZYDIS_MNEMONIC_MOV, (int)r, (int)r, 0, disp}};
        const TlsExpected expect_op[] = {{ZYDIS_MNEMONIC_LEA, 4, 4, 0, -136},
                                         {ZYDIS_MNEMONIC_PUSH, s, -2, 0, 0},
                                         {ZYDIS_MNEMONIC_MOV, s, -1, 1, slot_disp},
                                         {ZYDIS_MNEMONIC_MOV, s, s, 0, tcb_disp},
                                         {mn, (int)r, s, 0, disp},
                                         {ZYDIS_MNEMONIC_POP, s, -2, 0, 0},
                                         {ZYDIS_MNEMONIC_LEA, 4, 4, 0, 136}};
        const int verified = op == 0x8B ? tls_verify(slot + TLS_CODE_OFFSET, expect_mov, 3, site + 9)
                                        : tls_verify(slot + TLS_CODE_OFFSET, expect_op, 7, site + 9);
        if (!verified) {
            static volatile LONG reported;
            if (InterlockedExchange(&reported, 1) == 0)
                fprintf(stderr, "TLS_PATCH verify failed at 0x%llx op 0x%02x; site left emulated\n",
                        (unsigned long long)site, op);
        }
        DWORD old;
        if (verified && rel == (int32_t)rel && tls_site_insert(site, entry) &&
            VirtualProtect((void *)(uintptr_t)site, 9, PAGE_EXECUTE_READWRITE, &old)) {
            volatile unsigned char *x = (volatile unsigned char *)(uintptr_t)site;
            x[0] = 0xCC;
            FlushInstructionCache(GetCurrentProcess(), (void *)(uintptr_t)site, 9);
            const int32_t rel32 = (int32_t)rel;
            for (int i = 0; i < 4; ++i) x[1 + i] = (unsigned char)(rel32 >> (8 * i));
            for (int i = 5; i < 9; ++i) x[i] = 0xCC;
            FlushInstructionCache(GetCurrentProcess(), (void *)(uintptr_t)site, 9);
            x[0] = 0xE9;
            FlushInstructionCache(GetCurrentProcess(), (void *)(uintptr_t)site, 9);
            VirtualProtect((void *)(uintptr_t)site, 9, old, &old);
            const LONG n = InterlockedIncrement(&tls_patched);
            if ((n & (n - 1)) == 0) fprintf(stderr, "TLS_PATCH sites=%ld\n", n);
        }
    }
    ReleaseSRWLockExclusive(&tls_patch_lock);
}
// Exceptions of patched sites: the int3 of a site being patched, and a trampoline run by a thread
// whose guest TLS was not attached yet (the slot reads 0): attached, the trampoline starts over.
static int tls_patch_exception(EXCEPTION_POINTERS *e) {
    CONTEXT *c = e->ContextRecord;
    const DWORD code = e->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_BREAKPOINT) {
        const uint64_t entry = tls_site_lookup((uint64_t)(uintptr_t)e->ExceptionRecord->ExceptionAddress);
        if (!entry) return 0;
        c->Rip = entry;
        return 1;
    }
    if (code != EXCEPTION_ACCESS_VIOLATION || !tls_pool || c->Rip < (uintptr_t)tls_pool ||
        c->Rip >= (uintptr_t)tls_pool + TLS_POOL_BYTES || TlsGetValue(tls_slot))
        return 0;
    unsigned char *slot = tls_pool + ((c->Rip - (uintptr_t)tls_pool) & ~(uint64_t)(TLS_SLOT_BYTES - 1));
    const uint64_t start = (uint64_t)(uintptr_t)(slot + TLS_CODE_OFFSET);
    if (slot[0] == 1 && c->Rip == start + 9) {
        runtime_tls_attach();
        c->Rip = start;
        return 1;
    }
    if (slot[0] == 2 && c->Rip == start + 8 + 2 + 9) {
        DWORD64 *s = &c->Rax + slot[1]; // CONTEXT keeps Rax..R15 in register number order
        *s = *(DWORD64 *)(uintptr_t)c->Rsp;
        c->Rsp += 8 + 136;
        runtime_tls_attach();
        c->Rip = start;
        return 1;
    }
    return 0;
}

static int tls_emulate(EXCEPTION_POINTERS *e) {
    // Bootstrap path: emulate only executed, decoded FS absolute 64-bit MOV/XOR/CMP reads of the
    // guest TCB (with ISS_TLS_PATCH, the site is then rewritten, see tls_patch_site).
    if (tls_pool && tls_patch_exception(e)) return 1;
    // Windows can report an FS:[disp32] guest TCB read as PRIV_INSTRUCTION instead of AV.
    // The exact guest-image, FS, operand, displacement and opcode checks below still apply;
    // this is not a general privileged-instruction emulator.
    const DWORD exception_code = e->ExceptionRecord->ExceptionCode;
    if (!source_image || (exception_code != EXCEPTION_ACCESS_VIOLATION &&
                          exception_code != EXCEPTION_PRIV_INSTRUCTION)) return 0;
    CONTEXT *c = e->ContextRecord;
    if (c->Rip < (uintptr_t)source_image || c->Rip - (uintptr_t)source_image >= source_size) return 0;
    // An FS access needs the 0x64 segment prefix among the legacy prefixes (and REX) that start
    // the instruction. Every GPU page tracking fault of guest code passes here first: the check
    // reads the executing code directly instead of a system call and a full decode.
    {
        const unsigned char *at = (const unsigned char *)(uintptr_t)c->Rip;
        const uint64_t left = source_size - (c->Rip - (uintptr_t)source_image);
        int fs = 0;
        for (uint64_t i = 0; i < left && i < 15; ++i) {
            const unsigned char b = at[i];
            if (b == 0x64) fs = 1;
            else if (b != 0xf0 && b != 0xf2 && b != 0xf3 && b != 0x2e && b != 0x36 && b != 0x3e &&
                     b != 0x26 && b != 0x65 && b != 0x66 && b != 0x67 && (b & 0xf0) != 0x40) break;
        }
        if (!fs) return 0;
    }
    unsigned char code[15]; SIZE_T copied = 0;
    ReadProcessMemory(GetCurrentProcess(), (void *)(uintptr_t)c->Rip, code, sizeof(code), &copied);
    ZydisDecoder decoder;
    ZydisDecodedInstruction instruction;
    ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
    if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, code, copied, &instruction, operands)) ||
        instruction.operand_count_visible != 2 || operands[0].type != ZYDIS_OPERAND_TYPE_REGISTER ||
        operands[0].size != 64 || operands[1].type != ZYDIS_OPERAND_TYPE_MEMORY || operands[1].size != 64 ||
        operands[1].mem.segment != ZYDIS_REGISTER_FS || operands[1].mem.base != ZYDIS_REGISTER_NONE ||
        operands[1].mem.index != ZYDIS_REGISTER_NONE || operands[1].mem.disp.value < 0 ||
        operands[1].mem.disp.value > 48) return 0;
    if (instruction.mnemonic != ZYDIS_MNEMONIC_MOV && instruction.mnemonic != ZYDIS_MNEMONIC_XOR &&
        instruction.mnemonic != ZYDIS_MNEMONIC_CMP) return 0;
    DWORD64 *dst = context_register(c, operands[0].reg.value);
    if (!dst) return 0;
    uint64_t value, left = *dst;
    memcpy(&value, (unsigned char *)current_tls()->tcb + operands[1].mem.disp.value, 8);
    if (instruction.mnemonic == ZYDIS_MNEMONIC_MOV) *dst = value;
    else {
        uint64_t result = instruction.mnemonic == ZYDIS_MNEMONIC_XOR ? left ^ value : left - value;
        DWORD flags = result_flags(result);
        if (instruction.mnemonic == ZYDIS_MNEMONIC_CMP) {
            if (left < value) flags |= 1;
            if ((left ^ value ^ result) & 0x10) flags |= 0x10;
            if (((left ^ value) & (left ^ result)) >> 63) flags |= 0x800;
        } else *dst = result;
        c->EFlags = (c->EFlags & ~0x8d5u) | flags;
    }
    const uint64_t site = c->Rip;
    c->Rip += instruction.length;
    InterlockedIncrement64(&tls_faults);
    if (exception_code == EXCEPTION_PRIV_INSTRUCTION) {
        static volatile LONG reported;
        if (InterlockedExchange(&reported, 1) == 0)
            fprintf(stderr, "TLS_PRIV_EMULATED site=0x%llx length=%u disp=%lld\n",
                    (unsigned long long)site, (unsigned)instruction.length,
                    (long long)operands[1].mem.disp.value);
    }
    if (tls_patch_enabled())
        tls_patch_site(site, code, instruction.length, (int32_t)operands[1].mem.disp.value);
    return 1;
}
int runtime_emulate_tls(EXCEPTION_POINTERS *e) {
    if (tls_emulate(e)) return 1;
    // A thread that faulted on the original instruction while another rewrites the site reads
    // bytes that no longer decode as it. The site is in the table before any byte changes (a
    // thread seeing new bytes sees the entry): it goes to the trampoline instead.
    if (tls_pool && (e->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION ||
                     e->ExceptionRecord->ExceptionCode == EXCEPTION_PRIV_INSTRUCTION)) {
        const uint64_t entry = tls_site_lookup(e->ContextRecord->Rip);
        if (entry) {
            e->ContextRecord->Rip = entry;
            return 1;
        }
    }
    return 0;
}
uint64_t runtime_tls_fault_count(void) { return (uint64_t)InterlockedCompareExchange64(&tls_faults, 0, 0); }

// SSE4a (AMD only: the PS4's Jaguar has it, Intel CPUs do not). Guest EXTRQ/INSERTQ raise an
// illegal instruction on Intel; they are interpreted here on the faulting context, as shadPS4's
// cpu_patches.cpp does. Register operands only (the forms compilers emit): 66/F2 [REX] 0F 79 /r,
// 66 [REX] 0F 78 /0 ib ib, F2 [REX] 0F 78 /r ib ib. The upper qword of the destination is
// undefined in the spec; AMD CPUs zero it, and so does this.
static volatile LONG64 sse4a_emulated;
static uint64_t sse4a_field_mask(unsigned length) { return length ? (UINT64_C(1) << length) - 1 : ~UINT64_C(0); }
int runtime_emulate_sse4a(EXCEPTION_POINTERS *e) {
    CONTEXT *c = e->ContextRecord;
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_ILLEGAL_INSTRUCTION || !source_image ||
        c->Rip < (uintptr_t)source_image || c->Rip - (uintptr_t)source_image >= source_size) return 0;
    const uint64_t left = source_size - (c->Rip - (uintptr_t)source_image);
    const unsigned char *at = (const unsigned char *)(uintptr_t)c->Rip;
    if (left < 4 || (at[0] != 0x66 && at[0] != 0xf2)) return 0;
    unsigned n = 1, rex = 0;
    if ((at[n] & 0xf0) == 0x40) rex = at[n++];
    if (left < n + 3 || at[n] != 0x0f || (at[n + 1] != 0x78 && at[n + 1] != 0x79)) return 0;
    const int immediate = at[n + 1] == 0x78, insert = at[0] == 0xf2;
    const unsigned modrm = at[n + 2];
    if ((modrm >> 6) != 3) return 0;
    const unsigned reg = ((rex >> 2) & 1) << 3 | ((modrm >> 3) & 7), rm = (rex & 1) << 3 | (modrm & 7);
    n += 3;
    if (immediate && (left < n + 2 || (!insert && ((modrm >> 3) & 7)))) return 0;
    M128A *xmm = c->FltSave.XmmRegisters;
    if (!insert) {
        // EXTRQ: the immediate form works on xmm(rm) alone; the register form takes the field
        // from xmm(rm) bits 5:0 (length) and 13:8 (index) and works on xmm(reg).
        const unsigned dst = immediate ? rm : reg;
        const unsigned length = immediate ? at[n] & 0x3f : xmm[rm].Low & 0x3f;
        const unsigned index = immediate ? at[n + 1] & 0x3f : (xmm[rm].Low >> 8) & 0x3f;
        xmm[dst].Low = (uint64_t)(xmm[dst].Low >> index) & sse4a_field_mask(length);
        xmm[dst].High = 0;
    } else {
        // INSERTQ: the low length bits of xmm(rm) go to bit index of xmm(reg); the register form
        // takes the field from xmm(rm) bits 69:64 (length) and 77:72 (index).
        const uint64_t field = (uint64_t)xmm[rm].High;
        const unsigned length = immediate ? at[n] & 0x3f : field & 0x3f;
        const unsigned index = immediate ? at[n + 1] & 0x3f : (field >> 8) & 0x3f;
        const uint64_t mask = sse4a_field_mask(length);
        const uint64_t source = (uint64_t)xmm[rm].Low & mask;
        xmm[reg].Low = ((uint64_t)xmm[reg].Low & ~(mask << index)) | source << index;
        xmm[reg].High = 0;
    }
    const uint64_t site = c->Rip;
    c->Rip += n + (immediate ? 2 : 0);
    const LONG64 count = InterlockedIncrement64(&sse4a_emulated);
    if (count == 1)
        fprintf(stderr, "SSE4A_TRAP site=0x%llx: emulated through an exception (it overwrites the "
                        "guest red zone below rsp)\n", (unsigned long long)site);
    if ((count & (count - 1)) == 0)
        fprintf(stderr, "SSE4A_EMULATED count=%lld site=0x%llx\n", (long long)count, (unsigned long long)site);
    return 1;
}

// SSE4a site patching. An emulated EXTRQ is not enough: Windows writes the exception record and
// CONTEXT on the faulting thread's stack right below rsp, over the System V red zone the guest
// keeps live values in (the game's hot EXTRQ function holds red zone values across 32 of its 48
// EXTRQs, and came out wrong: a null pointer later in the intro). Before any guest code runs,
// every register-form EXTRQ of the executable segments is rewritten into a jump to a trampoline
// that computes it below the red zone, without an exception:
//   lea rsp,[rsp-128]; push rax; push rcx; push rdx; pushfq
//   movq rax, xmmS; movq rdx, xmmD; mov ecx, eax; shr ecx, 8; shr rdx, cl     (index)
//   lea ecx,[rax-1]; and ecx, 63; mov eax, 2; shl rax, cl; dec rax; and rdx, rax (length 0 = 64)
//   movq xmmD, rdx (upper qword zeroed, as AMD); popfq; pop rdx; pop rcx; pop rax; lea rsp,[rsp+128]
//   <the instructions displaced by the 5-byte jmp>; jmp back
// A site is left to the trap emulation when a displaced instruction is position dependent or a
// direct branch of the segment lands inside the rewritten bytes. ISS_SSE4A_PATCH=0 off.
enum { SSE4A_POOL_BYTES = 1 << 20, SSE4A_SLOT_BYTES = 128, SSE4A_MAX_SITES = 4096 };
typedef struct { uint64_t site, length, end; unsigned dst, src; int ok; } Sse4aSite;

static int cpu_has_sse4a(void) {
    int r[4];
    __cpuid(r, (int)0x80000000);
    if ((unsigned)r[0] < 0x80000001u) return 0;
    __cpuid(r, (int)0x80000001);
    return (r[2] >> 6) & 1;
}
// EXTRQ xmm, xmm at p: 66 [REX.R/B] 0F 79 modrm(mod=3). Returns its length or 0.
static unsigned sse4a_extrq_reg(const unsigned char *p, unsigned *dst, unsigned *src) {
    unsigned n = 1, rex = 0;
    if (p[0] != 0x66) return 0;
    if ((p[1] & 0xf0) == 0x40) {
        if (p[1] & 0x0a) return 0; // REX.W/REX.X: not the forms emitted
        rex = p[n++];
    }
    if (p[n] != 0x0f || p[n + 1] != 0x79 || (p[n + 2] >> 6) != 3) return 0;
    *dst = ((rex >> 2) & 1) << 3 | ((p[n + 2] >> 3) & 7);
    *src = (rex & 1) << 3 | (p[n + 2] & 7);
    return n + 3;
}
// movq r64, xmm (0x7E) or movq xmm, r64 (0x6E).
static unsigned char *sse4a_emit_movq(unsigned char *p, unsigned char op, unsigned xmm, unsigned gpr) {
    *p++ = 0x66;
    *p++ = (unsigned char)(0x48 | ((xmm >> 3) << 2) | (gpr >> 3));
    *p++ = 0x0f; *p++ = op;
    *p++ = (unsigned char)(0xc0 | ((xmm & 7) << 3) | (gpr & 7));
    return p;
}
static unsigned char *sse4a_emit_extrq(unsigned char *p, unsigned dst, unsigned src) {
    static const unsigned char enter[] = {0x48, 0x8D, 0x64, 0x24, 0x80, 0x50, 0x51, 0x52, 0x9C};
    static const unsigned char field[] = {0x89, 0xC1, 0xC1, 0xE9, 0x08, 0x48, 0xD3, 0xEA,
                                          0x8D, 0x48, 0xFF, 0x83, 0xE1, 0x3F, 0xB8, 0x02, 0x00, 0x00, 0x00,
                                          0x48, 0xD3, 0xE0, 0x48, 0xFF, 0xC8, 0x48, 0x21, 0xC2};
    static const unsigned char leave[] = {0x9D, 0x5A, 0x59, 0x58, 0x48, 0x8D, 0xA4, 0x24, 0x80, 0x00, 0x00, 0x00};
    memcpy(p, enter, sizeof(enter)); p += sizeof(enter);
    p = sse4a_emit_movq(p, 0x7e, src, 0); // rax = xmmS
    p = sse4a_emit_movq(p, 0x7e, dst, 2); // rdx = xmmD
    memcpy(p, field, sizeof(field)); p += sizeof(field);
    p = sse4a_emit_movq(p, 0x6e, dst, 2); // xmmD = rdx
    memcpy(p, leave, sizeof(leave)); p += sizeof(leave);
    return p;
}

void runtime_patch_sse4a(const uint64_t *segment_addresses, const uint64_t *segment_sizes, uint64_t count) {
    char value[8] = {0};
    GetEnvironmentVariableA("ISS_SSE4A_PATCH", value, sizeof(value));
    if (!source_image || value[0] == '0' || cpu_has_sse4a()) return;
    ZydisDecoder decoder;
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
    static Sse4aSite sites[SSE4A_MAX_SITES];
    uint64_t found = 0;
    // Pass 1: a linear sweep of the code finds the EXTRQ instruction starts and how many bytes
    // a 5-byte jmp displaces there.
    for (uint64_t s = 0; s < count; ++s) {
        const unsigned char *base = source_image + segment_addresses[s];
        for (uint64_t at = 0; at < segment_sizes[s];) {
            ZydisDecodedInstruction ins;
            const uint64_t left = segment_sizes[s] - at;
            if (!ZYAN_SUCCESS(ZydisDecoderDecodeInstruction(&decoder, NULL, base + at, left < 15 ? left : 15, &ins))) {
                ++at;
                continue;
            }
            unsigned dst, src;
            if (ins.mnemonic == ZYDIS_MNEMONIC_EXTRQ && left >= 16 && sse4a_extrq_reg(base + at, &dst, &src) == ins.length &&
                found < SSE4A_MAX_SITES) {
                Sse4aSite *site = &sites[found++];
                *site = (Sse4aSite){segment_addresses[s] + at, 0, 0, dst, src, 1};
                uint64_t cover = ins.length;
                while (cover < 5) {
                    ZydisDecodedInstruction next;
                    if (!ZYAN_SUCCESS(ZydisDecoderDecodeInstruction(&decoder, NULL, base + at + cover, 15, &next)) ||
                        ((next.attributes & ZYDIS_ATTRIB_IS_RELATIVE) && next.mnemonic != ZYDIS_MNEMONIC_EXTRQ) ||
                        next.meta.branch_type != ZYDIS_BRANCH_TYPE_NONE) {
                        site->ok = 0;
                        break;
                    }
                    cover += next.length;
                }
                site->length = cover;
                site->end = site->site + cover;
            }
            at += ins.length;
        }
    }
    if (!found) return;
    // Pass 2: no direct branch may land inside the bytes a site rewrites (after its first byte).
    for (uint64_t s = 0; s < count; ++s) {
        const unsigned char *base = source_image + segment_addresses[s];
        for (uint64_t at = 0; at < segment_sizes[s];) {
            ZydisDecodedInstruction ins;
            const uint64_t left = segment_sizes[s] - at;
            if (!ZYAN_SUCCESS(ZydisDecoderDecodeInstruction(&decoder, NULL, base + at, left < 15 ? left : 15, &ins))) {
                ++at;
                continue;
            }
            if (ins.meta.branch_type != ZYDIS_BRANCH_TYPE_NONE && ins.raw.imm[0].is_relative) {
                const uint64_t target = segment_addresses[s] + at + ins.length + (uint64_t)ins.raw.imm[0].value.s;
                for (uint64_t i = 0; i < found; ++i)
                    if (target > sites[i].site && target < sites[i].end) sites[i].ok = 0;
            }
            at += ins.length;
        }
    }
    unsigned char *pool = NULL;
    const uint64_t start = ((uint64_t)(uintptr_t)source_image + source_size + 0xffff) & ~UINT64_C(0xffff);
    for (uint64_t a = start; a < start + (UINT64_C(1) << 30) && !pool; a += 0x10000)
        pool = VirtualAlloc((void *)(uintptr_t)a, SSE4A_POOL_BYTES, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!pool) {
        fprintf(stderr, "SSE4A_PATCH no trampoline memory near the image; %llu sites left emulated\n",
                (unsigned long long)found);
        return;
    }
    uint64_t patched = 0, used = 0, patched_end = 0;
    for (uint64_t i = 0; i < found && used + SSE4A_SLOT_BYTES <= SSE4A_POOL_BYTES; ++i) {
        Sse4aSite *site = &sites[i];
        // An EXTRQ displaced by the jmp of the site before it runs in that site's trampoline.
        if (site->site < patched_end) {
            ++patched;
            continue;
        }
        if (!site->ok) {
            fprintf(stderr, "SSE4A_PATCH skipped site=0x%llx (displaced code is position dependent or a branch target)\n",
                    (unsigned long long)(uintptr_t)(source_image + site->site));
            continue;
        }
        unsigned char *code = source_image + site->site;
        unsigned char *slot = pool + used, *p = slot;
        // The EXTRQ, then the displaced instructions: further EXTRQs are emulated too, the rest
        // copied as they are (position independent, checked in pass 1).
        for (uint64_t at = 0; at < site->length;) {
            unsigned dst, src;
            const unsigned length = sse4a_extrq_reg(code + at, &dst, &src);
            ZydisDecodedInstruction ins;
            if (length) {
                p = sse4a_emit_extrq(p, dst, src);
                at += length;
            } else if (ZYAN_SUCCESS(ZydisDecoderDecodeInstruction(&decoder, NULL, code + at, 15, &ins))) {
                memcpy(p, code + at, ins.length);
                p += ins.length;
                at += ins.length;
            } else {
                break;
            }
        }
        const int64_t back = (int64_t)(uintptr_t)(code + site->length) - (int64_t)(uintptr_t)(p + 5);
        const int64_t there = (int64_t)(uintptr_t)slot - (int64_t)(uintptr_t)(code + 5);
        if ((uint64_t)(p - slot) + 5 > SSE4A_SLOT_BYTES || back != (int32_t)back || there != (int32_t)there) continue;
        *p++ = 0xe9;
        const int32_t back32 = (int32_t)back, there32 = (int32_t)there;
        memcpy(p, &back32, 4);
        used += SSE4A_SLOT_BYTES;
        code[0] = 0xe9;
        memcpy(code + 1, &there32, 4);
        for (uint64_t k = 5; k < site->length; ++k) code[k] = 0xcc;
        patched_end = site->end;
        ++patched;
    }
    FlushInstructionCache(GetCurrentProcess(), NULL, 0);
    fprintf(stderr, "SSE4A_PATCH sites=%llu patched=%llu (CPU without SSE4a)\n",
            (unsigned long long)found, (unsigned long long)patched);
}
