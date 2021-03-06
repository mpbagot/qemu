/*
 * Loader functions
 *
 * Copyright 1995, 2003 Alexandre Julliard
 * Copyright 2002 Dmitry Timoshkov for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */
#include "config.h"

#include <ntstatus.h>

#define WIN32_NO_STATUS
#define NONAMELESSUNION
#include "qemu/osdep.h"
#include "qemu-version.h"

#include <wine/port.h>
#include <wine/library.h>
#include <wine/debug.h>
#include <wine/unicode.h>
#include <wine/exception.h>
#include <winnt.h>
#include <winternl.h>
#include <delayloadhandler.h>

#include "qapi/error.h"
#include "qemu.h"
#include "qemu/path.h"
#include "qemu/config-file.h"
#include "qemu/cutils.h"
#include "qemu/help_option.h"
#include "cpu.h"
#include "exec/exec-all.h"
#include "qemu/timer.h"
#include "qemu/envlist.h"
#include "exec/log.h"
#include "trace/control.h"
#include "glib-compat.h"
#include "translate-all.h"

#include "win_syscall.h"
#include "pe.h"

WINE_DEFAULT_DEBUG_CHANNEL(qemu_module);

/* ntdll string exports. The ones from libwine_port.a are not reliably re-exported
 * by libwine (e.g. not on MacOS toolchains) and I couldn't get linking to this
 * file working reliably either. I suppose linking to msvcrt/ucrtbase and using
 * their WCHAR functions would work, but I want to avoid msvcrt dependencies in
 * qemu to delay its load. */
extern int __cdecl _wcsicmp( LPCWSTR str1, LPCWSTR str2 );
extern int __cdecl _wcsnicmp( LPCWSTR str1, LPCWSTR str2, size_t n );
extern LPWSTR __cdecl wcsstr( LPCWSTR str, LPCWSTR sub );
extern ULONG __cdecl wcstoul(LPCWSTR s, LPWSTR *end, INT base);

#undef IMAGE_SNAP_BY_ORDINAL
#define IMAGE_SNAP_BY_ORDINAL(a) (is_32_bit ? IMAGE_SNAP_BY_ORDINAL32(a) : IMAGE_SNAP_BY_ORDINAL64(a))

struct nt_header
{
    DWORD Signature;
    IMAGE_FILE_HEADER FileHeader;
    union
    {
        IMAGE_OPTIONAL_HEADER32 hdr32;
        IMAGE_OPTIONAL_HEADER64 hdr64;
    } opt;
};

enum loadorder
{
    LO_INVALID,
    LO_DISABLED,
    LO_NATIVE,
    LO_BUILTIN,
    LO_NATIVE_BUILTIN,  /* native then builtin */
    LO_BUILTIN_NATIVE,  /* builtin then native */
    LO_DEFAULT          /* nothing specified, use default strategy */
};

static enum loadorder get_load_order( const WCHAR *app_name, const WCHAR *path )
{
    return LO_NATIVE;
}

static LPCSTR debugstr_us( const UNICODE_STRING *us )
{
    if (!us) return "<null>";
    return wine_dbgstr_wn(us->Buffer, us->Length / sizeof(WCHAR));
}

#define DEFAULT_SECURITY_COOKIE_64  (((ULONGLONG)0x00002b99 << 32) | 0x2ddfa232)
#define DEFAULT_SECURITY_COOKIE_32  0xbb40e64e
#define DEFAULT_SECURITY_COOKIE_16  (DEFAULT_SECURITY_COOKIE_32 >> 16)

typedef DWORD (CALLBACK *DLLENTRYPROC)(HMODULE,DWORD,LPVOID);
typedef void  (CALLBACK *LDRENUMPROC)(LDR_DATA_TABLE_ENTRY *, void *, BOOLEAN *);

static BOOL imports_fixup_done = FALSE;  /* set once the imports have been fixed up, before attaching them */
static BOOL process_detaching = FALSE;  /* set on process detach to avoid deadlocks with thread detach */
static int free_lib_count;   /* recursion depth of LdrUnloadDll calls */

static const char * const reason_names[] =
{
    "PROCESS_DETACH",
    "PROCESS_ATTACH",
    "THREAD_ATTACH",
    "THREAD_DETACH",
    NULL, NULL, NULL, NULL,
    "WINE_PREATTACH"
};

static const WCHAR dllW[] = {'.','d','l','l',0};
static const WCHAR ntdllW[]    = {'n','t','d','l','l','.','d','l','l',0};
static const WCHAR kernel32W[] = {'k','e','r','n','e','l','3','2','.','d','l','l',0};
static const WCHAR user32W[] = {'u','s','e','r','3','2','.','d','l','l',0};

/* internal representation of 32bit modules. per process. */
typedef struct _wine_modref
{
    LDR_DATA_TABLE_ENTRY            ldr;
    int                   nDeps;
    struct _wine_modref **deps;
} WINE_MODREF;

static HANDLE main_exe_file;
static UINT tls_module_count;      /* number of modules with TLS directory */
static IMAGE_TLS_DIRECTORY *tls_dirs;  /* array of TLS directories */
LIST_ENTRY tls_links = { &tls_links, &tls_links };

static CRITICAL_SECTION dlldir_section = { NULL, -1, 0, 0, 0, 0 };
static WCHAR *dll_directory;  /* extra path for SetDllDirectoryW */

static WINE_MODREF *cached_modref;
static WINE_MODREF *current_modref;
static WINE_MODREF *last_failed_modref;

static NTSTATUS load_dll( LPCWSTR load_path, LPCWSTR libname, DWORD flags, WINE_MODREF** pwm );
static NTSTATUS process_attach( WINE_MODREF *wm, LPVOID lpReserved );
static FARPROC find_ordinal_export( HMODULE module, const IMAGE_EXPORT_DIRECTORY *exports,
                                    DWORD exp_size, DWORD ordinal, LPCWSTR load_path, const char *fwd_name );
static FARPROC find_named_export( HMODULE module, const IMAGE_EXPORT_DIRECTORY *exports,
                                  DWORD exp_size, const char *name, int hint, LPCWSTR load_path );

static NTSTATUS qemu_LdrUnloadDll( HMODULE hModule );
static WCHAR *MODULE_get_dll_load_path(const WCHAR *module);

/* convert PE image VirtualAddress to Real Address */
static inline void *get_rva( HMODULE module, DWORD va )
{
    return (void *)((char *)module + va);
}

/* check whether the file name contains a path */
static inline BOOL contains_path( LPCWSTR name )
{
    return ((*name && (name[1] == ':')) || strchrW(name, '/') || strchrW(name, '\\'));
}

/* convert from straight ASCII to Unicode without depending on the current codepage */
static inline void ascii_to_unicode( WCHAR *dst, const char *src, size_t len )
{
    while (len--) *dst++ = (unsigned char)*src++;
}

static size_t page_mask = TARGET_PAGE_SIZE - 1;

#define ROUND_SIZE(size)  (((size) + page_mask) & ~page_mask)

/*************************************************************************
 *		call_dll_entry_point
 *
 * Some brain-damaged dlls (ir32_32.dll for instance) modify ebx in
 * their entry point, so we need a small asm wrapper. Testing indicates
 * that only modifying esi leads to a crash, so use this one to backup
 * ebp while running the dll entry proc.
 */
static inline BOOL call_dll_entry_point( DLLENTRYPROC proc, void *module,
                                         UINT reason, void *reserved )
{
    /* qemu_execute supports only one parameter, and we don't have a natural way to
     * compile guest code, so we have to dump the wrapper that unpacks the structure
     * here in bytecode form. I could of course extend qemu_execute to support more
     * params, but I am not convinced this would be easier, as it would still need
     * platform-specific handling. Note that the __fastcall passes the first argument
     * in ECX in 32 bit, which is similar to the Win64 convention.
     *
     * #include <windows.h>
     * #include <stdint.h>
     *
     * struct DllMain_call_data
     * {
     *     uint64_t func;
     *     uint64_t module;
     *     uint64_t reason;
     *     uint64_t reserved;
     * };
     *
     *  typedef DWORD (CALLBACK *DLLENTRYPROC)(HMODULE,DWORD,LPVOID);
     *
     *  uint64_t __fastcall call_init(const struct DllMain_call_data *f)
     *  {
     *      DLLENTRYPROC proc = (DLLENTRYPROC)f->func;
     *      return proc((HMODULE)f->module, f->reason, (void *)f->reserved);
     *  }
     */

    static char *wrapper;
    struct DllMain_call_data
    {
        uint64_t func;
        uint64_t module;
        uint64_t reason;
        uint64_t reserved;
    };

    struct DllMain_call_data call;

    if (!is_32_bit) {
      static const char x86_64_wrapper[] =
      {
          0x48, 0x83, 0xec, 0x28,     /* sub    $0x28,%rsp        */
          0x48, 0x89, 0xc8,           /* mov    %rcx,%rax         */
          0x48, 0x8b, 0x49, 0x08,     /* mov    0x8(%rcx),%rcx    */
          0x4c, 0x8b, 0x40, 0x18,     /* mov    0x18(%rax),%r8    */
          0x8b, 0x50, 0x10,           /* mov    0x10(%rax),%edx   */
          0xff, 0x10,                 /* callq  *(%rax)           */
          0x89, 0xc0,                 /* mov    %eax,%eax ???     */
          0x48, 0x83, 0xc4, 0x28,     /* add    $0x28,%rsp        */
          0xc3,                       /* retq                     */
      };
      /* Alloc it on the heap, qemu's static data is loaded > 4GB. */
      wrapper = my_alloc(sizeof(x86_64_wrapper));
      memcpy(wrapper, x86_64_wrapper, sizeof(x86_64_wrapper));
    } else {
      static const char i386_wrapper[] =
      {
          0x83, 0xec, 0x1c,           /* sub    $0x1c,%esp        */
          0x8b, 0x41, 0x18,           /* mov    0x18(%ecx),%eax   */
          0x89, 0x44, 0x24, 0x08,     /* mov    %eax,0x8(%esp)    */
          0x8b, 0x41, 0x10,           /* mov    0x10(%ecx),%eax   */
          0x89, 0x44, 0x24, 0x04,     /* mov    %eax,0x4(%esp)    */
          0x8b, 0x41, 0x08,           /* mov    0x8(%ecx),%eax    */
          0x89, 0x04, 0x24,           /* mov    %eax,(%esp)       */
          0xff, 0x11,                 /* call   *(%ecx)           */
          0x83, 0xec, 0x0c,           /* sub    $0xc,%esp         */
          0x31, 0xd2,                 /* xor    %edx,%edx         */
          0x83, 0xc4, 0x1c,           /* add    $0x1c,%esp        */
          0xc3,                       /* ret                      */
      };
      /* Alloc it on the heap, qemu's static data is loaded > 4GB. */
      wrapper = my_alloc(sizeof(i386_wrapper));
      memcpy(wrapper, i386_wrapper, sizeof(i386_wrapper));
    }

    call.func = QEMU_H2G(proc);
    call.module = (uint64_t)module;
    call.reason = reason;
    call.reserved = (uint64_t)reserved;

    return qemu_execute(wrapper, QEMU_H2G(&call));
}

/*************************************************************************
 *		get_modref
 *
 * Looks for the referenced HMODULE in the current process
 * The loader_section must be locked while calling this function.
 */
static WINE_MODREF *get_modref( HMODULE hmod )
{
    PLIST_ENTRY mark, entry;
    PLDR_DATA_TABLE_ENTRY mod;

    if (cached_modref && cached_modref->ldr.DllBase == hmod) return cached_modref;

    mark = &qemu_getTEB()->Peb->LdrData->InMemoryOrderModuleList;
    for (entry = mark->Flink; entry != mark; entry = entry->Flink)
    {
        mod = CONTAINING_RECORD(entry, LDR_DATA_TABLE_ENTRY, InMemoryOrderLinks);
        if (mod->DllBase == hmod)
            return cached_modref = CONTAINING_RECORD(mod, WINE_MODREF, ldr);
    }
    return NULL;
}


/**********************************************************************
 *	    find_basename_module
 *
 * Find a module from its base name.
 * The loader_section must be locked while calling this function
 */
static WINE_MODREF *find_basename_module( LPCWSTR name )
{
    PLIST_ENTRY mark, entry;

    if (cached_modref && !_wcsicmp( name, cached_modref->ldr.BaseDllName.Buffer ))
        return cached_modref;

    mark = &qemu_getTEB()->Peb->LdrData->InLoadOrderModuleList;
    for (entry = mark->Flink; entry != mark; entry = entry->Flink)
    {
        LDR_DATA_TABLE_ENTRY *mod = CONTAINING_RECORD(entry, LDR_DATA_TABLE_ENTRY, InLoadOrderLinks);
        if (!_wcsicmp( name, mod->BaseDllName.Buffer ))
        {
            cached_modref = CONTAINING_RECORD(mod, WINE_MODREF, ldr);
            return cached_modref;
        }
    }
    return NULL;
}


/**********************************************************************
 *	    find_fullname_module
 *
 * Find a module from its full path name.
 * The loader_section must be locked while calling this function
 */
static WINE_MODREF *find_fullname_module( LPCWSTR name )
{
    PLIST_ENTRY mark, entry;

    if (cached_modref && !_wcsicmp( name, cached_modref->ldr.FullDllName.Buffer ))
        return cached_modref;

    mark = &qemu_getTEB()->Peb->LdrData->InLoadOrderModuleList;
    for (entry = mark->Flink; entry != mark; entry = entry->Flink)
    {
        LDR_DATA_TABLE_ENTRY *mod = CONTAINING_RECORD(entry, LDR_DATA_TABLE_ENTRY, InLoadOrderLinks);
        if (!_wcsicmp( name, mod->FullDllName.Buffer ))
        {
            cached_modref = CONTAINING_RECORD(mod, WINE_MODREF, ldr);
            return cached_modref;
        }
    }
    return NULL;
}


/*************************************************************************
 *		find_forwarded_export
 *
 * Find the final function pointer for a forwarded function.
 * The loader_section must be locked while calling this function.
 */
static FARPROC find_forwarded_export( HMODULE module, const char *forward, LPCWSTR load_path, const char *export )
{
    const IMAGE_EXPORT_DIRECTORY *exports;
    DWORD exp_size;
    WINE_MODREF *wm;
    WCHAR mod_name[32];
    const char *end = strrchr(forward, '.');
    FARPROC proc = NULL;

    if (!end) return NULL;
    if ((end - forward) * sizeof(WCHAR) >= sizeof(mod_name)) return NULL;
    ascii_to_unicode( mod_name, forward, end - forward );
    mod_name[end - forward] = 0;
    if (!strchrW( mod_name, '.' ))
    {
        if ((end - forward) * sizeof(WCHAR) >= sizeof(mod_name) - sizeof(dllW)) return NULL;
        memcpy( mod_name + (end - forward), dllW, sizeof(dllW) );
    }

    if (!strcmp(end + 1, "__qemu_native_data__"))
    {
        module = GetModuleHandleW(mod_name);
        if (!module)
        {
            WINE_ERR("Host module %s not loaded.\n", wine_dbgstr_w(mod_name));
        }
        if (!export)
        {
            WINE_ERR("Missing native data export for ordinals.\n");
        }
        proc = GetProcAddress(module, export);
        WINE_TRACE("Forward to host %s.%s=%p\n", wine_dbgstr_w(mod_name), export, proc);
        return proc;
    }

    if (!(wm = find_basename_module( mod_name )))
    {
        WINE_TRACE( "delay loading %s for '%s'\n", wine_dbgstr_w(mod_name), forward );
        if (load_dll( load_path, mod_name, 0, &wm ) == STATUS_SUCCESS &&
            !(wm->ldr.Flags & LDR_DONT_RESOLVE_REFS))
        {
            if (!imports_fixup_done && current_modref)
            {
                WINE_MODREF **deps;
                if (current_modref->nDeps)
                    deps = RtlReAllocateHeap( GetProcessHeap(), 0, current_modref->deps,
                                              (current_modref->nDeps + 1) * sizeof(*deps) );
                else
                    deps = RtlAllocateHeap( GetProcessHeap(), 0, sizeof(*deps) );
                if (deps)
                {
                    deps[current_modref->nDeps++] = wm;
                    current_modref->deps = deps;
                }
            }
            else if (process_attach( wm, NULL ) != STATUS_SUCCESS)
            {
                qemu_LdrUnloadDll( wm->ldr.DllBase );
                wm = NULL;
            }
        }

        if (!wm)
        {
            WINE_ERR( "module not found for forward '%s' used by %s\n",
                 forward, wine_dbgstr_w(get_modref(module)->ldr.FullDllName.Buffer) );
            return NULL;
        }
    }
    if ((exports = RtlImageDirectoryEntryToData( wm->ldr.DllBase, TRUE,
                                                 IMAGE_DIRECTORY_ENTRY_EXPORT, &exp_size )))
    {
        const char *name = end + 1;
        if (*name == '#')  /* ordinal */
            proc = find_ordinal_export( wm->ldr.DllBase, exports, exp_size, atoi(name+1), load_path, NULL );
        else
            proc = find_named_export( wm->ldr.DllBase, exports, exp_size, name, -1, load_path );
    }

    if (!proc)
    {
        WINE_ERR("function not found for forward '%s' used by %s."
            " If you are using builtin %s, try using the native one instead.\n",
            forward, wine_dbgstr_w(get_modref(module)->ldr.FullDllName.Buffer),
            wine_dbgstr_w(get_modref(module)->ldr.BaseDllName.Buffer) );
    }
    return proc;
}


/*************************************************************************
 *		find_ordinal_export
 *
 * Find an exported function by ordinal.
 * The exports base must have been subtracted from the ordinal already.
 * The loader_section must be locked while calling this function.
 */
