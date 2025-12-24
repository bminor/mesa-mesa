/*
 * Copyright 2015 Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: MIT
 */

#include "ac_perfcounter.h"
#include "ac_spm.h"

/* CB */
static unsigned gfx12_CB_select0[] = {
   R_037004_CB_PERFCOUNTER0_SELECT,
   R_03700C_CB_PERFCOUNTER1_SELECT,
   R_037010_CB_PERFCOUNTER2_SELECT,
   R_037014_CB_PERFCOUNTER3_SELECT,
};
static unsigned gfx12_CB_select1[] = {
   R_037008_CB_PERFCOUNTER0_SELECT1,
};
static struct ac_pc_block_base gfx12_CB = {
   .gpu_block = CB,
   .name = "CB",
   .distribution = AC_PC_PER_SHADER_ARRAY,
   .num_counters = 4,
   .flags = AC_PC_BLOCK_SE | AC_PC_BLOCK_INSTANCE_GROUPS,

   .select0 = gfx12_CB_select0,
   .select1 = gfx12_CB_select1,
   .counter0_lo = R_035018_CB_PERFCOUNTER0_LO,

   .num_spm_counters = 1,
   .num_spm_wires = 2,
   .spm_block_select = AC_SPM_SE_BLOCK_CB,
};

/* CPC */
static unsigned gfx12_CPC_select0[] = {
   R_036024_CPC_PERFCOUNTER0_SELECT,
   R_03600C_CPC_PERFCOUNTER1_SELECT,
};
static unsigned gfx12_CPC_select1[] = {
   R_036010_CPC_PERFCOUNTER0_SELECT1,
};
static unsigned gfx12_CPC_counters[] = {
   R_034018_CPC_PERFCOUNTER0_LO,
   R_034010_CPC_PERFCOUNTER1_LO,
};
static struct ac_pc_block_base gfx12_CPC = {
   .gpu_block = CPC,
   .name = "CPC",
   .distribution = AC_PC_GLOBAL_BLOCK,
   .num_counters = 2,

   .select0 = gfx12_CPC_select0,
   .select1 = gfx12_CPC_select1,
   .counters = gfx12_CPC_counters,

   .num_spm_counters = 1,
   .num_spm_wires = 2,
   .spm_block_select = AC_SPM_GLOBAL_BLOCK_CPC,
};

/* CPF */
static unsigned gfx12_CPF_select0[] = {
   R_03601C_CPF_PERFCOUNTER0_SELECT,
   R_036014_CPF_PERFCOUNTER1_SELECT,
};
static unsigned gfx12_CPF_select1[] = {
   R_036018_CPF_PERFCOUNTER0_SELECT1,
};
static unsigned gfx12_CPF_counters[] = {
   R_034028_CPF_PERFCOUNTER0_LO,
   R_034020_CPF_PERFCOUNTER1_LO,
};
static struct ac_pc_block_base gfx12_CPF = {
   .gpu_block = CPF,
   .name = "CPF",
   .distribution = AC_PC_GLOBAL_BLOCK,
   .num_counters = 2,

   .select0 = gfx12_CPF_select0,
   .select1 = gfx12_CPF_select1,
   .counters = gfx12_CPF_counters,

   .num_spm_counters = 1,
   .num_spm_wires = 2,
   .spm_block_select = AC_SPM_GLOBAL_BLOCK_CPF,
};

/* CPG */
static unsigned gfx12_CPG_select0[] = {
   R_036008_CPG_PERFCOUNTER0_SELECT,
   R_036000_CPG_PERFCOUNTER1_SELECT,
};
static unsigned gfx12_CPG_select1[] = {
   R_036004_CPG_PERFCOUNTER0_SELECT1
};
static unsigned gfx12_CPG_counters[] = {
   R_034008_CPG_PERFCOUNTER0_LO,
   R_034000_CPG_PERFCOUNTER1_LO,
};
static struct ac_pc_block_base gfx12_CPG = {
   .gpu_block = CPG,
   .name = "CPG",
   .distribution = AC_PC_GLOBAL_BLOCK,
   .num_counters = 2,

