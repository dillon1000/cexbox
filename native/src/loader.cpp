// Maps a PE32+ executable into this process by hand: sections, relocations,
// imports, delay imports, exception tables and page protections.
#include "xb.h"

#include <algorithm>
#include <map>
#include <stdio.h>

namespace xb {

Image g_image;

namespace {

struct Stats {
    int real = 0;
    int overridden = 0;
    int stubbed = 0;
} g_stats;

std::string lower(const char* s) {
    std::string out(s ? s : "");
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

std::string fmt(const char* f, unsigned long v) {
    char buf[256];
    snprintf(buf, sizeof buf, f, v);
    return buf;
}

bool read_at(HANDLE f, uint64_t offset, void* dst, size_t len) {
    LARGE_INTEGER li;
    li.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(f, li, nullptr, FILE_BEGIN)) return false;
    auto* p = static_cast<uint8_t*>(dst);
    while (len) {
        DWORD chunk = static_cast<DWORD>(std::min<size_t>(len, 64u << 20));
        DWORD got = 0;
        if (!ReadFile(f, p, chunk, &got, nullptr) || got == 0) return false;
        p += got;
        len -= got;
    }
    return true;
}

// Finds or loads a system DLL. Results are cached; a null entry means it isn't available.
HMODULE system_module(const std::string& dll) {
    static std::map<std::string, HMODULE> cache;
    auto it = cache.find(dll);
    if (it != cache.end()) return it->second;

    std::wstring w = widen(dll.c_str());
    HMODULE h = GetModuleHandleW(w.c_str());
    if (!h && real_LoadLibraryExW) h = real_LoadLibraryExW(w.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!h && real_LoadLibraryExW) h = real_LoadLibraryExW(w.c_str(), nullptr, 0);
    if (!h) h = LoadPackagedLibrary(w.c_str(), 0);
    log("module %s: %s", dll.c_str(), h ? "found" : "not available, its imports become stubs");
    cache[dll] = h;
    return h;
}

bool apply_relocations(Image& img, ptrdiff_t delta, std::string& err) {
    auto& dd = img.nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    if (!dd.VirtualAddress || !dd.Size) {
        err = "claude.exe could not be loaded at its preferred address and has no relocation table";
        return false;
    }
    uint8_t* p = img.base + dd.VirtualAddress;
    uint8_t* end = p + dd.Size;
    size_t applied = 0;
    while (p + sizeof(IMAGE_BASE_RELOCATION) <= end) {
        auto* block = reinterpret_cast<IMAGE_BASE_RELOCATION*>(p);
        if (block->SizeOfBlock < sizeof(IMAGE_BASE_RELOCATION)) break;
        size_t count = (block->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(WORD);
        auto* entries = reinterpret_cast<WORD*>(block + 1);
        for (size_t i = 0; i < count; ++i) {
            WORD type = entries[i] >> 12;
            WORD offset = entries[i] & 0xFFF;
            uint8_t* target = img.base + block->VirtualAddress + offset;
            switch (type) {
            case IMAGE_REL_BASED_ABSOLUTE:
                break;
            case IMAGE_REL_BASED_DIR64:
                *reinterpret_cast<uint64_t*>(target) += static_cast<uint64_t>(delta);
                ++applied;
                break;
            case IMAGE_REL_BASED_HIGHLOW:
                *reinterpret_cast<uint32_t*>(target) += static_cast<uint32_t>(delta);
                ++applied;
                break;
            default:
                err = fmt("unsupported relocation type %lu", type);
                return false;
            }
        }
        p += block->SizeOfBlock;
    }
    log("applied %zu relocations", applied);
    return true;
}

// Fills one import address table from its name table.
void bind_thunks(Image& img, const char* dll, IMAGE_THUNK_DATA64* names, IMAGE_THUNK_DATA64* iat) {
    for (; names->u1.AddressOfData; ++names, ++iat) {
        const char* name = nullptr;
        uint16_t ordinal = 0;
        if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) {
            ordinal = static_cast<uint16_t>(IMAGE_ORDINAL64(names->u1.Ordinal));
        } else {
            name = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(img.base + names->u1.AddressOfData)->Name;
        }
        iat->u1.Function = reinterpret_cast<ULONGLONG>(resolve_import(dll, name, ordinal));
    }
}

bool bind_imports(Image& img, std::string& err) {
    auto& dd = img.nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dd.Size) return true;
    for (auto* d = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(img.base + dd.VirtualAddress); d->Name; ++d) {
        const char* dll = reinterpret_cast<const char*>(img.base + d->Name);
        DWORD name_rva = d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk;
        bind_thunks(img, dll, reinterpret_cast<IMAGE_THUNK_DATA64*>(img.base + name_rva),
                    reinterpret_cast<IMAGE_THUNK_DATA64*>(img.base + d->FirstThunk));
    }
    (void)err;
    return true;
}

// Delay imports are bound up front, so the program's own delay-load helper never
// runs and never raises an exception for a DLL the Xbox doesn't have.
bool bind_delay_imports(Image& img, std::string& err) {
    auto& dd = img.nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
    if (!dd.Size) return true;
    for (auto* d = reinterpret_cast<IMAGE_DELAYLOAD_DESCRIPTOR*>(img.base + dd.VirtualAddress); d->DllNameRVA; ++d) {
        if (!d->Attributes.RvaBased) {
            err = "claude.exe uses an old delay-load table format";
            return false;
        }
        const char* dll = reinterpret_cast<const char*>(img.base + d->DllNameRVA);
        bind_thunks(img, dll, reinterpret_cast<IMAGE_THUNK_DATA64*>(img.base + d->ImportNameTableRVA),
                    reinterpret_cast<IMAGE_THUNK_DATA64*>(img.base + d->ImportAddressTableRVA));
        if (d->ModuleHandleRVA) {
            HMODULE h = system_module(lower(dll));
            *reinterpret_cast<HMODULE*>(img.base + d->ModuleHandleRVA) = h ? h : reinterpret_cast<HMODULE>(img.base);
        }
    }
    return true;
}

void register_exception_table(Image& img) {
    auto& dd = img.nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (!dd.Size) return;
    using AddTable = BOOLEAN(WINAPI*)(PRUNTIME_FUNCTION, DWORD, DWORD64);
    auto add = reinterpret_cast<AddTable>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlAddFunctionTable"));
    if (!add) {
        log("RtlAddFunctionTable is missing; exceptions in claude.exe will crash the app");
        return;
    }
    DWORD count = dd.Size / sizeof(RUNTIME_FUNCTION);
    BOOLEAN ok = add(reinterpret_cast<PRUNTIME_FUNCTION>(img.base + dd.VirtualAddress), count,
                     reinterpret_cast<DWORD64>(img.base));
    log("registered %lu unwind entries: %s", count, ok ? "ok" : "failed");
}

DWORD section_protection(DWORD ch) {
    bool x = ch & IMAGE_SCN_MEM_EXECUTE;
    bool r = ch & IMAGE_SCN_MEM_READ;
    bool w = ch & IMAGE_SCN_MEM_WRITE;
    if (x) return w ? PAGE_EXECUTE_READWRITE : (r ? PAGE_EXECUTE_READ : PAGE_EXECUTE);
    if (w) return PAGE_READWRITE;
    if (r) return PAGE_READONLY;
    return PAGE_NOACCESS;
}

bool protect_sections(Image& img, std::string& err) {
    ULONG old = 0;
    size_t align = img.nt->OptionalHeader.SectionAlignment;
    auto round_up = [align](size_t v) { return (v + align - 1) & ~(align - 1); };
    VirtualProtectFromApp(img.base, round_up(img.nt->OptionalHeader.SizeOfHeaders), PAGE_READONLY, &old);

    auto* sec = IMAGE_FIRST_SECTION(img.nt);
    for (WORD i = 0; i < img.nt->FileHeader.NumberOfSections; ++i) {
        size_t size = round_up(std::max<size_t>(sec[i].Misc.VirtualSize, sec[i].SizeOfRawData));
        if (!size) continue;
        DWORD prot = section_protection(sec[i].Characteristics);
        if (!VirtualProtectFromApp(img.base + sec[i].VirtualAddress, size, prot, &old)) {
            DWORD e = GetLastError();
            char name[9] = {};
            memcpy(name, sec[i].Name, 8);
            log("could not set protection 0x%lx on section %s: error %lu", prot, name, e);
            if (sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) {
                err = fmt("The Xbox refused to make claude.exe's code executable (error %lu). "
                          "The app may be missing the codeGeneration capability.", e);
                return false;
            }
        }
    }
    FlushInstructionCache(GetCurrentProcess(), img.base, img.size);
    return true;
}

}  // namespace