static FARPROC find_ordinal_export( HMODULE module, const IMAGE_EXPORT_DIRECTORY *exports,
                                    DWORD exp_size, DWORD ordinal, LPCWSTR load_path,
                                    const char *fwd_name)
{
    FARPROC proc;
    const DWORD *functions = get_rva( module, exports->AddressOfFunctions );

    if (ordinal >= exports->NumberOfFunctions)
    {
        WINE_TRACE("	ordinal %d out of range!\n", ordinal + exports->Base );
        return NULL;
    }
    if (!functions[ordinal]) return NULL;

    proc = get_rva( module, functions[ordinal] );

    /* if the address falls into the export dir, it's a forward */
    if (((const char *)proc >= (const char *)exports) && 
        ((const char *)proc < (const char *)exports + exp_size))
        return find_forwarded_export( module, (const char *)proc, load_path, fwd_name );

    return proc;
}


/*************************************************************************
 *		find_named_export
 *
 * Find an exported function by name.
 * The loader_section must be locked while calling this function.
 */
static FARPROC find_named_export( HMODULE module, const IMAGE_EXPORT_DIRECTORY *exports,
                                  DWORD exp_size, const char *name, int hint, LPCWSTR load_path )
{
    const WORD *ordinals = get_rva( module, exports->AddressOfNameOrdinals );
    const DWORD *names = get_rva( module, exports->AddressOfNames );
    int min = 0, max = exports->NumberOfNames - 1;

    /* first check the hint */
    if (hint >= 0 && hint <= max)
    {
        char *ename = get_rva( module, names[hint] );
        if (!strcmp( ename, name ))
            return find_ordinal_export( module, exports, exp_size, ordinals[hint], load_path, name );
    }

    /* then do a binary search */
    while (min <= max)
    {
        int res, pos = (min + max) / 2;
        char *ename = get_rva( module, names[pos] );
        if (!(res = strcmp( ename, name )))
            return find_ordinal_export( module, exports, exp_size, ordinals[pos], load_path, name );
        if (res > 0) max = pos - 1;
        else min = pos + 1;
    }

    return NULL;

}


/*************************************************************************
 *		import_dll
 *
 * Import the dll specified by the given import descriptor.
 * The loader_section must be locked while calling this function.
 */
static BOOL import_dll( HMODULE module, const IMAGE_IMPORT_DESCRIPTOR *descr, LPCWSTR load_path, WINE_MODREF **pwm )
{
    NTSTATUS status;
    WINE_MODREF *wmImp;
    HMODULE imp_mod;
    const IMAGE_EXPORT_DIRECTORY *exports;
    DWORD exp_size;
    const IMAGE_THUNK_DATA64 *import_list64;
    const IMAGE_THUNK_DATA32 *import_list32;
    IMAGE_THUNK_DATA64 *thunk_list64;
    IMAGE_THUNK_DATA32 *thunk_list32;
    WCHAR buffer[32];
    const char *name = get_rva( module, descr->Name );
    DWORD len = strlen(name);
    PVOID protect_base;
    SIZE_T protect_size = 0;
    DWORD protect_old;
    ULONGLONG orig_ordinal;

    thunk_list64 = get_rva( module, (DWORD)descr->FirstThunk );
    thunk_list32 = get_rva( module, (DWORD)descr->FirstThunk );
    if (descr->u.OriginalFirstThunk)
    {
        import_list64 = get_rva( module, (DWORD)descr->u.OriginalFirstThunk );
        import_list32 = get_rva( module, (DWORD)descr->u.OriginalFirstThunk );
    }
    else
    {
        import_list64 = thunk_list64;
        import_list32 = thunk_list32;
    }

    if (!(is_32_bit ? import_list32->u1.Ordinal: import_list64->u1.Ordinal))
    {
        WINE_WARN( "Skipping unused import %s\n", name );
        *pwm = NULL;
        return TRUE;
    }

    while (len && name[len-1] == ' ') len--;  /* remove trailing spaces */

    if (len * sizeof(WCHAR) < sizeof(buffer))
    {
        ascii_to_unicode( buffer, name, len );
        buffer[len] = 0;
        status = load_dll( load_path, buffer, 0, &wmImp );
    }
    else  /* need to allocate a larger buffer */
    {
        WCHAR *ptr = RtlAllocateHeap( GetProcessHeap(), 0, (len + 1) * sizeof(WCHAR) );
        if (!ptr) return FALSE;
        ascii_to_unicode( ptr, name, len );
        ptr[len] = 0;
        status = load_dll( load_path, ptr, 0, &wmImp );
        RtlFreeHeap( GetProcessHeap(), 0, ptr );
    }

    if (status)
    {
        if (status == STATUS_DLL_NOT_FOUND)
            WINE_ERR("Library %s (which is needed by %s) not found\n",
                name, wine_dbgstr_w(current_modref->ldr.FullDllName.Buffer));
        else
            WINE_ERR("Loading library %s (which is needed by %s) failed (error %x).\n",
                name, wine_dbgstr_w(current_modref->ldr.FullDllName.Buffer), status);
        return FALSE;
    }

    /* unprotect the import address table since it can be located in
     * readonly section */
    if (is_32_bit)
    {
        while (import_list32[protect_size].u1.Ordinal) protect_size++;
    }
    else
    {
        while (import_list64[protect_size].u1.Ordinal) protect_size++;
    }
    protect_base = thunk_list64;
    protect_size *= is_32_bit ? sizeof(*thunk_list32) : sizeof(*thunk_list64);
    NtProtectVirtualMemory( NtCurrentProcess(), &protect_base,
                            &protect_size, PAGE_READWRITE, &protect_old );

    imp_mod = wmImp->ldr.DllBase;
    exports = RtlImageDirectoryEntryToData( imp_mod, TRUE, IMAGE_DIRECTORY_ENTRY_EXPORT, &exp_size );

    if (!exports)
    {
        /* set all imported function to deadbeef */
        while ((orig_ordinal = is_32_bit ? import_list32->u1.Ordinal : import_list64->u1.Ordinal))
        {
            static const char dbgbreak[] = {0xcc};
            if (IMAGE_SNAP_BY_ORDINAL(orig_ordinal))
            {
                int ordinal = IMAGE_ORDINAL(orig_ordinal);
                WINE_ERR("No implementation for %s.%d", name, ordinal );
                if (is_32_bit)
                    thunk_list32->u1.Function = (ULONG_PTR)dbgbreak;
                else
                    thunk_list64->u1.Function = (ULONG_PTR)dbgbreak;
            }
            else
            {
                IMAGE_IMPORT_BY_NAME *pe_name;
                if (is_32_bit)
                    pe_name = get_rva( module, (DWORD)import_list32->u1.AddressOfData );
                else
                    pe_name = get_rva( module, (DWORD)import_list64->u1.AddressOfData );
                WINE_ERR("No implementation for %s.%s", name, pe_name->Name );
                if (is_32_bit)
                    thunk_list32->u1.Function = (ULONG_PTR)dbgbreak;
                else
                    thunk_list64->u1.Function = (ULONG_PTR)dbgbreak;
            }
            WINE_WARN(" imported from %s, allocating stub %p\n",
                 wine_dbgstr_w(current_modref->ldr.FullDllName.Buffer),
                 (void *)dbgbreak );
            import_list64++;
            thunk_list64++;
            import_list32++;
            thunk_list32++;
        }
        goto done;
    }

    while ((orig_ordinal = is_32_bit ? import_list32->u1.Ordinal : import_list64->u1.Ordinal))
    {
        ULONG_PTR func;
        static const char dbgbreak[] = {0xcc};
        if (IMAGE_SNAP_BY_ORDINAL(orig_ordinal))
        {
            int ordinal = IMAGE_ORDINAL(orig_ordinal);

            func = (ULONG_PTR)find_ordinal_export( imp_mod, exports, exp_size,
                                                                      ordinal - exports->Base, load_path, NULL );
            if (!func)
            {
                func = (ULONG_PTR)dbgbreak;
                WINE_FIXME("No implementation for %s.%d imported from %s, setting to %p\n",
                     name, ordinal, wine_dbgstr_w(current_modref->ldr.FullDllName.Buffer),
                     (void *)func );
            }
            WINE_TRACE("--- Ordinal %s.%d = %p\n", name, ordinal, (void *)func );
            if (is_32_bit)
                thunk_list32->u1.Function = func;
            else
                thunk_list64->u1.Function = func;
        }
        else  /* import by name */
        {
            IMAGE_IMPORT_BY_NAME *pe_name;
            if (is_32_bit)
                pe_name = get_rva( module, (DWORD)import_list32->u1.AddressOfData );
            else
                pe_name = get_rva( module, (DWORD)import_list64->u1.AddressOfData );
            func = (ULONG_PTR)find_named_export( imp_mod, exports, exp_size,
                                                                    (const char*)pe_name->Name,
                                                                    pe_name->Hint, load_path );
            if (!func)
            {
                func = (ULONG_PTR)dbgbreak;
                WINE_FIXME("No implementation for %s.%s imported from %s, setting to %p\n",
                     name, pe_name->Name, wine_dbgstr_w(current_modref->ldr.FullDllName.Buffer),
                     (void *)func );
            }
            WINE_TRACE("--- %s %s.%d = %p\n",
                            pe_name->Name, name, pe_name->Hint, (void *)func);
            if (is_32_bit)
                thunk_list32->u1.Function = func;
            else
                thunk_list64->u1.Function = func;
        }
        import_list64++;
        thunk_list64++;
        import_list32++;
        thunk_list32++;
    }

done:
    /* restore old protection of the import address table */
    NtProtectVirtualMemory( NtCurrentProcess(), &protect_base, &protect_size, protect_old, &protect_old );
    *pwm = wmImp;
    return TRUE;
}


/***********************************************************************
 *           create_module_activation_context
 */
static NTSTATUS create_module_activation_context( LDR_DATA_TABLE_ENTRY *module )
{
    NTSTATUS status;
    LDR_RESOURCE_INFO info;
    const IMAGE_RESOURCE_DATA_ENTRY *entry;

    info.Type = (ULONG_PTR)RT_MANIFEST;
    info.Name = (ULONG_PTR)ISOLATIONAWARE_MANIFEST_RESOURCE_ID;
    info.Language = 0;
    if (!(status = LdrFindResource_U( module->DllBase, &info, 3, &entry )))
    {
        ACTCTXW ctx;
        ctx.cbSize   = sizeof(ctx);
        ctx.lpSource = NULL;
        ctx.dwFlags  = ACTCTX_FLAG_RESOURCE_NAME_VALID | ACTCTX_FLAG_HMODULE_VALID;
        ctx.hModule  = module->DllBase;
        ctx.lpResourceName = (LPCWSTR)ISOLATIONAWARE_MANIFEST_RESOURCE_ID;
        status = RtlCreateActivationContext( &module->ActivationContext, &ctx );
    }
    return status;
}


/*************************************************************************
 *		is_dll_native_subsystem
 *
 * Check if dll is a proper native driver.
 * Some dlls (corpol.dll from IE6 for instance) are incorrectly marked as native
 * while being perfectly normal DLLs.  This heuristic should catch such breakages.
 */
static BOOL is_dll_native_subsystem( HMODULE module, const IMAGE_NT_HEADERS *nt, LPCWSTR filename )
{
    const IMAGE_IMPORT_DESCRIPTOR *imports;
    DWORD i, size;
    WCHAR buffer[16];

    if (nt->OptionalHeader.Subsystem != IMAGE_SUBSYSTEM_NATIVE) return FALSE;
    if (nt->OptionalHeader.SectionAlignment < TARGET_PAGE_SIZE) return TRUE;

    if ((imports = RtlImageDirectoryEntryToData( module, TRUE,
                                                 IMAGE_DIRECTORY_ENTRY_IMPORT, &size )))
    {
        for (i = 0; imports[i].Name; i++)
        {
            const char *name = get_rva( module, imports[i].Name );
            DWORD len = strlen(name);
            if (len * sizeof(WCHAR) >= sizeof(buffer)) continue;
            ascii_to_unicode( buffer, name, len + 1 );
            if (!_wcsicmp( buffer, ntdllW ) || !_wcsicmp( buffer, kernel32W ))
            {
                WINE_TRACE( "%s imports %s, assuming not native\n", wine_dbgstr_w(filename), wine_dbgstr_w(buffer) );
                return FALSE;
            }
        }
    }
    return TRUE;
}

/*************************************************************************
 *		alloc_tls_slot
 *
 * Allocate a TLS slot for a newly-loaded module.
 * The loader_section must be locked while calling this function.
 */
static SHORT alloc_tls_slot( LDR_DATA_TABLE_ENTRY *mod )
{
    IMAGE_TLS_DIRECTORY64 dir_copy = {0};
    const IMAGE_TLS_DIRECTORY64 *dir;
    const IMAGE_TLS_DIRECTORY32 *dir32;
    ULONG i, size;
    void *new_ptr;
    LIST_ENTRY *entry;

    if (is_32_bit)
    {
        if (!(dir32 = RtlImageDirectoryEntryToData( mod->DllBase, TRUE, IMAGE_DIRECTORY_ENTRY_TLS, &size )))
            return -1;
        dir_copy.StartAddressOfRawData = dir32->StartAddressOfRawData;
        dir_copy.EndAddressOfRawData = dir32->EndAddressOfRawData;
        dir_copy.AddressOfIndex = dir32->AddressOfIndex;
        dir_copy.AddressOfCallBacks = dir32->AddressOfCallBacks;
        dir_copy.SizeOfZeroFill = dir32->SizeOfZeroFill;
        dir_copy.Characteristics = dir32->Characteristics;
        dir = &dir_copy;
    }
    else
    {
        if (!(dir = RtlImageDirectoryEntryToData( mod->DllBase, TRUE, IMAGE_DIRECTORY_ENTRY_TLS, &size )))
            return -1;
    }

    size = dir->EndAddressOfRawData - dir->StartAddressOfRawData;
    if (!size && !dir->SizeOfZeroFill && !dir->AddressOfCallBacks) return -1;

    for (i = 0; i < tls_module_count; i++)
    {
        if (!tls_dirs[i].StartAddressOfRawData && !tls_dirs[i].EndAddressOfRawData &&
            !tls_dirs[i].SizeOfZeroFill && !tls_dirs[i].AddressOfCallBacks)
            break;
    }

    WINE_TRACE( "module %p data %p-%p zerofill %u index %p callback %p flags %x -> slot %u\n", mod->DllBase,
           (void *)dir->StartAddressOfRawData, (void *)dir->EndAddressOfRawData, dir->SizeOfZeroFill,
           (void *)dir->AddressOfIndex, (void *)dir->AddressOfCallBacks, dir->Characteristics, i );

    if (i == tls_module_count)
    {
        UINT new_count = max( 32, tls_module_count * 2 );

        if (!tls_dirs)
            new_ptr = RtlAllocateHeap( GetProcessHeap(), HEAP_ZERO_MEMORY, new_count * sizeof(*tls_dirs) );
        else
            new_ptr = RtlReAllocateHeap( GetProcessHeap(), HEAP_ZERO_MEMORY, tls_dirs,
                                         new_count * sizeof(*tls_dirs) );
        if (!new_ptr) return -1;

        /* resize the pointer block in all running threads */
        for (entry = tls_links.Flink; entry != &tls_links; entry = entry->Flink)
        {
            TEB *teb = CONTAINING_RECORD( entry, TEB, TlsLinks );
            TEB32 *teb32 = teb->glReserved2;

            if (teb32)
            {
                qemu_ptr *old = (qemu_ptr *)(ULONG_PTR)teb32->ThreadLocalStoragePointer;
                qemu_ptr *new = RtlAllocateHeap( GetProcessHeap(), HEAP_ZERO_MEMORY, new_count * sizeof(*new));

                if (!new) return -1;
                if (old) memcpy( new, old, tls_module_count * sizeof(*new) );
                teb32->ThreadLocalStoragePointer = (ULONG_PTR)new;
                WINE_TRACE( "thread %04lx tls block %p -> %p\n", (ULONG_PTR)teb32->ClientId.UniqueThread, old, new );
                /* FIXME: can't free old block here, should be freed at thread exit */
            }
            else
            {
                void **old = teb->ThreadLocalStoragePointer;
                void **new = RtlAllocateHeap( GetProcessHeap(), HEAP_ZERO_MEMORY, new_count * sizeof(*new));

                if (!new) return -1;
                if (old) memcpy( new, old, tls_module_count * sizeof(*new) );
                teb->ThreadLocalStoragePointer = new;
                WINE_TRACE( "thread %04lx tls block %p -> %p\n", (ULONG_PTR)teb->ClientId.UniqueThread, old, new );
                /* FIXME: can't free old block here, should be freed at thread exit */
            }
        }

        tls_dirs = new_ptr;
        tls_module_count = new_count;
    }

    /* allocate the data block in all running threads */
    for (entry = tls_links.Flink; entry != &tls_links; entry = entry->Flink)
    {
        TEB *teb = CONTAINING_RECORD( entry, TEB, TlsLinks );
        TEB32 *teb32 = teb->glReserved2;

        if (!(new_ptr = RtlAllocateHeap( GetProcessHeap(), 0, size + dir->SizeOfZeroFill ))) return -1;
        memcpy( new_ptr, (void *)dir->StartAddressOfRawData, size );
        memset( (char *)new_ptr + size, 0, dir->SizeOfZeroFill );

        WINE_TRACE( "thread %04lx slot %u: %u/%u bytes at %p\n",
               (ULONG_PTR)teb->ClientId.UniqueThread, i, size, dir->SizeOfZeroFill, new_ptr );

        if (teb32)
        {
            RtlFreeHeap( GetProcessHeap(), 0,
                    (void *)(ULONG_PTR)InterlockedExchange(((int *)(ULONG_PTR)teb32->ThreadLocalStoragePointer) + i, (ULONG_PTR)new_ptr ));
        }
        else
        {
            RtlFreeHeap( GetProcessHeap(), 0,
                    InterlockedExchangePointer( (void **)teb->ThreadLocalStoragePointer + i, new_ptr ));
        }
    }

    *(DWORD *)dir->AddressOfIndex = i;
    tls_dirs[i] = *dir;
    return i;
}


