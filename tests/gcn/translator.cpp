// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "shader_recompiler/runtime_info.h"
#include "translator.hpp"

#include <iostream>

#include "common/io_file.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/decode.h"
#include "shader_recompiler/frontend/translate/translate.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/basic_block.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/ir/post_order.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/recompiler.h"
#include "video_core/amdgpu/pixel_format.h"

using namespace Shader;

namespace Shader::Optimization {
void ResourceTrackingPassStub(IR::Program& program, const Profile& profile);
}

std::vector<u32> TranslateToSpirv(u64 raw_gcn_inst) {
    return TranslateToSpirv(std::span<const u64>{&raw_gcn_inst, 1});
}

static std::vector<u32> TranslateComputeToSpirv(std::span<const u64> raw_gcn_insts,
                                              u32 minimum_invocations = 0) {
    std::array<u32, 2> store{
        0xe0700000,
        0x80000000 // buffer_store_dword v0, v0, s[0:3], 0
    };
    Gcn::GcnCodeSlice second(store.data(), store.data() + store.size());

    Gcn::GcnDecodeContext decoder;
    std::vector<Gcn::GcnInst> instructions;
    instructions.reserve(raw_gcn_insts.size());
    for (const u64 raw_gcn_inst : raw_gcn_insts) {
        std::array<u32, 2> provided_inst{static_cast<u32>(raw_gcn_inst & 0xFFFFFFFFU),
                                         static_cast<u32>(raw_gcn_inst >> 32)};
        Gcn::GcnCodeSlice slice(provided_inst.data(), provided_inst.data() + provided_inst.size());
        instructions.push_back(decoder.decodeInstruction(slice));
    }
    Gcn::GcnInst store_inst = decoder.decodeInstruction(second);

    Shader::Info info{};
    info.hw_stage = HwStage::Compute;
    info.sw_stage = SwStage::Compute;
    info.flattened_ud_buf.resize(4);
    AmdGpu::Buffer buf = AmdGpu::Buffer::Null();
    std::memcpy(info.flattened_ud_buf.data(), &buf, sizeof(buf));

    IR::Program program{info};
    Pools pools{};

    IR::Block* block = pools.block_pool.Create(pools.inst_pool);
    program.blocks.push_back(block);

    program.syntax_list.emplace_back();
    program.syntax_list.back().type = IR::AbstractSyntaxNode::Type::Block;
    program.syntax_list.back().data.block = block;
    program.syntax_list.emplace_back();
    program.syntax_list.back().type = IR::AbstractSyntaxNode::Type::Return;
    program.post_order_blocks = Shader::IR::PostOrder(block);

    Profile profile{};
    profile.supported_spirv = 0x00010600;
    profile.subgroup_size = 32;

    RuntimeInfo runtime_info{};
    runtime_info.Initialize(HwStage::Compute, SwStage::Compute);
    runtime_info.props.num_user_data = 4;
    runtime_info.hw.cs.workgroup_size = {minimum_invocations ? minimum_invocations : 1, 1, 1};

    Gcn::Translator translator(program.info, runtime_info, profile);
    translator.EmitPrologue(block);

    for (int i = 0; i < 4; ++i) {
        // copy user data from SGPR to VGPR as (most?) instructions cannot access
        // two SGPRs
        Shader::Gcn::GcnInst mov{};
        mov.src[0].field = Shader::Gcn::OperandField::ScalarGPR;
        mov.src[0].code = i;
        mov.dst[0].field = Shader::Gcn::OperandField::VectorGPR;
        mov.dst[0].code = i;
        translator.S_MOV(mov);
    }
    for (const Gcn::GcnInst& inst : instructions) {
        translator.TranslateInstruction(inst);
    }
    if (minimum_invocations) {
        IR::IREmitter ir{*block};
        const auto lane = ir.GetAttributeU32(IR::Attribute::LocalInvocationId, 0);
        const auto value = ir.IAdd(ir.GetScalarReg(IR::ScalarReg::S0), ir.ISub(ir.Imm32(64), lane));
        ir.SetVectorReg(IR::VectorReg::V0, ir.GroupUMin(value));
        ir.SetVectorReg(IR::VectorReg::V1, ir.ShiftLeftLogical(lane, ir.Imm32(2)));
        store_inst.src[0].code = 1;
        store_inst.control.mubuf.offen = 1;
    }
    translator.TranslateInstruction(store_inst);

    Shader::Optimization::SsaRewritePass(program);
    Shader::Optimization::ResourceTrackingPassStub(program, profile);
    Shader::Optimization::ConstantPropagationPass(program.blocks);
    Shader::Optimization::DeadCodeEliminationPass(program);
    Shader::Optimization::CollectShaderInfoPass(program, profile);

    Backend::Bindings bindings{};

    const auto spirv = Backend::SPIRV::EmitSPIRV(profile, runtime_info, program, bindings);

    return spirv;
}

