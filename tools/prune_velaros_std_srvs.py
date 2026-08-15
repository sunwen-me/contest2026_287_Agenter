#!/usr/bin/env python3
#
# SPDX-License-Identifier: Apache-2.0
#
"""Remove ROS 2 service-event introspection from selected target codegen."""

from __future__ import annotations

import argparse
from pathlib import Path


class PruneError(RuntimeError):
    pass


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise PruneError(f"{label}: expected one marker, found {count}")
    return text.replace(old, new, 1)


def remove_between(
    text: str, start: str, end: str, label: str, keep_end: bool = True
) -> str:
    start_index = text.find(start)
    if start_index < 0:
        raise PruneError(f"{label}: start marker is missing")
    end_index = text.find(end, start_index + len(start))
    if end_index < 0:
        raise PruneError(f"{label}: end marker is missing")
    if not keep_end:
        end_index += len(end)
    return text[:start_index] + text[end_index:]


def rewrite(path: Path, transform) -> None:
    original = path.read_text(encoding="utf-8")
    rewritten = transform(original)
    if rewritten == original:
        raise PruneError(f"{path}: pruning made no change")
    path.write_text(rewritten, encoding="utf-8")


def prune_struct(text: str) -> str:
    return remove_between(
        text,
        "\n// Constants defined in the message\n\n"
        "// Include directives for member types\n// Member 'info'",
        "\n#ifdef __cplusplus\n}",
        "SetBool struct event block",
    )


def prune_functions_header(text: str) -> str:
    response_end = text.find("std_srvs__srv__SetBool_Response__Sequence__copy(")
    if response_end < 0:
        raise PruneError("SetBool functions header: response marker is missing")
    event_start = text.find("\n/// Initialize srv/SetBool message.", response_end)
    if event_start < 0:
        raise PruneError("SetBool functions header: event marker is missing")
    footer = text.find("\n#ifdef __cplusplus\n}", event_start)
    if footer < 0:
        raise PruneError("SetBool functions header: footer is missing")
    return text[:event_start] + text[footer:]


def prune_functions_source(text: str) -> str:
    marker = (
        "\n\n// Include directives for member types\n"
        "// Member `info`\n"
    )
    start = text.find(marker)
    if start < 0:
        raise PruneError("SetBool functions source: event marker is missing")
    return text[:start].rstrip() + "\n"


def prune_fastrtps_header(text: str) -> str:
    response = text.find(
        "ROSIDL_TYPESUPPORT_INTERFACE__MESSAGE_SYMBOL_NAME("
        "rosidl_typesupport_fastrtps_c, std_srvs, srv, SetBool_Response)();"
    )
    if response < 0:
        raise PruneError("SetBool Fast RTPS header: response marker is missing")
    start = text.find("\n// already included above", response)
    end_marker = '#include "rosidl_runtime_c/service_type_support_struct.h"'
    end = text.find(end_marker, start)
    if start < 0 or end < 0:
        raise PruneError("SetBool Fast RTPS header: event block markers are missing")
    return text[:start] + "\n" + text[end:]


def prune_generator_type_support_header(text: str) -> str:
    response = text.find(
        "ROSIDL_TYPESUPPORT_INTERFACE__MESSAGE_SYMBOL_NAME(\n"
        "  rosidl_typesupport_c,\n  std_srvs,\n  srv,\n  SetBool_Response\n)(void);"
    )
    if response < 0:
        raise PruneError("SetBool generator type-support header: response marker is missing")
    start = text.find("\n// already included above", response)
    end_marker = '#include "rosidl_runtime_c/service_type_support_struct.h"'
    end = text.find(end_marker, start)
    if start < 0 or end < 0:
        raise PruneError(
            "SetBool generator type-support header: event block markers are missing"
        )
    text = text[:start] + "\n" + text[end:]
    event_api = text.find(
        "\n// Forward declare the function to create a service event message"
    )
    footer = text.find("\n#ifdef __cplusplus\n}", event_api)
    if event_api < 0 or footer < 0:
        raise PruneError(
            "SetBool generator type-support header: event API markers are missing"
        )
    return text[:event_api] + text[footer:]


