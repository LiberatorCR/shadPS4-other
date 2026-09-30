// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <barrier>
#include <thread>
#include <gtest/gtest.h>
#include "core/libraries/kernel/equeue.h"
#include "core/platform.h"

using namespace Libraries::Kernel;

TEST(Equeue, ConcurrentPollConsumesClearEventOnce) {
    EqueueInternal queue{1, "concurrent-poll"};
    EqueueEvent event{};
    event.event.ident = 42;
    event.event.filter = OrbisKernelEvent::Filter::GraphicsCore;
    ASSERT_TRUE(queue.AddEvent(event));
    constexpr int ThreadCount = 8;
    constexpr int Rounds = 2000;
    std::barrier phase{ThreadCount};
    std::atomic<int> deliveries{0};
    std::atomic<int> wrong_payloads{0};
    std::vector<std::jthread> workers;
    for (int worker = 0; worker < ThreadCount; ++worker) {
        workers.emplace_back([&, worker] {
            const OrbisKernelUseconds poll = 0;
            for (int round = 1; round <= Rounds; ++round) {
                if (worker == 0) {
                    queue.TriggerEvent(42, OrbisKernelEvent::Filter::GraphicsCore,
                                       reinterpret_cast<void*>(static_cast<uintptr_t>(round)));
                }
                phase.arrive_and_wait();
                OrbisKernelEvent result{};
                const int count = queue.WaitForEvents(&result, 1, &poll);
                deliveries.fetch_add(count, std::memory_order_relaxed);
                if (count && (result.ident != 42 || result.data != round)) {
                    wrong_payloads.fetch_add(1, std::memory_order_relaxed);
                }
                phase.arrive_and_wait();
            }
        });
    }
    workers.clear();
    EXPECT_EQ(deliveries.load(), Rounds);
    EXPECT_EQ(wrong_payloads.load(), 0);
}

TEST(Equeue, PollRemovesOneShotAndAllowsReRegistration) {
    EqueueInternal queue{2, "one-shot"};
    const OrbisKernelUseconds poll = 0;
    for (int round = 0; round < 100; ++round) {
        EqueueEvent event{};
        event.event.ident = 7;
        event.event.filter = OrbisKernelEvent::Filter::GraphicsCore;
        event.event.flags = OrbisKernelEvent::Flags::OneShot;
        ASSERT_TRUE(queue.AddEvent(event));
        ASSERT_TRUE(queue.TriggerEvent(7, event.event.filter, nullptr));
        OrbisKernelEvent result{};
        ASSERT_EQ(queue.WaitForEvents(&result, 1, &poll), 1);
        EXPECT_FALSE(queue.EventExists(7, event.event.filter));
        EXPECT_EQ(queue.WaitForEvents(&result, 1, &poll), 0);
    }
}

TEST(Interrupts, ConcurrentChannelsKeepPersistentAndOneShotSubscribersDistinct) {
    Platform::IrqController controller;
    constexpr int ThreadCount = 8;
    constexpr int Rounds = 1000;
    std::barrier phase{ThreadCount};
    std::array<std::atomic<int>, ThreadCount> persistent{};
    std::array<std::atomic<int>, ThreadCount> once{};
    std::atomic<int> wrong_channel{};
    std::vector<std::jthread> workers;
    for (int worker = 0; worker < ThreadCount; ++worker) {
        workers.emplace_back([&, worker] {
            const auto irq = worker == 7 ? Platform::InterruptId::GfxEop
                                        : static_cast<Platform::InterruptId>(worker);
            phase.arrive_and_wait();
            controller.Register(irq, [&, worker, irq](auto actual) {
                persistent[worker]++;
                if (actual != irq) {
                    wrong_channel++;
                }
            }, &persistent[worker]);
            for (int round = 0; round < Rounds; ++round) {
                controller.RegisterOnce(irq, [&, worker, irq](auto actual) {
                    once[worker]++;
                    if (actual != irq) {
                        wrong_channel++;
                    }
                });
                controller.Signal(irq);
                controller.Signal(irq);
            }
            controller.Unregister(irq, &persistent[worker]);
            controller.Signal(irq);
        });
    }
    workers.clear();
    for (int worker = 0; worker < ThreadCount; ++worker) {
        EXPECT_EQ(persistent[worker].load(), 2 * Rounds);
        EXPECT_EQ(once[worker].load(), Rounds);
    }
    EXPECT_EQ(wrong_channel.load(), 0);
}
