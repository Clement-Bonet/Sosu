"""Writes the files of tests/data for the test program `sounds` (the package audio, spec §8.2).

Run from the repository root: python -I tests/programs/sounds/tests/make_data.py

- WAV files written by another program than Sosu: Python's `wave` module (8, 16 and 24 bits),
  and by hand here (floats with extra chunks, the extensible header, hostile headers).
- tone.ogg: a small Ogg Vorbis file written by the minimal encoder below (one codebook of
  classes, one of values, floor 1 flat at 0 dB, residue 1, blocks of 256 samples), and
  tone_decoded.wav, what a decoder must give from it, computed here from the Vorbis I
  specification (inverse MDCT, window, overlap-add, the last granule position), not by stb_vorbis.
- Ogg files with hostile headers.
- tone.flac: a FLAC file written by the minimal encoder below (verbatim, constant and fixed
  subframes, left/side stereo, Rice codes, the CRCs), and tone_flac.wav, the same samples: FLAC is
  lossless, so a decoder must give them exactly.
- tone.mp3: MPEG-1 layer III frames written by hand, each granule one spectral line (Huffman table
  1, no scale factors, no bit reservoir): a decoder must give 1152 samples a frame, a tone near
  1021 Hz.
- FLAC and MP3 files damaged or hostile.
- small.sofa and cartesian.sofa: SOFA files (HDF5, as netCDF-4 writes them, by the minimal writer
  below: superblock 2, object headers 2, compact links and attributes, variables contiguous or in
  chunks of a B-tree 1, shuffled and deflated) of six directions whose responses are a few known
  impulses; the second gives its positions in cartesian coordinates, its ears right first, and a
  delay for each direction. SOFA files damaged or hostile. mit_kemar_normal_pinna.sofa is not
  written here: it is the file of sofacoustics.org (THIRD_PARTY_NOTICES.md).

The files are committed: nothing here runs during the tests.
"""

import math
import os
import struct
import wave
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "data")

RATE = 8000
FRAMES = 2000


def pcm16_pattern():
    """Stereo frames of 16 bits: 440 Hz on the left, 660 Hz on the right."""
    frames = []
    for i in range(FRAMES):
        left = round(20000 * math.sin(2 * math.pi * 440 * i / RATE))
        right = round(-12000 * math.sin(2 * math.pi * 660 * i / RATE)) + (i % 7) - 3
        frames.append((left, right))
    return frames


def write_wave(name, width, frames, channels=2):
    with wave.open(os.path.join(DATA, name), "wb") as w:
        w.setnchannels(channels)
        w.setsampwidth(width)
        w.setframerate(RATE)
        out = bytearray()
        for frame in frames:
            for v in frame:
                if width == 1:
                    out += struct.pack("<B", v)
                elif width == 2:
                    out += struct.pack("<h", v)
                else:
                    out += struct.pack("<i", v)[:3]
        w.writeframes(bytes(out))


def chunk(tag, body):
    pad = b"\0" if len(body) % 2 else b""
    return tag + struct.pack("<I", len(body)) + body + pad


def riff(chunks):
    body = b"WAVE" + b"".join(chunks)
    return b"RIFF" + struct.pack("<I", len(body)) + body


def fmt(tag, channels, bits, extra=b""):
    block = channels * bits // 8
    return struct.pack("<HHIIHH", tag, channels, RATE, RATE * block, block, bits) + extra


GUID_TAIL = b"\x00\x00\x00\x00\x10\x00\x80\x00\x00\xaa\x00\x38\x9b\x71"


def extensible(subformat, channels, bits, mask):
    extra = struct.pack("<HHI", 22, bits, mask) + struct.pack("<H", subformat) + GUID_TAIL
    return fmt(0xFFFE, channels, bits, extra)


def write(name, data):
    with open(os.path.join(DATA, name), "wb") as f:
        f.write(data)


