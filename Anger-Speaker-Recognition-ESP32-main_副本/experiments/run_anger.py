"""
Table III — Anger detection results on the ESD test split.

For each model, fits the network, evaluates on the held-out test set, and
records:

    accuracy, F1, parameter count, TFLite arena (Float32 and INT8),
    flash footprint, host-side latency.

The script writes:
    outputs/anger/<model>.tflite              (float32)
    outputs/anger/<model>_int8.tflite         (int8 post-training quant.)
    outputs/anger/<model>.json                (metrics + arenas)
    outputs/anger/table_iii.json              (aggregated)
    outputs/anger/table_iii.tex               (LaTeX-ready Table III)

Usage
-----
    python -m experiments.run_anger \
        --esd-root /path/to/ESD/Emotional_Speech_Dataset \
        --output-dir outputs/anger \
        --epochs 60 --batch-size 32 --seed 42
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path

# Quiet TF before importing it.
os.environ.setdefault("TF_CPP_MIN_LOG_LEVEL", "2")

import numpy as np
import tensorflow as tf
from sklearn.utils.class_weight import compute_class_weight

from models import (
    build_micro_lightcnn, build_ds_cnn, build_matchboxnet,
    transpose_for_matchboxnet,
)
from utils import (
    load_esd_all,
    evaluate_anger,
    to_tflite_float32, to_tflite_int8,
    measure_arena_kb, measure_flash_kb, latency_host_ms,
    predict_tflite,
)


# ---------------------------------------------------------------------------
# Trainer
# ---------------------------------------------------------------------------
def _set_seed(seed: int) -> None:
    import random
    random.seed(seed)
    np.random.seed(seed)
    tf.random.set_seed(seed)


def _add_channel(X: np.ndarray) -> np.ndarray:
    """Add the trailing channel dim expected by 2-D conv models."""
    return X.reshape(X.shape[0], X.shape[1], X.shape[2], 1).astype(np.float32)


def _fit_anger(model: tf.keras.Model,
               X_train: np.ndarray, y_train: np.ndarray,
               X_val:   np.ndarray, y_val:   np.ndarray,
               epochs: int, batch_size: int) -> tf.keras.callbacks.History:
    cw = compute_class_weight("balanced", classes=np.unique(y_train), y=y_train)
    class_weight = dict(enumerate(cw))
    cbs = [
        tf.keras.callbacks.EarlyStopping(monitor="val_accuracy", patience=15,
                                         restore_best_weights=True, verbose=0),
        tf.keras.callbacks.ReduceLROnPlateau(monitor="val_loss", factor=0.5,
                                             patience=6, min_lr=1e-6, verbose=0),
    ]
    return model.fit(
        X_train, y_train.astype(np.float32),
        validation_data=(X_val, y_val.astype(np.float32)),
        epochs=epochs, batch_size=batch_size,
        class_weight=class_weight, callbacks=cbs, verbose=2,
    )


# ---------------------------------------------------------------------------
# One model
# ---------------------------------------------------------------------------
def run_one_model(name: str, build_fn, dataset, args, transpose: bool = False):
    (X_tr, y_tr), (X_va, y_va), (X_te, y_te) = (
        dataset["train"], dataset["evaluation"], dataset["test"],
    )

    if transpose:
        X_tr_in = transpose_for_matchboxnet(X_tr)
        X_va_in = transpose_for_matchboxnet(X_va)
        X_te_in = transpose_for_matchboxnet(X_te)
    else:
        X_tr_in = _add_channel(X_tr)
        X_va_in = _add_channel(X_va)
        X_te_in = _add_channel(X_te)

    print(f"\n{'=' * 70}\n  {name}\n{'=' * 70}")
    model = build_fn()
    print(f"  parameters: {model.count_params():,}")

    _fit_anger(model, X_tr_in, y_tr, X_va_in, y_va,
               epochs=args.epochs, batch_size=args.batch_size)

    # Keras eval (Float32 reference).
    y_score = model.predict(X_te_in, verbose=0).reshape(-1)
    metrics_f32_keras = evaluate_anger(y_te, y_score)
    print(f"  [Keras F32] acc={metrics_f32_keras.accuracy:.4f}  "
          f"F1={metrics_f32_keras.f1:.4f}  AUC={metrics_f32_keras.auc:.4f}")

    out_dir = Path(args.output_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    safe = name.lower().replace("+", "p").replace(" ", "_")

    # ----- Float32 TFLite -----
    tfl_f32 = to_tflite_float32(model)
    (out_dir / f"{safe}_f32.tflite").write_bytes(tfl_f32)
    y_score_f32 = predict_tflite(tfl_f32, X_te_in).reshape(-1)
    m_f32 = evaluate_anger(y_te, y_score_f32)
    arena_f32 = measure_arena_kb(tfl_f32)
    flash_f32 = measure_flash_kb(tfl_f32)
    lat_f32 = latency_host_ms(tfl_f32, X_te_in[0])

    # ----- INT8 TFLite -----
    tfl_int8 = to_tflite_int8(model, X_tr_in[:200])
    (out_dir / f"{safe}_int8.tflite").write_bytes(tfl_int8)
    y_score_int8 = predict_tflite(tfl_int8, X_te_in).reshape(-1)
    m_int8 = evaluate_anger(y_te, y_score_int8)
    arena_int8 = measure_arena_kb(tfl_int8)
    flash_int8 = measure_flash_kb(tfl_int8)
    lat_int8 = latency_host_ms(tfl_int8, X_te_in[0])

    result = {
        "name":   name,
        "params": int(model.count_params()),
        "f32": {
            "accuracy":  m_f32.accuracy,  "precision": m_f32.precision,
            "recall":    m_f32.recall,    "f1":        m_f32.f1,
            "auc":       m_f32.auc,
            "arena_kb":  arena_f32,       "flash_kb":  flash_f32,
            "lat_host_ms": lat_f32,
            "confusion": m_f32.confusion.tolist(),
        },
        "int8": {
            "accuracy":  m_int8.accuracy, "precision": m_int8.precision,
            "recall":    m_int8.recall,   "f1":        m_int8.f1,
            "auc":       m_int8.auc,
            "arena_kb":  arena_int8,      "flash_kb":  flash_int8,
            "lat_host_ms": lat_int8,
            "confusion": m_int8.confusion.tolist(),
        },
    }
    (out_dir / f"{safe}.json").write_text(json.dumps(result, indent=2))
    print(f"  [F32 TFLite]  acc={m_f32.accuracy:.4f}  F1={m_f32.f1:.4f}  "
          f"arena={arena_f32}KB  flash={flash_f32}KB  lat_host={lat_f32:.1f}ms")
    print(f"  [INT8 TFLite] acc={m_int8.accuracy:.4f} F1={m_int8.f1:.4f}  "
          f"arena={arena_int8}KB flash={flash_int8}KB lat_host={lat_int8:.1f}ms")
    return result


# ---------------------------------------------------------------------------
# LaTeX exporter
# ---------------------------------------------------------------------------
def export_table_iii(results: list[dict], path: Path) -> None:
    """Render the Table III LaTeX block, matching the paper's column order."""
    rows = []
    for r in results:
        name = r["name"]
        if name == "MicroLightCNN":
            # Report both F32 (paper-chosen) and INT8 ablation row.
            rows.append((f"{name} (F32)",  r["params"], r["f32"]))
            rows.append((f"{name} (INT8)", r["params"], r["int8"]))
        else:
            rows.append((f"{name} (INT8)", r["params"], r["int8"]))

    lines = [
        r"\begin{table}[t]",
        r"\centering",
        r"\caption{Anger detection results on the held-out ESD test set.}",
        r"\label{tab:anger_results}",
        r"\begin{tabular}{lcccccc}",
        r"\toprule",
        r"Model & Acc.\ & F1 & AUC & Params & Arena (KB) & Lat. (ms) \\",
        r"\midrule",
    ]
    for label, params, m in rows:
        lines.append(
            f"{label} & {m['accuracy']*100:.1f} & {m['f1']*100:.1f} & "
            f"{m['auc']*100:.1f} & {params:,} & {m['arena_kb']:.1f} & "
            f"{m['lat_host_ms']:.1f} \\\\"
        )
    lines += [r"\bottomrule", r"\end{tabular}", r"\end{table}", ""]
    path.write_text("\n".join(lines), encoding="utf-8")
    print(f"\n  -> wrote LaTeX table to {path}")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
