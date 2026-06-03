#!/usr/bin/env python3
"""
Patch ChapProcess chapter data into an MKV file.

Strategy:
- Reads the original SeekHead to find where Info, Tracks, Cues, Tags live
- Inserts Chapters immediately after SeekHead+Void (where mkvpropedit puts them)
- Rebuilds the entire SeekHead with all offsets corrected for the insertion
- Writes the new SeekHead + Void into the space formerly occupied by old SeekHead + Void
- Fixes the Segment size field

This produces a clean file with no broken SeekHead entries.

Usage:
    python3 patch_mkv_chapters.py source.mkv bandersnatch.json SegmentMap.json [output.mkv]
"""

import sys
import os
import json
import struct
import random

# ── EBML element IDs ──────────────────────────────────────────────
ID_EBML                = 0x1A45DFA3
ID_Segment             = 0x18538067
ID_SeekHead            = 0x114D9B74
ID_Void                = 0xEC
ID_Chapters            = 0x1043A770

ID_EditionEntry        = 0x45B9
ID_EditionUID          = 0x45BC
ID_EditionFlagHidden   = 0x45BD
ID_EditionFlagDefault  = 0x45DB
ID_EditionFlagOrdered  = 0x45DD
ID_ChapterAtom         = 0xB6
ID_ChapterUID          = 0x73C4   # 0x73C4 per ffprobe/ffmpeg; spec says 0x73C5
ID_ChapterStringUID    = 0x5654
ID_ChapterTimeStart    = 0x91
ID_ChapterTimeEnd      = 0x92
ID_ChapterFlagHidden   = 0x98
ID_ChapterFlagEnabled  = 0x4598
ID_ChapterDisplay      = 0x80
ID_ChapterString       = 0x85
ID_ChapterLanguage     = 0x437C
ID_ChapProcess         = 0x6944
ID_ChapProcessCodecID  = 0x6955
ID_ChapProcessCommand  = 0x6911
ID_ChapProcessTime     = 0x6922
ID_ChapProcessData     = 0x6933

# Known SeekHead element IDs for display
KNOWN_IDS = {
    bytes([0x15,0x49,0xa9,0x66]): "Info",
    bytes([0x16,0x54,0xae,0x6b]): "Tracks",
    bytes([0x1c,0x53,0xbb,0x6b]): "Cues",
    bytes([0x12,0x54,0xc3,0x67]): "Tags",
    bytes([0x10,0x43,0xa7,0x70]): "Chapters",
    bytes([0x11,0x4d,0x9b,0x74]): "SeekHead",
}

# ── EBML encoding ─────────────────────────────────────────────────

def vint(value):
    if value < 0x7F:
        return bytes([value | 0x80])
    elif value < 0x3FFF:
        return struct.pack('>H', value | 0x4000)
    elif value < 0x1FFFFF:
        return struct.pack('>I', value | 0x200000)[1:]
    elif value < 0x0FFFFFFF:
        return struct.pack('>I', value | 0x10000000)
    elif value < 0x07FFFFFFFF:
        return struct.pack('>Q', value | 0x0800000000)[3:]
    elif value < 0x03FFFFFFFFFF:
        return struct.pack('>Q', value | 0x040000000000)[2:]
    elif value < 0x01FFFFFFFFFFFF:
        return struct.pack('>Q', value | 0x02000000000000)[1:]
    else:
        return struct.pack('>Q', value | 0x0100000000000000)

def vint_size(value):
    if value < 0x7F:        return 1
    elif value < 0x3FFF:    return 2
    elif value < 0x1FFFFF:  return 3
    elif value < 0x0FFFFFFF: return 4
    elif value < 0x07FFFFFFFF: return 5
    else:                   return 6

def eid(element_id):
    if element_id <= 0xFF:
        return bytes([element_id])
    elif element_id <= 0xFFFF:
        return struct.pack('>H', element_id)
    elif element_id <= 0xFFFFFF:
        return struct.pack('>I', element_id)[1:]
    else:
        return struct.pack('>I', element_id)

def eid_size(element_id):
    if element_id <= 0xFF:      return 1
    elif element_id <= 0xFFFF:  return 2
    elif element_id <= 0xFFFFFF: return 3
    else:                        return 4

def el(element_id, data):
    return eid(element_id) + vint(len(data)) + data

