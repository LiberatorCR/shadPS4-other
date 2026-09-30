// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <unordered_set>
#include "shader_recompiler/frontend/control_flow_graph.h"
#include "shader_recompiler/ir/basic_block.h"
#include "shader_recompiler/ir/opcodes.h"
#include "shader_recompiler/resource.h"

namespace Shader::Optimization {

// Removing a dominating edge makes the use unreachable. This remains conservative
// for loops and joins, and never evaluates lane-dependent conditions on the host.
inline ImageResource::UseGuard FindImageUseGuard(const IR::BlockList& blocks,
                                               const IR::Block* use) {
    if (blocks.empty()) {
        return {};
    }
    for (auto it = blocks.rbegin(); it != blocks.rend(); ++it) {
        const auto* branch = *it;
        const auto* cfg = branch->cfg_block;
        if (!cfg || !cfg->branch_true || !cfg->branch_false ||
            cfg->branch_true == cfg->branch_false) {
            continue;
        }
        auto condition = branch->branch_cond.TryInst();
        bool inverted = false;
        while (condition && (condition->GetOpcode() == IR::Opcode::ConditionRef ||
                             condition->GetOpcode() == IR::Opcode::LogicalNot)) {
            inverted ^= condition->GetOpcode() == IR::Opcode::LogicalNot;
            condition = condition->Arg(0).TryInst();
        }
        if (!condition || condition->GetOpcode() != IR::Opcode::ULessThan32 ||
            !condition->Arg(1).IsImmediate()) {
            continue;
        }
        const auto* scalar = condition->Arg(0).TryInst();
        if (!scalar || scalar->GetOpcode() != IR::Opcode::ReadConst ||
            !scalar->Arg(1).IsImmediate()) {
            continue;
        }
        const auto offset = scalar->Flags<SharpLocation>();
        if (offset == 0 || offset == UNKNOWN_LOCATION) {
            continue;
        }
        for (const bool true_edge : {true, false}) {
            const auto* removed =
                (true_edge ? cfg->branch_true : cfg->branch_false)->ir_block;
            std::unordered_set<const IR::Block*> visited;
            std::vector<const IR::Block*> pending{blocks.front()};
            while (!pending.empty()) {
                const auto* block = pending.back();
                pending.pop_back();
                if (!visited.insert(block).second) {
                    continue;
                }
                for (const auto* successor : block->ImmSuccessors()) {
                    if (block != branch || successor != removed) {
                        pending.push_back(successor);
                    }
                }
            }
            if (!visited.contains(use)) {
                return {offset, condition->Arg(1).U32(), true_edge != inverted};
            }
        }
    }
    return {};
}

} // namespace Shader::Optimization
