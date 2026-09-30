// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <gtest/gtest.h>
#include "shader_recompiler/frontend/wave_minimum.h"

namespace {
using namespace Shader::Gcn;
GcnInst Inst(Opcode opcode, OperandField destination, u32 dst, OperandField source, u32 src) {
    GcnInst inst{};
    inst.opcode = opcode;
    inst.length = 4;
    inst.dst_count = 1;
    inst.dst[0].field = destination;
    inst.dst[0].code = dst;
    inst.src_count = 1;
    inst.src[0].field = source;
    inst.src[0].code = src;
    return inst;
}
std::vector<GcnInst> EntryMinimum() {
    std::vector<GcnInst> code{
        Inst(Opcode::S_MOV_B64, OperandField::ScalarGPR, 10, OperandField::ExecLo, 126),
        Inst(Opcode::S_WQM_B64, OperandField::ExecLo, 126, OperandField::ExecLo, 126),
        Inst(Opcode::S_AND_SAVEEXEC_B64, OperandField::ScalarGPR, 12, OperandField::ScalarGPR, 10),
        Inst(Opcode::S_ORN2_SAVEEXEC_B64, OperandField::VccLo, 106, OperandField::ExecLo, 126),
        Inst(Opcode::V_CNDMASK_B32, OperandField::VectorGPR, 31, OperandField::SignedConstIntNeg,
             193),
    };
    code.back().encoding = InstEncoding::VOP2;
    code.back().src[1].field = OperandField::VectorGPR;
    code.back().src[1].code = 29;
    for (u32 step = 0; step < 5; ++step) {
        auto shuffle =
            Inst(Opcode::DS_SWIZZLE_B32, OperandField::VectorGPR, 30, OperandField::VectorGPR, 31);
        shuffle.control.ds.offset0 = 31;
        shuffle.control.ds.offset1 = (16 >> step) * 4;
        code.push_back(shuffle);
        code.push_back(
            Inst(Opcode::S_WAITCNT, OperandField::Undefined, 0, OperandField::Undefined, 0));
        auto minimum =
            Inst(Opcode::V_MIN_U32, OperandField::VectorGPR, 31, OperandField::VectorGPR, 31);
        minimum.encoding = InstEncoding::VOP2;
        minimum.src[1].field = OperandField::VectorGPR;
        minimum.src[1].code = 30;
        code.push_back(minimum);
    }
    for (u32 half = 0; half < 2; ++half) {
        auto read = Inst(Opcode::V_READLANE_B32, OperandField::ScalarGPR, 14 + half,
                         OperandField::VectorGPR, 31);
        read.encoding = InstEncoding::VOP2;
        read.src[1].field = OperandField::SignedConstIntPos;
        read.src[1].code = half ? 191 : 159;
        code.push_back(read);
    }
    auto minimum =
        Inst(Opcode::S_MIN_U32, OperandField::ScalarGPR, 16, OperandField::ScalarGPR, 14);
    minimum.src[1].field = OperandField::ScalarGPR;
    minimum.src[1].code = 15;
    code.push_back(minimum);
    code.push_back(
        Inst(Opcode::S_CMPK_EQ_U32, OperandField::ScalarGPR, 16, OperandField::Undefined, 0));
    return code;
}
} // namespace