/*************************************************************************
 *		free_tls_slot
 *
 * Free the module TLS slot on unload.
 * The loader_section must be locked while calling this function.
 */
static void free_tls_slot( LDR_DATA_TABLE_ENTRY *mod )
{
    ULONG i = (USHORT)mod->TlsIndex;

    if (mod->TlsIndex == -1) return;
    assert( i < tls_module_count );
    memset( &tls_dirs[i], 0, sizeof(tls_dirs[i]) );
}


/****************************************************************
 *       fixup_imports
 *
 * Fixup all imports of a given module.
 * The loader_section must be locked while calling this function.
 */
static NTSTATUS fixup_imports( WINE_MODREF *wm, LPCWSTR load_path )
{
    int i, nb_imports;
    const IMAGE_IMPORT_DESCRIPTOR *imports;
    WINE_MODREF *prev, *imp;
    DWORD size;
    NTSTATUS status;
    ULONG_PTR cookie;

    if (!(wm->ldr.Flags & LDR_DONT_RESOLVE_REFS)) return STATUS_SUCCESS;  /* already done */
    wm->ldr.Flags &= ~LDR_DONT_RESOLVE_REFS;

    wm->ldr.TlsIndex = alloc_tls_slot( &wm->ldr );

    if (!(imports = RtlImageDirectoryEntryToData( wm->ldr.DllBase, TRUE,
                                                  IMAGE_DIRECTORY_ENTRY_IMPORT, &size )))
        return STATUS_SUCCESS;

    nb_imports = 0;
    while (imports[nb_imports].Name && imports[nb_imports].FirstThunk) nb_imports++;

    if (!nb_imports) return STATUS_SUCCESS;  /* no imports */

    if (!create_module_activation_context( &wm->ldr ))
        RtlActivateActivationContext( 0, wm->ldr.ActivationContext, &cookie );

    /* Allocate module dependency list */
    wm->nDeps = nb_imports;
    wm->deps  = RtlAllocateHeap( GetProcessHeap(), 0, nb_imports*sizeof(WINE_MODREF *) );

    /* load the imported modules. They are automatically
     * added to the modref list of the process.
     */
    prev = current_modref;
    current_modref = wm;
    status = STATUS_SUCCESS;
    for (i = 0; i < nb_imports; i++)
    {
        if (!import_dll( wm->ldr.DllBase, &imports[i], load_path, &imp ))
        {
            imp = NULL;
            status = STATUS_DLL_NOT_FOUND;
        }
        wm->deps[i] = imp;
    }
    current_modref = prev;
    if (wm->ldr.ActivationContext) RtlDeactivateActivationContext( 0, cookie );
    return status;
}


/*************************************************************************
 *		alloc_module
 *
 * Allocate a WINE_MODREF structure and add it to the process list
 * The loader_section must be locked while calling this function.
 */
static WINE_MODREF *alloc_module( HMODULE hModule, LPCWSTR filename )
{
    WINE_MODREF *wm;
    const WCHAR *p;
    const IMAGE_NT_HEADERS *nt = RtlImageNtHeader(hModule);

    if (!(wm = RtlAllocateHeap( GetProcessHeap(), 0, sizeof(*wm) ))) return NULL;

    wm->nDeps    = 0;
    wm->deps     = NULL;

    wm->ldr.DllBase   = hModule;
    wm->ldr.EntryPoint    = NULL;
    wm->ldr.SizeOfImage   = nt->OptionalHeader.SizeOfImage;
    wm->ldr.Flags         = LDR_DONT_RESOLVE_REFS;
    wm->ldr.TlsIndex      = -1;
    wm->ldr.LoadCount     = 1;
    wm->ldr.SectionHandle = NULL;
    wm->ldr.CheckSum      = 0;
    wm->ldr.TimeDateStamp = 0;
    wm->ldr.ActivationContext = 0;

    RtlCreateUnicodeString( &wm->ldr.FullDllName, filename );
    if ((p = strrchrW( wm->ldr.FullDllName.Buffer, '\\' ))) p++;
    else p = wm->ldr.FullDllName.Buffer;
    RtlInitUnicodeString( &wm->ldr.BaseDllName, p );

    if (!(nt->FileHeader.Characteristics & IMAGE_FILE_DLL) || !is_dll_native_subsystem( hModule, nt, p ))
    {
        if (nt->FileHeader.Characteristics & IMAGE_FILE_DLL)
            wm->ldr.Flags |= LDR_IMAGE_IS_DLL;
        if (nt->OptionalHeader.AddressOfEntryPoint)
            wm->ldr.EntryPoint = (char *)hModule + nt->OptionalHeader.AddressOfEntryPoint;
    }

    InsertTailList(&qemu_getTEB()->Peb->LdrData->InLoadOrderModuleList,
                   &wm->ldr.InLoadOrderLinks);
    InsertTailList(&qemu_getTEB()->Peb->LdrData->InMemoryOrderModuleList,
                   &wm->ldr.InMemoryOrderLinks);

    /* wait until init is called for inserting into this list */
    wm->ldr.InInitializationOrderLinks.Flink = NULL;
    wm->ldr.InInitializationOrderLinks.Blink = NULL;

    /* FIXME: This code gets triggered by our DLLs, and it disables NX on the host side.
     * Both things of that are wrong. Disable it for now, but it needs to be re-enabled
     * once the DLLs have proper flags and we have page execute permissions integrated in
     * qemu.
     *
     * Setting this on the host side mysteriously breaks creating windows on OSX, with
     * weird crashes deep inside OSX libraries. */
    if (0 && !(nt->OptionalHeader.DllCharacteristics & IMAGE_DLLCHARACTERISTICS_NX_COMPAT))
    {
        ULONG flags = MEM_EXECUTE_OPTION_ENABLE;
        WINE_WARN( "disabling no-exec because of %s\n", wine_dbgstr_w(wm->ldr.BaseDllName.Buffer) );
        NtSetInformationProcess( GetCurrentProcess(), ProcessExecuteFlags, &flags, sizeof(flags) );
    }
    return wm;
}


/*************************************************************************
 *              alloc_thread_tls
 *
 * Allocate the per-thread structure for module TLS storage.
 */
static NTSTATUS alloc_thread_tls(void)
{
    UINT i, size;
    TEB *teb = qemu_getTEB();
    TEB32 *teb32 = qemu_getTEB32();

    if (!tls_module_count) return STATUS_SUCCESS;

    if (teb32)
    {
        qemu_ptr *pointers;
        if (!(pointers = RtlAllocateHeap( GetProcessHeap(), HEAP_ZERO_MEMORY,
                                        tls_module_count * sizeof(*pointers) )))
            return STATUS_NO_MEMORY;

        for (i = 0; i < tls_module_count; i++)
        {
            /* Don't use IMAGE_TLS_DIRECTORY32 here, we stored the contents of the converted structure. */
            const IMAGE_TLS_DIRECTORY *dir = &tls_dirs[i];

            if (!dir) continue;
            size = dir->EndAddressOfRawData - dir->StartAddressOfRawData;
            if (!size && !dir->SizeOfZeroFill) continue;

            if (!(pointers[i] = (ULONG_PTR)RtlAllocateHeap( GetProcessHeap(), 0, size + dir->SizeOfZeroFill )))
            {
                while (i) RtlFreeHeap( GetProcessHeap(), 0, (void *)(ULONG_PTR)pointers[--i] );
                RtlFreeHeap( GetProcessHeap(), 0, pointers );
                return STATUS_NO_MEMORY;
            }
            memcpy( (void *)(ULONG_PTR)pointers[i], (void *)dir->StartAddressOfRawData, size );
            memset( ((char *)(ULONG_PTR)pointers[i]) + size, 0, dir->SizeOfZeroFill );

            WINE_TRACE( "thread %04x slot %u: %u/%u bytes at 0x%08x\n",
                GetCurrentThreadId(), i, size, dir->SizeOfZeroFill, pointers[i] );
        }
        teb32->ThreadLocalStoragePointer = (ULONG_PTR)pointers;
    }
    else
    {
        void **pointers;
        if (!(pointers = RtlAllocateHeap( GetProcessHeap(), HEAP_ZERO_MEMORY,
                                        tls_module_count * sizeof(*pointers) )))
            return STATUS_NO_MEMORY;

        for (i = 0; i < tls_module_count; i++)
        {
            const IMAGE_TLS_DIRECTORY *dir = &tls_dirs[i];

            if (!dir) continue;
            size = dir->EndAddressOfRawData - dir->StartAddressOfRawData;
            if (!size && !dir->SizeOfZeroFill) continue;

            if (!(pointers[i] = RtlAllocateHeap( GetProcessHeap(), 0, size + dir->SizeOfZeroFill )))
            {
                while (i) RtlFreeHeap( GetProcessHeap(), 0, pointers[--i] );
                RtlFreeHeap( GetProcessHeap(), 0, pointers );
                return STATUS_NO_MEMORY;
            }
            memcpy( pointers[i], (void *)dir->StartAddressOfRawData, size );
            memset( (char *)pointers[i] + size, 0, dir->SizeOfZeroFill );

            WINE_TRACE( "thread %04x slot %u: %u/%u bytes at %p\n",
                GetCurrentThreadId(), i, size, dir->SizeOfZeroFill, pointers[i] );
        }
        teb->ThreadLocalStoragePointer = pointers;
    }
    return STATUS_SUCCESS;
}


/*************************************************************************
 *              call_tls_callbacks
 */
static void call_tls_callbacks( HMODULE module, UINT reason )
{
    ULONG dirsize;

    if (is_32_bit)
    {
        const IMAGE_TLS_DIRECTORY32 *dir32;
        const qemu_ptr *callback_array;
        PIMAGE_TLS_CALLBACK callback;
        dir32 = RtlImageDirectoryEntryToData( module, TRUE, IMAGE_DIRECTORY_ENTRY_TLS, &dirsize );
        if (!dir32 || !dir32->AddressOfCallBacks) return;

        for (callback_array = (qemu_ptr *)(ULONG_PTR)dir32->AddressOfCallBacks;
                (callback = (PIMAGE_TLS_CALLBACK)(ULONG_PTR)(*callback_array));
                callback_array++)
        {
            /* FIXME: Exception handling removed. */
            call_dll_entry_point( (DLLENTRYPROC)callback, module, reason, NULL );
        }
    }
    else
    {
        const IMAGE_TLS_DIRECTORY64 *dir;
        const PIMAGE_TLS_CALLBACK *callback;
        dir = RtlImageDirectoryEntryToData( module, TRUE, IMAGE_DIRECTORY_ENTRY_TLS, &dirsize );
        if (!dir || !dir->AddressOfCallBacks) return;

        for (callback = (const PIMAGE_TLS_CALLBACK *)dir->AddressOfCallBacks; *callback; callback++)
        {
            /* FIXME: Exception handling removed. */
            call_dll_entry_point( (DLLENTRYPROC)*callback, module, reason, NULL );
        }
    }
}


/*************************************************************************
 *              MODULE_InitDLL
 */
static NTSTATUS MODULE_InitDLL( WINE_MODREF *wm, UINT reason, LPVOID lpReserved )
{
    NTSTATUS status = STATUS_SUCCESS;
    DLLENTRYPROC entry = wm->ldr.EntryPoint;
    void *module = wm->ldr.DllBase;
    BOOL retv = FALSE;

    /* Skip calls for modules loaded with special load flags */

    if (wm->ldr.Flags & LDR_DONT_RESOLVE_REFS) return STATUS_SUCCESS;
    if (wm->ldr.TlsIndex != -1) call_tls_callbacks( wm->ldr.DllBase, reason );
    if (!entry || !(wm->ldr.Flags & LDR_IMAGE_IS_DLL)) return STATUS_SUCCESS;

    WINE_TRACE("(%p %s,%s,%p) - CALL\n", module, wine_dbgstr_w(wm->ldr.BaseDllName.Buffer),
            reason_names[reason], lpReserved );

    /* FIXME: Exception handling removed. */
    retv = call_dll_entry_point( entry, module, reason, lpReserved );
    if (!retv)
        status = STATUS_DLL_INIT_FAILED;

    /* The state of the module list may have changed due to the call
       to the dll. We cannot assume that this module has not been
       deleted.  */
    WINE_TRACE("(%p,%s,%p) - RETURN %d\n", module, reason_names[reason], lpReserved, retv );

    return status;
}


/*************************************************************************
 *		process_attach
 *
 * Send the process attach notification to all DLLs the given module
 * depends on (recursively). This is somewhat complicated due to the fact that
 *
 * - we have to respect the module dependencies, i.e. modules implicitly
 *   referenced by another module have to be initialized before the module
 *   itself can be initialized
 *
 * - the initialization routine of a DLL can itself call LoadLibrary,
 *   thereby introducing a whole new set of dependencies (even involving
 *   the 'old' modules) at any time during the whole process
 *
 * (Note that this routine can be recursively entered not only directly
 *  from itself, but also via LoadLibrary from one of the called initialization
 *  routines.)
 *
 * Furthermore, we need to rearrange the main WINE_MODREF list to allow
 * the process *detach* notifications to be sent in the correct order.
 * This must not only take into account module dependencies, but also
 * 'hidden' dependencies created by modules calling LoadLibrary in their
 * attach notification routine.
 *
 * The strategy is rather simple: we move a WINE_MODREF to the head of the
 * list after the attach notification has returned.  This implies that the
 * detach notifications are called in the reverse of the sequence the attach
 * notifications *returned*.
 *
 * The loader_section must be locked while calling this function.
 */
static NTSTATUS process_attach( WINE_MODREF *wm, LPVOID lpReserved )
{
    NTSTATUS status = STATUS_SUCCESS;
    ULONG_PTR cookie;
    int i;

    if (process_detaching) return status;

    /* prevent infinite recursion in case of cyclical dependencies */
    if (    ( wm->ldr.Flags & LDR_LOAD_IN_PROGRESS )
         || ( wm->ldr.Flags & LDR_PROCESS_ATTACHED ) )
        return status;

    WINE_TRACE("(%s,%p) - START\n", wine_dbgstr_w(wm->ldr.BaseDllName.Buffer), lpReserved );

    /* Tag current MODREF to prevent recursive loop */
    wm->ldr.Flags |= LDR_LOAD_IN_PROGRESS;
    if (lpReserved) wm->ldr.LoadCount = -1;  /* pin it if imported by the main exe */
    if (wm->ldr.ActivationContext) RtlActivateActivationContext( 0, wm->ldr.ActivationContext, &cookie );

    /* Recursively attach all DLLs this one depends on */
    for ( i = 0; i < wm->nDeps; i++ )
    {
        if (!wm->deps[i]) continue;
        if ((status = process_attach( wm->deps[i], lpReserved )) != STATUS_SUCCESS) break;
    }

    if (!wm->ldr.InInitializationOrderLinks.Flink)
        InsertTailList(&qemu_getTEB()->Peb->LdrData->InInitializationOrderModuleList,
                &wm->ldr.InInitializationOrderLinks);

    /* Call DLL entry point */
    if (status == STATUS_SUCCESS)
    {
        WINE_MODREF *prev = current_modref;
        current_modref = wm;
        status = MODULE_InitDLL( wm, DLL_PROCESS_ATTACH, lpReserved );
        if (status == STATUS_SUCCESS)
            wm->ldr.Flags |= LDR_PROCESS_ATTACHED;
        else
        {
            MODULE_InitDLL( wm, DLL_PROCESS_DETACH, lpReserved );
            /* point to the name so LdrInitializeThunk can print it */
            last_failed_modref = wm;
            WINE_WARN("Initialization of %s failed\n", wine_dbgstr_w(wm->ldr.BaseDllName.Buffer));
        }
        current_modref = prev;
    }

    if (wm->ldr.ActivationContext) RtlDeactivateActivationContext( 0, cookie );
    /* Remove recursion flag */
    wm->ldr.Flags &= ~LDR_LOAD_IN_PROGRESS;

    WINE_TRACE("(%s,%p) - END\n", wine_dbgstr_w(wm->ldr.BaseDllName.Buffer), lpReserved );
    return status;
}


/**********************************************************************
 *	    attach_implicitly_loaded_dlls
 *
 * Attach to the (builtin) dlls that have been implicitly loaded because
 * of a dependency at the Unix level, but not imported at the Win32 level.
 */
static void attach_implicitly_loaded_dlls( LPVOID reserved )
{
    for (;;)
    {
        PLIST_ENTRY mark, entry;

        mark = &qemu_getTEB()->Peb->LdrData->InLoadOrderModuleList;
        for (entry = mark->Flink; entry != mark; entry = entry->Flink)
        {
            LDR_DATA_TABLE_ENTRY *mod = CONTAINING_RECORD(entry, LDR_DATA_TABLE_ENTRY, InLoadOrderLinks);

            if (mod->Flags & (LDR_LOAD_IN_PROGRESS | LDR_PROCESS_ATTACHED)) continue;
            WINE_TRACE( "found implicitly loaded %s, attaching to it\n",
                   wine_dbgstr_w(mod->BaseDllName.Buffer));
            process_attach( CONTAINING_RECORD(mod, WINE_MODREF, ldr), reserved );
            break;  /* restart the search from the start */
        }
        if (entry == mark) break;  /* nothing found */
    }
}


/*************************************************************************
 *		process_detach
 *
 * Send DLL process detach notifications.  See the comment about calling
 * sequence at process_attach.
 */