def main():
    p = argparse.ArgumentParser()
    p.add_argument("--esd-root", required=True,
                   help="Path to ESD/Emotional_Speech_Dataset")
    p.add_argument("--output-dir", default="outputs/anger")
    p.add_argument("--epochs", type=int, default=60)
    p.add_argument("--batch-size", type=int, default=32)
    p.add_argument("--seed", type=int, default=42)
    p.add_argument("--max-per-class", type=int, default=None,
                   help="Cap samples per class for faster runs.")
    args = p.parse_args()

    _set_seed(args.seed)
    dataset = load_esd_all(args.esd_root,
                           max_per_class=args.max_per_class,
                           seed=args.seed)

    results: list[dict] = []

    # 1) MicroLightCNN
    results.append(run_one_model(
        "MicroLightCNN", build_micro_lightcnn, dataset, args, transpose=False))

    # 2) DS-CNN
    results.append(run_one_model(
        "DS-CNN", build_ds_cnn, dataset, args, transpose=False))

    # 3) MatchboxNet
    results.append(run_one_model(
        "MatchboxNet", build_matchboxnet, dataset, args, transpose=True))

    # Save aggregated results.
    out_dir = Path(args.output_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / "table_iii.json").write_text(json.dumps(results, indent=2))
    export_table_iii(results, out_dir / "table_iii.tex")
    print(f"\nDone. Outputs in {out_dir}")


if __name__ == "__main__":
    main()
