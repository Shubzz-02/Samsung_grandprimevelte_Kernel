#ifndef __MACH_CLK_CORE_HELANX_H
#define __MACH_CLK_CORE_HELANX_H

#include <linux/clk/mmpdcstat.h>

/* reserved flag for different FC version */
#define HELANX_FC_V1	BIT(0)

/* RTC/WTC table used for solution change rtc/wtc on the fly */
struct cpu_rtcwtc {
	unsigned int max_pclk;	/* max rate could be used by this rtc/wtc */
	unsigned int l1_xtc;
	unsigned int l2_xtc;
};

/*
 * AP clock source:
 * 0x0 = PLL1 624 MHz
 * 0x1 = PLL1 1248 MHz  or PLL3_CLKOUT
 * (depending on PLL3_CR[18])
 * 0x2 = PLL2_CLKOUT
 * 0x3 = PLL2_CLKOUTP
 */
enum ap_clk_sel {
	AP_CLK_SRC_PLL1_624 = 0x0,
	AP_CLK_SRC_PLL1_1248 = 0x1,
	AP_CLK_SRC_PLL2 = 0x2,
	AP_CLK_SRC_PLL2P = 0x3,
	AP_CLK_SRC_PLL3P = 0x5,
};

struct cpu_opt {
	unsigned int pclk;		/* core clock */
	unsigned int pdclk;		/* DDR interface clock */
	unsigned int baclk;		/* bus interface clock */
	enum ap_clk_sel ap_clk_sel;	/* core src sel val */
	struct clk *parent;		/* core clk parent node */
	unsigned int ap_clk_src;	/* core src rate */
	unsigned int pclk_div;		/* core clk divider*/
	unsigned int pdclk_div;		/* DDR interface clock divider */
	unsigned int baclk_div;		/* bus interface clock divider */
	unsigned int l1_xtc;		/* L1 cache RTC/WTC */
	unsigned int l2_xtc;		/* L2 cache RTC/WTC */
	struct list_head node;
};

/*
 * struct core_params store core specific data
 */
struct core_params {
	void __iomem			*apmu_base;
	void __iomem			*mpmu_base;
	void __iomem			*ciu_base;
	struct parents_table	*parent_table;
	int				parent_table_size;
	struct cpu_opt			*cpu_opt;
	int				cpu_opt_size;
	struct cpu_rtcwtc		*cpu_rtcwtc_table;
	int				cpu_rtcwtc_table_size;
	unsigned int			max_cpurate;
	unsigned int			bridge_cpurate;
	spinlock_t			*shared_lock;

	/* dynamic dc stat support? */
	bool				dcstat_support;

	powermode			pxa_powermode;

};

extern struct clk *mmp_clk_register_core(const char *name,
		const char **parent_name,
		u8 num_parents, unsigned long flags, u32 core_flags,
		spinlock_t *lock, struct core_params *params);

/* DDR */
struct ddr_opt {
	unsigned int dclk;		/* ddr clock */
	unsigned int ddr_tbl_index;	/* ddr FC table index */
	unsigned int ddr_lpmtbl_index;	/* ddr LPM table index */
	unsigned int ddr_freq_level;	/* ddr freq level(0~7) */
	unsigned int ddr_volt_level;	/* ddr voltage level (0~7) */
	u32 ddr_clk_sel;		/* ddr src sel val */
	unsigned int ddr_clk_src;	/* ddr src rate */
	struct clk *ddr_parent;		/* ddr clk parent node */
	unsigned int dclk_div;		/* ddr clk divider */
};

struct ddr_params {
	void __iomem			*apmu_base;
	void __iomem			*mpmu_base;
	void __iomem			*dmcu_base;

	struct parents_table		*parent_table;
	int				parent_table_size;
	struct ddr_opt			*ddr_opt;
	int				ddr_opt_size;
	unsigned long			*hwdfc_freq_table;
	int				hwdfc_table_size;
	/* dynamic dc stat support? */
	bool				dcstat_support;
};

/* FLAGS */
/* only show ddr rate for AP part */
#define MMP_DDR_RATE_AP_ONLY		BIT(3)
struct clk *mmp_clk_register_ddr(const char *name, const char **parent_name,
		u8 num_parents, unsigned long flags, u32 ddr_flags,
		spinlock_t *lock, struct ddr_params *params);

/*
 * For combined ddr solution, other components can
 * register to let ddr change its rate
 */
struct ddr_combclk_relation {
	unsigned long dclk_rate;
	unsigned long combclk_rate;
};

struct ddr_combined_clk {
	struct clk *clk;
	unsigned long maxrate;
	struct list_head node;
	/* Describe the relationship with Dclk */
	struct ddr_combclk_relation *relationtbl;
	unsigned int num_relationtbl;
};

int register_clk_bind2ddr(struct clk *clk, unsigned long max_freq,
			  struct ddr_combclk_relation *relationtbl,
			  unsigned int num_relationtbl);
/*
 * MMP AXI:
 */
struct axi_opt {
	unsigned int aclk;		/* axi clock */
	u32 axi_clk_sel;		/* axi src sel val */
	unsigned int axi_clk_src;	/* axi src rate */
	struct clk *axi_parent;		/* axi clk parent node */
	unsigned int aclk_div;		/* axi clk divider */
};

struct axi_params {
	void __iomem			*apmu_base;
	void __iomem			*mpmu_base;
	struct parents_table		*parent_table;
	int				parent_table_size;
	struct axi_opt			*axi_opt;
	int				axi_opt_size;
	/* dynamic dc stat support? */
	bool				dcstat_support;
};

struct clk *mmp_clk_register_axi(const char *name, const char **parent_name,
		u8 num_parents, unsigned long flags, u32 axi_flags,
		spinlock_t *lock, struct axi_params *params);



#endif
