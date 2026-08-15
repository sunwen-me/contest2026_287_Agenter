#!/usr/bin/env python3
#
# SPDX-License-Identifier: Apache-2.0
#
"""Prune ROS 2 Action service events and enforce target sequence bounds."""

from __future__ import annotations

import argparse
import re
from pathlib import Path


class PruneError(RuntimeError):
    pass


def rewrite(path: Path, transform) -> None:
    original = path.read_text(encoding="utf-8")
    rewritten = transform(original)
    if rewritten == original:
        raise PruneError(f"{path}: pruning made no change")
    path.write_text(rewritten, encoding="utf-8")


def remove_span(text: str, start: int, end: int, label: str) -> str:
    if start < 0 or end < 0 or end <= start:
        raise PruneError(f"{label}: invalid generated-code markers")
    return text[:start] + text[end:]


def prune_struct_events(text: str, events: list[str]) -> str:
    for event in events:
        declaration = f"typedef struct {event}\n"
        index = text.find(declaration)
        if index < 0:
            raise PruneError(f"{event}: struct declaration is missing")
        start = text.rfind("\n// Constants defined in the message", 0, index)
        end = text.find("\n// Constants defined in the message", index + len(declaration))
        if end < 0:
            end = text.find("\n#ifdef __cplusplus\n}", index)
        text = remove_span(text, start, end, f"{event} struct")
    return text


def prune_function_events(
    text: str, events_and_next: list[tuple[str, str | None]], header: bool
) -> str:
    for event, next_type in events_and_next:
        signature = f"{event}__init("
        index = text.find(signature)
        if index < 0:
            raise PruneError(f"{event}: init function is missing")
        marker = "\n/// Initialize" if header else "\n\n// Include directives for member types"
        start = text.rfind(marker, 0, index)
        if next_type is None:
            end = text.find("\n#ifdef __cplusplus\n}", index) if header else len(text)
        else:
            next_index = text.find(f"{next_type}__init(", index)
            if next_index < 0:
                raise PruneError(f"{event}: next type {next_type} is missing")
            end = text.rfind(marker, index, next_index)
        text = remove_span(text, start, end, f"{event} functions")
    return text.rstrip() + "\n"


def find_line_start(text: str, needle: str, start: int) -> int:
    index = text.find(needle, start)
    if index < 0:
        return -1
    return text.rfind("\n", 0, index)


def prune_typesupport_header_events(text: str, events: list[str]) -> str:
    for event in events:
        index = text.find(event)
        if index < 0:
            raise PruneError(f"{event}: type-support declaration is missing")
        start = text.rfind("\n// already included above", 0, index)
        if start < 0:
            start = text.rfind("\n#include \"rosidl_runtime_c/message_type_support_struct.h\"", 0, index)
        end = find_line_start(text, "service_type_support_struct.h", index)
        text = remove_span(text, start, end, f"{event} type-support declaration")

    create_marker = "\n// Forward declare the function to create a service event message"
    while create_marker in text:
        start = text.find(create_marker)
        next_message = text.find("\n// already included above", start + len(create_marker))
        footer = text.find("\n#ifdef __cplusplus\n}", start)
        if footer < 0:
            raise PruneError("service event API footer is missing")
        end = next_message if 0 <= next_message < footer else footer
        text = remove_span(text, start, end, "service event API")
    return text


def prune_fastrtps_header_events(text: str, events: list[str]) -> str:
    for event in events:
        index = text.find(event)
        if index < 0:
            raise PruneError(f"{event}: Fast RTPS declaration is missing")
        start = text.rfind("\n// already included above", 0, index)
        if start < 0:
            start = text.rfind("\n#include \"rosidl_runtime_c/message_type_support_struct.h\"", 0, index)
        end = find_line_start(text, "service_type_support_struct.h", index)
        text = remove_span(text, start, end, f"{event} Fast RTPS declaration")
    return text


