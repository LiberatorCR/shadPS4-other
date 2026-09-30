// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cmath>
#include <bit>
#include <limits>
#include <unordered_map>
#include <utility>

#include <gtest/gtest.h>
#include <half.hpp>
#include <spirv/unified1/spirv.hpp11>

#include "gcn_test_runner.hpp"
#include "instructions.hpp"
#include "translator.hpp"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/resource_use_guard.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/runtime_info.h"

TEST(ShaderSharedMemory, StorageFallbackUsesTypedIndicesAcrossWorkgroups) {
    for (int operation = 0; operation < 4; ++operation) {
        const int bits = operation == 0 ? 16 : operation == 1 ? 32 : 64;
        Shader::Info info{};
        info.hw_stage = Shader::HwStage::Compute;
        Shader::IR::Program program{info};
        Common::ObjectPool<Shader::IR::Inst> pool{64};
        Shader::IR::Block block{pool};
        program.blocks.push_back(&block);
        Shader::IR::IREmitter ir{block};
        const Shader::IR::Value value = operation == 3
            ? Shader::IR::Value{ir.SharedAtomicIAdd(
                  ir.Imm32(16), Shader::IR::U32U64{ir.Imm64(u64{1})}, false)}
            : ir.LoadShared(bits, false, ir.Imm32(16));
        ir.WriteShared(bits, value, ir.Imm32(24));
        Shader::RuntimeInfo runtime{};
        runtime.hw.cs.shared_memory_size = 64;
        Shader::Profile profile{};
        profile.max_shared_memory_size = 0;
        Shader::Optimization::SharedMemoryToStoragePass(program, runtime, profile);
        ASSERT_EQ(info.buffers.size(), 1);
        EXPECT_EQ(info.buffers[0].buffer_type, Shader::BufferType::SharedMemory);
        for (auto& inst : block.Instructions()) {
            if (inst.GetOpcode() == Shader::IR::Opcode::GetAttributeU32) {
                inst.ReplaceUsesWith(ir.Imm32(3));
            }
        }
        Shader::Optimization::ConstantPropagationPass(program.blocks);
        int reads = 0, writes = 0;
        for (const auto& inst : block.Instructions()) {
            switch (inst.GetOpcode()) {
            case Shader::IR::Opcode::LoadBufferU16:
            case Shader::IR::Opcode::LoadBufferU32:
            case Shader::IR::Opcode::LoadBufferU64:
            case Shader::IR::Opcode::BufferAtomicIAdd64:
                ASSERT_TRUE(inst.Arg(1).IsImmediate());
                EXPECT_EQ(inst.Arg(1).U32(), (3 * 64 + 16) / (bits / 8));
                ++reads;
                break;
            case Shader::IR::Opcode::StoreBufferU16:
            case Shader::IR::Opcode::StoreBufferU32:
            case Shader::IR::Opcode::StoreBufferU64:
                ASSERT_TRUE(inst.Arg(1).IsImmediate());
                EXPECT_EQ(inst.Arg(1).U32(), (3 * 64 + 24) / (bits / 8));
                ++writes;
                break;
            default:
                break;
            }
        }
        EXPECT_EQ(reads, 1);
        EXPECT_EQ(writes, 1);
    }
}

TEST(ShaderHardwareIntrinsics, PackedAncillaryPreservesMultipleExtracts) {
    Shader::Info info{};
    Shader::IR::Program program{info};
    Common::ObjectPool<Shader::IR::Inst> pool{64};
    Shader::IR::Block block{pool};
    program.blocks.push_back(&block);
    Shader::IR::IREmitter ir{block};
    const auto packed = ir.GetAttributeU32(Shader::IR::Attribute::PackedAncillary);
    const auto sample = ir.BitFieldExtract(packed, ir.Imm32(8), ir.Imm32(4));
    const auto target = ir.BitFieldExtract(packed, ir.Imm32(16), ir.Imm32(11));
    const auto sum = ir.IAdd(sample, target);
    Shader::Optimization::LowerHardwareIntrinsics(program);
    ASSERT_FALSE(sum.Inst()->Arg(0).IsImmediate());
    ASSERT_FALSE(sum.Inst()->Arg(1).IsImmediate());
    EXPECT_EQ(sum.Inst()->Arg(0).Inst()->Arg(0).Attribute(), Shader::IR::Attribute::SampleIndex);
    EXPECT_EQ(sum.Inst()->Arg(1).Inst()->Arg(0).Attribute(), Shader::IR::Attribute::RenderTargetIndex);
}

TEST(ShaderResourceUse, ScalarGuardRequiresADominatingEdge) {
    Common::ObjectPool<Shader::IR::Inst> inst_pool{64};
    Shader::IR::Block entry{inst_pool}, conditional{inst_pool}, output{inst_pool}, exit{inst_pool};
    Shader::Gcn::Block cfg{};
    Shader::Gcn::Block output_cfg{}, exit_cfg{};
    output_cfg.ir_block = &output;
    exit_cfg.ir_block = &exit;
    cfg.branch_true = &exit_cfg;
    cfg.branch_false = &output_cfg;
    conditional.cfg_block = &cfg;
    entry.AddBranch(&conditional);
    conditional.AddBranch(&exit);
    conditional.AddBranch(&output);
    output.AddBranch(&exit);
    Shader::IR::IREmitter ir{conditional};
    auto count = ir.ReadConst(ir.CompositeConstruct(ir.Imm32(0), ir.Imm32(0)), ir.Imm32(0));
    count.Inst()->SetFlags(16u);
    conditional.branch_cond = ir.ConditionRef(ir.ILessThan(count, ir.Imm32(4), false));
    const Shader::IR::BlockList blocks{&entry, &conditional, &output, &exit};
    auto guard = Shader::Optimization::FindImageUseGuard(blocks, &output);
    EXPECT_EQ(guard.offset, 16u);
    EXPECT_EQ(guard.limit, 4u);
    std::array<u32, 17> flatbuf{};
    flatbuf[16] = 3;
    EXPECT_FALSE(guard.Active(flatbuf));
    flatbuf[16] = 4;
    EXPECT_TRUE(guard.Active(flatbuf));
    // The join is reachable on either edge, so it cannot inherit the guard.
    EXPECT_EQ(Shader::Optimization::FindImageUseGuard(blocks, &exit).offset,
              Shader::UNKNOWN_LOCATION);
    // Another path into the output makes this edge non-dominating.
    entry.AddBranch(&output);
    EXPECT_EQ(Shader::Optimization::FindImageUseGuard(blocks, &output).offset,
              Shader::UNKNOWN_LOCATION);
}