bool in_guest(const void* p) {
    auto* b = static_cast<const uint8_t*>(p);
    return g_image.base && b >= g_image.base && b < g_image.base + g_image.size;
}

void* find_export(const Image& img, const char* name, uint16_t ordinal) {
    auto& dd = img.nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dd.Size) return nullptr;
    auto* ed = reinterpret_cast<IMAGE_EXPORT_DIRECTORY*>(img.base + dd.VirtualAddress);
    auto* funcs = reinterpret_cast<DWORD*>(img.base + ed->AddressOfFunctions);
    auto* names = reinterpret_cast<DWORD*>(img.base + ed->AddressOfNames);
    auto* ords = reinterpret_cast<WORD*>(img.base + ed->AddressOfNameOrdinals);

    DWORD index;
    if (!name) {
        if (ordinal < ed->Base || ordinal - ed->Base >= ed->NumberOfFunctions) return nullptr;
        index = ordinal - ed->Base;
    } else {
        // Export names are sorted, so search them by halves.
        DWORD lo = 0, hi = ed->NumberOfNames;
        bool found = false;
        index = 0;
        while (lo < hi) {
            DWORD mid = (lo + hi) / 2;
            int c = strcmp(name, reinterpret_cast<const char*>(img.base + names[mid]));
            if (c == 0) {
                index = ords[mid];
                found = true;
                break;
            }
            if (c < 0) hi = mid; else lo = mid + 1;
        }
        if (!found) return nullptr;
    }
    DWORD rva = funcs[index];
    if (!rva) return nullptr;
    if (rva >= dd.VirtualAddress && rva < dd.VirtualAddress + dd.Size) return nullptr;  // forwarded
    return img.base + rva;
}