def el_uint(element_id, value, ms=1):
    size = max(ms, (value.bit_length() + 7) // 8) if value > 0 else ms
    return el(element_id, value.to_bytes(size, 'big'))

def el_str(element_id, text):
    return el(element_id, text.encode('utf-8'))

def el_bin(element_id, data):
    return el(element_id, data)

# ── EBML decoding ─────────────────────────────────────────────────

def read_vint(f):
    first = f.read(1)
    if not first: return None, 0
    b = first[0]
    if b & 0x80: return b & 0x7F, 1
    elif b & 0x40: return ((b & 0x3F) << 8) | f.read(1)[0], 2
    elif b & 0x20:
        rest = f.read(2)
        return ((b & 0x1F) << 16) | (rest[0] << 8) | rest[1], 3
    elif b & 0x10:
        rest = f.read(3)
        return ((b & 0x0F) << 24) | (rest[0] << 16) | (rest[1] << 8) | rest[2], 4
    elif b & 0x08:
        rest = f.read(4)
        val = (b & 0x07)
        for byte in rest: val = (val << 8) | byte
        return val, 5
    elif b & 0x04:
        rest = f.read(5)
        val = (b & 0x03)
        for byte in rest: val = (val << 8) | byte
        return val, 6
    elif b & 0x02:
        rest = f.read(6)
        val = (b & 0x01)
        for byte in rest: val = (val << 8) | byte
        return val, 7
    else:
        rest = f.read(7)
        val = 0
        for byte in rest: val = (val << 8) | byte
        return val, 8

def read_element_id(f):
    first = f.read(1)
    if not first: return None, 0
    b = first[0]
    if b & 0x80: return b, 1
    elif b & 0x40: return (b << 8) | f.read(1)[0], 2
    elif b & 0x20:
        rest = f.read(2)
        return (b << 16) | (rest[0] << 8) | rest[1], 3
    else:
        rest = f.read(3)
        return (b << 24) | (rest[0] << 16) | (rest[1] << 8) | rest[2], 4

# ── SeekHead parser ───────────────────────────────────────────────

def parse_seekhead(data):
    """
    Parse SeekHead content bytes.
    Returns list of (id_bytes, relative_position) tuples.
    """
    entries = []
    pos = 0
    while pos < len(data):
        if pos >= len(data): break
        b = data[pos]
        if b & 0x80: seek_id = b; idl = 1
        elif b & 0x40: seek_id = (b << 8) | data[pos+1]; idl = 2
        else: break
        pos += idl

        b = data[pos]
        if b & 0x80: sz = b & 0x7F; szl = 1
        elif b & 0x40: sz = ((b & 0x3F) << 8) | data[pos+1]; szl = 2
        else: sz = b & 0x7F; szl = 1
        pos += szl

        if seek_id != 0x4DBB:  # not a Seek element
            pos += sz
            continue

        content = data[pos:pos+sz]
        pos += sz

        # Parse SeekID and SeekPosition from content
        seek_id_bytes = None
        seek_pos = None
        spos = 0
        while spos < len(content):
            b = content[spos]
            if b == 0x53:
                b2 = content[spos+1]
                if b2 == 0xAB:  # SeekID
                    spos += 2
                    b = content[spos]
                    if b & 0x80: ssz = b & 0x7F; spos += 1
                    elif b & 0x40: ssz = ((b & 0x3F) << 8) | content[spos+1]; spos += 2
                    else: ssz = b & 0x7F; spos += 1
                    seek_id_bytes = bytes(content[spos:spos+ssz])
                    spos += ssz
                elif b2 == 0xAC:  # SeekPosition
                    spos += 2
                    b = content[spos]
                    if b & 0x80: ssz = b & 0x7F; spos += 1
                    elif b & 0x40: ssz = ((b & 0x3F) << 8) | content[spos+1]; spos += 2
                    else: ssz = b & 0x7F; spos += 1
                    seek_pos = int.from_bytes(content[spos:spos+ssz], 'big')
                    spos += ssz
                else:
                    spos += 1
            else:
                spos += 1

        if seek_id_bytes is not None and seek_pos is not None:
            entries.append((seek_id_bytes, seek_pos))

    return entries

def build_seek_entry(id_bytes, relative_pos):
    """Build a single Seek element."""
    seek_id_el  = el_bin(0x53AB, id_bytes)
    pos_bytes   = relative_pos.to_bytes(max(1, (relative_pos.bit_length()+7)//8), 'big')
    seek_pos_el = el_bin(0x53AC, pos_bytes)
    return el(0x4DBB, seek_id_el + seek_pos_el)

def build_seekhead(entries):
    """Build a complete SeekHead element from list of (id_bytes, pos) tuples."""
    content = b""
    for id_bytes, pos in entries:
        content += build_seek_entry(id_bytes, pos)
    return el(ID_SeekHead, content)

def chapters_id_bytes():
    return bytes([0x10, 0x43, 0xA7, 0x70])

def content_size_for_void_total(total):
    """Find content_size such that Void element exactly fills total bytes."""
    void_id_sz = eid_size(ID_Void)
    for c in range(total - void_id_sz - 1, -1, -1):
        if void_id_sz + vint_size(c) + c == total:
            return c
    return None

# ── Scan source to get original SeekHead info ─────────────────────

def scan_source(filepath):
    """
    Read the source file's SeekHead and surrounding structure.
    Returns dict with:
      segment_body_start, seekhead_offset, seekhead_total,
      void_offset, void_total, insertion_offset,
      original_entries [(id_bytes, rel_pos)]
    """
    with open(filepath, 'rb') as f:
        # Skip EBML header
        elem_id, id_len = read_element_id(f)
        size, size_len = read_vint(f)
        f.seek(id_len + size_len + size)

        # Segment
        seg_id, seg_id_len = read_element_id(f)
        seg_size, seg_size_len = read_vint(f)
        segment_body_start = f.tell()

        # SeekHead
        sh_offset = f.tell()
        sh_id, sh_id_len = read_element_id(f)
        if sh_id != ID_SeekHead:
            raise ValueError(f"Expected SeekHead at {sh_offset}, got 0x{sh_id:X}")
        sh_size, sh_size_len = read_vint(f)
        sh_header_size = sh_id_len + sh_size_len
        sh_content = f.read(sh_size)
        sh_total = sh_header_size + sh_size

        # Skip Void elements
        while True:
            elem_offset = f.tell()
            elem_id, id_len = read_element_id(f)
            if elem_id != ID_Void:
                insertion_offset = elem_offset
                break
            size, size_len = read_vint(f)
            f.seek(elem_offset + id_len + size_len + size)

        # Find the Void total (everything between end of SeekHead and insertion_offset)
        void_total = insertion_offset - (sh_offset + sh_total)

        entries = parse_seekhead(sh_content)

        return {
            'segment_body_start': segment_body_start,
            'seekhead_offset': sh_offset,
            'seekhead_total': sh_total,
            'void_total': void_total,
            'insertion_offset': insertion_offset,
            'original_entries': entries,
        }

# ── Script generation ─────────────────────────────────────────────

STRING_ENUMS = {
    "p_ps": {"n": 0, "b": 1, "f": 2, "t": 3},
    "p_pc": {"n": 0, "o": 1, "t": 2},
    "p_vs": {"n": 0, "c": 1, "k": 2, "t": 3},
}

def str_to_int(varname, strval):
    return STRING_ENUMS.get(varname, {}).get(strval, 0)

def translate_precondition(cond):
    if not cond: return "1"
    op = cond[0]
    if op == "persistentState": return f"{cond[1]} != 0"
    if op == "not":
        inner = cond[1] if len(cond) == 2 else ["and"] + list(cond[1:])
        return f"!({translate_precondition(inner)})"
    if op == "and":
        return " && ".join(f"({translate_precondition(c)})" for c in cond[1:])
    if op == "or":
        return " || ".join(f"({translate_precondition(c)})" for c in cond[1:])
    if op == "eql":
        lc = cond[1]; right = cond[2]
        if lc[0] == "persistentState":
            var = lc[1]
            if isinstance(right, bool): return f"{var} == {'1' if right else '0'}"
            elif isinstance(right, str): return f"{var} == {str_to_int(var, right)}  // \"{right}\""
            else: return f"{var} == {right}"
        return f"{translate_precondition(lc)} == {right}"
    return "1"

def state_to_lets(d, indent="    "):
    lines = []
    for var, val in d.items():
        if isinstance(val, bool):
            lines.append(f"{indent}Let({var}, {'1' if val else '0'});")
        elif isinstance(val, str):
            lines.append(f"{indent}Let({var}, {str_to_int(var, val)}); // \"{val}\"")
        elif isinstance(val, (int, float)):
            lines.append(f"{indent}Let({var}, {int(val)});")
    return lines

def build_scripts(seg_id, seg_data, seg_moments, preconditions, segmentGroups, uid_map):
    entry_lines = []
    leave_lines = []

    for m in seg_moments:
        if m["type"] != "notification:playbackImpression": continue
        persistent = m.get("impressionData", {}).get("data", {}).get("persistent", {})
        if not persistent: continue
        pre = m.get("precondition", [])
        lets = state_to_lets(persistent)
        if pre:
            entry_lines.append(f"    if ({translate_precondition(pre)}) {{")
            entry_lines += [f"    {l}" for l in lets]
            entry_lines.append("    }")
        else:
            entry_lines += lets

    choice_moment = next((m for m in seg_moments if m["type"] == "scene:cs_bs"), None)
    extra_blocks = []

    if choice_moment:
        choices = choice_moment.get("choices", [])
        ui_display = choice_moment.get("uiDisplayMS", seg_data.get("endTimeMs", 0))
        ui_hide = choice_moment.get("uiHideMS", ui_display + 10000)
        default_idx = choice_moment.get("defaultChoiceIndex", 0)
        timeout_s = max(1, int((ui_hide - ui_display) / 1000))
        option_lines = []

        for i, choice in enumerate(choices, 1):
            text = choice.get("text", f"Option {i}").title().replace('"', "'")
            cimp = choice.get("impressionData", {}).get("data", {}).get("persistent", {})

            if "segmentId" in choice:
                target = choice["segmentId"]
                tuid = uid_map.get(target)
                if tuid is None:
                    option_lines.append(f'        Option("{text}", GotoAndPlay(0))')
                    continue
                if cimp:
                    bname = f"opt_{seg_id}_{i}"
                    blines = [f"{bname}: {{"] + state_to_lets(cimp) + \
                             [f"    GotoAndPlay({tuid}); // -> {target}", "}"]
                    extra_blocks.append("\n".join(blines))
                    option_lines.append(f'        Option("{text}", {bname})')
                else:
                    option_lines.append(
                        f'        Option("{text}", GotoAndPlay({tuid})) // -> {target}')

            elif "sg" in choice:
                sg_name = choice["sg"]
                group = segmentGroups.get(sg_name, [])
                bname = f"sg_{seg_id}_{i}"
                blines = [f"{bname}: {{"]
                if cimp: blines += state_to_lets(cimp)
                cases = []
                for item in group:
                    if isinstance(item, str):
                        pre = preconditions.get(item, [])
                        cases.append((translate_precondition(pre), item, uid_map.get(item, 0)))
                    elif isinstance(item, dict):
                        seg = item.get("segment", "")
                        pre = preconditions.get(item.get("precondition", ""), [])
                        cases.append((translate_precondition(pre), seg, uid_map.get(seg, 0)))
                blines.append("    Select {")
                for expr, tseg, tuid in cases:
                    blines.append(f"        Case({expr}) {{ GotoAndPlay({tuid}); }} // -> {tseg}")
                blines.append(f'        Default {{ Panic("Unresolved {sg_name} in {seg_id}"); }}')
                blines += ["    }", "}"]
                extra_blocks.append("\n".join(blines))
                option_lines.append(f'        Option("{text}", {bname})')

        if option_lines:
            leave_lines += ["entry: {", f"    Menu(timeout: {timeout_s}, default: {default_idx + 1},"]
            for j, ol in enumerate(option_lines):
                leave_lines.append(ol + ("," if j < len(option_lines) - 1 else ""))
            leave_lines += ["    );", "}"]
            for eb in extra_blocks:
                leave_lines += ["", eb]
    else:
        dn = seg_data.get("defaultNext")
        if dn and dn in uid_map:
            leave_lines += ["entry: {", f"    GotoAndPlay({uid_map[dn]}); // -> {dn}", "}"]

    entry = ("entry: {\n" + "\n".join(entry_lines) + "\n}") if entry_lines else None
    leave = "\n".join(leave_lines) if leave_lines else None
    return entry, leave

def build_chapter_atom(seg_id, seg_data, seg_moments, preconditions, segmentGroups, uid_map):
    uid = uid_map[seg_id]
    start_ns = int(seg_data["startTimeMs"]) * 1000000
    end_ns = int(seg_data.get("endTimeMs", seg_data["startTimeMs"] + 1000)) * 1000000

    entry_script, leave_script = build_scripts(
        seg_id, seg_data, seg_moments, preconditions, segmentGroups, uid_map)

    display = el(ID_ChapterDisplay,
        el_str(ID_ChapterString, seg_id) + el_str(ID_ChapterLanguage, "eng"))

    atom_data = (
        el_uint(ID_ChapterUID, uid, 4) +
        el_uint(ID_ChapterTimeStart, start_ns, 8) +
        el_uint(ID_ChapterTimeEnd, end_ns, 8) +
        el_uint(ID_ChapterFlagHidden, 0) +
        el_uint(ID_ChapterFlagEnabled, 1) +
        display
    )

    atom_data += el_str(ID_ChapterStringUID, seg_id)

    for script, time_val in [(entry_script, 1), (leave_script, 2)]:
        if script:
            cmd = el(ID_ChapProcessCommand,
                el_uint(ID_ChapProcessTime, time_val) +
                el_bin(ID_ChapProcessData, script.encode('utf-8')))
            atom_data += el(ID_ChapProcess, el_uint(ID_ChapProcessCodecID, 0) + cmd)

    return el(ID_ChapterAtom, atom_data)

def build_chapters_ebml(segments, moments_by_seg, preconditions, segmentGroups):
    uid_map = {seg_id: random.randint(1, 2**32) for seg_id in segments}
    edition_uid = random.randint(1, 2**32)

    atoms = b""
    total = len(segments)
    for i, (seg_id, seg_data) in enumerate(segments.items(), 1):
        if i % 50 == 0:
            print(f"  Chapter {i}/{total}: {seg_id}", flush=True)
        atoms += build_chapter_atom(
            seg_id, seg_data, moments_by_seg.get(seg_id, []),
            preconditions, segmentGroups, uid_map)

    edition = el(ID_EditionEntry,
        el_uint(ID_EditionUID, edition_uid, 4) +
        el_uint(ID_EditionFlagHidden, 0) +
        el_uint(ID_EditionFlagDefault, 1) +
        el_uint(ID_EditionFlagOrdered, 1) +
        atoms)
    return el(ID_Chapters, edition), uid_map

# ── MKV rewriter ──────────────────────────────────────────────────

def rewrite_with_chapters(source_path, chapters_ebml, out_path):
    source_size = os.path.getsize(source_path)
    chunk_size = 8 * 1024 * 1024

    print(f"Scanning source file structure...")
    info = scan_source(source_path)

    seg_body_start  = info['segment_body_start']
    sh_offset       = info['seekhead_offset']
    sh_total        = info['seekhead_total']
    void_total      = info['void_total']
    insertion_offset = info['insertion_offset']
    orig_entries    = info['original_entries']

    chapters_size = len(chapters_ebml)
    space_for_seekhead = sh_total + void_total  # bytes available for new SeekHead+Void

    print(f"  Segment body starts at: {seg_body_start}")
    print(f"  SeekHead at {sh_offset}, total {sh_total} bytes")
    print(f"  Void total: {void_total} bytes")
    print(f"  Space for SeekHead+Void: {space_for_seekhead} bytes")
    print(f"  Inserting Chapters at offset: {insertion_offset}")
    print(f"  Chapters size: {chapters_size:,} bytes")

    # Build updated SeekHead entries:
    # - Add Chapters entry at insertion_offset (relative to seg_body_start)
    # - Shift all existing entries that point at or after insertion_offset
    chapters_rel = insertion_offset - seg_body_start
    new_entries = []
    for id_bytes, rel_pos in orig_entries:
        abs_pos = seg_body_start + rel_pos
        if abs_pos >= insertion_offset:
            new_rel = rel_pos + chapters_size
            name = KNOWN_IDS.get(id_bytes, id_bytes.hex())
            print(f"  {name}: {rel_pos} → {new_rel} (+{chapters_size})")
            new_entries.append((id_bytes, new_rel))
        else:
            name = KNOWN_IDS.get(id_bytes, id_bytes.hex())
            print(f"  {name}: {rel_pos} (unchanged)")
            new_entries.append((id_bytes, rel_pos))

    # Add Chapters entry
    new_entries.append((chapters_id_bytes(), chapters_rel))
    print(f"  Chapters: {chapters_rel} (new)")

    # Build new SeekHead
    new_sh = build_seekhead(new_entries)
    new_sh_size = len(new_sh)
    print(f"  New SeekHead size: {new_sh_size} bytes (space: {space_for_seekhead})")

    if new_sh_size >= space_for_seekhead:
        raise ValueError(f"New SeekHead ({new_sh_size}) doesn't fit in available space ({space_for_seekhead})")

    # Calculate Void to fill remaining space
    remaining = space_for_seekhead - new_sh_size
    void_content_size = content_size_for_void_total(remaining)
    if void_content_size is None:
        raise ValueError(f"Cannot fit Void into {remaining} bytes")

    new_void = eid(ID_Void) + vint(void_content_size) + bytes(void_content_size)
    total_used = new_sh_size + len(new_void)
    assert total_used == space_for_seekhead, f"Size mismatch: {total_used} != {space_for_seekhead}"
    print(f"  New Void: {len(new_void)} bytes (content={void_content_size})")

    print(f"\nWriting output: {out_path}")

    written = 0
    with open(source_path, 'rb') as src, open(out_path, 'wb') as dst:

        # Write everything up to SeekHead
        remaining_bytes = sh_offset
        while remaining_bytes > 0:
            data = src.read(min(chunk_size, remaining_bytes))
            if not data: break
            dst.write(data)
            written += len(data)
            remaining_bytes -= len(data)

        # Write new SeekHead + Void (replaces old SeekHead + old Void)
        dst.write(new_sh)
        dst.write(new_void)
        written += len(new_sh) + len(new_void)

        # Skip old SeekHead + old Void in source
        src.seek(sh_offset + sh_total + void_total)

        # Copy from insertion_offset-start to insertion_offset
        # (the metadata between Void end and Chapters insertion point)
        remaining_bytes = insertion_offset - (sh_offset + sh_total + void_total)
        while remaining_bytes > 0:
            data = src.read(min(chunk_size, remaining_bytes))
            if not data: break
            dst.write(data)
            written += len(data)
            remaining_bytes -= len(data)

        # Insert Chapters
        dst.write(chapters_ebml)
        written += len(chapters_ebml)
        print(f"  Inserted Chapters element ({len(chapters_ebml):,} bytes)")

        # Copy remainder
        last_pct = -1
        while True:
            data = src.read(chunk_size)
            if not data: break
            dst.write(data)
            written += len(data)
            pct = int(src.tell() / source_size * 100)
            if pct != last_pct:
                print(f"  Progress: {pct}%\r", end="", flush=True)
                last_pct = pct

    print(f"\n  Done. {written:,} bytes written.")

    # Fix Segment size
    actual_size = os.path.getsize(out_path)
    with open(out_path, 'rb') as f:
        header = f.read(60)
    b = header[4]
    if b & 0x80: ebml_sz = b & 0x7F; ebml_szl = 1
    elif b & 0x40: ebml_sz = ((b & 0x3F) << 8) | header[5]; ebml_szl = 2
    else: ebml_sz = 0; ebml_szl = 1
    ebml_end = 4 + ebml_szl + ebml_sz
    seg_size_offset = ebml_end + 4
    seg_body_start2 = seg_size_offset + 8
    correct_seg_size = actual_size - seg_body_start2
    new_size_bytes = struct.pack('>Q', correct_seg_size | 0x0100000000000000)
    with open(out_path, 'r+b') as f:
        f.seek(seg_size_offset)
        f.write(new_size_bytes)
    print(f"  Segment size updated to {correct_seg_size:,}")

# ── Main ──────────────────────────────────────────────────────────

def main():
    if len(sys.argv) not in (4, 5):
        print(f"Usage: {sys.argv[0]} source.mkv bandersnatch.json SegmentMap.json [output.mkv]")
        sys.exit(1)

    source_path = sys.argv[1]
    json_path   = sys.argv[2]
    segmap_path = sys.argv[3]
    out_path    = sys.argv[4] if len(sys.argv) == 5 else \
                  source_path.rsplit('.mkv', 1)[0] + '_chaptered.mkv'

    if not os.path.exists(source_path):
        print(f"Error: {source_path} not found")
        sys.exit(1)

    print("Loading JSON data...")
    with open(json_path) as f:
        bdata = json.load(f)
    with open(segmap_path) as f:
        smap = json.load(f)

    info = bdata["videos"]["80988062"]["interactiveVideoMoments"]["value"]

    print(f"Building chapters for {len(smap['segments'])} segments...")
    chapters_ebml, uid_map = build_chapters_ebml(
        smap["segments"],
        info["momentsBySegment"],
        info["preconditions"],
        info["segmentGroups"],
    )
    print(f"Chapters EBML: {len(chapters_ebml):,} bytes")
    print()

    rewrite_with_chapters(source_path, chapters_ebml, out_path)

    print(f"\nComplete. {len(uid_map)} segments.")
    print(f"Output: {out_path}")

if __name__ == "__main__":
    main()
