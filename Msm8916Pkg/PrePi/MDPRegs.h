#ifndef _MDP_REGS_H_
#define _MDP_REGS_H_

/* SSPP (source pipe) register offsets, MDP5 v1.6 */
#define PIPE_SSPP_SRC_SIZE                      0x00
#define PIPE_SSPP_SRC0_ADDR                     0x14
#define PIPE_SSPP_SRC_FORMAT                    0x30
#define PIPE_SSPP_SRC_UNPACK_PATTERN            0x34
#define PIPE_BASE				0x1A15000
#define PIPE_BASE1				0x1A17000
#define PIPE_SSPP_SRC_YSTRIDE			0x24

#define MDP_BASE                    (0x1A00000)
#define REG_MDP(off)                (MDP_BASE + (off))
#define MDP_HW_REV                              REG_MDP(0x1000)
#define MDP_VP_0_VIG_0_BASE                     REG_MDP(0x5000)
#define MDP_VP_0_VIG_1_BASE                     REG_MDP(0x7000)
#define MDP_VP_0_RGB_0_BASE                     REG_MDP(0x15000)
#define MDP_VP_0_RGB_1_BASE                     REG_MDP(0x17000)
#define MDP_VP_0_DMA_0_BASE                     REG_MDP(0x25000)
#define MDP_VP_0_DMA_1_BASE                     REG_MDP(0x27000)
#define MDP_VP_0_MIXER_0_BASE                   REG_MDP(0x45000)
#define MDP_VP_0_MIXER_1_BASE                   REG_MDP(0x46000)

#define MDP_CTL_0_BASE                          0x1A02000
#define MDP_CTL_1_BASE                          0x1A02200
#define CTL_LAYER_0				0x00	/* mixer 0 blend stages */
#define CTL_FLUSH				0x18

/* Stage field (3 bits) of each pipe in CTL_LAYER_x, and its CTL_FLUSH bit */
#define CTL_LAYER_VIG0_SHIFT			0
#define CTL_LAYER_RGB0_SHIFT			9
#define CTL_LAYER_RGB1_SHIFT			12
#define CTL_LAYER_DMA0_SHIFT			18
#define CTL_FLUSH_VIG0				(1 << 0)
#define CTL_FLUSH_RGB0				(1 << 3)
#define CTL_FLUSH_RGB1				(1 << 4)
#define CTL_FLUSH_DMA0				(1 << 11)

#endif /* _MDP_REGS_H_ */
