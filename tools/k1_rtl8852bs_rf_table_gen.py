#!/usr/bin/env python3
"""Select one RTL8852B radio A/B register image from the vendor RF table.

The vendor table array_mp_8852b_radio{a,b}[] is a conditional package: a
headline block keyed by {RFE type, chip cut version} followed by a body of
IF / ELSE IF / CHK / ELSE / END directives interleaved with register pairs.
Only the branches selected by the running board's RFE/CV are written to the
chip, so the whole table must never be shipped verbatim.

This host tool reproduces the vendor selection exactly:

  halrf_sel_headline_8852b()          headline match, five ordered cases
  halrf_config_8852b_radio_a_reg()    IF/ELSE IF/CHK/ELSE/END walk
  halrf_config_8852b_radio_b_reg()    same walk, radio B array

Reference: spacemit-com/linux-6.6 k1-bl-v2.2.y revision
31c449aeaad8c7759bc983ca0e26946e5b6746dc, files
drivers/net/wireless/realtek/rtl8852bs/phl/hal_g6/phy/rf/halrf_8852b/
{halrf_hwimg_8852b.c,halrf_hwimg_raw_data_8852b.h}.

The vendor header is not redistributed with this project.  Pass a local copy
with --source.  --emit-dir writes three generated GPL-2.0-only includes in the
style of chip/k1/k1_rtl8852bs_phy_reg_8852b.inc: the selected radio A image,
the selected radio B image, and the compact {RFE, CV, images} headline table
the driver replays at run time to prove the board needs the compiled image.
"""

# SPDX-License-Identifier: GPL-2.0-only

from __future__ import annotations

import argparse
import hashlib
import pathlib
import re
import sys

VENDOR_REVISION = "31c449aeaad8c7759bc983ca0e26946e5b6746dc"
VENDOR_PATH = (
    "drivers/net/wireless/realtek/rtl8852bs/phl/hal_g6/phy/rf/"
    "halrf_8852b/halrf_hwimg_raw_data_8852b.h"
)

# The same path split so every generated comment line stays inside 79 columns.
VENDOR_PATH_COMMENT = (
    "drivers/net/wireless/realtek/rtl8852bs/phl/hal_g6/phy/rf/\n"
    " * halrf_8852b/halrf_hwimg_raw_data_8852b.h"
)

OPCODE_HEADLINE = 0xF
OPCODE_IF = 0x8
OPCODE_ELSE_IF = 0x9
OPCODE_ELSE = 0xA
OPCODE_END = 0xB
OPCODE_CHK = 0x4
DONT_CARE = 0xFF

RF_DDIE_FLAG = 1 << 16
RF_DELAY_FIRST = 0xF9
RF_DELAY_LAST = 0xFE
RF_DATA_MAX = 0xFFFFF

ARRAY_NAMES = {
    "a": "array_mp_8852b_radioa",
    "b": "array_mp_8852b_radiob",
}


class TableError(RuntimeError):
    """The vendor table or the requested RFE/CV pair cannot be resolved."""


def parse_array(text, name):
    """Return one const u32 array from the vendor header as plain integers."""

    match = re.search(
        r"const\s+u32\s+" + re.escape(name) + r"\s*\[\s*\]\s*=\s*\{(.*?)\n\};",
        text,
        re.S,
    )
    if match is None:
        raise TableError("array %s not found in the vendor header" % name)

    body = match.group(1)
    if "//" in body or "/*" in body:
        raise TableError(
            "array %s contains comments; refusing to guess" % name
        )

    tokens = re.findall(r"0x([0-9a-fA-F]+)", body)
    values = [int(token, 16) for token in tokens]
    if len(values) % 2 != 0:
        raise TableError("array %s has an odd word count" % name)
    return values


def headline_size(array):
    """Return the word count of the leading headline block."""

    index = 0
    while index + 1 < len(array):
        if (array[index] >> 28) != OPCODE_HEADLINE:
            return index
        index += 2
    return index


def headlines(array):
    """Return (pair index, RFE parameter, CV parameter) for every headline."""

    size = headline_size(array)
    entries = []
    for index in range(0, size, 2):
        condition = array[index] & 0x0FFFFFFF
        entries.append(
            (index >> 1, (condition & 0x00FF0000) >> 16, condition & 0xFF)
        )
    return entries


