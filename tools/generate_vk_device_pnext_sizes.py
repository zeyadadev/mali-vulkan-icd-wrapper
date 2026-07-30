#!/usr/bin/env python3
"""Generate sizeof checks for structures accepted by VkDeviceCreateInfo.

The output is included inside a C++ function. Platform- and beta-protected
structures inherit their guards from the Vulkan registry extension metadata.
"""

import argparse
import collections
import xml.etree.ElementTree as ET


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("registry")
    parser.add_argument("header")
    parser.add_argument("output")
    args = parser.parse_args()

    root = ET.parse(args.registry).getroot()
    with open(args.header, "r", encoding="utf-8") as header_file:
        header = header_file.read()
    platform_guards = {
        platform.get("name"): platform.get("protect")
        for platform in root.findall("./platforms/platform")
    }

    guards = collections.defaultdict(set)
    for extension in root.findall("./extensions/extension"):
        guard = platform_guards.get(extension.get("platform"))
        if extension.get("provisional") == "true":
            guard = "VK_ENABLE_BETA_EXTENSIONS"
        if not guard:
            continue
        for type_ref in extension.findall("./require/type"):
            name = type_ref.get("name") or (type_ref.text or "").strip()
            if name:
                guards[name].add(guard)

    aliases = {}
    records = []
    for type_node in root.findall("./types/type"):
        if type_node.get("category") != "struct":
            continue
        name = type_node.get("name") or type_node.findtext("name")
        if not name:
            continue
        if type_node.get("alias"):
            aliases[name] = type_node.get("alias")
            continue
        extends = (type_node.get("structextends") or "").split(",")
        if "VkDeviceCreateInfo" not in extends:
            continue
        stype = None
        for member in type_node.findall("member"):
            if member.findtext("name") == "sType":
                stype = member.get("values")
                break
        if stype:
            # Distributions can ship a newer registry than vulkan_core.h.
            # Only emit expressions that the compiler's actual header knows.
            if name not in header or stype not in header:
                continue
            records.append((stype, name))

    # Aliases use the canonical structure's sType and size. They do not need a
    # duplicate comparison because the numeric structure type is identical.
    records.sort()
    lines = [
        "// Generated from vk.xml; do not edit.",
        "// Each comparison is an if (rather than switch) because aliases can",
        "// share a numeric VkStructureType value.",
    ]
    for stype, name in records:
        active_guards = sorted(guards.get(name, ()))
        for guard in active_guards:
            lines.append(f"#ifdef {guard}")
        lines.append(f"    if (type == {stype}) return sizeof({name});")
        for guard in reversed(active_guards):
            lines.append(f"#endif // {guard}")

    with open(args.output, "w", encoding="utf-8") as output:
        output.write("\n".join(lines))
        output.write("\n")


if __name__ == "__main__":
    main()