def prune_fastrtps_source(text: str) -> str:
    response = text.find(
        "ROSIDL_TYPESUPPORT_INTERFACE__MESSAGE_SYMBOL_NAME("
        "rosidl_typesupport_fastrtps_c, std_srvs, srv, SetBool_Response)()"
    )
    if response < 0:
        raise PruneError("SetBool Fast RTPS source: response marker is missing")
    start = text.find("\n// already included above\n// #include <cassert>", response)
    end_marker = '#include "rosidl_typesupport_fastrtps_cpp/service_type_support.h"'
    end = text.find(end_marker, start)
    if start < 0 or end < 0:
        raise PruneError("SetBool Fast RTPS source: event block markers are missing")
    text = text[:start] + "\n" + text[end:]

    text = replace_once(
        text,
        "  &_SetBool_Event__type_support,\n"
        "  ROSIDL_TYPESUPPORT_INTERFACE__SERVICE_CREATE_EVENT_MESSAGE_SYMBOL_NAME(\n"
        "    rosidl_typesupport_c,\n"
        "    std_srvs,\n"
        "    srv,\n"
        "    SetBool\n"
        "  ),\n"
        "  ROSIDL_TYPESUPPORT_INTERFACE__SERVICE_DESTROY_EVENT_MESSAGE_SYMBOL_NAME(\n"
        "    rosidl_typesupport_c,\n"
        "    std_srvs,\n"
        "    srv,\n"
        "    SetBool\n"
        "  ),",
        "  nullptr,\n  nullptr,\n  nullptr,",
        "SetBool service event handles",
    )

    for symbol in (
        "std_srvs__srv__SetBool_Request",
        "std_srvs__srv__SetBool_Response",
        "std_srvs__srv__SetBool",
    ):
        text = replace_once(
            text,
            f"  &{symbol}__get_type_description,\n"
            f"  &{symbol}__get_type_description_sources,",
            "  nullptr,\n  nullptr,",
            f"{symbol} type-description callbacks",
        )
    return text


def prune_cpp_public_header(text: str) -> str:
    """Keep the request/response struct include and remove generic helpers."""

    for include in (
        '#include "std_srvs/srv/detail/set_bool__builder.hpp"    // IWYU pragma: export\n',
        '#include "std_srvs/srv/detail/set_bool__traits.hpp"    // IWYU pragma: export\n',
        '#include "std_srvs/srv/detail/set_bool__type_support.hpp"    // IWYU pragma: export\n',
    ):
        text = replace_once(text, include, "", "SetBool C++ public header")
    return text


def prune_cpp_struct(text: str) -> str:
    event_start = text.find(
        "\n// Include directives for member types\n// Member 'info'\n"
    )
    if event_start < 0:
        raise PruneError("SetBool C++ struct: event marker is missing")
    service_start = text.find(
        "\nnamespace std_srvs\n{\n\nnamespace srv\n{\n\nstruct SetBool\n",
        event_start,
    )
    if service_start < 0:
        raise PruneError("SetBool C++ struct: service marker is missing")
    text = text[:event_start] + text[service_start:]
    return replace_once(
        text,
        "  using Event = std_srvs::srv::SetBool_Event;\n",
        "",
        "SetBool C++ Event alias",
    )


def prune_fastrtps_cpp_header(text: str) -> str:
    response = text.find(
        "ROSIDL_TYPESUPPORT_INTERFACE__MESSAGE_SYMBOL_NAME("
        "rosidl_typesupport_fastrtps_cpp, std_srvs, srv, SetBool_Response)();"
    )
    if response < 0:
        raise PruneError("SetBool Fast RTPS C++ header: response marker is missing")
    start = text.find("\n// already included above", response)
    end_marker = '#include "rmw/types.h"'
    end = text.find(end_marker, start)
    if start < 0 or end < 0:
        raise PruneError(
            "SetBool Fast RTPS C++ header: event block markers are missing"
        )
    return text[:start] + "\n" + text[end:]


