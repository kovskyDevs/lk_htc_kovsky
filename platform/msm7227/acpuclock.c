/*
* Copyright (c) 2008, Google Inc.
* All rights reserved.
* Copyright (c) 2009, Code Aurora Forum. All rights reserved.
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following conditions
* are met:
*  * Redistributions of source code must retain the above copyright
*    notice, this list of conditions and the following disclaimer.
*  * Redistributions in binary form must reproduce the above copyright
*    notice, this list of conditions and the following disclaimer in
*    the documentation and/or other materials provided with the
*    distribution.
*
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
* "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
* LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
* FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
* COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
* INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
* BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
* OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
* AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
* OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
* OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
* SUCH DAMAGE.
*/

#include <stdint.h>
#include <kernel/thread.h>
#include <platform/iomap.h>
#include <reg.h>
#include <debug.h>

/* Fancy debugging messages */
#define TCXO_RATE			19200000
#define PLLn_BASE(n)	(MSM_CLK_CTL_BASE + 0x300 + 28 * (n))
#define PLLn_L_VAL(n)	(PLLn_BASE(n) + 4)
#define PLL_FREQ(l, m, n)	(TCXO_RATE * (l) + TCXO_RATE * (m) / (n))

static void pll_dump(void)
{
	unsigned int mode, L, M, N, freq;

	for (int pll = 0; pll <= 3; pll++) {
		mode = readl(PLLn_BASE(pll) + 0x0);
		L = readl(PLLn_BASE(pll) + 0x4);
		M = readl(PLLn_BASE(pll) + 0x8);
		N = readl(PLLn_BASE(pll) + 0xc);
		freq = PLL_FREQ(L, M, N);
		dprintf(INFO, "PLL%d: MODE=%08x L=%08x M=%08x N=%08x freq=%u Hz (%u MHz)\n",
			pll, mode, L, M, N, freq, freq / 1000000);
	}
}

#define ARRAY_SIZE(x) (sizeof(x)/sizeof((x)[0]))

#define A11S_CLK_CNTL_ADDR	(MSM_CSR_BASE + 0x100)
#define A11S_CLK_SEL_ADDR	(MSM_CSR_BASE + 0x104)
#define VDD_SVS_PLEVEL_ADDR	(MSM_CSR_BASE + 0x124)
#define PLL2_L_VAL_ADDR		(MSM_CLK_CTL_BASE + 0x33C)

#define SRC_SEL_PLL1	1	/* PLL1. */
#define SRC_SEL_PLL2	2	/* PLL2. */
#define SRC_SEL_PLL3	3	/* PLL3. Used for 7x25. */
#define DIV_4		3
#define DIV_2		1
#define WAIT_CNT	100
#define VDD_LEVEL	7
#define MIN_AXI_HZ	120000000
#define ACPU_800MHZ	41

void pll_request(unsigned pll, unsigned enable);
void axi_clock_init(unsigned rate);

/* The stepping frequencies have been choosen to make sure the step
* is <= 256 MHz for both turbo mode and normal mode targets.  The
* table also assumes the ACPU is running at TCXO freq and AHB div is
* set to DIV_1.
*
* To use the tables:
* - Start at location 0/1 depending on clock source sel bit.
* - Set values till end of table skipping every other entry.
* - When you reach the end of the table, you are done scaling.
*
* TODO: Need to fix SRC_SEL_PLL1 for 7x25.
*/

uint32_t const clk_cntl_reg_val_7625[] = {
	(WAIT_CNT << 16) | (SRC_SEL_PLL1 << 4) | DIV_4,
	(WAIT_CNT << 16) | (SRC_SEL_PLL1 << 12) | (DIV_4 << 8),
	(WAIT_CNT << 16) | (SRC_SEL_PLL1 << 12) | (DIV_2 << 8),
	(WAIT_CNT << 16) | (SRC_SEL_PLL1 << 4) | DIV_2,
	(WAIT_CNT << 16) | (SRC_SEL_PLL3 << 4) | DIV_2,
	(WAIT_CNT << 16) | (SRC_SEL_PLL3 << 12) | (DIV_2 << 8),
};

uint32_t const clk_cntl_reg_val_7627[] = {
	(WAIT_CNT << 16) | (SRC_SEL_PLL1 << 4) | DIV_4,
	(WAIT_CNT << 16) | (SRC_SEL_PLL1 << 12) | (DIV_4 << 8),
	(WAIT_CNT << 16) | (SRC_SEL_PLL1 << 12) | (DIV_2 << 8),
	(WAIT_CNT << 16) | (SRC_SEL_PLL1 << 4) | DIV_2,
	(WAIT_CNT << 16) | (SRC_SEL_PLL2 << 4) | DIV_2,
	(WAIT_CNT << 16) | (SRC_SEL_PLL2 << 12) | (DIV_2 << 8),
};

uint32_t const clk_cntl_reg_val_7627T[] = {
	(WAIT_CNT << 16) | (SRC_SEL_PLL1 << 4) | DIV_4,
	(WAIT_CNT << 16) | (SRC_SEL_PLL1 << 12) | (DIV_4 << 8),
	(WAIT_CNT << 16) | (SRC_SEL_PLL1 << 12) | (DIV_2 << 8),
	(WAIT_CNT << 16) | (SRC_SEL_PLL1 << 4) | DIV_2,
	(WAIT_CNT << 16) | (SRC_SEL_PLL2 << 4),
	(WAIT_CNT << 16) | (SRC_SEL_PLL2 << 12),
};

/* Using DIV_4 for all cases to avoid worrying about turbo vs. normal
* mode. Able to use DIV_4 for all steps because it's the largest AND
* the final value. */
uint32_t const clk_sel_reg_val[] = {
	DIV_4 << 1 | 1,
	DIV_4 << 1 | 0,
	DIV_4 << 1 | 0,
	DIV_4 << 1 | 1,
	DIV_4 << 1 | 1,
	DIV_4 << 1 | 0,
};

void mdelay(unsigned msecs);

void acpu_clock_init(void)
{
	unsigned i, clk;

#if (!ENABLE_NANDWRITE)
	int *modem_stat_check = (MSM_SHARED_BASE + 0x14);

	/* Wait for modem to be ready before clock init */
	while (readl(modem_stat_check) != 1) ;
#endif

	/* Increase VDD level to the final value. */
	writel((1 << 7) | (VDD_LEVEL << 3), VDD_SVS_PLEVEL_ADDR);
	mdelay(1);

	/* Read clock source select bit. */
	i = readl(A11S_CLK_SEL_ADDR) & 1;
	clk = readl(PLL2_L_VAL_ADDR) & 0x3F;

	/* Jump into table and set every other entry. */
	for (; i < ARRAY_SIZE(clk_cntl_reg_val_7627); i += 2) {
#ifdef ENABLE_PLL3
		writel(clk_cntl_reg_val_7625[i], A11S_CLK_CNTL_ADDR);
#else
		if (clk == ACPU_800MHZ)
			writel(clk_cntl_reg_val_7627T[i], A11S_CLK_CNTL_ADDR);
		else
			writel(clk_cntl_reg_val_7627[i], A11S_CLK_CNTL_ADDR);
#endif
	/* Would need a dmb() here but the whole address space is
	 * strongly ordered, so it should be fine.
	 */
		writel(clk_sel_reg_val[i], A11S_CLK_SEL_ADDR);
		mdelay(1);
	}
	pll_dump();
}