TEST(WaveMinimum, RecognizesEntryExecReductionAndTracksByteLength) {
    auto code = EntryMinimum();
    code[5].length = 8;
    const auto match = FindEntryWaveMinimum(code);
    ASSERT_TRUE(match);
    EXPECT_EQ(match->input_vgpr, 29u);
    EXPECT_EQ(match->end_pc, 96u);
}
TEST(WaveMinimum, RejectsMissingSentinelOrDifferentReduction) {
    auto code = EntryMinimum();
    code[4].src[0].code = 128;
    EXPECT_FALSE(FindEntryWaveMinimum(code));
    code = EntryMinimum();
    code[5].control.ds.offset1 = 32;
    EXPECT_FALSE(FindEntryWaveMinimum(code));
    code = EntryMinimum();
    code[21].src[1].code = 159;
    EXPECT_FALSE(FindEntryWaveMinimum(code));
}
TEST(WaveMinimum, RejectsChangedEntryMaskAndClobberedInput) {
    auto code = EntryMinimum();
    code.insert(code.begin() + 2,
                Inst(Opcode::S_MOV_B32, OperandField::ScalarGPR, 10, OperandField::ScalarGPR, 0));
    EXPECT_FALSE(FindEntryWaveMinimum(code));
    code = EntryMinimum();
    code[4].src[1].code = 31;
    EXPECT_FALSE(FindEntryWaveMinimum(code));
}
TEST(WaveMinimum, RejectsLiveSccAndInteriorBranchTarget) {
    auto code = EntryMinimum();
    code.back().opcode = Opcode::S_CSELECT_B32;
    EXPECT_FALSE(FindEntryWaveMinimum(code));
    code = EntryMinimum();
    auto branch = Inst(Opcode::S_BRANCH, OperandField::Undefined, 0, OperandField::Undefined, 0);
    branch.control.sopp.simm = -20;
    code.push_back(branch);
    EXPECT_FALSE(FindEntryWaveMinimum(code));
}
TEST(WaveMinimum, RejectsTruncatedSequence) {
    auto code = EntryMinimum();
    code.resize(22);
    EXPECT_FALSE(FindEntryWaveMinimum(code));
}

TEST(WaveMinimum, RejectsMaskSavedAfterEarlierExecChange) {
    auto code = EntryMinimum();
    code.insert(code.begin(), Inst(Opcode::S_AND_SAVEEXEC_B64, OperandField::ScalarGPR, 20,
                                   OperandField::ScalarGPR, 22));
    EXPECT_FALSE(FindEntryWaveMinimum(code));
}

TEST(WaveMinimum, RejectsModifiedVectorOperands) {
    auto code = EntryMinimum();
    code[4].src[1].input_modifier.neg = true;
    EXPECT_FALSE(FindEntryWaveMinimum(code));
    code = EntryMinimum();
    code[7].dst[0].output_modifier.clamp = true;
    EXPECT_FALSE(FindEntryWaveMinimum(code));
}

TEST(WaveMinimum, ScalarMovesOnlyClobberTheirActualRegisters) {
    auto code = EntryMinimum();
    code.insert(code.begin() + 2,
                Inst(Opcode::S_MOV_B32, OperandField::ScalarGPR, 9, OperandField::ScalarGPR, 0));
    EXPECT_TRUE(FindEntryWaveMinimum(code));
    code[2].opcode = Opcode::S_MOV_B64;
    EXPECT_FALSE(FindEntryWaveMinimum(code));
    code[2].dst[0].code = 8;
    EXPECT_TRUE(FindEntryWaveMinimum(code));
    code[2].dst[0].code = 11;
    EXPECT_FALSE(FindEntryWaveMinimum(code));
}

TEST(WaveMinimum, RecognizesMultipleReductionsUsingUnmodifiedSavedMask) {
    auto code = EntryMinimum();
    code.push_back(Inst(Opcode::S_MOV_B64, OperandField::ExecLo, 126,
                        OperandField::ScalarGPR, 12));
    const auto second = EntryMinimum();
    code.insert(code.end(), second.begin() + 2, second.end());
    const auto matches = FindEntryWaveMinima(code);
    ASSERT_EQ(matches.size(), 2u);
    EXPECT_LT(matches[0].end_pc, matches[1].end_pc);
    code.insert(code.begin() + 25,
                Inst(Opcode::S_MOV_B32, OperandField::ScalarGPR, 10,
                     OperandField::ScalarGPR, 0));
    EXPECT_EQ(FindEntryWaveMinima(code).size(), 1u);
}

TEST(WaveMinimum, ScalarLoadsInvalidateOnlyOverlappingRegisterSpans) {
    auto code = EntryMinimum();
    code.insert(code.begin() + 2,
                Inst(Opcode::S_LOAD_DWORDX4, OperandField::ScalarGPR, 6,
                     OperandField::ScalarGPR, 0));
    EXPECT_TRUE(FindEntryWaveMinimum(code));
    code[2].dst[0].code = 7;
    EXPECT_FALSE(FindEntryWaveMinimum(code));
    code[2].dst[0].code = 11;
    EXPECT_FALSE(FindEntryWaveMinimum(code));
    code[2].dst[0].code = 12;
    EXPECT_TRUE(FindEntryWaveMinimum(code));
}
