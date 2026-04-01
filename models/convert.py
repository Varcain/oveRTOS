#!/usr/bin/env python3
# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""Convert .tflite model files to C arrays for firmware embedding.

Scans a directory recursively for *.tflite files and generates
matching .c/.h pairs with aligned byte arrays and size defines.

Usage:
    python3 models/convert.py --model-dir models/ --output-dir output/generated_models/
"""

import argparse
import os
import sys


def tflite_to_c(tflite_path, output_dir):
    """Convert a single .tflite file to .c and .h files."""
    basename = os.path.splitext(os.path.basename(tflite_path))[0]
    symbol = f"g_{basename}"

    with open(tflite_path, "rb") as f:
        data = f.read()

    size = len(data)
    guard = f"{symbol.upper()}_MODEL_DATA_H"

    # Generate .h
    h_path = os.path.join(output_dir, f"{symbol}_model_data.h")
    with open(h_path, "w") as f:
        f.write(f"/* Auto-generated from {os.path.basename(tflite_path)} — do not edit. */\n")
        f.write(f"#ifndef {guard}\n")
        f.write(f"#define {guard}\n\n")
        f.write(f"#define {symbol}_model_data_size {size}\n\n")
        f.write(f"#ifdef __cplusplus\n")
        f.write(f'extern "C" {{\n')
        f.write(f"#endif\n\n")
        f.write(f"extern const unsigned char {symbol}_model_data[{size}];\n")
        f.write(f"extern const unsigned int  {symbol}_model_data_len;\n\n")
        f.write(f"#ifdef __cplusplus\n")
        f.write(f"}}\n")
        f.write(f"#endif\n\n")
        f.write(f"#endif /* {guard} */\n")

    # Generate .c
    c_path = os.path.join(output_dir, f"{symbol}_model_data.c")
    with open(c_path, "w") as f:
        f.write(f"/* Auto-generated from {os.path.basename(tflite_path)} — do not edit. */\n")
        f.write(f'#include "{symbol}_model_data.h"\n\n')
        f.write(f"__attribute__((aligned(16)))\n")
        f.write(f"const unsigned char {symbol}_model_data[{size}] = {{\n")
        for i in range(0, size, 12):
            chunk = data[i : i + 12]
            hex_str = ", ".join(f"0x{b:02x}" for b in chunk)
            f.write(f"    {hex_str},\n")
        f.write("};\n\n")
        f.write(f"const unsigned int {symbol}_model_data_len = {size};\n")

    return basename, size


def main():
    parser = argparse.ArgumentParser(
        description="Convert .tflite models to C arrays"
    )
    parser.add_argument(
        "--model-dir",
        required=True,
        help="Directory to scan recursively for .tflite files",
    )
    parser.add_argument(
        "--output-dir",
        required=True,
        help="Output directory for generated .c/.h files",
    )
    args = parser.parse_args()

    os.makedirs(args.output_dir, exist_ok=True)

    models = []
    for root, _, files in os.walk(args.model_dir):
        for fname in sorted(files):
            if fname.endswith(".tflite"):
                path = os.path.join(root, fname)
                name, size = tflite_to_c(path, args.output_dir)
                models.append((name, size))

    if models:
        print(f"Generated {len(models)} model(s) in {args.output_dir}:")
        for name, size in models:
            print(f"  {name}: {size} bytes")
    else:
        print(f"No .tflite files found in {args.model_dir}", file=sys.stderr)


if __name__ == "__main__":
    main()
