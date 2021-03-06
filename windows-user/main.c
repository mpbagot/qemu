/*
 *  qemu user main
 *
 *  Copyright (c) 2003-2008 Fabrice Bellard
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include <ntstatus.h>
#define WIN32_NO_STATUS

#include "qemu/osdep.h"
#include "qemu-version.h"

#include <wine/library.h>
#include <wine/debug.h>
#include <wine/unicode.h>
#include <delayloadhandler.h>

#include "qapi/error.h"
#include "qemu.h"
#include "qemu/path.h"
#include "qemu/config-file.h"
#include "qemu/cutils.h"
#include "qemu/help_option.h"
#include "cpu.h"
#include "exec/exec-all.h"
#include "tcg/tcg.h"
#include "qemu/timer.h"
#include "qemu/envlist.h"
#include "exec/log.h"
#include "trace/control.h"
#include "glib-compat.h"
#include "qemu-common.h"

#include "win_syscall.h"
#include "pe.h"

WINE_DEFAULT_DEBUG_CHANNEL(qemu);

extern LPWSTR __cdecl wcsstr( LPCWSTR str, LPCWSTR sub );

char *exec_path;

int singlestep;
static const char *filename;
unsigned long guest_base;
bool have_guest_base;
unsigned long reserved_va;
static struct qemu_pe_image image;
BOOL is_32_bit;
unsigned long last_brk;
const WCHAR *qemu_pathname;

PEB guest_PEB;
PEB32 *guest_PEB32;
static PEB_LDR_DATA guest_ldr;
static RTL_USER_PROCESS_PARAMETERS process_params;
static RTL_USER_PROCESS_PARAMETERS32 *process_params32;
static RTL_BITMAP guest_tls_bitmap;
static RTL_BITMAP guest_tls_expansion_bitmap;
static RTL_BITMAP guest_fls_bitmap;

__thread CPUState *thread_cpu;
__thread TEB *guest_teb;
__thread TEB32 *guest_teb32;

char * (* WINAPI p_CharNextA)(const char *ptr);

/* Helper function to read the TEB exception filter chain. */
uint64_t guest_exception_handler, guest_call_entry;

bool qemu_cpu_is_self(CPUState *cpu)
{
    qemu_log("qemu_cpu_is_self unimplemented.\n");
    return true;
}

void qemu_cpu_kick(CPUState *cpu)
{
    qemu_log("qemu_cpu_kick unimplemented.\n");
}

uint64_t cpu_get_tsc(CPUX86State *env)
{
    qemu_log("cpu_get_tsc unimplemented.\n");
    return 0;
}

int cpu_get_pic_interrupt(CPUX86State *env)
{
    qemu_log("cpu_get_pic_interrupt unimplemented.\n");
    return -1;
}

static void write_dt(void *ptr, unsigned long addr, unsigned long limit,
                     int flags)
{
    unsigned int e1, e2;
    uint32_t *p;
    e1 = (addr << 16) | (limit & 0xffff);
    e2 = ((addr >> 16) & 0xff) | (addr & 0xff000000) | (limit & 0x000f0000);
    e2 |= flags;
    p = ptr;
    p[0] = tswap32(e1);
    p[1] = tswap32(e2);
}

static uint64_t *idt_table;
static void set_gate64(void *ptr, unsigned int type, unsigned int dpl,
                       uint64_t addr, unsigned int sel)
{
    uint32_t *p, e1, e2;
    e1 = (addr & 0xffff) | (sel << 16);
    e2 = (addr & 0xffff0000) | 0x8000 | (dpl << 13) | (type << 8);
    p = ptr;
    p[0] = tswap32(e1);
    p[1] = tswap32(e2);
    p[2] = tswap32(addr >> 32);
    p[3] = 0;
}
/* only dpl matters as we do only user space emulation */
static void set_idt(int n, unsigned int dpl)
{
    set_gate64(idt_table + n * 2, 0, dpl, 0, 0);
}

static void free_teb(TEB *teb, TEB32 *teb32)
{
    VirtualFree(teb->Tib.StackLimit, 0, MEM_RELEASE);
    VirtualFree(teb, 0, MEM_RELEASE);
    if (teb32)
        VirtualFree(teb32, 0, MEM_RELEASE);
}

static TEB *alloc_teb(TEB32 **teb32)
{
    TEB *ret;
    TEB32 *ret32 = NULL;

    ret = VirtualAlloc(NULL, 0x2000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!ret)
    {
        fprintf(stderr, "Failed to allocate TEB\n");
        ExitProcess(1);
    }

    ret->Tib.Self = &ret->Tib;
    ret->Tib.ExceptionList = (void *)~0UL;
    ret->Peb = &guest_PEB;
    ret->ClientId = NtCurrentTeb()->ClientId;
    ret->StaticUnicodeString.Buffer = ret->StaticUnicodeBuffer;
    ret->StaticUnicodeString.MaximumLength = sizeof(ret->StaticUnicodeBuffer);

    if (is_32_bit)
    {
        ret32 = VirtualAlloc(NULL, 0x2000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!ret)
        {
            fprintf(stderr, "Failed to allocate 32 bit TEB\n");
            ExitProcess(1);
        }

        ret32->Tib.Self = (qemu_ptr)(ULONG_PTR)&ret32->Tib;
        ret32->Tib.ExceptionList = ~0U;
        ret32->Peb = (qemu_ptr)(ULONG_PTR)guest_PEB32;
        ret32->ClientId.UniqueProcess = (ULONG_PTR)ret->ClientId.UniqueProcess;
        ret32->ClientId.UniqueThread = (ULONG_PTR)ret->ClientId.UniqueThread;
        ret32->StaticUnicodeString.Buffer = (ULONG_PTR)ret32->StaticUnicodeBuffer;
        ret32->StaticUnicodeString.MaximumLength = sizeof(ret32->StaticUnicodeBuffer);
        ret->glReserved2 = ret32;
    }

    *teb32 = ret32;
    return ret;
}

TEB *qemu_getTEB(void)
{
    return guest_teb;
}

TEB32 *qemu_getTEB32(void)
{
    return guest_teb32;
}

