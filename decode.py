# -*- coding: utf-8 -*-
"""
Omi Glass 音频解码脚本

把蓝牙收到的 Opus 音频包解码成 WAV。

支持两种输入:
  1. nRF Connect 导出的宏文件(.xml): 自动提取 <assert-value value="hex"> 里的音频数据
  2. 纯文本文件: 每行一个包的 hex

用法:
    py decode.py <输入文件> [-o <输出wav>]

示例:
    py decode.py test4.xml               → 生成 test4.wav
    py decode.py test4.xml -o 我的录音.wav → 生成指定文件
    py decode.py packets.txt             → 生成 packets.wav
"""
import ctypes
import os
import re
import sys
import wave

# 让中文输出在 Windows 终端不乱码
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")


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

libopus.opus_decoder_destroy.argtypes = [ctypes.c_void_p]
libopus.opus_decoder_destroy.restype = None

FS = 16000            # 采样率
CHANNELS = 1          # 单声道
FRAME_SAMPLES = 320   # 20ms 帧


def parse_args(argv):
    """解析命令行参数，返回 (输入文件, 输出文件或 None)"""
    infile = None
    outfile = None
    i = 0
    while i < len(argv):
        a = argv[i]
        if a in ("-o", "--output"):
            outfile = argv[i + 1]
            i += 2
        elif a.startswith("-o") and len(a) > 2:
            outfile = a[2:]
            i += 1
        elif not a.startswith("-"):
            infile = a
            i += 1
        else:
            i += 1
    return infile, outfile


DEFAULT_FOLDER = "audio_test_folder"


def resolve_path(p):
    """不带路径的文件名 → 自动放到 audio_test_folder 里；带路径则原样返回。"""
    if p and os.path.dirname(p) == "":
        return os.path.join(DEFAULT_FOLDER, p)
    return p


def extract_packets(infile):
    """从文件提取 hex 包列表。
    支持 nRF 宏(.xml) 和纯文本(每行一个 hex) 两种格式。"""
    with open(infile, encoding="utf-8", errors="replace") as f:
        content = f.read()

    packets = []
    if infile.lower().endswith(".xml") or "<assert-value" in content:
        # nRF 宏文件：提取所有 value="hex"
        for m in re.findall(r'value="([0-9A-Fa-f]+)"', content):
            if len(m) >= 6:  # 至少 3 字节头 + 1 字节数据
                packets.append(m)
    else:
        # 纯文本：每行一个 hex
        for line in content.splitlines():
            line = line.strip()
            if line and not line.startswith("#"):
                packets.append(line)
    return packets


def clean_hex(s):
    """去掉空格/逗号/横线，返回纯 hex 字符串"""
    return s.replace(" ", "").replace(",", "").replace("-", "")


def decode_packets(packets):
    """把 hex 包列表解码成 PCM 字节（16kHz 单声道 int16）。
    返回 (pcm_bytes, 解码帧数, 坏包数)。
    每次调用创建独立 decoder，用完销毁，避免跨文件状态污染。"""
    err = ctypes.c_int(0)
    dec = libopus.opus_decoder_create(FS, CHANNELS, ctypes.byref(err))
    if err.value != 0 or not dec:
        raise RuntimeError(f"创建解码器失败,err={err.value}")

    try:
        pcm_all = bytearray()
        count = 0
        bad = 0
        for i, pkt in enumerate(packets):
            raw = bytes.fromhex(clean_hex(pkt))
            if len(raw) < 4:
                print(f"跳过过短包(第{i + 1}包, {len(raw)}字节): {pkt[:20]}")
                bad += 1
                continue
            opus_data = raw[3:]  # 去掉 3 字节头(2字节序号 + 1字节子索引)
            buf = (ctypes.c_ubyte * len(opus_data)).from_buffer_copy(opus_data)
            pcm = (ctypes.c_int16 * (FRAME_SAMPLES * CHANNELS))()
            n = libopus.opus_decode(dec, buf, len(opus_data),
                                    pcm, FRAME_SAMPLES, 0)
            if n < 0:
                print(f"跳过坏包(第{i + 1}包, 错误 {n}): {pkt[:40]}")
                bad += 1
                continue
            pcm_all += ctypes.string_at(pcm, n * CHANNELS * 2)
            count += 1
        return bytes(pcm_all), count, bad
    finally:
        libopus.opus_decoder_destroy(dec)


def main():
    infile, outfile = parse_args(sys.argv[1:])
    if not infile:
        print("用法: py decode.py <输入文件> [-o <输出wav>]")
        print("示例: py decode.py test5.xml   (自动在 audio_test_folder 里找)")
        sys.exit(1)

    infile = resolve_path(infile)
    if not os.path.exists(infile):
        print(f"找不到文件 {infile}")
        sys.exit(1)

    if not outfile:
        outfile = os.path.splitext(infile)[0] + ".wav"
    else:
        outfile = resolve_path(outfile)

    packets = extract_packets(infile)
    print(f"共提取 {len(packets)} 个包")

    pcm_all, count, bad = decode_packets(packets)

    with wave.open(outfile, "wb") as w:
        w.setnchannels(CHANNELS)
        w.setsampwidth(2)          # 16bit
        w.setframerate(FS)         # 16kHz
        w.writeframes(bytes(pcm_all))

    dur = len(pcm_all) / (FS * 2)
    print(f"解码完成: {count} 帧, 跳过 {bad} 个坏包, 时长 {dur:.2f} 秒")
    print(f"已保存: {outfile}")


if __name__ == "__main__":
    main()
