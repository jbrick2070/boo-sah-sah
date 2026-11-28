#!/usr/bin/env python3
"""Compare capture directories without dependencies outside the Python standard library."""

import argparse
from collections import Counter
import fnmatch
from itertools import accumulate
import json
from operator import sub
from pathlib import Path
import shutil
import struct
import sys
import zlib


FLAT_PIXEL_SHARE = 0.995
FLAT_COLOR_TOLERANCE = 4
NEAR_BLACK_CHANNEL_MAX = 8
MAX_NEAR_BLACK_SHARE_INCREASE = 0.25
# The darkest real capture (the midnight bathroom with its lamps off) is 95.1 % near-black.
MAX_NEAR_BLACK_SHARE = 0.985
REJECT_MEAN_LIMIT = 3.0
PNG_SIGNATURE = b'\x89PNG\r\n\x1a\n'
OVER8 = bytes(int(i > 8) for i in range(256))
OVER64 = bytes(int(i > 64) for i in range(256))
# A categorical heatmap makes the two metric thresholds visible.
HEAT_R = bytes(0 if i <= 8 else 255 for i in range(256))
HEAT_G = bytes(255 if 8 < i <= 64 else 0 for i in range(256))
HEAT_B = bytes(0 if i == 0 or 8 < i <= 64 else 128 if i <= 8 else 128
               for i in range(256))


def png_chunk(kind, data):
    return (struct.pack('>I', len(data)) + kind + data +
            struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff))


def write_png(path, width, height, rows):
    path.parent.mkdir(parents=True, exist_ok=True)
    raw = b''.join(b'\x00' + row for row in rows)
    path.write_bytes(PNG_SIGNATURE +
                     png_chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)) +
                     png_chunk(b'IDAT', zlib.compress(raw)) + png_chunk(b'IEND', b''))


def read_png(data):
    if not data.startswith(PNG_SIGNATURE):
        raise ValueError('invalid PNG signature')
    pos, header, compressed, ended = 8, None, [], False
    while pos < len(data):
        if pos + 12 > len(data):
            raise ValueError('truncated PNG chunk')
        length = struct.unpack_from('>I', data, pos)[0]
        end = pos + 12 + length
        if end > len(data):
            raise ValueError('truncated PNG chunk data')
        kind, content = data[pos + 4:pos + 8], data[pos + 8:pos + 8 + length]
        crc = struct.unpack_from('>I', data, pos + 8 + length)[0]
        if zlib.crc32(kind + content) & 0xffffffff != crc:
            raise ValueError('PNG chunk CRC mismatch')
        if kind == b'IHDR':
            if header is not None or pos != 8 or length != 13:
                raise ValueError('invalid PNG IHDR')
            header = struct.unpack('>IIBBBBB', content)
        elif kind == b'IDAT':
            compressed.append(content)
        elif kind == b'PLTE':
            # Truecolour PNGs may include a suggested palette.
            if header is None or compressed or not length or length > 768 or length % 3:
                raise ValueError('invalid PNG palette')
        elif kind == b'IEND':
            ended = True
            break
        elif kind[0] & 32 == 0:
            raise ValueError('unsupported critical PNG chunk ' + repr(kind))
        pos = end
    if header is None or not compressed or not ended:
        raise ValueError('incomplete PNG')
    width, height, depth, color, compression, filtering, interlace = header
    if not width or not height or depth != 8 or color not in (2, 6) or compression or filtering or interlace:
        raise ValueError('only non-interlaced 8-bit RGB/RGBA PNG is supported')
    channels = 3 if color == 2 else 4
    stride = width * channels
    raw = zlib.decompress(b''.join(compressed))
    if len(raw) != (stride + 1) * height:
        raise ValueError('invalid PNG image data length')
    rows, previous = [], bytearray(stride)
    for offset in range(0, len(raw), stride + 1):
        method = raw[offset]
        row = bytearray(raw[offset + 1:offset + 1 + stride])
        if method == 1:
            # Each channel is an independent running sum, modulo 256.
            for c in range(channels):
                row[c::channels] = bytes(value & 255 for value in accumulate(row[c::channels]))
        elif method == 2:
            # Widen each byte to a 16-bit lane before adding: carries cannot
            # cross lanes, and the low bytes are the sums modulo 256.
            a, b = bytearray(stride * 2), bytearray(stride * 2)
            a[::2], b[::2] = row, previous
            row = bytearray((int.from_bytes(a, 'little') +
                             int.from_bytes(b, 'little')).to_bytes(stride * 2, 'little')[::2])
        elif method == 3:
            for i in range(channels):
                row[i] = (row[i] + (previous[i] >> 1)) & 255
            for i in range(channels, stride):
                row[i] = (row[i] + ((row[i - channels] + previous[i]) >> 1)) & 255
        elif method == 4:
            for i in range(stride):
                left = row[i - channels] if i >= channels else 0
                up = previous[i]
                up_left = previous[i - channels] if i >= channels else 0
                p = left + up - up_left
                a, b, c = abs(p - left), abs(p - up), abs(p - up_left)
                predictor = left if a <= b and a <= c else up if b <= c else up_left
                row[i] = (row[i] + predictor) & 255
        elif method != 0:
            raise ValueError('unsupported PNG filter {}'.format(method))
        previous = row
        if channels == 4:
            rgb = bytearray(width * 3)
            rgb[0::3], rgb[1::3], rgb[2::3] = row[0::4], row[1::4], row[2::4]
            rows.append(bytes(rgb))
        else:
            rows.append(bytes(row))
    return width, height, rows


