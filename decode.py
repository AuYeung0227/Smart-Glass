# -*- coding: utf-8 -*-
"""
Omi Glass 音频解码脚本
把蓝牙收到的 Opus 音频包(每行一个 hex)解码成 WAV。

用法:
    py decode.py [packets.txt]

packets.txt 格式:每行一个包的 hex,例如:
    C6 1B 00 48 07 C9 79 C5 12 F7 BC
"""
import ctypes
import os
import sys
import wave


# ---------- 1. 定位 opus.dll ----------
def find_opus_dll():
    candidates = [
        r"C:/Users/86133/AppData/Local/Programs/Python/Python314/opus.dll",
        r"C:/Users/86133/AppData/Local/Programs/Python/Python314/Lib/site-packages/pyogg/opus.dll",
        os.path.join(os.path.dirname(os.path.abspath(__file__)), "opus.dll"),
        "opus.dll",
    ]
    for c in candidates:
        if os.path.exists(c):
            return c
    raise FileNotFoundError("找不到 opus.dll,请确认它已复制到 Python 目录")


# ---------- 2. 加载 libopus ----------
libopus = ctypes.CDLL(find_opus_dll())

libopus.opus_decoder_create.argtypes = [
    ctypes.c_int, ctypes.c_int, ctypes.POINTER(ctypes.c_int)
]
libopus.opus_decoder_create.restype = ctypes.c_void_p

libopus.opus_decode.argtypes = [
    ctypes.c_void_p, ctypes.POINTER(ctypes.c_ubyte), ctypes.c_int32,
    ctypes.POINTER(ctypes.c_int16), ctypes.c_int, ctypes.c_int
]
libopus.opus_decode.restype = ctypes.c_int

FS = 16000            # 采样率
CHANNELS = 1          # 单声道
FRAME_SAMPLES = 320   # 20ms 帧

err = ctypes.c_int(0)
decoder = libopus.opus_decoder_create(FS, CHANNELS, ctypes.byref(err))
if err.value != 0 or not decoder:
    raise RuntimeError(f"创建解码器失败,err={err.value}")


def main():
    txt = sys.argv[1] if len(sys.argv) > 1 else "packets.txt"
    if not os.path.exists(txt):
        print(f"找不到文件 {txt},请先准备数据文件")
        sys.exit(1)

    pcm_all = bytearray()
    count = 0
    with open(txt, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            raw = bytes.fromhex(line.replace("-", " ").replace(",", " "))
            opus_data = raw[3:]  # 去掉 3 字节头(2字节序号 + 1字节子索引)
            if not opus_data:
                continue

            buf = (ctypes.c_ubyte * len(opus_data)).from_buffer_copy(opus_data)
            pcm = (ctypes.c_int16 * (FRAME_SAMPLES * CHANNELS))()
            n = libopus.opus_decode(decoder, buf, len(opus_data),
                                    pcm, FRAME_SAMPLES, 0)
            if n < 0:
                print(f"跳过坏包(错误 {n}): {line[:40]}...")
                continue
            pcm_all += ctypes.string_at(pcm, n * CHANNELS * 2)
            count += 1

    out_wav = "out.wav"
    with wave.open(out_wav, "wb") as w:
        w.setnchannels(CHANNELS)
        w.setsampwidth(2)          # 16bit
        w.setframerate(FS)         # 16kHz
        w.writeframes(bytes(pcm_all))

    dur = len(pcm_all) / (FS * 2)
    print(f"解码完成: {count} 帧, 时长 {dur:.2f} 秒, 已保存 {out_wav}")


if __name__ == "__main__":
    main()
