// Generates src/idw_distab.h from the PCjs x86 debugger's disassembly tables.
// Usage: node idw_distab.mjs <pcjs debugger.js> > idw_distab.h
import fs from "fs";

const src = fs.readFileSync(process.argv[2], "utf8").split("\n");
const start = src.findIndex(l => /^\s*Debuggerx86\.INS = \{/.test(l));
const end = src.findIndex(l => /^\s*Debuggerx86\.SYSDESCS = \{/.test(l));
if (start < 0 || end < 0) throw new Error("table section not found");

const Debuggerx86 = {};
new Function("Debuggerx86", "DEBUG", src.slice(start, end).join("\n"))(Debuggerx86, false);
const D = Debuggerx86;

const out = [];
const hex = (n, w = 4) => "0x" + n.toString(16).toUpperCase().padStart(w, "0");
const UNDEF = 0xFFFF;

function desc(a) {
    if (!a) return "{ 0, 0, 0, { 0, 0, 0, 0 } }";
    if (a.length > 5) throw new Error("too many operands: " + JSON.stringify(a));
    const ops = [1, 2, 3, 4].map(i => (i < a.length) ? (a[i] === undefined ? UNDEF : a[i]) : 0);
    return `{ 1, ${a[0]}, ${a.length - 1}, { ${ops.map(o => hex(o)).join(", ")} } }`;
}
function strs(name, a) {
    out.push(`static const char *const ${name}[] = {`);
    for (let i = 0; i < a.length; i += 8)
        out.push("    " + a.slice(i, i + 8).map(s => (s == null) ? "NULL" : JSON.stringify(s)).join(", ") + ",");
    out.push("};", "");
}

out.push("/*",
    " * Disassembly tables for the Internal Debugger Window (IDW).",
    " *",
    " * GENERATED FILE; DO NOT EDIT. Generated from the PCjs x86 debugger",
    " * (machines/pcx86/modules/v2/debugger.js) by src/idw_distab.mjs.",
    " */",
    "#ifndef EMU_IDW_DISTAB_H",
    "#define EMU_IDW_DISTAB_H",
    "");

for (const [k, v] of Object.entries(D.INS)) out.push(`#define IDW_INS_${k} ${v}`);
out.push("");
for (const k of Object.keys(D).filter(k => /^(TYPE|CPU|REG)_/.test(k) && typeof D[k] == "number"))
    out.push(`#define IDW_${k} ${hex(D[k])}`);
out.push(`#define IDW_TYPE_UNDEF ${hex(UNDEF)}`, "");
out.push(`#define IDW_INS_NAMES_COUNT ${D.INS_NAMES.length}`);
out.push(`#define IDW_FINS_NAMES_COUNT ${D.FINS_NAMES.length}`, "");

out.push("typedef struct {",
    "    uint8_t  valid;",
    "    uint8_t  ins;",
    "    uint8_t  count;",
    "    uint16_t ops[4];",
    "} idw_opdesc_t;", "");

strs("idw_ins_names", D.INS_NAMES);
strs("idw_fins_names", D.FINS_NAMES);
strs("idw_reg_names", D.REGS);
strs("idw_rm_names", D.RMS);

out.push(`static const idw_opdesc_t idw_op_desc_popcs     = ${desc(D.aOpDescPopCS)};`);
out.push(`static const idw_opdesc_t idw_op_desc_undefined = ${desc(D.aOpDescUndefined)};`);
out.push(`static const idw_opdesc_t idw_op_desc_0f        = ${desc(D.aOpDesc0F)};`, "");

out.push("static const idw_opdesc_t idw_op_descs[256] = {");
for (let i = 0; i < 256; i++) out.push(`    /* ${hex(i, 2)} */ ${desc(D.aaOpDescs[i])},`);
out.push("};", "");

out.push("static const idw_opdesc_t idw_op0f_descs[256] = {");
for (let i = 0; i < 256; i++) out.push(`    /* ${hex(i, 2)} */ ${desc(D.aaOp0FDescs[i] || D.aOpDescUndefined)},`);
out.push("};", "");

out.push(`static const idw_opdesc_t idw_grp_descs[${D.aaGrpDescs.length}][8] = {`);
D.aaGrpDescs.forEach((g, n) => {
    out.push(`    { /* group ${n} */`);
    for (let i = 0; i < 8; i++) out.push(`        ${desc(g[i])},`);
    out.push("    },");
});
out.push("};", "");

out.push("/* Indexed by [opcode - 0xD8][modReg]; entries with valid == 0 stay ESC instructions. */");
out.push("static const idw_opdesc_t idw_fpu_descs[8][0x80] = {");
for (let op = 0xD8; op <= 0xDF; op++) {
    const t = D.aaaOpFPUDescs[op] || {};
    for (const k of Object.keys(t)) if (+k >= 0x80) throw new Error("FPU index too large");
    out.push(`    { /* ${hex(op, 2)} */`);
    for (let i = 0; i < 0x80; i++) if (t[i]) out.push(`        [${hex(i, 2)}] = ${desc(t[i])},`);
    out.push("    },");
}
out.push("};", "", "#endif", "");

process.stdout.write(out.join("\n"));