static void process_detach(void)
{
    PLIST_ENTRY mark, entry;
    PLDR_DATA_TABLE_ENTRY mod;

    mark = &qemu_getTEB()->Peb->LdrData->InInitializationOrderModuleList;
    do
    {
        for (entry = mark->Blink; entry != mark; entry = entry->Blink)
        {
            mod = CONTAINING_RECORD(entry, LDR_DATA_TABLE_ENTRY, 
                                    InInitializationOrderLinks);
            /* Check whether to detach this DLL */
            if ( !(mod->Flags & LDR_PROCESS_ATTACHED) )
                continue;
            if ( mod->LoadCount && !process_detaching )
                continue;

            /* Call detach notification */
            mod->Flags &= ~LDR_PROCESS_ATTACHED;
            MODULE_InitDLL( CONTAINING_RECORD(mod, WINE_MODREF, ldr), 
                            DLL_PROCESS_DETACH, ULongToPtr(process_detaching) );

            /* Restart at head of WINE_MODREF list, as entries might have
               been added and/or removed while performing the call ... */
            break;
        }
    } while (entry != mark);
}

/*************************************************************************
 *		MODULE_DllThreadAttach
 *
 * Send DLL thread attach notifications. These are sent in the
 * reverse sequence of process detach notification.
 *
 */
NTSTATUS MODULE_DllThreadAttach( LPVOID lpReserved )
{
    PLIST_ENTRY mark, entry;
    PLDR_DATA_TABLE_ENTRY mod;
    NTSTATUS    status;
    ULONG_PTR magic;

    /* don't do any attach calls if process is exiting */
    if (process_detaching) return STATUS_SUCCESS;

    LdrLockLoaderLock( 0, NULL, &magic );
    qemu_loader_thread_init();

    if ((status = alloc_thread_tls()) != STATUS_SUCCESS) goto done;

    mark = &qemu_getTEB()->Peb->LdrData->InInitializationOrderModuleList;
    for (entry = mark->Flink; entry != mark; entry = entry->Flink)
    {
        mod = CONTAINING_RECORD(entry, LDR_DATA_TABLE_ENTRY, 
                                InInitializationOrderLinks);
        if ( !(mod->Flags & LDR_PROCESS_ATTACHED) )
            continue;
        if ( mod->Flags & LDR_NO_DLL_CALLS )
            continue;

        MODULE_InitDLL( CONTAINING_RECORD(mod, WINE_MODREF, ldr),
                        DLL_THREAD_ATTACH, lpReserved );
    }

done:
    LdrUnlockLoaderLock( 0, magic );
    return status;
}

NTSTATUS qemu_LdrDisableThreadCalloutsForDll(HMODULE hModule)
{
    WINE_MODREF *wm;
    NTSTATUS    ret = STATUS_SUCCESS;
    ULONG_PTR magic;

    LdrLockLoaderLock( 0, NULL, &magic );

    wm = get_modref( hModule );
    if (!wm || wm->ldr.TlsIndex != -1)
        ret = STATUS_DLL_NOT_FOUND;
    else
        wm->ldr.Flags |= LDR_NO_DLL_CALLS;

    LdrUnlockLoaderLock( 0, magic );

    return ret;
}

/******************************************************************
 *              LdrFindEntryForAddress (NTDLL.@)
 *
 * The loader_section must be locked while calling this function
 */
static NTSTATUS qemu_LdrFindEntryForAddress(const void* addr, PLDR_DATA_TABLE_ENTRY* pmod)
{
    PLIST_ENTRY mark, entry;
    PLDR_DATA_TABLE_ENTRY mod;

    mark = &qemu_getTEB()->Peb->LdrData->InMemoryOrderModuleList;
    for (entry = mark->Flink; entry != mark; entry = entry->Flink)
    {
        mod = CONTAINING_RECORD(entry, LDR_DATA_TABLE_ENTRY, InMemoryOrderLinks);
        if (mod->DllBase <= addr &&
            (const char *)addr < (char*)mod->DllBase + mod->SizeOfImage)
        {
            *pmod = mod;
            return STATUS_SUCCESS;
        }
    }
    return STATUS_NO_MORE_ENTRIES;
}

/* Give Wine's loader information about our loaded modules. This is needed for creating
 * activation contexts from loaded guest modules. */
NTSTATUS WINAPI hook_LdrFindEntryForAddress(const void* addr, PLDR_DATA_TABLE_ENTRY* pmod)
{
    PLIST_ENTRY mark, entry;
    PLDR_DATA_TABLE_ENTRY mod;
    NTSTATUS ret;

    WINE_TRACE("Looking for address %p.\n", addr);
    mark = &NtCurrentTeb()->Peb->LdrData->InMemoryOrderModuleList;
    for (entry = mark->Flink; entry != mark; entry = entry->Flink)
    {
        mod = CONTAINING_RECORD(entry, LDR_DATA_TABLE_ENTRY, InMemoryOrderLinks);
        if (mod->DllBase <= addr &&
            (const char *)addr < (char*)mod->DllBase + mod->SizeOfImage)
        {
            WINE_TRACE("Found address %p in Wine's PEB.\n", addr);
            *pmod = mod;
            return STATUS_SUCCESS;
        }
    }

    ret = qemu_LdrFindEntryForAddress(addr, pmod);
    if (ret == STATUS_SUCCESS)
        WINE_TRACE("Found address %p in qemu's PEB.\n", addr);
    else
        WINE_WARN("Did not find a module for address %p.\n", addr);
    return ret;
}

/******************************************************************
 *              LdrEnumerateLoadedModules (NTDLL.@)
 */
NTSTATUS WINAPI qemu_LdrEnumerateLoadedModules( void *unknown, void *cb, void *context )
{
    LIST_ENTRY *mark, *entry;
    LDR_DATA_TABLE_ENTRY *mod;
    BOOLEAN stop = FALSE;
    LDRENUMPROC callback = cb;
    ULONG_PTR magic;

    WINE_TRACE( "(%p, %p, %p)\n", unknown, callback, context );

    if (unknown || !callback)
        return STATUS_INVALID_PARAMETER;

    LdrLockLoaderLock( 0, NULL, &magic );

    mark = &qemu_getTEB()->Peb->LdrData->InMemoryOrderModuleList;
    for (entry = mark->Flink; entry != mark; entry = entry->Flink)
    {
        mod = CONTAINING_RECORD( entry, LDR_DATA_TABLE_ENTRY, InMemoryOrderLinks );
        callback( mod, context, &stop );
        if (stop) break;
    }

    LdrUnlockLoaderLock( 0, magic );
    return STATUS_SUCCESS;
}

/******************************************************************
 *		qemu_LdrGetProcedureAddress  (NTDLL.@)
 */
NTSTATUS qemu_LdrGetProcedureAddress(HMODULE module, const ANSI_STRING *name,
        ULONG ord, PVOID *address)
{
    IMAGE_EXPORT_DIRECTORY *exports;
    DWORD exp_size;
    NTSTATUS ret = STATUS_PROCEDURE_NOT_FOUND;
    ULONG_PTR magic;

    LdrLockLoaderLock( 0, NULL, &magic );

    /* check if the module itself is invalid to return the proper error */
    if (!get_modref( module )) ret = STATUS_DLL_NOT_FOUND;
    else if ((exports = RtlImageDirectoryEntryToData( module, TRUE,
                                                      IMAGE_DIRECTORY_ENTRY_EXPORT, &exp_size )))
    {
        LPCWSTR load_path = MODULE_get_dll_load_path(NULL); /* FIXME */
        void *proc = name ? find_named_export( module, exports, exp_size, name->Buffer, -1, load_path )
                          : find_ordinal_export( module, exports, exp_size, ord - exports->Base, load_path, NULL );
        if (proc)
        {
            *address = proc;
            ret = STATUS_SUCCESS;
        }
    }

    LdrUnlockLoaderLock( 0, magic );
    return ret;
}


/***********************************************************************
 *           is_fake_dll
 *
 * Check if a loaded native dll is a Wine fake dll.
 */
static BOOL is_fake_dll( HANDLE handle )
{
    static const char fakedll_signature[] = "Wine placeholder DLL";
    char buffer[sizeof(IMAGE_DOS_HEADER) + sizeof(fakedll_signature)];
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)buffer;
    IO_STATUS_BLOCK io;
    LARGE_INTEGER offset;

    offset.QuadPart = 0;
    if (NtReadFile( handle, 0, NULL, 0, &io, buffer, sizeof(buffer), &offset, NULL )) return FALSE;
    if (io.Information < sizeof(buffer)) return FALSE;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return FALSE;
    if (dos->e_lfanew >= sizeof(*dos) + sizeof(fakedll_signature) &&
        !memcmp( dos + 1, fakedll_signature, sizeof(fakedll_signature) )) return TRUE;
    return FALSE;
}

/***********************************************************************
 *           set_security_cookie
 *
 * Create a random security cookie for buffer overflow protection. Make
 * sure it does not accidentally match the default cookie value.
 */
static void set_security_cookie( void *module, SIZE_T len )
{
    static ULONG seed;
    IMAGE_LOAD_CONFIG_DIRECTORY *loadcfg;
    ULONG loadcfg_size;
    ULONG_PTR *cookie;

    loadcfg = RtlImageDirectoryEntryToData( module, TRUE, IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG, &loadcfg_size );
    if (!loadcfg) return;
    if (loadcfg_size < offsetof(IMAGE_LOAD_CONFIG_DIRECTORY, SecurityCookie) + sizeof(loadcfg->SecurityCookie)) return;
    if (!loadcfg->SecurityCookie) return;
    if (loadcfg->SecurityCookie < (ULONG_PTR)module ||
        loadcfg->SecurityCookie > (ULONG_PTR)module + len - sizeof(ULONG_PTR))
    {
        WINE_WARN( "security cookie %p outside of image %p-%p\n",
              (void *)loadcfg->SecurityCookie, module, (char *)module + len );
        return;
    }

    cookie = (ULONG_PTR *)loadcfg->SecurityCookie;
    WINE_TRACE( "initializing security cookie %p\n", cookie );

    if (!seed) seed = NtGetTickCount() ^ GetCurrentProcessId();
    for (;;)
    {
        if (*cookie == DEFAULT_SECURITY_COOKIE_16)
            *cookie = RtlRandom( &seed ) >> 16; /* leave the high word clear */
        else if (*cookie == DEFAULT_SECURITY_COOKIE_32)
            *cookie = RtlRandom( &seed );
#ifdef DEFAULT_SECURITY_COOKIE_64
        else if (*cookie == DEFAULT_SECURITY_COOKIE_64)
        {
            *cookie = RtlRandom( &seed );
            /* fill up, but keep the highest word clear */
            *cookie ^= (ULONG_PTR)RtlRandom( &seed ) << 16;
        }
#endif
        else
            break;
    }
}

static IMAGE_BASE_RELOCATION * qemu_LdrProcessRelocationBlock( void *page, UINT count,
                                                               USHORT *relocs, INT_PTR delta )
{
    while (count--)
    {
        USHORT offset = *relocs & 0xfff;
        int type = *relocs >> 12;
        switch(type)
        {
        case IMAGE_REL_BASED_ABSOLUTE:
            break;
        case IMAGE_REL_BASED_HIGH:
            *(short *)((char *)page + offset) += HIWORD(delta);
            break;
        case IMAGE_REL_BASED_LOW:
            *(short *)((char *)page + offset) += LOWORD(delta);
            break;
        case IMAGE_REL_BASED_HIGHLOW:
            *(int *)((char *)page + offset) += delta;
            break;
        case IMAGE_REL_BASED_DIR64:
            *(INT_PTR *)((char *)page + offset) += delta;
            break;
        default:
            WINE_FIXME("Unknown/unsupported fixup type %x.\n", type);
            return NULL;
        }
        relocs++;
    }
    return (IMAGE_BASE_RELOCATION *)relocs;  /* return address of next block */
}

static NTSTATUS perform_relocations( void *module, SIZE_T len )
{
    IMAGE_NT_HEADERS *nt;
    char *base;
    IMAGE_BASE_RELOCATION *rel, *end;
    const IMAGE_DATA_DIRECTORY *relocs;
    const IMAGE_SECTION_HEADER *sec;
    INT_PTR delta;
    ULONG protect_old[96], i;
    IMAGE_OPTIONAL_HEADER32 *hdr32 = NULL;
    IMAGE_OPTIONAL_HEADER64 *hdr64 = NULL;
    DWORD alignment;

    nt = RtlImageNtHeader( module );
    if (is_32_bit)
    {
        hdr32 = (IMAGE_OPTIONAL_HEADER32 *)&nt->OptionalHeader;
        base = (char *)(ULONG_PTR)hdr32->ImageBase;
        alignment = hdr32->SectionAlignment;
        relocs = &hdr32->DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    }
    else
    {
        hdr64 = &nt->OptionalHeader;
        alignment = hdr64->SectionAlignment;
        base = (char *)hdr64->ImageBase;
        relocs = &hdr64->DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    }

    assert( module != base );

    /* no relocations are performed on non page-aligned binaries */
    if (alignment < TARGET_PAGE_SIZE)
        return STATUS_SUCCESS;

    if (!(nt->FileHeader.Characteristics & IMAGE_FILE_DLL) && qemu_getTEB()->Peb->ImageBaseAddress)
        return STATUS_SUCCESS;

    if (nt->FileHeader.Characteristics & IMAGE_FILE_RELOCS_STRIPPED)
    {
        WINE_WARN( "Need to relocate module from %p to %p, but there are no relocation records\n",
              base, module );
        return STATUS_CONFLICTING_ADDRESSES;
    }

    if (!relocs->Size) return STATUS_SUCCESS;
    if (!relocs->VirtualAddress) return STATUS_CONFLICTING_ADDRESSES;

    if (nt->FileHeader.NumberOfSections > sizeof(protect_old)/sizeof(protect_old[0]))
        return STATUS_INVALID_IMAGE_FORMAT;

    sec = (const IMAGE_SECTION_HEADER *)((const char *)&nt->OptionalHeader +
                                         nt->FileHeader.SizeOfOptionalHeader);
    for (i = 0; i < nt->FileHeader.NumberOfSections; i++)
    {
        void *addr = get_rva( module, sec[i].VirtualAddress );
        SIZE_T size = sec[i].SizeOfRawData;
        NtProtectVirtualMemory( NtCurrentProcess(), &addr,
                                &size, PAGE_READWRITE, &protect_old[i] );
    }

    WINE_TRACE( "relocating from %p-%p to %p-%p\n",
           base, base + len, module, (char *)module + len );

    rel = get_rva( module, relocs->VirtualAddress );
    end = get_rva( module, relocs->VirtualAddress + relocs->Size );
    delta = (char *)module - base;

    while (rel < end - 1 && rel->SizeOfBlock)
    {
        if (rel->VirtualAddress >= len)
        {
            WINE_WARN( "invalid address %p in relocation %p\n", get_rva( module, rel->VirtualAddress ), rel );
            return STATUS_ACCESS_VIOLATION;
        }
        rel = qemu_LdrProcessRelocationBlock( get_rva( module, rel->VirtualAddress ),
                                         (rel->SizeOfBlock - sizeof(*rel)) / sizeof(USHORT),
                                         (USHORT *)(rel + 1), delta );
        if (!rel) return STATUS_INVALID_IMAGE_FORMAT;
    }

    for (i = 0; i < nt->FileHeader.NumberOfSections; i++)
    {
        void *addr = get_rva( module, sec[i].VirtualAddress );
        SIZE_T size = sec[i].SizeOfRawData;
        NtProtectVirtualMemory( NtCurrentProcess(), &addr,
                                &size, protect_old[i], &protect_old[i] );
    }

    return STATUS_SUCCESS;
}

