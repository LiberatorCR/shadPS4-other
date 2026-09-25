// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/assert.h"
#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <new>
#include "common/logging/log.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/libs.h"
#include "libc_internal_memory.h"

namespace Libraries::LibcInternal {

void* PS4_SYSV_ABI internal_memset(void* s, int c, size_t n) {
    return std::memset(s, c, n);
}

void* PS4_SYSV_ABI internal_memcpy(void* dest, const void* src, size_t n) {
    return std::memcpy(dest, src, n);
}

s32 PS4_SYSV_ABI internal_memcpy_s(void* dest, size_t destsz, const void* src, size_t count) {
#ifdef _WIN64
    return memcpy_s(dest, destsz, src, count);
#else
    std::memcpy(dest, src, count);
    return 0; // ALL OK
#endif
}

s32 PS4_SYSV_ABI internal_memcmp(const void* s1, const void* s2, size_t n) {
    return std::memcmp(s1, s2, n);
}

static u64 g_mspace_atomic_id_mask = 0;
static u64 g_mstate_table[64] = {0};

struct HeapInfoInfo {
    u64 size = sizeof(HeapInfoInfo);
    u32 flag;
    u32 getSegmentInfo;
    u64* mspace_atomic_id_mask;
    u64* mstate_table;
};

void PS4_SYSV_ABI sceLibcHeapGetTraceInfo(HeapInfoInfo* info) {
    info->mspace_atomic_id_mask = &g_mspace_atomic_id_mask;
    info->mstate_table = g_mstate_table;
    info->getSegmentInfo = 0;
}

// A guest mspace owns a caller-supplied memory region. Keep allocator bookkeeping on the host,
// but return allocations from that region so guest code sees the expected addresses.
struct Mspace {
    u8* base;
    size_t size;
    std::mutex mutex;
    std::map<size_t, size_t> free_ranges;
    std::map<size_t, size_t> allocations;

    Mspace(void* memory, size_t bytes) : base{static_cast<u8*>(memory)}, size{bytes} {
        free_ranges.emplace(0, bytes);
    }

    void* Allocate(size_t bytes, size_t alignment) {
        if (bytes == 0 || alignment == 0 || !std::has_single_bit(alignment)) {
            return nullptr;
        }
        alignment = std::max(alignment, size_t{16});
        std::lock_guard lock{mutex};
        for (auto it = free_ranges.begin(); it != free_ranges.end(); ++it) {
            const size_t start = it->first;
            const size_t end = start + it->second;
            const uintptr_t begin = reinterpret_cast<uintptr_t>(base) + start;
            if (begin > std::numeric_limits<uintptr_t>::max() - (alignment - 1)) {
                continue;
            }
            const uintptr_t aligned = (begin + alignment - 1) & ~(alignment - 1);
            const size_t offset = aligned - reinterpret_cast<uintptr_t>(base);
            if (offset > end || bytes > end - offset) {
                continue;
            }
            free_ranges.erase(it);
            if (offset > start) {
                free_ranges.emplace(start, offset - start);
            }
            if (offset + bytes < end) {
                free_ranges.emplace(offset + bytes, end - offset - bytes);
            }
            allocations.emplace(offset, bytes);
            return base + offset;
        }
        return nullptr;
    }

    size_t AllocationSize(const void* pointer) {
        if (!pointer) {
            return 0;
        }
        const uintptr_t address = reinterpret_cast<uintptr_t>(pointer);
        const uintptr_t begin = reinterpret_cast<uintptr_t>(base);
        if (address < begin || address - begin >= size) {
            return 0;
        }
        std::lock_guard lock{mutex};
        const auto it = allocations.find(address - begin);
        return it == allocations.end() ? 0 : it->second;
    }