   .select0 = gfx12_CPG_select0,
   .select1 = gfx12_CPG_select1,
   .counters = gfx12_CPG_counters,

   .num_spm_counters = 1,
   .num_spm_wires = 2,
   .spm_block_select = AC_SPM_GLOBAL_BLOCK_CPG,
};

/* GRBM */
static unsigned gfx12_GRBM_select0[] = {
   R_036100_GRBM_PERFCOUNTER0_SELECT,
   R_036104_GRBM_PERFCOUNTER1_SELECT,
};
static unsigned gfx12_GRBM_counters[] = {
   R_034100_GRBM_PERFCOUNTER0_LO,
   R_03410C_GRBM_PERFCOUNTER1_LO,
};
static struct ac_pc_block_base gfx12_GRBM = {
   .gpu_block = GRBM,
   .name = "GRBM",
   .distribution = AC_PC_GLOBAL_BLOCK,
   .num_counters = 2,

   .select0 = gfx12_GRBM_select0,
   .counters = gfx12_GRBM_counters,
};

/* PA_SC */
static unsigned gfx12_PA_SC_select0[] = {
   R_036500_PA_SC_PERFCOUNTER0_SELECT,
   R_036508_PA_SC_PERFCOUNTER1_SELECT,
   R_03650C_PA_SC_PERFCOUNTER2_SELECT,
   R_036510_PA_SC_PERFCOUNTER3_SELECT,
   R_036514_PA_SC_PERFCOUNTER4_SELECT,
   R_036518_PA_SC_PERFCOUNTER5_SELECT,
   R_03651C_PA_SC_PERFCOUNTER6_SELECT,
   R_036520_PA_SC_PERFCOUNTER7_SELECT,
};
static unsigned gfx12_PA_SC_select1[] = {
   R_036504_PA_SC_PERFCOUNTER0_SELECT1,
};
static struct ac_pc_block_base gfx12_PA_SC = {
   .gpu_block = PA_SC,
   .name = "PA_SC",
   .num_counters = 8,
   .distribution = AC_PC_PER_SHADER_ARRAY,
   .flags = AC_PC_BLOCK_SE,

   .select0 = gfx12_PA_SC_select0,
   .select1 = gfx12_PA_SC_select1,
   .counter0_lo = R_034500_PA_SC_PERFCOUNTER0_LO,

   .num_spm_counters = 1,
   .num_spm_wires = 2,
   .spm_block_select = AC_SPM_SE_BLOCK_SC,
};

/* SPI */
static unsigned gfx12_SPI_select0[] = {
   R_036600_SPI_PERFCOUNTER0_SELECT,
   R_036604_SPI_PERFCOUNTER1_SELECT,
   R_036608_SPI_PERFCOUNTER2_SELECT,
   R_03660C_SPI_PERFCOUNTER3_SELECT,
   R_036610_SPI_PERFCOUNTER4_SELECT,
   R_036614_SPI_PERFCOUNTER5_SELECT,
};
static unsigned gfx12_SPI_select1[] = {
   R_036618_SPI_PERFCOUNTER0_SELECT1,
   R_03661C_SPI_PERFCOUNTER1_SELECT1,
   R_036620_SPI_PERFCOUNTER2_SELECT1,
   R_036624_SPI_PERFCOUNTER3_SELECT1,
   R_036628_SPI_PERFCOUNTER4_SELECT1,
   R_03662C_SPI_PERFCOUNTER5_SELECT1
};
static struct ac_pc_block_base gfx12_SPI = {
   .gpu_block = SPI,
   .name = "SPI",
   .distribution = AC_PC_PER_SHADER_ENGINE,
   .num_counters = 6,
   .flags = AC_PC_BLOCK_SE,

   .select0 = gfx12_SPI_select0,
   .select1 = gfx12_SPI_select1,
   .counter0_lo = R_034604_SPI_PERFCOUNTER0_LO,

