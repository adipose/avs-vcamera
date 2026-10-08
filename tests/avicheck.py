"""Read an uncompressed AVI recorded from avs-vcamera and say what is in it.

For every video frame, the frame number the driver drew into the strip along
the top of the picture (32 blocks, most significant bit on the left, white is
1). For the audio, its format, length, and the frequency on each channel.

    py avicheck.py capture.avi                  # JSON report
    py avicheck.py capture.avi --min-seconds 4 --expect-tones 1000,1500

With any --expect/--min option the exit code says whether the recording
passed: 0 yes, 1 no (the reasons are in the report's "problems").

A frozen picture -- the symptom in clsid2/mpc-hc#4280 and #4285 -- shows as
frame numbers that stop increasing. A dropped frame shows as a gap.

Plain Python 3, no packages. Reads RIFF and OpenDML (AVIX) files as written
by DirectShow's AVI Mux, with UYVY, YUY2 or RGB24 video and PCM audio.
"""

import argparse
import json
import struct
import sys

STRIP_BITS = 32
STRIP_HEIGHT_DIVISOR = 12


def chunks(data, start, end):
    """Yield (fourcc, list_type_or_None, body_start, body_end) for every chunk, descending into RIFF and LIST."""
    pos = start
    while pos + 8 <= end:
        fourcc = data[pos:pos + 4]
        size = struct.unpack_from('<I', data, pos + 4)[0]
        body = pos + 8
        body_end = min(body + size, end)
        if fourcc in (b'RIFF', b'LIST'):
            kind = data[body:body + 4]
            yield fourcc, kind, body + 4, body_end
            yield from chunks(data, body + 4, body_end)
        else:
            yield fourcc, None, body, body_end
        pos = body + size + (size & 1)


def parse(path):
    data = open(path, 'rb').read()
    streams = []
    frames = {}          # stream index -> list of (start, end)
    current = None
    for fourcc, kind, s, e in chunks(data, 0, len(data)):
        if fourcc == b'strh':
            fcc_type = data[s:s + 4].decode('ascii', 'replace')
            handler = data[s + 4:s + 8]
            scale, rate = struct.unpack_from('<II', data, s + 20)
            length = struct.unpack_from('<I', data, s + 32)[0]
            current = {'type': fcc_type, 'handler': handler.decode('ascii', 'replace'),
                       'scale': scale, 'rate': rate, 'length': length}
            streams.append(current)
        elif fourcc == b'strf' and current is not None:
            if current['type'] == 'vids':
                (_, w, h, _, bpp, comp) = struct.unpack_from('<IiiHHI', data, s)
                current.update(width=w, height=h, bitCount=bpp,
                               compression=struct.pack('<I', comp).decode('ascii', 'replace') if comp else 'RGB')
            elif current['type'] == 'auds':
                (tag, ch, sps, _, align, bits) = struct.unpack_from('<HHIIHH', data, s)
                current.update(formatTag=tag, channels=ch, samplesPerSec=sps, blockAlign=align, bitsPerSample=bits)
        elif kind is None and len(fourcc) == 4 and fourcc[:2].isdigit() and fourcc[2:] in (b'dc', b'db', b'wb'):
            frames.setdefault(int(fourcc[:2]), []).append((s, e))
    return data, streams, frames


def strip_value(frame, width, height, compression, bit_count):
    """The number in the strip, or None if the blocks are not cleanly black and white."""
    abs_h = abs(height)
    strip_h = abs_h // STRIP_HEIGHT_DIVISOR
    block_w = width // STRIP_BITS
    y = strip_h // 2
    value = 0
    for bit in range(STRIP_BITS):
        x = bit * block_w + block_w // 2
        if compression in ('UYVY', 'YUY2'):
            row = y * width * 2
            luma = frame[row + x * 2 + (1 if compression == 'UYVY' else 0)]
        elif compression == 'RGB' and bit_count == 24:
            stride = (width * 3 + 3) & ~3
            row_index = y if height < 0 else abs_h - 1 - y          # positive height is bottom-up
            b, g, r = frame[row_index * stride + x * 3: row_index * stride + x * 3 + 3]
            luma = (r * 299 + g * 587 + b * 114) // 1000
        else:
            return None
        if 64 < luma < 192:
            return None
        value = (value << 1) | (1 if luma >= 192 else 0)
    return value


