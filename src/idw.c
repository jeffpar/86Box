/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Internal Debugger Window (IDW) command processor.
 *
 *          The UI queues command lines with idw_command() and requests a
 *          stop with idw_break(). The GDB stub calls idw_process() on the
 *          CPU thread from its cpu_exec hook, the same place it services
 *          GDB packets, so commands can safely read CPU state and memory.
 *          Stopping, stepping and continuing all go through gdbstub_step.
 */
#include <ctype.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define HAVE_STDARG_H
#include <86box/86box.h>
#include "cpu.h"
#include "x86.h"
#include "x86seg_common.h"
#include "x86_flags.h"
#include <86box/mem.h>
#include <86box/thread.h>
#include <86box/gdbstub.h>
#include <86box/idw.h>

#define IDW_QUEUE_LEN 8
#define IDW_LINE_LEN  256
#define IDW_DUMP_LEN  0x80
#define IDW_DUMP_MAX  0x1000

typedef struct {
    const char *name;
    x86seg     *seg;
} idw_segreg_t;

static const idw_segreg_t idw_segregs[] = {
    { "cs", &cpu_state.seg_cs },
    { "ds", &cpu_state.seg_ds },
    { "es", &cpu_state.seg_es },
    { "ss", &cpu_state.seg_ss },
    { "fs", &cpu_state.seg_fs },
    { "gs", &cpu_state.seg_gs }
};

/* Shared between the UI and CPU threads; protected by idw_mutex. */
static mutex_t *idw_mutex;
static char     idw_queue[IDW_QUEUE_LEN][IDW_LINE_LEN];
static int      idw_queue_head;
static int      idw_queue_count;
static int      idw_break_pending;

/* A code address, like PCjs's DbgAddrx86; type is '&' (real/V86), '#' (protected) or '%' (linear). */
typedef struct {
    char     type;
    uint16_t sel;
    uint32_t off;
} idw_addr_t;

/* CPU thread only. */
static int        idw_was_stopped;
static int        idw_tracing;   /* a t is in progress, so its halt isn't a status change */
static int        idw_stepping;  /* likewise for a p that's running to its temporary breakpoint */
static int        idw_brief;     /* that t or p shows just the next instruction (tr and pr dump registers) */
static int        idw_regs_full; /* 1 if rp was used more recently than r, so later dumps use its layout */
static uint64_t   idw_run_ops;    /* idw_instructions when the CPU last resumed */
static uint64_t   idw_run_tsc;    /* tsc when the CPU last resumed */

uint64_t idw_instructions;
static idw_addr_t idw_next_code; /* where a bare "u" continues */
static idw_addr_t idw_next_data; /* where a bare "d" continues */
static int        idw_next_code_set;
static int        idw_next_data_set;
static int        idw_dump_size = 1; /* the last dump's unit size, for a bare d */

/* Breakpoints, numbered 1 to IDW_BREAK_MAX; they live in the GDB stub's lists. */
#define IDW_BREAK_MAX 99

typedef struct {
    int        type;  /* GDBSTUB_BREAK_HW, _RWATCH or _WWATCH, or 0 if unused */
    int        owned; /* 1 if we added it to the stub's list (0 if a GDB client already had) */
    int        temp;  /* 1 for p's breakpoint, which is cleared when the CPU next halts */
    idw_addr_t addr;
    int        size32;
    uint32_t   linear;
} idw_break_t;

static idw_break_t idw_breaks[IDW_BREAK_MAX + 1];
static int         idw_stop_type; /* why the CPU last stopped (a GDBSTUB_* value) */
static uint32_t    idw_stop_addr; /* the linear address that stopped it */

static void
idw_printf(const char *fmt, ...)
{
    char    buf[1024];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    idw_output(buf);
}

static int
idw_stopped(void)
{
    return gdbstub_step >= GDBSTUB_BREAK;
}

/* Read a byte at a linear address without disturbing the CPU; returns -1 on a page fault. */
int
idw_read_byte(uint32_t addr)
{
    uint8_t  orig_abrt       = cpu_state.abrt;
    uint32_t orig_abrt_error = abrt_error;
    uint32_t orig_cr2        = cr2;
    uint8_t  val;

    cpl_override = 1;
    val          = readmembl(addr);
    cpl_override = 0;

    if (cpu_state.abrt != orig_abrt) {
        cpu_state.abrt = orig_abrt;
        abrt_error     = orig_abrt_error;
        cr2            = orig_cr2;
        return -1;
    }

    return val;
}

void
idw_help(void)
{
    idw_output("  ? [expr]        help / evaluate\n"
               "  b               manage breakpoints\n"
               "  d [addr [end]]  dump memory\n"
               "  g [addr]        go\n"
               "  h               halt\n"
               "  p               ptrace (step over)\n"
               "  r               dump registers\n"
               "  t               trace (step into)\n"
               "  u [addr [end]]  unassemble\n"
               "Type a command followed by ? (eg, d?) for details.\n");
}

