/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Internal Debugger Window (IDW) disassembler.
 *
 *          A port of getInstruction() and its helpers from the PCjs x86
 *          debugger (machines/pcx86/modules/v2/debugger.js), using tables
 *          generated from that same file (see idw_distab.h).
 */
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <86box/86box.h>
#include "cpu.h"
#include <86box/idw.h>
#include "idw_distab.h"

#define OPCODE_OS    0x66
#define OPCODE_AS    0x67
#define OPCODE_LOCK  0xF0
#define OPCODE_CMPSB 0xA6
#define OPCODE_CMPSW 0xA7
#define OPCODE_SCASB 0xAE
#define OPCODE_SCASW 0xAF

typedef struct {
    uint32_t base;     /* segment base, for fetching bytes */
    uint32_t off;
    uint32_t off_mask; /* 0xffff for 16-bit code segments */
    int      data32;
    int      addr32;
} dis_addr_t;

static const int dis_cpus[] = { 8086, 80186, 80286, 80386 };

static int
dis_cpu_model(void)
{
    if (is386)
        return IDW_CPU_80386;
    if (is286)
        return IDW_CPU_80286;
    if (is_nec || (cpu_s->cpu_type >= CPU_188))
        return IDW_CPU_80186;
    return IDW_CPU_8086;
}

/* Append n in hex, like StrLib.toHex(): cch digits, or 2, 4 or 8 by magnitude when cch is 0. */
static void
dis_cat_hex(char *s, int64_t n, int cch)
{
    if (!cch) {
        int64_t v = (n < 0) ? -n : n;
        cch       = (v <= 0xff) ? 2 : ((v <= 0xffff) ? 4 : 8);
    }
    if (n < 0)
        n += (int64_t) 1 << (4 * cch);
    sprintf(s + strlen(s), "%0*" PRIX64, cch, (uint64_t) n);
}

static uint8_t
dis_get_byte(dis_addr_t *a)
{
    int b = idw_read_byte(a->base + a->off);

    a->off = (a->off + 1) & a->off_mask;
    return (b < 0) ? 0xff : (uint8_t) b;
}

static uint32_t
dis_get_short(dis_addr_t *a)
{
    uint32_t w = dis_get_byte(a);

    return w | (dis_get_byte(a) << 8);
}

static int32_t
dis_get_long(dis_addr_t *a)
{
    uint32_t l = dis_get_short(a);

    return (int32_t) (l | (dis_get_short(a) << 16));
}

static int64_t
dis_get_word(dis_addr_t *a)
{
    return a->data32 ? dis_get_long(a) : dis_get_short(a);
}

static const char *
dis_reg_operand(int reg, uint16_t type, const dis_addr_t *a, int model)
{
    int mode = type & IDW_TYPE_MODE;

    if (mode == IDW_TYPE_SEGREG) {
        if ((reg > IDW_REG_GS) || ((reg >= IDW_REG_FS) && (model < IDW_CPU_80386)))
            return "??";
        reg += IDW_REG_SEG;
    } else if (mode == IDW_TYPE_CTLREG) {
        reg += IDW_REG_CR0;
    } else if (mode == IDW_TYPE_DBGREG) {
        reg += IDW_REG_DR0;
    } else if (mode == IDW_TYPE_TSTREG) {
        reg += IDW_REG_TR0;
    } else {
        int size = type & IDW_TYPE_SIZE;
        if (size >= IDW_TYPE_SHORT) {
            if (reg < IDW_REG_AX)
                reg += IDW_REG_AX - IDW_REG_AL;
            if ((size == IDW_TYPE_LONG) || ((size == IDW_TYPE_WORD) && a->data32))
                reg += IDW_REG_EAX - IDW_REG_AX;
        }
    }

    return idw_reg_names[reg];
}

static void
dis_sib_operand(char *s, int mod, dis_addr_t *a)
{
    int sib   = dis_get_byte(a);
    int scale = sib >> 6;
    int index = (sib >> 3) & 0x7;
    int base  = sib & 0x7;

    /* Unless mod is zero AND base is 5, there's always a base register. */
    if (mod || (base != 5))
        strcpy(s, idw_rm_names[base + 8]);
    if (index != 4) {
        if (*s)
            strcat(s, "+");
        strcat(s, idw_rm_names[index + 8]);
        if (scale)
            sprintf(s + strlen(s), "*%d", 1 << scale);
    }
    /* If mod is zero AND base is 5, there's a 32-bit displacement instead of a base register. */
    if (!mod && (base == 5)) {
        if (*s)
            strcat(s, "+");
        dis_cat_hex(s, dis_get_long(a), 0);
    }
}