def prune_fastrtps_cpp_source(text: str) -> str:
    response = text.find(
        "ROSIDL_TYPESUPPORT_INTERFACE__MESSAGE_SYMBOL_NAME("
        "rosidl_typesupport_fastrtps_cpp, std_srvs, srv, SetBool_Response)()"
    )
    if response < 0:
        raise PruneError("SetBool Fast RTPS C++ source: response marker is missing")
    start = text.find("\n// already included above\n// #include <cstddef>", response)
    end_marker = '#include "rmw/error_handling.h"'
    end = text.find(end_marker, start)
    if start < 0 or end < 0:
        raise PruneError(
            "SetBool Fast RTPS C++ source: event block markers are missing"
        )
    text = text[:start] + "\n" + text[end:]

    for symbol in (
        "std_srvs__srv__SetBool_Request",
        "std_srvs__srv__SetBool_Response",
    ):
        text = replace_once(
            text,
            f"  &{symbol}__get_type_hash,\n"
            f"  &{symbol}__get_type_description,\n"
            f"  &{symbol}__get_type_description_sources,",
            f"  &{symbol}__get_type_hash,\n  nullptr,\n  nullptr,",
            f"{symbol} C++ type-description callbacks",
        )

    text = replace_once(
        text,
        "  ::rosidl_typesupport_fastrtps_cpp::get_message_type_support_handle<"
        "std_srvs::srv::SetBool_Event>(),\n"
        "  &::rosidl_typesupport_cpp::service_create_event_message<"
        "std_srvs::srv::SetBool>,\n"
        "  &::rosidl_typesupport_cpp::service_destroy_event_message<"
        "std_srvs::srv::SetBool>,",
        "  nullptr,\n  nullptr,\n  nullptr,",
        "SetBool C++ service event handles",
    )
    text = replace_once(
        text,
        "  &std_srvs__srv__SetBool__get_type_hash,\n"
        "  &std_srvs__srv__SetBool__get_type_description,\n"
        "  &std_srvs__srv__SetBool__get_type_description_sources,",
        "  &std_srvs__srv__SetBool__get_type_hash,\n  nullptr,\n  nullptr,",
        "SetBool C++ service type-description callbacks",
    )
    return text


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("root", type=Path)
    args = parser.parse_args()
    generator = args.root / "rosidl_generator_c/std_srvs/srv/detail"
    fastrtps = args.root / "rosidl_typesupport_fastrtps_c/std_srvs/srv/detail"
    generator_cpp = args.root / "rosidl_generator_cpp/std_srvs/srv"
    fastrtps_cpp = (
        args.root / "rosidl_typesupport_fastrtps_cpp/std_srvs/srv/detail"
    )

    rewrite(generator / "set_bool__struct.h", prune_struct)
    rewrite(generator / "set_bool__functions.h", prune_functions_header)
    rewrite(generator / "set_bool__functions.c", prune_functions_source)
    rewrite(
        generator / "set_bool__type_support.h",
        prune_generator_type_support_header,
    )
    rewrite(
        fastrtps / "set_bool__rosidl_typesupport_fastrtps_c.h",
        prune_fastrtps_header,
    )
    rewrite(fastrtps / "set_bool__type_support_c.cpp", prune_fastrtps_source)
    rewrite(generator_cpp / "set_bool.hpp", prune_cpp_public_header)
    rewrite(generator_cpp / "detail/set_bool__struct.hpp", prune_cpp_struct)
    rewrite(
        fastrtps_cpp / "set_bool__rosidl_typesupport_fastrtps_cpp.hpp",
        prune_fastrtps_cpp_header,
    )
    rewrite(
        fastrtps_cpp / "dds_fastrtps/set_bool__type_support.cpp",
        prune_fastrtps_cpp_source,
    )

    for path in args.root.rglob("*"):
        if path.is_file() and path.suffix in {".c", ".cpp", ".h", ".hpp"}:
            content = path.read_text(encoding="utf-8")
            if "ServiceEventInfo" in content or "SetBool_Event" in content:
                raise PruneError(f"service introspection survived pruning: {path}")
    print("Pruned std_srvs/SetBool service-event introspection: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