def prune_fastrtps_source(
    text: str,
    events: list[str],
    service_names: list[str],
    sequence_limit_macro: str,
) -> str:
    for event in events:
        index = text.find(f"using _{event.split('__')[-1]}__ros_msg_type")
        if index < 0:
            index = text.find(event)
        if index < 0:
            raise PruneError(f"{event}: Fast RTPS implementation is missing")
        start = text.rfind("\n// already included above\n// #include <cassert>", 0, index)
        end = find_line_start(text, "service_type_support.h", index)
        text = remove_span(text, start, end, f"{event} Fast RTPS implementation")

    for service in service_names:
        pattern = re.compile(
            rf"  &_({re.escape(service)})_Event__type_support,\n"
            rf"  ROSIDL_TYPESUPPORT_INTERFACE__SERVICE_CREATE_EVENT_MESSAGE_SYMBOL_NAME\(\n"
            rf"(?:.*\n)*?  \),\n"
            rf"  ROSIDL_TYPESUPPORT_INTERFACE__SERVICE_DESTROY_EVENT_MESSAGE_SYMBOL_NAME\(\n"
            rf"(?:.*\n)*?  \),",
        )
        text, count = pattern.subn("  nullptr,\n  nullptr,\n  nullptr,", text, count=1)
        if count != 1:
            raise PruneError(f"{service}: service event handle is missing")

    description_pattern = re.compile(
        r"  &(\w+)__get_type_description,\n"
        r"  &\1__get_type_description_sources,"
    )
    text, description_count = description_pattern.subn("  nullptr,\n  nullptr,", text)
    if description_count == 0:
        raise PruneError("type-description callbacks are missing")

    size_marker = "    size_t size = static_cast<size_t>(cdrSize);\n"
    bound = (
        size_marker
        + f"    if (size > {sequence_limit_macro}) {{\n"
        + "      fprintf(stderr, \"VelaROS Action sequence bound exceeded\\n\");\n"
        + "      return false;\n"
        + "    }\n"
    )
    count = text.count(size_marker)
    if count > 0:
        text = text.replace(size_marker, bound)

    define_anchor = '#include "fastcdr/Cdr.h"\n'
    if define_anchor not in text:
        raise PruneError("Fast CDR include marker is missing")
    limit_define = (
        define_anchor
        + f"\n#ifndef {sequence_limit_macro}\n"
        + f"#define {sequence_limit_macro} 4\n"
        + "#endif\n"
    )
    text = text.replace(define_anchor, limit_define, 1)
    return text


def extract_hashes(description_files: list[Path], output: Path) -> None:
    function_pattern = re.compile(
        r"(?:ROSIDL_GENERATOR_C_PUBLIC_\w+\n)?"
        r"const rosidl_type_hash_t \*\n"
        r"(\w+__get_type_hash)\(\n"
        r"  const [^\n]+\n"
        r"\{\n.*?\n\}\n",
        re.DOTALL,
    )
    includes: list[tuple[str, bool]] = []
    functions: list[tuple[str, bool]] = []
    for path in description_files:
        text = path.read_text(encoding="utf-8")
        include = re.search(r'^#include "([^"]+__functions\.h)"$', text, re.MULTILINE)
        if include is None:
            raise PruneError(f"{path}: functions include is missing")
        include_path = include.group(1)
        guarded = include_path.startswith("example_interfaces/")
        include_entry = (include_path, guarded)
        if include_entry not in includes:
            includes.append(include_entry)
        matches = list(function_pattern.finditer(text))
        if not matches:
            raise PruneError(f"{path}: no TypeHash functions found")
        for match in matches:
            if "_Event__get_type_hash" not in match.group(1):
                functions.append((match.group(0).strip(), guarded))

    body = [
        "/* Generated from locked ROS 2 Lyrical interface descriptions. */",
        "/* Service-event and runtime type-description functions are omitted. */",
        "",
    ]
    for include, guarded in includes:
        if guarded:
            body.extend(
                (
                    "#ifdef CONFIG_VELAROS_ACTION_FIBONACCI",
                    f'#include "{include}"',
                    "#endif",
                )
            )
        else:
            body.append(f'#include "{include}"')
    body.append("")
    fibonacci_open = False
    for function, guarded in functions:
        if guarded and not fibonacci_open:
            body.append("#ifdef CONFIG_VELAROS_ACTION_FIBONACCI")
            fibonacci_open = True
        elif not guarded and fibonacci_open:
            body.append("#endif")
            fibonacci_open = False
        body.append(function)
    if fibonacci_open:
        body.append("#endif")
    output.write_text("\n\n".join(body).rstrip() + "\n", encoding="utf-8")


def prune_description_declarations(text: str) -> str:
    pattern = re.compile(
        r"\n/// Retrieve pointer to the description of this type\.\n"
        r".*?__get_type_description_sources\(\n"
        r"  const [^\n]+\);\n",
        re.DOTALL,
    )
    text, count = pattern.subn("\n", text)
    if count == 0:
        raise PruneError("runtime type-description declarations are missing")
    return text


def preserve_get_result_hash(text: str, package: str, action: str) -> str:
    symbol = f"{package}__action__{action}_GetResult__get_type_hash"
    if symbol in text:
        return text
    footer = text.find("\n#ifdef __cplusplus\n}")
    if footer < 0:
        raise PruneError(f"{action} functions header footer is missing")
    declaration = (
        "\n/// Retrieve pointer to the hash of the GetResult service.\n"
        f"ROSIDL_GENERATOR_C_PUBLIC_{package}\n"
        "const rosidl_type_hash_t *\n"
        f"{symbol}(\n"
        "  const rosidl_service_type_support_t * type_support);\n"
    )
    return text[:footer] + declaration + text[footer:]