TEST(ShaderInterpolation, MixedComponentsKeepTheirModesInEitherInstructionOrder) {
    for (const bool amd : {false, true}) {
        for (const bool mov_first : {false, true}) {
            const auto code = TranslateMixedInterpolationToSpirv(amd, mov_first);
            std::unordered_map<u32, std::unordered_map<u32, u32>> decorations;
            for (size_t offset = 5; offset < code.size();) {
                const u32 count = code[offset] >> 16;
                ASSERT_GT(count, 0u);
                ASSERT_LE(offset + count, code.size());
                if ((code[offset] & 0xffffu) == u32(spv::Op::OpDecorate) && count >= 3) {
                    decorations[code[offset + 1]][code[offset + 2]] = count > 3 ? code[offset + 3] : 0;
                }
                offset += count;
            }
            u32 found = 0;
            for (const auto& [id, dec] : decorations) {
                if (!dec.contains(u32(spv::Decoration::Component))) {
                    continue;
                }
                ASSERT_TRUE(dec.contains(u32(spv::Decoration::Location)));
                EXPECT_EQ(dec.at(u32(spv::Decoration::Location)), 3u);
                const auto component = dec.at(u32(spv::Decoration::Component));
                const auto per_vertex = u32(amd ? spv::Decoration::ExplicitInterpAMD
                                               : spv::Decoration::PerVertexKHR);
                if (component == 0) {
                    EXPECT_TRUE(dec.contains(u32(spv::Decoration::Sample)));
                    EXPECT_FALSE(dec.contains(per_vertex));
                } else {
                    EXPECT_EQ(component, 1u);
                    EXPECT_TRUE(dec.contains(per_vertex));
                    EXPECT_FALSE(dec.contains(u32(spv::Decoration::Sample)));
                }
                ++found;
            }
            EXPECT_EQ(found, 2u);
        }
    }
}

class GcnTest : public ::testing::Test {
protected:
    void SetUp() override {}

    void TearDown() override {}

    static void TearDownTestSuite() {
        gcn_test::Runner::DestroyInstance();
    }
};

TEST_F(GcnTest, bfm_uses_five_bit_width_and_offset) {
    auto runner = gcn_test::Runner::instance().value();
    const auto spirv = TranslateToSpirv(VOP2(OpcodeVOP2::V_BFM_B32, VOperand8::V0,
                                          SOperand9::V0, VOperand8::V1).Get());
    for (const auto width : {0U, 16U, 31U, 32U, 49U}) {
        for (const auto offset : {0U, 16U, 31U, 32U, 49U}) {
            const auto result = runner->run<u32>(spirv, std::array{width, offset, 0U, 0U});
            ASSERT_TRUE(result.has_value());
            EXPECT_EQ(*result, ((1U << (width & 31)) - 1U) << (offset & 31));
        }
    }
}

TEST_F(GcnTest, align_handles_zero_and_wrapped_shifts) {
    auto runner = gcn_test::Runner::instance().value();
    constexpr u32 high = 0x81234567, low = 0xfedcba98;
    for (const auto opcode : {OpcodeVOP3::V_ALIGNBIT_B32, OpcodeVOP3::V_ALIGNBYTE_B32}) {
        const auto spirv = TranslateToSpirv(VOP3A(opcode, VOperand8::V0, SOperand9::V0,
                                                SOperand9::V1, SOperand9::V2).Get());
        for (const auto input : {0U, 1U, 3U, 4U, 16U, 31U, 32U, 63U}) {
            const auto result = runner->run<u32>(spirv, std::array{high, low, input, 0U});
            ASSERT_TRUE(result.has_value());
            const u32 shift = opcode == OpcodeVOP3::V_ALIGNBIT_B32 ? input & 31 : (input & 3) * 8;
            EXPECT_EQ(*result, u32(((u64{high} << 32) | low) >> shift));
        }
    }
}

TEST(GcnDataShare, IndexedGdsUsesM0ByteBase) {
    for (const auto operation :
         {DataShareTestOperation::Read, DataShareTestOperation::Write, DataShareTestOperation::Add,
          DataShareTestOperation::CompareExchange}) {
        EXPECT_EQ(TranslateDataShareAddresses(operation, true, 0x07000040, 12),
                  (std::vector<u32>{0x710}));
        EXPECT_EQ(TranslateDataShareAddresses(operation, true, 0x09280040, 12),
                  (std::vector<u32>{0x938}));
    }
}

TEST(GcnDataShare, PairedGdsOffsetsAreDwordsAfterByteBase) {
    for (const auto operation :
         {DataShareTestOperation::ReadPair, DataShareTestOperation::WritePair}) {
        EXPECT_EQ(TranslateDataShareAddresses(operation, true, 0x07000040, 12),
                  (std::vector<u32>{0x71c, 0x728}));
    }
}

TEST(GcnDataShare, LdsDoesNotUseM0AsAddressBase) {
    for (const auto operation :
         {DataShareTestOperation::Read, DataShareTestOperation::Write, DataShareTestOperation::Add,
          DataShareTestOperation::CompareExchange}) {
        EXPECT_EQ(TranslateDataShareAddresses(operation, false, 0x07000040, 12),
                  (std::vector<u32>{16}));
    }
    EXPECT_EQ(TranslateDataShareAddresses(DataShareTestOperation::ReadPair, false, 0x07000040, 12),
              (std::vector<u32>{28, 40}));
}

struct F32x2 {
    float a;
    float b;
};

struct FrontFaceSpirvInfo {
    bool has_front_facing{};
    u32 select_count{};
    u32 true_value{};
    u32 false_value{};
};

FrontFaceSpirvInfo InspectFrontFaceSpirv(const std::vector<u32>& spirv) {
    FrontFaceSpirvInfo info{};
    if (spirv.size() < 5U) {
        ADD_FAILURE() << "SPIR-V header is truncated";
        return info;
    }

    std::unordered_map<u32, u32> constants;
    u32 true_value_id{};
    u32 false_value_id{};
    for (size_t offset = 5; offset < spirv.size();) {
        const u32 instruction = spirv[offset];
        const u32 word_count = instruction >> 16;
        const auto opcode = static_cast<spv::Op>(instruction & 0xffffU);
        if (word_count == 0U || offset + word_count > spirv.size()) {
            ADD_FAILURE() << "Malformed SPIR-V instruction at word " << offset;
            return info;
        }

        if (opcode == spv::Op::OpDecorate && word_count >= 4U &&
            spirv[offset + 2] == static_cast<u32>(spv::Decoration::BuiltIn) &&
            spirv[offset + 3] == static_cast<u32>(spv::BuiltIn::FrontFacing)) {
            info.has_front_facing = true;
        } else if (opcode == spv::Op::OpConstant && word_count == 4U) {
            constants.emplace(spirv[offset + 2], spirv[offset + 3]);
        } else if (opcode == spv::Op::OpSelect && word_count == 6U) {
            ++info.select_count;
            true_value_id = spirv[offset + 4];
            false_value_id = spirv[offset + 5];
        }
        offset += word_count;
    }

    if (info.select_count == 1U && constants.contains(true_value_id) &&
        constants.contains(false_value_id)) {
        info.true_value = constants.at(true_value_id);
        info.false_value = constants.at(false_value_id);
    }
    return info;
}