def select_headline(array, rfe, cv):
    """Reproduce halrf_sel_headline_8852b().

    Return (headline size, selected pair index, vendor case name).
    """

    size = headline_size(array)
    if size == 0:
        return 0, 0, "no headline"

    target = ((rfe & 0xFF) << 16) | (cv & 0xFF)
    for index in range(0, size, 2):
        if (array[index] & 0x0FFFFFFF) == target:
            return size, index >> 1, "1 {RFE:Match, cv:Match}"

    target = ((rfe & 0xFF) << 16) | (DONT_CARE & 0xFF)
    for index in range(0, size, 2):
        if (array[index] & 0x0FFFFFFF) == target:
            return size, index >> 1, "2 {RFE:Match, cv:Dont_Care}"

    matched = None
    cv_max = 0
    for index, rfe_para, cv_para in headlines(array):
        if rfe_para == rfe and cv_para >= cv_max:
            cv_max = cv_para
            matched = index
    if matched is not None:
        return size, matched, "3 {RFE:Match, cv:Max_in_Table}"

    matched = None
    cv_max = 0
    for index, rfe_para, cv_para in headlines(array):
        if rfe_para == DONT_CARE and cv_para >= cv_max:
            cv_max = cv_para
            matched = index
    if matched is not None:
        return size, matched, "4 {RFE:Dont_Care, cv:Max_in_Table}"

    raise TableError(
        "no headline matches RFE 0x%02x CV %d; the vendor driver aborts here"
        % (rfe, cv)
    )


def walk(array, size, target):
    """Reproduce the IF/ELSE IF/CHK/ELSE/END walk and return the writes."""

    selected = []
    is_matched = True
    find_target = False
    parameter = 0
    index = size

    while index + 1 < len(array):
        first = array[index]
        second = array[index + 1]
        index += 2

        opcode = first >> 28
        if opcode in (OPCODE_IF, OPCODE_ELSE_IF):
            parameter = first & 0x0FFFFFFF
        elif opcode == OPCODE_ELSE:
            is_matched = False
            if not find_target:
                raise TableError(
                    "vendor walk aborts: no branch matched before ELSE at "
                    "word %d" % (index - 2)
                )
        elif opcode == OPCODE_END:
            is_matched = True
            find_target = False
        elif opcode == OPCODE_CHK:
            if find_target:
                is_matched = False
            elif parameter == target:
                is_matched = True
                find_target = True
            else:
                is_matched = False
                find_target = False
        elif is_matched:
            selected.append((first, second))

    return selected


def classify(address):
    if address & RF_DDIE_FLAG:
        return "ddie"
    if RF_DELAY_FIRST <= address <= RF_DELAY_LAST:
        return "pseudo"
    return "direct"


def summarize(selected):
    counts = {"direct": 0, "ddie": 0, "pseudo": 0}
    oversized = []
    for address, data in selected:
        counts[classify(address)] += 1
        if data > RF_DATA_MAX:
            oversized.append((address, data))

    digest = hashlib.sha256()
    for address, data in selected:
        digest.update(("%08x%08x" % (address, data)).encode("ascii"))

    return {
        "total": len(selected),
        "counts": counts,
        "oversized": oversized,
        "addresses": sorted({address for address, _ in selected}),
        "digest": digest.hexdigest(),
    }


INCLUDE_HEADER = """\
/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Generated by tools/k1_rtl8852bs_rf_table_gen.py from
 * %(array)s in:
 * spacemit-com/linux-6.6 k1-bl-v2.2.y revision
 * %(revision)s,
 * %(vendor_path)s.
 *
 * The vendor table is a conditional package.  This image is the branch the
 * vendor driver selects for RFE type 0x%(rfe)02x and chip CV %(cv)d:
 * halrf_sel_headline_8852b() case %(case)s
 * at headline pair %(headline)d, then the IF/ELSE IF/CHK/ELSE/END walk of
 * halrf_config_8852b_radio_%(path_id)s_reg() with cfg_target 0x%(target)08x.
 *
 * %(total)d register pairs: %(direct)d RF serial-interface writes and
 * %(ddie)d RF D-die writes.  D-die entries keep bit 16 set so the driver can
 * route them to the vendor BB aperture; every value is applied under the
 * vendor MASKRF mask 0x000fffff.
 *
 * Image sha256
 * %(digest)s.
 *
 * Regenerate, never hand-edit.  Do not add the conditional vendor table.
 */
"""


def emit_include(path, path_id, rfe, cv, case, headline, target, selected):
    """Write the generated include for one radio path."""

    stats = summarize(selected)
    header = INCLUDE_HEADER % {
        "array": ARRAY_NAMES[path_id],
        "revision": VENDOR_REVISION,
        "vendor_path": VENDOR_PATH_COMMENT,
        "digest": stats["digest"],
        "rfe": rfe,
        "cv": cv,
        "case": case,
        "headline": headline,
        "target": target,
        "path_id": path_id,
        "total": stats["total"],
        "direct": stats["counts"]["direct"],
        "ddie": stats["counts"]["ddie"],
    }

    lines = [header]
    for address, data in selected:
        lines.append("  {0x%05xu, 0x%05xu},\n" % (address, data))
    path.write_text("".join(lines), encoding="ascii")
    return stats


