"""
Table V — Hybrid quantization ablation.

For the chosen anger/speaker pair (MicroLightCNN + MLP-XiEmbedding), measure
the four combinations of (anger precision) x (speaker precision):

      (F32, F32) | (F32, INT8) <-- chosen | (INT8, F32) | (INT8, INT8)

For each cell we report:
    anger accuracy, speaker top-1 accuracy (under joint threshold),
    total arena = anger arena + speaker arena (KB),
    end-to-end host-side latency (anger + speaker inference, ms).

The script reuses the TFLite files produced by `run_anger.py` and
`run_speaker.py` when they are available, and re-trains the corresponding
network otherwise (idempotent).

Usage
-----
    python -m experiments.run_quant_ablation \
        --esd-root  /path/to/ESD/Emotional_Speech_Dataset \
        --dataset-dir ../dataset_fam \
        --anger-dir   outputs/anger \
        --speaker-dir outputs/speaker \
        --output-dir  outputs/quant
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path

os.environ.setdefault("TF_CPP_MIN_LOG_LEVEL", "2")

import numpy as np
import tensorflow as tf
from sklearn.preprocessing import StandardScaler
from sklearn.utils.class_weight import compute_class_weight

from models import build_micro_lightcnn, build_mlp_xi_embedding
from utils import (
    load_esd_all,
    collect_speaker_segments, split_records_by_source, augment_records,
    records_to_xivector,
    evaluate_anger, evaluate_speaker,
    to_tflite_float32, to_tflite_int8,
    measure_arena_kb, latency_host_ms, predict_tflite,
)


def _set_seed(seed: int) -> None:
    import random
    random.seed(seed); np.random.seed(seed); tf.random.set_seed(seed)


def _add_channel(X: np.ndarray) -> np.ndarray:
    return X.reshape(X.shape[0], X.shape[1], X.shape[2], 1).astype(np.float32)


# ---------------------------------------------------------------------------
# Anger - get or train MicroLightCNN
# ---------------------------------------------------------------------------
def _prepare_anger(args, anger_dir: Path):
    """Returns dict with tflite bytes (f32, int8) and the test set."""
    tfl_f32 = anger_dir / "microlightcnn_f32.tflite"
    tfl_int8 = anger_dir / "microlightcnn_int8.tflite"

    dataset = load_esd_all(args.esd_root, max_per_class=args.max_per_class,
                           seed=args.seed)
    X_te = _add_channel(dataset["test"][0])
    y_te = dataset["test"][1]

    if tfl_f32.exists() and tfl_int8.exists():
        print(f"[anger] Reusing {tfl_f32.name} and {tfl_int8.name}")
        return {
            "f32_bytes":  tfl_f32.read_bytes(),
            "int8_bytes": tfl_int8.read_bytes(),
            "X_test":     X_te,
            "y_test":     y_te,
        }

    print("[anger] No cached TFLite found; training MicroLightCNN from scratch.")
    X_tr = _add_channel(dataset["train"][0]); y_tr = dataset["train"][1]
    X_va = _add_channel(dataset["evaluation"][0]); y_va = dataset["evaluation"][1]

    model = build_micro_lightcnn()
    cw = compute_class_weight("balanced", classes=np.unique(y_tr), y=y_tr)
    model.fit(
        X_tr, y_tr.astype(np.float32),
        validation_data=(X_va, y_va.astype(np.float32)),
        epochs=args.epochs, batch_size=args.batch_size,
        class_weight=dict(enumerate(cw)),
        callbacks=[
            tf.keras.callbacks.EarlyStopping(monitor="val_accuracy",
                                             patience=15, restore_best_weights=True),
        ],
        verbose=2,
    )
    f32 = to_tflite_float32(model)
    int8 = to_tflite_int8(model, X_tr[:200])
    anger_dir.mkdir(parents=True, exist_ok=True)
    tfl_f32.write_bytes(f32); tfl_int8.write_bytes(int8)
    return {"f32_bytes": f32, "int8_bytes": int8, "X_test": X_te, "y_test": y_te}


# ---------------------------------------------------------------------------
# Speaker - get or train MLP-XiEmbedding
# ---------------------------------------------------------------------------
def _prepare_speaker(args, speaker_dir: Path):
    tfl_f32 = speaker_dir / "mlp_xiembedding_f32.tflite"
    tfl_int8 = speaker_dir / "mlp_xiembedding_int8.tflite"

    records, labels = collect_speaker_segments(args.dataset_dir)
    train_clean, val_records = split_records_by_source(
        records, val_ratio=args.val_ratio, seed=args.seed)
    train_records = augment_records(train_clean, copies=args.augment_copies,
                                    mode="mixed", seed=args.seed)

    X_tr_raw, y_tr = records_to_xivector(train_records)
    X_va_raw, y_va = records_to_xivector(val_records)
    scaler = StandardScaler()
    X_tr = scaler.fit_transform(X_tr_raw).astype(np.float32)
    X_va = scaler.transform(X_va_raw).astype(np.float32)

    if tfl_f32.exists() and tfl_int8.exists():
        print(f"[speaker] Reusing {tfl_f32.name} and {tfl_int8.name}")
        return {
            "f32_bytes":  tfl_f32.read_bytes(),
            "int8_bytes": tfl_int8.read_bytes(),
            "X_val":      X_va,
            "y_val":      y_va,
            "labels":     labels,
        }

    print("[speaker] No cached TFLite found; training MLP-XiEmbedding from scratch.")
    model = build_mlp_xi_embedding(num_classes=len(labels))
    cw = compute_class_weight("balanced", classes=np.unique(y_tr), y=y_tr)
    model.fit(X_tr, y_tr, validation_data=(X_va, y_va),
              epochs=args.epochs, batch_size=args.batch_size,
              class_weight=dict(enumerate(cw)),
              callbacks=[tf.keras.callbacks.EarlyStopping(
                  monitor="val_accuracy", patience=20, restore_best_weights=True)],
              verbose=2)
    f32 = to_tflite_float32(model)
    int8 = to_tflite_int8(model, X_tr[:200])
    speaker_dir.mkdir(parents=True, exist_ok=True)
    tfl_f32.write_bytes(f32); tfl_int8.write_bytes(int8)
    return {"f32_bytes": f32, "int8_bytes": int8, "X_val": X_va,
            "y_val": y_va, "labels": labels}


# ---------------------------------------------------------------------------
# Cell evaluation
# ---------------------------------------------------------------------------
def _eval_cell(anger_bytes: bytes, speaker_bytes: bytes,
               anger_data: dict, speaker_data: dict,
               tau_sp: float, delta: float) -> dict:
    # Anger.
    y_score_a = predict_tflite(anger_bytes, anger_data["X_test"]).reshape(-1)
    m_a = evaluate_anger(anger_data["y_test"], y_score_a)
    lat_a = latency_host_ms(anger_bytes, anger_data["X_test"][0])
    arena_a = measure_arena_kb(anger_bytes)

    # Speaker.
    proba_s = predict_tflite(speaker_bytes, speaker_data["X_val"])
    m_s = evaluate_speaker(speaker_data["y_val"], proba_s,
                           tau_sp=tau_sp, delta=delta)
    lat_s = latency_host_ms(speaker_bytes, speaker_data["X_val"][0])
    arena_s = measure_arena_kb(speaker_bytes)

    return {
        "anger_accuracy":   m_a.accuracy,
        "anger_f1":         m_a.f1,
        "speaker_top1":     m_s.top1_accepted,
        "speaker_eer":      m_s.eer_macro,
        "speaker_rej":      m_s.rejection_rate,
        "anger_arena_kb":   arena_a,
        "speaker_arena_kb": arena_s,
        "total_arena_kb":   round(arena_a + arena_s, 1),
        "anger_lat_ms":     lat_a,
        "speaker_lat_ms":   lat_s,
        "total_lat_ms":     round(lat_a + lat_s, 1),
    }


# ---------------------------------------------------------------------------
# LaTeX exporter
# ---------------------------------------------------------------------------
def export_table_v(cells: dict[tuple[str, str], dict], path: Path) -> None:
    order = [("Float32", "Float32"), ("Float32", "INT8"),
             ("INT8",    "Float32"), ("INT8",    "INT8")]
    lines = [
        r"\begin{table}[t]", r"\centering",
        r"\caption{Hybrid quantization ablation. The chosen configuration "
        r"(Float32 anger / INT8 speaker) is highlighted.}",
        r"\label{tab:quant_ablation}",
        r"\begin{tabular}{llcccc}",
        r"\toprule",
        r"Anger prec.\ & Speaker prec.\ & Anger Acc.\ & Spk.\ Acc.\ & Total Arena & Total Lat. \\",
        r"\midrule",
    ]
    for ang_p, spk_p in order:
        c = cells[(ang_p, spk_p)]
        chosen = (ang_p, spk_p) == ("Float32", "INT8")
        prefix = r"\textbf{" if chosen else ""
        suffix = r"}" if chosen else ""
        tail = r" (chosen)" if chosen else ""
        lines.append(
            f"{prefix}{ang_p}{suffix} & {prefix}{spk_p}{tail}{suffix} & "
            f"{prefix}{c['anger_accuracy']*100:.1f}{suffix} & "
            f"{prefix}{c['speaker_top1']*100:.1f}{suffix} & "
            f"{prefix}{c['total_arena_kb']:.0f} KB{suffix} & "
            f"{prefix}{c['total_lat_ms']:.1f} ms{suffix} \\\\"
        )
    lines += [r"\bottomrule", r"\end{tabular}", r"\end{table}", ""]
    path.write_text("\n".join(lines), encoding="utf-8")
    print(f"  -> wrote LaTeX table to {path}")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
def main():
    p = argparse.ArgumentParser()
    p.add_argument("--esd-root", required=True)
    p.add_argument("--dataset-dir", default="../dataset_fam")
    p.add_argument("--anger-dir",   default="outputs/anger")
    p.add_argument("--speaker-dir", default="outputs/speaker")
    p.add_argument("--output-dir",  default="outputs/quant")
    p.add_argument("--epochs", type=int, default=60)
    p.add_argument("--batch-size", type=int, default=32)
    p.add_argument("--seed", type=int, default=42)
    p.add_argument("--max-per-class", type=int, default=None)
    p.add_argument("--augment-copies", type=int, default=2)
    p.add_argument("--val-ratio", type=float, default=0.2)
    p.add_argument("--tau-sp", type=float, default=0.60)
    p.add_argument("--delta",  type=float, default=0.20)
    args = p.parse_args()

    _set_seed(args.seed)

    anger    = _prepare_anger(args,   Path(args.anger_dir))
    speaker  = _prepare_speaker(args, Path(args.speaker_dir))

    cells: dict[tuple[str, str], dict] = {}
    combos = [
        (("Float32", "Float32"), anger["f32_bytes"],  speaker["f32_bytes"]),
        (("Float32", "INT8"),    anger["f32_bytes"],  speaker["int8_bytes"]),
        (("INT8",    "Float32"), anger["int8_bytes"], speaker["f32_bytes"]),
        (("INT8",    "INT8"),    anger["int8_bytes"], speaker["int8_bytes"]),
    ]
    print("\n" + "=" * 70)
    print("  Quantization ablation (Table V)")
    print("=" * 70)
    for (ang_p, spk_p), ab, sb in combos:
        print(f"\n[{ang_p:>7s}  /  {spk_p:>7s}]")
        c = _eval_cell(ab, sb, anger, speaker, args.tau_sp, args.delta)
        cells[(ang_p, spk_p)] = c
        print(f"  anger_acc = {c['anger_accuracy']*100:5.2f} %   "
              f"speaker_top1 = {c['speaker_top1']*100:5.2f} %   "
              f"total_arena = {c['total_arena_kb']:6.1f} KB   "
              f"total_lat = {c['total_lat_ms']:6.1f} ms")

    out_dir = Path(args.output_dir); out_dir.mkdir(parents=True, exist_ok=True)
    serializable = {f"{k[0]}_{k[1]}": v for k, v in cells.items()}
    (out_dir / "table_v.json").write_text(json.dumps(serializable, indent=2))
    export_table_v(cells, out_dir / "table_v.tex")
    print(f"\nDone. Outputs in {out_dir}")


if __name__ == "__main__":
    main()