static void init_thread_cpu(void)
{
    CPUX86State *env;
    void *stack;
    CPUState *cpu = thread_cpu;
    DWORD stack_reserve = image.stack_reserve ? image.stack_reserve : DEFAULT_STACK_SIZE;

    guest_teb = alloc_teb(&guest_teb32);

    if (!cpu)
        cpu = cpu_create(X86_CPU_TYPE_NAME("qemu64"));
    if (!cpu)
    {
        fprintf(stderr, "Unable to find CPU definition\n");
        ExitProcess(EXIT_FAILURE);
    }
    env = cpu->env_ptr;
    cpu_reset(cpu);

    env->cr[0] = CR0_PG_MASK | CR0_WP_MASK | CR0_PE_MASK;
    env->hflags |= HF_PE_MASK | HF_CPL_MASK;
    if (env->features[FEAT_1_EDX] & CPUID_SSE) {
        env->cr[4] |= CR4_OSFXSR_MASK;
        env->hflags |= HF_OSFXSR_MASK;
    }
    if (!is_32_bit)
    {
        /* enable 64 bit mode if possible */
        if (!(env->features[FEAT_8000_0001_EDX] & CPUID_EXT2_LM))
        {
            fprintf(stderr, "The selected x86 CPU does not support 64 bit mode\n");
            ExitProcess(EXIT_FAILURE);
        }
        env->cr[4] |= CR4_PAE_MASK;
        env->efer |= MSR_EFER_LMA | MSR_EFER_LME;
        env->hflags |= HF_LMA_MASK;
    }
    /* flags setup : we activate the IRQs by default as in user mode */
    env->eflags |= IF_MASK;

    /* FPU control word. */
    cpu_set_fpuc(env, 0x27f);

    /* FIXME: I should RESERVE stack_reserve bytes, and commit only stack_commit bytes and
     * place a guard page at the end of the committed range. This will need exception handing
     * (and better knowledge in my brain), so commit the entire stack for now.
     *
     * Afaics when the reserved area is exhausted an exception is triggered and Windows does
     * not try to reserve more. Is this correct? */
    stack = VirtualAlloc(NULL, stack_reserve, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!stack)
    {
        fprintf(stderr, "Could not reserve stack space size %u.\n", stack_reserve);
        ExitProcess(EXIT_FAILURE);
    }
    /* Stack grows down, so point to the end of the allocation. */
    env->regs[R_ESP] = h2g(stack) + stack_reserve;

    /* FIXME: Something here does not make sense for 32 bit. set_idt should behave differently,
     * and probably a few other things. */
    env->idt.limit = is_32_bit ? 255 : 511;
    idt_table = my_alloc(sizeof(uint64_t) * (511 + 1));
    env->idt.base = h2g(idt_table);
    set_idt(0, 0);
    set_idt(1, 0);
    set_idt(2, 0);
    set_idt(3, 3);
    set_idt(4, 3);
    set_idt(5, 0);
    set_idt(6, 0);
    set_idt(7, 0);
    set_idt(8, 0);
    set_idt(9, 0);
    set_idt(10, 0);
    set_idt(11, 0);
    set_idt(12, 0);
    set_idt(13, 0);
    set_idt(14, 0);
    set_idt(15, 0);
    set_idt(16, 0);
    set_idt(17, 0);
    set_idt(18, 0);
    set_idt(19, 0);
    set_idt(0x80, 3);

    /* linux segment setup */
    {
        uint64_t *gdt_table;
        env->gdt.base = h2g(my_alloc(sizeof(uint64_t) * TARGET_GDT_ENTRIES));
        env->gdt.limit = sizeof(uint64_t) * TARGET_GDT_ENTRIES - 1;
        gdt_table = g2h(env->gdt.base);
        if (is_32_bit)
        {
            write_dt(&gdt_table[__USER_CS >> 3], 0, 0xfffff,
                    DESC_G_MASK | DESC_B_MASK | DESC_P_MASK | DESC_S_MASK |
                    (3 << DESC_DPL_SHIFT) | (0xa << DESC_TYPE_SHIFT));
        }
        else
        {
            /* 64 bit code segment */
            write_dt(&gdt_table[__USER_CS >> 3], 0, 0xfffff,
                    DESC_G_MASK | DESC_B_MASK | DESC_P_MASK | DESC_S_MASK |
                    DESC_L_MASK |
                    (3 << DESC_DPL_SHIFT) | (0xa << DESC_TYPE_SHIFT));
        }
        write_dt(&gdt_table[__USER_DS >> 3], 0, 0xfffff,
                DESC_G_MASK | DESC_B_MASK | DESC_P_MASK | DESC_S_MASK |
                (3 << DESC_DPL_SHIFT) | (0x2 << DESC_TYPE_SHIFT));
    }
    cpu_x86_load_seg(env, R_CS, __USER_CS);
    cpu_x86_load_seg(env, R_SS, __USER_DS);
    if (is_32_bit)
    {
        cpu_x86_load_seg(env, R_DS, __USER_DS);
        cpu_x86_load_seg(env, R_ES, __USER_DS);
        cpu_x86_load_seg(env, R_FS, __USER_DS);
        cpu_x86_load_seg(env, R_GS, __USER_DS);
        /* ??? */
        /* env->segs[R_FS].selector = 0; */
    }
    else
    {
        cpu_x86_load_seg(env, R_DS, 0);
        cpu_x86_load_seg(env, R_ES, 0);
        cpu_x86_load_seg(env, R_FS, 0);
        cpu_x86_load_seg(env, R_GS, 0);
    }
    env->segs[R_GS].base = h2g(guest_teb);

    guest_teb->Tib.StackBase = (void *)(h2g(stack) + stack_reserve);
    guest_teb->Tib.StackLimit = (void *)h2g(stack);

    if (guest_teb32)
    {
        env->segs[R_FS].base = h2g(guest_teb32);
        guest_teb32->Tib.StackBase = (qemu_ptr)(h2g(stack) + stack_reserve);
        guest_teb32->Tib.StackLimit = (qemu_ptr)h2g(stack);
    }

    /* FIXME: Figure out how to free the CPU, stack, TEB and IDT on thread exit. */
    thread_cpu = cpu;
}

static void cpu_env_to_context_64(qemu_CONTEXT_X86_64 *context, CPUX86State *env)
{
    X86XSaveArea buf;

    memset(context, 0, sizeof(*context));

    /* PXhome */

    context->ContextFlags = QEMU_CONTEXT_CONTROL | QEMU_CONTEXT_INTEGER | QEMU_CONTEXT_SEGMENTS | QEMU_CONTEXT_DEBUG_REGISTERS;
    context->MxCsr = env->mxcsr;

    /* FIXME: Do I really want .selector? I'm not entirely sure how those segment regs work. */
    context->SegCs = env->segs[R_CS].selector;
    context->SegDs = env->segs[R_DS].selector;
    context->SegEs = env->segs[R_ES].selector;
    context->SegFs = env->segs[R_FS].selector;
    context->SegGs = env->segs[R_GS].selector;
    context->SegSs = env->segs[R_SS].selector;

    context->EFlags = env->eflags;

    context->Dr0 = env->dr[0];
    context->Dr1 = env->dr[1];
    context->Dr2 = env->dr[2];
    context->Dr3 = env->dr[3];
    context->Dr6 = env->dr[6];
    context->Dr7 = env->dr[7];

    context->Rax = env->regs[R_EAX];
    context->Rbx = env->regs[R_EBX];
    context->Rcx = env->regs[R_ECX];
    context->Rdx = env->regs[R_EDX];
    context->Rsp = env->regs[R_ESP];
    context->Rbp = env->regs[R_EBP];
    context->Rsi = env->regs[R_ESI];
    context->Rdi = env->regs[R_EDI];
    context->R8 = env->regs[8];
    context->R9 = env->regs[9];
    context->R10 = env->regs[10];
    context->R11 = env->regs[11];
    context->R12 = env->regs[12];
    context->R13 = env->regs[13];
    context->R14 = env->regs[14];
    context->R15 = env->regs[15];
    context->Rip = env->eip;

    /* Floating point. */
    x86_cpu_xsave_all_areas(env_archcpu(env), &buf);
    memcpy(&context->FltSave, &buf.legacy, sizeof(context->FltSave));
    /* This is implicitly set to 0 by x86_cpu_xsave_all_areas (via memset),
     * but the fxsave implementation in target/i386/fpu_helper.c sets it
     * to the value below. */
    context->FltSave.MxCsr_Mask = 0x0000ffff;
}

