"""
Classification metrics used by Tables III and IV.

Anger (binary):    accuracy, precision, recall, F1, ROC-AUC
Speaker (N-way):   top-1, EER (one-vs-rest macro), rejection rate under
                   the joint threshold (tau_sp, delta) used in the paper.
"""
from __future__ import annotations

from dataclasses import dataclass

import numpy as np
from sklearn.metrics import (
    accuracy_score, precision_recall_fscore_support, roc_auc_score,
    confusion_matrix, roc_curve,
)


# =========================================================================
# Anger (binary)
# =========================================================================
@dataclass
class AngerMetrics:
    accuracy:  float
    precision: float
    recall:    float
    f1:        float
    auc:       float
    confusion: np.ndarray  # 2x2


def evaluate_anger(y_true: np.ndarray, y_score: np.ndarray,
                   threshold: float = 0.5) -> AngerMetrics:
    y_score = np.asarray(y_score, dtype=np.float64).reshape(-1)
    # Sanitize potential NaN/Inf from quantization.
    if not np.isfinite(y_score).all():
        y_score = np.nan_to_num(y_score, nan=0.5, posinf=1.0, neginf=0.0)
    y_pred = (y_score >= threshold).astype(np.int32)
    acc = accuracy_score(y_true, y_pred)
    p, r, f1, _ = precision_recall_fscore_support(
        y_true, y_pred, average="binary", pos_label=1, zero_division=0,
    )
    try:
        auc = roc_auc_score(y_true, y_score)
    except ValueError:
        auc = float("nan")
    cm = confusion_matrix(y_true, y_pred, labels=[0, 1])
    return AngerMetrics(acc, p, r, f1, auc, cm)


# =========================================================================
# Speaker (N-way) with paper's joint threshold (tau_sp, delta)
# =========================================================================
@dataclass
class SpeakerMetrics:
    top1_accepted:   float   # accuracy over decisions that pass the threshold
    top1_all:        float   # accuracy if we forced a decision on every sample
    eer_macro:       float   # one-vs-rest macro EER
    rejection_rate:  float
    confusion:       np.ndarray


def _eer_one_vs_rest(y_true_bin: np.ndarray, y_score: np.ndarray) -> float:
    """Equal-Error Rate for one-vs-rest binary scores."""
    y_score = np.asarray(y_score, dtype=np.float64)
    if not np.isfinite(y_score).all():
        # NaN/Inf in scores (typical of poorly quantized INT8 attentive
        # pooling): replace with 0.5 so the ROC curve is well-defined and
        # EER degrades gracefully toward chance.
        y_score = np.nan_to_num(y_score, nan=0.5, posinf=1.0, neginf=0.0)
    if len(np.unique(y_score)) < 2:
        return 0.5
    fpr, tpr, _ = roc_curve(y_true_bin, y_score)
    fnr = 1 - tpr
    idx = int(np.nanargmin(np.abs(fpr - fnr)))
    return float((fpr[idx] + fnr[idx]) / 2.0)


def evaluate_speaker(y_true: np.ndarray, proba: np.ndarray,
                     tau_sp: float = 0.60, delta: float = 0.20,
                     ) -> SpeakerMetrics:
    """
    Parameters
    ----------
    proba : (N, C) softmax posteriors.
    tau_sp, delta : decision thresholds from Section III-D.
    """
    proba = np.asarray(proba, dtype=np.float64)
    y_true = np.asarray(y_true)
    n, c = proba.shape

    # Sanitize NaN/Inf that may emerge from INT8-quantized non-linearities
    # in baselines (attentive stats pooling, etc.). Equivalent to a uniform
    # posterior so the row is automatically rejected by the joint threshold.
    bad_rows = ~np.isfinite(proba).all(axis=1)
    if bad_rows.any():
        proba[bad_rows] = 1.0 / c

    # Joint threshold: accept if max >= tau_sp AND p1 - p2 >= delta.
    top1_idx = np.argmax(proba, axis=1)
    top1_val = proba[np.arange(n), top1_idx]
    sorted_pp = np.sort(proba, axis=1)
    margin = sorted_pp[:, -1] - sorted_pp[:, -2]
    accept = (top1_val >= tau_sp) & (margin >= delta)

    if accept.any():
        top1_acc = float((top1_idx[accept] == y_true[accept]).mean())
    else:
        top1_acc = 0.0
    top1_all = float((top1_idx == y_true).mean())
    rej_rate = float(1.0 - accept.mean())

    # Macro EER (one-vs-rest).
    eers = []
    for cls in range(c):
        y_bin = (y_true == cls).astype(np.int32)
        if y_bin.sum() == 0 or y_bin.sum() == n:
            continue
        eers.append(_eer_one_vs_rest(y_bin, proba[:, cls]))
    eer = float(np.mean(eers)) if eers else float("nan")

    cm = confusion_matrix(y_true, top1_idx, labels=list(range(c)))
    return SpeakerMetrics(top1_acc, top1_all, eer, rej_rate, cm)