struct PullModelSpirvInfo {
    u32 bary_coord_khr_count{};
    u32 frag_coord_count{};
    u32 pull_model_amd_count{};
    u32 fragment_barycentric_khr_count{};
    u32 fmul_count{};
};

PullModelSpirvInfo InspectPullModelSpirv(const std::vector<u32>& spirv) {
    PullModelSpirvInfo info{};
    if (spirv.size() < 5U) {
        ADD_FAILURE() << "SPIR-V header is truncated";
        return info;
    }

    for (size_t offset = 5; offset < spirv.size();) {
        const u32 instruction = spirv[offset];
        const u32 word_count = instruction >> 16;
        const auto opcode = static_cast<spv::Op>(instruction & 0xffffU);
        if (word_count == 0U || offset + word_count > spirv.size()) {
            ADD_FAILURE() << "Malformed SPIR-V instruction at word " << offset;
            return info;
        }

        if (opcode == spv::Op::OpCapability && word_count >= 2U &&
            spirv[offset + 1] == static_cast<u32>(spv::Capability::FragmentBarycentricKHR)) {
            ++info.fragment_barycentric_khr_count;
        } else if (opcode == spv::Op::OpDecorate && word_count >= 4U &&
                   spirv[offset + 2] == static_cast<u32>(spv::Decoration::BuiltIn)) {
            const auto builtin = static_cast<spv::BuiltIn>(spirv[offset + 3]);
            switch (builtin) {
            case spv::BuiltIn::BaryCoordKHR:
                ++info.bary_coord_khr_count;
                break;
            case spv::BuiltIn::FragCoord:
                ++info.frag_coord_count;
                break;
            case spv::BuiltIn::BaryCoordPullModelAMD:
                ++info.pull_model_amd_count;
                break;
            default:
                break;
            }
        } else if (opcode == spv::Op::OpFMul) {
            ++info.fmul_count;
        }
        offset += word_count;
    }
    return info;
}

TEST_F(GcnTest, fragment_front_face_uses_float_sign_bits) {
    const auto info = InspectFrontFaceSpirv(TranslateFragmentFrontFaceToSpirv(false));

    EXPECT_TRUE(info.has_front_facing);
    EXPECT_EQ(info.select_count, 1U);
    EXPECT_EQ(info.true_value, 0x3f800000U);
    EXPECT_EQ(info.false_value, 0xbf800000U);
}

TEST_F(GcnTest, fragment_front_face_uses_all_bits) {
    const auto info = InspectFrontFaceSpirv(TranslateFragmentFrontFaceToSpirv(true));

    EXPECT_TRUE(info.has_front_facing);
    EXPECT_EQ(info.select_count, 1U);
    EXPECT_EQ(info.true_value, 1U);
    EXPECT_EQ(info.false_value, 0U);
}

TEST_F(GcnTest, khr_barycentrics_reconstruct_pull_model) {
    const auto info = InspectPullModelSpirv(TranslateFragmentPullModelToSpirv(false));

    EXPECT_EQ(info.bary_coord_khr_count, 1U);
    EXPECT_EQ(info.frag_coord_count, 1U);
    EXPECT_EQ(info.fragment_barycentric_khr_count, 1U);
    EXPECT_EQ(info.pull_model_amd_count, 0U);
    EXPECT_EQ(info.fmul_count, 2U);
}

TEST_F(GcnTest, amd_barycentrics_use_native_pull_model) {
    const auto info = InspectPullModelSpirv(TranslateFragmentPullModelToSpirv(true));

    EXPECT_EQ(info.pull_model_amd_count, 1U);
    EXPECT_EQ(info.frag_coord_count, 0U);
    EXPECT_EQ(info.bary_coord_khr_count, 0U);
    EXPECT_EQ(info.fmul_count, 0U);
}

TEST_F(GcnTest, interp_mov_selects_p10_p20_and_p0) {
    const auto p10 = TranslateFragmentInterpMovSelector(0, false, false);
    EXPECT_EQ(p10.attribute_indices, (std::vector<u32>{1U, 0U}));
    EXPECT_EQ(p10.fsub_count, 1U);

    const auto p20 = TranslateFragmentInterpMovSelector(1, false, false);
    EXPECT_EQ(p20.attribute_indices, (std::vector<u32>{2U, 0U}));
    EXPECT_EQ(p20.fsub_count, 1U);

    const auto p0 = TranslateFragmentInterpMovSelector(2, false, false);
    EXPECT_EQ(p0.attribute_indices, (std::vector<u32>{0U}));
    EXPECT_EQ(p0.fsub_count, 0U);
}

TEST_F(GcnTest, interp_mov_uses_vertex_values_only_for_passthrough_inputs) {
    for (const auto [flat_shade, offset5] :
         {std::pair{false, false}, std::pair{true, false}, std::pair{false, true}}) {
        const auto p10 = TranslateFragmentInterpMovSelector(0, flat_shade, offset5);
        EXPECT_EQ(p10.attribute_indices, (std::vector<u32>{1U, 0U}));
        EXPECT_EQ(p10.fsub_count, 1U);
    }

    const auto p10 = TranslateFragmentInterpMovSelector(0, true, true);
    EXPECT_EQ(p10.attribute_indices, (std::vector<u32>{1U}));
    EXPECT_EQ(p10.fsub_count, 0U);
}

// Example
// TEST_F(GcnTest, test_name) {
//     // Runner sets the vulkan context
//     auto runner = gcn_test::Runner::instance().value();
//
//     // v_add_f32 v0, v0, v1
//     auto spirv = TranslateToSpirv(VOP2(OpcodeVOP2::V_ADD_F32, VOperand8::V0, SOperand9::V0, VOperand8::V1).Get());
//
//     // run<T> tells how to interpret the result (only 32bit as of now)
//     // the second argument is templated, it can be at most 4 u32s
//     // the data is accessible by the instruction in v0-4 and s0-4 (mirrored)
//     // the result has to be placed in v0
//     auto result = runner->run<float>(spirv, F32x2{1.5f, 6.0f});
//
//     EXPECT_TRUE(result.has_value());
//     EXPECT_EQ(*result, 7.5f);
// }