/* Details for "d?" and the like; returns 0 if there are none for that command. */
static int
idw_help_command(const char *cmd)
{
    if (cmd[0] == 'b') {
        idw_output("  bp addr         set exec breakpoint at addr\n"
                   "  br addr         set read breakpoint at addr\n"
                   "  bw addr         set write breakpoint at addr\n"
                   "  bc n            clear breakpoint n (1-99), or all with bc *\n"
                   "  bl              list breakpoints\n");
        return 1;
    }
    if (!strcmp(cmd, "d") || !strcmp(cmd, "db") || !strcmp(cmd, "dw") || !strcmp(cmd, "dd")) {
        idw_output("d [addr [end]]  dump memory at addr through end;\n"
                   "                end may be another addr or 'l' followed by length;\n"
                   "                use db for bytes, dw for words, dd for dwords\n");
    } else if (!strcmp(cmd, "g")) {
        idw_output("g [addr]        go: resume execution until halted (with h or Ctrl-C),\n"
                   "                or until addr is reached\n");
    } else if (!strcmp(cmd, "h")) {
        idw_output("h               halt: stop execution and dump registers (Ctrl-C also halts)\n");
    } else if (!strcmp(cmd, "p") || !strcmp(cmd, "pr")) {
        idw_output("p               ptrace: like t, but a CALL, HLT, INT, LOOP, or REP string\n"
                   "                instruction runs until the next instruction is reached\n"
                   "pr              likewise, then dump registers\n");
    } else if (!strcmp(cmd, "r") || !strcmp(cmd, "rp")) {
        idw_output("r               dump registers and the next instruction to execute\n"
                   "rp              likewise, with segment bases and limits, descriptor\n"
                   "                tables, TR, A20, and control registers\n");
    } else if (!strcmp(cmd, "t") || !strcmp(cmd, "tr")) {
        idw_output("t               trace: execute one instruction, then show the next\n"
                   "tr              likewise, then dump registers\n");
    } else if (!strcmp(cmd, "u")) {
        idw_output("u [addr [end]]  unassemble instructions at addr through end;\n"
                   "                end may be another addr or 'l' followed by a count\n");
    } else {
        return 0;
    }
    return 1;
}

/* A segment register, like PCjs's getSegOutput(): with base and limit in protected mode. */
static void
idw_cat_seg(char *s, const char *name, const x86seg *seg, int prot)
{
    s += strlen(s);
    s += sprintf(s, "%s=%04X", name, seg->seg);
    if (prot)
        sprintf(s, "[%0*X,%0*X]", is386 ? 8 : 6, seg->base, (seg->limit & ~0xffff) ? 8 : 4, seg->limit);
}

/* A descriptor table register, like PCjs's getDTROutput(). */
static void
idw_cat_dtr(char *s, const char *name, const x86seg *seg, int show_sel)
{
    s += strlen(s);
    s += sprintf(s, "%s=", name);
    if (show_sel)
        s += sprintf(s, "%04X", seg->seg);
    sprintf(s, "[%0*X,%04X] ", is386 ? 8 : 6, seg->base, seg->limit);
}

/*
 * The next instruction's disassembly, which also becomes where a bare u continues; with
 * counts, it ends with the instructions (and, for one, cycles) since the CPU last resumed.
 */
static void
idw_next_line(char *ins, size_t size, int counts)
{
    int prot = msw & 1;
    int v86  = prot && is386 && (cpu_state.eflags & VM_FLAG);

    idw_next_code.type = (prot && !v86) ? '#' : '&';
    idw_next_code.sel  = CS;
    idw_next_code.off  = idw_disasm(ins, size, idw_next_code.type, CS, cs, cpu_state.pc, (idw_next_code.type == '#') && use32);
    idw_next_code_set  = 1;

    /* After a stop, the instructions (and, for one, cycles) since the CPU last resumed, in the comment column. */
    if (counts) {
        uint64_t ops   = idw_instructions - idw_run_ops;
        uint64_t multi = xt_cpu_multi >> 32;
        int      n     = (int) strlen(ins);
        int      col   = IDW_COMMENT_COL;
        char    *p     = ins + n;

        if (!strchr(ins, ';') && (n < col))
            p += snprintf(p, size - (p - ins), "%*s", col - n, "");
        /* One instruction shows its cycles instead (unless tsc started over after a hard reset). */
        if ((ops == 1) && multi && (tsc >= idw_run_tsc)) {
            uint64_t clocks = (tsc - idw_run_tsc) / multi;
            snprintf(p, size - (p - ins), "; %" PRIu64 " cycle%s", clocks, (clocks == 1) ? "" : "s");
        } else {
            snprintf(p, size - (p - ins), "; %" PRIu64 " instruction%s", ops, (ops == 1) ? "" : "s");
        }
    }
}

/*
 * Registers and the next instruction, in the PCjs debugger's format; eg, for real mode:
 *
 *      AX=0B00 BX=0009 CX=0188 DX=0300 SP=7BDE BP=7BE8 SI=EFC7 DI=0044
 *      SS=0000 DS=0040 ES=0060 PS=F202 V0 D0 I1 T0 S0 Z0 A0 P0 C0
 *      &F000:EEC3 E2FE             LOOP     EEC3
 *
 * With full (rp, on a 286 or later), segment registers include [base,limit], followed
 * by the descriptor table registers, TR, A20, and control registers, in any mode.
 */