static void
dis_modrm_operand(char *s, const char *opcode, const char *segment, int modrm, uint16_t type, int count, dis_addr_t *a, int model)
{
    int  mod = modrm >> 6;
    int  rm  = modrm & 0x7;
    char mem[64] = "";

    if (mod == 3) {
        const char *reg = dis_reg_operand(rm, type, a, model);
        strcpy(s, reg ? reg : "");
        return;
    }

    if (!mod && ((!a->addr32 && (rm == 6)) || (a->addr32 && (rm == 5)))) {
        mod = 2;
    } else {
        if (a->addr32) {
            if (rm != 4)
                rm += 8;
            else
                dis_sib_operand(mem, mod, a);
        }
        if (!*mem)
            strcpy(mem, idw_rm_names[rm]);
    }

    if (mod == 1) {
        int disp = dis_get_byte(a);
        if (!(disp & 0x80)) {
            strcat(mem, "+");
            dis_cat_hex(mem, disp, 2);
        } else {
            strcat(mem, "-");
            dis_cat_hex(mem, -(int8_t) disp, 2);
        }
    } else if (mod == 2) {
        if (*mem)
            strcat(mem, "+");
        if (!a->addr32)
            dis_cat_hex(mem, dis_get_short(a), 4);
        else
            dis_cat_hex(mem, dis_get_long(a), 0);
    }

    sprintf(s, "%s[%s]", segment, mem);

    /* With only one operand, memory operands are prefixed with their size. */
    if (count == 1) {
        int         integer = !strncmp(opcode, "FI", 2);
        const char *size    = "";

        type &= IDW_TYPE_SIZE;
        if (type == IDW_TYPE_WORD)
            type = a->data32 ? IDW_TYPE_LONG : IDW_TYPE_SHORT;
        switch (type) {
            case IDW_TYPE_FARP:
                size = "FAR";
                break;
            case IDW_TYPE_BYTE:
                size = "BYTE";
                break;
            case IDW_TYPE_SHORT:
                size = integer ? "INT16" : "WORD";
                break;
            case IDW_TYPE_LONG:
                size = "DWORD";
                break;
            case IDW_TYPE_SINT:
                size = integer ? "INT32" : "REAL32";
                break;
            case IDW_TYPE_LINT:
                size = integer ? "INT64" : "REAL64";
                break;
            case IDW_TYPE_TREAL:
                size = "REAL80";
                break;
            case IDW_TYPE_BCD80:
                size = "BCD80";
                break;
            default:
                break;
        }
        if (*size) {
            memmove(s + strlen(size) + 1, s, strlen(s) + 1);
            memcpy(s, size, strlen(size));
            s[strlen(size)] = ' ';
        }
    }
}

/* Returns 0 if the operand should be omitted (eg, the 0A that follows AAM and AAD). */
static int
dis_imm_operand(char *s, uint16_t type, dis_addr_t *a, char prefix)
{
    int64_t  off;
    uint32_t sel;

    switch (type & IDW_TYPE_SIZE) {
        case IDW_TYPE_BYTE:
            if (!(type & IDW_TYPE_BOTH)) {
                dis_get_byte(a);
                return 0;
            }
            dis_cat_hex(s, dis_get_byte(a), 2);
            break;
        case IDW_TYPE_SBYTE:
            dis_cat_hex(s, (int8_t) dis_get_byte(a), 0);
            break;
        case IDW_TYPE_WORD:
            if (a->data32) {
                dis_cat_hex(s, dis_get_long(a), 8);
                break;
            }
            /* fallthrough */
        case IDW_TYPE_SHORT:
            dis_cat_hex(s, dis_get_short(a), 4);
            break;
        case IDW_TYPE_FARP:
            off = dis_get_word(a);
            sel = dis_get_short(a);
            sprintf(s, "%c%04X:", prefix, sel);
            dis_cat_hex(s, off, ((off & ~0xffff) || a->addr32) ? 8 : 4);
            break;
        default:
            sprintf(s, "imm(0x%04X)", type);
            break;
    }

    return 1;
}

