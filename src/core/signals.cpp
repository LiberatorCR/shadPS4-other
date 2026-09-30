// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/diagnostic_env.h"
#include "common/arch.h"
#include "common/assert.h"
#include "common/decoder.h"
#include "common/signal_context.h"
#include "core/cpu_patches.h" // Windows static guest red-zone protection
#include "core/libraries/kernel/kernel.h"
#include "core/libraries/kernel/threads/exception.h"
#include "core/signals.h"
#include "emulator.h"

#include <cstring>
#include <cstdlib>
#include <atomic>
#include <array>
#include <vector>

#ifdef _WIN32
#include <windows.h>
static constexpr DWORD MS_VC_EXCEPTION = 0x406D1388;
#else
#include <csignal>
#include <pthread.h>
#ifdef ARCH_X86_64
#include <Zydis/Formatter.h>
#endif
#endif

namespace Core {

#if defined(_WIN32)

static u64 got_dependency_trace_base{};
static std::atomic<u64> got_job_record_writes{};
static std::atomic<u64> got_job_record_sequence{};
static std::atomic<u64> got_job_record_table{};
static std::atomic<u64> got_job_record_phase_started{};
static std::atomic<u64> got_job_record_phase_finished{};
static std::atomic<u64> got_job_record_phase_owner{};
static std::atomic<u64> got_last_consumer_completion{};
static std::atomic<u64> got_proxy_target_written{};
static std::atomic<u32> got_proxy_target_value{};
static thread_local u64 got_job_consumer_started{};
static thread_local u64 got_job_consumer_phase{};
struct GoTDependencyHistory {
    std::atomic<u64> sequence{};
    std::atomic<u64> caller{};
    std::atomic<s32> count_before{};
};
static std::array<GoTDependencyHistory, 2> got_companion_dependencies;
struct GoTJobRecordHistory {
    std::atomic<u64> value{};
    std::atomic<u64> visited{};
    std::atomic<u64> produced{};
    std::atomic<u64> cleared{};
    std::atomic<u64> clear_site{};
};
static std::array<GoTJobRecordHistory, 4096> got_job_records;

void InstallGoTDependencyTrace(u64 eboot_base) {
    struct TraceSite {
        u64 offset;
        std::vector<u8> expected;
    };
    std::vector<TraceSite> sites = {
        {0xCECA8E, {0xF0, 0xFF, 0x05, 0x3F, 0x14, 0xF1, 0x02}},
        {0xCECDE2, {0xE8, 0x79, 0x03, 0xB0, 0xFF}},
        {0x7ED160, {0x55}},
    };
    if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_JOB_RECORDS">()) {
        sites.push_back({0x5822AC, {0x44, 0x89, 0x8B, 0xB8, 0x1B, 0, 0}});
        sites.push_back({0xB8ACF3, {0x49, 0x8B, 0x47, 0x08}});
        sites.push_back({0xB8CB87, {0x48, 0x8B, 0x9D, 0x48, 0xFD, 0xFF, 0xFF}});
        sites.push_back({0xB8D170, {0x55}});
        sites.push_back({0x7ED2D7, {0x8B, 0x0B}});
        sites.push_back({0xB8CAF1, {0x48, 0x89, 0x44, 0x17, 0x28}});
        sites.push_back({0xB8B25F, {0x4C, 0x8B, 0xA6, 0x08, 0xF2, 0, 0}});
        sites.push_back({0xB8D458, {0x4D, 0x8B, 0x4C, 0x13, 0x28}});
        sites.push_back({0xB8AA66, {0x48, 0xC7, 0x44, 0x1A, 0x28, 0, 0, 0, 0}});
        sites.push_back({0xB8AB00, {0x48, 0xC7, 0x44, 0x01, 0x28, 0, 0, 0, 0}});
        sites.push_back({0xB8ABE6, {0x48, 0xC7, 0x44, 0x07, 0x28, 0, 0, 0, 0}});
    }
    for (const auto& site : sites) {
        auto* address = reinterpret_cast<u8*>(eboot_base + site.offset);
        if (std::memcmp(address, site.expected.data(), site.expected.size()) != 0) {
            LOG_WARNING(Debug, "GoT dependency trace: instruction mismatch at {:#x}",
                        site.offset);
            return;
        }
    }
    for (const auto& site : sites) {
        auto* address = reinterpret_cast<u8*>(eboot_base + site.offset);
        DWORD old_protection{};
        if (!VirtualProtect(address, 1, PAGE_EXECUTE_READWRITE, &old_protection)) {
            LOG_WARNING(Debug, "GoT dependency trace: protection change failed at {:#x}",
                        site.offset);
            return;
        }
        *address = 0xCC;
        FlushInstructionCache(GetCurrentProcess(), address, 1);
        DWORD unused{};
        VirtualProtect(address, 1, old_protection, &unused);
    }
    got_dependency_trace_base = eboot_base;
    if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SKIP_CPU_OCCLUSION">()) {
        // CUSA11456 diagnostic workaround. The CPU depth-occlusion map is all zero
        // in the failing menu, causing the game to reject the sword after one frame.
        // Use the game's existing no-map path, preserving earlier frustum/distance
        // checks. This restores the sword but does not fix the corrupted shading
        // or the still-unidentified producer of the empty depth map.
        constexpr u64 offset = 0x1028dae;
        constexpr u8 expected[]{0x48, 0x8b, 0x5e, 0x10, // mov rbx, [rsi+0x10]
                                0x48, 0x85, 0xdb,       // test rbx, rbx
                                0x0f, 0x84, 0x33, 0x01, 0x00, 0x00};
        constexpr u8 replacement[]{0x31, 0xdb, 0x90, 0x90}; // xor ebx, ebx; nop; nop
        auto* address = reinterpret_cast<u8*>(eboot_base + offset);
        if (std::memcmp(address, expected, sizeof(expected)) != 0) {
            LOG_WARNING(Debug, "GoT CPU occlusion workaround: instruction mismatch");
        } else {
            DWORD old_protection{};
            if (VirtualProtect(address, sizeof(replacement), PAGE_EXECUTE_READWRITE,
                               &old_protection)) {
                std::memcpy(address, replacement, sizeof(replacement));
                FlushInstructionCache(GetCurrentProcess(), address, sizeof(replacement));
                DWORD unused{};
                VirtualProtect(address, sizeof(replacement), old_protection, &unused);
                LOG_INFO(Debug, "GoT CPU depth-occlusion bypass enabled (partial workaround)");
            } else {
                LOG_WARNING(Debug, "GoT CPU occlusion workaround: protection change failed");
            }
        }
    }

    LOG_INFO(Debug, "GoT dependency trace armed at base {:#x}", eboot_base);
}

