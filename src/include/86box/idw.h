/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Definitions for the Internal Debugger Window (IDW).
 *
 *          The IDW is built on the GDB stub's CPU hooks: commands are
 *          queued by the UI and executed on the CPU thread from inside
 *          the stub's cpu_exec hook, where CPU state is safe to inspect.
 */
#ifndef EMU_IDW_H
#define EMU_IDW_H

#ifdef IDW

#    include <stddef.h>
#    include <stdint.h>

#    ifdef __cplusplus
extern "C" {
#    endif

/* Called from the UI thread. */
extern void idw_break(void);
extern void idw_command(const char *line);
extern void idw_help(void);

/* Called from the GDB stub; idw_instructions counts every instruction executed. */
extern uint64_t idw_instructions;
extern void idw_init(void);
extern void idw_note_stop(int type, uint32_t addr);
extern void idw_process(void);

/* Supplied by the UI; called from any thread. */
extern void idw_output(const char *text);

/* Shared by idw.c and idw_dis.c; the column (from 0) where comments on disassembly lines start. */
#    define IDW_COMMENT_COL 68

/* Also shared by idw.c and idw_dis.c; called from the CPU thread. */
extern int      idw_read_byte(uint32_t addr);
/* type is the PCjs address prefix: '&' for real/V86, '#' for protected, or '%' for linear (sel ignored). */
extern uint32_t idw_disasm(char *line, size_t size, char type, uint16_t sel, uint32_t base, uint32_t off, int size32);
extern int      idw_step_over(uint32_t base, uint32_t off, int size32, uint32_t *next);

#    ifdef __cplusplus
}
#    endif

#endif

#endif