def prune_action(
    root: Path,
    package: str,
    action: str,
    snake_name: str,
    sequence_limit_macro: str,
) -> None:
    generator = root / f"rosidl_generator_c/{package}/action/detail"
    fastrtps = root / f"rosidl_typesupport_fastrtps_c/{package}/action/detail"
    events = [
        f"{package}__action__{action}_SendGoal_Event",
        f"{package}__action__{action}_GetResult_Event",
    ]
    next_types = [
        (events[0], f"{package}__action__{action}_GetResult_Request"),
        (events[1], f"{package}__action__{action}_FeedbackMessage"),
    ]
    rewrite(
        generator / f"{snake_name}__struct.h",
        lambda text: prune_struct_events(text, events),
    )
    rewrite(
        generator / f"{snake_name}__functions.h",
        lambda text: preserve_get_result_hash(
            prune_function_events(text, next_types, True), package, action
        ),
    )
    rewrite(
        generator / f"{snake_name}__functions.c",
        lambda text: prune_function_events(text, next_types, False),
    )
    rewrite(
        generator / f"{snake_name}__type_support.h",
        lambda text: prune_typesupport_header_events(
            text, [f"{action}_SendGoal_Event", f"{action}_GetResult_Event"]
        ),
    )
    rewrite(
        fastrtps / f"{snake_name}__rosidl_typesupport_fastrtps_c.h",
        lambda text: prune_fastrtps_header_events(
            text, [f"{action}_SendGoal_Event", f"{action}_GetResult_Event"]
        ),
    )
    rewrite(
        fastrtps / f"{snake_name}__type_support_c.cpp",
        lambda text: prune_fastrtps_source(
            text,
            events,
            [f"{action}_SendGoal", f"{action}_GetResult"],
            sequence_limit_macro,
        ),
    )


def validate(root: Path) -> None:
    forbidden = ("ServiceEventInfo", "_Event", "get_type_description(")
    for path in root.rglob("*"):
        if path.is_file() and path.suffix in {".c", ".cpp", ".h", ".hpp"}:
            content = path.read_text(encoding="utf-8")
            for token in forbidden:
                if token in content:
                    raise PruneError(f"forbidden {token} survived in {path}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("root", type=Path)
    parser.add_argument("--description", action="append", type=Path, required=True)
    args = parser.parse_args()

    action_generator = args.root / "rosidl_generator_c/action_msgs"
    action_fastrtps = args.root / "rosidl_typesupport_fastrtps_c/action_msgs"

    cancel_event = "action_msgs__srv__CancelGoal_Event"
    rewrite(
        action_generator / "srv/detail/cancel_goal__struct.h",
        lambda text: prune_struct_events(text, [cancel_event]),
    )
    rewrite(
        action_generator / "srv/detail/cancel_goal__functions.h",
        lambda text: prune_function_events(text, [(cancel_event, None)], True),
    )
    rewrite(
        action_generator / "srv/detail/cancel_goal__functions.c",
        lambda text: prune_function_events(text, [(cancel_event, None)], False),
    )
    rewrite(
        action_generator / "srv/detail/cancel_goal__type_support.h",
        lambda text: prune_typesupport_header_events(text, ["CancelGoal_Event"]),
    )
    rewrite(
        action_fastrtps / "srv/detail/cancel_goal__rosidl_typesupport_fastrtps_c.h",
        lambda text: prune_fastrtps_header_events(text, ["CancelGoal_Event"]),
    )
    rewrite(
        action_fastrtps / "srv/detail/cancel_goal__type_support_c.cpp",
        lambda text: prune_fastrtps_source(
            text, [cancel_event], ["CancelGoal"], "VELAROS_ACTION_MAX_GOALS"
        ),
    )

    prune_action(
        args.root,
        "example_interfaces",
        "Fibonacci",
        "fibonacci",
        "VELAROS_ACTION_SEQUENCE_CAPACITY",
    )
    prune_action(
        args.root,
        "velaros_interfaces",
        "MoveRelative",
        "move_relative",
        "VELAROS_ACTION_MAX_GOALS",
    )

    message_fastrtps_roots = (
        args.root / "rosidl_typesupport_fastrtps_c/builtin_interfaces/msg/detail",
        args.root / "rosidl_typesupport_fastrtps_c/unique_identifier_msgs/msg/detail",
        action_fastrtps / "msg/detail",
    )
    for message_root in message_fastrtps_roots:
        for source in message_root.glob("*__type_support_c.cpp"):
            rewrite(
                source,
                lambda text: prune_fastrtps_source(
                    text, [], [], "VELAROS_ACTION_MAX_GOALS"
                ),
            )

    for header in args.root.glob(
        "rosidl_generator_c/*/*/detail/*__functions.h"
    ):
        rewrite(header, prune_description_declarations)

    extract_hashes(args.description, args.root / "velaros_action_hashes.c")
    validate(args.root)
    print("Pruned VelaROS Action introspection and applied sequence bounds: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