   .num_spm_counters = 6,
   .num_spm_wires = 12,
   .spm_block_select = AC_SPM_SE_BLOCK_SPI,
};

/* SX */
static unsigned gfx12_SX_select0[] = {
   R_036900_SX_PERFCOUNTER0_SELECT,
   R_036904_SX_PERFCOUNTER1_SELECT,
   R_036908_SX_PERFCOUNTER2_SELECT,
   R_03690C_SX_PERFCOUNTER3_SELECT,
};
static unsigned gfx12_SX_select1[] = {
   R_036910_SX_PERFCOUNTER0_SELECT1,
   R_036914_SX_PERFCOUNTER1_SELECT1,
   R_036918_SX_PERFCOUNTER2_SELECT1,
   R_03691C_SX_PERFCOUNTER3_SELECT1,
};
static struct ac_pc_block_base gfx12_SX = {
   .gpu_block = SX,
   .name = "SX",
   .distribution = AC_PC_PER_SHADER_ARRAY,
   .num_counters = 4,
   .flags = AC_PC_BLOCK_SE,

   .select0 = gfx12_SX_select0,
   .select1 = gfx12_SX_select1,
   .counter0_lo = R_034900_SX_PERFCOUNTER0_LO,

   .num_spm_counters = 4,
   .num_spm_wires = 8,
   .spm_block_select = AC_SPM_SE_BLOCK_SX,
};

/* TA */
static unsigned gfx12_TA_select0[] = {
   R_036B00_TA_PERFCOUNTER0_SELECT,
   R_036B08_TA_PERFCOUNTER1_SELECT,
};
static unsigned gfx12_TA_select1[] = {
   R_036B04_TA_PERFCOUNTER0_SELECT1,
};
static struct ac_pc_block_base gfx12_TA = {
   .gpu_block = TA,
   .name = "TA",
   .distribution = AC_PC_PER_SHADER_ARRAY,
   .num_counters = 2,
   .flags = AC_PC_BLOCK_SE | AC_PC_BLOCK_INSTANCE_GROUPS | AC_PC_BLOCK_SHADER_WINDOWED,

   .select0 = gfx12_TA_select0,
   .select1 = gfx12_TA_select1,
   .counter0_lo = R_034B00_TA_PERFCOUNTER0_LO,

   .num_spm_counters = 1,
   .num_spm_wires = 2,
   .spm_block_select = AC_SPM_SE_BLOCK_TA,
};

/* TD */
static unsigned gfx12_TD_select0[] = {
   R_036C00_TD_PERFCOUNTER0_SELECT,
   R_036C08_TD_PERFCOUNTER1_SELECT,
};
static unsigned gfx12_TD_select1[] = {
   R_036C04_TD_PERFCOUNTER0_SELECT1,
};
static struct ac_pc_block_base gfx12_TD = {
   .gpu_block = TD,
   .name = "TD",
   .distribution = AC_PC_PER_SHADER_ARRAY,
   .num_counters = 2,
   .flags = AC_PC_BLOCK_SE | AC_PC_BLOCK_INSTANCE_GROUPS | AC_PC_BLOCK_SHADER_WINDOWED,

   .select0 = gfx12_TD_select0,
   .select1 = gfx12_TD_select1,
   .counter0_lo = R_034C00_TD_PERFCOUNTER0_LO,

   .num_spm_counters = 1,
   .num_spm_wires = 2,
   .spm_block_select = AC_SPM_SE_BLOCK_TD,
};