    void Free(void* pointer) {
        if (!pointer) {
            return;
        }
        const uintptr_t address = reinterpret_cast<uintptr_t>(pointer);
        const uintptr_t begin = reinterpret_cast<uintptr_t>(base);
        if (address < begin || address - begin >= size) {
            LOG_ERROR(Lib_LibcInternal, "mspace free outside region: {}", pointer);
            return;
        }
        std::lock_guard lock{mutex};
        const size_t offset = address - begin;
        auto allocation = allocations.find(offset);
        if (allocation == allocations.end()) {
            LOG_ERROR(Lib_LibcInternal, "mspace free of unknown allocation: {}", pointer);
            return;
        }
        size_t start = offset;
        size_t end = offset + allocation->second;
        allocations.erase(allocation);
        auto next = free_ranges.lower_bound(start);
        if (next != free_ranges.begin()) {
            auto prev = std::prev(next);
            if (prev->first + prev->second == start) {
                start = prev->first;
                free_ranges.erase(prev);
            }
        }
        if (next != free_ranges.end() && next->first == end) {
            end += next->second;
            free_ranges.erase(next);
        }
        free_ranges.emplace(start, end - start);
    }
};

void* PS4_SYSV_ABI sceLibcMspaceCreate(const char* name, void* memory, size_t size, u32 flags) {
    if (!memory || size < 4096) {
        LOG_ERROR(Lib_LibcInternal, "sceLibcMspaceCreate invalid region: name={} memory={} size={:#x}",
                  name ? name : "(null)", memory, size);
        return nullptr;
    }
    LOG_DEBUG(Lib_LibcInternal, "sceLibcMspaceCreate name={} memory={} size={:#x} flags={:#x}",
              name ? name : "(null)", memory, size, flags);
    return new (std::nothrow) Mspace(memory, size);
}

void PS4_SYSV_ABI sceLibcMspaceDestroy(void* handle) {
    delete static_cast<Mspace*>(handle);
}

void* PS4_SYSV_ABI sceLibcMspaceMalloc(void* handle, size_t size) {
    return handle ? static_cast<Mspace*>(handle)->Allocate(size, 16) : nullptr;
}

void* PS4_SYSV_ABI sceLibcMspaceMemalign(void* handle, size_t alignment, size_t size) {
    return handle ? static_cast<Mspace*>(handle)->Allocate(size, alignment) : nullptr;
}

void PS4_SYSV_ABI sceLibcMspaceFree(void* handle, void* pointer) {
    if (handle) {
        static_cast<Mspace*>(handle)->Free(pointer);
    }
}

size_t PS4_SYSV_ABI sceLibcMspaceMallocUsableSize(void* handle, void* pointer) {
    return handle ? static_cast<Mspace*>(handle)->AllocationSize(pointer) : 0;
}

void* PS4_SYSV_ABI sceLibcMspaceCalloc(void* handle, size_t count, size_t size) {
    if (size && count > std::numeric_limits<size_t>::max() / size) {
        return nullptr;
    }
    const size_t bytes = count * size;
    void* pointer = sceLibcMspaceMalloc(handle, bytes);
    if (pointer) {
        std::memset(pointer, 0, bytes);
    }
    return pointer;
}

void* PS4_SYSV_ABI sceLibcMspaceRealloc(void* handle, void* pointer, size_t size) {
    if (!handle) {
        return nullptr;
    }
    if (!pointer) {
        return sceLibcMspaceMalloc(handle, size);
    }
    if (size == 0) {
        sceLibcMspaceFree(handle, pointer);
        return nullptr;
    }
    auto* space = static_cast<Mspace*>(handle);
    const size_t old_size = space->AllocationSize(pointer);
    if (old_size == 0) {
        return nullptr;
    }
    void* replacement = space->Allocate(size, 16);
    if (replacement) {
        std::memcpy(replacement, pointer, std::min(old_size, size));
        space->Free(pointer);
    }
    return replacement;
}

void RegisterlibSceLibcInternalMemory(Core::Loader::SymbolsResolver* sym) {

    LIB_FUNCTION("NFLs+dRJGNg", "libSceLibcInternal", 1, "libSceLibcInternal", internal_memcpy_s);
    LIB_FUNCTION("Q3VBxCXhUHs", "libSceLibcInternal", 1, "libSceLibcInternal", internal_memcpy);
    LIB_FUNCTION("8zTFvBIAIN8", "libSceLibcInternal", 1, "libSceLibcInternal", internal_memset);
    LIB_FUNCTION("DfivPArhucg", "libSceLibcInternal", 1, "libSceLibcInternal", internal_memcmp);

    LIB_FUNCTION("NWtTN10cJzE", "libSceLibcInternalExt", 1, "libSceLibcInternal",
                 sceLibcHeapGetTraceInfo);

    LIB_FUNCTION("-hn1tcVHq5Q", "libSceLibcInternal", 1, "libSceLibcInternal",
                 sceLibcMspaceCreate);
    LIB_FUNCTION("W6SiVSiCDtI", "libSceLibcInternal", 1, "libSceLibcInternal",
                 sceLibcMspaceDestroy);
    LIB_FUNCTION("OJjm-QOIHlI", "libSceLibcInternal", 1, "libSceLibcInternal",
                 sceLibcMspaceMalloc);
    LIB_FUNCTION("iF1iQHzxBJU", "libSceLibcInternal", 1, "libSceLibcInternal",
                 sceLibcMspaceMemalign);
    LIB_FUNCTION("Vla-Z+eXlxo", "libSceLibcInternal", 1, "libSceLibcInternal",
                 sceLibcMspaceFree);
    LIB_FUNCTION("fEoW6BJsPt4", "libSceLibcInternal", 1, "libSceLibcInternal",
                 sceLibcMspaceMallocUsableSize);
    LIB_FUNCTION("LYo3GhIlB38", "libSceLibcInternal", 1, "libSceLibcInternal",
                 sceLibcMspaceCalloc);
    LIB_FUNCTION("gigoVHZvVPE", "libSceLibcInternal", 1, "libSceLibcInternal",
                 sceLibcMspaceRealloc);
}

} // namespace Libraries::LibcInternal
