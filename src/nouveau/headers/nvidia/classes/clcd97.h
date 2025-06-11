#ifndef _cl_blackwell_a_h_
#define _cl_blackwell_a_h_

#define BLACKWELL_A 0xCD97

#define NVCD97_SET_ST_A                                                                                    0x0f00
#define NVCD97_SET_ST_A_OFFSET_UPPER                                                                          7:0

#define NVCD97_SET_ST_B                                                                                    0x0f04
#define NVCD97_SET_ST_B_OFFSET_LOWER                                                                         31:0

#define NVCD97_SET_ST_BLOCK_SIZE                                                                           0x0f08
#define NVCD97_SET_ST_BLOCK_SIZE_WIDTH                                                                        3:0
#define NVCD97_SET_ST_BLOCK_SIZE_WIDTH_ONE_GOB                                                         0x00000000
#define NVCD97_SET_ST_BLOCK_SIZE_HEIGHT                                                                       7:4
#define NVCD97_SET_ST_BLOCK_SIZE_HEIGHT_ONE_GOB                                                        0x00000000
#define NVCD97_SET_ST_BLOCK_SIZE_HEIGHT_TWO_GOBS                                                       0x00000001
#define NVCD97_SET_ST_BLOCK_SIZE_HEIGHT_FOUR_GOBS                                                      0x00000002
#define NVCD97_SET_ST_BLOCK_SIZE_HEIGHT_EIGHT_GOBS                                                     0x00000003
#define NVCD97_SET_ST_BLOCK_SIZE_HEIGHT_SIXTEEN_GOBS                                                   0x00000004
#define NVCD97_SET_ST_BLOCK_SIZE_HEIGHT_THIRTYTWO_GOBS                                                 0x00000005
#define NVCD97_SET_ST_BLOCK_SIZE_DEPTH                                                                       11:8
#define NVCD97_SET_ST_BLOCK_SIZE_DEPTH_ONE_GOB                                                         0x00000000

#define NVCD97_SET_ST_ARRAY_PITCH                                                                          0x0f0c
#define NVCD97_SET_ST_ARRAY_PITCH_V                                                                          31:0

#define NVCD97_SET_ZT_FORMAT                                                                               0x0fe8
#define NVCD97_SET_ZT_FORMAT_V                                                                                4:0
#define NVCD97_SET_ZT_FORMAT_V_Z16                                                                     0x00000013
#define NVCD97_SET_ZT_FORMAT_V_Z24S8                                                                   0x00000014
#define NVCD97_SET_ZT_FORMAT_V_X8Z24                                                                   0x00000015
#define NVCD97_SET_ZT_FORMAT_V_S8Z24                                                                   0x00000016
#define NVCD97_SET_ZT_FORMAT_V_S8                                                                      0x00000017
#define NVCD97_SET_ZT_FORMAT_V_ZF32                                                                    0x0000000A
#define NVCD97_SET_ZT_FORMAT_V_ZF32_X24S8                                                              0x00000019
#define NVCD97_SET_ZT_FORMAT_STENCIL_IS_SEPARATE                                                              8:8
#define NVCD97_SET_ZT_FORMAT_STENCIL_IS_SEPARATE_FALSE                                                 0x00000000
#define NVCD97_SET_ZT_FORMAT_STENCIL_IS_SEPARATE_TRUE                                                  0x00000001

// equivalent of SET_ZT_SIZE_[AB]
#define NVCD97_SET_ST_SIZE_A                                                                               0x120c
#define NVCD97_SET_ST_SIZE_A_WIDTH                                                                           27:0

#define NVCD97_SET_ST_SIZE_B                                                                               0x1210
#define NVCD97_SET_ST_SIZE_B_HEIGHT                                                                          17:0

#endif