/* CHA */
static unsigned gfx12_CHA_select0[] = {
   R_037780_CHA_PERFCOUNTER0_SELECT,
   R_037788_CHA_PERFCOUNTER1_SELECT,
   R_037790_CHA_PERFCOUNTER2_SELECT,
   R_037798_CHA_PERFCOUNTER3_SELECT,
};
static unsigned gfx12_CHA_select1[] = {
   R_037784_CHA_PERFCOUNTER0_SELECT1,
   R_03778C_CHA_PERFCOUNTER1_SELECT1,
   R_037794_CHA_PERFCOUNTER2_SELECT1,
   R_03779C_CHA_PERFCOUNTER3_SELECT1,
};
static struct ac_pc_block_base gfx12_CHA = {
   .gpu_block = CHA,
   .name = "CHA",
   .distribution = AC_PC_GLOBAL_BLOCK,
   .num_counters = 4,

   .select0 = gfx12_CHA_select0,
   .select1 = gfx12_CHA_select1,
   .counter0_lo = R_035800_CHA_PERFCOUNTER0_LO,

   .num_spm_counters = 4,
   .num_spm_wires = 8,
   .spm_block_select = AC_SPM_GLOBAL_BLOCK_CHA,
};

/* CHC */
static unsigned gfx12_CHC_select0[] = {
   R_036F00_CHC_PERFCOUNTER0_SELECT,
   R_036F08_CHC_PERFCOUNTER1_SELECT,
   R_036F10_CHC_PERFCOUNTER2_SELECT,
   R_036F18_CHC_PERFCOUNTER3_SELECT,
};
static unsigned gfx12_CHC_select1[] = {
   R_036F04_CHC_PERFCOUNTER0_SELECT1,
   R_036F0C_CHC_PERFCOUNTER1_SELECT1,
   R_036F14_CHC_PERFCOUNTER2_SELECT1,
   R_036F1C_CHC_PERFCOUNTER3_SELECT1,
};
static struct ac_pc_block_base gfx12_CHC = {
   .gpu_block = CHC,
   .name = "CHC",
   .distribution = AC_PC_GLOBAL_BLOCK,
   .num_counters = 4,

   .select0 = gfx12_CHC_select0,
   .select1 = gfx12_CHC_select1,
   .counter0_lo = R_034F00_CHC_PERFCOUNTER0_LO,

   .num_spm_counters = 4,
   .num_spm_wires = 8,
   .spm_block_select = AC_SPM_GLOBAL_BLOCK_CHC,
};

/* DB */
static unsigned gfx12_DB_select0[] = {
   R_037100_DB_PERFCOUNTER0_SELECT,
   R_037108_DB_PERFCOUNTER1_SELECT,
   R_037110_DB_PERFCOUNTER2_SELECT,
   R_037118_DB_PERFCOUNTER3_SELECT,
};
static unsigned gfx12_DB_select1[] = {
   R_037104_DB_PERFCOUNTER0_SELECT1,
   R_03710C_DB_PERFCOUNTER1_SELECT1,
   R_037114_DB_PERFCOUNTER2_SELECT1,
   R_03711C_DB_PERFCOUNTER3_SELECT1,
};
static struct ac_pc_block_base gfx12_DB = {
   .gpu_block = DB,
   .name = "DB",
   .distribution = AC_PC_PER_SHADER_ARRAY,
   .num_counters = 4,
   .flags = AC_PC_BLOCK_SE | AC_PC_BLOCK_INSTANCE_GROUPS,

   .select0 = gfx12_DB_select0,
   .select1 = gfx12_DB_select1,
   .counter0_lo = R_035100_DB_PERFCOUNTER0_LO,

   .num_spm_counters = 4,
   .num_spm_wires = 8,
   .spm_block_select = AC_SPM_SE_BLOCK_DB,
};

/* GCR */
static unsigned gfx12_GCR_select0[] = {
   R_037580_GCR_PERFCOUNTER0_SELECT,
   R_037588_GCR_PERFCOUNTER1_SELECT,
};
static unsigned gfx12_GCR_select1[] = {
   R_037584_GCR_PERFCOUNTER0_SELECT1,
   R_03758C_GCR_PERFCOUNTER1_SELECT1,
};
static struct ac_pc_block_base gfx12_GCR = {
   .gpu_block = GCR,
   .name = "GCR",
   .distribution = AC_PC_GLOBAL_BLOCK,
   .num_counters = 2,