def wav_files():
    frames = pcm16_pattern()
    write_wave("pcm16.wav", 2, frames)
    write_wave("pcm8.wav", 1, [tuple((v >> 8) + 128 for v in f) for f in frames])
    write_wave("pcm24.wav", 3, [tuple(v * 256 + (i * 37 % 256) - 128 for v in f) for i, f in enumerate(frames)])
    # floats of 32 bits: the same samples as pcm16.wav, exactly; a fmt chunk of 18 bytes, then a
    # fact chunk and a LIST chunk of an odd size (padded) before the data
    floats = b"".join(struct.pack("<ff", l / 32768, r / 32768) for l, r in frames)
    info = b"INFOISFT" + struct.pack("<I", 5) + b"sosu\0"
    write("float32.wav", riff([chunk(b"fmt ", fmt(3, 2, 32, struct.pack("<H", 0))),
                               chunk(b"fact", struct.pack("<I", FRAMES)), chunk(b"LIST", info + b"x"),
                               chunk(b"data", floats)]))
    # the extensible header: 24 bits stereo, and floats of 64 bits mono (the left channel)
    ints24 = b"".join(struct.pack("<i", v * 256)[:3] for f in frames for v in f)
    write("extensible24.wav", riff([chunk(b"fmt ", extensible(1, 2, 24, 3)), chunk(b"data", ints24)]))
    doubles = b"".join(struct.pack("<d", l / 32768) for l, r in frames)
    write("float64.wav", riff([chunk(b"fmt ", extensible(3, 1, 64, 4)), chunk(b"data", doubles)]))
    # hostile and damaged headers
    pcm = open(os.path.join(DATA, "pcm16.wav"), "rb").read()
    write("bad_huge_data.wav", pcm[:40] + struct.pack("<I", 0xFFFFFFF0) + pcm[44:1044])  # cut to the file
    write("bad_truncated.wav", pcm[:30])
    write("bad_no_format.wav", riff([chunk(b"data", b"\0" * 16)]))
    write("bad_channels.wav", riff([chunk(b"fmt ", fmt(1, 0, 16)), chunk(b"data", b"\0" * 16)]))
    write("bad_many_channels.wav", riff([chunk(b"fmt ", fmt(1, 300, 16)), chunk(b"data", b"\0" * 600)]))
    write("bad_block.wav", riff([chunk(b"fmt ", struct.pack("<HHIIHH", 1, 2, RATE, RATE * 4, 3, 16)),
                                 chunk(b"data", b"\0" * 16)]))
    write("bad_adpcm.wav", riff([chunk(b"fmt ", struct.pack("<HHIIHH", 2, 1, RATE, RATE, 256, 4) + b"\0" * 4),
                                 chunk(b"data", b"\0" * 16)]))
    write("bad_chunk_size.wav", riff([chunk(b"fmt ", fmt(1, 2, 16))]) +
          b"LIST" + struct.pack("<I", 0x7FFFFFFF) + b"abcd")
    write("bad_rate.wav", riff([chunk(b"fmt ", struct.pack("<HHIIHH", 1, 1, 0, 0, 2, 16)), chunk(b"data", b"\0" * 4)]))


# ---- A minimal Vorbis encoder ----

class Bits:
    """Bits packed as Vorbis packs them: the low bit of each byte first."""

    def __init__(self):
        self.out = bytearray()
        self.acc = 0
        self.n = 0

    def put(self, value, count):
        for i in range(count):
            self.acc |= ((value >> i) & 1) << self.n
            self.n += 1
            if self.n == 8:
                self.out.append(self.acc)
                self.acc = 0
                self.n = 0

    def word(self, codeword, length):
        """A Huffman codeword: its high bit first."""
        for i in reversed(range(length)):
            self.put((codeword >> i) & 1, 1)

    def bytes(self):
        return bytes(self.out) + (bytes([self.acc]) if self.n else b"")


def vorbis_float(value):
    """The float32 of the Vorbis codebooks: a mantissa of 21 bits, an exponent biased by 788."""
    if value == 0:
        return 0
    sign = 0x80000000 if value < 0 else 0
    mantissa, exponent = math.frexp(abs(value))  # value = mantissa * 2^exponent, 0.5 <= mantissa < 1
    m = int(mantissa * (1 << 21))
    assert m * 2.0 ** (exponent - 21) == abs(value) and m < (1 << 21)
    return sign | (exponent - 21 + 788) << 21 | m


BLOCK = 256  # both block sizes
HALF = BLOCK // 2
PARTITION = 32
VALUES = 16  # entries of the book of values: -1 to 0.875 by 0.125
MINIMUM = -1.0
DELTA = 0.125
CHANNELS = 2


def identification():
    return (b"\x01vorbis" + struct.pack("<IBIiiiBB", 0, CHANNELS, RATE, 0, 0, 0, 8 << 4 | 8, 1))


def comment(vendor=b"Sosu tests", vendor_length=None):
    n = len(vendor) if vendor_length is None else vendor_length
    return b"\x03vorbis" + struct.pack("<I", n) + vendor + struct.pack("<I", 0) + b"\x01"


