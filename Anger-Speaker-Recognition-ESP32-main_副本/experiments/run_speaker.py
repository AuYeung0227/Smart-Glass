"""
Table IV — Speaker identification results.

For each speaker model, fits on the augmented training split of
`dataset_fam/`, evaluates on the held-out validation split (per-source
hold-out), and records:

    top-1 accuracy (under joint threshold (tau_sp, delta)=(0.60, 0.20)),
    macro EER, rejection rate, parameter count, arena, latency.

Outputs per-model TFLite (Float32 and INT8), per-model JSON, the aggregate
`table_iv.json`, and a LaTeX-ready `table_iv.tex`.

Usage
-----
    python -m experiments.run_speaker \
        --dataset-dir ../dataset_fam \
        --output-dir outputs/speaker \
        --epochs 100 --batch-size 64 --seed 42
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

from models import (
    build_mlp_xi_embedding, build_ecapa_pruned, build_campp_pruned,
)
from utils import (
    collect_speaker_segments, split_records_by_source, augment_records,
    records_to_mfcc, records_to_xivector,
    evaluate_speaker,
    to_tflite_float32, to_tflite_int8,
    measure_arena_kb, measure_flash_kb, latency_host_ms, predict_tflite,
)


def _set_seed(seed: int) -> None:
    import random
    random.seed(seed)
    np.random.seed(seed)
    tf.random.set_seed(seed)


def _fit_speaker(model: tf.keras.Model,
                 X_train: np.ndarray, y_train: np.ndarray,
                 X_val:   np.ndarray, y_val:   np.ndarray,
                 epochs: int, batch_size: int):
    cw = compute_class_weight("balanced", classes=np.unique(y_train), y=y_train)
    class_weight = dict(enumerate(cw))
    cbs = [
        tf.keras.callbacks.EarlyStopping(monitor="val_accuracy", patience=20,
                                         restore_best_weights=True, verbose=0),
        tf.keras.callbacks.ReduceLROnPlateau(monitor="val_loss", factor=0.5,
                                             patience=7, min_lr=1e-6, verbose=0),
    ]
    return model.fit(X_train, y_train,
                     validation_data=(X_val, y_val),
                     epochs=epochs, batch_size=batch_size,
                     class_weight=class_weight, callbacks=cbs, verbose=2)


def _evaluate_tflite_speaker(tfl_bytes: bytes, X_val: np.ndarray, y_val: np.ndarray,
                             tau_sp: float, delta: float):
    proba = predict_tflite(tfl_bytes, X_val)
    return evaluate_speaker(y_val, proba, tau_sp=tau_sp, delta=delta), proba


def _train_and_eval_xi(name: str, args, labels: list[str],
                       train_records: list[dict], val_records: list[dict]):
    print(f"\n{'=' * 70}\n  {name}  (Xi-Vector front-end)\n{'=' * 70}")
    X_tr_raw, y_tr = records_to_xivector(train_records)
    X_va_raw, y_va = records_to_xivector(val_records)

    scaler = StandardScaler()
    X_tr = scaler.fit_transform(X_tr_raw).astype(np.float32)
    X_va = scaler.transform(X_va_raw).astype(np.float32)

    model = build_mlp_xi_embedding(num_classes=len(labels))
    print(f"  parameters: {model.count_params():,}")
    _fit_speaker(model, X_tr, y_tr, X_va, y_va,
                 epochs=args.epochs, batch_size=args.batch_size)

    tfl_f32 = to_tflite_float32(model)
    tfl_int8 = to_tflite_int8(model, X_tr[:200])

    out_dir = Path(args.output_dir); out_dir.mkdir(parents=True, exist_ok=True)
    safe = name.lower().replace("-", "_").replace(" ", "_")
    (out_dir / f"{safe}_f32.tflite").write_bytes(tfl_f32)
    (out_dir / f"{safe}_int8.tflite").write_bytes(tfl_int8)
    np.save(out_dir / f"{safe}_xi_mean.npy", scaler.mean_)
    np.save(out_dir / f"{safe}_xi_std.npy", scaler.scale_)

    m_f32, _  = _evaluate_tflite_speaker(tfl_f32, X_va, y_va, args.tau_sp, args.delta)
    m_int8, _ = _evaluate_tflite_speaker(tfl_int8, X_va, y_va, args.tau_sp, args.delta)

    return _pack(name, model.count_params(),
                 tfl_f32, tfl_int8, m_f32, m_int8,
                 X_va, out_dir, safe)


def _train_and_eval_frame_model(name: str, build_fn, args, labels: list[str],
                                train_records: list[dict], val_records: list[dict]):
    print(f"\n{'=' * 70}\n  {name}  (frame-level MFCC)\n{'=' * 70}")
    X_tr_mfcc, y_tr = records_to_mfcc(train_records)
    X_va_mfcc, y_va = records_to_mfcc(val_records)

    # ECAPA and CAM++ expect (T, F) = (63, 20).
    X_tr = np.transpose(X_tr_mfcc, (0, 2, 1)).astype(np.float32)
    X_va = np.transpose(X_va_mfcc, (0, 2, 1)).astype(np.float32)

    model = build_fn(num_classes=len(labels))
    print(f"  parameters: {model.count_params():,}")
    _fit_speaker(model, X_tr, y_tr, X_va, y_va,
                 epochs=args.epochs, batch_size=args.batch_size)

    tfl_f32 = to_tflite_float32(model)
    tfl_int8 = to_tflite_int8(model, X_tr[:200])

    out_dir = Path(args.output_dir); out_dir.mkdir(parents=True, exist_ok=True)
    safe = name.lower().replace("-", "_").replace("+", "p").replace(" ", "_")
    (out_dir / f"{safe}_f32.tflite").write_bytes(tfl_f32)
    (out_dir / f"{safe}_int8.tflite").write_bytes(tfl_int8)

    m_f32, _  = _evaluate_tflite_speaker(tfl_f32, X_va, y_va, args.tau_sp, args.delta)
    m_int8, _ = _evaluate_tflite_speaker(tfl_int8, X_va, y_va, args.tau_sp, args.delta)

    return _pack(name, model.count_params(),
                 tfl_f32, tfl_int8, m_f32, m_int8,
                 X_va, out_dir, safe)


def _pack(name: str, params: int,
          tfl_f32: bytes, tfl_int8: bytes, m_f32, m_int8,
          X_val: np.ndarray, out_dir: Path, safe: str) -> dict:
    def _block(tfl, m):
        return {
            "top1_accepted":  m.top1_accepted,
            "top1_all":       m.top1_all,
            "eer":            m.eer_macro,
            "rejection_rate": m.rejection_rate,
            "confusion":      m.confusion.tolist(),
            "arena_kb":       measure_arena_kb(tfl),
            "flash_kb":       measure_flash_kb(tfl),
            "lat_host_ms":    latency_host_ms(tfl, X_val[0]),
        }
    result = {
        "name": name, "params": int(params),
        "f32":  _block(tfl_f32, m_f32),
        "int8": _block(tfl_int8, m_int8),
    }
    (out_dir / f"{safe}.json").write_text(json.dumps(result, indent=2))
    print(f"  [F32 ] top-1(acc)={m_f32.top1_accepted:.3f}  "
          f"EER={m_f32.eer_macro:.3f}  rej={m_f32.rejection_rate:.2f}  "
          f"arena={result['f32']['arena_kb']}KB  lat={result['f32']['lat_host_ms']:.1f}ms")
    print(f"  [INT8] top-1(acc)={m_int8.top1_accepted:.3f}  "
          f"EER={m_int8.eer_macro:.3f}  rej={m_int8.rejection_rate:.2f}  "
          f"arena={result['int8']['arena_kb']}KB  lat={result['int8']['lat_host_ms']:.1f}ms")
    return result


def export_table_iv(results: list[dict], path: Path) -> None:
    rows = []
    for r in results:
        if r["name"] == "MLP-XiEmbedding":
            rows.append((f"{r['name']} (INT8)", r["params"], r["int8"]))
        else:
            rows.append((f"{r['name']} (F32)", r["params"], r["f32"]))

    lines = [
        r"\begin{table}[t]", r"\centering",
        r"\caption{Speaker identification results for $N=4$ enrolled speakers. "
        r"Rejection rate measured under joint thresholds $\tau_{sp}=0.60$ and $\delta=0.20$.}",
        r"\label{tab:speaker_results}",
        r"\begin{tabular}{lccccc}",
        r"\toprule",
        r"Model & Top-1 & EER & Rej. Rate & Params & Lat.\ (ms) \\",
        r"\midrule",
    ]
    for label, params, m in rows:
        lines.append(
            f"{label} & {m['top1_accepted']*100:.1f} & {m['eer']:.2f} & "
            f"{m['rejection_rate']*100:.1f} & {params:,} & "
            f"{m['lat_host_ms']:.1f} \\\\"
        )
    lines += [r"\bottomrule", r"\end{tabular}", r"\end{table}", ""]
    path.write_text("\n".join(lines), encoding="utf-8")
    print(f"\n  -> wrote LaTeX table to {path}")


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--dataset-dir", default="../dataset_fam")
    p.add_argument("--output-dir", default="outputs/speaker")
    p.add_argument("--epochs", type=int, default=100)
    p.add_argument("--batch-size", type=int, default=64)
    p.add_argument("--seed", type=int, default=42)
    p.add_argument("--augment-copies", type=int, default=2)
    p.add_argument("--augment-mode", choices=["loud", "snr", "mixed"], default="mixed")
    p.add_argument("--val-ratio", type=float, default=0.2)
    p.add_argument("--tau-sp", type=float, default=0.60)
    p.add_argument("--delta",  type=float, default=0.20)
    args = p.parse_args()

    _set_seed(args.seed)

    records, labels = collect_speaker_segments(args.dataset_dir)
    print(f"Speakers: {labels}")
    print(f"Clean segments: {len(records)}")
    train_clean, val_records = split_records_by_source(
        records, val_ratio=args.val_ratio, seed=args.seed)
    train_records = augment_records(train_clean, copies=args.augment_copies,
                                    mode=args.augment_mode, seed=args.seed)
    print(f"Train (augmented): {len(train_records)}  Val: {len(val_records)}")

    results = []

    results.append(_train_and_eval_xi(
        "MLP-XiEmbedding", args, labels, train_records, val_records))

    results.append(_train_and_eval_frame_model(
        "ECAPA-TDNN-pruned", build_ecapa_pruned, args, labels,
        train_records, val_records))

    results.append(_train_and_eval_frame_model(
        "CAM++", build_campp_pruned, args, labels,
        train_records, val_records))

    out_dir = Path(args.output_dir); out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / "table_iv.json").write_text(json.dumps(results, indent=2))
    export_table_iv(results, out_dir / "table_iv.tex")
    print(f"\nDone. Outputs in {out_dir}")


if __name__ == "__main__":
    main()