   .select0 = gfx12_GCR_select0,
   .select1 = gfx12_GCR_select1,
   .counter0_lo = R_035480_GCR_PERFCOUNTER0_LO,

   .num_spm_counters = 2,
   .num_spm_wires = 4,
   .spm_block_select = AC_SPM_GLOBAL_BLOCK_GCR,
};

/* GE */
static unsigned gfx12_GE_select0[] = {
   R_036290_GE1_PERFCOUNTER0_SELECT,
   R_036298_GE1_PERFCOUNTER1_SELECT,
   R_0362A0_GE1_PERFCOUNTER2_SELECT,
   R_0362A8_GE1_PERFCOUNTER3_SELECT,
};
static unsigned gfx12_GE_select1[] = {
   R_036294_GE1_PERFCOUNTER0_SELECT1,
   R_03629C_GE1_PERFCOUNTER1_SELECT1,
   R_0362A4_GE1_PERFCOUNTER2_SELECT1,
   R_0362AC_GE1_PERFCOUNTER3_SELECT1,
};
static struct ac_pc_block_base gfx12_GE = {
   .gpu_block = GE,
   .name = "GE",
   .distribution = AC_PC_GLOBAL_BLOCK,
   .num_counters = 4,

   .select0 = gfx12_GE_select0,
   .select1 = gfx12_GE_select1,
   .counter0_lo = R_034290_GE1_PERFCOUNTER0_LO,

   .num_spm_counters = 4,
   .num_spm_wires = 8,
   .spm_block_select = AC_SPM_GLOBAL_BLOCK_GE,
};

/* GL1A */
static unsigned gfx12_GL1A_select0[] = {
   R_037700_GL1A_PERFCOUNTER0_SELECT,
   R_037708_GL1A_PERFCOUNTER1_SELECT,
   R_037710_GL1A_PERFCOUNTER2_SELECT,
   R_037718_GL1A_PERFCOUNTER3_SELECT,
};
static unsigned gfx12_GL1A_select1[] = {
   R_037704_GL1A_PERFCOUNTER0_SELECT1,
   R_03770C_GL1A_PERFCOUNTER1_SELECT1,
   R_037714_GL1A_PERFCOUNTER2_SELECT1,
   R_03771C_GL1A_PERFCOUNTER3_SELECT1,
};
static struct ac_pc_block_base gfx12_GL1A = {
   .gpu_block = GL1A,
   .name = "GL1A",
   .distribution = AC_PC_PER_SHADER_ARRAY,
   .num_counters = 4,
   .flags = AC_PC_BLOCK_SE | AC_PC_BLOCK_SHADER_WINDOWED,

   .select0 = gfx12_GL1A_select0,
   .select1 = gfx12_GL1A_select1,
   .counter0_lo = R_035700_GL1A_PERFCOUNTER0_LO,

   .num_spm_counters = 4,
   .num_spm_wires = 8,
   .spm_block_select = AC_SPM_SE_BLOCK_GL1A,
};

/* GL1C */
static unsigned gfx12_GL1C_select0[] = {
   R_036E80_GL1C_PERFCOUNTER0_SELECT,
   R_036E88_GL1C_PERFCOUNTER1_SELECT,
   R_036E90_GL1C_PERFCOUNTER2_SELECT,
   R_036E98_GL1C_PERFCOUNTER3_SELECT,
};
static unsigned gfx12_GL1C_select1[] = {
   R_036E84_GL1C_PERFCOUNTER0_SELECT1,
   R_036E8C_GL1C_PERFCOUNTER1_SELECT1,
   R_036E94_GL1C_PERFCOUNTER2_SELECT1,
   R_036E9C_GL1C_PERFCOUNTER3_SELECT1,
};
static struct ac_pc_block_base gfx12_GL1C = {
   .gpu_block = GL1C,
   .name = "GL1C",
   .distribution = AC_PC_PER_SHADER_ARRAY,
   .num_counters = 4,
   .flags = AC_PC_BLOCK_SE | AC_PC_BLOCK_SHADER_WINDOWED,