def setup(value_entries=VALUES):
    b = Bits()
    for c in b"\x05vorbis":
        b.put(c, 8)
    b.put(2 - 1, 8)  # 2 codebooks
    # book 0, of the classes: 1 dimension, 2 entries of 1 bit
    b.put(0x564342, 24)
    b.put(1, 16)
    b.put(2, 24)
    b.put(0, 1)  # not ordered
    b.put(0, 1)  # not sparse
    b.put(0, 5)
    b.put(0, 5)
    b.put(0, 4)  # no lookup
    # book 1, of the values: 1 dimension, 16 entries of 4 bits, lookup 1: -1 + 0.125 i
    b.put(0x564342, 24)
    b.put(1, 16)
    b.put(value_entries, 24)
    b.put(0, 1)
    b.put(0, 1)
    for _ in range(min(value_entries, VALUES)):
        b.put(4 - 1, 5)
    b.put(1, 4)
    b.put(vorbis_float(MINIMUM), 32)
    b.put(vorbis_float(DELTA), 32)
    b.put(4 - 1, 4)  # bits of the multiplicands
    b.put(0, 1)  # not a sequence
    for i in range(VALUES):
        b.put(i, 4)
    b.put(0, 6)  # 1 time transform, of type 0
    b.put(0, 16)
    b.put(0, 6)  # 1 floor, of type 1: no partition, two posts (x = 0 and 128), multiplier 1
    b.put(1, 16)
    b.put(0, 5)
    b.put(0, 2)
    b.put(7, 4)
    b.put(0, 6)  # 1 residue, of type 1, over the 128 coefficients, partitions of 32, 2 classes
    b.put(1, 16)
    b.put(0, 24)
    b.put(HALF, 24)
    b.put(PARTITION - 1, 24)
    b.put(2 - 1, 6)
    b.put(0, 8)  # the book of the classes
    b.put(0, 3)  # class 0: no pass (silent)
    b.put(0, 1)
    b.put(1, 3)  # class 1: pass 0, with book 1
    b.put(0, 1)
    b.put(1, 8)
    b.put(0, 6)  # 1 mapping, of type 0: 1 submap, no coupling
    b.put(0, 16)
    b.put(0, 1)
    b.put(0, 1)
    b.put(0, 2)
    b.put(0, 8)
    b.put(0, 8)  # floor 0
    b.put(0, 8)  # residue 0
    b.put(0, 6)  # 1 mode: short blocks, mapping 0
    b.put(0, 1)
    b.put(0, 16)
    b.put(0, 16)
    b.put(0, 8)
    b.put(1, 1)  # framing
    return b.bytes()


def spectrum(packet, channel):
    """The coefficients of a block: a few values of the book, changing from block to block."""
    x = [0.0] * HALF
    if channel == 0:
        x[10] = 0.5
        x[45] = 0.25 if packet % 2 else -0.25
    else:
        x[20] = -0.75
        x[100] = 0.125 * (packet % 4)  # partition 3 silent every 4 blocks
        x[64 + packet % 16] = 0.375
    return x


def entry_of(value):
    e = round((value - MINIMUM) / DELTA)
    assert 0 <= e < VALUES and MINIMUM + e * DELTA == value
    return e


