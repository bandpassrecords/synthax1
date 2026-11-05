#!/usr/bin/env python3
"""Inspect Sylenth1 .fxp/.fxb files and assemble new banks from existing presets.

Mirrors src/sylenth/SylenthFormat.cpp. Values stay in Sylenth1's normalized form, so built
banks load in Sylenth1 itself and in Synthax1.

  sylenth_bank.py info    FILE...                         container, layout, program count
  sylenth_bank.py list    FILE... [--all]                 numbered program names (Init slots hidden)
  sylenth_bank.py dump    FILE [--program N]              raw normalized field values
  sylenth_bank.py extract BANK.fxb OUTDIR [--all]         one .fxp per program
  sylenth_bank.py build   OUT.fxb SOURCE... [--no-pad] [--force]
                                                          new bank from selected programs,
                                                          filled to 512 slots with Init

A SOURCE is a .fxp, a whole .fxb, or a selection from a bank (1-based, as `list` shows):
  pads.fxb:3          program 3
  pads.fxb:1-8,12     ranges and lists
  pads.fxb:/warm/     programs whose name matches a regex (case-insensitive)
  pads.fxb:3=New Name rename while selecting a single program
Selecting a whole bank skips its Init slots.

Chunk layout (little-endian), established from 264 real files:
  '1lys' version programCount fieldsPerProgram currentProgram
  programCount x (float fields[fieldsPerProgram], char name[36])
Version 3045 wraps that chunk as '1lys' version + zlib stream; 3031 and older do not.
"""

from __future__ import annotations

import argparse
import re
import struct
import sys
import zlib
from dataclasses import dataclass, field, replace
from pathlib import Path

BASE_FIELDS = 242
NAME_BYTES = 36
HEADER_NAME_BYTES = 28
PROGRAMS_PER_BANK = 512
MAX_BYTES = 64 << 20
SOURCE_DIR = Path(__file__).resolve().parent.parent / "src" / "sylenth"


def field_names() -> list[str]:
    names = re.findall(r"^X\((\w+),", (SOURCE_DIR / "SylenthFields.inc").read_text(encoding="utf-8"), re.M)
    if len(names) != BASE_FIELDS:
        raise SystemExit(f"SylenthFields.inc lists {len(names)} fields, expected {BASE_FIELDS}")
    return names


@dataclass
class Patch:
    name: str
    values: list[float]                               # the 242 base fields
    extra: list[float] = field(default_factory=list)  # later fields verbatim; extra[0] is ZEQ mode


def init_patch() -> Patch:
    text = (SOURCE_DIR / "SylenthInit.inc").read_text(encoding="utf-8")
    values = [float(v) for v in re.findall(r"(-?[0-9.]+(?:e-?\d+)?)f", text)]
    if len(values) != BASE_FIELDS:
        raise SystemExit(f"SylenthInit.inc holds {len(values)} values, expected {BASE_FIELDS}")
    return Patch("Init", values, [0.0, 0.0])


@dataclass
class Info:
    kind: str = "FBCh"
    fxp_version: int = 1
    fx_version: int = 1
    outer_current: int = 0
    chunk_version: int = 3045
    fields: int = 244
    current: int = 0
    compressed: bool = True
    tail: bytes = b""


@dataclass
class Document:
    info: Info
    patches: list[Patch]


class FormatError(Exception):
    pass


def c_string(raw: bytes) -> str:
    return raw.split(b"\0", 1)[0].decode("latin-1")


def plausible(chunk: bytes) -> bool:
    if len(chunk) < 20 or chunk[:4] != b"1lys":
        return False
    _, count, fields, _ = struct.unpack_from("<4i", chunk, 4)
    return 0 < count <= 4096 and BASE_FIELDS <= fields <= 4096 and 20 + count * (fields * 4 + NAME_BYTES) <= len(chunk)