static const idw_opdesc_t *
dis_fpu_desc(int opcode, int modrm)
{
    int mod     = (modrm >> 6) & 0x3;
    int reg     = (modrm >> 3) & 0x7;
    int rm      = modrm & 0x7;
    int mod_reg = ((mod < 3) ? 0 : 0x30) + reg;

    /* All values >= 0x34 imply mod == 3 and reg >= 4, so shift reg into the high nibble and rm into the low. */
    if (((opcode == 0xD9) || (opcode == 0xDB)) && (mod_reg >= 0x34))
        mod_reg = (reg << 4) | rm;

    return idw_fpu_descs[opcode - 0xD8][mod_reg].valid ? &idw_fpu_descs[opcode - 0xD8][mod_reg] : NULL;
}

/* What dis_instruction() decoded, for idw_step_over(). */
typedef struct {
    int ins;    /* IDW_INS_* value, or -1 for an FPU instruction */
    int opcode; /* the opcode byte (with the second byte in bits 8-15 for 0F opcodes) */
    int rep;    /* 1 if prefixed by REPZ or REPNZ */
} dis_info_t;

static uint32_t
dis_instruction(char *line, size_t size, char type, uint16_t sel, uint32_t base, uint32_t off, int size32, dis_info_t *info)
{
    const char          addr_type = type;
    dis_addr_t          a        = { base, off, (size32 || (type == '%')) ? 0xffffffff : 0xffff, size32, size32 };
    const char *const  *names    = idw_ins_names;
    int                 nnames   = IDW_INS_NAMES_COUNT;
    const char         *prefix   = "";
    const char         *segment  = "";
    const idw_opdesc_t *desc     = NULL;
    int                 model    = dis_cpu_model();
    int                 modrm    = -1;
    int                 type_cpu = -1;
    int                 data_prefix = 0;
    int                 addr_prefix = 0;
    int                 opcode   = 0;
    int                 ins      = 0;
    int                 count;
    char                opname[16];
    char                operands[160] = "";
    char                bytes[64]     = "";
    char                buf[256];
    int                 n;

    /* Incorporate segment, operand size, and address size overrides, ignoring redundant ones. */
    for (int overrides = 8; overrides >= 0; overrides--) {
        opcode = dis_get_byte(&a);
        if (opcode == 0x0F)
            desc = (model >= IDW_CPU_80286) ? &idw_op_desc_0f : ((model >= IDW_CPU_80186) ? &idw_op_desc_undefined : &idw_op_desc_popcs);
        else
            desc = &idw_op_descs[opcode];
        ins = desc->ins;
        if (desc->ops[0] == IDW_TYPE_PREFIX) {
            if (opcode >= OPCODE_LOCK)
                prefix = names[ins];
            else
                segment = names[ins];
        } else if (desc->ops[0] == (IDW_TYPE_PREFIX | IDW_TYPE_80386)) {
            if (model < IDW_CPU_80386)
                break;
            if (opcode == OPCODE_OS) {
                if (!data_prefix) {
                    a.data32    = !a.data32;
                    data_prefix = 1;
                }
            } else if (opcode == OPCODE_AS) {
                if (!addr_prefix) {
                    a.addr32    = !a.addr32;
                    addr_prefix = 1;
                }
            } else {
                segment = names[ins];
            }
        } else {
            break;
        }
    }

    if (ins == IDW_INS_OP0F) {
        int b  = dis_get_byte(&a);
        desc   = &idw_op0f_descs[b];
        opcode |= b << 8;
        ins    = desc->ins;
    }

    if (ins == IDW_INS_ESC) {
        const idw_opdesc_t *fpu;
        modrm = dis_get_byte(&a);
        fpu   = dis_fpu_desc(opcode, modrm);
        if (fpu) {
            names  = idw_fins_names;
            nnames = IDW_FINS_NAMES_COUNT;
            desc   = fpu;
            ins    = desc->ins;
        }
    }

    if (ins >= nnames) {
        const idw_opdesc_t *grp;
        modrm = dis_get_byte(&a);
        grp   = &idw_grp_descs[ins - nnames][(modrm >> 3) & 0x7];
        desc  = grp->valid ? grp : &idw_op_desc_undefined;
        ins   = desc->ins;
    }

    snprintf(opname, sizeof(opname), "%s", names[ins]);
    count = desc->count;

    if (a.data32 && (names == idw_ins_names)) {
        if (ins == IDW_INS_CBW)
            strcpy(opname, "CWDE"); /* sign-extend AX into EAX, instead of AL into AX */
        else if (ins == IDW_INS_CWD)
            strcpy(opname, "CDQ"); /* sign-extend EAX into EDX:EAX, instead of AX into DX:AX */
        else if ((ins >= IDW_INS_POPA) && (ins <= IDW_INS_PUSHA))
            strcat(opname, "D"); /* POPA/POPF/PUSHF/PUSHA become POPAD/POPFD/PUSHFD/PUSHAD */
    }

    /* Suppress operands for string instructions, and add a D suffix as appropriate. */
    if (((opcode >= 0xA4) && (opcode <= OPCODE_CMPSW)) || ((opcode >= 0xAA) && (opcode <= OPCODE_SCASW))) {
        count = 0;
        if (a.data32 && (opname[strlen(opname) - 1] == 'W'))
            opname[strlen(opname) - 1] = 'D';
    }

    for (int i = 0; i < count; i++) {
        uint16_t    type = desc->ops[i];
        int         mode;
        int         tsize;
        char        op[96] = "";
        const char *reg;

        if (type == IDW_TYPE_UNDEF)
            continue;
        if (type_cpu < 0)
            type_cpu = type >> IDW_TYPE_CPU_SHIFT;

        if ((names == idw_ins_names) && (ins == IDW_INS_LOADALL)) {
            if (type_cpu == IDW_CPU_80286)
                strcpy(operands, "[%800]");
            else if (type_cpu == IDW_CPU_80386)
                sprintf(operands, "%s[%sDI]", *segment ? segment : "ES:", a.addr32 ? "E" : "");
        }

        /* A prefix that isn't one on this CPU (eg, 66 on an 8086) is treated like TYPE_NONE, as PCjs intends. */
        tsize = type & IDW_TYPE_SIZE;
        if ((tsize == IDW_TYPE_NONE) || (tsize == IDW_TYPE_PREFIX))
            continue;

        mode = type & IDW_TYPE_MODE;
        if (mode >= IDW_TYPE_MODRM) {
            if (modrm < 0)
                modrm = dis_get_byte(&a);
            if (mode < IDW_TYPE_MODREG) {
                dis_modrm_operand(op, opname, segment, modrm, type, count, &a, model);
            } else {
                reg = dis_reg_operand((mode == IDW_TYPE_MODREG) ? (modrm & 0x7) : ((modrm >> 3) & 0x7), type, &a, model);
                strcpy(op, reg ? reg : "");
            }
        } else if (mode == IDW_TYPE_ONE) {
            strcpy(op, "1");
        } else if (mode == IDW_TYPE_IMM) {
            /* A far pointer is a seg:off, even when unassembling at a linear address. */
            char far_type = (addr_type != '%') ? addr_type : (((msw & 1) && !(cpu_state.eflags & VM_FLAG)) ? '#' : '&');
            if (!dis_imm_operand(op, type, &a, far_type))
                continue;
        } else if (mode == IDW_TYPE_IMMOFF) {
            sprintf(op, "%s[", segment);
            if (!a.addr32)
                dis_cat_hex(op, dis_get_short(&a), 4);
            else
                dis_cat_hex(op, (uint32_t) dis_get_long(&a), 8);
            strcat(op, "]");
        } else if (mode == IDW_TYPE_IMMREL) {
            int64_t  disp = (tsize == IDW_TYPE_BYTE) ? (int8_t) dis_get_byte(&a) : dis_get_word(&a);
            uint32_t dest = (uint32_t) (a.off + disp) & (a.data32 ? 0xffffffff : 0xffff);
            dis_cat_hex(op, dest, a.data32 ? 8 : 4);
        } else if (mode == IDW_TYPE_IMPREG) {
            if (tsize == IDW_TYPE_ST) {
                strcpy(op, "ST");
            } else if (tsize == IDW_TYPE_STREG) {
                sprintf(op, "ST(%d)", modrm & 0x7);
            } else {
                reg = dis_reg_operand((type & IDW_TYPE_IREG) >> 8, type, &a, model);
                strcpy(op, reg ? reg : "");
            }
        } else if (mode == IDW_TYPE_IMPSEG) {
            reg = dis_reg_operand((type & IDW_TYPE_IREG) >> 8, IDW_TYPE_SEGREG, &a, model);
            strcpy(op, reg ? reg : "");
        } else if (mode == IDW_TYPE_DSSI) {
            sprintf(op, "%s[SI]", *segment ? segment : "DS:");
        } else if (mode == IDW_TYPE_ESDI) {
            sprintf(op, "%s[DI]", *segment ? segment : "ES:");
        }

        if (!*op) {
            strcpy(operands, "INVALID");
            break;
        }
        if (*operands)
            strcat(operands, ",");
        strcat(operands, op);
    }

    /* The instruction's bytes, from its first prefix through its last operand byte. */
    for (uint32_t i = 0, len = (a.off - off) & a.off_mask; (i < len) && (i < 16); i++) {
        int b = idw_read_byte(base + ((off + i) & a.off_mask));
        if (b < 0)
            strcat(bytes, "??");
        else
            sprintf(bytes + strlen(bytes), "%02X", b);
    }

    if (info) {
        info->ins    = (names == idw_ins_names) ? ins : -1;
        info->opcode = opcode;
        info->rep    = !strncmp(prefix, "REP", 3);
    }

    /* Except for CMPS and SCAS, REPZ and REPNZ are both simply REP. */
    if (!strncmp(prefix, "REP", 3) && (opcode != OPCODE_CMPSB) && (opcode != OPCODE_CMPSW)
        && (opcode != OPCODE_SCASB) && (opcode != OPCODE_SCASW))
        prefix = "REP";
    if (*prefix) {
        snprintf(buf, sizeof(buf), "%s%s%s", opname, *operands ? " " : "", operands);
        snprintf(operands, sizeof(operands), "%s", buf);
        snprintf(opname, sizeof(opname), "%s", prefix);
    }

    if (addr_type == '%') {
        strcpy(buf, "%");
        dis_cat_hex(buf, off, 0);
    } else {
        sprintf(buf, "%c%04X:%0*X", addr_type, sel, ((off & ~0xffff) || size32) ? 8 : 4, off);
    }
    n = strlen(buf);
    n += snprintf(buf + n, sizeof(buf) - n, " %-*s %-8s%s%s", size32 ? 20 : 16, bytes,
                  opname, *operands ? " " : "", operands);

    if ((type_cpu >= 0) && (model < type_cpu) && (n < (int) sizeof(buf))) {
        int col = IDW_COMMENT_COL;
        snprintf(buf + n, sizeof(buf) - n, "%*s;%d CPU only", (n < col) ? (col - n) : 0, "", dis_cpus[type_cpu]);
    }

    snprintf(line, size, "%s", buf);
    return a.off;
}