std::vector<u32> TranslateToSpirv(std::span<const u64> raw_gcn_insts) {
    return TranslateComputeToSpirv(raw_gcn_insts);
}

std::vector<u32> TranslateGroupMinimumToSpirv(u32 active_invocations) {
    return TranslateComputeToSpirv({}, active_invocations);
}

std::vector<u32> TranslateFragmentFrontFaceToSpirv(bool front_face_all_bits) {
    Shader::Info info{};
    info.hw_stage = HwStage::Fragment;
    info.sw_stage = SwStage::Fragment;

    IR::Program program{info};
    Pools pools{};
    IR::Block* block = pools.block_pool.Create(pools.inst_pool);
    program.blocks.push_back(block);
    program.syntax_list.emplace_back();
    program.syntax_list.back().type = IR::AbstractSyntaxNode::Type::Block;
    program.syntax_list.back().data.block = block;
    program.syntax_list.emplace_back();
    program.syntax_list.back().type = IR::AbstractSyntaxNode::Type::Return;
    program.post_order_blocks = IR::PostOrder(block);

    Profile profile{};
    profile.supported_spirv = 0x00010600;
    RuntimeInfo runtime_info{};
    runtime_info.Initialize(HwStage::Fragment, SwStage::Fragment);
    runtime_info.hw.fs.en_flags.front_face_ena = 1;
    runtime_info.hw.fs.addr_flags.front_face_ena = 1;
    runtime_info.hw.fs.front_face_all_bits = front_face_all_bits;
    runtime_info.hw.fs.color_buffers[0].num_format = AmdGpu::NumberFormat::Float;

    Gcn::Translator translator(program.info, runtime_info, profile);
    translator.EmitPrologue(block);

    IR::IREmitter ir{*block};
    const IR::U32 front_face = ir.GetVectorReg<IR::U32>(IR::VectorReg::V0);
    ir.SetAttribute(IR::Attribute::RenderTarget0, ir.BitCast<IR::F32>(front_face));
    ir.Epilogue();

    Optimization::SsaRewritePass(program);
    Optimization::ConstantPropagationPass(program.blocks);
    Optimization::DeadCodeEliminationPass(program);
    Optimization::CollectShaderInfoPass(program, profile);
    Backend::Bindings bindings{};
    return Backend::SPIRV::EmitSPIRV(profile, runtime_info, program, bindings);
}

std::vector<u32> TranslateFragmentPullModelToSpirv(bool use_amd_barycentrics) {
    Shader::Info info{};
    info.hw_stage = HwStage::Fragment;
    info.sw_stage = SwStage::Fragment;

    IR::Program program{info};
    Pools pools{};
    IR::Block* block = pools.block_pool.Create(pools.inst_pool);
    program.blocks.push_back(block);
    program.syntax_list.emplace_back();
    program.syntax_list.back().type = IR::AbstractSyntaxNode::Type::Block;
    program.syntax_list.back().data.block = block;
    program.syntax_list.emplace_back();
    program.syntax_list.back().type = IR::AbstractSyntaxNode::Type::Return;
    program.post_order_blocks = IR::PostOrder(block);

    Profile profile{};
    profile.supported_spirv = 0x00010600;
    profile.supports_amd_shader_explicit_vertex_parameter = use_amd_barycentrics;
    profile.supports_fragment_shader_barycentric = !use_amd_barycentrics;

    RuntimeInfo runtime_info{};
    runtime_info.Initialize(HwStage::Fragment, SwStage::Fragment);
    runtime_info.hw.fs.addr_flags.persp_pull_model_ena = 1;
    runtime_info.hw.fs.color_buffers[0].num_format = AmdGpu::NumberFormat::Float;

    IR::IREmitter ir{*block};
    ir.Prologue();
    IR::F32 sum = ir.Imm32(0.0f);
    for (u32 comp = 0; comp < 3; ++comp) {
        sum = ir.FPAdd(sum, ir.GetAttribute(IR::Attribute::BaryCoordPullModel, comp));
    }
    ir.SetAttribute(IR::Attribute::RenderTarget0, sum);
    ir.Epilogue();

    Optimization::CollectShaderInfoPass(program, profile);
    Backend::Bindings bindings{};
    return Backend::SPIRV::EmitSPIRV(profile, runtime_info, program, bindings);
}