TEST_F(GcnTest, add_f32) {
    auto runner = gcn_test::Runner::instance().value();

    auto spirv = TranslateToSpirv(VOP2(OpcodeVOP2::V_ADD_F32, VOperand8::V0, SOperand9::V0, VOperand8::V1).Get());
    auto result = runner->run<float>(spirv, F32x2{1.5f, 6.0f});

    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(*result, 7.5f);
}

TEST_F(GcnTest, add_i32_carry_feeds_addc_u32) {
    auto runner = gcn_test::Runner::instance().value();
    const std::array<u64, 2> instructions{
        VOP2(OpcodeVOP2::V_ADD_I32, VOperand8::V1, SOperand9::S0, VOperand8::V1).Get(),
        VOP2(OpcodeVOP2::V_ADDC_U32, VOperand8::V0, SOperand9::S2, VOperand8::V3).Get(),
    };
    const auto spirv = TranslateToSpirv(instructions);

    const auto overflow = runner->run<u32>(spirv, std::array{0xffffffffU, 1U, 7U, 0U});
    ASSERT_TRUE(overflow.has_value());
    EXPECT_EQ(*overflow, 8U);

    const auto no_overflow = runner->run<u32>(spirv, std::array{2U, 1U, 7U, 0U});
    ASSERT_TRUE(no_overflow.has_value());
    EXPECT_EQ(*no_overflow, 7U);
}

TEST_F(GcnTest, add_nan) {
    auto runner = gcn_test::Runner::instance().value();

    auto spirv = TranslateToSpirv(VOP2(OpcodeVOP2::V_ADD_F32, VOperand8::V0, SOperand9::V0, VOperand8::V1).Get());
    auto result = runner->run<float>(spirv, F32x2{1.0f, std::numeric_limits<float>::quiet_NaN()});

    EXPECT_TRUE(result.has_value());
    EXPECT_TRUE(std::isnan(*result));
}

using half = half_float::half;

struct F16x2 {
    half a;
    half b = half(0.0f);

    bool operator==(const F16x2& rhs) const = default;
};

static_assert(sizeof(F16x2) == sizeof(float));

TEST_F(GcnTest, add_f16) {
    auto runner = gcn_test::Runner::instance().value();

    auto spirv = TranslateToSpirv(VOP2(OpcodeVOP2::V_ADD_F16, VOperand8::V0, SOperand9::V0, VOperand8::V1).Get());
    auto result = runner->run<F16x2>(spirv, std::array{F16x2{half(1.0f)}, F16x2{half(1.0f)}});

    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(*result, F16x2{half(2.0f)});
}

TEST_F(GcnTest, add_f16_clamp) {
    auto runner = gcn_test::Runner::instance().value();

    auto spirv = TranslateToSpirv(VOP3A(OpcodeVOP3::V_ADD_F16, VOperand8::V0, SOperand9::V0, SOperand9::V1).SetClamp(true).Get());
    auto result = runner->run<F16x2>(spirv, std::array{F16x2{half(1.0f)}, F16x2{half(1.0f)}});

    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(*result, F16x2{half(1.0f)}); //confirmed with neo
}

TEST_F(GcnTest, add_f16_neg) {
    auto runner = gcn_test::Runner::instance().value();

    auto spirv = TranslateToSpirv(VOP3A(OpcodeVOP3::V_ADD_F16, VOperand8::V0, SOperand9::V0, SOperand9::V1).SetNeg({true, true, false}).Get());
    auto result = runner->run<F16x2>(spirv, std::array{F16x2{half(1.0f)}, F16x2{half(1.0f)}});

    EXPECT_TRUE(result.has_value());
    EXPECT_EQ((*result).a, half(-2.0f)); //confirmed with neo
}

TEST_F(GcnTest, add_f16_opsel_hi) {
    auto runner = gcn_test::Runner::instance().value();

    auto spirv = TranslateToSpirv(VOP3A(OpcodeVOP3::V_ADD_F16, VOperand8::V0, SOperand9::V0, SOperand9::V1).SetOpSel({true, true, false, true}).Get());
    auto result = runner->run<F16x2>(spirv, std::array{F16x2{half(1.0f), half(2.0f)}, F16x2{half(1.0f), half(2.0f)}});

    EXPECT_TRUE(result.has_value());
    EXPECT_EQ((*result).a, half(1.0f));
    EXPECT_EQ((*result).b, half(4.0f));
}

TEST_F(GcnTest, sub_f16) {
    auto runner = gcn_test::Runner::instance().value();

    auto spirv = TranslateToSpirv(VOP2(OpcodeVOP2::V_SUB_F16, VOperand8::V0, SOperand9::V0, VOperand8::V1).Get());
    auto result = runner->run<F16x2>(spirv, std::array{F16x2{half(0.0f)}, F16x2{half(1.0f)}});

    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(*result, F16x2{half(-1.0f)}); //confirmed with neo
}

TEST_F(GcnTest, mul_legacy_nan) {
    auto runner = gcn_test::Runner::instance().value();

    auto spirv = TranslateToSpirv(VOP2(OpcodeVOP2::V_MUL_LEGACY_F32, VOperand8::V0, SOperand9::V0, VOperand8::V1).Get());
    auto result = runner->run<u32>(spirv, std::array{u32(0), u32(0x7fc00000)});

    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(*result, 0);
}

TEST_F(GcnTest, mul_nan) {
    auto runner = gcn_test::Runner::instance().value();

    auto spirv = TranslateToSpirv(VOP2(OpcodeVOP2::V_MUL_F32, VOperand8::V0, SOperand9::V0, VOperand8::V1).Get());
    auto result = runner->run<float>(spirv, std::array{u32(0), u32(0x7fc00000)});

    EXPECT_TRUE(result.has_value());
    EXPECT_TRUE(std::isnan(*result));
}

TEST_F(GcnTest, min_legacy_nan) {
    auto runner = gcn_test::Runner::instance().value();

    auto spirv = TranslateToSpirv(VOP2(OpcodeVOP2::V_MIN_LEGACY_F32, VOperand8::V0, SOperand9::V0, VOperand8::V1).Get());
    auto result = runner->run<u32>(spirv, std::array{u32(0), u32(0x7fc00000)});

    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(*result, 0x7fc00000);
}