static void cpu_env_to_context_32(struct qemu_CONTEXT_X86 *context, CPUX86State *env)
{
    memset(context, 0, sizeof(*context));

    /* PXhome */

    context->ContextFlags = QEMU_CONTEXT_CONTROL | QEMU_CONTEXT_INTEGER | QEMU_CONTEXT_SEGMENTS | QEMU_CONTEXT_DEBUG_REGISTERS;

    /* FIXME: Do I really want .selector? I'm not entirely sure how those segment regs work. */
    context->SegCs = env->segs[R_CS].selector;
    context->SegDs = env->segs[R_DS].selector;
    context->SegEs = env->segs[R_ES].selector;
    context->SegFs = env->segs[R_FS].selector;
    context->SegGs = env->segs[R_GS].selector;
    context->SegSs = env->segs[R_SS].selector;

    context->EFlags = env->eflags;

    context->Dr0 = env->dr[0];
    context->Dr1 = env->dr[1];
    context->Dr2 = env->dr[2];
    context->Dr3 = env->dr[3];
    context->Dr6 = env->dr[6];
    context->Dr7 = env->dr[7];

    context->Eax = env->regs[R_EAX];
    context->Ebx = env->regs[R_EBX];
    context->Ecx = env->regs[R_ECX];
    context->Edx = env->regs[R_EDX];
    context->Esp = env->regs[R_ESP];
    context->Ebp = env->regs[R_EBP];
    context->Esi = env->regs[R_ESI];
    context->Edi = env->regs[R_EDI];
    context->Eip = env->eip;

    /* TODO: Floating point. */
    /* TODO: What is ExtendedRegisters? Seems to belong to float and contain stuff like MMX. */
}

static void cpu_loop(const void *code)
{
    CPUState *cs;
    CPUX86State *env;
    int trapnr;
    void *syscall;
    EXCEPTION_POINTERS except;
    struct
    {
        qemu_ptr ExceptionRecord;
        qemu_ptr ContextRecord;
    } except32;
    EXCEPTION_RECORD exception_record;
    struct
    {
        DWORD ExceptionCode;
        DWORD ExceptionFlags;
        qemu_ptr *ExceptionRecord;
        qemu_ptr ExceptionAddress;
        DWORD NumberParameters;
        qemu_ptr ExceptionInformation[EXCEPTION_MAXIMUM_PARAMETERS];
    } record32;
    union
    {
        struct qemu_CONTEXT_X86 guest_context32;
        qemu_CONTEXT_X86_64 guest_context64;
    } ctxes;
    TEB *teb = NtCurrentTeb();

    cs = thread_cpu;
    env = cs->env_ptr;

    env->eip = h2g(code);

    for (;;)
    {
        cpu_exec_start(cs);
        trapnr = cpu_exec(cs);
        cpu_exec_end(cs);
        process_queued_cpu_work(cs);

        switch (trapnr)
        {
            case EXCP_SYSCALL:
                syscall = g2h(env->regs[R_ECX]);
                if (!syscall) /* Return from guest to host. */
                    return;

                /* Making SetLastError / GetLastError call out of the VM is
                 * slow and doesn't work with inlined code. Hooking SetLastError
                 * in theory allows us to replace it with something that updates
                 * the host and guest TEB, but fails if SetLastError is inlined
                 * in the Wine libraries as it should (but somehow on arm64 it
                 * is not inlined). So do the other quite expensive thing and
                 * copy the values on every syscall. */
                if (is_32_bit)
                {
                    teb->LastErrorValue = guest_teb32->LastErrorValue;
                    do_syscall(syscall);
                    guest_teb32->LastErrorValue = teb->LastErrorValue;
                }
                else
                {
                    teb->LastErrorValue = guest_teb->LastErrorValue;
                    do_syscall(syscall);
                    guest_teb->LastErrorValue = teb->LastErrorValue;
                }
                continue;

            case EXCP0E_PAGE:
                if (is_32_bit)
                {
                    memset(&except32, 0, sizeof(except32));
                    except32.ExceptionRecord = (ULONG_PTR)&record32;
                    except32.ContextRecord = (ULONG_PTR)&ctxes.guest_context32;

                    memset(&record32, 0, sizeof(record32));
                    record32.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
                    record32.ExceptionFlags = 0;
                    record32.ExceptionRecord = 0;
                    record32.ExceptionAddress = (ULONG_PTR)env->eip;
                    record32.NumberParameters = 0;

                    cpu_env_to_context_32(&ctxes.guest_context32, env);
                }
                else
                {
                    memset(&except, 0, sizeof(except));
                    except.ExceptionRecord = &exception_record;
                    except.ContextRecord = (void *)&ctxes.guest_context64;

                    memset(&exception_record, 0, sizeof(exception_record));
                    exception_record.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
                    exception_record.ExceptionFlags = 0;
                    exception_record.ExceptionRecord = NULL;
                    exception_record.ExceptionAddress = (void *)env->eip;
                    exception_record.NumberParameters = 0;

                    cpu_env_to_context_64(&ctxes.guest_context64, env);
                }

                if (env->eip == guest_exception_handler)
                {
                    fprintf(stderr, "Failure on first exception handler instruction, terminating.\n");
                    cpu_dump_state(cs, stderr, 0);
                    ExitProcess(1);
                }

                WINE_ERR("Got a page fault in user code, resuming execution at exception handler 0x%lx, rsp %p.\n",
                        guest_exception_handler, (void *)env->regs[R_ESP]);
                cpu_dump_state(cs, stderr, 0);

                env->regs[R_ESP] -= 0x20; /* Reserve 32 bytes for the handler function. */
                /* It seems we have to deliberately misalign the stack by 8 bytes here because
                 * we don't push a return address onto the stack. */
                env->regs[R_ESP] &= ~0xf;
                env->regs[R_ESP] += 8;
                env->regs[R_ECX] = is_32_bit ? h2g(&except32) : h2g(&except);
                env->eip = guest_exception_handler;
                continue;

            case EXCP_INTERRUPT:
                break;

            case EXCP_ATOMIC:
                cpu_exec_step_atomic(cs);
                break;

            default:
                WINE_ERR("Unhandled trap %x, exiting.\n", trapnr);
                cpu_dump_state(cs, stderr, 0);
                ExitProcess(255);
        }
    }
}