def expand(chunk: bytes, info: Info) -> bytes:
    if len(chunk) < 20 or chunk[:4] != b"1lys":
        raise FormatError("chunk does not start with the Sylenth1 signature")
    info.compressed = False
    if plausible(chunk):
        return chunk
    inflater = zlib.decompressobj()
    try:
        inner = inflater.decompress(chunk[8:], MAX_BYTES)
    except zlib.error as error:
        raise FormatError(f"Sylenth1 chunk header is inconsistent and the body is not zlib ({error})") from None
    if inflater.unconsumed_tail or not inflater.eof or not plausible(inner):
        raise FormatError("compressed Sylenth1 chunk is incomplete, too large or inconsistent")
    info.compressed = True
    return inner


def parse_chunk(chunk: bytes, info: Info) -> list[Patch]:
    plain = expand(chunk, info)
    info.chunk_version, count, info.fields, info.current = struct.unpack_from("<4i", plain, 4)
    record = info.fields * 4 + NAME_BYTES
    patches, offset = [], 20
    for _ in range(count):
        values = list(struct.unpack_from(f"<{info.fields}f", plain, offset))
        name = c_string(plain[offset + info.fields * 4: offset + record])
        patches.append(Patch(name, values[:BASE_FIELDS], values[BASE_FIELDS:]))
        offset += record
    info.tail = plain[offset:]
    return patches


def read(data: bytes) -> Document:
    if len(data) < 60 or data[:4] != b"CcnK":
        raise FormatError("not an .fxp/.fxb file")
    _, magic, fxp_version, plugin_id, fx_version, _count = struct.unpack_from(">I4si4sii", data, 4)
    if plugin_id != b"syl1":
        raise FormatError(f"file belongs to plugin '{plugin_id.decode('latin-1')}', not Sylenth1")
    info = Info(kind=magic.decode("latin-1"), fxp_version=fxp_version, fx_version=fx_version)
    if magic == b"FBCh":
        if fxp_version >= 2:
            (info.outer_current,) = struct.unpack_from(">i", data, 28)
        (size,) = struct.unpack_from(">I", data, 156)
        return Document(info, parse_chunk(data[160:160 + size], info))
    if magic == b"FPCh":
        (size,) = struct.unpack_from(">I", data, 56)
        patches = parse_chunk(data[60:60 + size], info)
        if len(patches) != 1:
            raise FormatError(f"program file holds {len(patches)} programs")
        patches[0].name = patches[0].name or c_string(data[28:56])
        return Document(info, patches)
    if magic in (b"FxCk", b"FxBk"):
        raise FormatError("parameter-list files are read by the plugin; this tool reads chunk files only")
    raise FormatError(f"unsupported container '{magic.decode('latin-1')}'")


def chunk_bytes(patches: list[Patch], info: Info) -> bytes:
    fields = max(info.fields, BASE_FIELDS)
    init = init_patch()
    out = [b"1lys", struct.pack("<4i", info.chunk_version, len(patches), fields, info.current)]
    for patch in patches:
        extra = [patch.extra[i] if i < len(patch.extra) else (init.extra[i] if i < len(init.extra) else 0.0)
                 for i in range(fields - BASE_FIELDS)]
        out.append(struct.pack(f"<{fields}f", *patch.values, *extra))
        out.append(patch.name.encode("latin-1", "replace")[:NAME_BYTES - 1].ljust(NAME_BYTES, bytes(1)))
    out.append(info.tail)
    plain = b"".join(out)
    if not info.compressed:
        return plain
    return b"1lys" + struct.pack("<i", info.chunk_version) + zlib.compress(plain, 6)


def container(magic: bytes, info: Info, count: int, body: bytes) -> bytes:
    rest = magic + struct.pack(">i", info.fxp_version) + b"syl1" + struct.pack(">ii", info.fx_version, count) + body
    return b"CcnK" + struct.pack(">I", len(rest)) + rest


def write_bank(patches: list[Patch], info: Info) -> bytes:
    future = struct.pack(">i", info.outer_current if info.fxp_version >= 2 else 0).ljust(128, bytes(1))
    chunk = chunk_bytes(patches, info)
    return container(b"FBCh", info, len(patches), future + struct.pack(">I", len(chunk)) + chunk)