TEST_F(GcnTest, min_nan) {
    auto runner = gcn_test::Runner::instance().value();

    auto spirv = TranslateToSpirv(VOP2(OpcodeVOP2::V_MIN_F32, VOperand8::V0, SOperand9::V0, VOperand8::V1).Get());
    auto result = runner->run<float>(spirv, std::array{u32(0), u32(0x7fc00000)});

    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(*result, 0);
}

TEST_F(GcnTest, add3_u32_1) {
    auto runner = gcn_test::Runner::instance().value();

    auto spirv = TranslateToSpirv(VOP3A(OpcodeVOP3::V_ADD3_U32, VOperand8::V0, SOperand9::V0, SOperand9::V1, SOperand9::V2).Get());
    auto result = runner->run<u32>(spirv, std::array{0, 1, 2});

    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(*result, 3);
}

TEST_F(GcnTest, add3_u32_2) {
    auto runner = gcn_test::Runner::instance().value();
    auto big = 2000000000;

    auto spirv = TranslateToSpirv(VOP3A(OpcodeVOP3::V_ADD3_U32, VOperand8::V0, SOperand9::V0, SOperand9::V1, SOperand9::V2).Get());
    auto result = runner->run<u32>(spirv, std::array{big, big, big});

    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(*result, 0x65A0BC00);
}

TEST_F(GcnTest, add3_u32_3) {
    auto runner = gcn_test::Runner::instance().value();
    auto big = 2000000000;

    auto spirv = TranslateToSpirv(VOP3A(OpcodeVOP3::V_ADD3_U32, VOperand8::V0, SOperand9::V0, SOperand9::V1, SOperand9::V2).SetClamp(true).Get());
    auto result = runner->run<u32>(spirv, std::array{big, big, big});

    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(*result, 0x65A0BC00);
}

TEST_F(GcnTest, add3_u32_4) {
    auto runner = gcn_test::Runner::instance().value();

    auto spirv = TranslateToSpirv(VOP3A(OpcodeVOP3::V_ADD3_U32, VOperand8::V0, SOperand9::V0, SOperand9::V1, SOperand9::V2).SetNeg({1,0,0}).Get());
    auto result = runner->run<u32>(spirv, std::array{0, 1, 2});

    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(*result, 0x80000003);
}

TEST_F(GcnTest, or3_u32_1) {
    auto runner = gcn_test::Runner::instance().value();

    auto spirv = TranslateToSpirv(VOP3A(OpcodeVOP3::V_OR3_B32, VOperand8::V0, SOperand9::V0, SOperand9::V1, SOperand9::V2).Get());
    auto result = runner->run<u32>(spirv, std::array<u32,3>{0xF0F0F0F0, 0x07070707, 0x11111111});

    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(*result, 0xF7F7F7F7);
}

TEST_F(GcnTest, or3_u32_2) {
    auto runner = gcn_test::Runner::instance().value();

    auto spirv = TranslateToSpirv(VOP3A(OpcodeVOP3::V_OR3_B32, VOperand8::V0, SOperand9::V0, SOperand9::V1, SOperand9::V2).Get());
    auto result = runner->run<u32>(spirv, std::array{0x07070707, 0x11111111, 0x40404040});

    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(*result, 0x57575757);
}

TEST_F(GcnTest, or3_u32_3) {
    auto runner = gcn_test::Runner::instance().value();
    auto big = 2000000000;

    auto spirv = TranslateToSpirv(VOP3A(OpcodeVOP3::V_OR3_B32, VOperand8::V0, SOperand9::V0, SOperand9::V1, SOperand9::V2).SetClamp(true).Get());
    auto result = runner->run<u32>(spirv, std::array{0x07070707, 0x11111111, 0x40404040});

    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(*result, 0x57575757);
}

TEST_F(GcnTest, or3_u32_4) {
    auto runner = gcn_test::Runner::instance().value();

    auto spirv = TranslateToSpirv(VOP3A(OpcodeVOP3::V_OR3_B32, VOperand8::V0, SOperand9::V0, SOperand9::V1, SOperand9::V2).SetNeg({0,0,1}).Get());
    auto result = runner->run<u32>(spirv, std::array{0x07070707, 0x11111111, 0x40404040});

    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(*result, 0xD7575757);
}

TEST_F(GcnTest, and_or_b32_1) {
    auto runner = gcn_test::Runner::instance().value();

    auto spirv = TranslateToSpirv(VOP3A(OpcodeVOP3::V_AND_OR_B32, VOperand8::V0, SOperand9::V0, SOperand9::V1, SOperand9::V2).Get());
    auto result = runner->run<u32>(spirv, std::array<u32,3>{0xF0F0F0F0, 0x07070707, 0x11111111});

    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(*result, 0x11111111);
}

TEST_F(GcnTest, and_or_b32_2) {
    auto runner = gcn_test::Runner::instance().value();

    auto spirv = TranslateToSpirv(VOP3A(OpcodeVOP3::V_AND_OR_B32, VOperand8::V0, SOperand9::V0, SOperand9::V1, SOperand9::V2).SetOmod(Omod::Mul2).Get());
    auto result = runner->run<u32>(spirv, std::array{0x40404040, 0x40404040, 0x40404040});

    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(*result, 0x40404040);
}

TEST_F(GcnTest, and_or_b32_3) {
    auto runner = gcn_test::Runner::instance().value();

    auto spirv = TranslateToSpirv(VOP3A(OpcodeVOP3::V_AND_OR_B32, VOperand8::V0, SOperand9::V0, SOperand9::V1, SOperand9::V2).SetClamp(true).Get());
    auto result = runner->run<u32>(spirv, std::array{0x40404040, 0x40404040, 0x40404040});

    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(*result, 0x40404040);
}

TEST_F(GcnTest, and_or_b32_4) {
    auto runner = gcn_test::Runner::instance().value();

    auto spirv = TranslateToSpirv(VOP3A(OpcodeVOP3::V_AND_OR_B32, VOperand8::V0, SOperand9::V0, SOperand9::V1, SOperand9::V2).SetNeg({1,0,0}).Get());
    auto result = runner->run<u32>(spirv, std::array<u32,3>{0x07070707, 0x11111111, 0xF0F0F0F0});

    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(*result, 0xF1F1F1F1);
}

TEST_F(GcnTest, and_or_b32_5) {
    auto runner = gcn_test::Runner::instance().value();

    auto spirv = TranslateToSpirv(VOP3A(OpcodeVOP3::V_AND_OR_B32, VOperand8::V0, SOperand9::V0, SOperand9::V1, SOperand9::V2).SetNeg({1,0,0}).SetAbs({1,0,0}).Get());
    auto result = runner->run<u32>(spirv, std::array<u32,3>{0x77777777, 0xB0B0B0B0, 0x11111111});

    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(*result, 0xB1313131);
}