static NTSTATUS map_library(HANDLE file, void **module, SIZE_T *len)
{
    IMAGE_DOS_HEADER dos;
    struct nt_header nt;
    BOOL ret;
    DWORD read;
    SIZE_T image_size, header_size, section_align;
    void *image_base;
    SIZE_T fixed_header_size;
    unsigned int i;
    void *base = NULL, *alloc;
    const IMAGE_SECTION_HEADER *section;
    NTSTATUS status = STATUS_DLL_NOT_FOUND;
    DWORD protect, old_protect;

    SetFilePointer(file, 0, NULL, FILE_BEGIN);
    ret = ReadFile(file, &dos, sizeof(dos), &read, NULL);
    if (!ret || read != sizeof(dos))
    {
        WINE_ERR("Failed to read DOS header.\n");
        status = STATUS_INVALID_IMAGE_NOT_MZ;
        goto error;
    }
    if (dos.e_magic != IMAGE_DOS_SIGNATURE)
    {
        WINE_ERR("Invalid DOS signature.\n");
        status = STATUS_INVALID_IMAGE_NOT_MZ;
        goto error;
    }

    SetFilePointer(file, dos.e_lfanew, NULL, FILE_BEGIN);
    ret = ReadFile(file, &nt, sizeof(nt), &read, NULL);
    if (!ret || read != sizeof(nt))
    {
        WINE_ERR("Failed to read PE header.\n");
        status = STATUS_INVALID_IMAGE_FORMAT;
        goto error;
    }
    if (nt.Signature != IMAGE_NT_SIGNATURE)
    {
        WINE_ERR("Invalid NT signature.\n");
        if (nt.Signature == IMAGE_OS2_SIGNATURE) status = STATUS_INVALID_IMAGE_NE_FORMAT;
        status = STATUS_INVALID_IMAGE_PROTECT;
        goto error;
    }

    fixed_header_size = dos.e_lfanew + sizeof(nt.Signature) + sizeof(nt.FileHeader);

    switch (nt.FileHeader.Machine)
    {
        case IMAGE_FILE_MACHINE_I386:
            if (nt.opt.hdr64.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC)
            {
                WINE_ERR("Wrong optional header magic.\n");
                status = STATUS_INVALID_IMAGE_FORMAT;
                goto error;
            }
            image_base = (void *)(DWORD_PTR)nt.opt.hdr32.ImageBase;
            image_size = ROUND_SIZE(nt.opt.hdr32.SizeOfImage);
            header_size = nt.opt.hdr32.SizeOfHeaders;
            section_align = nt.opt.hdr32.SectionAlignment;
            fixed_header_size += sizeof(nt.opt.hdr32);
            break;
        case IMAGE_FILE_MACHINE_AMD64:
            if (nt.opt.hdr64.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
            {
                WINE_ERR("Wrong optional header magic.\n");
                status = STATUS_INVALID_IMAGE_FORMAT;
                goto error;
            }
            image_base = (void *)nt.opt.hdr64.ImageBase;
            image_size = ROUND_SIZE(nt.opt.hdr64.SizeOfImage);
            header_size = nt.opt.hdr64.SizeOfHeaders;
            section_align = nt.opt.hdr64.SectionAlignment;
            fixed_header_size += sizeof(nt.opt.hdr64);
            break;
        default:
            WINE_ERR("Unsupported machine %d.\n", nt.FileHeader.Machine);
            status = STATUS_INVALID_IMAGE_FORMAT;
            goto error;
    }

    /* Why not use CreateFileMapping(SEC_IMAGE) and remove most of the code in this
     * function you ask? Because SEC_IMAGE only works with files that have a matching
     * target CPU, at least in Wine. This is also a large part why we need the custom
     * loader and can't just do LoadLibraryEx(DON'T_RESOLVE_DLL_REFERENCES).
     * GetModuleHandle is the other big problem we're facing with mixing libs of two
     * architectures in the same process.
     *
     * Unfortunately Windows has no way to reserve an area of address space and then
     * map file(s) into it later. The other problem is that MapViewOfFile needs 64k
     * aligned offsets, but PE section alignment is 4k. So alloc anonymous memory and
     * read the file contents into it.
     *
     * A future optimization could try to map as much read-only data as possible from
     * the file and alloc+read the rest. We'd probably manage headers + .text, which
     * I expect to be the majority of the file. */

    WINE_TRACE("Trying to map file size %lu at %p.\n", (unsigned long)image_size, image_base);
    base = VirtualAlloc(image_base, image_size, MEM_RESERVE, PAGE_READONLY);
    WINE_TRACE("Got %p\n", base);
    if (!base)
    {
        base = VirtualAlloc(NULL, image_size, MEM_RESERVE, PAGE_READONLY);
        if (!base)
        {
            WINE_ERR("Failed to reserve address space for image!\n");
            status = STATUS_SECTION_TOO_BIG; /* Not sure */
            goto error;
        }
    }

    alloc = VirtualAlloc(base, header_size, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!alloc)
    {
        WINE_ERR("Failed to commit memory for image headers.\n");
        status = STATUS_SECTION_TOO_BIG; /* Not sure */
        goto error;
    }
    SetFilePointer(file, 0, NULL, FILE_BEGIN);
    ret = ReadFile(file, base, header_size, &read, NULL);
    if (!ret || read != header_size)
    {
        WINE_ERR("Failed to read image headers.\n");
        status = STATUS_INVALID_FILE_FOR_SECTION; /* Not sure */
        goto error;
    }
    if (section_align >= TARGET_PAGE_SIZE)
        VirtualProtect(base, header_size, PAGE_READONLY, &old_protect);

    section = (const IMAGE_SECTION_HEADER *)((char *)base + fixed_header_size);
    WINE_TRACE("Got %u sections at %p\n", nt.FileHeader.NumberOfSections, section);

    for (i = 0; i < nt.FileHeader.NumberOfSections; i++)
    {
        void *location = get_rva(base, section[i].VirtualAddress);
        SIZE_T map_size = section[i].Misc.VirtualSize;
        WINE_TRACE("Mapping section %8s at %p.\n", section[i].Name, location);

        alloc = VirtualAlloc(location, map_size, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        if (!alloc)
        {
            WINE_ERR("Failed to commit memory for section %8s.\n", section[i].Name);
            status = STATUS_SECTION_TOO_BIG; /* Not sure */
            goto error;
        }

        if (section[i].SizeOfRawData)
        {
            WINE_TRACE("Reading %8s from 0x%x to %p.\n",
                    section[i].Name, section[i].PointerToRawData, location);

            SetFilePointer(file, section[i].PointerToRawData, NULL, FILE_BEGIN);
            ret = ReadFile(file, alloc, section[i].SizeOfRawData, &read, NULL);
            if (!ret || read != section[i].SizeOfRawData)
            {
                WINE_ERR("Failed to read section %8s.\n", section[i].Name);
                status = STATUS_INVALID_FILE_FOR_SECTION; /* Not sure */
                goto error;
            }
        }

        /* Everything that has write but not read probably doesn't make sense. There is
         * no PAGE_WRITEONLY or PAGE_WRITEEXECUTE flag. And writing at a poor alignment
         * probably requires a read anyway. */
        switch (section[i].Characteristics
                & (IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE | IMAGE_SCN_MEM_EXECUTE))
        {
            case IMAGE_SCN_MEM_READ:
                WINE_TRACE("Section %s is read-only.\n", section[i].Name);
                protect = PAGE_READONLY;
                break;
            case IMAGE_SCN_MEM_WRITE:
                WINE_TRACE("Section %s is write-only.\n", section[i].Name);
                protect = PAGE_READWRITE;
                break;
            case IMAGE_SCN_MEM_EXECUTE:
                WINE_TRACE("Section %s is execute-only.\n", section[i].Name);
                protect = PAGE_EXECUTE;
                break;

            case IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE:
                WINE_TRACE("Section %s is read-write.\n", section[i].Name);
                protect = PAGE_READWRITE;
                break;
            case IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_EXECUTE:
                WINE_TRACE("Section %s is read-execute.\n", section[i].Name);
                protect = PAGE_EXECUTE_READ;
                break;
            case IMAGE_SCN_MEM_WRITE | IMAGE_SCN_MEM_EXECUTE:
                WINE_TRACE("Section %s is write-execute.\n", section[i].Name);
                protect = PAGE_EXECUTE_READWRITE;
                break;

            case IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE | IMAGE_SCN_MEM_EXECUTE:
                WINE_TRACE("Section %s is read-write-execute.\n", section[i].Name);
                protect = PAGE_EXECUTE_READWRITE;
                break;

            default:
                WINE_ERR("Forgot to handle %x.\n", section[i].Characteristics);
                protect = PAGE_EXECUTE_READWRITE;
        }
        if (protect != PAGE_EXECUTE_READWRITE && !VirtualProtect(alloc, map_size, protect, &old_protect))
            WINE_ERR("VirtualProtect failed.\n");
    }

    *module = base;
    *len = image_size;
    return base == image_base ? STATUS_SUCCESS : STATUS_IMAGE_NOT_AT_BASE;

error:
    if (base)
        VirtualFree(base, 0, MEM_RELEASE);

    return status;
}

/******************************************************************************
 *	load_native_dll  (internal)
 */
static NTSTATUS load_native_dll( LPCWSTR load_path, LPCWSTR name, HANDLE file,
                                 DWORD flags, WINE_MODREF** pwm )
{
    void *module;
    IMAGE_NT_HEADERS *nt;
    SIZE_T len = 0;
    WINE_MODREF *wm;
    NTSTATUS status;

    WINE_TRACE("Trying native dll %s\n", wine_dbgstr_w(name));

    status = map_library(file, &module, &len);
    if (status != STATUS_SUCCESS && status != STATUS_IMAGE_NOT_AT_BASE)
        return status;

    /* perform base relocation, if necessary */

    if (status == STATUS_IMAGE_NOT_AT_BASE)
        status = perform_relocations( module, len );

    if (status != STATUS_SUCCESS)
    {
        if (module) VirtualFree(module, 0, MEM_RELEASE);
        goto done;
    }

    /* create the MODREF */

    if (!(wm = alloc_module( module, name )))
    {
        status = STATUS_NO_MEMORY;
        goto done;
    }

    set_security_cookie( module, len );

    /* fixup imports */

    nt = RtlImageNtHeader( module );

    if (!(flags & DONT_RESOLVE_DLL_REFERENCES) &&
        ((nt->FileHeader.Characteristics & IMAGE_FILE_DLL) ||
         nt->OptionalHeader.Subsystem == IMAGE_SUBSYSTEM_NATIVE))
    {
        if ((status = fixup_imports( wm, load_path )) != STATUS_SUCCESS)
        {
            /* the module has only be inserted in the load & memory order lists */
            RemoveEntryList(&wm->ldr.InLoadOrderLinks);
            RemoveEntryList(&wm->ldr.InMemoryOrderLinks);

            /* WINE_FIXME: there are several more dangling references
             * left. Including dlls loaded by this dll before the
             * failed one. Unrolling is rather difficult with the
             * current structure and we can leave them lying
             * around with no problems, so we don't care.
             * As these might reference our wm, we don't free it.
             */
            goto done;
        }
    }

    WINE_TRACE( "Loaded %s at %p: native\n", wine_dbgstr_w(wm->ldr.FullDllName.Buffer), module );

    wm->ldr.LoadCount = 1;
    *pwm = wm;
    status = STATUS_SUCCESS;
done:
    return status;
}

/***********************************************************************
 *	find_actctx_dll
 *
 * Find the full path (if any) of the dll from the activation context.
 */
static NTSTATUS find_actctx_dll( LPCWSTR libname, LPWSTR *fullname )
{
    static const WCHAR winsxsW[] = {'\\','w','i','n','s','x','s','\\'};
    static const WCHAR dotManifestW[] = {'.','m','a','n','i','f','e','s','t',0};

    ACTIVATION_CONTEXT_ASSEMBLY_DETAILED_INFORMATION *info;
    ACTCTX_SECTION_KEYED_DATA data;
    UNICODE_STRING nameW;
    NTSTATUS status;
    SIZE_T needed, size = 1024;
    WCHAR *p;
    WCHAR NtSystemRoot[MAX_PATH];

    RtlInitUnicodeString( &nameW, libname );
    data.cbSize = sizeof(data);
    status = RtlFindActivationContextSectionString( FIND_ACTCTX_SECTION_KEY_RETURN_HACTCTX, NULL,
                                                    ACTIVATION_CONTEXT_SECTION_DLL_REDIRECTION,
                                                    &nameW, &data );
    if (status != STATUS_SUCCESS) return status;

    for (;;)
    {
        if (!(info = RtlAllocateHeap( GetProcessHeap(), 0, size )))
        {
            status = STATUS_NO_MEMORY;
            goto done;
        }
        status = RtlQueryInformationActivationContext( 0, data.hActCtx, &data.ulAssemblyRosterIndex,
                                                       AssemblyDetailedInformationInActivationContext,
                                                       info, size, &needed );
        if (status == STATUS_SUCCESS) break;
        if (status != STATUS_BUFFER_TOO_SMALL) goto done;
        RtlFreeHeap( GetProcessHeap(), 0, info );
        size = needed;
        /* restart with larger buffer */
    }

    if (!info->lpAssemblyManifestPath || !info->lpAssemblyDirectoryName)
    {
        status = STATUS_SXS_KEY_NOT_FOUND;
        goto done;
    }

    if ((p = strrchrW( info->lpAssemblyManifestPath, '\\' )))
    {
        DWORD dirlen = info->ulAssemblyDirectoryNameLength / sizeof(WCHAR);

        p++;
        if (_wcsnicmp( p, info->lpAssemblyDirectoryName, dirlen ) || _wcsicmp( p + dirlen, dotManifestW ))
        {
            /* manifest name does not match directory name, so it's not a global
             * windows/winsxs manifest; use the manifest directory name instead */
            dirlen = p - info->lpAssemblyManifestPath;
            needed = (dirlen + 1) * sizeof(WCHAR) + nameW.Length;
            if (!(*fullname = p = RtlAllocateHeap( GetProcessHeap(), 0, needed )))
            {
                status = STATUS_NO_MEMORY;
                goto done;
            }
            memcpy( p, info->lpAssemblyManifestPath, dirlen * sizeof(WCHAR) );
            p += dirlen;
            strcpyW( p, libname );
            goto done;
        }
    }

    GetWindowsDirectoryW(NtSystemRoot, sizeof(NtSystemRoot) / sizeof(*NtSystemRoot));
    needed = (strlenW(NtSystemRoot) * sizeof(WCHAR) +
              sizeof(winsxsW) + info->ulAssemblyDirectoryNameLength + nameW.Length + 2*sizeof(WCHAR));

    if (!(*fullname = p = RtlAllocateHeap( GetProcessHeap(), 0, needed )))
    {
        status = STATUS_NO_MEMORY;
        goto done;
    }
    strcpyW( p, NtSystemRoot );
    p += strlenW(p);
    memcpy( p, winsxsW, sizeof(winsxsW) );
    p += sizeof(winsxsW) / sizeof(WCHAR);
    memcpy( p, info->lpAssemblyDirectoryName, info->ulAssemblyDirectoryNameLength );
    p += info->ulAssemblyDirectoryNameLength / sizeof(WCHAR);
    *p++ = '\\';
    strcpyW( p, libname );
done:
    RtlFreeHeap( GetProcessHeap(), 0, info );
    RtlReleaseActivationContext( data.hActCtx );
    return status;
}

static WCHAR *get_guest_dll_path(void)
{
    static BOOL cached_32 = FALSE;
    static WCHAR *cached_path;
    static const WCHAR qemu_guest_dll32[] =
            {'\\','q','e','m','u','_','g','u','e','s','t','_','d','l','l','3','2','\\', 0};
    static const WCHAR qemu_guest_dll64[] =
            {'\\','q','e','m','u','_','g','u','e','s','t','_','d','l','l','6','4','\\', 0};

    if (cached_path && cached_32 == is_32_bit)
        return cached_path;

    HeapFree(GetProcessHeap(), 0, cached_path);

    cached_32 = is_32_bit;
    cached_path = HeapAlloc(GetProcessHeap(), 0, (MAX_PATH + 12) * sizeof(*cached_path));
    memcpy(cached_path, qemu_pathname, (lstrlenW(qemu_pathname) + 1 ) * sizeof(*qemu_pathname));
    my_PathRemoveFileSpecW(cached_path);
    if (is_32_bit)
        lstrcatW(cached_path, qemu_guest_dll32);
    else
        lstrcatW(cached_path, qemu_guest_dll64);

    return cached_path;
}

/***********************************************************************
 *	find_dll_file
 *
 * Find the file (or already loaded module) for a given dll name.
 */
static NTSTATUS find_dll_file( const WCHAR *load_path, const WCHAR *libname,
                               WCHAR *filename, ULONG *size, WINE_MODREF **pwm, HANDLE *handle )
{
    OBJECT_ATTRIBUTES attr;
    IO_STATUS_BLOCK io;
    UNICODE_STRING nt_name, nt_name_orig;
    WCHAR *file_part, *ext, *dllname, *sysdir, *qemu_sysdir = get_guest_dll_path();
    ULONG len, sysdirlen;
    BOOLEAN convert;
    static const WCHAR fusionW[] = {'f','u','s','i','o','n','.','d','l','l',0};

    /* first append .dll if needed */

    dllname = NULL;
    if (!(ext = strrchrW( libname, '.')) || strchrW( ext, '/' ) || strchrW( ext, '\\'))
    {
        if (!(dllname = RtlAllocateHeap( GetProcessHeap(), 0,
                                         (strlenW(libname) * sizeof(WCHAR)) + sizeof(dllW) )))
            return STATUS_NO_MEMORY;
        strcpyW( dllname, libname );
        strcatW( dllname, dllW );
        libname = dllname;
    }

    sysdirlen = GetSystemDirectoryW( NULL, 0 );
    sysdir = my_alloc(sizeof(*sysdir) * sysdirlen);
    GetSystemDirectoryW(sysdir, sysdirlen);

    nt_name.Buffer = NULL;
    nt_name_orig.Buffer = NULL;

    if (!contains_path( libname ))
    {
        NTSTATUS status;
        WCHAR *fullname = NULL;

        if ((*pwm = find_basename_module( libname )) != NULL) goto found;

        status = find_actctx_dll( libname, &fullname );
        if (status == STATUS_SUCCESS)
        {
            WINE_TRACE ("found %s for %s\n", wine_dbgstr_w(fullname), wine_dbgstr_w(libname) );
            RtlFreeHeap( GetProcessHeap(), 0, dllname );
            libname = dllname = fullname;
        }
        else if (status != STATUS_SXS_KEY_NOT_FOUND)
        {
            RtlFreeHeap( GetProcessHeap(), 0, dllname );
            return status;
        }
    }

    if (RtlDetermineDosPathNameType_U( libname ) == RELATIVE_PATH)
    {
        /* we need to search for it */
        len = RtlDosSearchPath_U( load_path, libname, NULL, *size, filename, &file_part );
        /* If we haven't found it yet, try to search in qemu's DLL directory. There may be libs in there
         * that are not in system32, e.g. libwine.dll. Don't bother to fake the directory of those files
         * as they should only be internal files and not directly accessed by the app. */
        if (!len)
            len = RtlDosSearchPath_U( qemu_sysdir, libname, NULL, *size, filename, &file_part );

        if (len)
        {
            if (len >= *size) goto overflow;
            if ((*pwm = find_fullname_module( filename )) || !handle) goto found;

            /* Load anything that is placed in system32 from qemu's guest DLL dir instead, but
             * pretend that the file is from system32. */
            if (!_wcsnicmp(filename, sysdir, sysdirlen - 1))
            {
                WCHAR *filename2;

                len = strlenW(qemu_sysdir) + strlenW(filename) + 1;
                filename2 = my_alloc(len * sizeof(*filename2));
                strcpyW(filename2, qemu_sysdir);
                strcatW(filename2, filename + sysdirlen);

                convert = RtlDosPathNameToNtPathName_U( filename2, &nt_name, NULL, NULL );
                my_free(filename2);
            }
            else
            {
                convert = RtlDosPathNameToNtPathName_U( filename, &nt_name, NULL, NULL );
            }

            if (!convert)
            {
                RtlFreeHeap( GetProcessHeap(), 0, dllname );
                my_free(sysdir);
                return STATUS_NO_MEMORY;
            }
            attr.Length = sizeof(attr);
            attr.RootDirectory = 0;
            attr.Attributes = OBJ_CASE_INSENSITIVE;
            attr.ObjectName = &nt_name;
            attr.SecurityDescriptor = NULL;
            attr.SecurityQualityOfService = NULL;
            if (NtOpenFile( handle, GENERIC_READ|SYNCHRONIZE, &attr, &io, FILE_SHARE_READ|FILE_SHARE_DELETE, FILE_SYNCHRONOUS_IO_NONALERT|FILE_NON_DIRECTORY_FILE )) *handle = 0;
            goto found;
        }

        /* not found */

        if (!contains_path( libname ))
        {
            /* if libname doesn't contain a path at all, we simply return the name as is,
             * to be loaded as builtin */
            len = strlenW(libname) * sizeof(WCHAR);
            if (len >= *size) goto overflow;
            strcpyW( filename, libname );
            goto found;
        }
    }

    /* absolute path name, or relative path name but not found above */

    /* Load anything that is placed in system32 from qemu's guest DLL dir instead, but
     *  pretend that the file is from system32.
     *
     * Also HACK: Load fusion.dll from qemu's guest DLL dir no matter where it is found.
     * .NET places it in C:\windows\Microsoft.NET\Framework\fusion.dll and native fusion
     * does not work because it tries to create reparse points. We need Wine's fusion.
     *
     * This thing should be made more generic to have a switch between native, wine PE build,
     * hangover wrapper. */
    if (wcsstr(libname, fusionW))
    {
        WCHAR *filename2;
        convert = RtlDosPathNameToNtPathName_U( libname, &nt_name_orig, &file_part, NULL );

        len = strlenW(qemu_sysdir) + strlenW(fusionW) + 1;
        filename2 = my_alloc(len * sizeof(*filename2));
        strcpyW(filename2, qemu_sysdir);
        strcatW(filename2, fusionW);

        WINE_TRACE("Loading %s instead of %s\n", wine_dbgstr_w(filename2), wine_dbgstr_w(libname));

        /* FIXME: File_part? We'll probably only get here if the original path was absolute anyway. */
        convert = RtlDosPathNameToNtPathName_U( filename2, &nt_name, &file_part, NULL );
        my_free(filename2);
    }
    else if (!_wcsnicmp(libname, sysdir, sysdirlen - 1))
    {
        WCHAR *filename2;

        convert = RtlDosPathNameToNtPathName_U( libname, &nt_name_orig, &file_part, NULL );

        len = strlenW(qemu_sysdir) + strlenW(libname) + 1;
        filename2 = my_alloc(len * sizeof(*filename2));
        strcpyW(filename2, qemu_sysdir);
        strcatW(filename2, libname + sysdirlen);

        WINE_TRACE("Loading %s instead of %s\n", wine_dbgstr_w(filename2), wine_dbgstr_w(libname));

        /* FIXME: File_part? We'll probably only get here if the original path was absolute anyway. */
        convert = RtlDosPathNameToNtPathName_U( filename2, &nt_name, &file_part, NULL );
        my_free(filename2);
    }
    else
    {
        convert = RtlDosPathNameToNtPathName_U( libname, &nt_name, &file_part, NULL );
    }

    if (!convert)
    {
        RtlFreeHeap( GetProcessHeap(), 0, dllname );
        my_free(sysdir);
        return STATUS_NO_MEMORY;
    }
    if (nt_name_orig.Buffer)
    {
        /* Because the loaded DLL list stores the original name we have to call find_fullname_module
         * with the unmodified name.
         *
         * But handling it this way is dead ugly. Try to do this nicer. */
        len = nt_name_orig.Length - 4*sizeof(WCHAR);  /* for \??\ prefix */
        if (len >= *size) goto overflow;
        memcpy( filename, nt_name_orig.Buffer + 4, len + sizeof(WCHAR) );
        RtlFreeUnicodeString( &nt_name_orig );
    }
    else
    {
        len = nt_name.Length - 4*sizeof(WCHAR);  /* for \??\ prefix */
        if (len >= *size) goto overflow;
        memcpy( filename, nt_name.Buffer + 4, len + sizeof(WCHAR) );
    }
    if (!(*pwm = find_fullname_module( filename )) && handle)
    {
        attr.Length = sizeof(attr);
        attr.RootDirectory = 0;
        attr.Attributes = OBJ_CASE_INSENSITIVE;
        attr.ObjectName = &nt_name;
        attr.SecurityDescriptor = NULL;
        attr.SecurityQualityOfService = NULL;
        if (NtOpenFile( handle, GENERIC_READ|SYNCHRONIZE, &attr, &io, FILE_SHARE_READ|FILE_SHARE_DELETE, FILE_SYNCHRONOUS_IO_NONALERT|FILE_NON_DIRECTORY_FILE )) *handle = 0;
    }
found:
    RtlFreeUnicodeString( &nt_name );
    RtlFreeHeap( GetProcessHeap(), 0, dllname );
    my_free(sysdir);
    return STATUS_SUCCESS;

overflow:
    RtlFreeUnicodeString( &nt_name );
    RtlFreeHeap( GetProcessHeap(), 0, dllname );
    *size = len + sizeof(WCHAR);
    my_free(sysdir);
    return STATUS_BUFFER_TOO_SMALL;
}


/***********************************************************************
 *	load_dll  (internal)
 *
 * Load a PE style module according to the load order.
 * The loader_section must be locked while calling this function.
 */
static NTSTATUS load_dll( LPCWSTR load_path, LPCWSTR libname, DWORD flags, WINE_MODREF** pwm )
{
    enum loadorder loadorder;
    WCHAR buffer[64];
    WCHAR *filename;
    ULONG size;
    WINE_MODREF *main_exe;
    HANDLE handle = 0;
    NTSTATUS nts;

    WINE_TRACE( "looking for %s in %s\n", wine_dbgstr_w(libname), wine_dbgstr_w(load_path) );

    *pwm = NULL;
    filename = buffer;
    size = sizeof(buffer);
    for (;;)
    {
        nts = find_dll_file( load_path, libname, filename, &size, pwm, &handle );
        if (nts == STATUS_SUCCESS) break;
        if (filename != buffer) RtlFreeHeap( GetProcessHeap(), 0, filename );
        if (nts != STATUS_BUFFER_TOO_SMALL) return nts;
        /* grow the buffer and retry */
        if (!(filename = RtlAllocateHeap( GetProcessHeap(), 0, size ))) return STATUS_NO_MEMORY;
    }

    if (*pwm)  /* found already loaded module */
    {
        if ((*pwm)->ldr.LoadCount != -1) (*pwm)->ldr.LoadCount++;

        WINE_TRACE("Found %s for %s at %p, count=%d\n",
              wine_dbgstr_w((*pwm)->ldr.FullDllName.Buffer), wine_dbgstr_w(libname),
              (*pwm)->ldr.DllBase, (*pwm)->ldr.LoadCount);
        if (filename != buffer) RtlFreeHeap( GetProcessHeap(), 0, filename );
        return STATUS_SUCCESS;
    }

    main_exe = get_modref( qemu_getTEB()->Peb->ImageBaseAddress );
    loadorder = get_load_order( main_exe ? main_exe->ldr.BaseDllName.Buffer : NULL, filename );

    if (handle && is_fake_dll( handle ))
    {
        WINE_TRACE( "%s is a fake Wine dll\n", wine_dbgstr_w(filename) );
        NtClose( handle );
        handle = 0;
    }

    switch(loadorder)
    {
    case LO_INVALID:
        nts = STATUS_NO_MEMORY;
        break;
    case LO_DISABLED:
        nts = STATUS_DLL_NOT_FOUND;
        break;
    case LO_NATIVE:
    case LO_NATIVE_BUILTIN:
        if (!handle) nts = STATUS_DLL_NOT_FOUND;
        else
        {
            nts = load_native_dll( load_path, filename, handle, flags, pwm );
        }
        break;
    case LO_BUILTIN:
    case LO_BUILTIN_NATIVE:
    case LO_DEFAULT:  /* default is builtin,native */
        WINE_ERR("Unexpected codepath.\n");
    }

    if (nts == STATUS_SUCCESS)
    {
        /* Initialize DLL just loaded */
        WINE_TRACE("Loaded module %s (%s) at %p\n", wine_dbgstr_w(filename),
              ((*pwm)->ldr.Flags & LDR_WINE_INTERNAL) ? "builtin" : "native",
              (*pwm)->ldr.DllBase);
        if (handle) NtClose( handle );
        if (filename != buffer) RtlFreeHeap( GetProcessHeap(), 0, filename );
        return nts;
    }

    WINE_WARN("Failed to load module %s; status=%x\n", wine_dbgstr_w(libname), nts);
    if (handle) NtClose( handle );
    if (filename != buffer) RtlFreeHeap( GetProcessHeap(), 0, filename );
    return nts;
}

NTSTATUS qemu_LdrLoadDll(LPCWSTR path_name, DWORD flags,
        const UNICODE_STRING *libname, HMODULE* hModule)
{
    WINE_MODREF *wm;
    NTSTATUS nts;
    ULONG_PTR magic;

    LdrLockLoaderLock( 0, NULL, &magic );

    if (!path_name) path_name = qemu_getTEB()->Peb->ProcessParameters->DllPath.Buffer;
    nts = load_dll( path_name, libname->Buffer, flags, &wm );

    if (nts == STATUS_SUCCESS && !(wm->ldr.Flags & LDR_DONT_RESOLVE_REFS))
    {
        nts = process_attach( wm, NULL );
        if (nts != STATUS_SUCCESS)
        {
            qemu_LdrUnloadDll(wm->ldr.DllBase);
            wm = NULL;
        }
    }
    *hModule = (wm) ? wm->ldr.DllBase : NULL;

    LdrUnlockLoaderLock( 0, magic );
    return nts;
}


/******************************************************************
 *		LdrGetDllHandle (NTDLL.@)
 */
NTSTATUS qemu_LdrGetDllHandle( LPCWSTR load_path, ULONG flags, const UNICODE_STRING *name, HMODULE *base )
{
    NTSTATUS status;
    WCHAR buffer[128];
    WCHAR *filename;
    ULONG size;
    WINE_MODREF *wm;
    ULONG_PTR magic;

    LdrLockLoaderLock( 0, NULL, &magic );

    if (!load_path) load_path = qemu_getTEB()->Peb->ProcessParameters->DllPath.Buffer;

    filename = buffer;
    size = sizeof(buffer);
    for (;;)
    {
        status = find_dll_file( load_path, name->Buffer, filename, &size, &wm, NULL );
        if (filename != buffer) RtlFreeHeap( GetProcessHeap(), 0, filename );
        if (status != STATUS_BUFFER_TOO_SMALL) break;
        /* grow the buffer and retry */
        if (!(filename = RtlAllocateHeap( GetProcessHeap(), 0, size )))
        {
            status = STATUS_NO_MEMORY;
            break;
        }
    }

    if (status == STATUS_SUCCESS)
    {
        if (wm) *base = wm->ldr.DllBase;
        else status = STATUS_DLL_NOT_FOUND;
    }

    LdrUnlockLoaderLock( 0, magic );
    WINE_TRACE( "%s -> %p (load path %s)\n", debugstr_us(name), status ? NULL : *base, wine_dbgstr_w(load_path) );
    return status;
}


/******************************************************************
 *		LdrAddRefDll (NTDLL.@)
 */
static NTSTATUS qemu_LdrAddRefDll( ULONG flags, HMODULE module )
{
    NTSTATUS ret = STATUS_SUCCESS;
    WINE_MODREF *wm;
    ULONG_PTR magic;

    if (flags & ~LDR_ADDREF_DLL_PIN) WINE_FIXME( "%p flags %x not implemented\n", module, flags );

    LdrLockLoaderLock( 0, NULL, &magic );

    if ((wm = get_modref( module )))
    {
        if (flags & LDR_ADDREF_DLL_PIN)
            wm->ldr.LoadCount = -1;
        else
            if (wm->ldr.LoadCount != -1) wm->ldr.LoadCount++;
        WINE_TRACE( "(%s) ldr.LoadCount: %d\n", wine_dbgstr_w(wm->ldr.BaseDllName.Buffer), wm->ldr.LoadCount );
    }
    else ret = STATUS_INVALID_PARAMETER;

    LdrUnlockLoaderLock( 0, magic );
    return ret;
}

static NTSTATUS query_dword_option( HANDLE hkey, LPCWSTR name, ULONG *value )
{
    NTSTATUS status;
    UNICODE_STRING str;
    ULONG size;
    WCHAR buffer[64];
    KEY_VALUE_PARTIAL_INFORMATION *info = (KEY_VALUE_PARTIAL_INFORMATION *)buffer;

    RtlInitUnicodeString( &str, name );

    size = sizeof(buffer) - sizeof(WCHAR);
    if ((status = NtQueryValueKey( hkey, &str, KeyValuePartialInformation, buffer, size, &size )))
        return status;

    if (info->Type != REG_DWORD)
    {
        buffer[size / sizeof(WCHAR)] = 0;
        *value = wcstoul( (WCHAR *)info->Data, 0, 16 );
    }
    else memcpy( value, info->Data, sizeof(*value) );
    return status;
}

static NTSTATUS query_string_option( HANDLE hkey, LPCWSTR name, ULONG type,
                                     void *data, ULONG in_size, ULONG *out_size )
{
    NTSTATUS status;
    UNICODE_STRING str;
    ULONG size;
    char *buffer;
    KEY_VALUE_PARTIAL_INFORMATION *info;
    static const int info_size = FIELD_OFFSET( KEY_VALUE_PARTIAL_INFORMATION, Data );

    RtlInitUnicodeString( &str, name );

    size = info_size + in_size;
    if (!(buffer = RtlAllocateHeap( GetProcessHeap(), 0, size ))) return STATUS_NO_MEMORY;
    info = (KEY_VALUE_PARTIAL_INFORMATION *)buffer;
    status = NtQueryValueKey( hkey, &str, KeyValuePartialInformation, buffer, size, &size );
    if (!status || status == STATUS_BUFFER_OVERFLOW)
    {
        if (out_size) *out_size = info->DataLength;
        if (data && !status) memcpy( data, info->Data, info->DataLength );
    }
    RtlFreeHeap( GetProcessHeap(), 0, buffer );
    return status;
}


/******************************************************************
 *		LdrQueryImageFileExecutionOptions  (NTDLL.@)
 */
static NTSTATUS qemu_LdrQueryImageFileExecutionOptions( const UNICODE_STRING *key, LPCWSTR value, ULONG type,
                                                   void *data, ULONG in_size, ULONG *out_size )
{
    static const WCHAR optionsW[] = {'M','a','c','h','i','n','e','\\',
                                     'S','o','f','t','w','a','r','e','\\',
                                     'M','i','c','r','o','s','o','f','t','\\',
                                     'W','i','n','d','o','w','s',' ','N','T','\\',
                                     'C','u','r','r','e','n','t','V','e','r','s','i','o','n','\\',
                                     'I','m','a','g','e',' ','F','i','l','e',' ',
                                     'E','x','e','c','u','t','i','o','n',' ','O','p','t','i','o','n','s','\\'};
    WCHAR path[MAX_PATH + sizeof(optionsW)/sizeof(WCHAR)];
    OBJECT_ATTRIBUTES attr;
    UNICODE_STRING name_str;
    HANDLE hkey;
    NTSTATUS status;
    ULONG len;
    WCHAR *p;

    attr.Length = sizeof(attr);
    attr.RootDirectory = 0;
    attr.ObjectName = &name_str;
    attr.Attributes = OBJ_CASE_INSENSITIVE;
    attr.SecurityDescriptor = NULL;
    attr.SecurityQualityOfService = NULL;

    if ((p = memrchrW( key->Buffer, '\\', key->Length / sizeof(WCHAR) ))) p++;
    else p = key->Buffer;
    len = key->Length - (p - key->Buffer) * sizeof(WCHAR);
    name_str.Buffer = path;
    name_str.Length = sizeof(optionsW) + len;
    name_str.MaximumLength = name_str.Length;
    memcpy( path, optionsW, sizeof(optionsW) );
    memcpy( path + sizeof(optionsW)/sizeof(WCHAR), p, len );
    if ((status = NtOpenKey( &hkey, KEY_QUERY_VALUE, &attr ))) return status;

    if (type == REG_DWORD)
    {
        if (out_size) *out_size = sizeof(ULONG);
        if (in_size >= sizeof(ULONG)) status = query_dword_option( hkey, value, data );
        else status = STATUS_BUFFER_OVERFLOW;
    }
    else status = query_string_option( hkey, value, type, data, in_size, out_size );

    NtClose( hkey );
    return status;
}

/****************************************************************************
 *              LdrResolveDelayLoadedAPI   (NTDLL.@)
 */
void* qemu_LdrResolveDelayLoadedAPI( void* base, const IMAGE_DELAYLOAD_DESCRIPTOR* desc,
        void *dllhook, void* syshook, IMAGE_THUNK_DATA* addr, ULONG flags )
{
    IMAGE_THUNK_DATA64 *pIAT64, *pINT64;
    IMAGE_THUNK_DATA32 *pIAT32, *pINT32;
    DELAYLOAD_INFO delayinfo;
    UNICODE_STRING mod;
    const CHAR* name;
    HMODULE *phmod, mod64;
    NTSTATUS nts;
    FARPROC fp;
    DWORD id;
    ULONGLONG ord;

    WINE_TRACE("(%p, %p, %p, %p, %p, 0x%08x), partial stub\n", base, desc, dllhook, syshook, addr, flags);

    phmod = get_rva(base, desc->ModuleHandleRVA);
    pIAT64 = get_rva(base, desc->ImportAddressTableRVA);
    pIAT32 = (IMAGE_THUNK_DATA32 *)pIAT64;
    pINT64 = get_rva(base, desc->ImportNameTableRVA);
    pINT32 = (IMAGE_THUNK_DATA32 *)pINT64;
    name = get_rva(base, desc->DllNameRVA);
    id = is_32_bit ? ((IMAGE_THUNK_DATA32 *)addr) - pIAT32 : ((IMAGE_THUNK_DATA64 *)addr) - pIAT64;

    mod64 = is_32_bit ? (HMODULE)(ULONG_PTR)(*((qemu_ptr *)phmod)) : *phmod;
    if (!mod64)
    {
        LPCWSTR load_path = MODULE_get_dll_load_path(NULL); /* FIXME */

        if (!RtlCreateUnicodeStringFromAsciiz(&mod, name))
        {
            nts = STATUS_NO_MEMORY;
            goto fail;
        }
        nts = qemu_LdrLoadDll(load_path, 0, &mod, &mod64);
        RtlFreeUnicodeString(&mod);
        if (nts) goto fail;
        if (is_32_bit)
            *((qemu_ptr *)phmod) = (ULONG_PTR)mod64;
        else
            *phmod = mod64;
    }

    ord = is_32_bit ? pINT32[id].u1.Ordinal : pINT64[id].u1.Ordinal;

    if (IMAGE_SNAP_BY_ORDINAL(ord))
        nts = qemu_LdrGetProcedureAddress(mod64, NULL, LOWORD(ord), (void**)&fp);
    else
    {
        const IMAGE_IMPORT_BY_NAME* iibn;
        ANSI_STRING fnc;

        if (is_32_bit)
            iibn = get_rva(base, pINT32[id].u1.AddressOfData);
        else
            iibn = get_rva(base, pINT64[id].u1.AddressOfData);

        RtlInitAnsiString(&fnc, (char*)iibn->Name);
        nts = qemu_LdrGetProcedureAddress(mod64, &fnc, 0, (void**)&fp);
    }
    if (!nts)
    {
        if (is_32_bit)
            pIAT32[id].u1.Function = (ULONG_PTR)fp;
        else
            pIAT64[id].u1.Function = (ULONG_PTR)fp;
        return fp;
    }

fail:
    /* FIXME: This has 32/64 bit specific stuff. Currently the caller will write a WINE_FIXME. */
    delayinfo.Size = sizeof(delayinfo);
    delayinfo.DelayloadDescriptor = desc;
    delayinfo.ThunkAddress = addr;
    delayinfo.TargetDllName = name;
    delayinfo.TargetApiDescriptor.ImportDescribedByName = !IMAGE_SNAP_BY_ORDINAL(pINT64[id].u1.Ordinal);
    delayinfo.TargetApiDescriptor.Description.Ordinal = LOWORD(pINT64[id].u1.Ordinal);
    delayinfo.TargetModuleBase = *phmod;
    delayinfo.Unused = NULL;
    delayinfo.LastError = nts;
    return ((PDELAYLOAD_FAILURE_DLL_CALLBACK)dllhook)(4, &delayinfo);
}

#if 0
/******************************************************************
 *		LdrShutdownProcess (NTDLL.@)
 *
 */
void WINAPI LdrShutdownProcess(void)
{
    WINE_TRACE("()\n");
    process_detaching = TRUE;
    process_detach();
}


/******************************************************************
 *		RtlExitUserProcess (NTDLL.@)
 */
void WINAPI RtlExitUserProcess( DWORD status )
{
    LdrLockLoaderLock( 0, NULL, &magic );
    RtlAcquirePebLock();
    NtTerminateProcess( 0, status );
    LdrShutdownProcess();
    NtTerminateProcess( GetCurrentProcess(), status );
    exit( status );
}

/******************************************************************
 *		LdrShutdownThread (NTDLL.@)
 *
 */
void WINAPI LdrShutdownThread(void)
{
    PLIST_ENTRY mark, entry;
    PLDR_DATA_TABLE_ENTRY mod;
    UINT i;
    void **pointers;

    WINE_TRACE("()\n");

    /* don't do any detach calls if process is exiting */
    if (process_detaching) return;

    LdrLockLoaderLock( 0, NULL, &magic );

    mark = &qemu_getTEB()->Peb->LdrData->InInitializationOrderModuleList;
    for (entry = mark->Blink; entry != mark; entry = entry->Blink)
    {
        mod = CONTAINING_RECORD(entry, LDR_DATA_TABLE_ENTRY, 
                                InInitializationOrderLinks);
        if ( !(mod->Flags & LDR_PROCESS_ATTACHED) )
            continue;
        if ( mod->Flags & LDR_NO_DLL_CALLS )
            continue;

        MODULE_InitDLL( CONTAINING_RECORD(mod, WINE_MODREF, ldr), 
                        DLL_THREAD_DETACH, NULL );
    }

    RtlAcquirePebLock();
    RemoveEntryList( &qemu_getTEB()->TlsLinks );
    RtlReleasePebLock();

    if ((pointers = qemu_getTEB()->ThreadLocalStoragePointer))
    {
        for (i = 0; i < tls_module_count; i++) RtlFreeHeap( GetProcessHeap(), 0, pointers[i] );
        RtlFreeHeap( GetProcessHeap(), 0, pointers );
    }
    RtlFreeHeap( GetProcessHeap(), 0, qemu_getTEB()->FlsSlots );
    RtlFreeHeap( GetProcessHeap(), 0, qemu_getTEB()->TlsExpansionSlots );
    LdrUnlockLoaderLock( 0, magic );
}
#endif


/***********************************************************************
 *           free_modref
 *
 */
static void free_modref( WINE_MODREF *wm )
{
    RemoveEntryList(&wm->ldr.InLoadOrderLinks);
    RemoveEntryList(&wm->ldr.InMemoryOrderLinks);
    if (wm->ldr.InInitializationOrderLinks.Flink)
        RemoveEntryList(&wm->ldr.InInitializationOrderLinks);

    WINE_TRACE(" unloading %s\n", wine_dbgstr_w(wm->ldr.FullDllName.Buffer));
    WINE_TRACE("Unloaded module %s : %s\n",
                    wine_dbgstr_w(wm->ldr.FullDllName.Buffer),
                    (wm->ldr.Flags & LDR_WINE_INTERNAL) ? "builtin" : "native" );

    mmap_lock();
    tb_invalidate_phys_range((ULONG_PTR)wm->ldr.DllBase,
            (ULONG_PTR)wm->ldr.DllBase + (ULONG_PTR)wm->ldr.SizeOfImage);
    mmap_unlock();

    free_tls_slot( &wm->ldr );
    RtlReleaseActivationContext( wm->ldr.ActivationContext );
    //if (wm->ldr.Flags & LDR_WINE_INTERNAL) wine_dll_unload( wm->ldr.SectionHandle );
    VirtualFree(wm->ldr.DllBase, 0, MEM_RELEASE);
    if (cached_modref == wm) cached_modref = NULL;
    RtlFreeUnicodeString( &wm->ldr.FullDllName );
    RtlFreeHeap( GetProcessHeap(), 0, wm->deps );
    RtlFreeHeap( GetProcessHeap(), 0, wm );
}

/***********************************************************************
 *           MODULE_FlushModrefs
 *
 * Remove all unused modrefs and call the internal unloading routines
 * for the library type.
 *
 * The loader_section must be locked while calling this function.
 */
static void MODULE_FlushModrefs(void)
{
    PLIST_ENTRY mark, entry, prev;
    PLDR_DATA_TABLE_ENTRY mod;
    WINE_MODREF*wm;

    mark = &qemu_getTEB()->Peb->LdrData->InInitializationOrderModuleList;
    for (entry = mark->Blink; entry != mark; entry = prev)
    {
        mod = CONTAINING_RECORD(entry, LDR_DATA_TABLE_ENTRY, InInitializationOrderLinks);
        wm = CONTAINING_RECORD(mod, WINE_MODREF, ldr);
        prev = entry->Blink;
        if (!mod->LoadCount) free_modref( wm );
    }

    /* check load order list too for modules that haven't been initialized yet */
    mark = &qemu_getTEB()->Peb->LdrData->InLoadOrderModuleList;
    for (entry = mark->Blink; entry != mark; entry = prev)
    {
        mod = CONTAINING_RECORD(entry, LDR_DATA_TABLE_ENTRY, InLoadOrderLinks);
        wm = CONTAINING_RECORD(mod, WINE_MODREF, ldr);
        prev = entry->Blink;
        if (!mod->LoadCount) free_modref( wm );
    }
}

/***********************************************************************
 *           MODULE_DecRefCount
 *
 * The loader_section must be locked while calling this function.
 */
static void MODULE_DecRefCount( WINE_MODREF *wm )
{
    int i;

    if ( wm->ldr.Flags & LDR_UNLOAD_IN_PROGRESS )
        return;

    if ( wm->ldr.LoadCount <= 0 )
        return;

    --wm->ldr.LoadCount;
    WINE_TRACE("(%s) ldr.LoadCount: %d\n", wine_dbgstr_w(wm->ldr.BaseDllName.Buffer), wm->ldr.LoadCount );

    if ( wm->ldr.LoadCount == 0 )
    {
        wm->ldr.Flags |= LDR_UNLOAD_IN_PROGRESS;

        for ( i = 0; i < wm->nDeps; i++ )
            if ( wm->deps[i] )
                MODULE_DecRefCount( wm->deps[i] );

        wm->ldr.Flags &= ~LDR_UNLOAD_IN_PROGRESS;
    }
}

/******************************************************************
 *		LdrUnloadDll (NTDLL.@)
 *
 *
 */
static NTSTATUS qemu_LdrUnloadDll( HMODULE hModule )
{
    WINE_MODREF *wm;
    NTSTATUS retv = STATUS_SUCCESS;
    ULONG_PTR magic;

    if (process_detaching) return retv;

    WINE_TRACE("(%p)\n", hModule);

    LdrLockLoaderLock( 0, NULL, &magic );

    free_lib_count++;
    if ((wm = get_modref( hModule )) != NULL)
    {
        WINE_TRACE("(%s) - START\n", wine_dbgstr_w(wm->ldr.BaseDllName.Buffer));

        /* Recursively decrement reference counts */
        MODULE_DecRefCount( wm );

        /* Call process detach notifications */
        if ( free_lib_count <= 1 )
        {
            process_detach();
            MODULE_FlushModrefs();
        }

        WINE_TRACE("END\n");
    }
    else
        retv = STATUS_DLL_NOT_FOUND;

    free_lib_count--;

    LdrUnlockLoaderLock( 0, magic );

    return retv;
}

/***********************************************************************
 *           attach_process_dlls
 *
 * Initial attach to all the dlls loaded by the process.
 */
static NTSTATUS attach_process_dlls( void *wm )
{
    NTSTATUS status;
    ULONG_PTR magic;

    LdrLockLoaderLock( 0, NULL, &magic );
    if ((status = process_attach( wm, (LPVOID)1 )) != STATUS_SUCCESS)
    {
        if (last_failed_modref)
            WINE_ERR( "%s failed to initialize, aborting\n",
                 wine_dbgstr_w(last_failed_modref->ldr.BaseDllName.Buffer) + 1 );
        return status;
    }
    attach_implicitly_loaded_dlls( (LPVOID)1 );
    imports_fixup_done = TRUE;
    LdrUnlockLoaderLock( 0, magic );
    return status;
}

static void version_init( const WCHAR *appname )
{
    /* Just copy the info from the host TEB. */
    RTL_OSVERSIONINFOEXW current_version;

    current_version.dwMajorVersion = NtCurrentTeb()->Peb->OSMajorVersion;
    current_version.dwMinorVersion = NtCurrentTeb()->Peb->OSMinorVersion;
    current_version.dwBuildNumber = NtCurrentTeb()->Peb->OSBuildNumber;
    current_version.dwPlatformId = NtCurrentTeb()->Peb->OSPlatformId;

    qemu_getTEB()->Peb->OSMajorVersion = current_version.dwMajorVersion;
    qemu_getTEB()->Peb->OSMinorVersion = current_version.dwMinorVersion;
    qemu_getTEB()->Peb->OSBuildNumber  = current_version.dwBuildNumber;
    qemu_getTEB()->Peb->OSPlatformId   = current_version.dwPlatformId;

    WINE_TRACE( "got %d.%d platform %d build %x name %s service pack %d.%d product %d\n",
           current_version.dwMajorVersion, current_version.dwMinorVersion,
           current_version.dwPlatformId, current_version.dwBuildNumber,
           wine_dbgstr_w(current_version.szCSDVersion),
           current_version.wServicePackMajor, current_version.wServicePackMinor,
           current_version.wProductType );
}

static const WCHAR *get_dll_system_path(void)
{
    static WCHAR *cached_path;

    if (!cached_path)
    {
        WCHAR *p, *path;
        int len = 1;

        len += 2 * GetSystemDirectoryW( NULL, 0 );
        len += GetWindowsDirectoryW( NULL, 0 );
        p = path = HeapAlloc( GetProcessHeap(), 0, len * sizeof(WCHAR) );
        GetSystemDirectoryW( p, path + len - p);
        p += strlenW(p);
        /* if system directory ends in "32" add 16-bit version too */
        if (p[-2] == '3' && p[-1] == '2')
        {
            *p++ = ';';
            GetSystemDirectoryW( p, path + len - p);
            p += strlenW(p) - 2;
        }
        *p++ = ';';
        GetWindowsDirectoryW( p, path + len - p);
        cached_path = path;
    }
    return cached_path;
}

static inline const WCHAR *get_module_path_end(const WCHAR *module)
{
    const WCHAR *p;
    const WCHAR *mod_end = module;
    if (!module) return mod_end;

    if ((p = strrchrW( mod_end, '\\' ))) mod_end = p;
    if ((p = strrchrW( mod_end, '/' ))) mod_end = p;
    if (mod_end == module + 2 && module[1] == ':') mod_end++;
    if (mod_end == module && module[0] && module[1] == ':') mod_end += 2;

    return mod_end;
}

static WCHAR *MODULE_get_dll_load_path(const WCHAR *module)
{
    static const WCHAR pathW[] = {'P','A','T','H',0};

    const WCHAR *system_path = get_dll_system_path();
    const WCHAR *mod_end = NULL;
    UNICODE_STRING name, value;
    WCHAR *p, *ret;
    int len = 0, path_len = 0;

    /* adjust length for module name */

    if (module)
        mod_end = get_module_path_end( module );
    /* if module is NULL or doesn't contain a path, fall back to directory
     * process was loaded from */
    if (module == mod_end)
    {
        module = qemu_getTEB()->Peb->ProcessParameters->ImagePathName.Buffer;
        mod_end = get_module_path_end( module );
    }
    len += (mod_end - module) + 1;

    len += strlenW( system_path ) + 2;

    /* get the PATH variable */

    RtlInitUnicodeString( &name, pathW );
    value.Length = 0;
    value.MaximumLength = 0;
    value.Buffer = NULL;
    if (RtlQueryEnvironmentVariable_U( NULL, &name, &value ) == STATUS_BUFFER_TOO_SMALL)
        path_len = value.Length;

    RtlEnterCriticalSection( &dlldir_section );
    if (dll_directory) len += strlenW(dll_directory) + 1;
    if ((p = ret = HeapAlloc( GetProcessHeap(), 0, path_len + len * sizeof(WCHAR) )))
    {
        if (module)
        {
            memcpy( ret, module, (mod_end - module) * sizeof(WCHAR) );
            p += (mod_end - module);
            *p++ = ';';
        }
        if (dll_directory)
        {
            strcpyW( p, dll_directory );
            p += strlenW(p);
            *p++ = ';';
        }
    }
    RtlLeaveCriticalSection( &dlldir_section );
    if (!ret) return NULL;

    strcpyW( p, system_path );
    p += strlenW(p);
    *p++ = ';';
    value.Buffer = p;
    value.MaximumLength = path_len;

    while (RtlQueryEnvironmentVariable_U( NULL, &name, &value ) == STATUS_BUFFER_TOO_SMALL)
    {
        WCHAR *new_ptr;

        /* grow the buffer and retry */
        path_len = value.Length;
        if (!(new_ptr = HeapReAlloc( GetProcessHeap(), 0, ret, path_len + len * sizeof(WCHAR) )))
        {
            HeapFree( GetProcessHeap(), 0, ret );
            return NULL;
        }
        value.Buffer = new_ptr + (value.Buffer - ret);
        value.MaximumLength = path_len;
        ret = new_ptr;
    }
    value.Buffer[value.Length / sizeof(WCHAR)] = 0;
    return ret;
}

/******************************************************************
 *		LdrInitializeThunk (NTDLL.@)
 *
 * Slightly different from what Wine is doing.
 */
NTSTATUS qemu_LdrInitializeThunk(void)
{
    static const WCHAR globalflagW[] = {'G','l','o','b','a','l','F','l','a','g',0};
    NTSTATUS status;
    WINE_MODREF *wm;
    LPCWSTR load_path;
    PEB *peb = qemu_getTEB()->Peb;
    PEB *hostpeb = NtCurrentTeb()->Peb;

    if (main_exe_file) NtClose( main_exe_file );  /* at this point the main module is created */

    /* allocate the modref for the main exe (if not already done) */
    wm = get_modref( peb->ImageBaseAddress );
    assert( wm );
    if (wm->ldr.Flags & LDR_IMAGE_IS_DLL)
    {
        WINE_ERR("%s is a dll, not an executable\n", wine_dbgstr_w(wm->ldr.FullDllName.Buffer) );
        exit(1);
    }

    peb->LoaderLock = hostpeb->LoaderLock; /* FIXME: Am I not going to run into trouble with Win32 here? */
    peb->ProcessParameters->ImagePathName = wm->ldr.FullDllName;
    if (!peb->ProcessParameters->WindowTitle.Buffer)
        peb->ProcessParameters->WindowTitle = wm->ldr.FullDllName;
    version_init( wm->ldr.FullDllName.Buffer );

    qemu_LdrQueryImageFileExecutionOptions( &peb->ProcessParameters->ImagePathName, globalflagW,
                                       REG_DWORD, &peb->NtGlobalFlag, sizeof(peb->NtGlobalFlag), NULL );

    /* the main exe needs to be the first in the load order list */
    RemoveEntryList( &wm->ldr.InLoadOrderLinks );
    InsertHeadList( &peb->LdrData->InLoadOrderModuleList, &wm->ldr.InLoadOrderLinks );
    RemoveEntryList( &wm->ldr.InMemoryOrderLinks );
    InsertHeadList( &peb->LdrData->InMemoryOrderModuleList, &wm->ldr.InMemoryOrderLinks );

    /* TODO: Integrate stack allocation similarly to the way Wine handles this. */

    /* actctx_init(); */
    load_path = qemu_getTEB()->Peb->ProcessParameters->DllPath.Buffer;
    load_path = MODULE_get_dll_load_path(NULL); /* FIXME! */
    if ((status = fixup_imports( wm, load_path )) != STATUS_SUCCESS) goto error;

    status = attach_process_dlls(wm);
    if (status != STATUS_SUCCESS) goto error;

    return status;

error:
    WINE_ERR( "Main exe initialization for %s failed, status %x\n",
         wine_dbgstr_w(peb->ProcessParameters->ImagePathName.Buffer), status );
    return status;
}

void qemu_loader_thread_init(void)
{
    TEB *teb = qemu_getTEB();
    RtlAcquirePebLock();
    InsertHeadList(&tls_links, &teb->TlsLinks);
    RtlReleasePebLock();
}

void qemu_loader_thread_stop(void)
{
    TEB *teb = qemu_getTEB();
    RtlAcquirePebLock();
    RemoveEntryList(&teb->TlsLinks);
    RtlReleasePebLock();
}

BOOL qemu_FreeLibrary(HMODULE module)
{
    BOOL                retv = FALSE;
    NTSTATUS            nts;

    if (!module)
    {
        SetLastError( ERROR_INVALID_HANDLE );
        return FALSE;
    }

    if ((ULONG_PTR)module & 1)
    {
        /* this is a LOAD_LIBRARY_AS_DATAFILE module */
        char *ptr = (char *)module - 1;
        return UnmapViewOfFile( ptr );
    }

    if ((nts = qemu_LdrUnloadDll( module )) == STATUS_SUCCESS) retv = TRUE;
    else SetLastError( RtlNtStatusToDosError( nts ) );

    return retv;
}

void *qemu_RtlPcToFileHeader(void *pc, void **address)
{
    LDR_DATA_TABLE_ENTRY *module;
    PVOID ret = NULL;
    ULONG_PTR magic;

    LdrLockLoaderLock( 0, NULL, &magic );
    if (!qemu_LdrFindEntryForAddress( pc, &module )) ret = module->DllBase;
    LdrUnlockLoaderLock( 0, magic );
    *address = ret;
    return ret;
}

HMODULE qemu_GetModuleHandleEx(DWORD flags, const WCHAR *name)
{
    NTSTATUS status = STATUS_SUCCESS;
    HMODULE ret = NULL;
    ULONG_PTR magic;
    BOOL lock;

    /* if we are messing with the refcount, grab the loader lock */
    lock = (flags & GET_MODULE_HANDLE_EX_FLAG_PIN) || !(flags & GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT);
    if (lock)
        LdrLockLoaderLock( 0, NULL, &magic );

    if (!name)
    {
        ret = qemu_getTEB()->Peb->ImageBaseAddress;
    }
    else if (flags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS)
    {
        void *dummy;
        if (!(ret = RtlPcToFileHeader( (void *)name, &dummy ))) status = STATUS_DLL_NOT_FOUND;
    }
    else
    {
        UNICODE_STRING wstr;
        RtlInitUnicodeString( &wstr, name );
        /* FIXME: Should not need the path. Init DllPath in PEB properly. */
        status = qemu_LdrGetDllHandle( MODULE_get_dll_load_path(NULL), 0, &wstr, &ret );
    }

    if (status == STATUS_SUCCESS)
    {
        if (flags & GET_MODULE_HANDLE_EX_FLAG_PIN)
            qemu_LdrAddRefDll( LDR_ADDREF_DLL_PIN, ret );
        else if (!(flags & GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT))
            qemu_LdrAddRefDll( 0, ret );
    }
    else SetLastError( RtlNtStatusToDosError( status ) );

    if (lock)
        LdrUnlockLoaderLock( 0, magic );

    if (status == STATUS_SUCCESS) return ret;
    else return NULL;
}

DWORD qemu_GetModuleFileName(HMODULE hModule, LPWSTR lpFileName, DWORD size)
{
    ULONG len = 0;
    ULONG_PTR magic;
    LDR_DATA_TABLE_ENTRY *pldr;
    NTSTATUS nts;

    LdrLockLoaderLock( 0, NULL, &magic );

    if (!hModule) hModule = qemu_getTEB()->Peb->ImageBaseAddress;
    nts = qemu_LdrFindEntryForAddress( hModule, &pldr );
    if (nts == STATUS_SUCCESS)
    {
        len = min(size, pldr->FullDllName.Length / sizeof(WCHAR));
        memcpy(lpFileName, pldr->FullDllName.Buffer, len * sizeof(WCHAR));
        if (len < size)
        {
            lpFileName[len] = '\0';
            SetLastError( 0 );
        }
        else
            SetLastError( ERROR_INSUFFICIENT_BUFFER );
    }
    else SetLastError( RtlNtStatusToDosError( nts ) );

    LdrUnlockLoaderLock( 0, magic );

    WINE_TRACE( "%s\n", wine_dbgstr_wn(lpFileName, len) );
    return len;
}

static BOOL load_library_as_datafile( LPCWSTR name, HMODULE* hmod)
{
    static const WCHAR dotDLL[] = {'.','d','l','l',0};

    WCHAR filenameW[MAX_PATH];
    HANDLE hFile = INVALID_HANDLE_VALUE;
    HANDLE mapping;
    HMODULE module;

    *hmod = 0;

    if (SearchPathW( NULL, name, dotDLL, sizeof(filenameW) / sizeof(filenameW[0]),
                     filenameW, NULL ))
    {
        hFile = CreateFileW( filenameW, GENERIC_READ, FILE_SHARE_READ,
                             NULL, OPEN_EXISTING, 0, 0 );
    }
    if (hFile == INVALID_HANDLE_VALUE) return FALSE;

    mapping = CreateFileMappingW( hFile, NULL, PAGE_READONLY, 0, 0, NULL );
    CloseHandle( hFile );
    if (!mapping) return FALSE;

    module = MapViewOfFile( mapping, FILE_MAP_READ, 0, 0, 0 );
    CloseHandle( mapping );
    if (!module) return FALSE;

    /* make sure it's a valid PE file */
    if (!RtlImageNtHeader(module))
    {
        UnmapViewOfFile( module );
        return FALSE;
    }
    *hmod = (HMODULE)((char *)module + 1);  /* set low bit of handle to indicate datafile module */
    return TRUE;
}

HMODULE qemu_LoadLibrary(const WCHAR *name, DWORD flags)
{
    NTSTATUS nts;
    HMODULE hModule;
    WCHAR *load_path;
    UNICODE_STRING wstr;
    static const DWORD unsupported_flags = 
        LOAD_IGNORE_CODE_AUTHZ_LEVEL |
        LOAD_LIBRARY_AS_IMAGE_RESOURCE |
        LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE |
        LOAD_LIBRARY_REQUIRE_SIGNED_TARGET |
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
        LOAD_LIBRARY_SEARCH_APPLICATION_DIR |
        LOAD_LIBRARY_SEARCH_USER_DIRS |
        LOAD_LIBRARY_SEARCH_SYSTEM32 |
        LOAD_LIBRARY_SEARCH_DEFAULT_DIRS;

    if( flags & unsupported_flags)
        WINE_FIXME("unsupported flag(s) used (flags: 0x%08x)\n", flags);

    if (!name)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }

    load_path = MODULE_get_dll_load_path( flags & LOAD_WITH_ALTERED_SEARCH_PATH ? name : NULL );

    if (flags & LOAD_LIBRARY_AS_DATAFILE)
    {
        ULONG_PTR magic;

        LdrLockLoaderLock( 0, NULL, &magic );
        RtlInitUnicodeString(&wstr, name);
        if (!qemu_LdrGetDllHandle( load_path, flags, &wstr, &hModule ))
        {
            qemu_LdrAddRefDll( 0, hModule );
            LdrUnlockLoaderLock( 0, magic );
            goto done;
        }
        LdrUnlockLoaderLock( 0, magic );

        /* The method in load_library_as_datafile allows searching for the
         * 'native' libraries only
         */
        if (load_library_as_datafile( name, &hModule )) goto done;
        flags |= DONT_RESOLVE_DLL_REFERENCES; /* Just in case */
        /* Fallback to normal behaviour */
    }

    RtlInitUnicodeString(&wstr, name);
    if (wstr.Buffer[wstr.Length/sizeof(WCHAR) - 1] != ' ')
    {
        nts = qemu_LdrLoadDll(load_path, flags, &wstr, &hModule);
    }
    else
    {
        /* Library name has trailing spaces */
        RtlCreateUnicodeString(&wstr, name);
        while (wstr.Length > sizeof(WCHAR) &&
            wstr.Buffer[wstr.Length/sizeof(WCHAR) - 1] == ' ')
        {
            wstr.Length -= sizeof(WCHAR);
        }
        wstr.Buffer[wstr.Length/sizeof(WCHAR)] = '\0';
        nts = qemu_LdrLoadDll(load_path, flags, &wstr, &hModule);
        RtlFreeUnicodeString(&wstr);
    }
    if (nts != STATUS_SUCCESS)
    {
        hModule = 0;
        if (nts == STATUS_DLL_NOT_FOUND && (GetVersion() & 0x80000000))
            SetLastError( ERROR_DLL_NOT_FOUND );
        else
            SetLastError( RtlNtStatusToDosError( nts ) );
    }

done:
    HeapFree(GetProcessHeap(), 0, load_path);
    return hModule;
}

BOOL qemu_FindEntryForAddress(void *addr, HMODULE *mod)
{
    LDR_DATA_TABLE_ENTRY *ldr_mod;
    NTSTATUS status = qemu_LdrFindEntryForAddress(addr, &ldr_mod);
    if (status != STATUS_SUCCESS)
        return FALSE;
    *mod = ldr_mod->DllBase;
    return TRUE;
}

void qemu_get_image_info(const HMODULE module, struct qemu_pe_image *info)
{
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)module;
    const struct nt_header *nt = (const struct nt_header *)((const char *)dos + dos->e_lfanew);

    info->entrypoint = (void *)((char *)module) + nt->opt.hdr64.AddressOfEntryPoint;
    info->stack_reserve = nt->opt.hdr64.SizeOfStackReserve;
    info->stack_commit = nt->opt.hdr64.SizeOfStackCommit;
}

