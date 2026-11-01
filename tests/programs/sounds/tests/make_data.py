"""Writes the files of tests/data for the test program `sounds` (the package audio, spec §8.2).

Run from the repository root: python -I tests/programs/sounds/tests/make_data.py

- WAV files written by another program than Sosu: Python's `wave` module (8, 16 and 24 bits),
  and by hand here (floats with extra chunks, the extensible header, hostile headers).
- tone.ogg: a small Ogg Vorbis file written by the minimal encoder below (one codebook of
  classes, one of values, floor 1 flat at 0 dB, residue 1, blocks of 256 samples), and
  tone_decoded.wav, what a decoder must give from it, computed here from the Vorbis I
  specification (inverse MDCT, window, overlap-add, the last granule position), not by stb_vorbis.
- Ogg files with hostile headers.

The files are committed: nothing here runs during the tests.
"""

import math
import os
import struct
import wave

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


def page(packets, granule, sequence, kind):
    lacing = bytearray()
    for p in packets:
        lacing += bytes([255] * (len(p) // 255) + [len(p) % 255])
    assert len(lacing) <= 255
    header = struct.pack("<4sBBqIIIB", b"OggS", 0, kind, granule, 0x536F7375, sequence, 0, len(lacing)) + lacing
    body = b"".join(packets)
    crc = crc32(header + body)
    return header[:22] + struct.pack("<I", crc) + header[26:] + body


PACKETS = 40
PER_PAGE = 10
TRIM = 50  # the last granule position cuts the last block


def ogg(comment_packet=None, setup_packet=None):
    pages = [page([identification()], 0, 0, 0x02),
             page([comment_packet or comment(), setup_packet or setup()], 0, 1, 0)]
    for start in range(0, PACKETS, PER_PAGE):
        end = start + PER_PAGE
        granule = (end - 1) * HALF - (TRIM if end == PACKETS else 0)
        packets = [audio_packet(p) for p in range(start, end)]
        pages.append(page(packets, granule, len(pages), 0x04 if end == PACKETS else 0))
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


if __name__ == "__main__":
    os.makedirs(DATA, exist_ok=True)
    wav_files()
    ogg_files()
