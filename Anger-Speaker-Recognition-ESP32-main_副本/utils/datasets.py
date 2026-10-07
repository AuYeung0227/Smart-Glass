"""
Dataset loaders for anger and speaker tasks.

Anger
-----
Uses the Emotional Speech Dataset (ESD), keeping the binary mapping
documented in the project notebook `ModelMFCC_MicroLightCNN.ipynb`:
    Angry  -> 1   (positive class)
    other  -> 0   (Happy + Neutral + Sad + Surprise, both English and Chinese
                   sides of the corpus, balanced to match the anger count).

Speaker
-------
Uses `dataset_fam/` with one folder per enrolled household speaker.
The training pipeline replicates the augmentation strategy from
`Train_xvector_robusto/xvR.py` (loud / SNR / speed perturbations).
"""
from __future__ import annotations

import random
from collections import Counter, defaultdict
from pathlib import Path

import numpy as np

from .mfcc import (
    SEGMENT_SAMPLES, STEP_SAMPLES, ENERGY_THRESHOLD,
    NUM_FRAMES, MEL_BANDS,
    compute_mfcc, xi_vector, load_audio, normalize_peak,
)


# =========================================================================
# Anger dataset (ESD)
# =========================================================================
ANGRY = 1
OTHER = 0

ESD_EMOTION_DIRS = {
    "Angry":    ANGRY,
    "Happy":    OTHER,
    "Neutral":  OTHER,
    "Sad":      OTHER,
    "Surprise": OTHER,
}


def _list_esd_wavs(esd_root: Path, language: str, split: str) -> dict[str, list[Path]]:
    """
    Return {emotion: [paths]} for one language ('English' or 'Chinese')
    and one split ('train'/'test'/'evaluation').
    """
    out: dict[str, list[Path]] = {}
    for emotion in ESD_EMOTION_DIRS:
        d = esd_root / language / emotion / split
        if not d.is_dir():
            out[emotion] = []
            continue
        out[emotion] = sorted(d.glob("*.wav"))
    return out