uint64_t qemu_execute(const void *code, uint64_t rcx)
{
    CPUState *cs;
    CPUX86State *env;
    uint64_t backup_eip, retval;
    target_ulong backup_regs[CPU_NB_REGS];
    static char *ret_code;
    TEB *teb = NtCurrentTeb();

    if (!code)
    {
        WINE_ERR("Attempting to execute NULL.\n");
        ExitProcess(1);
    }

    if (!ret_code)
    {
        if (is_32_bit)
        {
            static const char ret_code32[] =
            {
                0x31, 0xc9,         /* xor %ecx, %ecx */
                0x0f, 0x05          /* syscall        */
            };
            ret_code = my_alloc(sizeof(ret_code32));
            memcpy(ret_code, ret_code32, sizeof(ret_code32));
        }
        else
        {
            static const char ret_code64[] =
            {
                0x48, 0x31, 0xc9,   /* xor %rcx, %rcx */
                0x0f, 0x05          /* syscall        */
            };
            ret_code = my_alloc(sizeof(ret_code64));
            memcpy(ret_code, ret_code64, sizeof(ret_code64));
        }
    }

    /* The basic idea of this function is to back up all registers, write the function argument
     * into rcx, reserve stack space on the guest stack as the Win64 calling convention mandates
     * and call the emulated CPU to execute the requested code.
     *
     * We need to make sure the emulated CPU interrupts execution after the called function
     * returns and cpu_loop() returns as well. cpu_loop should not return if the function executes
     * a syscall. To achieve that, we push a return address into the guest stack that points to
     * an syscall(rcx=0) instruction. The this interrupt the CPU and cpu_loop recognizes the
     * zero value and returns gracefully.
     *
     * Afterwards restore registers and read the return value from EAX / RAX.
     *
     * Note that we're also storing caller-saved registers. From the view of our guest libraries
     * it is doing a syscall and not a function call, so it doesn't know it has to back up caller-
     * saved regs. We could alternatively tell gcc to clobber everything that is not callee-saved.
     * However, syscalls happen very often and callbacks into app code are relatively rare. Keep
     * the backup cost on the callback side. It's also a host memcpy vs emulated code. */
    cs = thread_cpu;
    if (!cs)
    {
        WINE_TRACE("Initializing new CPU for thread %x.\n", GetCurrentThreadId());
        rcu_register_thread();
        tcg_register_thread();
        init_thread_cpu();
        cs = thread_cpu;
        MODULE_DllThreadAttach(NULL);
    }
    env = cs->env_ptr;

    backup_eip = env->eip;
    memcpy(backup_regs, env->regs, sizeof(backup_regs));
    env->regs[R_ECX] = rcx;

    if (is_32_bit)
    {
        env->regs[R_ESP] -= 0x24; /* Keeps the longjmp detection simpler */
        /* Write the address of our return code onto the stack. */
        *(uint32_t *)g2h(env->regs[R_ESP]) = h2g(ret_code);
        guest_teb32->LastErrorValue = teb->LastErrorValue;
    }
    else
    {
        env->regs[R_ESP] -= 0x28; /* Reserve 32 bytes + 8 for the return address. */
        /* Write the address of our return code onto the stack. */
        *(uint64_t *)g2h(env->regs[R_ESP]) = h2g(ret_code);
        guest_teb->LastErrorValue = teb->LastErrorValue;
    }

    WINE_TRACE("Going to call guest code %p.\n", code);
    cpu_loop(code);

    if (is_32_bit)
        teb->LastErrorValue = guest_teb32->LastErrorValue;
    else
        teb->LastErrorValue = guest_teb->LastErrorValue;

    if (backup_regs[R_ESP] - 0x20 != env->regs[R_ESP])
    {
        WINE_ERR("Stack pointer is 0x%lx, expected 0x%lx, longjump or unwind going on?\n",
                backup_regs[R_ESP] - 0x20, env->regs[R_ESP]);
        ExitProcess(1);
    }

    retval = env->regs[R_EAX];
    memcpy(env->regs, backup_regs, sizeof(backup_regs));
    env->eip = backup_eip;

    WINE_TRACE("retval %lx.\n", retval);
    return retval;
}

static void usage(int exitcode);

static void handle_arg_help(const char *arg)
{
    usage(EXIT_SUCCESS);
}

static void handle_arg_log(const char *arg)
{
    int mask;

    mask = qemu_str_to_log_mask(arg);
    if (!mask)
    {
        qemu_print_log_usage(stdout);
        ExitProcess(EXIT_FAILURE);
    }
    qemu_log_needs_buffers();
    qemu_set_log(mask);
}

struct qemu_argument
{
    const char *argv;
    const char *env;
    bool has_arg;
    void (*handle_opt)(const char *arg);
    const char *example;
    const char *help;
};

static const struct qemu_argument arg_table[] =
{
    {"h",          "",                 false, handle_arg_help,
     "",           "print this help"},
    {"help",       "",                 false, handle_arg_help,
     "",           ""},
    {"d",          "QEMU_LOG",         true,  handle_arg_log,
     "item[,...]", "enable logging of specified items "
     "(use '-d help' for a list of items)"},
    {NULL, NULL, false, NULL, NULL, NULL}
};

static void usage(int exitcode)
{
    const struct qemu_argument *arginfo;
    int maxarglen;
    int maxenvlen;

    printf("usage: qemu-" TARGET_NAME " [options] program [arguments...]\n"
           "Linux CPU emulator (compiled for " TARGET_NAME " emulation)\n"
           "\n"
           "Options and associated environment variables:\n"
           "\n");

    /* Calculate column widths. We must always have at least enough space
     * for the column header.
     */
    maxarglen = strlen("Argument");
    maxenvlen = strlen("Env-variable");

    for (arginfo = arg_table; arginfo->handle_opt != NULL; arginfo++)
    {
        int arglen = strlen(arginfo->argv);
        if (arginfo->has_arg)
        {
            arglen += strlen(arginfo->example) + 1;
        }
        if (strlen(arginfo->env) > maxenvlen)
        {
            maxenvlen = strlen(arginfo->env);
        }
        if (arglen > maxarglen)
        {
            maxarglen = arglen;
        }
    }

    printf("%-*s %-*s Description\n", maxarglen+1, "Argument",
            maxenvlen, "Env-variable");

    for (arginfo = arg_table; arginfo->handle_opt != NULL; arginfo++)
    {
        if (arginfo->has_arg)
        {
            printf("-%s %-*s %-*s %s\n", arginfo->argv,
                   (int)(maxarglen - strlen(arginfo->argv) - 1),
                   arginfo->example, maxenvlen, arginfo->env, arginfo->help);
        }
        else
        {
            printf("-%-*s %-*s %s\n", maxarglen, arginfo->argv,
                    maxenvlen, arginfo->env,
                    arginfo->help);
        }
    }

    ExitProcess(exitcode);
}

static int parse_args(int argc, char **argv)
{
    const char *r;
    int optind;
    const struct qemu_argument *arginfo;

    for (arginfo = arg_table; arginfo->handle_opt != NULL; arginfo++) {
        if (arginfo->env == NULL) {
            continue;
        }

        r = getenv(arginfo->env);
        if (r != NULL) {
            arginfo->handle_opt(r);
        }
    }

    optind = 1;
    for (;;) {
        if (optind >= argc) {
            break;
        }
        r = argv[optind];
        if (r[0] != '-') {
            break;
        }
        optind++;
        r++;
        if (!strcmp(r, "-")) {
            break;
        }
        /* Treat --foo the same as -foo.  */
        if (r[0] == '-') {
            r++;
        }

        for (arginfo = arg_table; arginfo->handle_opt != NULL; arginfo++) {
            if (!strcmp(r, arginfo->argv)) {
                if (arginfo->has_arg) {
                    if (optind >= argc) {
                        (void) fprintf(stderr,
                            "qemu: missing argument for option '%s'\n", r);
                        ExitProcess(EXIT_FAILURE);
                    }
                    arginfo->handle_opt(argv[optind]);
                    optind++;
                } else {
                    arginfo->handle_opt(NULL);
                }
                break;
            }
        }

        /* no option matched the current argv */
        if (arginfo->handle_opt == NULL) {
            (void) fprintf(stderr, "qemu: unknown option '%s'\n", r);
            ExitProcess(EXIT_FAILURE);
        }
    }

    if (optind >= argc) {
        (void) fprintf(stderr, "qemu: no user program specified\n");
        ExitProcess(EXIT_FAILURE);
    }

    filename = argv[optind];
    exec_path = argv[optind];

    return optind;
}