void* resolve_import(const char* dll, const char* name, uint16_t ordinal) {
    if (name) {
        if (void* o = lookup_override(name)) {
            ++g_stats.overridden;
            return o;
        }
    }
    std::string d = lower(dll);
    // wsock32 is the old Winsock 1 DLL. Its functions live in ws2_32 under the same ordinals.
    if (d == "wsock32.dll") d = "ws2_32.dll";
    if (HMODULE h = system_module(d)) {
        LPCSTR key = name ? name : reinterpret_cast<LPCSTR>(static_cast<uintptr_t>(ordinal));
        if (FARPROC p = GetProcAddress(h, key)) {
            ++g_stats.real;
            return reinterpret_cast<void*>(p);
        }
    }
    ++g_stats.stubbed;
    if (name) log("stub: %s!%s", dll, name);
    else log("stub: %s!#%u", dll, ordinal);
    return make_stub(dll, name, ordinal);
}

bool load_image(const wchar_t* path, Image& img, std::string& err) {
    HANDLE f = CreateFile2(path, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        err = fmt("Could not open claude.exe (error %lu)", GetLastError());
        return false;
    }
    struct Closer {
        HANDLE h;
        ~Closer() { CloseHandle(h); }
    } closer{f};

    IMAGE_DOS_HEADER dos;
    if (!read_at(f, 0, &dos, sizeof dos) || dos.e_magic != IMAGE_DOS_SIGNATURE) {
        err = "claude.exe is not a Windows program";
        return false;
    }
    IMAGE_NT_HEADERS64 nt;
    if (!read_at(f, static_cast<uint64_t>(dos.e_lfanew), &nt, sizeof nt) || nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        err = "claude.exe is not a 64-bit x64 Windows program";
        return false;
    }

    size_t image_size = nt.OptionalHeader.SizeOfImage;
    auto* preferred = reinterpret_cast<void*>(nt.OptionalHeader.ImageBase);
    auto* base = static_cast<uint8_t*>(VirtualAllocFromApp(preferred, image_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!base) base = static_cast<uint8_t*>(VirtualAllocFromApp(nullptr, image_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!base) {
        err = fmt("Could not reserve memory for claude.exe (error %lu)", GetLastError());
        return false;
    }
    log("mapping %zu bytes at %p (preferred %p)", image_size, base, preferred);

    size_t header_size = std::min<size_t>(nt.OptionalHeader.SizeOfHeaders, image_size);
    if (!read_at(f, 0, base, header_size)) {
        err = "Could not read claude.exe's headers";
        return false;
    }
    img.base = base;
    img.size = image_size;
    img.path = path;
    img.nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos.e_lfanew);

    auto* sec = IMAGE_FIRST_SECTION(img.nt);
    for (WORD i = 0; i < img.nt->FileHeader.NumberOfSections; ++i) {
        if (!sec[i].SizeOfRawData || !sec[i].PointerToRawData) continue;
        size_t va = sec[i].VirtualAddress;
        if (va >= image_size) continue;
        size_t n = sec[i].SizeOfRawData;
        if (sec[i].Misc.VirtualSize && sec[i].Misc.VirtualSize < n) n = sec[i].Misc.VirtualSize;
        n = std::min(n, image_size - va);
        if (!read_at(f, sec[i].PointerToRawData, base + va, n)) {
            err = "Could not read claude.exe's sections";
            return false;
        }
    }

    ptrdiff_t delta = base - static_cast<uint8_t*>(preferred);
    if (delta && !apply_relocations(img, delta, err)) return false;
    img.nt->OptionalHeader.ImageBase = reinterpret_cast<ULONGLONG>(base);

    if (!bind_imports(img, err)) return false;
    if (!bind_delay_imports(img, err)) return false;
    log("imports: %d real, %d replaced, %d stubbed", g_stats.real, g_stats.overridden, g_stats.stubbed);
    if (!stubs_finalize()) {
        err = "Could not make the generated stubs executable";
        return false;
    }

    register_exception_table(img);
    if (!tls_init(img, err)) return false;
    if (!protect_sections(img, err)) return false;

    img.entry_rva = img.nt->OptionalHeader.AddressOfEntryPoint;
    img.stack_reserve = static_cast<size_t>(img.nt->OptionalHeader.SizeOfStackReserve);
    return true;
}

}  // namespace xb