static void
idw_regs(int counts, int full)
{
    static const struct {
        char     name;
        uint16_t bit;
    } flags[] = {
        { 'V', V_FLAG }, { 'D', D_FLAG }, { 'I', I_FLAG }, { 'T', T_FLAG }, { 'S', N_FLAG },
        { 'Z', Z_FLAG }, { 'A', A_FLAG }, { 'P', P_FLAG }, { 'C', C_FLAG }
    };
    char s[1024] = "";
    char ins[160];
    int  prot = full && (is286 || is386); /* the full layout (rp), with bases, limits, and more */

    cpu_386_flags_rebuild();

    if (is386)
        sprintf(s, "EAX=%08X EBX=%08X ECX=%08X EDX=%08X \nESP=%08X EBP=%08X ESI=%08X EDI=%08X \n",
                EAX, EBX, ECX, EDX, ESP, EBP, ESI, EDI);
    else
        sprintf(s, "AX=%04X BX=%04X CX=%04X DX=%04X SP=%04X BP=%04X SI=%04X DI=%04X \n",
                AX, BX, CX, DX, SP, BP, SI, DI);

    idw_cat_seg(s, "SS", &cpu_state.seg_ss, prot);
    strcat(s, " ");
    idw_cat_seg(s, "DS", &cpu_state.seg_ds, prot);
    strcat(s, " ");
    idw_cat_seg(s, "ES", &cpu_state.seg_es, prot);
    strcat(s, " ");

    if (prot) {
        char tr_s[16];
        char a20_s[16];

        sprintf(tr_s, "%sTR=%04X", is386 ? "" : "\n", tr.seg);
        sprintf(a20_s, "A20=%s ", mem_a20_state ? "ON" : "OFF");
        if (!is386) {
            strcat(s, a20_s);
            a20_s[0] = '\0';
        }
        strcat(s, "\n");
        idw_cat_seg(s, "CS", &cpu_state.seg_cs, prot);
        strcat(s, " ");
        if (is386) {
            strcat(a20_s, "\n");
            idw_cat_seg(s, "FS", &cpu_state.seg_fs, prot);
            strcat(s, " ");
            idw_cat_seg(s, "GS", &cpu_state.seg_gs, prot);
            strcat(s, "\n");
        }
        idw_cat_dtr(s, "LD", &ldt, 1);
        idw_cat_dtr(s, "GD", &gdt, 0);
        idw_cat_dtr(s, "ID", &idt, 0);
        sprintf(s + strlen(s), "%s %s", tr_s, a20_s);
        if (is386)
            sprintf(s + strlen(s), "CR0=%08X CR2=%08X CR3=%08X ", cr0, cr2, cr3);
        else
            sprintf(s + strlen(s), "MS=%04X ", msw);
    } else if (is386) {
        idw_cat_seg(s, "FS", &cpu_state.seg_fs, prot);
        strcat(s, " ");
        idw_cat_seg(s, "GS", &cpu_state.seg_gs, prot);
        strcat(s, " ");
    }

    if (is386)
        sprintf(s + strlen(s), "PS=%08X ", cpu_state.flags | ((uint32_t) cpu_state.eflags << 16));
    else
        sprintf(s + strlen(s), "PS=%04X ", cpu_state.flags);
    for (size_t i = 0; i < sizeof(flags) / sizeof(flags[0]); i++)
        sprintf(s + strlen(s), "%c%d ", flags[i].name, !!(cpu_state.flags & flags[i].bit));

    idw_next_line(ins, sizeof(ins), counts);
    idw_printf("%s\n%s\n", s, ins);
}

/* Note the counts that the next stop's register dump will report against. */
static void
idw_resume(void)
{
    idw_run_ops = idw_instructions;
    idw_run_tsc = tsc;
}

/* Set by idw_parse_value() if the value involved a 32-bit register or a number above FFFF. */
static int idw_value_wide;

/* The value of a register named like PCjs's REGS (eg, ax, al, eax, cs, ip); returns 0 if unknown. */
static int
idw_reg_value(const char *name, uint32_t *val)
{
    static const char *r16[] = { "ax", "cx", "dx", "bx", "sp", "bp", "si", "di" };
    static const char *r8[]  = { "al", "cl", "dl", "bl", "ah", "ch", "dh", "bh" };

    for (int i = 0; i < 8; i++) {
        if (!strcmp(name, r16[i])) {
            *val = cpu_state.regs[i].w;
            return 1;
        }
        if (is386 && (name[0] == 'e') && !strcmp(name + 1, r16[i])) {
            *val           = cpu_state.regs[i].l;
            idw_value_wide = 1;
            return 1;
        }
        if (!strcmp(name, r8[i])) {
            *val = (i < 4) ? cpu_state.regs[i].b.l : cpu_state.regs[i - 4].b.h;
            return 1;
        }
    }
    for (size_t i = 0; i < sizeof(idw_segregs) / sizeof(idw_segregs[0]); i++) {
        if (!strcmp(name, idw_segregs[i].name) && (is386 || (i < 4))) {
            *val = idw_segregs[i].seg->seg;
            return 1;
        }
    }
    if (!strcmp(name, "ip") || (is386 && !strcmp(name, "eip"))) {
        *val = (name[0] == 'e') ? cpu_state.pc : (cpu_state.pc & 0xffff);
        idw_value_wide |= (name[0] == 'e');
        return 1;
    }
    return 0;
}

/*
 * Expressions, parsed by recursive descent with C's operators and precedence:
 * parentheses, unary - ~ +, then * / %, + -, << >>, &, ^, and |. Values are 32-bit
 * numbers (hex unless followed by a period, eg, 16.) and register names.
 */
typedef struct {
    const char *start;
    const char *p;
    int         error;
} idw_expr_t;

static uint32_t idw_expr_or(idw_expr_t *e);

static void
idw_expr_fail(idw_expr_t *e, const char *msg)
{
    if (!e->error)
        idw_printf(msg, e->p);
    e->error = 1;
}

/* A number, register name, parenthesized expression, or unary operator applied to one. */
static uint32_t
idw_expr_unary(idw_expr_t *e)
{
    char     term[16];
    char    *end;
    size_t   n = 0;
    uint32_t v;

    if (e->error)
        return 0;

    switch (*e->p) {
        case '-':
            e->p++;
            return -idw_expr_unary(e);
        case '+':
            e->p++;
            return idw_expr_unary(e);
        case '~':
            e->p++;
            return ~idw_expr_unary(e);
        case '(':
            e->p++;
            v = idw_expr_or(e);
            if (*e->p != ')')
                idw_expr_fail(e, *e->p ? "Missing ')' at '%s'\n" : "Missing ')'\n");
            else
                e->p++;
            return v;
        default:
            break;
    }

    /* A number (with an optional trailing period) or a register name. */
    while (isalnum((unsigned char) e->p[n]))
        n++;
    if (e->p[n] == '.')
        n++;
    if (!n || (n >= sizeof(term))) {
        idw_expr_fail(e, *e->p ? "Bad value '%s'\n" : "Missing value\n");
        return 0;
    }
    memcpy(term, e->p, n);
    term[n] = '\0';

    /* Numbers are hex, unless followed by a period (eg, 100.), which makes them decimal. */
    v = strtoul(term, &end, ((n > 1) && (term[n - 1] == '.')) ? 10 : 16);
    if ((end != term) && ((*end == '\0') || ((*end == '.') && (end[1] == '\0'))))
        idw_value_wide |= (v > 0xffff);
    else if (!idw_reg_value(term, &v)) {
        idw_printf("Unknown value '%s'\n", term);
        e->error = 1;
        return 0;
    }

    e->p += n;
    return v;
}