   .select0 = gfx12_GL1C_select0,
   .select1 = gfx12_GL1C_select1,
   .counter0_lo = R_034E80_GL1C_PERFCOUNTER0_LO,

   .num_spm_counters = 4,
   .num_spm_wires = 8,
   .spm_block_select = AC_SPM_SE_BLOCK_GL1C,
};

/* GL2A */
static unsigned gfx12_GL2A_select0[] = {
   R_036E40_GL2A_PERFCOUNTER0_SELECT,
   R_036E48_GL2A_PERFCOUNTER1_SELECT,
   R_036E50_GL2A_PERFCOUNTER2_SELECT,
   R_036E58_GL2A_PERFCOUNTER3_SELECT,
};
static unsigned gfx12_GL2A_select1[] = {
   R_036E44_GL2A_PERFCOUNTER0_SELECT1,
   R_036E4C_GL2A_PERFCOUNTER1_SELECT1,
   R_036E54_GL2A_PERFCOUNTER2_SELECT1,
   R_036E5C_GL2A_PERFCOUNTER3_SELECT1,
};
static struct ac_pc_block_base gfx12_GL2A = {
   .gpu_block = GL2A,
   .name = "GL2A",
   .distribution = AC_PC_GLOBAL_BLOCK,
   .num_counters = 4,

   .select0 = gfx12_GL2A_select0,
   .select1 = gfx12_GL2A_select1,
   .counter0_lo = R_034E40_GL2A_PERFCOUNTER0_LO,

   .num_spm_counters = 4,
   .num_spm_wires = 8,
   .spm_block_select = AC_SPM_GLOBAL_BLOCK_GL2A,
};

/* GL2C */
static unsigned gfx12_GL2C_select0[] = {
   R_036E00_GL2C_PERFCOUNTER0_SELECT,
   R_036E08_GL2C_PERFCOUNTER1_SELECT,
   R_036E10_GL2C_PERFCOUNTER2_SELECT,
   R_036E18_GL2C_PERFCOUNTER3_SELECT,
};
static unsigned gfx12_GL2C_select1[] = {
   R_036E04_GL2C_PERFCOUNTER0_SELECT1,
   R_036E0C_GL2C_PERFCOUNTER1_SELECT1,
   R_036E14_GL2C_PERFCOUNTER2_SELECT1,
   R_036E1C_GL2C_PERFCOUNTER3_SELECT1,
};
static struct ac_pc_block_base gfx12_GL2C = {
   .gpu_block = GL2C,
   .name = "GL2C",
   .distribution = AC_PC_GLOBAL_BLOCK,
   .num_counters = 4,

   .select0 = gfx12_GL2C_select0,
   .select1 = gfx12_GL2C_select1,
   .counter0_lo = R_034E00_GL2C_PERFCOUNTER0_LO,

   .num_spm_counters = 4,
   .num_spm_wires = 8,
   .spm_block_select = AC_SPM_GLOBAL_BLOCK_GL2C,
};

/* PA_SU */
static unsigned gfx12_PA_SU_select0[] = {
   R_036400_PA_SU_PERFCOUNTER0_SELECT,
   R_036408_PA_SU_PERFCOUNTER1_SELECT,
   R_036410_PA_SU_PERFCOUNTER2_SELECT,
   R_036418_PA_SU_PERFCOUNTER3_SELECT,
};
static unsigned gfx12_PA_SU_select1[] = {
   R_036404_PA_SU_PERFCOUNTER0_SELECT1,
   R_03640C_PA_SU_PERFCOUNTER1_SELECT1,
   R_036414_PA_SU_PERFCOUNTER2_SELECT1,
   R_03641C_PA_SU_PERFCOUNTER3_SELECT1,
};
static struct ac_pc_block_base gfx12_PA_SU = {
   .gpu_block = PA_SU,
   .name = "PA_SU",
   .distribution = AC_PC_PER_SHADER_ENGINE,
   .num_counters = 4,
   .flags = AC_PC_BLOCK_SE,

