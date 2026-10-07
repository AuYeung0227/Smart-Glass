"""
experiments/generate_anger_figures.py

Generate the two figures for the anger detection section of the paper:

    (1) 2x2 confusion matrix of MicroLightCNN INT8 on the ESD test set
        (rows = true label, columns = predicted label)
    (2) ROC curve of MicroLightCNN INT8 on the ESD test set, with AUC

Both figures are saved as PDF (vector, IEEE-friendly) under outputs/figures/.

Usage
-----
    python -m experiments.generate_anger_figures \
        --esd-root ESD/Emotional_Speech_Dataset \
        --anger-dir outputs/anger \
        --output-dir outputs/figures \
        --threshold 0.5
"""
from __future__ import annotations

import argparse
from pathlib import Path

import numpy as np
import matplotlib
matplotlib.use("Agg")  # no display needed
import matplotlib.pyplot as plt
from matplotlib.ticker import MultipleLocator
from sklearn.metrics import (
    confusion_matrix,
    roc_curve,
    roc_auc_score,
    f1_score,
    accuracy_score,
)

from utils.datasets import load_esd_all
from utils.tflite_tools import predict_tflite


# --------------------------------------------------------------------------- #
#  IEEE-friendly matplotlib defaults                                          #
# --------------------------------------------------------------------------- #
def _apply_style() -> None:
    plt.rcParams.update({
        "font.family": "serif",
        "font.serif": ["Times New Roman", "Times", "DejaVu Serif"],
        "font.size": 10,
        "axes.titlesize": 11,
        "axes.labelsize": 10,
        "xtick.labelsize": 9,
        "ytick.labelsize": 9,
        "legend.fontsize": 9,
        "axes.linewidth": 0.8,
        "axes.spines.top": False,
        "axes.spines.right": False,
        "pdf.fonttype": 42,   # TrueType embedding (editable in Illustrator/Figma)
        "ps.fonttype": 42,
    })


# --------------------------------------------------------------------------- #
#  Figure 1 — Confusion matrix                                                #
# --------------------------------------------------------------------------- #
def plot_confusion_matrix(y_true: np.ndarray,
                          y_pred: np.ndarray,
                          out_pdf: Path) -> dict:
    """2x2 confusion matrix with counts (top) and row-normalized rates (bottom)."""
    cm = confusion_matrix(y_true, y_pred, labels=[0, 1])
    cm_norm = cm.astype(float) / cm.sum(axis=1, keepdims=True)

    fig, ax = plt.subplots(figsize=(3.4, 3.2), dpi=300)
    im = ax.imshow(cm_norm, cmap="Blues", vmin=0.0, vmax=1.0)

    # Axis ticks/labels
    ax.set_xticks([0, 1]); ax.set_yticks([0, 1])
    ax.set_xticklabels(["Not-Angry", "Angry"])
    ax.set_yticklabels(["Not-Angry", "Angry"])
    ax.set_xlabel("Predicted label")
    ax.set_ylabel("True label")
    ax.set_title("MicroLightCNN (INT8) — ESD test set", pad=10)

    # Cell annotations: count + percentage
    for i in range(2):
        for j in range(2):
            count = cm[i, j]
            pct = 100.0 * cm_norm[i, j]
            color = "white" if cm_norm[i, j] > 0.55 else "black"
            ax.text(j, i - 0.08, f"{count:d}",
                    ha="center", va="center",
                    color=color, fontsize=14, fontweight="bold")
            ax.text(j, i + 0.18, f"({pct:.1f}\\%)" if False else f"({pct:.1f}%)",
                    ha="center", va="center",
                    color=color, fontsize=9)

    # Colorbar
    cbar = fig.colorbar(im, ax=ax, fraction=0.046, pad=0.04)
    cbar.set_label("Row-normalized rate", fontsize=9)
    cbar.ax.tick_params(labelsize=8)
    cbar.outline.set_linewidth(0.6)

    plt.tight_layout()
    plt.savefig(out_pdf, format="pdf", bbox_inches="tight")
    plt.close(fig)

    # Return summary stats for the caption
    tn, fp, fn, tp = cm.ravel()
    return {
        "tn": int(tn), "fp": int(fp),
        "fn": int(fn), "tp": int(tp),
        "accuracy": float(accuracy_score(y_true, y_pred)),
        "f1": float(f1_score(y_true, y_pred)),
        "precision": float(tp / (tp + fp)) if (tp + fp) > 0 else 0.0,
        "recall": float(tp / (tp + fn)) if (tp + fn) > 0 else 0.0,
        "specificity": float(tn / (tn + fp)) if (tn + fp) > 0 else 0.0,
    }