def report(path_id, array, size, headline, case, target, selected, stream):
    stats = summarize(selected)
    counts = stats["counts"]
    addresses = stats["addresses"]
    stream.write(
        "radio %s: %d words, %d headline pairs, %d body pairs\n"
        % (path_id.upper(), len(array), size // 2, (len(array) - size) // 2)
    )
    stream.write(
        "radio %s: headline case %s, pair %d, cfg_target 0x%08x\n"
        % (path_id.upper(), case, headline, target)
    )
    stream.write(
        "radio %s: %d writes (%d serial-interface, %d D-die, %d pseudo), "
        "%d distinct addresses 0x%05x..0x%05x\n"
        % (
            path_id.upper(),
            stats["total"],
            counts["direct"],
            counts["ddie"],
            counts["pseudo"],
            len(addresses),
            addresses[0] if addresses else 0,
            addresses[-1] if addresses else 0,
        )
    )
    stream.write("radio %s: sha256 %s\n" % (path_id.upper(), stats["digest"]))
    if counts["pseudo"]:
        stream.write(
            "radio %s: WARNING %d entries fall in the 0xf9..0xfe delay "
            "pseudo-address range\n" % (path_id.upper(), counts["pseudo"])
        )
    for address, data in stats["oversized"]:
        stream.write(
            "radio %s: WARNING address 0x%05x value 0x%08x exceeds MASKRF\n"
            % (path_id.upper(), address, data)
        )
    return stats


def survey(text, path_id, stream):
    """Print every headline and the image each one selects."""

    array = parse_array(text, ARRAY_NAMES[path_id])
    size = headline_size(array)
    groups = {}
    stream.write(
        "radio %s: %d headline pairs\n" % (path_id.upper(), size // 2)
    )
    for headline, rfe_para, cv_para in headlines(array):
        target = array[headline << 1] & 0x0FFFFFFF
        try:
            selected = walk(array, size, target)
        except TableError as error:
            stream.write(
                "  pair %2d RFE 0x%02x CV %d: %s\n"
                % (headline, rfe_para, cv_para, error)
            )
            continue
        stats = summarize(selected)
        stream.write(
            "  pair %2d RFE 0x%02x CV %d target 0x%08x: %4d writes "
            "(%4d SI, %3d D-die) sha256 %s\n"
            % (
                headline,
                rfe_para,
                cv_para,
                target,
                stats["total"],
                stats["counts"]["direct"],
                stats["counts"]["ddie"],
                stats["digest"][:16],
            )
        )
        groups.setdefault(stats["digest"], []).append((rfe_para, cv_para))

    stream.write(
        "radio %s: %d distinct images\n" % (path_id.upper(), len(groups))
    )
    for digest, members in groups.items():
        stream.write(
            "  %s: %s\n"
            % (
                digest[:16],
                " ".join("RFE0x%02x/CV%d" % pair for pair in members),
            )
        )



HEADLINE_HEADER = """\
/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Generated by tools/k1_rtl8852bs_rf_table_gen.py from the headline blocks of
 * array_mp_8852b_radioa[] and array_mp_8852b_radiob[] in:
 * spacemit-com/linux-6.6 k1-bl-v2.2.y revision
 * %(revision)s,
 * %(vendor_path)s.
 *
 * One row per vendor headline in table order: {RFE type, chip CV, images}.
 * The third field records which generated image that headline selects: bit 0
 * radio A, bit 1 radio B.  A set bit means the branch the vendor walk takes
 * for that headline is byte-identical to the image generated beside this
 * file.  Replaying halrf_sel_headline_8852b() at run time against the real
 * board RFE/CV therefore either lands on 0x3 or proves the compiled image is
 * wrong for this board.
 *
 * Generated for RFE type 0x%(rfe)02x and chip CV %(cv)d.
 * radio A image sha256
 * %(digest_a)s.
 * radio B image sha256
 * %(digest_b)s.
 *
 * Regenerate, never hand-edit.  Do not add the conditional vendor table.
 */
"""


def headline_images(text):
    """Digest the image every headline selects, for both radio paths."""

    tables = {}
    for path_id in ("a", "b"):
        array = parse_array(text, ARRAY_NAMES[path_id])
        size = headline_size(array)
        entries = []
        for index, rfe_para, cv_para in headlines(array):
            target = array[index << 1] & 0x0FFFFFFF
            try:
                selected = walk(array, size, target)
            except TableError:
                digest = None
            else:
                digest = summarize(selected)["digest"]
            entries.append((rfe_para, cv_para, digest))
        tables[path_id] = entries

    keys_a = [(rfe, cv) for rfe, cv, _ in tables["a"]]
    keys_b = [(rfe, cv) for rfe, cv, _ in tables["b"]]
    if keys_a != keys_b:
        raise TableError(
            "radio A and radio B headline blocks differ; one shared runtime "
            "guard table cannot describe both"
        )

    return tables


def emit_headlines(path, text, rfe, cv, digests):
    """Write the compact runtime guard table for halrf_sel_headline_8852b()."""

    tables = headline_images(text)
    rows = []
    for entry_a, entry_b in zip(tables["a"], tables["b"]):
        rfe_para, cv_para, digest_a = entry_a
        flags = 0
        if digest_a is not None and digest_a == digests["a"]:
            flags |= 1
        if entry_b[2] is not None and entry_b[2] == digests["b"]:
            flags |= 2
        rows.append((rfe_para, cv_para, flags))

    if not any(flags == 3 for _, _, flags in rows):
        raise TableError(
            "no headline reproduces both generated images; the guard table "
            "would reject every board"
        )

    lines = [
        HEADLINE_HEADER % {
            "revision": VENDOR_REVISION,
            "vendor_path": VENDOR_PATH_COMMENT,
            "rfe": rfe,
            "cv": cv,
            "digest_a": digests["a"],
            "digest_b": digests["b"],
        }
    ]
    for rfe_para, cv_para, flags in rows:
        lines.append(
            "  {0x%02xu, 0x%02xu, 0x%xu},\n" % (rfe_para, cv_para, flags)
        )
    path.write_text("".join(lines), encoding="ascii")
    return rows


def parse_int(text):
    return int(text, 0)


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Select the RTL8852B radio A/B image for one RFE/CV pair."
    )
    parser.add_argument(
        "--source",
        required=True,
        type=pathlib.Path,
        help="local copy of the vendor halrf_hwimg_raw_data_8852b.h",
    )
    parser.add_argument(
        "--path",
        choices=("a", "b", "both"),
        default="both",
        help="radio path to process (default: both)",
    )
    parser.add_argument(
        "--rfe",
        type=parse_int,
        help="RFE type reported by the board (logical eFuse offset 0x2ca)",
    )
    parser.add_argument(
        "--cv",
        type=parse_int,
        help="chip cut version, CAV=0, from R_AX_SYS_CFG1 bits 15:12",
    )
    parser.add_argument(
        "--emit-dir",
        type=pathlib.Path,
        help="directory that receives the generated .inc files",
    )
    parser.add_argument(
        "--survey",
        action="store_true",
        help="list every headline and the image it selects, then exit",
    )
    arguments = parser.parse_args(argv)

    text = arguments.source.read_text(encoding="utf-8", errors="replace")
    path_ids = ("a", "b") if arguments.path == "both" else (arguments.path,)

    try:
        if arguments.survey:
            for path_id in path_ids:
                survey(text, path_id, sys.stdout)
            return 0

        if arguments.rfe is None or arguments.cv is None:
            parser.error("--rfe and --cv are required unless --survey is used")

        if arguments.emit_dir is not None and arguments.path != "both":
            parser.error("--emit-dir needs --path both for the guard table")

        digests = {}
        for path_id in path_ids:
            array = parse_array(text, ARRAY_NAMES[path_id])
            size, headline, case = select_headline(
                array, arguments.rfe, arguments.cv
            )
            target = array[headline << 1] & 0x0FFFFFFF if size else 0
            selected = walk(array, size, target)
            report(
                path_id, array, size, headline, case, target, selected,
                sys.stdout,
            )

            digests[path_id] = summarize(selected)["digest"]

            if arguments.emit_dir is not None:
                output = arguments.emit_dir / (
                    "k1_rtl8852bs_rf_radio_%s_8852b.inc" % path_id
                )
                emit_include(
                    output,
                    path_id,
                    arguments.rfe,
                    arguments.cv,
                    case,
                    headline,
                    target,
                    selected,
                )
                sys.stdout.write(
                    "radio %s: wrote %s\n" % (path_id.upper(), output)
                )

        if arguments.emit_dir is not None:
            output = arguments.emit_dir / "k1_rtl8852bs_rf_headline_8852b.inc"
            rows = emit_headlines(
                output, text, arguments.rfe, arguments.cv, digests
            )
            sys.stdout.write(
                "guard: wrote %s, %d headlines, %d select both images\n"
                % (
                    output,
                    len(rows),
                    sum(1 for _, _, flags in rows if flags == 3),
                )
            )
    except TableError as error:
        sys.stderr.write("error: %s\n" % error)
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