def write_program(patch: Patch, info: Info) -> bytes:
    name = patch.name.encode("latin-1", "replace")[:HEADER_NAME_BYTES - 1].ljust(HEADER_NAME_BYTES, bytes(1))
    chunk = chunk_bytes([patch], info)
    return container(b"FPCh", info, max(info.fields, BASE_FIELDS), name + struct.pack(">I", len(chunk)) + chunk)


def info_for_sources(sources: list[Info], kind: str) -> Info:
    if not sources:
        return Info(kind=kind)
    return Info(kind=kind, fxp_version=sources[-1].fxp_version, fx_version=sources[-1].fx_version,
                chunk_version=max(s.chunk_version for s in sources), fields=max(s.fields for s in sources),
                compressed=any(s.compressed for s in sources))


# ---------------------------------------------------------------- command line
def load(path: Path) -> Document:
    try:
        if path.stat().st_size > MAX_BYTES:
            raise FormatError("file is larger than 64 MB")
        return read(path.read_bytes())
    except (FormatError, OSError) as error:
        raise SystemExit(f"{path}: {error}") from None


def is_filler(patch: Patch) -> bool:
    return patch.name in ("Init", "")


def parse_selection(spec: str, patches: list[Patch]) -> list[int]:
    if spec.startswith("/") and spec.endswith("/") and len(spec) > 2:
        pattern = re.compile(spec[1:-1], re.I)
        return [i for i, p in enumerate(patches) if pattern.search(p.name) and not is_filler(p)]
    picked: list[int] = []
    for part in spec.split(","):
        bounds = [b.strip() for b in part.split("-")]
        if len(bounds) > 2 or not all(b.isdigit() for b in bounds):
            raise SystemExit(f"bad selection '{part}'")
        first, last = int(bounds[0]), int(bounds[-1])
        if not 1 <= first <= last <= len(patches):
            raise SystemExit(f"selection '{part}' is outside 1..{len(patches)}")
        picked.extend(range(first - 1, last))
    return picked


def split_source(source: str) -> tuple[str, str]:
    """'bank.fxb:1-4' -> ('bank.fxb', '1-4'); a Windows drive colon is not a separator."""
    drive = ""
    if re.match(r"^[A-Za-z]:[\\/]", source):
        drive, source = source[:2], source[2:]
    path_text, _, selection = source.partition(":")
    return drive + path_text, selection


def resolve_source(source: str) -> tuple[list[Patch], Info]:
    path_text, selection = split_source(source)
    doc = load(Path(path_text))
    rename = None
    if "=" in selection:
        selection, rename = selection.split("=", 1)
    if doc.info.kind == "FPCh":
        indices = [0]  # a program file is always wanted, whatever its program is called
        if is_filler(doc.patches[0]):
            doc.patches[0].name = Path(path_text).stem  # vendors often ship programs still named "Init"
    elif selection:
        indices = parse_selection(selection, doc.patches)
    else:
        indices = [i for i, p in enumerate(doc.patches) if not is_filler(p)]
    if rename is not None and len(indices) != 1:
        raise SystemExit(f"'{source}': rename needs exactly one selected program")
    picked = [replace(doc.patches[i], values=list(doc.patches[i].values), extra=list(doc.patches[i].extra)) for i in indices]
    if rename is not None:
        picked[0].name = rename
    return picked, doc.info


def command_info(args) -> None:
    for path in args.files:
        doc = load(path)
        i = doc.info
        used = sum(not is_filler(p) for p in doc.patches)
        print(f"{path}: {i.kind}, chunk v{i.chunk_version}{' zlib' if i.compressed else ''}, {i.fields} fields, "
              f"{len(doc.patches)} slots ({used} in use), selected {i.current + 1}"
              + (f", {len(i.tail)} trailing bytes" if i.tail else ""))