def audio_packet(packet):
    b = Bits()
    b.put(0, 1)  # an audio packet; the mode takes no bit (one mode)
    for _ in range(CHANNELS):
        b.put(1, 1)  # the floor is used
        b.put(255, 8)  # its two posts at 255: 0 dB (1.0)
        b.put(255, 8)
    spectra = [spectrum(packet, c) for c in range(CHANNELS)]
    classes = [[1 if any(s[p * PARTITION:(p + 1) * PARTITION]) else 0 for p in range(HALF // PARTITION)]
               for s in spectra]
    for p in range(HALF // PARTITION):
        for c in range(CHANNELS):
            b.word(classes[c][p], 1)
        for c in range(CHANNELS):
            if classes[c][p]:
                for v in spectra[c][p * PARTITION:(p + 1) * PARTITION]:
                    b.word(entry_of(v), 4)
    return b.bytes()


def crc32(data):
    crc = 0
    for byte in data:
        crc ^= byte << 24
        for _ in range(8):
            crc = (crc << 1) ^ 0x04C11DB7 if crc & 0x80000000 else crc << 1
            crc &= 0xFFFFFFFF
    return crc


def page(packets, granule, sequence, kind, serial=0x536F7375):
    lacing = bytearray()
    for p in packets:
        lacing += bytes([255] * (len(p) // 255) + [len(p) % 255])
    assert len(lacing) <= 255
    header = struct.pack("<4sBBqIIIB", b"OggS", 0, kind, granule, serial, sequence, 0, len(lacing)) + lacing
    body = b"".join(packets)
    crc = crc32(header + body)
    return header[:22] + struct.pack("<I", crc) + header[26:] + body


PACKETS = 40
PER_PAGE = 10
TRIM = 50  # the last granule position cuts the last block


def ogg(comment_packet=None, setup_packet=None, last_granule=None, serial=0x536F7375):
    pages = [page([identification()], 0, 0, 0x02, serial),
             page([comment_packet or comment(), setup_packet or setup()], 0, 1, 0, serial)]
    for start in range(0, PACKETS, PER_PAGE):
        end = start + PER_PAGE
        granule = (end - 1) * HALF - (TRIM if end == PACKETS else 0)
        if end == PACKETS and last_granule is not None:
            granule = last_granule
        packets = [audio_packet(p) for p in range(start, end)]
        pages.append(page(packets, granule, len(pages), 0x04 if end == PACKETS else 0, serial))
    return b"".join(pages)


# ---- What a decoder gives (Vorbis I, section 1.3.2: inverse MDCT, window, overlap-add) ----

def imdct(x):
    n = 2 * len(x)
    return [sum(x[k] * math.cos(2 * math.pi / n * (i + 0.5 + n / 4) * (k + 0.5)) for k in range(len(x)))
            for i in range(n)]


def window(i, n):
    return math.sin(math.pi / 2 * math.sin((i + 0.5) / n * math.pi) ** 2)


def decoded():
    """The frames of tone.ogg: from the middle of each block to the middle of the next."""
    blocks = []
    for p in range(PACKETS):
        blocks.append([[window(i, BLOCK) * v for i, v in enumerate(imdct(spectrum(p, c)))] for c in range(CHANNELS)])
    frames = []
    for p in range(1, PACKETS):
        for i in range(HALF):
            frames.append(tuple(blocks[p - 1][c][HALF + i] + blocks[p][c][i] for c in range(CHANNELS)))
    return frames[:(PACKETS - 1) * HALF - TRIM]


def ogg_files():
    tone = ogg()
    write("tone.ogg", tone)
    floats = b"".join(struct.pack("<ff", *f) for f in decoded())
    write("tone_decoded.wav", riff([chunk(b"fmt ", fmt(3, 2, 32)), chunk(b"data", floats)]))
    write("bad_truncated.ogg", tone[:len(tone) // 2])
    write("bad_vendor.ogg", ogg(comment_packet=comment(b"", 0xFFFFFFFF)))  # CVE-2023-45675: the comment header is skipped unread
    write("bad_huge_codebook.ogg", ogg(setup_packet=setup(0xFFFFFF)))  # 16 MB of lengths
    damaged = bytearray(tone)
    damaged[len(tone) // 3] ^= 0xFF
    write("bad_crc.ogg", bytes(damaged))
    write("bad_not_vorbis.ogg", page([b"\x01theora" + b"\0" * 22], 0, 0, 0x02))
    write("bad_long.ogg", ogg(last_granule=0x80000004))  # a length above INT_MAX (make fuzz-ogg)
    # chained: two logical streams one after the other (as a radio recorded, or tracks joined)
    write("chained.ogg", tone + ogg(serial=0x536F7376))
    write("bad_chained.ogg", tone + tone[:len(tone) // 2] + b"OggS\x00\x02")  # its second link cut, then a page cut


class BigBits:
    """Bits written from the most significant, as FLAC and MP3 read them."""

    def __init__(self):
        self.bits = []

    def put(self, value, count):
        for i in range(count - 1, -1, -1):
            self.bits.append((value >> i) & 1)

    def align(self):
        while len(self.bits) % 8:
            self.bits.append(0)

    def bytes(self):
        self.align()
        return bytes(int("".join(map(str, self.bits[i:i + 8])), 2) for i in range(0, len(self.bits), 8))


def crc8(data):
    crc = 0
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc


def crc16(data):
    crc = 0
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x8005) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


FLAC_RATE = 44100
FLAC_BLOCK = 4096


def flac_samples():
    """Left: a tone of 440 Hz; right: one of 660 Hz, constant (1234) during the third frame."""
    frames = []
    total = 3 * FLAC_BLOCK + 1000
    for i in range(total):
        left = int(round(16000 * math.sin(2 * math.pi * 440 * i / FLAC_RATE)))
        right = int(round(9000 * math.sin(2 * math.pi * 660 * i / FLAC_RATE))) + (i % 7) - 3
        if 2 * FLAC_BLOCK <= i < 3 * FLAC_BLOCK:
            right = 1234
        frames.append((left, right))
    return frames


def subframe(bits, kind, samples, bps, order=0):
    """A subframe: VERBATIM, CONSTANT, or FIXED of `order` with one Rice partition."""
    bits.put(0, 1)
    if kind == "constant":
        bits.put(0b000000, 6)
        bits.put(0, 1)
        bits.put(samples[0] & ((1 << bps) - 1), bps)
        return
    if kind == "verbatim":
        bits.put(0b000001, 6)
        bits.put(0, 1)
        for v in samples:
            bits.put(v & ((1 << bps) - 1), bps)
        return
    bits.put(0b001000 | order, 6)
    bits.put(0, 1)
    for v in samples[:order]:
        bits.put(v & ((1 << bps) - 1), bps)
    coefficients = {0: [], 1: [1], 2: [2, -1], 3: [3, -3, 1], 4: [4, -6, 4, -1]}[order]
    residuals = []
    for n in range(order, len(samples)):
        guess = sum(c * samples[n - 1 - k] for k, c in enumerate(coefficients))
        residuals.append(samples[n] - guess)
    zigzag = [2 * r if r >= 0 else -2 * r - 1 for r in residuals]
    mean = sum(zigzag) / max(len(zigzag), 1)
    k = max(0, min(14, int(math.log2(mean)) if mean >= 1 else 0))
    bits.put(0, 2)  # Rice, parameters of 4 bits
    bits.put(0, 4)  # partition order 0
    bits.put(k, 4)
    for u in zigzag:
        bits.put(0, u >> k)
        bits.put(1, 1)
        bits.put(u & ((1 << k) - 1), k)


def flac_frame(number, block):
    """A frame of `block` (one (left, right) per sample), coded as `number` says."""
    size = len(block)
    header = BigBits()
    header.put(0xFFF8, 16)  # sync, fixed block size
    header.put(12 if size == FLAC_BLOCK else 7, 4)  # 4096, or 16 bits of size - 1 at the end
    header.put(9, 4)  # 44.1 kHz
    header.put(8 if number == 1 else 1, 4)  # left/side, or two independent channels
    header.put(4, 3)  # 16 bits
    header.put(0, 1)
    header.put(number, 8)  # the frame number, as UTF-8 (< 128)
    if size != FLAC_BLOCK:
        header.put(size - 1, 16)
    head = header.bytes()
    body = BigBits()
    left = [f[0] for f in block]
    right = [f[1] for f in block]
    if number == 0:
        subframe(body, "verbatim", left, 16)
        subframe(body, "fixed", right, 16, 2)
    elif number == 1:
        subframe(body, "fixed", left, 16, 1)
        subframe(body, "fixed", [x - y for x, y in zip(left, right)], 17, 1)
    elif number == 2:
        subframe(body, "fixed", left, 16, 4)
        subframe(body, "constant", right, 16)
    else:
        subframe(body, "fixed", left, 16, 3)
        subframe(body, "fixed", right, 16, 0)
    frame = head + bytes([crc8(head)]) + body.bytes()
    return frame + struct.pack(">H", crc16(frame))


def flac(frames, channels=2, rate=FLAC_RATE, max_block=FLAC_BLOCK):
    info = BigBits()
    info.put(FLAC_BLOCK, 16)
    info.put(max_block, 16)
    info.put(0, 24)
    info.put(0, 24)
    info.put(rate, 20)
    info.put(channels - 1, 3)
    info.put(15, 5)
    info.put(len(frames), 36)
    info.put(0, 128)  # no MD5
    streaminfo = info.bytes()
    out = b"fLaC" + bytes([0x80, 0, 0, len(streaminfo)]) + streaminfo
    for n in range(0, (len(frames) + FLAC_BLOCK - 1) // FLAC_BLOCK):
        out += flac_frame(n, frames[n * FLAC_BLOCK:(n + 1) * FLAC_BLOCK])
    return out


MP3_FRAMES = 20
MP3_LINE = 24  # the spectral line of each granule: (24 + 0.5) * 48000 / 1152 = 1020.8 Hz


def mp3_frame():
    """One frame of MPEG-1 layer III, 128 kb/s, 48 kHz, mono: 384 bytes, its two granules alike."""
    pairs = MP3_LINE // 2 + 1
    granule = BigBits()
    for p in range(pairs):
        if p == MP3_LINE // 2:
            granule.put(0b01, 2)  # table 1: (1, 0)
            granule.put(0, 1)     # its sign: positive
        else:
            granule.put(1, 1)     # (0, 0)
    length = len(granule.bits)
    side = BigBits()
    side.put(0, 9)  # main_data_begin: no reservoir
    side.put(0, 5)
    side.put(0, 4)  # scfsi
    for _ in range(2):
        side.put(length, 12)  # part2_3_length
        side.put(pairs, 9)    # big_values
        side.put(200, 8)      # global_gain: 2^((200 - 210) / 4)
        side.put(0, 4)        # scalefac_compress: no scale factors
        side.put(0, 1)        # long blocks
        for _ in range(3):
            side.put(1, 5)    # table 1 in the three regions
        side.put(7, 4)
        side.put(7, 3)
        side.put(0, 3)        # preflag, scalefac_scale, count1table_select
    main = BigBits()
    main.bits = granule.bits + granule.bits
    data = bytes([0xFF, 0xFB, 0x94, 0xC0]) + side.bytes() + main.bytes()
    return data + b"\0" * (384 - len(data))


def coded_files():
    frames = flac_samples()
    tone = flac(frames)
    write("tone.flac", tone)
    pcm = b"".join(struct.pack("<hh", *f) for f in frames)
    write("tone_flac.wav", riff([chunk(b"fmt ", struct.pack("<HHIIHH", 1, 2, FLAC_RATE, FLAC_RATE * 4, 4, 16)),
                                 chunk(b"data", pcm)]))
    write("bad_truncated.flac", tone[:len(tone) // 2])
    damaged = bytearray(tone)
    damaged[len(tone) // 3] ^= 0xFF
    write("bad_crc.flac", bytes(damaged))
    write("bad_header.flac", tone[:4] + b"\xFF" * 60)
    write("bad_huge_block.flac", flac(frames[:FLAC_BLOCK], channels=8, max_block=65535))  # 8 channels promised, 2 given
    write("bad_rate.flac", flac(frames[:FLAC_BLOCK], rate=0))
    mp3 = mp3_frame() * MP3_FRAMES
    write("tone.mp3", mp3)
    write("tone_id3.mp3", b"ID3\x04\x00\x00\x00\x00\x00\x0A" + b"\0" * 10 + mp3)  # an empty tag of 10 bytes first
    write("bad_truncated.mp3", mp3[:1000])
    write("bad_garbage.mp3", bytes([0xFF, 0xFB]) + bytes((i * 89 + 7) % 256 for i in range(3000)))


# ---- SOFA: HDF5 files of the parts audio.hdf5 reads ----

def rot(x, k):
    return ((x << k) | (x >> (32 - k))) & 0xFFFFFFFF


def lookup3(data, init=0):
    """Bob Jenkins' hashlittle, the checksum of the structures of HDF5 1.8."""
    n = len(data)
    a = b = c = (0xDEADBEEF + n + init) & 0xFFFFFFFF
    i = 0
    while n > 12:
        a = (a + int.from_bytes(data[i:i + 4], "little")) & 0xFFFFFFFF
        b = (b + int.from_bytes(data[i + 4:i + 8], "little")) & 0xFFFFFFFF
        c = (c + int.from_bytes(data[i + 8:i + 12], "little")) & 0xFFFFFFFF
        a = (a - c) & 0xFFFFFFFF; a ^= rot(c, 4); c = (c + b) & 0xFFFFFFFF
        b = (b - a) & 0xFFFFFFFF; b ^= rot(a, 6); a = (a + c) & 0xFFFFFFFF
        c = (c - b) & 0xFFFFFFFF; c ^= rot(b, 8); b = (b + a) & 0xFFFFFFFF
        a = (a - c) & 0xFFFFFFFF; a ^= rot(c, 16); c = (c + b) & 0xFFFFFFFF
        b = (b - a) & 0xFFFFFFFF; b ^= rot(a, 19); a = (a + c) & 0xFFFFFFFF
        c = (c - b) & 0xFFFFFFFF; c ^= rot(b, 4); b = (b + a) & 0xFFFFFFFF
        n -= 12
        i += 12
    if n == 0:
        return c
    tail = data[i:] + b"\0" * (12 - n)
    a = (a + int.from_bytes(tail[0:4], "little")) & 0xFFFFFFFF
    b = (b + int.from_bytes(tail[4:8], "little")) & 0xFFFFFFFF
    c = (c + int.from_bytes(tail[8:12], "little")) & 0xFFFFFFFF
    c ^= b; c = (c - rot(b, 14)) & 0xFFFFFFFF
    a ^= c; a = (a - rot(c, 11)) & 0xFFFFFFFF
    b ^= a; b = (b - rot(a, 25)) & 0xFFFFFFFF
    c ^= b; c = (c - rot(b, 16)) & 0xFFFFFFFF
    a ^= c; a = (a - rot(c, 4)) & 0xFFFFFFFF
    b ^= a; b = (b - rot(a, 14)) & 0xFFFFFFFF
    c ^= b; c = (c - rot(b, 24)) & 0xFFFFFFFF
    return c


UNDEF = 0xFFFFFFFFFFFFFFFF


class Hdf5:
    """A file of HDF5: its root group of variables of doubles, each with string attributes."""

    def __init__(self):
        self.out = bytearray(48)  # the superblock, written last
        self.links = []

    def add(self, data):
        at = len(self.out)
        self.out += data
        while len(self.out) % 8:
            self.out += b"\0"
        return at

    @staticmethod
    def message(kind, body):
        return struct.pack("<BHB", kind, len(body), 0) + body

    def header(self, messages):
        body = b"".join(messages)
        head = b"OHDR" + bytes([2, 0x02]) + struct.pack("<I", len(body))
        data = head + body
        return self.add(data + struct.pack("<I", lookup3(data)))

    @staticmethod
    def dataspace(dims):
        return Hdf5.message(1, bytes([2, len(dims), 0, 1]) + b"".join(struct.pack("<Q", d) for d in dims))

    @staticmethod
    def double():
        return Hdf5.message(3, bytes([0x11, 0x20, 0x3F, 0x00]) + struct.pack("<IHHBBBBI", 8, 0, 64, 52, 11, 0, 52, 1023))

    @staticmethod
    def attribute(name, text):
        n = name.encode() + b"\0"
        value = text.encode()
        kind = bytes([0x13, 0, 0, 0]) + struct.pack("<I", len(value))
        space = bytes([2, 0, 0, 0])
        body = bytes([3, 0]) + struct.pack("<HHHB", len(n), len(kind), len(space), 0) + n + kind + space + value
        return Hdf5.message(12, body)

    def variable(self, name, dims, values, attributes=(), chunk=None, filters=(2, 1), layout_version=3,
                 tree_at=None, filter_id=None, declared=None):
        raw = struct.pack("<%dd" % len(values), *values)
        messages = [self.dataspace(declared or dims), self.double()]
        if chunk is None:
            at = self.add(raw)
            messages.append(self.message(8, bytes([layout_version, 1]) + struct.pack("<QQ", at, len(raw))))
        else:
            tree = self.chunks(dims, chunk, values, filters)
            pipe = bytes([2, len(filters)])
            for f in filters:
                fid = filter_id if filter_id is not None else f
                pipe += struct.pack("<HHHI", fid, 0, 1, 8 if f == 2 else 6)
            messages.append(self.message(11, pipe))
            messages.append(self.message(8, bytes([layout_version, 2, len(dims) + 1])
                                         + struct.pack("<Q", tree if tree_at is None else tree_at)
                                         + b"".join(struct.pack("<I", c) for c in chunk) + struct.pack("<I", 8)))
        for a, t in attributes:
            messages.append(self.attribute(a, t))
        self.links.append((name, self.header(messages)))

    def chunks(self, dims, chunk, values, filters):
        """The chunks of a variable, and the B-tree (version 1, one leaf) that indexes them."""
        grid = [range(0, d, c) for d, c in zip(dims, chunk)]
        entries = []

        def walk(prefix, k):
            if k == len(dims):
                yield prefix
                return
            for o in grid[k]:
                yield from walk(prefix + [o], k + 1)

        for origin in walk([], 0):
            cells = []
            for i in range(math.prod(chunk)):
                idx = []
                r = i
                for c in reversed(chunk):
                    idx.append(r % c)
                    r //= c
                idx.reverse()
                pos = [o + j for o, j in zip(origin, idx)]
                if any(q >= d for q, d in zip(pos, dims)):
                    cells.append(0.0)
                    continue
                flat = 0
                for q, d in zip(pos, dims):
                    flat = flat * d + q
                cells.append(values[flat])
            data = struct.pack("<%dd" % len(cells), *cells)
            for f in filters:
                if f == 2:  # shuffle: the first byte of every element, then the second...
                    n = len(data) // 8
                    data = bytes(data[(i % n) * 8 + i // n] for i in range(len(data)))
                elif f == 1:
                    data = zlib.compress(data, 6)
            entries.append((origin, self.add(data), len(data)))
        node = b"TREE" + bytes([1, 0]) + struct.pack("<HQQ", len(entries), UNDEF, UNDEF)
        for origin, at, size in entries:
            node += struct.pack("<II", size, 0) + b"".join(struct.pack("<Q", o) for o in origin + [0])
            node += struct.pack("<Q", at)
        node += struct.pack("<II", 0, 0) + b"".join(struct.pack("<Q", d) for d in list(dims) + [0])
        return self.add(node)

    def bytes(self, version=2):
        links = []
        for name, at in self.links:
            n = name.encode()
            links.append(self.message(6, bytes([1, 0, len(n)]) + n + struct.pack("<Q", at)))
        root = self.header(links)
        sb = b"\x89HDF\r\n\x1a\n" + bytes([version, 8, 8, 0]) + struct.pack("<QQQQ", 0, UNDEF, len(self.out), root)
        self.out[0:48] = sb + struct.pack("<I", lookup3(sb))
        return bytes(self.out)


# Six directions (SOFA: the azimuth turning left from ahead), and for each ear the frame of its
# impulse and its height: the left first and louder for a sound on the left.
SOFA_TAPS = 32
SOFA_PLACES = [(0, 0), (90, 0), (270, 0), (180, 0), (0, 90), (45, -30)]
SOFA_EARS = [((8, 0.5), (8, 0.5)), ((4, 0.8), (12, 0.2)), ((12, 0.2), (4, 0.8)), ((8, 0.35), (8, 0.35)),
             ((6, 0.4), (6, 0.4)), ((6, 0.6), (9, 0.35))]


def sofa_response(onset, height):
    h = [0.0] * SOFA_TAPS
    h[onset] = height
    h[onset + 1] = -height / 2
    h[onset + 3] = height / 4
    return h


def sofa(cartesian=False, right_first=False, delays=None, version=2, dims=None, layout_version=3, tree_at=None,
         filter_id=None, links=None, receivers=None):
    f = Hdf5()
    ir = []
    for ears in SOFA_EARS:
        order = [ears[1], ears[0]] if right_first else [ears[0], ears[1]]
        for onset, height in order:
            ir += sofa_response(onset, height)
    m = len(SOFA_PLACES)
    f.variable("Data.IR", [m, 2, SOFA_TAPS], ir, chunk=[4, 1, SOFA_TAPS], layout_version=layout_version,
               tree_at=tree_at, filter_id=filter_id, declared=dims)
    f.variable("Data.SamplingRate", [1], [44100.0], [("Units", "hertz")])
    if cartesian:
        where = []
        for az, el in SOFA_PLACES:
            a, e = math.radians(az), math.radians(el)
            where += [1.2 * math.cos(e) * math.cos(a), 1.2 * math.cos(e) * math.sin(a), 1.2 * math.sin(e)]
        f.variable("SourcePosition", [m, 3], where, [("Type", "cartesian"), ("Units", "metre")])
    else:
        where = []
        for az, el in SOFA_PLACES:
            where += [float(az), float(el), 1.2]
        f.variable("SourcePosition", [m, 3], where, [("Type", "spherical"), ("Units", "degree, degree, metre")])
    ears = [0.0, 0.09, 0.0, 0.0, -0.09, 0.0]
    if right_first:
        ears = ears[3:] + ears[:3]
    f.variable("ReceiverPosition", [2, 3, 1], ears, [("Type", "cartesian")], declared=receivers)
    if delays is None:
        f.variable("Data.Delay", [1, 2], [0.0, 0.0])
    else:
        f.variable("Data.Delay", [m, 2], delays)
    if links is not None:
        f.links = [(name, at if name not in links else links[name]) for name, at in f.links]
    return f.bytes(version)


def sofa_files():
    write("small.sofa", sofa())
    # the same heads: cartesian, the right ear first, 3 frames of delay on the left and 1 on the right
    delays = []
    for _ in SOFA_PLACES:
        delays += [1.0, 3.0]  # right first
    write("cartesian.sofa", sofa(cartesian=True, right_first=True, delays=delays))
    good = sofa()
    write("bad_truncated.sofa", good[:len(good) // 2])
    write("bad_superblock0.sofa", sofa(version=0))
    write("bad_huge.sofa", sofa(dims=[1 << 40, 2, SOFA_TAPS]))
    write("bad_layout4.sofa", sofa(layout_version=4))
    write("bad_tree.sofa", sofa(tree_at=60))
    write("bad_filter.sofa", sofa(filter_id=32001))  # blosc: not read
    write("bad_link.sofa", sofa(links={"Data.IR": 1 << 40}))
    write("not_sofa.sofa", Hdf5().bytes())
    write("bad_receivers.sofa", sofa(receivers=[2, 3, 0]))  # no position of the ears: found by fuzzing


if __name__ == "__main__":
    os.makedirs(DATA, exist_ok=True)
    wav_files()
    ogg_files()
    coded_files()
    sofa_files()