static uint32_t
idw_expr_mul(idw_expr_t *e)
{
    uint32_t v = idw_expr_unary(e);

    while (!e->error && ((*e->p == '*') || (*e->p == '/') || (*e->p == '%'))) {
        char     op  = *e->p++;
        uint32_t rhs = idw_expr_unary(e);
        if (op == '*') {
            v *= rhs;
        } else if (!rhs) {
            idw_expr_fail(e, "Division by zero\n");
        } else {
            v = (op == '/') ? (v / rhs) : (v % rhs);
        }
    }
    return v;
}

static uint32_t
idw_expr_add(idw_expr_t *e)
{
    uint32_t v = idw_expr_mul(e);

    while (!e->error && ((*e->p == '+') || (*e->p == '-'))) {
        char op = *e->p++;
        v       = (op == '+') ? (v + idw_expr_mul(e)) : (v - idw_expr_mul(e));
    }
    return v;
}

static uint32_t
idw_expr_shift(idw_expr_t *e)
{
    uint32_t v = idw_expr_add(e);

    while (!e->error && (((e->p[0] == '<') && (e->p[1] == '<')) || ((e->p[0] == '>') && (e->p[1] == '>')))) {
        char     op  = e->p[0];
        uint32_t rhs;
        e->p += 2;
        rhs = idw_expr_add(e);
        v   = (rhs > 31) ? 0 : ((op == '<') ? (v << rhs) : (v >> rhs));
    }
    return v;
}

static uint32_t
idw_expr_and(idw_expr_t *e)
{
    uint32_t v = idw_expr_shift(e);

    while (!e->error && (*e->p == '&')) {
        e->p++;
        v &= idw_expr_shift(e);
    }
    return v;
}

static uint32_t
idw_expr_xor(idw_expr_t *e)
{
    uint32_t v = idw_expr_and(e);

    while (!e->error && (*e->p == '^')) {
        e->p++;
        v ^= idw_expr_and(e);
    }
    return v;
}

static uint32_t
idw_expr_or(idw_expr_t *e)
{
    uint32_t v = idw_expr_xor(e);

    while (!e->error && (*e->p == '|')) {
        e->p++;
        v |= idw_expr_xor(e);
    }
    return v;
}

/* Evaluate an expression (see above); returns 0 and prints a message on failure. */
static int
idw_parse_value(const char *s, uint32_t *val)
{
    idw_expr_t e = { s, s, 0 };
    uint32_t   v;

    idw_value_wide = 0;
    v              = idw_expr_or(&e);
    if (!e.error && *e.p)
        idw_expr_fail(&e, (*e.p == ')') ? "Unexpected ')' at '%s'\n" : "Bad value '%s'\n");
    if (e.error)
        return 0;

    *val = v;
    return 1;
}

/*
 * Parse an address the way PCjs's parseAddr() does: an optional &, # or % prefix,
 * then off or seg:off. A bare off keeps the segment of the default address.
 */
static int
idw_parse_addr(char *arg, const idw_addr_t *def, idw_addr_t *a)
{
    char    *colon;
    uint32_t val;

    *a = *def;

    if ((*arg == '&') || (*arg == '#') || (*arg == '%')) {
        a->type = *arg++;
        if (a->type == '%')
            a->sel = 0;
    } else if (strchr(arg, ':')) {
        a->type = ((msw & 1) && !(cpu_state.eflags & VM_FLAG)) ? '#' : '&';
    }

    colon = strchr(arg, ':');
    if (colon) {
        if (a->type == '%') {
            idw_output("A linear address can't have a segment\n");
            return 0;
        }
        *colon++ = '\0';
        if (!idw_parse_value(arg, &val))
            return 0;
        a->sel = (uint16_t) val;
        arg    = colon;
    }
    if (!idw_parse_value(arg, &a->off))
        return 0;

    return 1;
}

/* The base and default size of an address's segment; returns 0 and prints a message on failure. */
static int
idw_seg_base(const idw_addr_t *a, uint32_t *base, int *size32)
{
    const x86seg *table = (a->sel & 4) ? &ldt : &gdt;
    uint32_t      desc  = table->base + (a->sel & ~7);
    int           b[8];

    *size32 = 0;
    if (a->type == '%') {
        *base = 0;
        return 1;
    }
    if (a->type == '&') {
        *base = (uint32_t) a->sel << 4;
        return 1;
    }
    if (a->sel == CS) {
        *base   = cs;
        *size32 = !!use32;
        return 1;
    }

    /* A selector that's loaded in a segment register uses that register's cached base. */
    for (size_t i = 0; i < sizeof(idw_segregs) / sizeof(idw_segregs[0]); i++) {
        if (a->sel == idw_segregs[i].seg->seg) {
            *base   = idw_segregs[i].seg->base;
            *size32 = is386 && (idw_segregs[i].seg->ar_high & 0x40);
            return 1;
        }
    }

    /* Otherwise, look the selector up in the GDT or LDT. */
    if ((a->sel & ~7) + 7 > table->limit) {
        idw_printf("Selector %04X is outside the %s\n", a->sel, (a->sel & 4) ? "LDT" : "GDT");
        return 0;
    }
    for (int i = 0; i < 8; i++) {
        if ((b[i] = idw_read_byte(desc + i)) < 0) {
            idw_printf("Can't read the descriptor for selector %04X\n", a->sel);
            return 0;
        }
    }
    *base = b[2] | (b[3] << 8) | (b[4] << 16);
    if (is386) {
        *base |= (uint32_t) b[7] << 24;
        *size32 = !!(b[6] & 0x40);
    }
    return 1;
}

