#!/usr/bin/env python3
"""
从 jieba 词典生成 C 源码文件（字典数据+二分查找），编译进 libkanbudb。
用法: python3 gen_dict_header.py ../src/vector/dict_data.c
"""
import sys
import os

DICT_PATH = "/usr/local/lib/python3.12/dist-packages/jieba/dict.txt"
MAX_WORD_LEN = 8  # 最长词（字符数）

def main():
    out_path = sys.argv[1] if len(sys.argv) > 1 else "../src/vector/dict_data.c"

    # 读词典，提取词条
    words = set()
    with open(DICT_PATH, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            word = line.split()[0]
            # 按字符数过滤：单字到8字
            if 1 <= len(word) <= MAX_WORD_LEN:
                words.add(word)

    # 按 UTF-8 字节排序（与 strcmp 一致）
    words_sorted = sorted(words)
    count = len(words_sorted)

    # 构建字符串池（null-terminated）
    pool = b"".join(w.encode("utf-8") + b"\x00" for w in words_sorted)

    # 生成 .c 文件
    with open(out_path, "w", encoding="utf-8") as f:
        f.write(f"// Auto-generated from jieba dict.txt — DO NOT EDIT\n")
        f.write(f"// Source: {DICT_PATH}\n")
        f.write(f"// Total words: {count}\n\n")
        f.write('#include "dict_data.h"\n\n')

        # 字符串池（unsigned char 数组）
        f.write(f"const unsigned char dict_pool[{len(pool)}] = {{\n")
        # 每行写 24 个字节
        for i in range(0, len(pool), 24):
            chunk = pool[i:i+24]
            hex_bytes = ", ".join(f"0x{b:02x}" for b in chunk)
            f.write(f"  {hex_bytes},\n")
        f.write("};\n\n")

        # 偏移数组
        offset = 0
        offsets = []
        for w in words_sorted:
            offsets.append(offset)
            offset += len(w.encode("utf-8")) + 1

        f.write(f"const uint32_t dict_offsets[{count}] = {{\n")
        for i in range(0, count, 16):
            chunk = offsets[i:i+16]
            f.write("  " + ", ".join(str(o) for o in chunk) + ",\n")
        f.write("};\n\n")

        f.write(f"const int dict_count = {count};\n")

    # 生成 .h 文件
    h_path = out_path.replace(".c", ".h")
    with open(h_path, "w", encoding="utf-8") as f:
        f.write("// Auto-generated — DO NOT EDIT\n")
        f.write("#ifndef KANBUDB_DICT_DATA_H\n")
        f.write("#define KANBUDB_DICT_DATA_H\n\n")
        f.write("#include <stdint.h>\n\n")
        f.write(f"#define DICT_MAX_WORD_LEN {MAX_WORD_LEN}\n\n")
        f.write("extern const unsigned char dict_pool[];\n")
        f.write("extern const uint32_t dict_offsets[];\n")
        f.write("extern const int dict_count;\n\n")
        f.write("#endif /* KANBUDB_DICT_DATA_H */\n")

    print(f"  Generated {out_path} ({count} words, {len(pool)} bytes)")
    print(f"  Generated {h_path}")

if __name__ == "__main__":
    main()