def read_bmp(data):
    if len(data) < 54 or data[:2] != b'BM':
        raise ValueError('invalid BMP header')
    pixel_offset = struct.unpack_from('<I', data, 10)[0]
    dib_size = struct.unpack_from('<I', data, 14)[0]
    if dib_size < 40 or len(data) < 14 + dib_size:
        raise ValueError('unsupported BMP header')
    width, signed_height, planes, bits, compression = struct.unpack_from('<iiHHI', data, 18)
    if width <= 0 or signed_height == 0 or planes != 1 or bits not in (24, 32) or compression != 0:
        raise ValueError('only uncompressed 24/32-bit BMP is supported')
    height, channels = abs(signed_height), bits // 8
    stride = (width * channels + 3) & ~3
    if pixel_offset < 14 + dib_size or pixel_offset + stride * height > len(data):
        raise ValueError('invalid or truncated BMP pixel data')
    rows = []
    for y in range(height):
        source_y = y if signed_height < 0 else height - 1 - y
        row = data[pixel_offset + source_y * stride:pixel_offset + source_y * stride + width * channels]
        rgb = bytearray(width * 3)
        rgb[0::3], rgb[1::3], rgb[2::3] = row[2::channels], row[1::channels], row[0::channels]
        rows.append(bytes(rgb))
    return width, height, rows


def read_image(path):
    try:
        data = path.read_bytes()
        return read_png(data) if path.suffix.lower() == '.png' else read_bmp(data)
    except (OSError, ValueError, struct.error, zlib.error) as exc:
        raise ValueError('{}: {}'.format(path, exc)) from exc


def is_flat(image):
    width, height, rows = image
    channels = [Counter(), Counter(), Counter()]
    for row in rows:
        for c in range(3):
            channels[c].update(row[c::3])
    half = (width * height + 1) // 2
    medians = []
    for histogram in channels:
        count = 0
        for value in range(256):
            count += histogram[value]
            if count >= half:
                medians.append(value)
                break
    masks = [bytes(255 if abs(v - median) <= FLAT_COLOR_TOLERANCE else 0
                   for v in range(256)) for median in medians]
    close = 0
    for row in rows:
        # Combining byte masks with big integers avoids a Python loop per pixel.
        r = int.from_bytes(row[0::3].translate(masks[0]), 'little')
        g = int.from_bytes(row[1::3].translate(masks[1]), 'little')
        b = int.from_bytes(row[2::3].translate(masks[2]), 'little')
        close += (r & g & b).to_bytes(width, 'little').count(255)
    return close / (width * height) >= FLAT_PIXEL_SHARE