static BOOL build_command_line(char **argv, RTL_USER_PROCESS_PARAMETERS* rupp)
{
    int len;
    char **arg;
    LPWSTR p;

    if (rupp->CommandLine.Buffer) return TRUE; /* already got it from the server */

    len = 0;
    for (arg = argv; *arg; arg++)
    {
        BOOL has_space;
        int bcount;
        char* a;

        has_space=FALSE;
        bcount=0;
        a=*arg;
        if( !*a ) has_space=TRUE;
        while (*a!='\0') {
            if (*a=='\\') {
                bcount++;
            } else {
                if (*a==' ' || *a=='\t') {
                    has_space=TRUE;
                } else if (*a=='"') {
                    /* doubling of '\' preceding a '"',
                     * plus escaping of said '"'
                     */
                    len+=2*bcount+1;
                }
                bcount=0;
            }
            a++;
        }
        len+=(a-*arg)+1 /* for the separating space */;
        if (has_space)
            len+=2+bcount; /* for the quotes and doubling of '\' preceding the closing quote */
    }

    if (!(rupp->CommandLine.Buffer = RtlAllocateHeap( GetProcessHeap(), 0, len * sizeof(WCHAR))))
        return FALSE;

    p = rupp->CommandLine.Buffer;
    rupp->CommandLine.Length = (len - 1) * sizeof(WCHAR);
    rupp->CommandLine.MaximumLength = len * sizeof(WCHAR);
    for (arg = argv; *arg; arg++)
    {
        BOOL has_space,has_quote;
        WCHAR* a, *argW;
        int bcount;

        bcount = MultiByteToWideChar(CP_ACP, 0, *arg, -1, NULL, 0);
        argW = my_alloc(bcount * sizeof(*argW));
        MultiByteToWideChar(CP_ACP, 0, *arg, -1, argW, bcount);

        /* Check for quotes and spaces in this argument */
        has_space=has_quote=FALSE;
        a=argW;
        if( !*a ) has_space=TRUE;
        while (*a!='\0') {
            if (*a==' ' || *a=='\t') {
                has_space=TRUE;
                if (has_quote)
                    break;
            } else if (*a=='"') {
                has_quote=TRUE;
                if (has_space)
                    break;
            }
            a++;
        }

        /* Now transfer it to the command line */
        if (has_space)
            *p++='"';
        if (has_quote || has_space) {
            bcount=0;
            a=argW;
            while (*a!='\0') {
                if (*a=='\\') {
                    *p++=*a;
                    bcount++;
                } else {
                    if (*a=='"') {
                        int i;

                        /* Double all the '\\' preceding this '"', plus one */
                        for (i=0;i<=bcount;i++)
                            *p++='\\';
                        *p++='"';
                    } else {
                        *p++=*a;
                    }
                    bcount=0;
                }
                a++;
            }
        } else {
            WCHAR* x = argW;
            while ((*p=*x++)) p++;
        }
        if (has_space) {
            int i;

            /* Double all the '\' preceding the closing quote */
            for (i=0;i<bcount;i++)
                *p++='\\';
            *p++='"';
        }
        *p++=' ';
        my_free(argW);
    }
    if (p > rupp->CommandLine.Buffer)
        p--;  /* remove last space */
    *p = '\0';

    return TRUE;
}

static void init_process_params(char **argv, const char *filenme)
{
    WCHAR *cwd;
    DWORD size;
    static const WCHAR qemu_x86_64exeW[] = {'q','e','m','u','-','x','8','6','_','6','4','.','e','x','e', 0};

    /* FIXME: Wine allocates the string buffer right behind the process parameter structure. */
    build_command_line(argv, &process_params);
    guest_PEB.ProcessParameters = &process_params;
    guest_PEB.LdrData = &guest_ldr;
    guest_PEB.ProcessHeap = GetProcessHeap();

    /* FIXME: If no explicit title is given WindowTitle and ImagePathName are the same, except
     * that WindowTitle has the .so ending removed. This could be used for a more reliable check.
     *
     * Is there a way to catch a case where the title is deliberately set to "qemu-x86_64.exe"? */
    if (wcsstr(NtCurrentTeb()->Peb->ProcessParameters->WindowTitle.Buffer, qemu_x86_64exeW))
    {
        RtlCreateUnicodeStringFromAsciiz(&guest_PEB.ProcessParameters->WindowTitle, filename);
    }
    else
    {
        guest_PEB.ProcessParameters->WindowTitle = NtCurrentTeb()->Peb->ProcessParameters->WindowTitle;
    }

    /* The effect of this code is to inject the current working directory into the top of the DLL search
     * path. It will later be replaced by the directory where the .exe was loaded from. The \\qemu-x86_64.exe.so
     * part will be cut off by the loader. */
    size = GetCurrentDirectoryW(0, NULL);
    cwd = my_alloc((size + 16) * sizeof(*cwd));
    GetCurrentDirectoryW(size, cwd);
    cwd[size - 1] = '\\';
    cwd[size] = 0;
    strcatW(cwd, qemu_x86_64exeW);
    RtlInitUnicodeString(&guest_PEB.ProcessParameters->ImagePathName, cwd);

    guest_ldr.Length = sizeof(guest_ldr);
    guest_ldr.Initialized = TRUE;
    RtlInitializeBitMap( &guest_tls_bitmap, guest_PEB.TlsBitmapBits, sizeof(guest_PEB.TlsBitmapBits) * 8 );
    RtlInitializeBitMap( &guest_tls_expansion_bitmap, guest_PEB.TlsExpansionBitmapBits,
                         sizeof(guest_PEB.TlsExpansionBitmapBits) * 8 );
    RtlInitializeBitMap( &guest_fls_bitmap, guest_PEB.FlsBitmapBits, sizeof(guest_PEB.FlsBitmapBits) * 8 );
    RtlSetBits( guest_PEB.TlsBitmap, 0, 1 ); /* TLS index 0 is reserved and should be initialized to NULL. */
    RtlSetBits( guest_PEB.FlsBitmap, 0, 1 );
    InitializeListHead( &guest_PEB.FlsListHead );
    InitializeListHead( &guest_ldr.InLoadOrderModuleList );
    InitializeListHead( &guest_ldr.InMemoryOrderModuleList );
    InitializeListHead( &guest_ldr.InInitializationOrderModuleList );

    guest_PEB.ProcessParameters->CurrentDirectory = NtCurrentTeb()->Peb->ProcessParameters->CurrentDirectory;

    if (is_32_bit)
    {
        guest_PEB32 = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*guest_PEB32));
        process_params32 = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*process_params32));

        guest_PEB32->ProcessParameters = (ULONG_PTR)process_params32;
        guest_PEB32->ProcessHeap = (ULONG_PTR)GetProcessHeap();
        /* TODO: Loader data? Will be tricky as I have to make pe.c update 32 bit pointers. */

        /* FIXME: This may be broken if we're taking it from Wine's PEB. */
        process_params32->WindowTitle.Length = guest_PEB.ProcessParameters->WindowTitle.Length;
        process_params32->WindowTitle.MaximumLength = guest_PEB.ProcessParameters->WindowTitle.MaximumLength;
        process_params32->WindowTitle.Buffer = (ULONG_PTR)guest_PEB.ProcessParameters->WindowTitle.Buffer;

        process_params32->ImagePathName.Length = guest_PEB.ProcessParameters->ImagePathName.Length;
        process_params32->ImagePathName.MaximumLength = guest_PEB.ProcessParameters->ImagePathName.MaximumLength;
        process_params32->ImagePathName.Buffer = (ULONG_PTR)guest_PEB.ProcessParameters->ImagePathName.Buffer;

        /* TODO: Bitmaps. Needs to be mirrored in kernel32 functions too */

        process_params32->CurrentDirectory.Handle = (ULONG_PTR)guest_PEB.ProcessParameters->CurrentDirectory.Handle;
        process_params32->CurrentDirectory.DosPath.Length = guest_PEB.ProcessParameters->CurrentDirectory.DosPath.Length;
        process_params32->CurrentDirectory.DosPath.MaximumLength = guest_PEB.ProcessParameters->CurrentDirectory.DosPath.MaximumLength;
        process_params32->CurrentDirectory.DosPath.Buffer = (ULONG_PTR)my_alloc(process_params32->CurrentDirectory.DosPath.MaximumLength);

        memcpy((void *)(ULONG_PTR)process_params32->CurrentDirectory.DosPath.Buffer,
                guest_PEB.ProcessParameters->CurrentDirectory.DosPath.Buffer,
                guest_PEB.ProcessParameters->CurrentDirectory.DosPath.MaximumLength);
    }
}