def command_list(args) -> None:
    for path in args.files:
        doc = load(path)
        print(f"# {path}")
        for index, patch in enumerate(doc.patches, 1):
            if args.all or not is_filler(patch):
                print(f"{index:4d}  {patch.name}")


def command_dump(args) -> None:
    doc = load(args.file)
    if not 1 <= args.program <= len(doc.patches):
        raise SystemExit(f"program must be 1..{len(doc.patches)}")
    patch = doc.patches[args.program - 1]
    print(f"# {patch.name}")
    for name, value in zip(field_names(), patch.values):
        print(f"{name:20s} {value:.6f}")
    for index, value in enumerate(patch.extra):
        print(f"{'ZEQMode' if index == 0 else f'Extra{index}':20s} {value:.6f}")


def safe_filename(name: str) -> str:
    return re.sub(r'[<>:"/\\|?*\x00-\x1f]', "_", name).strip(" .") or "Untitled"


def command_extract(args) -> None:
    doc = load(args.bank)
    args.outdir.mkdir(parents=True, exist_ok=True)
    info = replace(doc.info, kind="FPCh", tail=b"", current=0)
    written = 0
    for index, patch in enumerate(doc.patches, 1):
        if is_filler(patch) and not args.all:
            continue
        (args.outdir / f"{index:03d} {safe_filename(patch.name)}.fxp").write_bytes(write_program(patch, info))
        written += 1
    print(f"wrote {written} program(s) to {args.outdir}")


def command_build(args) -> None:
    if args.output.exists() and not args.force:
        raise SystemExit(f"{args.output} exists; pass --force to replace it")
    patches: list[Patch] = []
    infos: list[Info] = []
    for source in args.sources:
        picked, source_info = resolve_source(source)
        patches.extend(picked)
        infos.append(source_info)
    if not patches:
        raise SystemExit("selection is empty")
    if len(patches) > PROGRAMS_PER_BANK:
        raise SystemExit(f"{len(patches)} programs selected; a Sylenth1 bank holds {PROGRAMS_PER_BANK}")
    used = len(patches)
    if not args.no_pad:
        patches += [init_patch() for _ in range(PROGRAMS_PER_BANK - len(patches))]
    info = info_for_sources(infos, "FBCh")
    data = write_bank(patches, info)
    temporary = args.output.with_name(args.output.name + ".tmp")
    temporary.write_bytes(data)
    temporary.replace(args.output)
    print(f"wrote {args.output}: {used} program(s)" + ("" if args.no_pad else f", {len(patches) - used} Init slot(s)")
          + f", chunk v{info.chunk_version}{' zlib' if info.compressed else ''}")
    for index, patch in enumerate(patches[:used], 1):
        print(f"{index:4d}  {patch.name}")


def main(argv: list[str] | None = None) -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    sub = commands.add_parser("info")
    sub.add_argument("files", nargs="+", type=Path)
    sub.set_defaults(handler=command_info)
    sub = commands.add_parser("list")
    sub.add_argument("files", nargs="+", type=Path)
    sub.add_argument("--all", action="store_true", help="include Init slots")
    sub.set_defaults(handler=command_list)
    sub = commands.add_parser("dump")
    sub.add_argument("file", type=Path)
    sub.add_argument("--program", type=int, default=1)
    sub.set_defaults(handler=command_dump)
    sub = commands.add_parser("extract")
    sub.add_argument("bank", type=Path)
    sub.add_argument("outdir", type=Path)
    sub.add_argument("--all", action="store_true", help="include Init slots")
    sub.set_defaults(handler=command_extract)
    sub = commands.add_parser("build")
    sub.add_argument("output", type=Path)
    sub.add_argument("sources", nargs="+")
    sub.add_argument("--no-pad", action="store_true", help="write only the selected programs")
    sub.add_argument("--force", action="store_true")
    sub.set_defaults(handler=command_build)
    args = parser.parse_args(argv)
    args.handler(args)


if __name__ == "__main__":
    main()
