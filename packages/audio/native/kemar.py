"""Writes native/kemar.c from the "compact" KEMAR measurements of the MIT Media Lab (Bill Gardner
and Keith Martin, 1994: https://sound.media.mit.edu/resources/KEMAR.html, compact.zip), which
audio.hrtf reads. The data is Copyright 1994 by the MIT Media Lab, provided without restrictions on
use, the authors cited (THIRD_PARTY_NOTICES.md).

    python kemar.py <the folder of compact.zip, unzipped>

The folder holds elev-40 ... elev90, each a WAV file per direction (H<elevation>e<azimuth>a.wav):
two channels (the left ear, the right ear), 16 bits, 44.1 kHz, 128 frames, for azimuths of 0 to 180
degrees to the right (the left side is the same, the ears swapped).
"""
import os
import re
import sys
import wave

TAPS = 128
RATE = 44100


def read(folder):
    places = []
    for root, _, files in os.walk(folder):
        for name in files:
            m = re.fullmatch(r"H(-?\d+)e(\d{3})a\.wav", name)
            if not m:
                continue
            with wave.open(os.path.join(root, name)) as w:
                assert (w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()) == (2, 2, RATE, TAPS), name
                frames = w.readframes(TAPS)
            samples = [int.from_bytes(frames[i:i + 2], "little", signed=True) for i in range(0, len(frames), 2)]
            places.append((int(m.group(1)), int(m.group(2)), samples[0::2], samples[1::2]))
    places.sort(key=lambda p: (p[0], p[1]))
    return places


def write(places, out):
    lines = [
        "/* audio.hrtf: the head-related impulse responses of KEMAR, \"compact\" set, measured by Bill",
        "   Gardner and Keith Martin at the MIT Media Lab (1994). Copyright 1994 by the MIT Media Lab,",
        "   provided without restrictions on use, the authors cited (THIRD_PARTY_NOTICES.md).",
        "   Written by native/kemar.py: do not edit. */",
        "#include <stdint.h>",
        "",
        "#define COUNT %d" % len(places),
        "#define TAPS %d" % TAPS,
        "",
        "/* elevation, azimuth (degrees: azimuth 0 ahead, 90 right) */",
        "static const int16_t places[COUNT][2] = {",
    ]
    for e, a, _, _ in places:
        lines.append("    {%d, %d}," % (e, a))
    lines.append("};")
    lines.append("")
    lines.append("/* the left ear, then the right, 16 bits at 44.1 kHz */")
    lines.append("static const int16_t responses[COUNT][2][TAPS] = {")
    for e, a, left, right in places:
        lines.append("    {{%s}," % ",".join(map(str, left)))
        lines.append("     {%s}}," % ",".join(map(str, right)))
    lines.append("};")
    lines += [
        "",
        "int32_t sosu_audio_kemar_count(void) {",
        "    return COUNT;",
        "}",
        "",
        "int32_t sosu_audio_kemar_taps(void) {",
        "    return TAPS;",
        "}",
        "",
        "/* `out_places`: COUNT pairs of floats; `out_responses`: COUNT * 2 * TAPS floats, from -1 to 1. */",
        "void sosu_audio_kemar_copy(float *out_places, float *out_responses) {",
        "    for (int32_t i = 0; i < COUNT; i++) {",
        "        out_places[2 * i] = (float)places[i][0];",
        "        out_places[2 * i + 1] = (float)places[i][1];",
        "        for (int32_t ear = 0; ear < 2; ear++)",
        "            for (int32_t k = 0; k < TAPS; k++)",
        "                out_responses[(2 * i + ear) * TAPS + k] = (float)responses[i][ear][k] / 32768.0f;",
        "    }",
        "}",
        "",
    ]
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))


if __name__ == "__main__":
    places = read(sys.argv[1])
    assert len(places) == 368, len(places)
    write(places, os.path.join(os.path.dirname(os.path.abspath(__file__)), "kemar.c"))
    print("kemar.c: %d directions" % len(places))