def near_black_share(image):
    width, height, rows = image
    mask = bytes(255 if value <= NEAR_BLACK_CHANNEL_MAX else 0 for value in range(256))
    count = 0
    for row in rows:
        r = int.from_bytes(row[0::3].translate(mask), 'little')
        g = int.from_bytes(row[1::3].translate(mask), 'little')
        b = int.from_bytes(row[2::3].translate(mask), 'little')
        count += (r & g & b).to_bytes(width, 'little').count(255)
    return count / (width * height)



def is_emergency_test_picture(image):
    """Recognize the phase-invariant background and amber core of PSFallbackMain.

    Shader coordinates scale both axes by height, preserving the circular shape.
    Sample a fixed grid so the cost and the thresholds do not depend on resolution.
    An overlay may obscure a modest portion of the otherwise uniform background.
    """
    width, height, rows = image
    outside = background = centre = amber = 0
    for gy in range(72):
        y = min(height - 1, (2 * gy + 1) * height // 144)
        py = ((y + 0.5) / height - 0.5) * 2
        row = rows[y]
        for gx in range(128):
            x = min(width - 1, (2 * gx + 1) * width // 256)
            px = ((x + 0.5) / width - 0.5) * 2 * width / height
            r2 = px * px + py * py
            red, green, blue = row[x * 3:x * 3 + 3]
            if r2 > 0.58 ** 2:
                outside += 1
                background += (abs(red - 8) <= 6 and abs(green - 13) <= 6
                               and abs(blue - 20) <= 6)
            elif r2 < 0.12 ** 2:
                centre += 1
                amber += red > green > blue and red > 30 and green > 20
    return bool(outside and centre and background / outside >= 0.90 and amber / centre >= 0.75)


def box_average(image, target_width=64, target_height=36):
    """Area-weighted box resampling, including when the source is smaller."""
    width, height, rows = image
    horizontal = []
    for row in rows:
        out = bytearray(target_width * 3)
        for x in range(target_width):
            start, end = x * width, (x + 1) * width
            first, last = start // target_width, (end - 1) // target_width
            for c in range(3):
                value = 0
                for source in range(first, last + 1):
                    weight = min(end, (source + 1) * target_width) - max(start, source * target_width)
                    value += row[source * 3 + c] * weight
                out[x * 3 + c] = (value + width // 2) // width
        horizontal.append(out)
    result = []
    for y in range(target_height):
        start, end = y * height, (y + 1) * height
        first, last = start // target_height, (end - 1) // target_height
        row = bytearray(target_width * 3)
        for i in range(target_width * 3):
            value = sum(horizontal[source][i] *
                        (min(end, (source + 1) * target_height) - max(start, source * target_height))
                        for source in range(first, last + 1))
            row[i] = (value + height // 2) // height
        result.append(bytes(row))
    return target_width, target_height, result

def differences(ref, run, heatmap=False):
    width, height, ref_rows = ref
    run_rows = run[2]
    total = peak = over8 = over64 = 0
    heat_rows = []
    for a, b in zip(ref_rows, run_rows):
        diff = bytes(map(abs, map(sub, a, b)))
        total += sum(diff)
        peak = max(peak, max(diff))
        over8 += diff.translate(OVER8).count(1)
        over64 += diff.translate(OVER64).count(1)
        if heatmap:
            # Classify each pixel by its largest channel difference.
            intensity = bytes(map(max, diff[0::3], diff[1::3], diff[2::3]))
            rgb = bytearray(width * 3)
            rgb[0::3] = intensity.translate(HEAT_R)
            rgb[1::3] = intensity.translate(HEAT_G)
            rgb[2::3] = intensity.translate(HEAT_B)
            heat_rows.append(bytes(rgb))
    count = width * height * 3
    return peak, total / count, over8 / count, over64 / count, heat_rows


def collect(folder):
    if not folder.is_dir():
        raise ValueError('capture folder not found: {}'.format(folder))
    return {p.relative_to(folder).as_posix(): p for p in folder.rglob('*')
            if p.is_file() and p.suffix.lower() in ('.png', '.bmp')
            and p.relative_to(folder).parts[0].lower() != '_diff'}


def nonnegative(value):
    import math
    result = float(value)
    if not math.isfinite(result) or result < 0:
        raise argparse.ArgumentTypeError('must be a finite non-negative number')
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ref', type=Path, required=True)
    parser.add_argument('--run', type=Path, required=True)
    parser.add_argument('--max-mean', type=nonnegative, default=6.0)
    parser.add_argument('--max-over8', type=nonnegative)
    parser.add_argument('--max-over64', type=nonnegative, default=0.02)
    parser.add_argument('--baseline', type=Path)
    parser.add_argument('--json', type=Path)
    parser.add_argument('--heatmaps', action='store_true')
    parser.add_argument('--only', action='append', default=[], metavar='PATTERN')
    parser.add_argument('--map', action='append', default=[], metavar='SRC=DST',
                        help='map run paths to reference paths; use with --baseline for full completeness')
    parser.add_argument('--reject', action='append', default=[], type=Path, metavar='IMAGE')
    args = parser.parse_args(argv)
    try:
        if args.ref.resolve() == args.run.resolve():
            raise ValueError('reference and run folders must be different')
        mappings = []
        for mapping in args.map:
            if '=' not in mapping or not mapping.split('=', 1)[0]:
                raise ValueError('--map requires a nonempty SRC in SRC=DST')
            mappings.append(tuple(mapping.split('=', 1)))

        patterns = [pattern.replace('\\', '/') for pattern in args.only]

        def selected(name):
            return not patterns or any(fnmatch.fnmatchcase(name, pattern) for pattern in patterns)

        def mapped(name):
            for src, dst in mappings:
                name = name.replace(src, dst)
            return name

        volatile, stable = set(), set()
        if args.baseline:
            document = json.loads(args.baseline.read_text(encoding='utf-8-sig'))
            entries = document['files']
            if not isinstance(entries, dict):
                raise ValueError('baseline files must be an object')
            for name, entry in entries.items():
                if not isinstance(entry, dict) or not isinstance(entry.get('stable'), bool):
                    raise ValueError('invalid stable flag for {}'.format(name))
                name = name.replace('\\', '/')
                if selected(name):
                    (stable if entry['stable'] else volatile).add(name)
        refs, runs = collect(args.ref), collect(args.run)
        if not refs and not runs and not stable and not volatile:
            raise ValueError('no PNG or BMP files in either folder')
        # A mapped reference is consumed by its run path and must not also be
        # reported as an unmatched reference. Baseline names are run names.
        run_names = {name for name in runs if selected(name)} | stable | volatile
        comparisons = [(name, mapped(name), name if name in runs else None)
                       for name in run_names]
        used_refs = {ref_name for _, ref_name, _ in comparisons if ref_name in refs}
        comparisons.extend((name, name, None) for name in refs
                           if selected(name) and name not in used_refs and name not in run_names)

        if not comparisons:
            raise ValueError('no matching PNG or BMP files to compare')

        rejects = []
        for path in args.reject:
            try:
                rejects.append((path, box_average(read_image(path))))
            except ValueError as exc:
                raise ValueError('unreadable reject image: {}'.format(exc)) from exc

        if args.heatmaps:
            diff_dir = args.run / '_diff'
            if diff_dir.is_symlink():
                diff_dir.unlink()
            elif diff_dir.exists():
                shutil.rmtree(diff_dir)

        results = []
        for name, ref_name, run_name in sorted(comparisons):
            result = {'path': name, 'status': 'PASS', 'max': None, 'mean': None,
                      'over8': None, 'over64': None, 'reason': None}
            ref = run = None
            reasons, limit_reasons = [], []
            if ref_name not in refs:
                reasons.append('missing reference')
            else:
                try:
                    ref = read_image(refs[ref_name])
                except ValueError as exc:
                    reasons.append('unreadable reference: {}'.format(exc))
            if run_name is None:
                reasons.append('missing run')
            else:
                try:
                    run = read_image(runs[run_name])
                except ValueError as exc:
                    reasons.append('unreadable run: {}'.format(exc))
            identical = bool(ref and run and ref == run)
            ref_flat = is_flat(ref) if ref else False
            if ref_flat:
                reasons.append('reference image is nearly flat')
            if run and (ref_flat if identical else is_flat(run)):
                reasons.append('run image is nearly flat')
            ref_black = near_black_share(ref) if ref else None
            run_black = ref_black if identical else near_black_share(run) if run else None
            for label, image, black in (('reference', ref, ref_black), ('run', run, run_black)):
                if image:
                    if black > MAX_NEAR_BLACK_SHARE:
                        reasons.append(label + ' image has too many near-black pixels')
                    if is_emergency_test_picture(image):
                        reasons.append(label + ' image is emergency test picture')
                    if rejects:
                        small = box_average(image)
                        for reject_path, reject in rejects:
                            if differences(reject, small)[1] < REJECT_MEAN_LIMIT:
                                reasons.append('{} image matches reject image {}'.format(label, reject_path))
            heat = None
            if ref and run:
                if ref[:2] != run[:2]:
                    reasons.append('size differs ({}x{} vs {}x{})'.format(*ref[:2], *run[:2]))
                    if args.heatmaps:
                        # A missing portion of a differently sized image is black.
                        w, h = max(ref[0], run[0]), max(ref[1], run[1])
                        def pad(image):
                            return (w, h, [row + bytes((w - image[0]) * 3) for row in image[2]] +
                                    [bytes(w * 3)] * (h - image[1]))
                        _, _, _, _, heat = differences(pad(ref), pad(run), True)
                else:
                    if identical and not args.heatmaps:
                        peak, mean, over8, over64 = 0, 0.0, 0.0, 0.0
                    else:
                        peak, mean, over8, over64, heat = differences(ref, run, args.heatmaps)
                    result.update(max=peak, mean=mean, over8=over8, over64=over64)
                    if not identical and run_black - ref_black > MAX_NEAR_BLACK_SHARE_INCREASE:
                        reasons.append('run image has too many near-black pixels')
                    if mean > args.max_mean:
                        limit_reasons.append('mean exceeds limit')
                    if args.max_over8 is not None and over8 > args.max_over8:
                        limit_reasons.append('over8 exceeds limit')
                    if args.max_over64 is not None and over64 > args.max_over64:
                        limit_reasons.append('over64 exceeds limit')
            if reasons or (limit_reasons and name not in volatile):
                result['status'] = 'FAIL'
            elif name in volatile:
                result['status'] = 'VOLATILE'
            all_reasons = reasons + limit_reasons
            if all_reasons:
                result['reason'] = '; '.join(all_reasons)
            if args.heatmaps and result['status'] == 'FAIL' and heat is not None:
                # Include the extension so frame.bmp and frame.png never collide.
                write_png(args.run / '_diff' / (name + '.png'),
                          max(ref[0], run[0]), max(ref[1], run[1]), heat)
            results.append(result)
            fmt = lambda x: '-' if x is None else '{:.6f}'.format(x)
            print('{} {} max={} mean={} over8={} over64={}{}'.format(
                result['status'], result['path'], result['max'] if result['max'] is not None else '-',
                fmt(result['mean']), fmt(result['over8']), fmt(result['over64']),
                ' (' + result['reason'] + ')' if result['reason'] else ''), flush=True)
        summary = {'total': len(results), 'passed': sum(r['status'] == 'PASS' for r in results),
                   'failed': sum(r['status'] == 'FAIL' for r in results),
                   'volatile': sum(r['status'] == 'VOLATILE' for r in results)}
        failures = [r for r in results if r['status'] == 'FAIL']
        if failures:
            print('Failures (worst mean first):')
            for result in sorted(failures, key=lambda r: (r['mean'] is None,
                                                          -(r['mean'] or 0), r['path'])):
                print('  {} mean={} ({})'.format(result['path'], fmt(result['mean']), result['reason']))
        print('Summary: {total} files, {passed} passed, {failed} failed, {volatile} volatile'.format(**summary))
        if args.json:
            args.json.parent.mkdir(parents=True, exist_ok=True)
            args.json.write_text(json.dumps({'files': results, 'summary': summary}, indent=2) + '\n', encoding='utf-8')
        return 1 if summary['failed'] else 0
    except (OSError, ValueError, KeyError, TypeError, json.JSONDecodeError) as exc:
        print('error: {}'.format(exc), file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())
