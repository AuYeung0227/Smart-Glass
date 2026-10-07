r"""
Aggregate all four tables (III, IV, V, VI) into a single LaTeX file and a
single JSON summary, ready to be \input{} into the paper.

Usage
-----
    python -m experiments.build_all_tables \
        --anger-dir    outputs/anger \
        --speaker-dir  outputs/speaker \
        --quant-dir    outputs/quant \
        --pipeline-dir outputs/pipeline \
        --output-dir   outputs/paper_tables
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path


def _read_json(path: Path):
    return json.loads(path.read_text(encoding="utf-8")) if path.exists() else None


def _read_tex(path: Path) -> str:
    return path.read_text(encoding="utf-8") if path.exists() else (
        f"% {path.name} not found\n"
    )


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--anger-dir",    default="outputs/anger")
    p.add_argument("--speaker-dir",  default="outputs/speaker")
    p.add_argument("--quant-dir",    default="outputs/quant")
    p.add_argument("--pipeline-dir", default="outputs/pipeline")
    p.add_argument("--output-dir",   default="outputs/paper_tables")
    args = p.parse_args()

    out_dir = Path(args.output_dir); out_dir.mkdir(parents=True, exist_ok=True)

    # ---------- JSON summary ----------
    summary = {
        "table_iii_anger":    _read_json(Path(args.anger_dir)    / "table_iii.json"),
        "table_iv_speaker":   _read_json(Path(args.speaker_dir)  / "table_iv.json"),
        "table_v_quant":      _read_json(Path(args.quant_dir)    / "table_v.json"),
        "table_vi_pipeline":  _read_json(Path(args.pipeline_dir) / "table_vi.json"),
    }
    (out_dir / "all_tables.json").write_text(json.dumps(summary, indent=2),
                                             encoding="utf-8")
    print(f"-> wrote aggregate JSON to {out_dir/'all_tables.json'}")

    # ---------- LaTeX bundle ----------
    blocks = [
        "% --- Table III ---",
        _read_tex(Path(args.anger_dir)    / "table_iii.tex"),
        "% --- Table IV ---",
        _read_tex(Path(args.speaker_dir)  / "table_iv.tex"),
        "% --- Table V ---",
        _read_tex(Path(args.quant_dir)    / "table_v.tex"),
        "% --- Table VI ---",
        _read_tex(Path(args.pipeline_dir) / "table_vi.tex"),
    ]
    out_tex = out_dir / "all_tables.tex"
    out_tex.write_text("\n\n".join(blocks), encoding="utf-8")
    print(f"-> wrote aggregate LaTeX to {out_tex}")

    # ---------- Console summary ----------
    print("\nSummary of available tables:")
    for k, v in summary.items():
        status = "OK" if v is not None else "MISSING"
        print(f"  {k:24s}  {status}")


if __name__ == "__main__":
    main()