static void
idw_unassemble(int argc, char **argv)
{
    idw_addr_t a   = idw_next_code;
    int        n   = 8;
    uint32_t   cb  = 0x100;
    uint32_t   base;
    uint32_t   next;
    int        size32;
    char       line[160];

    if ((argc > 1) && !idw_parse_addr(argv[1], &idw_next_code, &a))
        return;

    /* With an end address (inclusive), unassemble through it, up to 4K; with l<n>, unassemble n instructions. */
    if (argc > 2) {
        if (argv[2][0] == 'l') {
            uint32_t lines;
            if (!idw_parse_value(argv[2][1] ? (argv[2] + 1) : ((argc > 3) ? argv[3] : ""), &lines))
                return;
            n  = (lines > 0x1000) ? 0x1000 : (int) lines;
            cb = 0xffffffff;
        } else {
            idw_addr_t end;
            if (!idw_parse_addr(argv[2], &a, &end))
                return;
            if (end.off < a.off) {
                idw_output("The end address is before the start address\n");
                return;
            }
            cb = end.off - a.off + 1;
            if (cb > 0x1000)
                cb = 0x1000;
            n = -1;
        }
    }

    if (!idw_seg_base(&a, &base, &size32))
        return;

    while ((cb > 0) && n--) {
        next = idw_disasm(line, sizeof(line), a.type, a.sel, base, a.off, size32);
        idw_printf("%s\n", line);
        cb    = (next - a.off > cb) ? 0 : (cb - (next - a.off));
        a.off = next;
    }

    idw_next_code = a;
}

/* Format an address like PCjs's toHexAddr(); eg, &0040:0000, #0008:00001000, or %FFFF0. */
static void
idw_format_addr(char *s, const idw_addr_t *a, int size32)
{
    if (a->type == '%')
        sprintf(s, "%%%0*X", (a->off <= 0xff) ? 2 : ((a->off <= 0xffff) ? 4 : 8), a->off);
    else
        sprintf(s, "%c%04X:%0*X", a->type, a->sel, ((a->off & ~0xffff) || size32) ? 8 : 4, a->off);
}

/*
 * Dump memory like PCjs's doDump(): d [addr [end]], or d [addr] l<len>, where size is
 * 1, 2 or 4 (db, dw or dd), or 0 for d, and len counts units of that size; a bare d
 * continues where the last dump ended, in the same format, and d with an address is db.
 */
static void
idw_dump(int argc, char **argv, int size)
{
    idw_addr_t a   = idw_next_data;
    uint32_t   len = IDW_DUMP_LEN;
    int        width;
    uint32_t   base;
    uint32_t   mask;
    int        size32;
    char       line[128];
    char       chars[17];

    if (!size)
        size = (argc > 1) ? 1 : idw_dump_size;
    width = size * 2 + ((size == 1) ? 1 : 2); /* each unit's column width */

    if ((argc > 1) && !idw_parse_addr(argv[1], &idw_next_data, &a))
        return;

    if (argc > 2) {
        if (argv[2][0] == 'l') {
            if (!idw_parse_value(argv[2][1] ? (argv[2] + 1) : ((argc > 3) ? argv[3] : ""), &len))
                return;
            len *= size;
        } else {
            idw_addr_t end;
            if (!idw_parse_addr(argv[2], &a, &end))
                return;
            if (end.off < a.off) {
                idw_output("The end address is before the start address\n");
                return;
            }
            len = end.off - a.off + 1;
        }
        if (!len)
            len = IDW_DUMP_LEN;
        if (len > IDW_DUMP_MAX)
            len = IDW_DUMP_MAX;
    }
    len = (len + size - 1) & ~(uint32_t) (size - 1);

    if (!idw_seg_base(&a, &base, &size32))
        return;
    mask = (size32 || (a.type == '%')) ? 0xffffffff : 0xffff;

    while (len) {
        uint32_t n = (len < 16) ? len : 16;

        idw_format_addr(line, &a, size32);
        strcat(line, "  ");
        for (uint32_t i = 0; i < n; i += size) {
            uint32_t data  = 0;
            int      fault = 0;

            for (int j = 0; j < size; j++) {
                int val = idw_read_byte(base + ((a.off + i + j) & mask));
                if (val < 0) {
                    fault            = 1;
                    chars[i + j]     = '?';
                } else {
                    data |= (uint32_t) val << (j * 8);
                    chars[i + j] = ((val >= 0x20) && (val < 0x7f)) ? (char) val : '.';
                }
            }
            if (fault)
                sprintf(line + strlen(line), "%.*s", size * 2, "????????");
            else
                sprintf(line + strlen(line), "%0*X", size * 2, data);
            strcat(line, (size > 1) ? "  " : ((i == 7) ? "-" : " "));
        }
        chars[n] = '\0';

        /* Pad a short last line so its characters line up. */
        idw_printf("%s%*s %s\n", line, (int) ((16 - n) / size) * width, "", chars);

        a.off = (a.off + n) & mask;
        len -= n;
    }

    idw_next_data     = a;
    idw_next_data_set = 1;
    idw_dump_size     = size;
}