static bool HandleGoTDependencyTrace(EXCEPTION_POINTERS* exception) {
    if (got_dependency_trace_base == 0 || exception->ExceptionRecord->ExceptionCode !=
                                              EXCEPTION_BREAKPOINT) {
        return false;
    }
    const u64 address = reinterpret_cast<u64>(exception->ExceptionRecord->ExceptionAddress);
    const u64 offset = address - got_dependency_trace_base;
    auto* job_context = exception->ContextRecord;
    if (offset == 0x5822AC) {
        *reinterpret_cast<u32*>(job_context->Rbx + 0x1BB8) = u32(job_context->R9);
        if (job_context->Rbx + 0x1BB8 == got_dependency_trace_base + 0x3BF0CC8) {
            got_proxy_target_value = u32(job_context->R9);
            got_proxy_target_written = ++got_job_record_sequence;
            LOG_INFO(Debug, "GoT proxy target written: target={} sequence={} state={} count={}",
                     u32(job_context->R9), got_proxy_target_written.load(),
                     *reinterpret_cast<const u32*>(got_dependency_trace_base + 0x3BFDED0),
                     *reinterpret_cast<const s32*>(got_dependency_trace_base + 0x3BFDED4));
        }
        job_context->Rip += 7;
        return true;
    }
    if (offset == 0x7ED2D7) {
        job_context->Rcx = *reinterpret_cast<const u32*>(job_context->Rbx);
        const auto group = job_context->R14;
        if (*reinterpret_cast<const u32*>(group + 4) > 0 &&
            *reinterpret_cast<const u64*>(group + 0x28) ==
                got_dependency_trace_base + 0xB89E40) {
            const auto argument = *reinterpret_cast<const u64*>(group + 0x30);
            const auto owner = *reinterpret_cast<const u64*>(argument + 8);
            if (owner == got_dependency_trace_base + 0x3BFDED0 - 0xF120) {
                const auto completion = got_last_consumer_completion.load();
                LOG_INFO(Debug,
                         "GoT rebuild queued: group={:#x} argument={:#x} dependency={:#x} "
                         "state={} count={} sequence={} consumer_completion={:#x}",
                         group, argument, job_context->Rbx, job_context->Rcx,
                         *reinterpret_cast<const s32*>(job_context->Rbx + 4),
                         ++got_job_record_sequence, completion);
                MEMORY_BASIC_INFORMATION mapping{};
                if (completion && VirtualQuery(reinterpret_cast<void*>(completion), &mapping,
                                               sizeof(mapping)) &&
                    mapping.State == MEM_COMMIT &&
                    !(mapping.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
                    completion + 8 <= reinterpret_cast<u64>(mapping.BaseAddress) +
                                          mapping.RegionSize) {
                    const auto* dependency = reinterpret_cast<const u32*>(completion);
                    LOG_INFO(Debug, "GoT previous consumer completion state={} count={}",
                             dependency[0], dependency[1]);
                }
            }
        }
        if (job_context->Rbx == got_dependency_trace_base + 0x3BFDED0 &&
            *reinterpret_cast<const u32*>(group + 4) > 0 &&
            *reinterpret_cast<const u64*>(group + 0x28) ==
                got_dependency_trace_base + 0xB8D170) {
            const auto argument = *reinterpret_cast<const u64*>(group + 0x30);
            const auto owner = *reinterpret_cast<const u64*>(argument);
            if (owner == got_dependency_trace_base + 0x3BFDED0 - 0xF120) {
                if (*reinterpret_cast<const s32*>(argument + 0x20) > 0) {
                    got_last_consumer_completion =
                        *reinterpret_cast<const u64*>(argument + 0x18);
                }
                static std::atomic<u32> queued_groups{};
                if (++queued_groups <= 65536) {
                    LOG_INFO(Debug,
                             "GoT consumer queued: group={:#x} argument={:#x} owner={:#x} "
                             "state={} count={} started={} finished={} sequence={} target={}",
                             group, argument, owner, job_context->Rcx,
                             *reinterpret_cast<const s32*>(job_context->Rbx + 4),
                             got_job_record_phase_started.load(),
                             got_job_record_phase_finished.load(), ++got_job_record_sequence,
                             *reinterpret_cast<const u32*>(owner + 0x1F18));
                }
            }
        }
        job_context->Rip += 2;
        return true;
    }
    if (offset == 0xB8D170) {
        got_job_consumer_started = ++got_job_record_sequence;
        got_job_consumer_phase = got_job_record_phase_started.load();
        job_context->Rsp -= sizeof(u64);
        *reinterpret_cast<u64*>(job_context->Rsp) = job_context->Rbp;
        ++job_context->Rip;
        return true;
    }
    if (offset == 0xB8ACF3) {
        job_context->Rax = *reinterpret_cast<const u64*>(job_context->R15 + 8);
        if (job_context->Rax == got_dependency_trace_base + 0x3BFDED0 - 0xF120) {
            got_job_record_phase_owner = job_context->Rax;
            got_job_record_phase_started = ++got_job_record_sequence;
        }
        job_context->Rip += 4;
        return true;
    }
    if (offset == 0xB8CB87) {
        job_context->Rbx = *reinterpret_cast<const u64*>(job_context->Rbp - 0x2B8);
        const auto owner = *reinterpret_cast<const u64*>(job_context->Rbp - 0x270);
        if (owner == got_dependency_trace_base + 0x3BFDED0 - 0xF120) {
            got_job_record_phase_finished = ++got_job_record_sequence;
        }
        job_context->Rip += 7;
        return true;
    }
    if (offset == 0xB8B25F) {
        job_context->R12 = *reinterpret_cast<u64*>(job_context->Rsi + 0xF208);
        const auto index = job_context->Rbx;
        if (job_context->R12 == got_job_record_table.load() && index < got_job_records.size()) {
            got_job_records[index].visited = ++got_job_record_sequence;
        }
        job_context->Rip += 7;
        return true;
    }
    if (offset == 0xB8AA66 || offset == 0xB8AB00 || offset == 0xB8ABE6) {
        const u64 table = offset == 0xB8AA66 ? job_context->Rdx
                          : offset == 0xB8AB00 ? job_context->Rcx : job_context->Rdi;
        const u64 displacement = offset == 0xB8AA66 ? job_context->Rbx : job_context->Rax;
        *reinterpret_cast<u64*>(table + displacement + 0x28) = 0;
        const auto index = displacement / 48;
        if (table == got_job_record_table.load() && index < got_job_records.size()) {
            auto& history = got_job_records[index];
            history.clear_site = offset;
            history.cleared = ++got_job_record_sequence;
        }
        job_context->Rip += 9;
        return true;
    }
    if (offset == 0xB8CAF1) {
        *reinterpret_cast<u64*>(job_context->Rdi + job_context->Rdx + 0x28) = job_context->Rax;
        const auto writes = ++got_job_record_writes;
        u64 expected_table = 0;
        got_job_record_table.compare_exchange_strong(expected_table, job_context->Rdi);
        const auto index = job_context->Rdx / 48;
        if (job_context->Rdi == got_job_record_table.load() && index < got_job_records.size()) {
            auto& history = got_job_records[index];
            history.value = job_context->Rax;
            history.produced = ++got_job_record_sequence;
        }
        if (writes <= 16) {
            LOG_INFO(Debug, "GoT job record producer: table={:#x} index={} record={:#x} writes={}",
                     job_context->Rdi, job_context->Rdx / 48, job_context->Rax, writes);
        }
        job_context->Rip += 5;
        return true;
    }
    if (offset == 0xB8D458) {
        job_context->R9 =
            *reinterpret_cast<u64*>(job_context->R11 + job_context->Rdx + 0x28);
        static std::atomic<u32> missing_records{};
        if (job_context->R9 == 0 && ++missing_records <= 32) {
            const auto* entry = reinterpret_cast<const u32*>(job_context->R11 + job_context->Rdx);
            LOG_INFO(Debug,
                     "GoT job record consumer missing: table={:#x} index={} flags={:#x} "
                     "kind={:#x} producer_writes={} owner={:#x}",
                     job_context->R11, job_context->Rdx / 48, entry[5], entry[4],
                     got_job_record_writes.load(), *reinterpret_cast<const u64*>(job_context->R12));
            const auto index = job_context->Rdx / 48;
            if (job_context->R11 == got_job_record_table.load() && index < got_job_records.size()) {
                const auto& history = got_job_records[index];
                LOG_INFO(Debug,
                         "GoT job record history: index={} last_value={:#x} visited={} produced={} "
                         "cleared={} clear_site={:#x}", index, history.value.load(),
                         history.visited.load(), history.produced.load(), history.cleared.load(),
                         history.clear_site.load());
                const auto owner = *reinterpret_cast<const u64*>(job_context->R12);
                LOG_INFO(Debug,
                         "GoT job record phase: owner={:#x} rebuild_workers={} entries={} "
                         "consumer_parent={:#x}", owner,
                         *reinterpret_cast<const s32*>(owner + 0xF1B0),
                         *reinterpret_cast<const s32*>(owner + 0xF210), job_context->R8);
                LOG_INFO(Debug,
                         "GoT record finalizer: owner={:#x} started={} finished={} now={}",
                         got_job_record_phase_owner.load(), got_job_record_phase_started.load(),
                         got_job_record_phase_finished.load(), got_job_record_sequence.load());
                LOG_INFO(Debug, "GoT consumer job entry: started={} finalizer_at_entry={}",
                         got_job_consumer_started, got_job_consumer_phase);
                LOG_INFO(Debug, "GoT last target write: target={} sequence={}",
                         got_proxy_target_value.load(), got_proxy_target_written.load());
                for (u32 i = 0; i < got_companion_dependencies.size(); ++i) {
                    const auto dep_offset = i == 0 ? 0x3BFDE88u : 0x3BFDF18u;
                    const auto* dep = reinterpret_cast<const u32*>(
                        got_dependency_trace_base + dep_offset);
                    const auto& history = got_companion_dependencies[i];
                    LOG_INFO(Debug,
                             "GoT companion dependency offset={:#x} state={} count={} "
                             "last_release={} caller={:#x} count_before={}",
                             dep_offset, dep[0], dep[1], history.sequence.load(),
                             history.caller.load(), history.count_before.load());
                }
                const auto completion_count =
                    *reinterpret_cast<const s32*>(job_context->R12 + 0x20);
                const auto completion_dependency =
                    *reinterpret_cast<const u64*>(job_context->R12 + 0x18);
                LOG_INFO(Debug,
                         "GoT consumer completion: argument={:#x} dependencies={} first={:#x}",
                         job_context->R12, completion_count, completion_dependency);
                MEMORY_BASIC_INFORMATION completion_mapping{};
                if (completion_count > 0 && completion_dependency != 0 &&
                    VirtualQuery(reinterpret_cast<void*>(completion_dependency),
                                 &completion_mapping, sizeof(completion_mapping)) &&
                    completion_mapping.State == MEM_COMMIT &&
                    !(completion_mapping.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
                    completion_dependency + 8 <=
                        reinterpret_cast<u64>(completion_mapping.BaseAddress) +
                            completion_mapping.RegionSize) {
                    const auto* completion =
                        reinterpret_cast<const u32*>(completion_dependency);
                    LOG_INFO(Debug, "GoT consumer completion state={} count={}",
                             completion[0], completion[1]);
                }
                const auto visibility = *reinterpret_cast<const u64*>(owner + 0xF228);
                if (visibility != 0) {
                    LOG_INFO(Debug, "GoT job record visibility: base={:#x} index={} value={:#x}",
                             visibility, index, reinterpret_cast<const u32*>(visibility)[index]);
                }
            }
        }
        job_context->Rip += 5;
        return true;
    }
    auto* count = reinterpret_cast<volatile LONG*>(got_dependency_trace_base + 0x3BFDED4);
    if (offset == 0x7ED160) {
        auto* context = exception->ContextRecord;
        if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_JOB_RECORDS">() &&
            (context->Rdi == got_dependency_trace_base + 0x3BFDE88 ||
             context->Rdi == got_dependency_trace_base + 0x3BFDF18)) {
            const auto index = context->Rdi == got_dependency_trace_base + 0x3BFDE88 ? 0 : 1;
            auto& history = got_companion_dependencies[index];
            history.caller = *reinterpret_cast<const u64*>(context->Rsp) -
                             got_dependency_trace_base;
            history.count_before = *reinterpret_cast<const s32*>(context->Rdi + 4);
            history.sequence = ++got_job_record_sequence;
        }
        if (context->Rdi == got_dependency_trace_base + 0x3BFDED0) {
            const u64 caller = *reinterpret_cast<u64*>(context->Rsp);
            const u32 state =
                *reinterpret_cast<volatile u32*>(got_dependency_trace_base + 0x3BFDED0);
            LOG_INFO(Debug, "GoT dependency decrement count={} state={} caller={:#x}",
                     *count, state, caller - got_dependency_trace_base);
            if (caller - got_dependency_trace_base == 0xB897F8) {
                const u64 proxy = context->Rbx;
                const auto slot = *reinterpret_cast<volatile s32*>(proxy + 0x1F1C);
                const auto target = *reinterpret_cast<volatile u32*>(proxy + 0x1F18);
                const auto table = *reinterpret_cast<u64*>(got_dependency_trace_base + 0x173C7C0);
                const auto actual = *reinterpret_cast<volatile u32*>(table + static_cast<s64>(slot) * 8);
                const auto eq = *reinterpret_cast<volatile s64*>(proxy + 0xF2E0);
                LOG_INFO(Debug,
                         "GoT ProxySetSync release proxy={:#x} slot={} actual={} target={} eq={}",
                         proxy, slot, actual, target, eq);
            }
        }
        context->Rsp -= sizeof(u64);
        *reinterpret_cast<u64*>(context->Rsp) = context->Rbp;
        context->Rip = got_dependency_trace_base + 0x7ED161;
        return true;
    }
    if (offset == 0xCECA8E) {
        const LONG before = *count;
        const LONG after = InterlockedIncrement(count);
        LOG_INFO(Debug, "GoT dependency increment count={} -> {} state={}", before, after,
                 *reinterpret_cast<volatile u32*>(got_dependency_trace_base + 0x3BFDED0));
        exception->ContextRecord->Rip = got_dependency_trace_base + 0xCECA95;
        return true;
    }
    if (offset == 0xCECDE2) {
        LOG_INFO(Debug, "GoT dependency release count={} state={} df60={}", *count,
                 *reinterpret_cast<volatile u32*>(got_dependency_trace_base + 0x3BFDED0),
                 *reinterpret_cast<volatile u32*>(got_dependency_trace_base + 0x3BFDF60));
        auto* context = exception->ContextRecord;
        context->Rsp -= sizeof(u64);
        *reinterpret_cast<u64*>(context->Rsp) = got_dependency_trace_base + 0xCECDE7;
        context->Rip = got_dependency_trace_base + 0x7ED160;
        return true;
    }
    return false;
}

static LONG WINAPI SignalHandler(EXCEPTION_POINTERS* pExp) noexcept {
    using namespace Libraries::Kernel;
    const auto* signals = Signals::Instance();

    const bool use_static_windows_guest_red_zone_protection =
        WindowsGuestRedZoneProtection::IsStaticPatchingEnabled();
    DWORD code = 0;
    PVOID address = nullptr;

    if (pExp != nullptr && pExp->ExceptionRecord != nullptr) {
        code = pExp->ExceptionRecord->ExceptionCode;
        address = pExp->ExceptionRecord->ExceptionAddress;
    }

    Ucontext guest_context{pExp->ContextRecord};
    Siginfo guest_info{
        ._si_signo = 0,
        ._si_errno = 0,
        ._si_code = POSIX_SI_NOINFO,
        ._si_addr = (void*)guest_context.uc_mcontext.mc_rip,
    };

    bool handled = false;
    bool static_protection_exception = false; // Windows static guest red-zone protection
    if (code == EXCEPTION_BREAKPOINT && HandleGoTDependencyTrace(pExp)) {
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
        guest_info._si_signo = POSIX_SIGSEGV;
        guest_info._si_code = POSIX_SEGV_MAPERR;
        static_protection_exception = true; // Windows static guest red-zone protection
        handled = signals->DispatchAccessViolation(
            pExp, reinterpret_cast<void*>(pExp->ExceptionRecord->ExceptionInformation[1]));
        break;
    case EXCEPTION_ILLEGAL_INSTRUCTION:
        guest_info._si_signo = POSIX_SIGILL;
        guest_info._si_code = POSIX_ILL_ILLOPC;
        static_protection_exception = true; // Windows static guest red-zone protection
        handled = signals->DispatchIllegalInstruction(pExp);
        break;
    case EXCEPTION_PRIV_INSTRUCTION: // Windows static guest red-zone protection
        if (use_static_windows_guest_red_zone_protection) {
            static_protection_exception = true;
            handled = signals->DispatchIllegalInstruction(pExp);
        }
        break;
    case EXCEPTION_IN_PAGE_ERROR:
        guest_info._si_signo = POSIX_SIGBUS;
        guest_info._si_code = POSIX_BUS_ADRALN;
        break;
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
        guest_info._si_signo = POSIX_SIGFPE;
        guest_info._si_code = POSIX_FPE_INTDIV;
        break;
    case EXCEPTION_INT_OVERFLOW:
        guest_info._si_signo = POSIX_SIGFPE;
        guest_info._si_code = POSIX_FPE_INTOVF;
        break;
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:
        guest_info._si_signo = POSIX_SIGFPE;
        guest_info._si_code = POSIX_FPE_FLTDIV;
        break;
    case EXCEPTION_FLT_INVALID_OPERATION:
        guest_info._si_signo = POSIX_SIGFPE;
        guest_info._si_code = POSIX_FPE_FLTINV;
        break;
    case EXCEPTION_FLT_OVERFLOW:
        guest_info._si_signo = POSIX_SIGFPE;
        guest_info._si_code = POSIX_FPE_FLTOVF;
        break;
    case EXCEPTION_FLT_UNDERFLOW:
        guest_info._si_signo = POSIX_SIGFPE;
        guest_info._si_code = POSIX_FPE_FLTUND;
        break;
    case EXCEPTION_FLT_DENORMAL_OPERAND:
        guest_info._si_signo = POSIX_SIGFPE;
        guest_info._si_code = POSIX_FPE_FLTSUB; // i am not sure about this one
        break;
    case EXCEPTION_FLT_INEXACT_RESULT:
        guest_info._si_signo = POSIX_SIGFPE;
        guest_info._si_code = POSIX_FPE_FLTRES;
        break;
    case EXCEPTION_FLT_STACK_CHECK:
        guest_info._si_signo = POSIX_SIGILL;
        guest_info._si_code = POSIX_ILL_BADSTK; // i am not sure about this one either
        break;
    case EXCEPTION_BREAKPOINT:
    case EXCEPTION_SINGLE_STEP:
        guest_info._si_signo = POSIX_SIGTRAP;
        guest_info._si_code = POSIX_TRAP_BRKPT;
        break;
    case DBG_PRINTEXCEPTION_C:
    case DBG_PRINTEXCEPTION_WIDE_C:
        // Used by OutputDebugString functions.
        return EXCEPTION_CONTINUE_EXECUTION;
    case MS_VC_EXCEPTION:
        LOG_DEBUG(Debug, "Pass MS_VC_EXCEPTION at {} to handler", address);
        return EXCEPTION_EXECUTE_HANDLER;
    default:
        break;
    }

    if (handled) {
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    if (guest_info._si_signo != 0) {
        if (g_curthread &&
            g_curthread->DispatchSignal(guest_info._si_signo, &guest_info, &guest_context)) {
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }

    const bool report_unhandled =
        use_static_windows_guest_red_zone_protection ? static_protection_exception : true;
    if (report_unhandled) {
        LOG_CRITICAL(Debug, "Unhandled Exception code {:#x} at {}", code, address);
        const auto& context = *pExp->ContextRecord;
        const auto& record = *pExp->ExceptionRecord;
        LOG_CRITICAL(Debug,
                     "Exception context: access={} fault={:#x} guest_offset={:#x} "
                     "rax={:#x} rbx={:#x} rcx={:#x} rdx={:#x} rsi={:#x} rdi={:#x} "
                     "rsp={:#x} rbp={:#x} r8={:#x} r9={:#x} r10={:#x} r11={:#x} "
                     "r12={:#x} r13={:#x} r14={:#x} r15={:#x}",
                     record.NumberParameters > 0 ? record.ExceptionInformation[0] : 0,
                     record.NumberParameters > 1 ? record.ExceptionInformation[1] : 0,
                     got_dependency_trace_base ? context.Rip - got_dependency_trace_base : 0,
                     context.Rax, context.Rbx, context.Rcx, context.Rdx, context.Rsi,
                     context.Rdi, context.Rsp, context.Rbp, context.R8, context.R9,
                     context.R10, context.R11, context.R12, context.R13, context.R14,
                     context.R15);
        Common::Singleton<Core::Emulator>::Instance()->Shutdown();
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

#else

static std::string DisassembleInstruction(void* code_address) {
    char buffer[256] = "<unable to decode>";

#ifdef ARCH_X86_64
    ZydisDecodedInstruction instruction;
    ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
    const auto status =
        Common::Decoder::Instance()->decodeInstruction(instruction, operands, code_address);
    if (ZYAN_SUCCESS(status)) {
        ZydisFormatter formatter;
        ZydisFormatterInit(&formatter, ZYDIS_FORMATTER_STYLE_INTEL);
        ZydisFormatterFormatInstruction(&formatter, &instruction, operands,
                                        instruction.operand_count_visible, buffer, sizeof(buffer),
                                        reinterpret_cast<u64>(code_address), ZYAN_NULL);
    }
#endif

    return buffer;
}

static s32 NativeSiCodeToGuest(s32 sig, s32 code) {
    using namespace Libraries::Kernel;
    switch (sig) {
    case SIGUSR1:
        return POSIX_SI_LWP;
    case SIGSEGV:
        switch (code) {
        case SEGV_MAPERR:
            return POSIX_SEGV_MAPERR;
        case SEGV_ACCERR:
            return POSIX_SEGV_ACCERR;
        }
    case SIGBUS:
        switch (code) {
        case BUS_ADRALN:
            return POSIX_BUS_ADRALN;
        case BUS_ADRERR:
            return POSIX_BUS_ADRERR;
        case BUS_OBJERR:
            return POSIX_BUS_OBJERR;
        }
    case SIGILL:
        switch (code) {
        case ILL_ILLOPC:
            return POSIX_ILL_ILLOPC;
        case ILL_ILLOPN:
            return POSIX_ILL_ILLOPN;
        case ILL_ILLADR:
            return POSIX_ILL_ILLADR;
        case ILL_ILLTRP:
            return POSIX_ILL_ILLTRP;
        case ILL_PRVOPC:
            return POSIX_ILL_PRVOPC;
        case ILL_PRVREG:
            return POSIX_ILL_PRVREG;
        case ILL_COPROC:
            return POSIX_ILL_COPROC;
        case ILL_BADSTK:
            return POSIX_ILL_BADSTK;
        }
    case SIGFPE:
        switch (code) {
        case FPE_INTOVF:
            return POSIX_FPE_INTOVF;
        case FPE_INTDIV:
            return POSIX_FPE_INTDIV;
        case FPE_FLTDIV:
            return POSIX_FPE_FLTDIV;
        case FPE_FLTOVF:
            return POSIX_FPE_FLTOVF;
        case FPE_FLTUND:
            return POSIX_FPE_FLTUND;
        case FPE_FLTRES:
            return POSIX_FPE_FLTRES;
        case FPE_FLTINV:
            return POSIX_FPE_FLTINV;
        case FPE_FLTSUB:
            return POSIX_FPE_FLTSUB;
        }
    case SIGTRAP:
        switch (code) {
        case TRAP_BRKPT:
            return POSIX_TRAP_BRKPT;
        case TRAP_TRACE:
            return POSIX_TRAP_TRACE;
#ifdef __FreeBSD__
        case TRAP_DTRACE:
            return POSIX_TRAP_DTRACE;
#endif
        }

    default:
        return POSIX_SI_NOINFO;
    }
}

void SignalHandler(int sig, siginfo_t* info, void* raw_context) {
    using namespace Libraries::Kernel;
    auto* thread = g_curthread;
    const auto* signals = Signals::Instance();

    auto* code_address = Common::GetRip(raw_context);

    Ucontext context{info, reinterpret_cast<ucontext_t*>(raw_context)};
    Siginfo guest_info{};
    if (info) {
        guest_info = *reinterpret_cast<Siginfo*>(info);
        guest_info._si_signo = sig == SIGUSR1 ? 0 : NativeToOrbisSignal(info->si_signo);
        guest_info._si_errno = NativeToPosixErrno(info->si_errno);
        guest_info._si_code = NativeSiCodeToGuest(sig, info->si_code);
        guest_info._si_addr = (void*)context.uc_mcontext.mc_rip;
    }
    Siginfo* info_p = info ? &guest_info : nullptr;
    Ucontext* context_p = raw_context ? &context : nullptr;

    switch (sig) {
    case SIGSEGV:
    case SIGBUS: {
        const bool is_write = Common::IsWriteError(raw_context);
        if (!signals->DispatchAccessViolation(raw_context, info->si_addr)) {
            if (thread && thread->DispatchSignal(NativeToOrbisSignal(sig), info_p, context_p)) {
                return;
            }
            UNREACHABLE_MSG("Unhandled access violation at code address {}: {} address {}",
                            fmt::ptr(code_address), is_write ? "Write to" : "Read from",
                            fmt::ptr(info->si_addr));
        }
        break;
    }
    case SIGILL:
        if (signals->DispatchIllegalInstruction(raw_context)) {
            return;
        }
    case SIGFPE:
    case SIGTRAP:
    case SIGSYS: {
        if (thread && thread->DispatchSignal(NativeToOrbisSignal(sig), info_p, context_p)) {
            return;
        }

        UNREACHABLE_MSG("Unhandled signal {} at code address {}", sig, fmt::ptr(code_address));
    }
    case SIGSLEEP: {
        // Sleep thread until signal is received again
        sigset_t sigset;
        sigemptyset(&sigset);
        sigaddset(&sigset, SIGSLEEP);
        sigwait(&sigset, &sig);
        break;
    }
    case SIGUSR1:
        if (thread) {
            thread->DispatchPendingSignals(info_p, context_p);
        }
        break;
    default:
        UNREACHABLE_MSG("Unhandled signal {} at code address {}", sig, fmt::ptr(code_address));
    }
}

#endif

SignalDispatch::SignalDispatch() {
#if defined(_WIN32)
    ASSERT_MSG(handle = AddVectoredExceptionHandler(0, SignalHandler),
               "Failed to register exception handler.");
#else
    struct sigaction action{};
    action.sa_sigaction = SignalHandler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);

    ASSERT_MSG(
        sigaction(SIGSEGV, &action, nullptr) == 0 && sigaction(SIGBUS, &action, nullptr) == 0 &&
            sigaction(SIGILL, &action, nullptr) == 0 && sigaction(SIGFPE, &action, nullptr) == 0 &&
            sigaction(SIGTRAP, &action, nullptr) == 0 && sigaction(SIGSYS, &action, nullptr) == 0 &&
            sigaction(SIGUSR1, &action, nullptr) == 0 && sigaction(SIGSLEEP, &action, nullptr) == 0,
        "Failed to register signal handlers.");
#endif
}

void SignalDispatch::RemoveHandlers() {
    // asserting here would get into an infinite loop until too
    // many nested exceptions makes the OS kill the process
#if defined(_WIN32)
    if (!(RemoveVectoredExceptionHandler(handle))) {
        LOG_CRITICAL(Core, "Failed to remove exception handler.");
        std::quick_exit(1);
    }
#else
    struct sigaction action{};
    action.sa_handler = SIG_DFL;
    action.sa_flags = 0;
    sigemptyset(&action.sa_mask);

    if (!(sigaction(SIGSEGV, &action, nullptr) == 0 && sigaction(SIGBUS, &action, nullptr) == 0 &&
          sigaction(SIGILL, &action, nullptr) == 0 && sigaction(SIGFPE, &action, nullptr) == 0 &&
          sigaction(SIGTRAP, &action, nullptr) == 0 && sigaction(SIGSYS, &action, nullptr) == 0 &&
          sigaction(SIGUSR1, &action, nullptr) == 0 &&
          sigaction(SIGSLEEP, &action, nullptr) == 0)) {
        LOG_CRITICAL(Core, "Failed to remove signal handlers.");
        std::quick_exit(1);
    }
#endif
}

SignalDispatch::~SignalDispatch() {
    RemoveHandlers();
}

bool SignalDispatch::DispatchAccessViolation(void* context, void* fault_address) const {
    for (const auto& [handler, _] : access_violation_handlers) {
        if (handler(context, fault_address)) {
            return true;
        }
    }
    return false;
}

bool SignalDispatch::DispatchIllegalInstruction(void* context) const {
    for (const auto& [handler, _] : illegal_instruction_handlers) {
        if (handler(context)) {
            return true;
        }
    }
    return false;
}

} // namespace Core