   .select0 = gfx12_PA_SU_select0,
   .select1 = gfx12_PA_SU_select1,
   .counter0_lo = R_034400_PA_SU_PERFCOUNTER0_LO,

   .num_spm_counters = 4,
   .num_spm_wires = 8,
   .spm_block_select = AC_SPM_SE_BLOCK_PA,
};

/* RLC */
static unsigned gfx12_RLC_select0[] = {
   R_037304_RLC_PERFCOUNTER0_SELECT,
   R_037308_RLC_PERFCOUNTER1_SELECT,
};
static struct ac_pc_block_base gfx12_RLC = {
   .gpu_block = RLC,
   .name = "RLC",
   .distribution = AC_PC_GLOBAL_BLOCK,
   .num_counters = 2,

   .select0 = gfx12_RLC_select0,
   .counter0_lo = R_035200_RLC_PERFCOUNTER0_LO,
   .num_spm_counters = 0,
};

/* SQ */
static unsigned gfx12_SQ_select0[] = {
   R_036700_SQ_PERFCOUNTER0_SELECT,
   R_036704_SQ_PERFCOUNTER1_SELECT,
   R_036708_SQ_PERFCOUNTER2_SELECT,
   R_03670C_SQ_PERFCOUNTER3_SELECT,
   R_036710_SQ_PERFCOUNTER4_SELECT,
   R_036714_SQ_PERFCOUNTER5_SELECT,
   R_036718_SQ_PERFCOUNTER6_SELECT,
   R_03671C_SQ_PERFCOUNTER7_SELECT,
   R_036720_SQ_PERFCOUNTER8_SELECT,
   R_036724_SQ_PERFCOUNTER9_SELECT,
   R_036728_SQ_PERFCOUNTER10_SELECT,
   R_03672C_SQ_PERFCOUNTER11_SELECT,
   R_036730_SQ_PERFCOUNTER12_SELECT,
   R_036734_SQ_PERFCOUNTER13_SELECT,
   R_036738_SQ_PERFCOUNTER14_SELECT,
   R_03673C_SQ_PERFCOUNTER15_SELECT,
};
static struct ac_pc_block_base gfx12_SQ = {
   .gpu_block = SQ,
   .name = "SQ",
   .distribution = AC_PC_PER_SHADER_ENGINE,
   .num_counters = 16,
   .flags = AC_PC_BLOCK_SE | AC_PC_BLOCK_SHADER,

   .select0 = gfx12_SQ_select0,
   .select_or = S_036700_SQC_BANK_MASK(15),
   .counter0_lo = R_034700_SQ_PERFCOUNTER0_LO,

   .num_spm_wires = 16,
   .spm_block_select = AC_SPM_SE_BLOCK_SQG,
};

/* TCP */
static unsigned gfx12_TCP_select0[] = {
   R_036D00_TCP_PERFCOUNTER0_SELECT,
   R_036D08_TCP_PERFCOUNTER1_SELECT,
   R_036D10_TCP_PERFCOUNTER2_SELECT,
   R_036D14_TCP_PERFCOUNTER3_SELECT,
};
static unsigned gfx12_TCP_select1[] = {
   R_036D04_TCP_PERFCOUNTER0_SELECT1,
   R_036D0C_TCP_PERFCOUNTER1_SELECT1,
};
static struct ac_pc_block_base gfx12_TCP = {
   .gpu_block = TCP,
   .name = "TCP",
   .distribution = AC_PC_PER_SHADER_ARRAY,
   .num_counters = 4,
   .flags = AC_PC_BLOCK_SE | AC_PC_BLOCK_INSTANCE_GROUPS | AC_PC_BLOCK_SHADER_WINDOWED,

   .select0 = gfx12_TCP_select0,
   .select1 = gfx12_TCP_select1,
   .counter0_lo = R_034D00_TCP_PERFCOUNTER0_LO,