static const char *
idw_break_name(int type)
{
    return (type == GDBSTUB_BREAK_HW) ? "bp" : ((type == GDBSTUB_BREAK_RWATCH) ? "br" : "bw");
}

/* Print a breakpoint like PCjs's printBreakpoint(), with its number; eg, "bp 1 &F000:E05B set" (or "bt" for p's). */
static void
idw_print_break(int i, const char *action)
{
    char addr[32];

    idw_format_addr(addr, &idw_breaks[i].addr, idw_breaks[i].size32);
    idw_printf("%s %d %s%s%s\n", idw_breaks[i].temp ? "bt" : idw_break_name(idw_breaks[i].type), i, addr,
               action ? " " : "", action ? action : "");
}

/* bp, br or bw addr: a bare off is relative to the next code address for bp, or the next data address otherwise. */
static void
idw_break_set(int argc, char **argv, int type)
{
    idw_addr_t   a;
    idw_break_t *b;
    uint32_t     base;
    uint32_t     linear;
    int          size32;
    int          slot = 0;

    if (argc < 2) {
        idw_output("Missing breakpoint address\n");
        return;
    }
    if (!idw_parse_addr(argv[1], (type == GDBSTUB_BREAK_HW) ? &idw_next_code : &idw_next_data, &a)
        || !idw_seg_base(&a, &base, &size32))
        return;
    linear = base + a.off;

    for (int i = 1; i <= IDW_BREAK_MAX; i++) {
        if ((idw_breaks[i].type == type) && (idw_breaks[i].linear == linear)) {
            idw_print_break(i, "exists");
            return;
        }
        if (!idw_breaks[i].type && !slot)
            slot = i;
    }
    if (!slot) {
        idw_printf("All %d breakpoints are in use; clear one with bc\n", IDW_BREAK_MAX);
        return;
    }

    b         = &idw_breaks[slot];
    b->type   = type;
    b->addr   = a;
    b->size32 = size32;
    b->linear = linear;
    b->owned  = gdbstub_idw_breakpoint(1, type, linear);
    idw_print_break(slot, "set");
}

static void
idw_break_remove(int i)
{
    if (idw_breaks[i].owned)
        gdbstub_idw_breakpoint(0, idw_breaks[i].type, idw_breaks[i].linear);
    memset(&idw_breaks[i], 0, sizeof(idw_breaks[i]));
}

/* bc n clears breakpoint n, and bc * clears them all. */
static void
idw_break_clear(int argc, char **argv)
{
    char *end;
    int   i;

    if (argc < 2) {
        idw_printf("Missing breakpoint number (1-%d, or * for all)\n", IDW_BREAK_MAX);
        return;
    }
    if (!strcmp(argv[1], "*")) {
        for (i = 1; i <= IDW_BREAK_MAX; i++) {
            if (idw_breaks[i].type)
                idw_break_remove(i);
        }
        idw_output("All breakpoints cleared\n");
        return;
    }

    i = (int) strtol(argv[1], &end, 10);
    if ((end == argv[1]) || *end || (i < 1) || (i > IDW_BREAK_MAX)) {
        idw_printf("Bad breakpoint number '%s'\n", argv[1]);
        return;
    }
    if (!idw_breaks[i].type) {
        idw_printf("No breakpoint %d\n", i);
        return;
    }
    idw_print_break(i, "cleared");
    idw_break_remove(i);
}

static void
idw_break_list(void)
{
    int count = 0;

    for (int i = 1; i <= IDW_BREAK_MAX; i++) {
        if (idw_breaks[i].type) {
            idw_print_break(i, NULL);
            count++;
        }
    }
    if (!count)
        idw_output("(no breakpoints)\n");
}

/*
 * Set a temporary breakpoint at a, which is cleared when the CPU next halts; an existing bp
 * there will do instead. Returns 0 and prints a message if there's no room for one.
 */
static int
idw_break_temp(const idw_addr_t *a, uint32_t base, int size32)
{
    idw_break_t *b;
    int          slot = 0;

    for (int i = 1; i <= IDW_BREAK_MAX; i++) {
        if ((idw_breaks[i].type == GDBSTUB_BREAK_HW) && (idw_breaks[i].linear == base + a->off))
            return 1;
        if (!idw_breaks[i].type && !slot)
            slot = i;
    }
    if (!slot) {
        idw_printf("All %d breakpoints are in use; clear one with bc\n", IDW_BREAK_MAX);
        return 0;
    }

    b         = &idw_breaks[slot];
    b->type   = GDBSTUB_BREAK_HW;
    b->temp   = 1;
    b->addr   = *a;
    b->size32 = size32;
    b->linear = base + a->off;
    b->owned  = gdbstub_idw_breakpoint(1, GDBSTUB_BREAK_HW, b->linear);
    return 1;
}

/* g [addr]: run, stopping at addr (by way of a temporary breakpoint) if given. */
static void
idw_go(int argc, char **argv)
{
    idw_addr_t a;
    uint32_t   base;
    int        size32;

    if (argc > 1) {
        if (!idw_parse_addr(argv[1], &idw_next_code, &a) || !idw_seg_base(&a, &base, &size32)
            || !idw_break_temp(&a, base, size32))
            return;
    }

    if (!idw_stopped()) {
        idw_output("(already running)\n");
        return;
    }
    idw_output("(running)\n");
    idw_resume();
    idw_was_stopped = 0;
    gdbstub_step    = GDBSTUB_EXEC;
}

/*
 * p: like t, except that a CALL, INT, LOOP or REP string instruction runs to completion,
 * by way of a temporary breakpoint on the instruction that follows it.
 */
