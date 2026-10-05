# -*- coding: utf-8 -*-
"""
音频波形图工具

把 wav 或 nRF 导出的 xml 音频，画成「原始振幅波形 + 音量包络」双子图，存 PNG。

用法:
    py plot_waveform.py <音频文件> [-o <输出png>]

示例:
    py plot_waveform.py test5.xml               → 生成 audio_test_folder/test5.png
    py plot_waveform.py test5.wav -o 波形.png    → 指定输出
"""
import os
import sys
import wave

import numpy as np
import matplotlib
matplotlib.use("Agg")  # 无界面，直接存文件
import matplotlib.pyplot as plt

import decode  # 复用 decode.py 的 xml 解码和路径逻辑


# 中文字体（Windows 下用微软雅黑，避免中文变方框）
plt.rcParams["font.sans-serif"] = ["Microsoft YaHei", "SimHei", "DejaVu Sans"]
plt.rcParams["axes.unicode_minus"] = False


def parse_args(argv):
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


def load_wav(infile):
    """读 wav，返回 (pcm int16 ndarray, 采样率)"""
    with wave.open(infile, "rb") as w:
        sr = w.getframerate()
        nch = w.getnchannels()
        sw = w.getsampwidth()
        frames = w.readframes(w.getnframes())

    if sw == 2:
        data = np.frombuffer(frames, dtype=np.int16)
    elif sw == 1:
        # 8bit 无符号 PCM（0~255），转成有符号 16bit
        data = (np.frombuffer(frames, dtype=np.uint8).astype(np.int16) - 128) * 256
    else:
        data = np.frombuffer(frames, dtype=np.int16)

    if nch > 1:
        data = data.reshape(-1, nch).mean(axis=1).astype(np.int16)
    return data, sr


def load_pcm(infile):
    """读 wav 或 xml（/txt），返回 (pcm int16 ndarray, 采样率)"""
    ext = os.path.splitext(infile)[1].lower()
    if ext == ".wav":
        return load_wav(infile)

    # xml 或 txt：走 decode 的 Opus 解码
    packets = decode.extract_packets(infile)
    pcm_bytes, count, bad = decode.decode_packets(packets)
    print(f"解码: {count} 帧, {bad} 个坏包")
    return np.frombuffer(pcm_bytes, dtype=np.int16), decode.FS


def downsample_peaks(pcm, max_points=200000):
    """peak/valley 降采样：长音频按块取 min/max 交替画，保留波形峰值形状。
    返回 (x 样本索引, y 振幅)。"""
    n = len(pcm)
    if n <= max_points:
        return np.arange(n), pcm

    half = max_points // 2
    chunk = int(np.ceil(n / half))
    n_trunc = (n // chunk) * chunk
    arr = pcm[:n_trunc].reshape(-1, chunk)

    mn = arr.min(axis=1)
    mx = arr.max(axis=1)
    imn = arr.argmin(axis=1)
    imx = arr.argmax(axis=1)
    base = np.arange(arr.shape[0]) * chunk

    x = np.empty(arr.shape[0] * 2)
    y = np.empty(arr.shape[0] * 2)
    mask = imn <= imx  # 判断 min 在前还是 max 在前，保持时间顺序
    x[0::2] = base + np.where(mask, imn, imx)
    y[0::2] = np.where(mask, mn, mx)
    x[1::2] = base + np.where(mask, imx, imn)
    y[1::2] = np.where(mask, mx, mn)
    return x, y


def rms_envelope(pcm, sr, frame_ms=20):
    """按帧算 RMS 音量包络，返回 (时间秒, rms)。"""
    frame = int(sr * frame_ms / 1000)
    n_frames = len(pcm) // frame
    arr = pcm[:n_frames * frame].reshape(n_frames, frame).astype(np.float64)
    rms = np.sqrt(np.mean(arr ** 2, axis=1))
    t = (np.arange(n_frames) * frame + frame / 2) / sr
    return t, rms


def main():
    infile, outfile = parse_args(sys.argv[1:])
    if not infile:
        print("用法: py plot_waveform.py <音频文件> [-o <输出png>]")
        print("示例: py plot_waveform.py test5.xml")
        sys.exit(1)

    infile = decode.resolve_path(infile)
    if not os.path.exists(infile):
        print(f"找不到文件 {infile}")
        sys.exit(1)

    if not outfile:
        outfile = os.path.splitext(infile)[0] + ".png"
    else:
        outfile = decode.resolve_path(outfile)

    pcm, sr = load_pcm(infile)
    dur = len(pcm) / sr
    print(f"音频: {len(pcm)} 样本, {sr}Hz, 时长 {dur:.2f} 秒")

    # 原始波形（降采样）
    xw, yw = downsample_peaks(pcm)
    tw = xw / sr

    # 音量包络
    tr, rms = rms_envelope(pcm, sr)

    fig, (ax1, ax2) = plt.subplots(
        2, 1, figsize=(12, 6.5), sharex=True,
        gridspec_kw={"height_ratios": [2, 1]}
    )
    fig.suptitle(f"{os.path.basename(infile)}  ({dur:.2f}s @ {sr}Hz)", fontsize=12)

    ax1.plot(tw, yw, color="#1f77b4", lw=0.6)
    ax1.set_ylabel("振幅 (Amplitude)")
    ax1.set_title("原始振幅波形", fontsize=11)
    ax1.grid(True, alpha=0.3)
    ax1.margins(x=0)

    ax2.plot(tr, rms, color="#d62728", lw=0.8)
    ax2.fill_between(tr, rms, color="#d62728", alpha=0.3)
    ax2.set_ylabel("音量 RMS")
    ax2.set_xlabel("时间 (秒)")
    ax2.set_title("音量包络（20ms 帧 RMS）", fontsize=11)
    ax2.grid(True, alpha=0.3)
    ax2.margins(x=0)

    fig.tight_layout(rect=[0, 0, 1, 0.96])
    fig.savefig(outfile, dpi=120, bbox_inches="tight")
    plt.close(fig)
    print(f"已保存: {outfile}")


if __name__ == "__main__":
    main()
