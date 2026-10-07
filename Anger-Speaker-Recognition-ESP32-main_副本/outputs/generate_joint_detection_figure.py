from __future__ import annotations

import sys
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[1]
OUTPUT_DIR = Path(__file__).resolve().parent

if str(PROJECT_ROOT) not in sys.path:
    sys.path.insert(0, str(PROJECT_ROOT))

from experiments.generate_joint_detection_figure import main


if __name__ == "__main__":
    if len(sys.argv) == 1:
        sys.argv.extend(
            [
                "--csv",
                str(OUTPUT_DIR / "joint_detection_results.csv"),
                "--output-dir",
                str(OUTPUT_DIR / "figures"),
            ]
        )
    main()