uint32_t
idw_disasm(char *line, size_t size, char type, uint16_t sel, uint32_t base, uint32_t off, int size32)
{
    return dis_instruction(line, size, type, sel, base, off, size32, NULL);
}

/*
 * Like PCjs's doStep(): returns 1 if p should step over the instruction at base+off (a CALL,
 * HLT, INT n, INT3, INTO, LOOP, LOOPZ, LOOPNZ, or REP string instruction), and sets *next to the
 * offset of the instruction that follows it.
 */
int
idw_step_over(uint32_t base, uint32_t off, int size32, uint32_t *next)
{
    dis_info_t info;
    char       line[160];

    *next = dis_instruction(line, sizeof(line), '&', 0, base, off, size32, &info);

    switch (info.ins) {
        case IDW_INS_CALL:
        case IDW_INS_HLT: /* resumes after the HLT once an interrupt has been handled */
        case IDW_INS_INT:
        case IDW_INS_INT3:
        case IDW_INS_INTO:
        case IDW_INS_LOOP:
        case IDW_INS_LOOPNZ:
        case IDW_INS_LOOPZ:
            return 1;
        default:
            /* INS, OUTS, MOVS, CMPS, STOS, LODS and SCAS. */
            return info.rep && (((info.opcode >= 0x6C) && (info.opcode <= 0x6F)) || ((info.opcode >= 0xA4) && (info.opcode <= 0xA7))
                                || ((info.opcode >= 0xAA) && (info.opcode <= 0xAF)));
    }
}