def _crop_or_pad(audio: np.ndarray, target: int = SEGMENT_SAMPLES) -> np.ndarray:
    if len(audio) >= target:
        start = (len(audio) - target) // 2
        return audio[start : start + target].astype(np.float32)
    pad = target - len(audio)
    return np.pad(audio, (pad // 2, pad - pad // 2), mode="constant").astype(np.float32)


def load_esd_split(esd_root: str | Path, split: str,
                   languages: tuple[str, ...] = ("English", "Chinese"),
                   max_per_class: int | None = None,
                   seed: int = 42,
                   ) -> tuple[np.ndarray, np.ndarray]:
    """
    Load one ESD split as a class-balanced MFCC tensor.

    Returns
    -------
    X : (N, 20, 63) float32
    y : (N,)         int32 in {0, 1}
    """
    esd_root = Path(esd_root)
    rng = random.Random(seed)

    by_label: dict[int, list[Path]] = defaultdict(list)
    for lang in languages:
        for emotion, paths in _list_esd_wavs(esd_root, lang, split).items():
            by_label[ESD_EMOTION_DIRS[emotion]].extend(paths)

    # Balance: cap the OTHER count to the ANGRY count, exactly as the notebook.
    n_angry = len(by_label[ANGRY])
    if n_angry == 0:
        raise FileNotFoundError(
            f"No Angry samples under {esd_root} for split '{split}'. "
            f"Verify the dataset layout: <esd_root>/<Language>/<Emotion>/<split>/*.wav"
        )
    if max_per_class is not None:
        n_angry = min(n_angry, max_per_class)

    angry_paths = rng.sample(by_label[ANGRY], n_angry)
    other_paths = by_label[OTHER]
    rng.shuffle(other_paths)
    other_paths = other_paths[:n_angry]

    X_list: list[np.ndarray] = []
    y_list: list[int] = []
    for label, paths in ((ANGRY, angry_paths), (OTHER, other_paths)):
        for p in paths:
            audio = load_audio(p)
            audio = _crop_or_pad(audio, SEGMENT_SAMPLES)
            X_list.append(compute_mfcc(audio))
            y_list.append(label)

    X = np.stack(X_list, axis=0).astype(np.float32)        # (N, 20, 63)
    y = np.asarray(y_list, dtype=np.int32)
    return X, y


def load_esd_all(esd_root: str | Path,
                 max_per_class: int | None = None,
                 seed: int = 42,
                 ) -> dict[str, tuple[np.ndarray, np.ndarray]]:
    """Return {'train': (X,y), 'test': (X,y), 'evaluation': (X,y)}."""
    out = {}
    for split in ("train", "test", "evaluation"):
        out[split] = load_esd_split(esd_root, split, max_per_class=max_per_class, seed=seed)
        print(f"[ESD] {split}: X={out[split][0].shape}  y={Counter(out[split][1].tolist())}")
    return out


# =========================================================================
# Speaker dataset (dataset_fam/)
# =========================================================================
def collect_speaker_segments(dataset_dir: str | Path,
                             segment_samples: int = SEGMENT_SAMPLES,
                             hop: int = STEP_SAMPLES,
                             energy_threshold: float = ENERGY_THRESHOLD,
                             ) -> tuple[list[dict], list[str]]:
    """
    Walk the per-speaker folders and split each wav into overlapping segments.

    Returns
    -------
    records : list of {'audio', 'label', 'speaker', 'source', 'start'}
    labels  : list of speaker names (index == label id)
    """
    dataset_dir = Path(dataset_dir)
    speakers = sorted([p for p in dataset_dir.iterdir() if p.is_dir()])
    labels = [p.name for p in speakers]

    records: list[dict] = []
    for label_idx, sp_dir in enumerate(speakers):
        wav_files = sorted(sp_dir.glob("*.wav"))
        for wav in wav_files:
            audio = load_audio(wav)
            for start in range(0, len(audio) - segment_samples + 1, hop):
                seg = audio[start : start + segment_samples].astype(np.float32)
                if float(np.mean(np.abs(seg))) < energy_threshold:
                    continue
                records.append({
                    "audio":   seg,
                    "label":   label_idx,
                    "speaker": sp_dir.name,
                    "source":  wav.name,
                    "start":   start,
                    "aug":     "clean",
                })
    return records, labels


def split_records_by_source(records: list[dict],
                            val_ratio: float = 0.2,
                            seed: int = 42,
                            ) -> tuple[list[dict], list[dict]]:
    """
    Hold out *whole WAV files* per speaker for validation, exactly as
    `Train_xvector_robusto/xvR.py` does. Prevents leakage of phrase context.
    """
    rng = random.Random(seed)
    by_label_source: dict[int, dict[str, list[dict]]] = defaultdict(lambda: defaultdict(list))
    for rec in records:
        by_label_source[rec["label"]][rec["source"]].append(rec)

    train, val = [], []
    for label, sources in by_label_source.items():
        keys = list(sources.keys())
        rng.shuffle(keys)
        n_val = max(1, int(round(len(keys) * val_ratio)))
        val_keys = set(keys[:n_val])
        for src, items in sources.items():
            (val if src in val_keys else train).extend(items)
    return train, val


# --- Augmentation (mirrors xvR.py loud + noise mixed regime) -------------
def _rms(x: np.ndarray) -> float:
    return float(np.sqrt(np.mean(np.square(x)) + 1e-12))


def _dynamic_compress(audio: np.ndarray, threshold: float = 0.22, ratio: float = 3.5) -> np.ndarray:
    sign = np.sign(audio)
    mag = np.abs(audio).astype(np.float32)
    over = mag > threshold
    mag[over] = threshold + (mag[over] - threshold) / ratio
    return (sign * mag).astype(np.float32)


def augment_loud(audio: np.ndarray, hard: bool = True) -> np.ndarray:
    gain_db = random.uniform(9.0, 17.0) if hard else random.uniform(6.0, 13.0)
    y = audio * (10 ** (gain_db / 20.0))
    y = _dynamic_compress(
        y,
        threshold=random.uniform(0.15, 0.27) if hard else random.uniform(0.22, 0.34),
        ratio=random.uniform(3.5, 6.5) if hard else random.uniform(2.2, 4.0),
    )
    if random.random() < 0.85:
        drive = random.uniform(1.8, 3.4) if hard else random.uniform(1.15, 2.0)
        y = np.tanh(drive * y) / np.tanh(drive)
    return normalize_peak(y)


def augment_snr(audio: np.ndarray, snr_db: int | None = None) -> np.ndarray:
    if snr_db is None:
        snr_db = random.choice((30, 25, 20))
    noise = np.random.normal(0.0, 1.0, len(audio)).astype(np.float32)
    target = _rms(audio) / (10 ** (snr_db / 20.0))
    noise = noise * (target / (_rms(noise) + 1e-9))
    return normalize_peak(audio + noise)


def augment_records(records: list[dict], copies: int = 2,
                    mode: str = "mixed", seed: int = 42) -> list[dict]:
    """Add `copies` augmented variants per record."""
    rng = random.Random(seed)
    random.seed(seed)
    out = list(records)
    for rec in records:
        for c in range(copies):
            if mode == "loud":
                new_audio = augment_loud(rec["audio"], hard=(c % 2 == 0))
                tag = "loud_hard" if c % 2 == 0 else "loud_moderate"
            elif mode == "snr":
                new_audio = augment_snr(rec["audio"])
                tag = "snr"
            else:  # mixed (default)
                if c % 3 == 0:
                    new_audio = augment_snr(rec["audio"]); tag = "snr"
                elif c % 3 == 1:
                    new_audio = augment_loud(rec["audio"], hard=True); tag = "loud_hard"
                else:
                    new_audio = augment_loud(rec["audio"], hard=False); tag = "loud_moderate"
            new_rec = dict(rec)
            new_rec["audio"] = new_audio.astype(np.float32)
            new_rec["aug"] = tag
            out.append(new_rec)
    return out


def records_to_mfcc(records: list[dict]) -> tuple[np.ndarray, np.ndarray]:
    """Return (X, y) with X of shape (N, 20, 63)."""
    X = np.stack([compute_mfcc(r["audio"]) for r in records], axis=0).astype(np.float32)
    y = np.asarray([r["label"] for r in records], dtype=np.int32)
    return X, y


def records_to_xivector(records: list[dict]) -> tuple[np.ndarray, np.ndarray]:
    """Return (X, y) with X of shape (N, 80)."""
    X = np.stack([xi_vector(compute_mfcc(r["audio"])) for r in records], axis=0).astype(np.float32)
    y = np.asarray([r["label"] for r in records], dtype=np.int32)
    return X, y