BOOL qemu_get_ldr_module(HANDLE process, HMODULE mod, void **ldr)
{
    WINE_MODREF *wm;

    *ldr = NULL;

    /* Copypasting the code from Wine should generally work, except that we need a
     * way to identify qemu processes and return the guest PIB. */
    if (process && process != GetCurrentProcess())
    {
        WINE_FIXME("Not supported for other processes yet.\n");
        return FALSE;
    }
    wm = get_modref(mod);
    if (!wm)
        return FALSE;
    *ldr = &wm->ldr;
    return TRUE;
}

/* If the file is not a valid exe file just return 64 bit. We'll terminate later. */
BOOL qemu_get_exe_properties(const WCHAR *path, WCHAR *exename, size_t name_len, BOOL *is_32_bit,
        BOOL *large_address_aware, DWORD_PTR *base, DWORD_PTR *size)
{
    HMODULE module = qemu_LoadLibrary(path, LOAD_LIBRARY_AS_DATAFILE);
    IMAGE_NT_HEADERS *hdr;
    BOOL ret = FALSE;
    IMAGE_OPTIONAL_HEADER32 *hdr32 = NULL;
    IMAGE_OPTIONAL_HEADER64 *hdr64 = NULL;
    static const WCHAR dotEXE[] = {'.','d','l','l',0};

    if (!module)
        return FALSE;

    if (!SearchPathW( NULL, path, dotEXE, name_len, exename, NULL ))
        WINE_ERR("Could not figure out the full .exe path, but the loader code found it. Re-think this logic...\n");

    hdr = RtlImageNtHeader((HMODULE)((ULONG_PTR)module - 1));
    if (hdr)
    {
        if (hdr->FileHeader.Machine == IMAGE_FILE_MACHINE_I386)
        {
            WINE_TRACE("%s is a 32 bit app\n", wine_dbgstr_w(path));

            hdr32 = (IMAGE_OPTIONAL_HEADER32 *)&hdr->OptionalHeader;
            *is_32_bit = TRUE;
            *base = hdr32->ImageBase;
            *size = ROUND_SIZE(hdr32->SizeOfImage);
        }
        else
        {
            WINE_TRACE("%s is a 64 bit app\n", wine_dbgstr_w(path));

            hdr64 = &hdr->OptionalHeader;
            *is_32_bit = FALSE;
            *base = hdr64->ImageBase;
            *size = ROUND_SIZE(hdr64->SizeOfImage);
        }

        *large_address_aware = hdr->FileHeader.Characteristics & IMAGE_FILE_LARGE_ADDRESS_AWARE;

        ret = TRUE;
    }

    qemu_FreeLibrary(module);
    return ret;
}

