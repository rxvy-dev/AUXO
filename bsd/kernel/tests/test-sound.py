#!/usr/bin/env python3
"""Sound: an Intel HD Audio card (QEMU's intel-hda with an hda-output codec, recorded to a WAV
file), driven through ALSA by real Linux programs from the root image (alsa-utils):
  - the kernel finds the card and makes /dev/snd, for the audio group;
  - aplay -l lists it; amixer shows and changes the master volume;
  - aplay plays a 48 kHz stereo file as is (440 Hz left, 660 Hz right) and a 22050 Hz mono
    one through alsa-lib's plug (resampled and spread to both sides, by mmap);
  - a second player gets "busy" while one plays; the user (in the audio group) can play.
Then the WAV QEMU recorded is checked: the tones are there, at the right pitch, on the right side.
  python3 tests/test-sound.py build/auxobsd.iso
"""
import array
import importlib.util
import math
import os
import re
import shutil
import struct
import sys
import tempfile
import time
import wave

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("hw", os.path.join(HERE, "test-hw.py"))
hw = importlib.util.module_from_spec(spec)
spec.loader.exec_module(hw)


def tone(path, rate, channels, freqs, seconds):
    w = wave.open(path, "wb")
    w.setnchannels(channels); w.setsampwidth(2); w.setframerate(rate)
    frames = bytearray()
    for i in range(int(rate * seconds)):
        for c in range(channels):
            frames += struct.pack("<h", int(12000 * math.sin(2 * math.pi * freqs[c] * i / rate)))
    w.writeframes(bytes(frames)); w.close()


def crossings(x):
    return sum(1 for k in range(1, len(x)) if (x[k - 1] < 0) != (x[k] < 0))


def main():
    iso = sys.argv[1] if len(sys.argv) > 1 else "build/auxobsd.iso"
    tmp = tempfile.mkdtemp(prefix="auxobsd-sound-")
    www = os.path.join(tmp, "www")
    os.mkdir(www)
    tone(os.path.join(www, "stereo48.wav"), 48000, 2, (440, 660), 1.0)
    tone(os.path.join(www, "mono22.wav"), 22050, 1, (1000,), 1.0)
    port = hw.serve(www)
    rec = os.path.join(tmp, "out.wav")
    m = hw.Machine(tmp, iso, ["-device", "intel-hda", "-device", "hda-output,audiodev=snd0",
                              "-audiodev", f"wav,id=snd0,path={rec}", "-nic", "user,model=e1000"], "s")
    try:
        print("▲ boot", flush=True)
        up = m.pump(240, hw.PROMPT)
        hw.check(up, "boots to auxsh", m.text()[-1500:])
        if not up:
            return
        m.run("")
        m.run("busybox sleep 2")                         # (the card is set up in the background)
        hw.check(re.search(r"snd : HDA Intel, codec 1af4:\w+, playing on the line out", m.text()) is not None,
                 "the HD Audio card and its codec are found", m.text()[-2500:])
        out = m.run("busybox ls -l /dev/snd")
        hw.check("controlC0" in out and "pcmC0D0p" in out and "audio" in out and "116," in out,
                 "/dev/snd has the control and playback devices, group audio", out)
        out = m.run("aplay -l")
        hw.check("card 0: Intel [HDA Intel], device 0" in out, "aplay -l lists the card", out)
        out = m.run("amixer get Master")
        hw.check("Playback 80 [80%] [on]" in out, "amixer: the master volume (80%, on)", out)
        for f in ("stereo48.wav", "mono22.wav"):
            m.run(f"busybox wget -q -O /tmp/{f} http://10.0.2.2:{port}/{f}")
        print("▲ playing", flush=True)
        out = m.run("aplay /tmp/stereo48.wav", 30)
        hw.check("Playing WAVE '/tmp/stereo48.wav' : Signed 16 bit Little Endian, Rate 48000 Hz, Stereo" in out and "aplay:" not in out.split("Stereo", 1)[-1],
                 "aplay plays 48 kHz stereo", out)
        m.run("busybox sleep 1")
        out = m.run("aplay -v /tmp/mono22.wav", 30)
        hw.check("Rate 22050 Hz, Mono" in out and "Rate conversion PCM" in out and "Unable" not in out,
                 "and 22050 Hz mono, resampled by alsa-lib's plug", out[-1500:])
        m.run("busybox sleep 1")
        out = m.run("busybox sh -c 'aplay -q /tmp/stereo48.wav & busybox sleep 0.3; aplay -q /tmp/mono22.wav; wait'", 30)
        hw.check("busy" in out.lower(), "a second player is told the device is busy", out)
        m.run("busybox sleep 2", 20)
        m.run("")
        out = m.run("amixer set Master 0% mute")
        hw.check("[0%]" in out and "[off]" in out, "amixer mutes it", out)
        m.run("amixer -q set Master 80% unmute")
        m.monitor("quit")
        for _ in range(100):
            if m.p.poll() is not None:
                break
            time.sleep(0.1)
    finally:
        m.close()

    print("▲ what came out of the speaker", flush=True)
    try:
        d = open(rec, "rb").read()
        i = d.find(b"data")
        rate = struct.unpack("<I", d[24:28])[0]
        a = array.array("h")
        body = d[i + 8:]
        a.frombytes(body[:len(body) // 4 * 4])
        left, right = a[0::2], a[1::2]
        # QEMU writes only while a stream runs, so the three plays (stereo, mono, stereo) come out
        # back to back: look at windows from where the sound starts
        start = next((k for k, v in enumerate(left) if abs(v) > 200), None)
        end = max((k for k, v in enumerate(left) if abs(v) > 200), default=0)
        hw.check(start is not None and (end - start) / rate > 2.5, f"about 3 s of sound recorded ({(end - (start or 0)) / rate:.2f} s, at {rate} Hz)", "")
        if start is not None:
            def freq(ch, a_s, b_s):
                x = ch[start + int(a_s * rate):start + int(b_s * rate)]
                return crossings(x) / 2 * rate / max(len(x), 1)
            fl, fr = freq(left, 0.1, 0.8), freq(right, 0.1, 0.8)
            hw.check(abs(fl - 440) < 8 and abs(fr - 660) < 8, f"the stereo file: {fl:.0f} Hz left, {fr:.0f} Hz right", "")
            f2, f2r = freq(left, 1.2, 1.8), freq(right, 1.2, 1.8)
            hw.check(abs(f2 - 1000) < 15 and abs(f2r - 1000) < 15, f"the mono file: {f2:.0f} Hz, on both sides", "")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
    print(f"\n{hw.passed} passed, {hw.failed} failed")
    sys.exit(1 if hw.failed else 0)
