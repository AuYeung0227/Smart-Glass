"""
experiments/generate_joint_detection_figure.py

Generate polar heatmaps of joint anger + speaker detection rates from
the on-device tests carried out in a domestic setting following the
PROTOCOL_joint_detection.md guidelines.

Inputs
------
CSV file with one row per (point, speaker, phrase_type) measurement:

    line_id,distance_cm,angle_deg,speaker,phrase_type,
    anger_detected,speaker_correct,notes

Expected layout: 8 radial lines × 4 distances × 4 speakers × 2 phrases
                = 256 binary measurements.

Outputs (under --output-dir)
----------------------------
  fig_joint_anger_polar.pdf       Polar heatmap of anger detection rate
                                  (averaged over 4 speakers × 2 phrases
                                  = 8 trials per (radius, angle) cell)
  fig_joint_speaker_polar.pdf     Polar heatmap of speaker ID rate
  fig_joint_combined_polar.pdf    Polar heatmap of joint (both correct)
  joint_detection_stats.json      Aggregated statistics for the caption

Usage
-----
    python -m experiments.generate_joint_detection_figure \
        --csv outputs/joint_detection_results.csv \
        --output-dir outputs/figures
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np
import pandas as pd
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib import cm
from matplotlib.colors import Normalize


# Expected grid
ANGLES_DEG = np.array([0, 45, 90, 135, 180, 225, 270, 315])   # 8 lines
DISTANCES_CM = np.array([50, 100, 150, 200])                  # 4 radii
EXPECTED_TRIALS_PER_CELL = 4 * 2   # 4 speakers × 2 phrases (neutral+angry)


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
        "pdf.fonttype": 42,
        "ps.fonttype": 42,
    })


# --------------------------------------------------------------------------- #
#  Per-row correctness                                                        #
# --------------------------------------------------------------------------- #
def add_correctness_columns(df: pd.DataFrame) -> pd.DataFrame:
    """Compute anger_correct, speaker_correct, joint_correct per measurement."""
    expected_anger = (df["phrase_type"].str.lower() == "angry").astype(int)
    df = df.copy()
    df["anger_correct"] = (df["anger_detected"].astype(int) == expected_anger).astype(int)
    # speaker_correct already in CSV; coerce to int just in case
    df["speaker_correct"] = df["speaker_correct"].astype(int)
    df["joint_correct"] = (df["anger_correct"] & df["speaker_correct"]).astype(int)
    return df


# --------------------------------------------------------------------------- #
#  Grid aggregation                                                           #
# --------------------------------------------------------------------------- #
def build_rate_grid(df: pd.DataFrame, value_col: str) -> tuple[np.ndarray, np.ndarray]:
    """
    Aggregate the per-trial binary column `value_col` into an
    (n_angles, n_distances) grid of detection rates.

    Returns
    -------
    rate_grid : ndarray, shape (8, 4)
        Detection rate in each (angle, distance) cell.
    count_grid : ndarray, shape (8, 4)
        Number of trials contributing to each cell (for sanity check).
    """
    rate = np.full((len(ANGLES_DEG), len(DISTANCES_CM)), np.nan)
    count = np.zeros_like(rate)

    for i, ang in enumerate(ANGLES_DEG):
        for j, d in enumerate(DISTANCES_CM):
            mask = (df["angle_deg"] == ang) & (df["distance_cm"] == d)
            sub = df.loc[mask, value_col]
            count[i, j] = len(sub)
            if len(sub) > 0:
                rate[i, j] = sub.mean()

    return rate, count


# --------------------------------------------------------------------------- #
#  Polar heatmap renderer                                                     #
# --------------------------------------------------------------------------- #
def plot_polar_heatmap(rate_grid: np.ndarray,
                       title: str,
                       out_pdf: Path,
                       cbar_label: str = "Detection rate") -> None:
    """
    Render an (n_angles, n_distances) grid as a filled polar heatmap.
    Cells are angular wedges (each 45° wide) × radial bands (50 cm each).
    """
    n_ang, n_dist = rate_grid.shape

    # Angular edges in radians (each wedge centered on ANGLES_DEG[i])
    half_wedge = np.deg2rad(45.0) / 2.0
    theta_centers = np.deg2rad(ANGLES_DEG)
    # Radial edges: [25, 75, 125, 175, 225] cm so that centers fall on 50,100,150,200
    radial_edges = np.array([25, 75, 125, 175, 225])

    fig = plt.figure(figsize=(4.4, 4.4), dpi=300)
    ax = fig.add_subplot(111, projection="polar")

    cmap = matplotlib.colormaps["RdYlGn"]  # red (bad) → green (good)
    norm = Normalize(vmin=0.0, vmax=1.0)

    # Draw each (angle, dist) cell as a wedge sector
    for i in range(n_ang):
        th_lo = theta_centers[i] - half_wedge
        th_hi = theta_centers[i] + half_wedge
        for j in range(n_dist):
            r_lo = radial_edges[j]
            r_hi = radial_edges[j + 1]
            v = rate_grid[i, j]
            if np.isnan(v):
                color = "#e5e7eb"  # missing → light grey
            else:
                color = cmap(norm(v))
            # Build a filled wedge using a high-resolution arc
            arc = np.linspace(th_lo, th_hi, 24)
            theta_poly = np.concatenate([arc, arc[::-1]])
            r_poly = np.concatenate([
                np.full_like(arc, r_lo),
                np.full_like(arc, r_hi),
            ])
            ax.fill(theta_poly, r_poly, color=color, edgecolor="white",
                    linewidth=0.8, zorder=2)
            # Annotate with percentage
            if not np.isnan(v):
                r_mid = 0.5 * (r_lo + r_hi)
                th_mid = theta_centers[i]
                ax.text(th_mid, r_mid, f"{100*v:.0f}",
                        ha="center", va="center",
                        fontsize=8, fontweight="bold",
                        color="white" if (v < 0.40 or v > 0.85) else "black",
                        zorder=4)

    # Cosmetic polar setup
    ax.set_theta_zero_location("N")     # 0° at top (Line 1)
    ax.set_theta_direction(-1)          # clockwise
    ax.set_rticks(DISTANCES_CM)
    ax.set_rlabel_position(22.5)        # offset r-labels so they don't overlap
    ax.set_yticklabels([f"{d} cm" for d in DISTANCES_CM], fontsize=8)
    ax.set_xticks(np.deg2rad(ANGLES_DEG))
    ax.set_xticklabels([f"{a}°" for a in ANGLES_DEG], fontsize=8)
    ax.set_rlim(0, 225)
    ax.set_title(title, pad=16)
    ax.grid(True, linewidth=0.4, alpha=0.5)
    ax.set_axisbelow(True)

    # Colorbar
    sm = cm.ScalarMappable(cmap=cmap, norm=norm); sm.set_array([])
    cbar = fig.colorbar(sm, ax=ax, fraction=0.05, pad=0.10, shrink=0.75)
    cbar.set_label(cbar_label, fontsize=9)
    cbar.ax.tick_params(labelsize=8)
    cbar.outline.set_linewidth(0.6)

    plt.tight_layout()
    plt.savefig(out_pdf, format="pdf", bbox_inches="tight")
    plt.close(fig)


# --------------------------------------------------------------------------- #
#  Summary statistics                                                         #
# --------------------------------------------------------------------------- #
def compute_summary(df: pd.DataFrame) -> dict:
    """Aggregate statistics suitable for the figure caption and Section IV-E."""
    out = {
        "n_measurements": int(len(df)),
        "n_speakers": int(df["speaker"].nunique()),
        "n_points": int(df[["line_id", "distance_cm"]].drop_duplicates().shape[0]),
    }
    out["overall"] = {
        "anger_acc":   round(float(df["anger_correct"].mean()),   4),
        "speaker_acc": round(float(df["speaker_correct"].mean()), 4),
        "joint_acc":   round(float(df["joint_correct"].mean()),   4),
    }
    # By distance
    by_dist = df.groupby("distance_cm")[["anger_correct", "speaker_correct",
                                          "joint_correct"]].mean().round(4)
    out["by_distance"] = {int(k): v.to_dict() for k, v in by_dist.iterrows()}

    # By speaker
    by_sp = df.groupby("speaker")[["anger_correct", "speaker_correct",
                                    "joint_correct"]].mean().round(4)
    out["by_speaker"] = {k: v.to_dict() for k, v in by_sp.iterrows()}

    # By phrase_type
    by_ph = df.groupby("phrase_type")[["anger_correct", "speaker_correct",
                                        "joint_correct"]].mean().round(4)
    out["by_phrase_type"] = {k: v.to_dict() for k, v in by_ph.iterrows()}

    return out


# --------------------------------------------------------------------------- #
#  Main                                                                       #
# --------------------------------------------------------------------------- #
def main() -> None:
    p = argparse.ArgumentParser()
    p.add_argument("--csv", type=str,
                   default="outputs/joint_detection_results.csv",
                   help="CSV file with the per-trial measurements.")
    p.add_argument("--output-dir", type=str, default="outputs/figures")
    args = p.parse_args()

    _apply_style()

    csv_path = Path(args.csv)
    if not csv_path.exists():
        raise FileNotFoundError(
            f"CSV not found at {csv_path}. Run the on-device experiments first "
            f"following PROTOCOL_joint_detection.md.")

    out_dir = Path(args.output_dir); out_dir.mkdir(parents=True, exist_ok=True)

    # 1. Load + sanity check
    print(f"[csv] loading {csv_path}")
    df = pd.read_csv(csv_path)
    required = {"line_id", "distance_cm", "angle_deg", "speaker",
                "phrase_type", "anger_detected", "speaker_correct"}
    missing = required - set(df.columns)
    if missing:
        raise ValueError(f"CSV missing columns: {missing}")

    print(f"[csv] {len(df)} rows  speakers={sorted(df['speaker'].unique())}  "
          f"distances={sorted(df['distance_cm'].unique())}  "
          f"angles={sorted(df['angle_deg'].unique())}")

    expected = len(ANGLES_DEG) * len(DISTANCES_CM) * EXPECTED_TRIALS_PER_CELL
    if len(df) != expected:
        print(f"[warn] expected {expected} rows but got {len(df)} — "
              f"missing or extra measurements will appear as NaN cells.")

    # 2. Per-trial correctness
    df = add_correctness_columns(df)

    # 3. Aggregate to (angle, distance) grids
    anger_grid,   _ = build_rate_grid(df, "anger_correct")
    speaker_grid, _ = build_rate_grid(df, "speaker_correct")
    joint_grid,   _ = build_rate_grid(df, "joint_correct")

    # 4. Render the three polar heatmaps
    print(f"[fig] writing polar heatmaps to {out_dir}")
    plot_polar_heatmap(anger_grid,
                       title="Anger detection (%)",
                       out_pdf=out_dir / "fig_joint_anger_polar.pdf",
                       cbar_label="Anger detection rate")
    plot_polar_heatmap(speaker_grid,
                       title="Speaker identification (%)",
                       out_pdf=out_dir / "fig_joint_speaker_polar.pdf",
                       cbar_label="Speaker ID rate")
    plot_polar_heatmap(joint_grid,
                       title="Joint accuracy: anger $\\wedge$ speaker (%)",
                       out_pdf=out_dir / "fig_joint_combined_polar.pdf",
                       cbar_label="Joint detection rate")

    # 5. Summary
    stats = compute_summary(df)
    stats_path = out_dir / "joint_detection_stats.json"
    stats_path.write_text(json.dumps(stats, indent=2, ensure_ascii=False))

    print("\n" + "=" * 64)
    print("  Joint detection statistics (for paper caption)")
    print("=" * 64)
    print(f"  Total measurements: {stats['n_measurements']}")
    print(f"  Speakers: {stats['n_speakers']}   Test points: {stats['n_points']}")
    print(f"\n  Overall:")
    print(f"    anger_acc   = {100*stats['overall']['anger_acc']:.1f}%")
    print(f"    speaker_acc = {100*stats['overall']['speaker_acc']:.1f}%")
    print(f"    joint_acc   = {100*stats['overall']['joint_acc']:.1f}%")
    print(f"\n  By distance:")
    for d, m in stats["by_distance"].items():
        print(f"    {d:>3} cm: anger={100*m['anger_correct']:5.1f}%  "
              f"speaker={100*m['speaker_correct']:5.1f}%  "
              f"joint={100*m['joint_correct']:5.1f}%")
    print(f"\n  By speaker:")
    for sp, m in stats["by_speaker"].items():
        print(f"    {sp:>6}: anger={100*m['anger_correct']:5.1f}%  "
              f"speaker={100*m['speaker_correct']:5.1f}%  "
              f"joint={100*m['joint_correct']:5.1f}%")
    print(f"\n  Stats saved to: {stats_path}")
    print(f"  Figures saved to: {out_dir}/fig_joint_*.pdf")


if __name__ == "__main__":
    main()