/* After blocking the 64 bit address space the host stack has no room to grow. Reserve some
 * space now. */
static void growstack(void)
{
    volatile char blob[1048576*4];
    memset((char *)blob, 0xad, sizeof(blob));
}

static void block_address_space(void)
{
    volatile void * volatile map; /* clang does not understand the concept of deliberately running out of memory. */
    unsigned long size = 1UL << 63UL;
    void (* WINAPI p__wine_RtlSetFirewallHeap)(BOOL firewall);
    HMODULE ntdll = GetModuleHandleA("ntdll");

    p__wine_RtlSetFirewallHeap = (void *)GetProcAddress(ntdll, "__wine_RtlSetFirewallHeap");

    /* mmap as much as possible. */
    while(size >= 4096)
    {
        do
        {
            map = mmap(NULL, size, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
        } while(map != (void *)0xffffffffffffffff);
        size >>= 1;
    }

    /* Wine may have pre-reserved areas that we can't catch with mmap, e.g. on x86_64 Linux hosts. */
    size = 1UL << 63UL;
    while(size >= 4096)
    {
        do
        {
            map = VirtualAlloc(0, size, MEM_RESERVE, PAGE_NOACCESS);
        } while(map);
        size >>= 1;
    }

    /* It appears that the heap manager has a few pages we can't mmap, but malloc will successfully
     * allocate from. On my system this gives me about 140kb of memory.
     *
     * Trying to malloc without virtual memory available hangs on OSX. We can happily skip it there
     * because the default malloc zone is placed below 4GB anyway.
     *
     * FIXME: Re-test if this is because clang optimized the malloc call away like it does on Linux. */
#ifndef __APPLE__
    size = 1UL << 63UL;
    while(size)
    {
        do
        {
            map = malloc(size);
        } while(map);
        size >>= 1;
    }
#endif

    /* Same for Wine's heap. */
    size = 1UL << 63UL;
    while(size)
    {
        do
        {
            map = my_alloc(size);
        } while(map);
        size >>= 1;
    }

    if(p__wine_RtlSetFirewallHeap)
        p__wine_RtlSetFirewallHeap(TRUE);
    else
        WINE_ERR("__wine_RtlSetFirewallHeap not found, expect problems.\n");

}

static char virtualalloc_blocked[0x100000000 / 0x10000];
static char mmap_blocked[0x100000000 / 0x1000];
static BOOL upper2gb;

static void fill_4g_holes(void)
{
    unsigned int i;

    /* The upper 2 GB are usually free. Try to allocate it in one go and reduce the loop iterations if possibe. */
    if (VirtualAlloc((void *)(ULONG_PTR)0x80000000, 0x80000000, MEM_RESERVE, PAGE_NOACCESS))
        upper2gb = TRUE;

    /* Try to allocate with both VirtualAlloc and mmap.
     *
     * The MacOS loader reserves some areas with sections in the main executable. They are not available for
     * mmap because it might break the Mac dynamic loader, but Wine will happily remap them with VirtualAlloc.
     *
     * However, VirtualAlloc can only allocate on a 16kb granularity, so we'd leave some holes around
     * things that are already mapped, like ntdll. Those holes would be filled by the address space blocking code
     * later. Fill them with mmap and free them after the blocking code.
     *
     * FIXME: This process is quite slow, especially the mmap part. See if there are ways to speed it up. */
    for (i = 1; i < ARRAY_SIZE(virtualalloc_blocked); i++)
    {
        if (upper2gb && (i * 0x10000) >= 0x80000000)
            break;

        if (VirtualAlloc((void *)(ULONG_PTR)(i * 0x10000), 0x10000, MEM_RESERVE, PAGE_NOACCESS))
            virtualalloc_blocked[i] = TRUE;
    }

    for (i = 1; i < ARRAY_SIZE(mmap_blocked); i++)
    {
        void *addr;

        if (upper2gb && (i * 0x1000) >= 0x80000000)
            break;

        addr = mmap((void *)(ULONG_PTR)(i * 0x1000), 0x1000,
                PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);

        if (addr == (void *)(ULONG_PTR)(i * 0x1000))
            mmap_blocked[i] = TRUE;
        else if (addr != (void *)0xffffffffffffffff)
            munmap(addr, 0x1000);
    }
}

static void free_4g_holes(BOOL free_high_2_gb)
{
    unsigned int i, end;

    end = ARRAY_SIZE(virtualalloc_blocked) / 2;
    if (free_high_2_gb)
    {
        if (upper2gb)
            VirtualFree((void *)(ULONG_PTR)0x80000000, 0, MEM_RELEASE);
        else
            end = ARRAY_SIZE(virtualalloc_blocked);
    }

    for (i = 0; i < end; i++)
    {
        if (virtualalloc_blocked[i])
            VirtualFree((void *)(ULONG_PTR)(i * 0x10000), 0, MEM_RELEASE);
    }

    if (free_high_2_gb)
        end = ARRAY_SIZE(mmap_blocked);
    else
        end = ARRAY_SIZE(mmap_blocked) / 2;

    for (i = 0; i < end; i++)
    {
        if (mmap_blocked[i])
            munmap((void *)(ULONG_PTR)(i * 0x1000), 0x1000);
    }
}

static void hook(void *to_hook, const void *replace)
{
    DWORD old_protect;
    size_t offset;
#ifdef __aarch64__
    struct hooked_function
    {
        DWORD ldr, br;
        const void *dst;
    } *hooked_function = to_hook;

    if(!VirtualProtect(hooked_function, sizeof(*hooked_function), PAGE_EXECUTE_READWRITE, &old_protect))
        fprintf(stderr, "Failed to make hooked function writeable.\n");

    offset = offsetof(struct hooked_function, dst) - offsetof(struct hooked_function, ldr);
    hooked_function->ldr = 0x58000005 | (offset << 3);   /* ldr x5, offset */;
    hooked_function->br = 0xd61f00a0; /* br x5 */;
    hooked_function->dst = replace;

    __clear_cache(hooked_function, (char *)hooked_function + sizeof(*hooked_function));
#elif defined(__x86_64__)
    struct hooked_function
    {
        char jmp[8];
        const void *dst;
    } *hooked_function = to_hook;

    if(!VirtualProtect(hooked_function, sizeof(*hooked_function), PAGE_EXECUTE_READWRITE, &old_protect))
        fprintf(stderr, "Failed to make hooked function writeable.\n");

    /* The offset is from the end of the jmp instruction (6 bytes) to the start of the destination. */
    offset = offsetof(struct hooked_function, dst) - offsetof(struct hooked_function, jmp) - 0x6;

    /* jmp *(rip + offset) */
    hooked_function->jmp[0] = 0xff;
    hooked_function->jmp[1] = 0x25;
    hooked_function->jmp[2] = offset;
    hooked_function->jmp[3] = 0x00;
    hooked_function->jmp[4] = 0x00;
    hooked_function->jmp[5] = 0x00;
    /* Filler */
    hooked_function->jmp[6] = 0xcc;
    hooked_function->jmp[7] = 0xcc;
    /* Dest address absolute */
    hooked_function->dst = replace;
#else
#error Implement hooks for your platform
#endif

    VirtualProtect(hooked_function, sizeof(*hooked_function), old_protect, &old_protect);
}

int main(int argc, char **argv, char **envp)
{
    HMODULE exe_module, user_module;
    int optind, i;
    WCHAR *filenameW, exename[MAX_PATH], *selfpath = NULL;
    BOOL large_address_aware;
    DWORD_PTR image_base, image_size;
    void *reserved;
    int ret;
    LDR_DATA_TABLE_ENTRY *self_module;
    ULONG_PTR magic;
    NTSTATUS nts;
    ANSI_STRING ansi;
    static const char *crts[] = {"msvcr100", "msvcr110", "msvcr120", "msvcr120_app", "msvcr70", "msvcr71",
            "msvcr80", "msvcr90", "msvcrt", "msvcrt20", "msvcrt40", "msvcrtd", "ucrtbase"};

    /* FIXME: The order of operations is a mess, especially setting up the TEB and loading the
     * guest binary. */

    fill_4g_holes();

    parallel_cpus = true;

    optind = parse_args(argc, argv);
    /* This is kinda dirty. It is too late to have an effect on kernel32 initialization,
     * but it should work OK for msvcrt because qemu doesn't link against msvcrt and the
     * library is not yet loaded. Turns out this is exactly what we want, but that's
     * more of a lucky coincidence than by design.
     *
     * It would be a bit more reliable if we added the offset before returning it to the
     * app, but msvcrt's getmainargs() has an option to expand wildcards, which makes
     * everything unpredictable. */
    WINE_TRACE("Fixing up command line\n");
    NtCurrentTeb()->Peb->ProcessParameters->CommandLine.Buffer = NULL;
    build_command_line(argv + optind, NtCurrentTeb()->Peb->ProcessParameters);
    RtlUnicodeStringToAnsiString( &ansi, &NtCurrentTeb()->Peb->ProcessParameters->CommandLine, TRUE );
    strcpy(GetCommandLineA(), ansi.Buffer);
    RtlFreeAnsiString(&ansi);
    WINE_TRACE("Done fixing up cmdline\n");

    for (i = 0; i < ARRAY_SIZE(crts); ++i)
    {
        if (GetModuleHandleA(crts[i]))
            WINE_ERR("%s loaded too soon.\n", crts[i]);
    }

    i = MultiByteToWideChar(CP_ACP, 0, filename, -1, NULL, 0) + 4;
    filenameW = my_alloc(i * sizeof(*filenameW));
    MultiByteToWideChar(CP_ACP, 0, filename, -1, filenameW, i);

    if (!strstr(filename, "."))
    {
        static const WCHAR exe[] = {'.','e','x','e',0};
        strcatW(filenameW, exe);
    }

    module_call_init(MODULE_INIT_TRACE);
    qemu_init_cpu_list();
    module_call_init(MODULE_INIT_QOM);

    tcg_exec_init(0);
    tcg_prologue_init(tcg_ctx);
    tcg_region_init();
    init_thread_cpu();
    qemu_loader_thread_init();

    i = MAX_PATH;
    do
    {
        HeapFree(GetProcessHeap(), 0, selfpath);
        i *= 2;
        selfpath = HeapAlloc(GetProcessHeap(), 0, i * sizeof(*selfpath));
        SetLastError(0);
        GetModuleFileNameW(NULL, selfpath, i);
    } while(GetLastError());
    qemu_pathname = selfpath;

    init_process_params(argv + optind, filename);

    if (!qemu_get_exe_properties(filenameW, exename, sizeof(exename) / sizeof(*exename), &is_32_bit,
            &large_address_aware, &image_base, &image_size))
    {
        fprintf(stderr, "Failed to load \"%s\", last error %u.\n", filename, GetLastError());
        ExitProcess(EXIT_FAILURE);
    }

    LdrLockLoaderLock( 0, NULL, &magic );
    nts = LdrFindEntryForAddress( NtCurrentTeb()->Peb->ImageBaseAddress, &self_module );
    if (nts)
        fprintf(stderr, "Could not find myself.\n");
    self_module->FullDllName.Buffer = exename;
    self_module->FullDllName.Length = strlenW(exename) * sizeof(*exename);
    LdrUnlockLoaderLock( 0, magic );

    user_module = LoadLibraryA("user32.dll");
    if (!user_module)
    {
        fprintf(stderr, "Cannot load user32.dll\n");
        ExitProcess(1);
    }
    p_CharNextA = (void *)GetProcAddress(user_module, "CharNextA");
    if (!p_CharNextA)
    {
        fprintf(stderr, "CharNextA not found in user32.dll\n");
        ExitProcess(1);
    }

    if (!load_host_dlls())
    {
        fprintf(stderr, "Failed to load host DLLs\n");
        ExitProcess(EXIT_FAILURE);
    }

    if (is_32_bit)
    {
        RemoveEntryList( &guest_teb->TlsLinks );
        free_teb(guest_teb, guest_teb32);
        memset(&guest_PEB, 0, sizeof(guest_PEB));
        my_free(process_params.CommandLine.Buffer);
        memset(&process_params, 0, sizeof(process_params));

        if ((ULONG_PTR)&i >= 0x100000000)
        {
            /* This will cause troubles whenever a callback function receives a pointer to a variable on the stack. */
            fprintf(stderr, "The host stack is above 4GB. This will likely cause isues.\n");
            /* Make sure the stack has some room before we block the address space above 4 GB. */
            growstack();
        }
        block_address_space();
    }

    free_4g_holes(large_address_aware);

    /* Make sure our TEB and Stack don't accidentally block the exe file. */
    reserved = VirtualAlloc((void *)image_base, image_size, MEM_RESERVE, PAGE_NOACCESS);
    if (!reserved)
        fprintf(stderr, "Could not reserve address for main image, expect trouble later\n");

    if (is_32_bit)
    {
        /* Re-init the CPU with (hopefully) 32 bit pointers. */

        /* Need a heap handle < 2^32. */
        if ((ULONG_PTR)NtCurrentTeb()->Peb->ProcessHeap >= (1ULL << 32ULL))
        {
            /* This will make HeapFree on existing allocs fail (not much of an issue),
             * but it will also make HeapReAlloc calls fail, which causes crashes in the
             * font code. */
            fprintf(stderr, "Warning: Swapping process heap. This can cause problems.\n");
            NtCurrentTeb()->Peb->ProcessHeap = HeapCreate(HEAP_GROWABLE, 0, 0);
        }

        init_process_params(argv + optind, filename);
        init_thread_cpu();
        qemu_loader_thread_init();
        fprintf(stderr, "32 bit environment set up, Large Address Aware: %s.\n", large_address_aware ? "YES" : "NO");
    }

    hook(GetProcAddress(GetModuleHandleA("ntdll"), "LdrFindEntryForAddress"), hook_LdrFindEntryForAddress);

    VirtualFree(reserved, 0, MEM_RELEASE);
    exe_module = qemu_LoadLibrary(filenameW, 0);
    my_free(filenameW);
    if (!exe_module)
    {
        fprintf(stderr, "Failed to load \"%s\", last error %u.\n", filename, GetLastError());
        ExitProcess(EXIT_FAILURE);
    }
    qemu_get_image_info(exe_module, &image);
    guest_PEB.ImageBaseAddress = exe_module;
    if (guest_PEB32)
        guest_PEB32->ImageBaseAddress = QEMU_H2G(exe_module);

    if (image.stack_reserve != DEFAULT_STACK_SIZE)
    {
        void *stack;
        CPUX86State *env = thread_cpu->env_ptr;

        VirtualFree(guest_teb->Tib.StackLimit, 0, MEM_RELEASE);
        stack = VirtualAlloc(NULL, image.stack_reserve, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!stack)
        {
            fprintf(stderr, "Could not reserve stack space size %u.\n", image.stack_reserve);
            ExitProcess(EXIT_FAILURE);
        }

        /* Stack grows down, so point to the end of the allocation. */
        env->regs[R_ESP] = h2g(stack) + image.stack_reserve;
        guest_teb->Tib.StackBase = (void *)(h2g(stack) + image.stack_reserve);
        guest_teb->Tib.StackLimit = (void *)h2g(stack);
        if (guest_teb32)
        {
            guest_teb32->Tib.StackBase = (qemu_ptr)(h2g(stack) + image.stack_reserve);
            guest_teb32->Tib.StackLimit = (qemu_ptr)h2g(stack);
        }
    }

    signal_init();

    WINE_TRACE("CPU Setup done\n");

    if (qemu_LdrInitializeThunk())
    {
        fprintf(stderr, "Process initialization failed.\n");
        ExitProcess(EXIT_FAILURE);
    }
    WINE_TRACE("Process init done.\n");

    /* Should not return, guest_call_entry calls ExitProcess if need be. */
    ret = qemu_execute(QEMU_G2H(guest_call_entry), QEMU_H2G(image.entrypoint));

    fprintf(stderr, "Main function returned, result %u.\n", ret);
    return ret;
}

BOOL qemu_DllMain(DWORD reason, void *reserved)
{
    WINE_TRACE("qemu DllMain(%u).\n", reason);

    if (reason == DLL_THREAD_DETACH && thread_cpu)
    {
        CPUX86State *env;

        cpu_list_lock();

        WINE_TRACE("Informing rcu about disappearing thread.\n");
        qemu_loader_thread_stop();

        env = thread_cpu->env_ptr;
        my_free(g2h(env->idt.base));
        my_free(g2h(env->gdt.base));

        QTAILQ_REMOVE(&cpus, thread_cpu, node);
        object_unref(OBJECT(thread_cpu));

        free_teb(guest_teb, guest_teb32);
        thread_cpu = NULL;

        rcu_unregister_thread();
        cpu_list_unlock();
    }

    return TRUE;
}

static void cpu_context_32_to_env(CPUX86State *env, const struct qemu_CONTEXT_X86 *context)
{
    /* FIXME: Do I really want .selector? I'm not entirely sure how those segment regs work. */
    if (context->ContextFlags & QEMU_CONTEXT_SEGMENTS)
    {
        env->segs[R_DS].selector = context->SegDs;
        env->segs[R_ES].selector = context->SegEs;
        env->segs[R_FS].selector = context->SegFs;
        env->segs[R_GS].selector = context->SegGs;
    }


    if (context->ContextFlags & QEMU_CONTEXT_DEBUG_REGISTERS)
    {
        env->dr[0] = context->Dr0;
        env->dr[1] = context->Dr1;
        env->dr[2] = context->Dr2;
        env->dr[3] = context->Dr3;
        env->dr[6] = context->Dr6;
        env->dr[7] = context->Dr7;
    }

    if (context->ContextFlags & CONTEXT_INTEGER)
    {
        env->regs[R_EAX] = context->Eax;
        env->regs[R_EBX] = context->Ebx;
        env->regs[R_ECX] = context->Ecx;
        env->regs[R_EDX] = context->Edx;
        env->regs[R_ESI] = context->Esi;
        env->regs[R_EDI] = context->Edi;
    }

    if (context->ContextFlags & QEMU_CONTEXT_CONTROL)
    {
        env->eip = context->Eip;
        env->segs[R_CS].selector = context->SegCs;
        env->regs[R_ESP] = context->Esp;
        env->regs[R_EBP] = context->Ebp;
        env->segs[R_SS].selector = context->SegSs;
        env->eflags = context->EFlags;
    }

    /* TODO: Floating point. */
    /* TODO: What is ExtendedRegisters? Seems to belong to float and contain stuff like MMX. */
}

NTSTATUS qemu_set_context(HANDLE thread, void *context)
{
    if (thread != GetCurrentThread())
    {
        /* Finding the right env pointer to manipulate shouldn't be too hard.
         * In addition we need a way to stop qemu from working if it is running,
         * (I guess EXCP_INTERRUPT should help here). If qemu is not running and
         * we are in a Windows API call we need to set the host context to get
         * out of the call. */
        fprintf(stderr, "Not supported on foreign thread yet.\n");
        return STATUS_UNSUCCESSFUL;
    }

    /* We are either in the NtSetContextThread syscall, which will return gracefully to
     * qemu's main loop after we return here, or we are in an exception handler. In the
     * latter case qemu_NtSetContextThread() will think it is returning to the guest
     * side NtSetContextThread, but in fact it just resumes execution whereever the new
     * context sets it to.
     *
     * If we're in a system call the next return from the guest to a host callback should
     * sort things out with the longjmp detection in qemu_execute(). */
    if (is_32_bit)
    {
        CPUX86State *env = thread_cpu->env_ptr;
        cpu_context_32_to_env(env, context);
    }
    else
    {
        /* Currently we have guest side code that doesn't set the debug registers... */
        fprintf(stderr, "Not implemented for 64 bit!\n");
        return STATUS_UNSUCCESSFUL;
    }

    return STATUS_SUCCESS;
}

/* copypasted from shlwapi. We can't load shlwapi.dll because it will load host shell32.dll, which will register
 * its window classes, so the shell32.dll inside the guest can't register them any more. */
BOOL my_PathRemoveFileSpecA(LPSTR lpszPath)
{
  LPSTR lpszFileSpec = lpszPath;
  BOOL bModified = FALSE;

  if(lpszPath)
  {
    /* Skip directory or UNC path */
    if (*lpszPath == '\\')
      lpszFileSpec = ++lpszPath;
    if (*lpszPath == '\\')
      lpszFileSpec = ++lpszPath;

    while (*lpszPath)
    {
      if(*lpszPath == '\\')
        lpszFileSpec = lpszPath; /* Skip dir */
      else if(*lpszPath == ':')
      {
        lpszFileSpec = ++lpszPath; /* Skip drive */
        if (*lpszPath == '\\')
          lpszFileSpec++;
      }
      if (!(lpszPath = p_CharNextA(lpszPath)))
        break;
    }

    if (*lpszFileSpec)
    {
      *lpszFileSpec = '\0';
      bModified = TRUE;
    }
  }
  return bModified;
}

/*************************************************************************
 * PathRemoveFileSpecW	[SHLWAPI.@]
 *
 * See PathRemoveFileSpecA.
 */
BOOL my_PathRemoveFileSpecW(LPWSTR lpszPath)
{
  LPWSTR lpszFileSpec = lpszPath;
  BOOL bModified = FALSE;

  if(lpszPath)
  {
    /* Skip directory or UNC path */
    if (*lpszPath == '\\')
      lpszFileSpec = ++lpszPath;
    if (*lpszPath == '\\')
      lpszFileSpec = ++lpszPath;

    while (*lpszPath)
    {
      if(*lpszPath == '\\')
        lpszFileSpec = lpszPath; /* Skip dir */
      else if(*lpszPath == ':')
      {
        lpszFileSpec = ++lpszPath; /* Skip drive */
        if (*lpszPath == '\\')
          lpszFileSpec++;
      }
      lpszPath++;
    }

    if (*lpszFileSpec)
    {
      *lpszFileSpec = '\0';
      bModified = TRUE;
    }
  }
  return bModified;
}
