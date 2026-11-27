# Decode Rogue Leader voice lines to WAV files, by the id a mission script uses (action 0x1A).
#   python tools/r2voice.py <outdir> <id hex>...      e.g. python tools/r2voice.py build/voices C63 C39
# Reads the extracted disc (orig/GSWE64/files/Rogue2): the voice directory is data.dat's
# "audio/voiceDir" resource (16-byte header, then 56-byte entries: u32 flags | rate, u32 offset
# and u32 size of the line in r2voice.vdy, 8 unused bytes, 16 DSP-ADPCM coefficients). Script id
# N plays entry N - 1 (VoiceEnqueue is called with id - 1).
import os
import struct
import sys
import wave

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "orig", "GSWE64", "files", "Rogue2")


def voice_dir():
    for line in open(os.path.join(ROOT, "data.dir"), encoding="latin-1"):
        parts = line.split()
        if parts and parts[0] == "audio/voiceDir":
            with open(os.path.join(ROOT, "data.dat"), "rb") as f:
                f.seek(int(parts[1]))
                return f.read(int(parts[2]))
    raise SystemExit("audio/voiceDir not found in data.dir")


def decode(vdir, vdy, entry):
    o = 16 + entry * 56
    flags, off, _, size = struct.unpack(">IIII", vdir[o:o + 16])
    coefs = struct.unpack(">16h", vdir[o + 24:o + 56])
    vdy.seek(off)
    data = vdy.read(size)
    h1 = h2 = 0
    out = []
    for f in range(0, len(data) - 7, 8):  # DSP ADPCM: 8-byte frames of 14 samples
        hdr = data[f]
        scale = 1 << (hdr & 15)
        c1, c2 = coefs[(hdr >> 4 & 7) * 2], coefs[(hdr >> 4 & 7) * 2 + 1]
        for i in range(14):
            b = data[f + 1 + i // 2]
            n = (b >> 4) if i % 2 == 0 else (b & 15)
            if n >= 8:
                n -= 16
            s = (((n * scale) << 11) + c1 * h1 + c2 * h2 + 1024) >> 11
            s = max(-32768, min(32767, s))
            out.append(s)
            h2, h1 = h1, s
    return flags & 0xFFFF, out


def main(argv):
    if len(argv) < 3:
        raise SystemExit(__doc__ or "usage: r2voice.py <outdir> <id hex>...")
    os.makedirs(argv[1], exist_ok=True)
    vdir = voice_dir()
    with open(os.path.join(ROOT, "r2voice.vdy"), "rb") as vdy:
        for idh in argv[2:]:
            sid = int(idh, 16)
            rate, pcm = decode(vdir, vdy, sid - 1)
            path = os.path.join(argv[1], f"v{sid:04X}.wav")
            with wave.open(path, "wb") as w:
                w.setnchannels(1)
                w.setsampwidth(2)
                w.setframerate(rate)
                w.writeframes(struct.pack(f"<{len(pcm)}h", *pcm))
            print(f"{path}: {len(pcm) / rate:.1f} s")


if __name__ == "__main__":
    main(sys.argv)