/* This function returns e.g. the host user32.dll module for the guest user32.dll module. This is
 * needed for functions that look up resource data in system DLLs. Our guest wrappers currently
 * do not have this data.
 *
 * FIXME: This function probably fails for modules loaded from WinSXS because we have to replace
 * the arch-specific path. It might also errnously translate a DLL that is loaded as a full Wine
 * PE DLL but is also loaded on the host side, like ole32.dll. Ideally the latter case shouldn't
 * matter. */
HMODULE qemu_ldr_module_g2h(uint64_t guest)
{
    HMODULE ret, wrapper;
    WINE_MODREF *modref;
    WCHAR *name, name_wrap[MAX_PATH] = {0};
    static const WCHAR qemu_[] = {'q','e','m','u','_',0};
    HMODULE guest_mod = QEMU_G2H(guest);
    DWORD le = GetLastError(); /* FIXME: Contemplate using ntdll functions that don't call SetLastError(). */
    ULONG_PTR magic;

    /* Translate NULL to the guest .exe instead of qemu. This is not correct for all functions,
     * e.g. some user32 functions translate NULL to user32.dll. Handle this yourself if you
     * need different behavior. */
    WINE_TRACE("Looking for %p.\n", guest_mod);

    if (!guest_mod)
    {
        ret = qemu_GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, NULL);
        WINE_TRACE("Returning main .exe file %p.\n", ret);
        SetLastError(le);
        return ret;
    }

    LdrLockLoaderLock( 0, NULL, &magic );
    modref = get_modref(guest_mod);
    LdrUnlockLoaderLock( 0, magic );
    if (!modref)
    {
        /* Sometimes we get junk pointers in situations where the application doesn't expect
         * us to load anything from a resource, e.g. in comctl32.PropertySheet. Handle this
         * gracefully by leaving the junk untouched. Figuring out if a HMOUDLE pointer will
         * be used by the host isn't always easy. */
        WINE_WARN("Cound not find modref for module %p.\n", guest_mod);
        SetLastError(le);
        return guest_mod;
    }

    name = modref->ldr.BaseDllName.Buffer;
    WINE_TRACE("Looking for %s.\n", wine_dbgstr_w(name));
    /* Look for a host module of the same name. Note that our own DLLs either have just the DLL name,
     * or are in C:\windows\system32. Both should work for Wine DLLs. */
    lstrcatW(name_wrap, qemu_);
    lstrcatW(name_wrap, name);
    wrapper = GetModuleHandleW(name_wrap);
    ret = GetModuleHandleW(name);

    /* Ignore ourselves. We replaced the .exe name with the guest name, so we'll find ourselves
     * if we look for the guest .exe. */
    if (ret == GetModuleHandleW(NULL))
        ret = NULL;

    if (ret && wrapper)
    {
        WINE_TRACE("Found %p for %p, name %s.\n", ret, guest_mod, wine_dbgstr_w(name));
    }
    else
    {
        /* There's nothing stopping Wine from loading e.g. the host-side ole32.dll even though we are using
         * a guest build for it. Some DLLs that have host wrappers like advapi32.dll load ole32 though delay
         * load. Delay load doesn't work with PE DLLs. Assume we do not want the host DLL in this case.
         *
         * This still indicates potential problems though. E.g. if the guest calls CoInitialize it won't
         * make its way through to the host lib. */
        if (ret)
        {
            WINE_FIXME("A host %s has been found, but no wrapper named %s.\n",
                    wine_dbgstr_w(name), wine_dbgstr_w(name_wrap));
        }

        /* This means that the module we are looking for is not part of a guest-host pair,
         * but is a DLL only loaded in the host, e.g. an application DLL. */
        WINE_TRACE("Did not find %s, using guest module %p.\n", wine_dbgstr_w(name), guest_mod);
        ret = guest_mod;
    }

    SetLastError(le);
    return ret;
}

uint64_t qemu_ldr_module_h2g(HMODULE host)
{
    WINE_TRACE("Looking for %p.\n", host);
    HMODULE guest = NULL;

    /* OK, I am not entirely sure what we want here in general. What started this function
     * is the user32 class test, which expects GetClassInfo to return a class HINSTANCE pointing
     * to kernel32 in some cases. So we want to translate host kernel32 to guest kernel32 here.
     * The same might need to be done for ntdll and user32 at some point.
     *
     * So for now just single out kernel32. */
    if (!host)
        return 0;

    if (host == GetModuleHandleW(kernel32W))
    {
        guest = qemu_GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, kernel32W);
        if (!guest)
            WINE_ERR("Did not find guest kernel32.\n");
        return QEMU_H2G(guest);
    }
    else if (host == GetModuleHandleW(user32W))
    {
        guest = qemu_GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, user32W);
        if (!guest)
            WINE_ERR("Did not find guest user32.\n");
        return QEMU_H2G(guest);
    }

    return QEMU_H2G(host);
}
