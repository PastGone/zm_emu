#include "../emu.h"
#include "../log/log.h"
#include <capstone/capstone.h>
#include <stddef.h>
#include <stdint.h>
#include <unicorn/unicorn.h>

void disassemble_and_log(uc_engine *uc, uint64_t address, size_t size) {
	uc_mem_read(uc, address, g_cscode, size);

	g_sc_count = cs_disasm(g_cs_handle, g_cscode, size, address, 0, &g_sc_insn);
	if (g_sc_count > 0) {
		char line[256];
		for (size_t i = 0; i < g_sc_count; i++) {
			int offset = snprintf(line, sizeof(line), "0x%08" PRIx64 ":  ", g_sc_insn[i].address);

			for (int j = 0; j < 4; j++) {
				if (j < g_sc_insn[i].size) {
					offset += snprintf(
						line + offset, sizeof(line) - offset, "%02x ", g_sc_insn[i].bytes[j]);
				} else {
					offset += snprintf(line + offset, sizeof(line) - offset, "   ");
				}
			}

			snprintf(line + offset,
					 sizeof(line) - offset,
					 "%-8s %s",
					 g_sc_insn[i].mnemonic,
					 g_sc_insn[i].op_str);

			log_trace("%s\n", line);
		}
		cs_free(g_sc_insn, g_sc_count);

	} else {
		fprintf(stderr, "Disassembly failed\n");
	}
};