static void
idw_ptrace(int brief)
{
    idw_addr_t a;
    uint32_t   base;
    uint32_t   next;
    int        size32;

    idw_brief = brief;
    a.type    = ((msw & 1) && !(cpu_state.eflags & VM_FLAG)) ? '#' : '&';
    a.sel     = CS;
    if (!idw_stopped() || !idw_seg_base(&a, &base, &size32) || !idw_step_over(base, cpu_state.pc, size32, &next)) {
        if (idw_stopped())
            idw_resume();
        idw_tracing       = idw_stopped();
        idw_was_stopped   = 0;
        gdbstub_step      = GDBSTUB_SSTEP;
        gdbstub_next_asap = 1;
        return;
    }

    a.off = next;
    if (!idw_break_temp(&a, base, size32))
        return;
    idw_stepping    = 1;
    idw_resume();
    idw_was_stopped = 0;
    gdbstub_step    = GDBSTUB_EXEC;
}

/* Clear any temporary breakpoint, returning 1 if it's where the CPU just stopped. */
static int
idw_clear_temp(void)
{
    int hit = 0;

    for (int i = 1; i <= IDW_BREAK_MAX; i++) {
        if (idw_breaks[i].temp) {
            hit |= (idw_stop_type == GDBSTUB_BREAK_HW) && (idw_breaks[i].linear == idw_stop_addr);
            idw_break_remove(i);
        }
    }
    return hit;
}

/* If the CPU stopped at one of our breakpoints, say which. */
static void
idw_report_hit(void)
{
    for (int i = 1; i <= IDW_BREAK_MAX; i++) {
        if ((idw_breaks[i].type == idw_stop_type) && (idw_breaks[i].linear == idw_stop_addr)) {
            idw_print_break(i, "hit");
            return;
        }
    }
}

/*
 * ? expr: print the value in hex and, like PCjs, in decimal with a trailing period; eg,
 * "E061 (57441.)". It's 32 bits in a 32-bit code segment, or if expr involves a 32-bit
 * register or a number above FFFF, and 16 bits otherwise.
 */
static void
idw_print_value(const char *expr)
{
    uint32_t val;
    int      wide;

    if (!idw_parse_value(expr, &val))
        return;
    wide = idw_value_wide || ((msw & 1) && !(cpu_state.eflags & VM_FLAG) && use32);
    if (!wide)
        val &= 0xffff;
    idw_printf("%0*X (%u.)\n", wide ? 8 : 4, val, val);
}

/* Until something sets them, a bare u starts at CS:IP and a bare d at DS:0. */
static void
idw_init_addrs(void)
{
    char type = ((msw & 1) && !(cpu_state.eflags & VM_FLAG)) ? '#' : '&';

    if (!idw_next_code_set) {
        idw_next_code.type = type;
        idw_next_code.sel  = CS;
        idw_next_code.off  = cpu_state.pc;
        idw_next_code_set  = 1;
    }
    if (!idw_next_data_set) {
        idw_next_data.type = type;
        idw_next_data.sel  = DS;
        idw_next_data.off  = 0;
        idw_next_data_set  = 1;
    }
}

