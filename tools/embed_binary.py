#!/usr/bin/env python3
"""Generate a small C++ header containing an immutable binary asset."""

import argparse
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("symbol")
    args = parser.parse_args()

    data = args.input.read_bytes()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", encoding="ascii", newline="\n") as output:
        output.write("#pragma once\n#include <cstddef>\n#include <cstdint>\n\n")
        output.write(f"inline constexpr std::uint8_t {args.symbol}[] = {{\n")
        for offset in range(0, len(data), 16):
            chunk = data[offset : offset + 16]
            output.write("   " + ", ".join(f"0x{byte:02x}" for byte in chunk) + ",\n")
        output.write("};\n")
        output.write(f"inline constexpr std::size_t {args.symbol}_size = sizeof({args.symbol});\n")


if __name__ == "__main__":
    main()