   .num_spm_counters = 2,
   .num_spm_wires = 4,
   .spm_block_select = AC_SPM_SE_BLOCK_TCP,
};

/* UTCL1 */
static unsigned gfx12_UTCL1_select0[] = {
   R_037680_UTCL1_PERFCOUNTER0_SELECT,
   R_037684_UTCL1_PERFCOUNTER1_SELECT,
   R_037688_UTCL1_PERFCOUNTER2_SELECT,
   R_03768C_UTCL1_PERFCOUNTER3_SELECT,
};
static struct ac_pc_block_base gfx12_UTCL1 = {
   .gpu_block = UTCL1,
   .name = "UTCL1",
   .distribution = AC_PC_PER_SHADER_ENGINE,
   .num_counters = 4,
   .flags = AC_PC_BLOCK_SE | AC_PC_BLOCK_SHADER_WINDOWED,

   .select0 = gfx12_UTCL1_select0,
   .counter0_lo = R_035680_UTCL1_PERFCOUNTER0_LO,

   .num_spm_counters = 0,
};

/* SQ_WQP */
static struct ac_pc_block_base gfx12_SQ_WGP = {
   .gpu_block = SQ_WGP,
   .name = "SQ_WGP",
   .distribution = AC_PC_PER_SHADER_ARRAY,
   .num_counters = 16,
   .flags = AC_PC_BLOCK_SE | AC_PC_BLOCK_SHADER,

   .select0 = gfx12_SQ_select0,
   .counter0_lo = R_034700_SQ_PERFCOUNTER0_LO,

   .num_spm_counters = 8,
   .num_spm_wires = 8,
   .spm_block_select = AC_SPM_SE_BLOCK_SQC,
};

/* GRBMSE */
static unsigned gfx12_GRBMSE_select0[] = {
   R_0363E0_GRBMH_PERFCOUNTER0_SELECT,
   R_0363E4_GRBMH_PERFCOUNTER1_SELECT,
};

static struct ac_pc_block_base gfx12_GRBMSE = {
   .gpu_block = GRBMSE,
   .name = "GRBMSE",
   .distribution = AC_PC_PER_SHADER_ENGINE,
   .num_counters = 2,
   .flags = AC_PC_BLOCK_SE | AC_PC_BLOCK_SHADER,

   .select0 = gfx12_GRBMSE_select0,
   .counter0_lo = R_0343E8_GRBMH_PERFCOUNTER0_LO,
};

static struct ac_pc_block_gfxdescr groups_gfx12[] = {
   {&gfx12_CB, 315},
   {&gfx12_CHA, 25},
   {&gfx12_CHC, 94, 4},
   {&gfx12_CPC, 55},
   {&gfx12_CPF, 43},
   {&gfx12_CPG, 95},
   {&gfx12_DB, 441},
   {&gfx12_GCR, 151},
   {&gfx12_GE, 54},
   {&gfx12_GL1A, 21},
   {&gfx12_GL1C, 121, 4},
   {&gfx12_GL2A, 114, 4},
   {&gfx12_GL2C, 249},
   {&gfx12_GRBM, 51},
   {&gfx12_GRBMSE, 25},
   {&gfx12_PA_SC, 821, 2},
   {&gfx12_PA_SU, 828},
   {&gfx12_RLC, 6},
   {&gfx12_SPI, 318},
   {&gfx12_SQ, 45},
   {&gfx12_SX, 81},
   {&gfx12_TA, 254},
   {&gfx12_TCP, 99},
   {&gfx12_TD, 271},
   {&gfx12_UTCL1, 71, 2},
   {&gfx12_SQ_WGP, 511, 4},
};

const struct ac_pc_block_gfxdescr *
ac_gfx12_get_perfcounters(uint32_t *num_blocks)
{
   *num_blocks = ARRAY_SIZE(groups_gfx12);
   return groups_gfx12;
}