TEST_F(GcnTest, and_or_b32_6) {
    auto runner = gcn_test::Runner::instance().value();

    auto spirv = TranslateToSpirv(VOP3A(OpcodeVOP3::V_AND_OR_B32, VOperand8::V0, SOperand9::V0, SOperand9::V1, SOperand9::V2).SetOmod(Omod::Mul2).Get());
    auto result = runner->run<u32>(spirv, std::array<u32,3>{0x40404040, 0xB0B0B0B0, 0x11111111});

    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(*result, 0x11111111);
}

TEST_F(GcnTest, and_or_b32_7) {
    auto runner = gcn_test::Runner::instance().value();

    auto spirv = TranslateToSpirv(VOP3A(OpcodeVOP3::V_AND_OR_B32, VOperand8::V0, SOperand9::V0, SOperand9::V1, SOperand9::V2).SetOmod(Omod::Div2).Get());
    auto result = runner->run<u32>(spirv, std::array<u32,3>{0xB0B0B0B0, 0x77777777, 0x40404040});

    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(*result, 0x70707070);
}

TEST_F(GcnTest, and_or_b32_8) {
    auto runner = gcn_test::Runner::instance().value();

    auto spirv = TranslateToSpirv(VOP3A(OpcodeVOP3::V_AND_OR_B32, VOperand8::V0, SOperand9::V0, SOperand9::V1, SOperand9::V2).SetAbs({1,1,0}).Get());
    auto result = runner->run<u32>(spirv, std::array<u32,3>{0xB0B0B0B0, 0x11111111, 0x11111111});

    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(*result, 0x11111111);
}

TEST_F(GcnTest, mad_mix_f32_1) {
    auto runner = gcn_test::Runner::instance().value();

    auto inst = VOP3P(OpcodeVOP3P::V_MAD_MIX_F32, VOperand8::V0, SOperand9::V0, SOperand9::V1, SOperand9::V2).SetOpSelHi({0}).Get();
    auto spirv = TranslateToSpirv(inst);
    auto result = runner->run<float>(spirv, std::array{2.0f, 3.0f, 4.0f});

    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(*result, 10.0f);
}

TEST_F(GcnTest, mad_mix_f32_2) {
    auto runner = gcn_test::Runner::instance().value();

    auto inst = VOP3P(OpcodeVOP3P::V_MAD_MIX_F32, VOperand8::V0, SOperand9::V0, SOperand9::V1, SOperand9::V2).SetOpSelHi({1,1,0}).SetOpSel({1,0,0}).Get();
    auto spirv = TranslateToSpirv(inst);
    auto result = runner->run<float>(spirv, std::array<u32,3>{
        std::bit_cast<u32>(F16x2{half(44.0f), half(0.5f)}), std::bit_cast<u32>(F16x2{half(44.0f), half(0.5f)}), std::bit_cast<u32>(4.0f)}
    );

    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(*result, 26.0f);
}

TEST_F(GcnTest, mad_mixlo_f16_1) {
    auto runner = gcn_test::Runner::instance().value();

    auto inst = VOP3P(OpcodeVOP3P::V_MAD_MIXLO_F16, VOperand8::V0, SOperand9::V0, SOperand9::V1, SOperand9::V2).SetOpSelHi({1,1,0}).SetOpSel({1,0,0}).Get();
    auto spirv = TranslateToSpirv(inst);
    auto result = runner->run<F16x2>(spirv, std::array<u32,3>{
        std::bit_cast<u32>(F16x2{half(44.0f), half(0.5f)}), std::bit_cast<u32>(F16x2{half(44.0f), half(0.5f)}), std::bit_cast<u32>(4.0f)}
    );

    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(*result, (F16x2{half(26.0f), half(0.5f)}));
}

TEST_F(GcnTest, mad_mixhi_f16_1) {
    auto runner = gcn_test::Runner::instance().value();

    auto inst = VOP3P(OpcodeVOP3P::V_MAD_MIXHI_F16, VOperand8::V0, SOperand9::V0, SOperand9::V1, SOperand9::V2).SetOpSelHi({1,1,0}).SetOpSel({1,0,0}).Get();
    auto spirv = TranslateToSpirv(inst);
    auto result = runner->run<F16x2>(spirv, std::array<u32,3>{
        std::bit_cast<u32>(F16x2{half(44.0f), half(0.5f)}), std::bit_cast<u32>(F16x2{half(44.0f), half(0.5f)}), std::bit_cast<u32>(4.0f)}
    );

    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(*result, (F16x2{half(44.0f), half(26.0f)}));
}

TEST_F(GcnTest, lshrrev_b16_1) {
    auto runner = gcn_test::Runner::instance().value();

    auto spirv = TranslateToSpirv(VOP3A(OpcodeVOP3::V_LSHRREV_B16, VOperand8::V0, SOperand9::V0, SOperand9::V1).Get());
    auto result = runner->run<u32>(spirv, std::array<u32,2>{0xFFFFFFF2, 0x88881000});

    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(*result, 0xFFFF0400);
}

TEST_F(GcnTest, lshrrev_b16_2) {
    auto runner = gcn_test::Runner::instance().value();

    auto spirv = TranslateToSpirv(VOP3A(OpcodeVOP3::V_LSHRREV_B16, VOperand8::V0, SOperand9::V0, SOperand9::V1).SetOpSel({0,0,0,1}).Get());
    auto result = runner->run<u32>(spirv, std::array<u32,2>{0xFFFFFFF2, 0x88881000});

    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(*result, 0x0400FFF2);
}

TEST_F(GcnTest, lshrrev_b16_3) {
    auto runner = gcn_test::Runner::instance().value();

    auto spirv = TranslateToSpirv(VOP3A(OpcodeVOP3::V_LSHRREV_B16, VOperand8::V0, SOperand9::V0, SOperand9::V1).SetOpSel({0,1,0,0}).Get());
    auto result = runner->run<u32>(spirv, std::array<u32,2>{0xFFFFFFF2, 0x88881000});

    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(*result, 0xFFFF2222);
}

TEST_F(GcnTest, lshlrev_b16_1) {
    auto runner = gcn_test::Runner::instance().value();

    auto spirv = TranslateToSpirv(VOP3A(OpcodeVOP3::V_LSHLREV_B16, VOperand8::V0, SOperand9::V0, SOperand9::V1).Get());
    auto result = runner->run<u32>(spirv, std::array<u32,2>{0xFFFFFFF3, 0x88888888});

    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(*result, 0xFFFF4440);
}