def tone_frequency(samples, rate):
    """Frequency of a pure tone from its zero crossings, or 0 for silence."""
    if len(samples) < 2 or max(abs(s) for s in samples) < 100:
        return 0.0
    crossings = []
    for i in range(1, len(samples)):
        a, b = samples[i - 1], samples[i]
        if a < 0 <= b:
            crossings.append(i - 1 + (-a) / (b - a))
    if len(crossings) < 2:
        return 0.0
    return (len(crossings) - 1) * rate / (crossings[-1] - crossings[0])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('avi')
    ap.add_argument('--min-seconds', type=float, help='video must cover at least this long, by its frame numbers')
    ap.add_argument('--expect-tones', help='comma-separated frequency per audio channel, e.g. 1000,1500')
    ap.add_argument('--max-gap', type=int, help='largest allowed jump between consecutive frame numbers')
    args = ap.parse_args()

    data, streams, frames = parse(args.avi)
    report = {'file': args.avi, 'streams': streams}
    problems = []

    for index, st in enumerate(streams):
        chunks_for = frames.get(index, [])
        if st['type'] == 'vids':
            numbers = []
            unreadable = 0
            for s, e in chunks_for:
                if e - s == 0:
                    continue                                         # a dropped-frame placeholder
                n = strip_value(data[s:e], st['width'], st['height'], st['compression'], st['bitCount'])
                if n is None:
                    unreadable += 1
                else:
                    numbers.append(n)
            repeats = sum(1 for a, b in zip(numbers, numbers[1:]) if b == a)
            backwards = sum(1 for a, b in zip(numbers, numbers[1:]) if b < a)
            gaps = [b - a - 1 for a, b in zip(numbers, numbers[1:]) if b > a + 1]
            fps = st['rate'] / st['scale'] if st['scale'] else 0
            span = (numbers[-1] - numbers[0] + 1) / fps if numbers and fps else 0
            st['frames'] = {'chunks': len(chunks_for), 'read': len(numbers), 'unreadable': unreadable,
                            'first': numbers[0] if numbers else None, 'last': numbers[-1] if numbers else None,
                            'repeats': repeats, 'backwards': backwards, 'gaps': len(gaps),
                            'missing': sum(gaps), 'largestGap': max(gaps) if gaps else 0,
                            'secondsByNumber': round(span, 3)}
            if not numbers:
                problems.append('no video frame carried a readable frame number')
            if repeats or backwards:
                problems.append(f'frame numbers did not keep advancing: {repeats} repeated, {backwards} went backwards')
            if args.min_seconds is not None and span < args.min_seconds:
                problems.append(f'video covers {span:.2f} s by its frame numbers, expected at least {args.min_seconds}')
            if args.max_gap is not None and gaps and max(gaps) > args.max_gap:
                problems.append(f'a gap of {max(gaps)} frames, more than {args.max_gap}')
        elif st['type'] == 'auds' and st.get('formatTag') == 1 and st.get('bitsPerSample') == 16:
            pcm = b''.join(data[s:e] for s, e in chunks_for)
            ch, rate = st['channels'], st['samplesPerSec']
            count = len(pcm) // (2 * ch)
            st['seconds'] = round(count / rate, 3) if rate else 0
            values = struct.unpack_from('<%dh' % (count * ch), pcm)
            # Judge the middle of the recording, away from start-up.
            mid = count // 2
            window = slice(max(0, mid - rate // 2), min(count, mid + rate // 2))
            st['frequencies'] = [round(tone_frequency(values[c::ch][window], rate), 1) for c in range(ch)]
            if args.expect_tones:
                want = [float(f) for f in args.expect_tones.split(',')]
                got = st['frequencies']
                if len(got) != len(want) or any(abs(g - w) > w * 0.01 for g, w in zip(got, want)):
                    problems.append(f'audio tones {got}, expected {want}')
            if args.min_seconds is not None and st['seconds'] < args.min_seconds:
                problems.append(f'audio is {st["seconds"]} s, expected at least {args.min_seconds}')

    if args.expect_tones and not any(st['type'] == 'auds' for st in streams):
        problems.append('no audio stream')
    report['problems'] = problems
    print(json.dumps(report, indent=2))
    checking = args.min_seconds is not None or args.expect_tones or args.max_gap is not None
    return 1 if checking and problems else 0


if __name__ == '__main__':
    sys.exit(main())
