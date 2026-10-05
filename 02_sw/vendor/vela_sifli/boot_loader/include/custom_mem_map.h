/****************************************************************************
 * vendor/my_vendor/boot_loader/include/custom_mem_map.h
 *
 * SiFli SDK-compatible memory map (sf32lb52-nano_a128r16).
 ****************************************************************************/

#ifndef __CUSTOM_MEM_MAP__
#define __CUSTOM_MEM_MAP__

#define USING_PARTITION_TABLE

#ifdef USING_PARTITION_TABLE
#  include "ptab.h"
#endif

#endif /* __CUSTOM_MEM_MAP__ */
