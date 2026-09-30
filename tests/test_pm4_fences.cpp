// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstring>
#include <gtest/gtest.h>
#include "video_core/amdgpu/pm4_cmds.h"

using namespace AmdGpu;

TEST(Pm4Fence, ReleaseMemInterruptModes) {
    for (u32 mode = 0; mode != 4; ++mode) {
        alignas(8) u64 label = 0;
        PM4CmdReleaseMem packet{.header = {PM4ItOpcode::ReleaseMem, 6}};
        const auto address = reinterpret_cast<uintptr_t>(&label);
        packet.address_lo = u32(address);
        packet.address_hi = u32(address >> 32);
        packet.int_sel.Assign(static_cast<InterruptSelect>(mode));
        packet.data_sel.Assign(mode == 1 ? DataSelect::None : DataSelect::Data64);
        packet.data_lo = 0x12345678;
        packet.data_hi = 0x9abcdef0;
        int interrupts = 0;
        packet.SignalFence([&] {
            ++interrupts;
            EXPECT_EQ(label, mode == 1 ? 0 : 0x9abcdef012345678ULL);
        }, [](VAddr, u16, u16) { FAIL() << "Unexpected GDS transfer"; });
        EXPECT_EQ(label, mode == 1 ? 0 : 0x9abcdef012345678ULL) << mode;
        EXPECT_EQ(interrupts, mode == 1 || mode == 2 ? 1 : 0) << mode;
    }
}

TEST(Pm4Fence, EventWriteEopInterruptModes) {
    for (u32 mode = 0; mode != 4; ++mode) {
        alignas(8) u64 label = 0;
        PM4CmdEventWriteEop packet{.header = {PM4ItOpcode::EventWriteEop, 5}};
        const auto address = reinterpret_cast<uintptr_t>(&label);
        packet.address_lo = u32(address);
        packet.address_hi.Assign(u32(address >> 32));
        packet.int_sel.Assign(static_cast<InterruptSelect>(mode));
        packet.data_sel.Assign(mode == 1 ? DataSelect::None : DataSelect::Data64);
        packet.data_lo = 0x12345678;
        packet.data_hi = 0x9abcdef0;
        int interrupts = 0;
        packet.SignalFence([&](auto* destination, auto value, size_t size) {
            EXPECT_EQ(destination, reinterpret_cast<u32*>(&label));
            std::memcpy(destination, &value, size);
        }, [&] {
            ++interrupts;
            EXPECT_EQ(label, mode == 1 ? 0 : 0x9abcdef012345678ULL);
        });
        EXPECT_EQ(label, mode == 1 ? 0 : 0x9abcdef012345678ULL) << mode;
        EXPECT_EQ(interrupts, mode == 1 || mode == 2 ? 1 : 0) << mode;
    }
}
