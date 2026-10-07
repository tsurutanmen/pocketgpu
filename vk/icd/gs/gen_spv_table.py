# SPIR-V の命令表（spirv.core.grammar.json）から、C で「どの語が id か」を読むための表を作る
#   python gen_spv_table.py > spv_table.h
# 記号：t 結果の型・r 結果の id・i id・l 1 語の値・s 文字列・n 残りの語全部（型で長さが決まる数）
#       e<k> 値の列挙（引数つき）・b<k> ビットの列挙（引数つき）・P id と値の組・Q 値と id の組・D id の組
#       ? 次の 1 つは無くてもよい・* 次の 1 つを終わりまで繰り返す
import json, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
G = json.load(open(os.path.join(HERE, "_ref_spirv.core.grammar.json"), encoding="utf-8"))
KINDS = {k["kind"]: k for k in G["operand_kinds"]}
enum_kinds = []  # 引数を持つ列挙だけ表にする


def kind_code(kind):
    if kind == "IdResultType":
        return "t"
    if kind == "IdResult":
        return "r"
    if kind.startswith("Id"):
        return "i"
    if kind == "LiteralString":
        return "s"
    if kind == "LiteralContextDependentNumber":
        return "n"
    if kind == "PairIdRefLiteralInteger":
        return "P"
    if kind == "PairLiteralIntegerIdRef":
        return "Q"
    if kind == "PairIdRefIdRef":
        return "D"
    k = KINDS.get(kind)
    if k and k["category"] in ("ValueEnum", "BitEnum"):
        if any(e.get("parameters") for e in k.get("enumerants", [])):
            if kind not in enum_kinds:
                enum_kinds.append(kind)
            return ("e" if k["category"] == "ValueEnum" else "b") + chr(ord("A") + enum_kinds.index(kind))
        return "l"
    return "l"


def ops_string(operands):
    s = ""
    for od in operands:
        q = od.get("quantifier", "")
        if q == "?":
            s += "?"
        elif q == "*":
            s += "*"
        s += kind_code(od["kind"])
    return s


rows = []
for ins in G["instructions"]:
    rows.append((ins["opcode"], ins["opname"], ops_string(ins.get("operands", []))))

out = []
out.append("// 自動で作った表（gen_spv_table.py）。SPIR-V %d.%d の命令表から" % (G["major_version"], G["minor_version"]))
out.append("#pragma once")
out.append("#include <stdint.h>")
out.append("typedef struct { uint32_t v; const char *p; } spv_enum_param;")
out.append("typedef struct { const spv_enum_param *e; uint32_t n; } spv_enum_kind;")
out.append("typedef struct { uint16_t op; const char *o; } spv_op_desc;")
for idx, kind in enumerate(enum_kinds):
    k = KINDS[kind]
    items = []
    seen = set()
    # ビットの列挙は、引数を小さいビットから順に読むので、値の順に並べる。同じ値（別名）は 1 つだけ
    for e in sorted(k["enumerants"], key=lambda e: e["value"] if isinstance(e["value"], int) else int(e["value"], 0)):
        ps = e.get("parameters", [])
        v = e["value"] if isinstance(e["value"], int) else int(e["value"], 0)
        if not ps or v in seen:
            continue
        seen.add(v)
        items.append("{0x%x, \"%s\"}" % (v, "".join(kind_code(p["kind"]) for p in ps)))
    out.append("static const spv_enum_param spv_ek_%d[] = { %s }; // %s" % (idx, ", ".join(items), kind))
out.append("static const spv_enum_kind spv_enum_kinds[] = {")
for idx, kind in enumerate(enum_kinds):
    out.append("    { spv_ek_%d, sizeof spv_ek_%d / sizeof spv_ek_%d[0] }," % (idx, idx, idx))
out.append("};")
out.append("static const spv_op_desc spv_ops[] = {")
for op, name, s in sorted(rows):
    out.append("    { %d, \"%s\" }, // %s" % (op, s, name))
out.append("};")
out.append("#define SPV_NOPS (sizeof spv_ops / sizeof spv_ops[0])")
sys.stdout.reconfigure(encoding="utf-8")
print("\n".join(out))
