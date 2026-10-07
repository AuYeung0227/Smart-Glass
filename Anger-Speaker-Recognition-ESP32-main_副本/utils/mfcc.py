"""
Shared MFCC front-end for all six models.

The configuration matches the firmware exactly:
    sampling rate    : 16 kHz
    window length    : 512 samples  (32 ms @ 16 kHz)
    hop length       : 256 samples  (16 ms @ 16 kHz)
    Mel bands        : 20
    output shape     : (20, 63)  -- 2-second window
    Xi-Vector pooling: [mu_k, sigma_k, max_k, min_k]_{k=1..20}  -> 80-d
"""
from __future__ import annotations

from pathlib import Path

import numpy as np
import soundfile as sf
from scipy.fftpack import dct
from scipy.signal import resample

# ---------------------------------------------------------------------------
# Constants (must match firmware MFCC.cpp / MFCC.h)
# ---------------------------------------------------------------------------
SR              = 16000
SEGMENT_SECONDS = 2
SEGMENT_SAMPLES = SR * SEGMENT_SECONDS
STEP_SAMPLES    = SEGMENT_SAMPLES // 4          # 0.5 s hop for sliding window
ENERGY_THRESHOLD = 0.0025                       # skip silent segments

MEL_BANDS  = 20
SIZE_WIN   = 512
SIZE_OFF   = 256
FREC_MIN   = 20
FREC_MAX   = SR // 2
NUM_FRAMES = 63                                 # canonical output frame count

XI_DIM = 4 * MEL_BANDS                          # = 80


# ---------------------------------------------------------------------------
# Audio I/O
# ---------------------------------------------------------------------------
def load_audio(path: str | Path, sr: int = SR) -> np.ndarray:
    """Load a WAV, force mono, resample to `sr` if needed."""
    audio, file_sr = sf.read(str(path), dtype="float32")
    if audio.ndim > 1:
        audio = audio.mean(axis=1)
    if file_sr != sr:
        audio = resample(audio, int(len(audio) * sr / file_sr)).astype(np.float32)
    return audio.astype(np.float32)


def normalize_peak(audio: np.ndarray, peak: float = 0.99) -> np.ndarray:
    max_abs = np.max(np.abs(audio)) + 1e-9
    if max_abs > peak:
        audio = audio * (peak / max_abs)
    return np.clip(audio, -1.0, 1.0).astype(np.float32)


# ---------------------------------------------------------------------------
# MFCC
# ---------------------------------------------------------------------------
def _mel_filterbank() -> np.ndarray:
    """Pre-compute the triangular Mel filterbank, identical to MFCC.cpp."""
    mel_min = 2595 * np.log10(1 + FREC_MIN / 700)
    mel_max = 2595 * np.log10(1 + FREC_MAX / 700)
    mel_points = np.linspace(mel_min, mel_max, MEL_BANDS + 2)
    hz = 700 * (10 ** (mel_points / 2595) - 1)
    bins = np.floor(((SIZE_WIN / 2) + 1) * hz / FREC_MAX).astype(int)

    fbank = np.zeros((MEL_BANDS, SIZE_WIN // 2 + 1), dtype=np.float32)
    for m in range(1, MEL_BANDS + 1):
        f1, f2, f3 = bins[m - 1], bins[m], bins[m + 1]
        for k in range(f1, f2):
            fbank[m - 1, k] = (k - f1) / (f2 - f1 + 1e-9)
        for k in range(f2, f3):
            fbank[m - 1, k] = (f3 - k) / (f3 - f2 + 1e-9)
    return fbank


_FBANK = _mel_filterbank()
_HAMMING = (
    0.53836
    - 0.46164 * np.cos((2.0 * np.pi * np.arange(SIZE_WIN)) / (SIZE_WIN - 1))
).astype(np.float32)


def compute_mfcc(audio: np.ndarray) -> np.ndarray:
    """
    Compute the (MEL_BANDS, NUM_FRAMES) MFCC matrix from a `SEGMENT_SAMPLES`
    audio buffer. Output is centre-cropped/zero-padded to NUM_FRAMES.
    """
    audio = audio.astype(np.float32).copy()

    # Pre-emphasis (in-place to mirror the firmware code path).
    for i in range(len(audio) - 1, 0, -1):
        audio[i] = audio[i] - audio[i - 1] * 0.97

    # Framing.
    n_frames = max(0, (len(audio) - SIZE_WIN) // SIZE_OFF + 1)
    frames = np.stack(
        [audio[i * SIZE_OFF : i * SIZE_OFF + SIZE_WIN] for i in range(n_frames)],
        axis=0,
    ) * _HAMMING

    # Power spectrum.
    spec = np.abs(np.fft.rfft(frames, SIZE_WIN)) ** 2

    # Mel + log + DCT.
    mel = spec @ _FBANK.T
    log_mel = np.log(mel + 1e-6)
    mfcc = dct(log_mel, type=2, axis=1, norm="ortho").T.astype(np.float32)

    # Centre-crop / pad to NUM_FRAMES.
    if mfcc.shape[1] >= NUM_FRAMES:
        start = (mfcc.shape[1] - NUM_FRAMES) // 2
        mfcc = mfcc[:, start : start + NUM_FRAMES]
    else:
        pad = NUM_FRAMES - mfcc.shape[1]
        mfcc = np.pad(mfcc, ((0, 0), (pad // 2, pad - pad // 2)), mode="constant")

    return mfcc.astype(np.float32)


# ---------------------------------------------------------------------------
# Xi-Vector pooling (deterministic, zero-parameter)
# ---------------------------------------------------------------------------
def xi_vector(mfcc: np.ndarray) -> np.ndarray:
    """
    Deterministic 4-statistic pooling per Mel band:
        xi = [mu_k, sigma_k, max_k, min_k]_{k=1..20}  -> shape (80,)
    """
    if mfcc.shape[0] != MEL_BANDS:
        raise ValueError(
            f"Expected ({MEL_BANDS}, T) MFCC matrix, got {mfcc.shape}"
        )
    mu  = mfcc.mean(axis=1)
    sd  = mfcc.std(axis=1)
    mx  = mfcc.max(axis=1)
    mn  = mfcc.min(axis=1)
    return np.stack([mu, sd, mx, mn], axis=1).reshape(-1).astype(np.float32)


# ---------------------------------------------------------------------------
# Segmenter
# ---------------------------------------------------------------------------
def segment_audio(audio: np.ndarray,
                  segment_samples: int = SEGMENT_SAMPLES,
                  hop: int = STEP_SAMPLES,
                  energy_threshold: float = ENERGY_THRESHOLD,
                  ) -> list[np.ndarray]:
    """Slice an audio file into overlapping segments, dropping silent ones."""
    segments = []
    for start in range(0, len(audio) - segment_samples + 1, hop):
        seg = audio[start : start + segment_samples].astype(np.float32)
        if float(np.mean(np.abs(seg))) >= energy_threshold:
            segments.append(seg)
    return segments