std::vector<u32> TranslateMixedInterpolationToSpirv(bool use_amd, bool mov_first) {
    Shader::Info info{};
    info.hw_stage = HwStage::Fragment;
    info.sw_stage = SwStage::Fragment;
    IR::Program program{info};
    Pools pools{};
    IR::Block* block = pools.block_pool.Create(pools.inst_pool);
    program.blocks.push_back(block);
    program.syntax_list.emplace_back();
    program.syntax_list.back().type = IR::AbstractSyntaxNode::Type::Block;
    program.syntax_list.back().data.block = block;
    program.syntax_list.emplace_back();
    program.syntax_list.back().type = IR::AbstractSyntaxNode::Type::Return;
    program.post_order_blocks = IR::PostOrder(block);
    Profile profile{};
    profile.supported_spirv = 0x00010600;
    profile.supports_amd_shader_explicit_vertex_parameter = use_amd;
    profile.supports_fragment_shader_barycentric = !use_amd;
    RuntimeInfo runtime{};
    runtime.Initialize(HwStage::Fragment, SwStage::Fragment);
    runtime.hw.fs.num_inputs = 1;
    runtime.hw.fs.inputs[0].param_index = 3;
    runtime.hw.fs.addr_flags.persp_sample_ena = 1;
    runtime.hw.fs.en_flags.persp_sample_ena = 1;
    runtime.hw.fs.color_buffers[0].num_format = AmdGpu::NumberFormat::Float;
    Gcn::Translator translator(program.info, runtime, profile);
    translator.EmitPrologue(block);
    Gcn::GcnInst mov{}, p2{};
    mov.src[0].code = 2;
    mov.dst[0].field = Gcn::OperandField::VectorGPR;
    mov.dst[0].code = 2;
    mov.control.vintrp.chan = 1;
    p2.src[0].code = 1;
    p2.src[0].field = Gcn::OperandField::VectorGPR;
    p2.dst[0].code = 3;
    p2.dst[0].field = Gcn::OperandField::VectorGPR;
    if (mov_first) {
        translator.V_INTERP_MOV_F32(mov);
        translator.V_INTERP_P2_F32(p2);
    } else {
        translator.V_INTERP_P2_F32(p2);
        translator.V_INTERP_MOV_F32(mov);
    }
    IR::IREmitter ir{*block};
    ir.SetAttribute(IR::Attribute::RenderTarget0, ir.GetVectorReg<IR::F32>(IR::VectorReg::V3), 0);
    ir.SetAttribute(IR::Attribute::RenderTarget0, ir.GetVectorReg<IR::F32>(IR::VectorReg::V2), 1);
    ir.Epilogue();
    Optimization::SsaRewritePass(program);
    Optimization::ConstantPropagationPass(program.blocks);
    Optimization::DeadCodeEliminationPass(program);
    Optimization::CollectShaderInfoPass(program, profile);
    Backend::Bindings bindings{};
    return Backend::SPIRV::EmitSPIRV(profile, runtime, program, bindings);
}