static void
idw_execute(char *line)
{
    /* Commands that take parameters, which may follow them without a space (eg, "bc1" or "d0"). */
    static const char *const param_cmds[] = { "bc", "bp", "br", "bw", "d", "db", "dd", "dw", "g", "u" };
    char                     cmd[4];
    char                    *argv[5];
    int                      argc = 0;

    /* ? followed by an expression prints its value; spaces within it are ignored. */
    {
        char   expr[IDW_LINE_LEN];
        size_t n = 0;
        char  *p = line;

        while (isspace((unsigned char) *p))
            p++;
        if (*p == '?') {
            for (p++; *p; p++) {
                if (!isspace((unsigned char) *p))
                    expr[n++] = (char) tolower((unsigned char) *p);
            }
            expr[n] = '\0';
            if (n) {
                idw_print_value(expr);
                return;
            }
        }
    }

    /* Split into lowercase words. */
    for (char *p = line; *p && (argc < 4);) {
        while (isspace((unsigned char) *p))
            *p++ = '\0';
        if (*p)
            argv[argc++] = p;
        while (*p && !isspace((unsigned char) *p)) {
            *p = (char) tolower((unsigned char) *p);
            p++;
        }
    }

    if (!argc)
        return;

    if (!strcmp(argv[0], "?")) {
        idw_help();
        return;
    }

    /* A command followed by ? (eg, "d?" or "d ?") shows details for it. */
    if ((argc > 1) && !strcmp(argv[1], "?")) {
        if (!idw_help_command(argv[0]))
            idw_printf("Unknown command '%s'; type ? for help\n", argv[0]);
        return;
    }
    if ((strlen(argv[0]) > 1) && (argv[0][strlen(argv[0]) - 1] == '?')) {
        argv[0][strlen(argv[0]) - 1] = '\0';
        if (!idw_help_command(argv[0]))
            idw_printf("Unknown command '%s'; type ? for help\n", argv[0]);
        return;
    }

    /* Split a parameter off a command it runs into, using the longest command that matches. */
    {
        size_t best = 0;
        for (size_t i = 0; i < sizeof(param_cmds) / sizeof(param_cmds[0]); i++) {
            size_t len = strlen(param_cmds[i]);
            if ((len > best) && !strncmp(argv[0], param_cmds[i], len))
                best = len;
        }
        if (best && argv[0][best]) {
            memmove(&argv[2], &argv[1], (argc - 1) * sizeof(argv[0]));
            argv[1] = argv[0] + best;
            memcpy(cmd, argv[0], best);
            cmd[best] = '\0';
            argv[0]   = cmd;
            argc++;
        }
    }

    if (!strcmp(argv[0], "h")) {
        if (gdbstub_step == GDBSTUB_EXEC)
            gdbstub_step = GDBSTUB_BREAK;
        else
            idw_output("(already halted)\n");
        return;
    }

    /*
     * Commands run between the CPU's execution slices, so r, d and u work while it runs,
     * showing its state as of that moment; t while running halts after one instruction.
     */
    idw_init_addrs();

    if (!strcmp(argv[0], "g")) {
        idw_go(argc, argv);
    } else if (!strcmp(argv[0], "t") || !strcmp(argv[0], "tr")) {
        idw_brief = !argv[0][1];
        if (idw_stopped())
            idw_resume();
        idw_tracing       = idw_stopped();
        idw_was_stopped   = 0;
        gdbstub_step      = GDBSTUB_SSTEP;
        gdbstub_next_asap = 1;
    } else if (!strcmp(argv[0], "r") || !strcmp(argv[0], "rp")) {
        idw_regs_full = (argv[0][1] == 'p');
        idw_regs(0, idw_regs_full);
    } else if (!strcmp(argv[0], "d")) {
        idw_dump(argc, argv, 0);
    } else if (!strcmp(argv[0], "db")) {
        idw_dump(argc, argv, 1);
    } else if (!strcmp(argv[0], "dw")) {
        idw_dump(argc, argv, 2);
    } else if (!strcmp(argv[0], "dd")) {
        idw_dump(argc, argv, 4);
    } else if (!strcmp(argv[0], "u")) {
        idw_unassemble(argc, argv);
    } else if (!strcmp(argv[0], "p") || !strcmp(argv[0], "pr")) {
        idw_ptrace(!argv[0][1]);
    } else if (!strcmp(argv[0], "bp")) {
        idw_break_set(argc, argv, GDBSTUB_BREAK_HW);
    } else if (!strcmp(argv[0], "br")) {
        idw_break_set(argc, argv, GDBSTUB_BREAK_RWATCH);
    } else if (!strcmp(argv[0], "bw")) {
        idw_break_set(argc, argv, GDBSTUB_BREAK_WWATCH);
    } else if (!strcmp(argv[0], "bc")) {
        idw_break_clear(argc, argv);
    } else if (!strcmp(argv[0], "bl")) {
        idw_break_list();
    } else if (!strcmp(argv[0], "b")) {
        idw_help_command("b");
    } else {
        idw_printf("Unknown command '%s'; type ? for help\n", argv[0]);
        return;
    }

    /* After r, d or u, note that what was shown may already be changing. */
    if ((argv[0][0] == 'r') || (argv[0][0] == 'd') || (argv[0][0] == 'u')) {
        if (!idw_stopped())
            idw_output("(running)\n");
    }
}

void
idw_break(void)
{
    if (!idw_mutex)
        return;

    thread_wait_mutex(idw_mutex);
    idw_break_pending = 1;
    thread_release_mutex(idw_mutex);

    gdbstub_next_asap = 1;
}

void
idw_command(const char *line)
{
    int full;

    if (!idw_mutex) {
        idw_output("The emulated machine hasn't started yet\n");
        return;
    }

    thread_wait_mutex(idw_mutex);
    full = (idw_queue_count == IDW_QUEUE_LEN);
    if (!full) {
        char *slot = idw_queue[(idw_queue_head + idw_queue_count++) % IDW_QUEUE_LEN];
        strncpy(slot, line, IDW_LINE_LEN - 1);
        slot[IDW_LINE_LEN - 1] = '\0';
    }
    thread_release_mutex(idw_mutex);

    if (full)
        idw_output("Too many commands pending; try again\n");
    else
        gdbstub_next_asap = 1;
}

void
idw_init(void)
{
    if (!idw_mutex)
        idw_mutex = thread_create_mutex();
}

/* Called by the GDB stub when the CPU stops, before it forgets why. */
void
idw_note_stop(int type, uint32_t addr)
{
    idw_stop_type = type;
    idw_stop_addr = addr;
}

void
idw_process(void)
{
    char line[IDW_LINE_LEN];
    int  brk;
    int  have_line;

    thread_wait_mutex(idw_mutex);
    brk               = idw_break_pending;
    idw_break_pending = 0;
    thread_release_mutex(idw_mutex);

    if (brk && (gdbstub_step == GDBSTUB_EXEC))
        gdbstub_step = GDBSTUB_BREAK;

    /* Announce each halt, whether it came from h, Ctrl-C, a trace or a GDB client. */
    if (idw_stopped() && !idw_was_stopped) {
        /* A completed t or p is a step, not a change in the machine's status. */
        int temp_hit  = idw_clear_temp();
        int step_done = idw_tracing || (temp_hit && idw_stepping);
        if (!step_done) {
            if (!temp_hit)
                idw_report_hit();
            idw_output("(halted)\n");
        }
        /* A completed t or p shows just the next instruction; anything else, all the registers. */
        if (step_done && idw_brief) {
            char ins[160];
            idw_next_line(ins, sizeof(ins), 1);
            idw_printf("%s\n", ins);
        } else {
            idw_regs(1, idw_regs_full);
        }
        idw_tracing   = 0;
        idw_stepping  = 0;
        idw_brief     = 0;
        idw_stop_type = 0;
    }
    idw_was_stopped = idw_stopped();

    do {
        thread_wait_mutex(idw_mutex);
        have_line = (idw_queue_count > 0);
        if (have_line) {
            strcpy(line, idw_queue[idw_queue_head]);
            idw_queue_head = (idw_queue_head + 1) % IDW_QUEUE_LEN;
            idw_queue_count--;
        }
        thread_release_mutex(idw_mutex);

        if (have_line)
            idw_execute(line);
    } while (have_line);
}
