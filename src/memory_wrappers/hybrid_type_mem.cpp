#include "hybrid_type_mem.h"
#include "mem_ctrls.h"
#include "ddr_mem.h"
#include "weave_md1_mem.h"
#include "dramsim_mem_ctrl.h"
#include "detailed_mem.h"

uint32_t HybridWrapperMemory::controllers = 0;
g_vector<uint32_t> HybridWrapperMemory::types = g_vector<uint32_t>();