# --------------------------------------------------------------------------- #
#  Figure 2 — ROC curve                                                       #
# --------------------------------------------------------------------------- #
def plot_roc_curve(y_true: np.ndarray,
                   y_score: np.ndarray,
                   out_pdf: Path) -> dict:
    """ROC with AUC; operating point at threshold=0.5 highlighted."""
    fpr, tpr, thr = roc_curve(y_true, y_score)
    auc = roc_auc_score(y_true, y_score)

    # Find index closest to threshold 0.5 for operating point
    idx_05 = int(np.argmin(np.abs(thr - 0.5))) if len(thr) > 0 else 0

    fig, ax = plt.subplots(figsize=(3.4, 3.2), dpi=300)

    # ROC curve
    ax.plot(fpr, tpr, color="#1d4ed8", linewidth=1.6,
            label=f"MicroLightCNN INT8 (AUC = {auc:.3f})")

    # Random chance
    ax.plot([0, 1], [0, 1], color="#9ca3af", linestyle="--",
            linewidth=1.0, label="Random (AUC = 0.500)")

    # Operating point @ threshold 0.5
    ax.scatter([fpr[idx_05]], [tpr[idx_05]],
               s=42, color="#dc2626", zorder=5,
               edgecolor="white", linewidth=0.8,
               label=f"Operating point ($\\tau=0.5$)")

    ax.set_xlim(-0.02, 1.02); ax.set_ylim(-0.02, 1.02)
    ax.set_xlabel("False positive rate")
    ax.set_ylabel("True positive rate")
    ax.set_title("ROC curve — ESD test set", pad=10)
    ax.xaxis.set_major_locator(MultipleLocator(0.2))
    ax.yaxis.set_major_locator(MultipleLocator(0.2))
    ax.grid(True, linewidth=0.4, alpha=0.4)
    ax.legend(loc="lower right", frameon=False)
    ax.set_aspect("equal", adjustable="box")

    plt.tight_layout()
    plt.savefig(out_pdf, format="pdf", bbox_inches="tight")
    plt.close(fig)

    return {
        "auc": float(auc),
        "operating_fpr": float(fpr[idx_05]),
        "operating_tpr": float(tpr[idx_05]),
        "operating_threshold": float(thr[idx_05]) if len(thr) > 0 else 0.5,
    }


# --------------------------------------------------------------------------- #
#  Main                                                                       #
# --------------------------------------------------------------------------- #
def main() -> None:
    p = argparse.ArgumentParser()
    p.add_argument("--esd-root", type=str,
                   default="ESD/Emotional_Speech_Dataset",
                   help="Root directory of the ESD dataset.")
    p.add_argument("--anger-dir", type=str, default="outputs/anger",
                   help="Directory containing trained TFLite models.")
    p.add_argument("--output-dir", type=str, default="outputs/figures",
                   help="Where to save the generated PDF figures.")
    p.add_argument("--threshold", type=float, default=0.5,
                   help="Decision threshold for the confusion matrix.")
    p.add_argument("--model", type=str, default="microlightcnn_int8",
                   choices=["microlightcnn_int8", "microlightcnn_f32"],
                   help="Which TFLite to evaluate.")
    p.add_argument("--seed", type=int, default=42)
    args = p.parse_args()

    _apply_style()

    anger_dir = Path(args.anger_dir)
    out_dir = Path(args.output_dir); out_dir.mkdir(parents=True, exist_ok=True)

    # ----- 1. Load ESD test split (binary: Angry=1 vs rest=0, balanced) -----
    print("[ESD] loading dataset...")
    dataset = load_esd_all(args.esd_root, seed=args.seed)
    X_test, y_test = dataset["test"]
    print(f"[ESD] test: X={X_test.shape}  y={dict(zip(*np.unique(y_test, return_counts=True)))}")

    # MicroLightCNN expects shape [N, 20, 63, 1]
    if X_test.ndim == 3:
        X_test_in = X_test[..., np.newaxis].astype(np.float32)
    else:
        X_test_in = X_test.astype(np.float32)

    # ----- 2. Load the TFLite model and predict -----
    tfl_path = anger_dir / f"{args.model}.tflite"
    if not tfl_path.exists():
        raise FileNotFoundError(
            f"TFLite model not found at {tfl_path}. Run experiments.run_anger first.")
    print(f"[model] loading {tfl_path}")
    tfl_bytes = tfl_path.read_bytes()
    y_score = predict_tflite(tfl_bytes, X_test_in).reshape(-1)
    y_pred = (y_score >= args.threshold).astype(np.int32)

    print(f"[eval] accuracy={accuracy_score(y_test, y_pred):.4f}  "
          f"F1={f1_score(y_test, y_pred):.4f}  "
          f"AUC={roc_auc_score(y_test, y_score):.4f}")

    # ----- 3. Generate the two figures -----
    cm_pdf = out_dir / "fig_anger_confusion_matrix.pdf"
    roc_pdf = out_dir / "fig_anger_roc_curve.pdf"

    print(f"\n[fig] writing confusion matrix to {cm_pdf}")
    cm_stats = plot_confusion_matrix(y_test, y_pred, cm_pdf)

    print(f"[fig] writing ROC curve to {roc_pdf}")
    roc_stats = plot_roc_curve(y_test, y_score, roc_pdf)

    # ----- 4. Print summary suitable for the paper caption -----
    print("\n" + "=" * 60)
    print("  Figure statistics (for paper caption)")
    print("=" * 60)
    print(f"  Confusion matrix:")
    print(f"    TN={cm_stats['tn']}  FP={cm_stats['fp']}  "
          f"FN={cm_stats['fn']}  TP={cm_stats['tp']}")
    print(f"    Accuracy={cm_stats['accuracy']:.4f}  "
          f"F1={cm_stats['f1']:.4f}")
    print(f"    Precision={cm_stats['precision']:.4f}  "
          f"Recall={cm_stats['recall']:.4f}  "
          f"Specificity={cm_stats['specificity']:.4f}")
    print(f"  ROC:")
    print(f"    AUC={roc_stats['auc']:.4f}")
    print(f"    Operating point @ tau={roc_stats['operating_threshold']:.3f}: "
          f"FPR={roc_stats['operating_fpr']:.3f}  "
          f"TPR={roc_stats['operating_tpr']:.3f}")

    print(f"\nDone. Figures in {out_dir}/")


if __name__ == "__main__":
    main()