TEST_F(GcnTest, ashrrev_i16_1) {
    auto runner = gcn_test::Runner::instance().value();

    auto spirv = TranslateToSpirv(VOP3A(OpcodeVOP3::V_ASHRREV_I16, VOperand8::V0, SOperand9::V0, SOperand9::V1).Get());
    auto result = runner->run<u32>(spirv, std::array<u32,2>{0x1234FFF3, 0x88888888});

    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(*result, 0x1234F111);
}

TEST_F(GcnTest, pk_add_f16_1) {
    auto runner = gcn_test::Runner::instance().value();

    auto spirv = TranslateToSpirv(VOP3P(OpcodeVOP3P::V_PK_ADD_F16, VOperand8::V0, SOperand9::V0, SOperand9::V1).Get());
    auto result = runner->run<F16x2>(spirv, std::array{F16x2{half(1.0f), half(2.0f)}, F16x2{half(3.0f), half(4.0f)}});

    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(*result, (F16x2{half(4.0f), half(6.0f)}));
}

TEST_F(GcnTest, pk_add_f16_2) {
    auto runner = gcn_test::Runner::instance().value();

    auto inst = VOP3P(OpcodeVOP3P::V_PK_ADD_F16, VOperand8::V0, SOperand9::Const0, SOperand9::ConstInv2Pi).Get();
    auto spirv = TranslateToSpirv(inst);
    auto result = runner->run<u32>(spirv, 0U);

    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(*result, 0x00003118);
}

TEST_F(GcnTest, pk_add_f16_3) {
    auto runner = gcn_test::Runner::instance().value();

    auto inst = VOP3P(OpcodeVOP3P::V_PK_ADD_F16, VOperand8::V0, SOperand9::Const0, SOperand9::ConstInv2Pi).SetOpSel({0,1,1}).Get();
    auto spirv = TranslateToSpirv(inst);
    auto result = runner->run<u32>(spirv, 0U);

    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(*result, 0);
}

TEST_F(GcnTest, pk_add_f16_4) {
    auto runner = gcn_test::Runner::instance().value();

    auto inst = VOP3P(OpcodeVOP3P::V_PK_ADD_F16, VOperand8::V0, SOperand9::Const0p5, SOperand9::Const0p5).Get();
    auto spirv = TranslateToSpirv(inst);
    auto result = runner->run<u32>(spirv, 0U);

    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(*result, 0x3C00);
}

TEST_F(GcnTest, pk_add_f16_5) {
    auto runner = gcn_test::Runner::instance().value();

    auto inst = VOP3P(OpcodeVOP3P::V_PK_ADD_F16, VOperand8::V0, SOperand9::Const0, SOperand9::ConstInv2Pi).SetOpSelHi({0,0,0}).Get();
    auto spirv = TranslateToSpirv(inst);
    auto result = runner->run<u32>(spirv, 0U);

    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(*result, 0x31183118);
}

TEST_F(GcnTest, pk_add_f16_neg_lo) {
    auto runner = gcn_test::Runner::instance().value();

    auto spirv = TranslateToSpirv(VOP3P(OpcodeVOP3P::V_PK_ADD_F16, VOperand8::V0, SOperand9::V0, SOperand9::V1).SetNeg({1,1,0}).Get());
    auto result = runner->run<F16x2>(spirv, std::array{F16x2{half(1.0f), half(2.0f)}, F16x2{half(3.0f), half(4.0f)}});

    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(*result, (F16x2{half(-4.0f), half(6.0f)}));
}

TEST_F(GcnTest, pk_add_f16_neg_hi) {
    auto runner = gcn_test::Runner::instance().value();

    auto spirv = TranslateToSpirv(VOP3P(OpcodeVOP3P::V_PK_ADD_F16, VOperand8::V0, SOperand9::V0, SOperand9::V1).SetNegHi({1,1,0}).Get());
    auto result = runner->run<F16x2>(spirv, std::array{F16x2{half(1.0f), half(2.0f)}, F16x2{half(3.0f), half(4.0f)}});

    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(*result, (F16x2{half(4.0f), half(-6.0f)}));
}

TEST_F(GcnTest, pk_add_f16_op_sel_reversed) {
    auto runner = gcn_test::Runner::instance().value();

    auto spirv = TranslateToSpirv(VOP3P(OpcodeVOP3P::V_PK_ADD_F16, VOperand8::V0, SOperand9::V0, SOperand9::V1).SetOpSel({1,1,1}).SetOpSelHi({0,0,0}).Get());
    auto result = runner->run<F16x2>(spirv, std::array{F16x2{half(1.0f), half(2.0f)}, F16x2{half(3.0f), half(4.0f)}});

    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(*result, (F16x2{half(6.0f), half(4.0f)}));
}

TEST_F(GcnTest, addk_i32_overflow_scc) {
    auto runner = gcn_test::Runner::instance().value();
    const std::array<u64, 3> instructions{
        SOPK(OpcodeSOPK::S_ADDK_I32, SOperand7::S0, 0xffff).Get(),
        SOP2(OpcodeSOP2::S_CSELECT_B32, SOperand7::S0, SOperand8::Const1, SOperand8::Const0).Get(),
        VOP1(OpcodeVOP1::V_MOV_B32, VOperand8::V0, SOperand9::S0).Get(),
    };
    const auto spirv = TranslateToSpirv(instructions);
    auto overflow = runner->run<u32>(spirv, std::array{0x80000000U, 0U, 0U, 0U});
    ASSERT_TRUE(overflow.has_value());
    EXPECT_EQ(*overflow, 1U);

    auto no_overflow = runner->run<u32>(spirv, std::array{5U, 0U, 0U, 0U});
    ASSERT_TRUE(no_overflow.has_value());
    EXPECT_EQ(*no_overflow, 0U);
}

TEST_F(GcnTest, bitcmp1_b64_bit32) {
    auto runner = gcn_test::Runner::instance().value();
    const std::array<u64, 3> instructions{
        SOPC(OpcodeSOPC::S_BITCMP1_B64, SOperand8::S0, SOperand8::S2).Get(),
        SOP2(OpcodeSOP2::S_CSELECT_B32, SOperand7::S0, SOperand8::Const1, SOperand8::Const0).Get(),
        VOP1(OpcodeVOP1::V_MOV_B32, VOperand8::V0, SOperand9::S0).Get(),
    };
    const auto spirv = TranslateToSpirv(instructions);

    auto result = runner->run<u32>(spirv, std::array{0U, 1U, 32U, 0U});
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 1U);
    for (const u32 bit : {0U, 31U, 32U, 63U}) {
        const u64 value = u64{1} << bit;
        for (const u32 position : {bit, bit + 64}) {
            auto set = runner->run<u32>(spirv, std::array{u32(value), u32(value >> 32), position, 0U});
            ASSERT_TRUE(set.has_value());
            EXPECT_EQ(*set, 1U);
            auto clear = runner->run<u32>(spirv, std::array{0U, 0U, position, 0U});
            ASSERT_TRUE(clear.has_value());
            EXPECT_EQ(*clear, 0U);
        }
    }
}