FragmentInterpMovInfo TranslateFragmentInterpMovSelector(u32 src_select, bool flat_shade,
                                                         bool offset5) {
    Shader::Info info{};
    info.hw_stage = HwStage::Fragment;
    info.sw_stage = SwStage::Fragment;

    IR::Program program{info};
    Pools pools{};
    IR::Block* block = pools.block_pool.Create(pools.inst_pool);
    program.blocks.push_back(block);

    Profile profile{};
    profile.supports_fragment_shader_barycentric = true;
    RuntimeInfo runtime_info{};
    runtime_info.Initialize(HwStage::Fragment, SwStage::Fragment);
    runtime_info.hw.fs.inputs[0].is_flat = flat_shade;
    runtime_info.hw.fs.inputs[0].is_default = offset5;

    Gcn::Translator translator(program.info, runtime_info, profile);
    translator.EmitPrologue(block);

    Gcn::GcnInst inst{};
    inst.src[0].code = src_select;
    inst.dst[0].field = Gcn::OperandField::VectorGPR;
    inst.dst[0].code = 0;
    inst.control.vintrp.attr = 0;
    inst.control.vintrp.chan = 0;
    translator.V_INTERP_MOV_F32(inst);

    FragmentInterpMovInfo interp_info{};
    for (const IR::Inst& ir_inst : block->Instructions()) {
        if (ir_inst.GetOpcode() == IR::Opcode::GetAttribute &&
            ir_inst.Arg(0).Attribute() == IR::Attribute::Param0) {
            interp_info.attribute_indices.push_back(ir_inst.Arg(2).U32());
        } else if (ir_inst.GetOpcode() == IR::Opcode::FPSub32) {
            ++interp_info.fsub_count;
        }
    }
    return interp_info;
}

std::vector<u32> TranslateDataShareAddresses(DataShareTestOperation operation, bool gds, u32 m0,
                                             u32 address) {
    Shader::Info info{};
    info.hw_stage = HwStage::Compute;
    info.sw_stage = SwStage::Compute;
    IR::Program program{info};
    Pools pools{};
    IR::Block* block = pools.block_pool.Create(pools.inst_pool);
    program.blocks.push_back(block);
    program.post_order_blocks = IR::PostOrder(block);
    Profile profile{};
    RuntimeInfo runtime_info{};
    runtime_info.Initialize(HwStage::Compute, SwStage::Compute);
    Gcn::Translator translator(program.info, runtime_info, profile);
    translator.EmitPrologue(block);
    IR::IREmitter ir{*block};
    ir.SetM0(ir.Imm32(m0));
    ir.SetVectorReg(IR::VectorReg::V0, ir.Imm32(address));
    ir.SetVectorReg(IR::VectorReg::V1, ir.Imm32(7u));
    ir.SetVectorReg(IR::VectorReg::V2, ir.Imm32(9u));
    Gcn::GcnInst inst{};
    constexpr std::array opcodes{Gcn::Opcode::DS_READ_B32,    Gcn::Opcode::DS_WRITE_B32,
                                 Gcn::Opcode::DS_ADD_RTN_U32, Gcn::Opcode::DS_CMPST_RTN_B32,
                                 Gcn::Opcode::DS_READ2_B32,   Gcn::Opcode::DS_WRITE2_B32};
    inst.opcode = opcodes[static_cast<u32>(operation)];
    for (u32 i = 0; i < 3; ++i) {
        inst.src[i].field = Gcn::OperandField::VectorGPR;
        inst.src[i].code = i;
    }
    inst.dst[0].field = Gcn::OperandField::VectorGPR;
    inst.dst[0].code = 3;
    inst.control.ds.gds = gds;
    inst.control.ds.offset0 = 4;
    inst.control.ds.offset1 = operation == DataShareTestOperation::ReadPair ||
                                      operation == DataShareTestOperation::WritePair
                                  ? 7
                                  : 0;
    translator.EmitDataShare(inst);
    Optimization::SsaRewritePass(program);
    Optimization::ConstantPropagationPass(program.blocks);
    std::vector<u32> addresses;
    for (const IR::Inst& op : block->Instructions()) {
        switch (op.GetOpcode()) {
        case IR::Opcode::LoadSharedU32:
        case IR::Opcode::WriteSharedU32:
        case IR::Opcode::SharedAtomicIAdd32:
        case IR::Opcode::SharedAtomicCmpSwap32:
            addresses.push_back(op.Arg(0).U32());
            break;
        default:
            break;
        }
    }
    return addresses;
}
