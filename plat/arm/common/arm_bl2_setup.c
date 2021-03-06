/*
 * Copyright (c) 2015-2018, ARM Limited and Contributors. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <arch_helpers.h>
#include <arm_def.h>
#include <assert.h>
#include <bl_common.h>
#include <debug.h>
#include <desc_image_load.h>
#include <generic_delay_timer.h>
#ifdef SPD_opteed
#include <optee_utils.h>
#endif
#include <plat_arm.h>
#include <platform.h>
#include <platform_def.h>
#include <string.h>
#include <utils.h>

/* Data structure which holds the extents of the trusted SRAM for BL2 */
static meminfo_t bl2_tzram_layout __aligned(CACHE_WRITEBACK_GRANULE);

/*
 * Check that BL2_BASE is above ARM_TB_FW_CONFIG_LIMIT. This reserved page is
 * for `meminfo_t` data structure and fw_configs passed from BL1.
 */
CASSERT(BL2_BASE >= ARM_TB_FW_CONFIG_LIMIT, assert_bl2_base_overflows);

/* Weak definitions may be overridden in specific ARM standard platform */
#pragma weak bl2_early_platform_setup2
#pragma weak bl2_platform_setup
#pragma weak bl2_plat_arch_setup
#pragma weak bl2_plat_sec_mem_layout

#define MAP_BL2_TOTAL		MAP_REGION_FLAT(			\
					bl2_tzram_layout.total_base,	\
					bl2_tzram_layout.total_size,	\
					MT_MEMORY | MT_RW | MT_SECURE)


#pragma weak arm_bl2_plat_handle_post_image_load

/*******************************************************************************
 * BL1 has passed the extents of the trusted SRAM that should be visible to BL2
 * in x0. This memory layout is sitting at the base of the free trusted SRAM.
 * Copy it to a safe location before its reclaimed by later BL2 functionality.
 ******************************************************************************/
void arm_bl2_early_platform_setup(uintptr_t tb_fw_config,
				  struct meminfo *mem_layout)
{
	/* Initialize the console to provide early debug support */
	arm_console_boot_init();

	/* Setup the BL2 memory layout */
	bl2_tzram_layout = *mem_layout;

	/* Initialise the IO layer and register platform IO devices */
	plat_arm_io_setup();

	if (tb_fw_config != 0U)
		arm_bl2_set_tb_cfg_addr((void *)tb_fw_config);
}

void bl2_early_platform_setup2(u_register_t arg0, u_register_t arg1, u_register_t arg2, u_register_t arg3)
{
	arm_bl2_early_platform_setup((uintptr_t)arg0, (meminfo_t *)arg1);

	generic_delay_timer_init();
}

/*
 * Perform  BL2 preload setup. Currently we initialise the dynamic
 * configuration here.
 */
void bl2_plat_preload_setup(void)
{
	arm_bl2_dyn_cfg_init();
}

/*
 * Perform ARM standard platform setup.
 */
void arm_bl2_platform_setup(void)
{
	/* Initialize the secure environment */
	plat_arm_security_setup();

#if defined(PLAT_ARM_MEM_PROT_ADDR)
	arm_nor_psci_do_static_mem_protect();
#endif
}

void bl2_platform_setup(void)
{
	arm_bl2_platform_setup();
}

/*******************************************************************************
 * Perform the very early platform specific architectural setup here. At the
 * moment this is only initializes the mmu in a quick and dirty way.
 ******************************************************************************/
void arm_bl2_plat_arch_setup(void)
{
#if USE_COHERENT_MEM && !ARM_CRYPTOCELL_INTEG
	/*
	 * Ensure ARM platforms don't use coherent memory in BL2 unless
	 * cryptocell integration is enabled.
	 */
	assert((BL_COHERENT_RAM_END - BL_COHERENT_RAM_BASE) == 0U);
#endif

	const mmap_region_t bl_regions[] = {
		MAP_BL2_TOTAL,
		ARM_MAP_BL_RO,
#if USE_ROMLIB
		ARM_MAP_ROMLIB_CODE,
		ARM_MAP_ROMLIB_DATA,
#endif
#if ARM_CRYPTOCELL_INTEG
		ARM_MAP_BL_COHERENT_RAM,
#endif
		{0}
	};

	arm_setup_page_tables(bl_regions, plat_arm_get_mmap());

#ifdef AARCH32
	enable_mmu_svc_mon(0);
#else
	enable_mmu_el1(0);
#endif

	arm_setup_romlib();
}

void bl2_plat_arch_setup(void)
{
	arm_bl2_plat_arch_setup();
}

int arm_bl2_handle_post_image_load(unsigned int image_id)
{
	int err = 0;
	bl_mem_params_node_t *bl_mem_params = get_bl_mem_params_node(image_id);
#ifdef SPD_opteed
	bl_mem_params_node_t *pager_mem_params = NULL;
	bl_mem_params_node_t *paged_mem_params = NULL;
#endif
	assert(bl_mem_params);

	switch (image_id) {
#ifdef AARCH64
	case BL32_IMAGE_ID:
#ifdef SPD_opteed
		pager_mem_params = get_bl_mem_params_node(BL32_EXTRA1_IMAGE_ID);
		assert(pager_mem_params);

		paged_mem_params = get_bl_mem_params_node(BL32_EXTRA2_IMAGE_ID);
		assert(paged_mem_params);

		err = parse_optee_header(&bl_mem_params->ep_info,
				&pager_mem_params->image_info,
				&paged_mem_params->image_info);
		if (err != 0) {
			WARN("OPTEE header parse error.\n");
		}
#endif
		bl_mem_params->ep_info.spsr = arm_get_spsr_for_bl32_entry();
		break;
#endif

	case BL33_IMAGE_ID:
		/* BL33 expects to receive the primary CPU MPID (through r0) */
		bl_mem_params->ep_info.args.arg0 = 0xffff & read_mpidr();
		bl_mem_params->ep_info.spsr = arm_get_spsr_for_bl33_entry();
		break;

#ifdef SCP_BL2_BASE
	case SCP_BL2_IMAGE_ID:
		/* The subsequent handling of SCP_BL2 is platform specific */
		err = plat_arm_bl2_handle_scp_bl2(&bl_mem_params->image_info);
		if (err) {
			WARN("Failure in platform-specific handling of SCP_BL2 image.\n");
		}
		break;
#endif
	default:
		/* Do nothing in default case */
		break;
	}

	return err;
}

/*******************************************************************************
 * This function can be used by the platforms to update/use image
 * information for given `image_id`.
 ******************************************************************************/
int arm_bl2_plat_handle_post_image_load(unsigned int image_id)
{
	return arm_bl2_handle_post_image_load(image_id);
}

int bl2_plat_handle_post_image_load(unsigned int image_id)
{
	return arm_bl2_plat_handle_post_image_load(image_id);
}