TEST_F(GcnTest, subb_u32_clears_vcc) {
    auto runner = gcn_test::Runner::instance().value();
    const std::array<u64, 3> instructions{
        VOP2(OpcodeVOP2::V_SUB_I32, VOperand8::V1, SOperand9::S0, VOperand8::V1).Get(),
        VOP2(OpcodeVOP2::V_SUBB_U32, VOperand8::V1, SOperand9::S2, VOperand8::V3).Get(),
        VOP2(OpcodeVOP2::V_ADDC_U32, VOperand8::V0, SOperand9::Const0, VOperand8::V3).Get(),
    };
    const auto spirv = TranslateToSpirv(instructions);

    auto result = runner->run<u32>(spirv, std::array{0U, 1U, 5U, 0U});
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 0U);
}

TEST_F(GcnTest, subb_u32_scc_wrap) {
    auto runner = gcn_test::Runner::instance().value();
    const std::array<u64, 4> instructions{
        SOP2(OpcodeSOP2::S_ADD_U32, SOperand7::S3, SOperand8::S0, SOperand8::S1).Get(),
        SOP2(OpcodeSOP2::S_SUBB_U32, SOperand7::S3, SOperand8::Const0, SOperand8::S2).Get(),
        SOP2(OpcodeSOP2::S_CSELECT_B32, SOperand7::S0, SOperand8::Const1, SOperand8::Const0).Get(),
        VOP1(OpcodeVOP1::V_MOV_B32, VOperand8::V0, SOperand9::S0).Get(),
    };
    const auto spirv = TranslateToSpirv(instructions);

    auto result = runner->run<u32>(spirv, std::array{0xffffffffU, 1U, 0xffffffffU, 0U});
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 1U);
}

TEST_F(GcnTest, addc_u32_clears_scc) {
    auto runner = gcn_test::Runner::instance().value();
    const std::array<u64, 4> instructions{
        SOP2(OpcodeSOP2::S_ADD_U32, SOperand7::S3, SOperand8::S0, SOperand8::S1).Get(),
        SOP2(OpcodeSOP2::S_ADDC_U32, SOperand7::S3, SOperand8::Const0, SOperand8::Const0).Get(),
        SOP2(OpcodeSOP2::S_CSELECT_B32, SOperand7::S0, SOperand8::Const1, SOperand8::Const0).Get(),
        VOP1(OpcodeVOP1::V_MOV_B32, VOperand8::V0, SOperand9::S0).Get(),
    };
    const auto spirv = TranslateToSpirv(instructions);

    auto result = runner->run<u32>(spirv, std::array{0xffffffffU, 1U, 0U, 0U});
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 0U);
}

TEST_F(GcnTest, addc_u32_result_uses_scc) {
    auto runner = gcn_test::Runner::instance().value();
    const std::array<u64, 3> instructions{
        SOP2(OpcodeSOP2::S_ADD_U32, SOperand7::S1, SOperand8::S0, SOperand8::S1).Get(),
        SOP2(OpcodeSOP2::S_ADDC_U32, SOperand7::S0, SOperand8::S2, SOperand8::S3).Get(),
        VOP1(OpcodeVOP1::V_MOV_B32, VOperand8::V0, SOperand9::S0).Get(),
    };
    const auto spirv = TranslateToSpirv(instructions);

    auto overflow = runner->run<u32>(spirv, std::array{0xffffffffU, 1U, 7U, 0U});
    ASSERT_TRUE(overflow.has_value());
    EXPECT_EQ(*overflow, 8U);

    auto no_overflow = runner->run<u32>(spirv, std::array{2U, 1U, 7U, 0U});
    ASSERT_TRUE(no_overflow.has_value());
    EXPECT_EQ(*no_overflow, 7U);
}

TEST_F(GcnTest, group_minimum_partial_subgroup) {
    auto runner = gcn_test::Runner::instance().value();
    const auto single = TranslateGroupMinimumToSpirv(1);
    auto one = runner->run<u32>(single, std::array{7u, 0u, 0u, 0u});
    ASSERT_TRUE(one.has_value());
    EXPECT_EQ(*one, 71u);
    const auto partial = TranslateGroupMinimumToSpirv(3);
    auto three = runner->run<std::array<u32, 3>>(partial, std::array{7u, 0u, 0u, 0u});
    ASSERT_TRUE(three.has_value());
    EXPECT_EQ(*three, (std::array{69u, 69u, 69u}));
}

TEST_F(GcnTest, cvt_pk_u8_console_rounding_saturation_and_selector) {
    auto runner = gcn_test::Runner::instance().value();
    const auto spirv = TranslateToSpirv(
        VOP3A(OpcodeVOP3::V_CVT_PK_U8_F32, VOperand8::V0,
              SOperand9::V0, SOperand9::V1, SOperand9::V2).Get());
    struct Case { float value; u32 selector; u32 expected; };
    const Case cases[] = {
        {0.5f, 0, 0x11223300}, {1.5f, 0, 0x11223302},
        {2.5f, 0, 0x11223302}, {127.5f, 0, 0x11223380},
        {255.5f, 0, 0x112233ff}, {300.0f, 0, 0x112233ff},
        {-1.0f, 0, 0x11223300},
        {std::numeric_limits<float>::infinity(), 0, 0x112233ff},
        {-std::numeric_limits<float>::infinity(), 0, 0x11223300},
        {std::numeric_limits<float>::quiet_NaN(), 0, 0x11223300},
        {171.0f, 0, 0x112233ab}, {171.0f, 1, 0x1122ab44},
        {171.0f, 2, 0x11ab3344}, {171.0f, 3, 0xab223344},
        {171.0f, 4, 0x112233ab}, {171.0f, 5, 0x1122ab44},
        {171.0f, 7, 0xab223344}, {171.0f, 0xffffffff, 0xab223344},
    };
    for (const auto& item : cases) {
        SCOPED_TRACE(item.selector);
        const auto result = runner->run<u32>(spirv,
            std::array{std::bit_cast<u32>(item.value), item.selector, u32{0x11223344}});
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(*result, item.expected);
    }
}
