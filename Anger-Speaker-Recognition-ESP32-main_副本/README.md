# Compact Hybrid Model for Anger and Speaker Recognition on ESP32

> **TinyML pipeline that jointly detects anger and identifies household speakers from a single MFCC front-end, deployed on an ESP32-S3-N16R8 microcontroller for domestic violence early-warning.**

[![Python](https://img.shields.io/badge/Python-3.10%2B-blue?logo=python&logoColor=white)](https://www.python.org/)
[![TensorFlow](https://img.shields.io/badge/TensorFlow-2.20-FF6F00?logo=tensorflow&logoColor=white)](https://www.tensorflow.org/)
[![TFLite](https://img.shields.io/badge/TFLite-INT8-orange)](https://www.tensorflow.org/lite)
[![Platform](https://img.shields.io/badge/Platform-ESP32--S3-green)](https://www.espressif.com/en/products/socs/esp32-s3)
[![License](https://img.shields.io/badge/License-MIT-lightgrey)](LICENSE)

---

## Abstract

Traditional voice-based emotion recognition systems run on computers or mobile devices, raising deployment cost and discarding speaker identity — limitations that prevent their adoption in the resource-constrained households where early-warning monitoring would be most beneficial. This repository contains the full training, evaluation, quantization, profiling and field-validation code for a **compact hybrid framework** that simultaneously extracts affective and identity-related cues from a single Mel-Frequency Cepstral Coefficient (MFCC) front-end.

Two complementary INT8 branches share the same **20 × 63 MFCC matrix**:

- **MicroLightCNN** — LightCNN-derived convolutional network for binary anger detection (**83.2 % INT8 accuracy, F1 = 82.8, AUC = 90.5, 59.7 KB arena, 19,409 params**).
- **MLP-XiEmbedding** — multilayer perceptron operating on a deterministic four-statistic Xi-Vector pooling (µ, σ, max, min over 20 Mel bands) that identifies up to ten enrolled household speakers (**98.6 % top-1 accuracy, EER = 0.014, 21.6 KB arena, 21,604 params**).

The combined tensor-arena footprint of **81.3 KB** fits within the 8 MB PSRAM of the ESP32-S3-N16R8. The pipeline produces contextual alerts of the form **"Dad angry — 63 %"** in under 500 ms per 2-second capture window (excluding the I2S capture itself, which overlaps with the next cycle).

A post-deployment field validation in a real domestic environment with 256 measurements across 8 radial lines × 4 distances × 4 speakers × 2 phrase types yields **94.5 % anger accuracy, 85.9 % speaker accuracy and 82.8 % joint accuracy**.

> **Paper:** Mitre-Hernandez H., Lara-Alvarez C., E. Lopez-Ortega *"Compact Hybrid Model for Anger and Speaker Recognition through Voice Analysis Implemented on an ESP32 Microcontroller."* Under review, **Expert Systems with Applications** (Elsevier, JCR Q1, IF = 7.5, ISSN 0957-4174), 2026. (IN REVIEW)

---

## Repository Structure

```
.
├── models/                              # Model definitions (Keras / TF)
│   ├── __init__.py
│   ├── micro_lightcnn.py                # MicroLightCNN — anger detection (Table III)
│   ├── ds_cnn.py                        # DS-CNN baseline — anger detection
│   ├── matchboxnet.py                   # MatchboxNet baseline — anger detection
│   ├── mlp_xi_embedding.py              # MLP-XiEmbedding — speaker ID (Table IV)
│   ├── ecapa_tdnn_pruned.py             # ECAPA-TDNN pruned baseline — speaker ID
│   └── campp_pruned.py                  # CAM++ pruned baseline — speaker ID
│
├── utils/                               # Shared utilities
│   ├── __init__.py
│   ├── mfcc.py                          # MFCC front-end (matches ESP32 firmware exactly)
│   ├── datasets.py                      # ESD and dataset_fam loaders + augmentation
│   ├── metrics.py                       # Anger (Acc/F1/AUC) and speaker (EER/rejection) metrics
│   └── tflite_tools.py                  # TFLite conversion, INT8 quantization, arena measurement
│
├── experiments/                         # Reproducible experiment scripts
│   ├── __init__.py
│   ├── run_anger.py                     # Train & evaluate anger models → Table III
│   ├── run_speaker.py                   # Train & evaluate speaker models → Table IV
│   ├── run_quant_ablation.py            # Quantization ablation (F32/INT8 × F32/INT8) → Table V
│   ├── profile_pipeline.py              # End-to-end latency profiling → Table VI
│   ├── generate_anger_figures.py        # Confusion matrix + ROC curve → Figure 2
│   ├── generate_joint_detection_figure.py  # Polar heatmaps → Figure 3
│   └── build_all_tables.py              # Aggregate Tables III–VI into LaTeX + JSON
│
├── dataset_fam/                         # In-house Mexican-Spanish household corpus (INCLUDED)
│   ├── papa/                            # 5 WAV recordings (gustos, molestia, rutina, saludo, vacaciones)
│   ├── mama/                            # 5 WAV recordings (same five thematic phrases)
│   ├── hijo/                            # 5 WAV recordings (hijo_1_*.wav)
│   └── hija/                            # 5 WAV recordings (hija_1_*.wav)
│
├── electrical_diagram/                  # Hardware schematic (KiCad 8)
│   ├── ED_ESP32-S3.pdf                  # Rendered schematic of the ESP32-S3 + I2S MEMS mic board
│   └── ElectricalDiagram.kicad_sch      # Editable KiCad source
│
├── outputs/                             # Pre-computed artifacts (auto-overwritten by scripts)
│   ├── anger/                           # 3 anger models × {F32, INT8} TFLite + per-model JSON
│   │   ├── microlightcnn_{f32,int8}.tflite
│   │   ├── ds-cnn_{f32,int8}.tflite
│   │   ├── matchboxnet_{f32,int8}.tflite
│   │   ├── *.json                       # Per-model metrics
│   │   ├── table_iii.json               # Aggregated Table III
│   │   └── table_iii.tex                # LaTeX-ready Table III
│   ├── speaker/                         # 3 speaker models × {F32, INT8} TFLite + StandardScaler
│   │   ├── mlp_xiembedding_{f32,int8}.tflite
│   │   ├── mlp_xiembedding_xi_mean.npy  # StandardScaler μ (firmware-ready)
│   │   ├── mlp_xiembedding_xi_std.npy   # StandardScaler σ (firmware-ready)
│   │   ├── ecapa_tdnn_pruned_{f32,int8}.tflite
│   │   ├── campp_{f32,int8}.tflite
│   │   ├── *.json
│   │   ├── table_iv.json
│   │   └── table_iv.tex
│   ├── quant/                           # Hybrid quantization ablation → Table V
│   │   ├── table_v.json
│   │   └── table_v.tex
│   ├── pipeline/                        # End-to-end profiling → Table VI
│   │   ├── table_vi.json
│   │   └── table_vi.tex
│   ├── paper_tables/                    # Aggregated LaTeX bundle
│   │   ├── all_tables.json
│   │   └── all_tables.tex               # `\input{}`-ready
│   ├── figures/                         # IEEE-ready PDF figures
│   │   ├── fig_anger_confusion_matrix.pdf
│   │   ├── fig_anger_roc_curve.pdf
│   │   ├── fig_joint_anger_polar.pdf    # Field validation heatmaps
│   │   ├── fig_joint_speaker_polar.pdf
│   │   ├── fig_joint_combined_polar.pdf
│   │   ├── joint_detection_stats.json   # Aggregated stats for paper caption
│   │   └── validacion.md                # Post-deployment validation report (Spanish)
│   ├── joint_detection_results.csv      # 256-measurement field validation dataset
│   ├── generate_joint_detection_figure.py  # Convenience wrapper (no flags needed)
│   ├── generate_joint_detection_figure.md  # Notes on the figure-generation pipeline
│   └── *.log                            # Training logs (anger, speaker, quant, pipeline)
│
└── ESD/                                 # Emotional Speech Dataset (NOT included — see below)
    └── Emotional_Speech_Dataset/
        ├── Chinese/{Angry,Happy,Neutral,Sad,Surprise}/{train,test,evaluation}/*.wav
        └── English/{Angry,Happy,Neutral,Sad,Surprise}/{train,test,evaluation}/*.wav
```

---

## Datasets

### Emotional Speech Dataset (ESD) — external

The anger branch is trained and evaluated on the public [ESD corpus](https://github.com/HLTSingapore/Emotional-Speech-Data) (Zhou et al., 2022): parallel emotional utterances in English and Mandarin Chinese from 20 native speakers across five emotion categories.

```
Angry → label 1   (positive class)
Happy, Neutral, Sad, Surprise → label 0   (balanced to match Angry count)

Official splits:  12,000 train · 800 validation · 1,200 test utterances
```

ESD is **not redistributed in this repository**. Download it from the original source and pass its root via the `--esd-root` flag to any anger experiment.

**Expected layout:**

```
ESD/Emotional_Speech_Dataset/
  English/
    Angry/
      train/      0011_000001.wav  ...
      test/       0011_000351.wav  ...
      evaluation/ 0011_000401.wav  ...
    Happy/  Neutral/  Sad/  Surprise/   (same structure)
  Chinese/
    Angry/  Happy/  Neutral/  Sad/  Surprise/
```

### In-house Household Corpus (`dataset_fam/`) — included

The speaker branch uses an in-house Mexican-Spanish corpus of **four enrolled speakers** (`papa`, `mama`, `hijo`, `hija`), recorded with an SPH0645 I2S MEMS microphone at 16 kHz. Each speaker contributed five thematic phrases (~25–35 s each, named `<speaker>_{gustos,molestia,rutina,saludo,vacaciones}.wav`), segmented into 2-second windows with a 0.5-second hop, yielding **~737 clean segments** (590 train / 147 validation after per-source hold-out). Training is augmented to **~1,770 segments** via additive white noise (SNR ∈ {30, 25, 20} dB), hard loud perturbation (9–17 dB + dynamic-range compression) and moderate loud perturbation (6–13 dB).

> **Privacy note:** The 20 WAV files shipped here belong to consenting members of the principal investigator's household. They are released only as a **reproducibility seed** for the speaker-identification experiments. To run the system with your own household, replace the contents of `dataset_fam/<speaker_name>/` with one folder per speaker and any number of WAV files per folder, then pass `--dataset-dir dataset_fam` to `run_speaker.py`.

### Field-validation Dataset (`outputs/joint_detection_results.csv`) — included

A spreadsheet with **256 on-device measurements** of the deployed system in a real domestic environment, following the protocol of `outputs/figures/validacion.md`:

```
8 radial lines × 4 distances × 4 speakers × 2 phrase types = 256 binary measurements
distances ∈ {50, 100, 150, 200} cm
angles ∈ {0°, 45°, 90°, 135°, 180°, 225°, 270°, 315°}
```

Columns: `line_id, distance_cm, angle_deg, speaker, predicted_speaker, phrase_type, anger_detected, speaker_correct, notes`.

---

## Models

### Anger Detection Branch — Table III

| Model | Params | INT8 Acc. | INT8 F1 | INT8 AUC | Arena (KB) | Flash (KB) |
|---|---:|---:|---:|---:|---:|---:|
| **MicroLightCNN (ours)** | **19,409** | **83.2 %** | **82.8** | **90.5** | **59.7** | **28.8** |
| DS-CNN (Zhang et al., 2017) | 5,153 | 83.1 % | 84.1 | 89.3 | 81.2 | 16.5 |
| MatchboxNet (Majumdar et al., 2020) | 18,241 | 82.2 % | 82.2 | 85.4 | 94.7 | 44.1 |

**MicroLightCNN architecture** — Input `[1, 20, 63, 1]` → 3 × (Conv2D 3×3 · 16f · BN · ReLU · MaxPool 2×2/2) → Flatten + L2-norm (224-d) → Dense(64) + Dropout(0.50) → Sigmoid. INT8 quantization is Pareto-dominant: it *improves* accuracy by 0.3 pp over Float32 thanks to the regularizing effect of BatchNorm folding.

### Speaker Identification Branch — Table IV

| Model | Params | Top-1 (INT8) | EER | Rej. Rate | Arena (KB) |
|---|---:|---:|---:|---:|---:|
| **MLP-XiEmbedding (ours)** | **21,604** | **98.6 %** | **0.014** | **6.1 %** | **21.6** |
| ECAPA-TDNN pruned (Desplanques et al., 2020) | 26,208 | 99.3 % | 0.008 | 2.0 % | 132.7 |
| CAM++ pruned (Wang et al., 2023) | 23,214 | 99.3 % | 0.009 | 5.4 % | 100.2 |

**MLP-XiEmbedding architecture** — Xi-Vector pooling [µ, σ, max, min] over 20 Mel bands → 80-d vector → StandardScaler (offline) → Dense(128, BN, Dropout 0.35) → Dense(64, BN, Dropout 0.30) ← *speaker embedding* → Dense(32, Dropout 0.20) → Dense(N) + Softmax. The deterministic Xi-Vector pooling shields the MLP from INT8 rounding error: speaker accuracy moves by < 0.01 pp between Float32 and INT8 while the arena drops from 82.6 KB to 21.6 KB.

### Quantization Ablation — Table V

| Anger precision | Speaker precision | Anger Acc. | Speaker Top-1 | Total Arena |
|---|---|---:|---:|---:|
| Float32 | Float32 | 82.8 % | 98.6 % | 293.5 KB |
| Float32 | INT8 | 82.8 % | 98.6 % | 232.5 KB |
| INT8 | Float32 | 83.2 % | 98.6 % | 142.3 KB |
| **INT8** | **INT8** | **83.2 %** | **98.6 %** | **81.3 KB** |

The fully-INT8 corner is Pareto-dominant: it has the smallest arena and the best anger accuracy (the INT8 anger model marginally outperforms its Float32 counterpart due to BN-folding regularization), with no measurable degradation on the speaker side.

---

## Getting Started

### Requirements

```bash
Python >= 3.10
TensorFlow >= 2.20
```

```bash
pip install tensorflow soundfile scipy scikit-learn matplotlib pandas
```

### Installation

```bash
git clone https://github.com/<your-org>/<repo-name>.git
cd <repo-name>
pip install -r requirements.txt   # or install manually as above
```

### Quick test (no dataset needed)

```python
from models import build_micro_lightcnn, build_mlp_xi_embedding
from utils.mfcc import compute_mfcc, xi_vector
import numpy as np

# Anger branch
anger_model = build_micro_lightcnn()
anger_model.summary()             # 19,409 trainable params

# Speaker branch (4 enrolled speakers)
speaker_model = build_mlp_xi_embedding(num_classes=4)
speaker_model.summary()           # 21,604 trainable params

# MFCC front-end
dummy_audio = np.random.randn(32000).astype(np.float32)   # 2 s @ 16 kHz
mfcc = compute_mfcc(dummy_audio)  # shape: (20, 63)
xi   = xi_vector(mfcc)            # shape: (80,)
print(f"MFCC: {mfcc.shape}  Xi-Vector: {xi.shape}")
```

---

## Reproducing the Paper Tables

All scripts are run as Python modules from the repository root. Replace paths as needed. Pre-computed outputs are already shipped in `outputs/` — running the scripts will overwrite them.

### Table III — Anger Detection Results

```bash
python -m experiments.run_anger \
    --esd-root    ESD/Emotional_Speech_Dataset \
    --output-dir  outputs/anger \
    --epochs      60 \
    --batch-size  32 \
    --seed        42
```

**Outputs:** `outputs/anger/{microlightcnn,ds-cnn,matchboxnet}_{f32,int8}.tflite`, per-model `*.json`, `table_iii.json`, `table_iii.tex`.

### Table IV — Speaker Identification Results

```bash
python -m experiments.run_speaker \
    --dataset-dir dataset_fam \
    --output-dir  outputs/speaker \
    --epochs      100 \
    --batch-size  64 \
    --seed        42
```

**Outputs:** `outputs/speaker/{mlp_xiembedding,ecapa_tdnn_pruned,campp}_{f32,int8}.tflite`, the StandardScaler arrays `mlp_xiembedding_xi_{mean,std}.npy`, per-model `*.json`, `table_iv.json`, `table_iv.tex`.

### Table V — Quantization Ablation

Requires Table III and Table IV outputs to exist first.

```bash
python -m experiments.run_quant_ablation \
    --esd-root    ESD/Emotional_Speech_Dataset \
    --dataset-dir dataset_fam \
    --anger-dir   outputs/anger \
    --speaker-dir outputs/speaker \
    --output-dir  outputs/quant
```

**Outputs:** `outputs/quant/table_v.json`, `outputs/quant/table_v.tex`.

### Table VI — End-to-End Pipeline Profiling

```bash
python -m experiments.profile_pipeline \
    --serial-log     salidasEnSerialMonitor.txt \
    --anger-tflite   outputs/anger/microlightcnn_f32.tflite \
    --speaker-tflite outputs/speaker/mlp_xiembedding_int8.tflite \
    --output-dir     outputs/pipeline
```

If `--serial-log` is omitted or the log contains no matching entries, the script imputes device-side latencies from host-side TFLite measurements while keeping the total fixed at the observed value. **Outputs:** `outputs/pipeline/table_vi.json`, `outputs/pipeline/table_vi.tex`.

### Figures

```bash
# Figure 2 — Confusion matrix and ROC curve (MicroLightCNN INT8)
python -m experiments.generate_anger_figures \
    --esd-root   ESD/Emotional_Speech_Dataset \
    --anger-dir  outputs/anger \
    --output-dir outputs/figures \
    --threshold  0.5

# Figure 3 — Polar heatmaps of joint detection rate
python -m experiments.generate_joint_detection_figure \
    --csv        outputs/joint_detection_results.csv \
    --output-dir outputs/figures

# Convenience wrapper (no flags needed — uses default paths under outputs/)
python outputs/generate_joint_detection_figure.py
```

### Aggregate LaTeX bundle

```bash
python -m experiments.build_all_tables \
    --anger-dir    outputs/anger \
    --speaker-dir  outputs/speaker \
    --quant-dir    outputs/quant \
    --pipeline-dir outputs/pipeline \
    --output-dir   outputs/paper_tables
```

**Outputs:** `outputs/paper_tables/all_tables.tex` (ready for `\input{}` in the paper), `outputs/paper_tables/all_tables.json`.

---

## Pipeline Architecture

```
I2S Audio Capture  (SPH0645 / INMP441 · 16 kHz · 2 s window)
        │
        ▼
   MFCC Front-End  (20 × 63 · shared buffer)
        │
   ┌────┴──────────────────────┐
   │ Branch A — Anger          │ Branch B — Speaker
   │                           │
   │  MicroLightCNN            │  Xi-Vector pooling
   │  INT8 · 59.7 KB arena     │  [µ, σ, max, min] · 80-d
   │  anger posterior p_em     │        │
   │                           │  StandardScaler (offline · 640 B)
   │                           │        │
   │                           │  MLP-XiEmbedding
   │                           │  INT8 · 21.6 KB arena
   │                           │  speaker posterior p_sp ∈ [0,1]^N
   └────────────┬──────────────┘
                ▼
        Decision Fusion
        (τ_sp = 0.60 · δ = 0.20)
                │
        Temporal Voting  (k = 3 frames)
                │
        Contextual Alert
        "Dad angry — 63 %"
```

**End-to-end latency** (ESP32-S3-N16R8 · dual-core Xtensa LX7 @ 240 MHz):

| Stage | Time (ms) | Source |
|---|---:|---|
| I2S audio capture (2 s window, non-additive) | 2,000.0 | informational |
| MFCC extraction | 190.0 | device (or imputed) |
| MicroLightCNN inference (F32) | 219.0 | device† |
| Xi-Vector pooling + StandardScaler | 1.0 | imputed |
| MLP-XiEmbedding inference (INT8) | 55.0 | imputed |
| Decision fusion + temporal voting | 0.5 | imputed |
| **End-to-end (processing)** | **465.5** | — |

† Float32 reference from Fernandez-Morales et al. (2025). The 2-s I2S window overlaps with the next inference cycle and is excluded from the processing total.

**Memory footprint** (INT8 / INT8 configuration):

| Component | Size |
|---|---:|
| MicroLightCNN arena (PSRAM) | 59.7 KB |
| MLP-XiEmbedding arena (PSRAM) | 21.6 KB |
| StandardScaler table (flash) | 640 B |
| **Total tensor-arena footprint** | **81.3 KB** |
| Internal SRAM free at runtime | ≥ 229 KB |

---

## Post-Deployment Field Validation

The repository ships a full post-deployment validation campaign carried out with the system already flashed on the ESP32-S3-N16R8 in a real domestic setting. Unlike the notebook evaluations of Tables III–V, the validation measures the **entire pipeline in vivo**: I2S microphone capture, VAD, MFCC, Xi-Vector pooling, both TFLite Micro inferences, decision fusion and temporal voting.

**Protocol** (see `outputs/figures/validacion.md`):

```
256 measurements = 8 radial lines × 4 distances × 4 speakers × 2 phrase types
distances: 50, 100, 150, 200 cm
angles:    0°, 45°, 90°, 135°, 180°, 225°, 270°, 315°
phrase types: {neutral, angry}
```

**Global results (256 samples):**

| Metric | Result |
|---|---:|
| `anger_accuracy` | 94.53 % |
| `speaker_accuracy` | 85.94 % |
| `joint_accuracy` (both correct in the same frame) | 82.81 % |
| `speaker_error_rate` | 14.06 % |
| `anger_false_positive_rate_neutral` | 2.34 % |
| `anger_miss_rate_angry` | 8.59 % |

**By distance:**

| Distance | Anger Acc. | Speaker Acc. | Joint Acc. |
|---:|---:|---:|---:|
| 50 cm | 98.4 % | 96.9 % | 96.9 % |
| 100 cm | 93.8 % | 92.2 % | 87.5 % |
| 150 cm | 93.8 % | 89.1 % | 84.4 % |
| 200 cm | 92.2 % | 65.6 % | 62.5 % |

Anger detection degrades gracefully with distance; speaker identification is more sensitive to distance, ambient noise and orientation. The full per-line, per-point and confusion-matrix breakdown is in `outputs/figures/joint_detection_stats.json`, and the polar heatmaps in `outputs/figures/fig_joint_{anger,speaker,combined}_polar.pdf`.

---

## Hardware

The ESP32-S3 + I2S MEMS microphone reference board is documented in `electrical_diagram/`:

- `ED_ESP32-S3.pdf` — rendered schematic ready for inspection.
- `ElectricalDiagram.kicad_sch` — editable KiCad 8 source for adapting the board to your enclosure.

**Bill of materials (minimum):**

- ESP32-S3-N16R8 (dual-core Xtensa LX7 @ 240 MHz, 16 MB flash, 8 MB PSRAM, 512 KB internal SRAM)
- SPH0645 (validation) or INMP441 (recommended) I2S MEMS microphone @ 16 kHz / 16-bit
- Standard USB-C power supply

---

## Firmware Integration

After training, export each model to a C header for inclusion in the ESP32 firmware:

```python
from utils.tflite_tools import write_c_header
from pathlib import Path

anger_bytes   = Path("outputs/anger/microlightcnn_int8.tflite").read_bytes()
speaker_bytes = Path("outputs/speaker/mlp_xiembedding_int8.tflite").read_bytes()

write_c_header(anger_bytes,   "firmware/micro_lightcnn_model.h",   "micro_lightcnn_model")
write_c_header(speaker_bytes, "firmware/mlp_xiembedding_model.h", "mlp_xiembedding_model")
```

The generated headers follow the format used by `tflite::GetModel()` in TFLite Micro. Include them in your `main.cpp` along with the StandardScaler parameters compiled from `outputs/speaker/mlp_xiembedding_xi_mean.npy` and `mlp_xiembedding_xi_std.npy` (640 B total).

---

## MFCC Configuration

The Python front-end in `utils/mfcc.py` matches the firmware `MFCC.cpp` exactly:

| Parameter | Value |
|---|---:|
| Sampling rate | 16,000 Hz |
| Window length | 512 samples (32 ms) |
| Hop length | 256 samples (16 ms, 50 % overlap) |
| Mel bands | 20 |
| Frequency range | 20 Hz – 8,000 Hz |
| Output shape | (20, 63) |
| Segment duration | 2 seconds |
| Energy threshold | 0.0025 (silent segment skip) |
| Xi-Vector dimension | 80 = 4 × 20 |
| Pre-emphasis | y[i] = x[i] − 0.97 · x[i − 1] |
| Window function | Hamming (α = 0.53836) |

---

## Citation

If you use this code or models in your research, please cite:

```bibtex
@article{mitre2026compact,
  title   = {Compact Hybrid Model for Anger and Speaker Recognition
             through Voice Analysis Implemented on an {ESP32} Microcontroller},
  author  = {Mitre-Hernandez, Hugo and Lara-Alvarez, Carlos and {Third Author}},
  journal = {Expert Systems with Applications},
  year    = {2026},
  note    = {Under review},
  issn    = {0957-4174}
}
```

Prior work on which this framework builds:

```bibtex
@article{fernandez2025compact,
  title   = {Compact Near-Real-Time Anger Detection System through Voice Analysis
             Implemented on {ESP32} Microcontroller},
  author  = {Fernandez-Morales, Ivan and Mitre-Hernandez, Hugo and
             Lara-Alvarez, Carlos and De-La-Torre-Gutierrez, Humberto and
             Jaramillo-Avila, Ulises},
  journal = {IEEE Transactions on Instrumentation and Measurement},
  volume  = {74},
  pages   = {1--15},
  year    = {2025},
  doi     = {10.1109/TIM.2025.3574903}
}
```

---

## Data Availability

- **ESD corpus:** Publicly available at [https://github.com/HLTSingapore/Emotional-Speech-Data](https://github.com/HLTSingapore/Emotional-Speech-Data).
- **In-house household corpus (`dataset_fam/`):** A reproducibility seed of 20 WAV files (4 speakers × 5 phrases) is included in this repository under informed consent. Extending the experiment to additional households requires re-recording.
- **Field-validation CSV (`outputs/joint_detection_results.csv`):** Anonymized — only line/distance/angle/speaker label/phrase type and binary correctness flags are stored, no audio.
- **Model weights and TFLite binaries:** Pre-built artifacts shipped under `outputs/anger/`, `outputs/speaker/`, `outputs/quant/`, `outputs/pipeline/`, `outputs/paper_tables/` and `outputs/figures/`.

---

## Acknowledgements

The authors thank the household members who provided voice recordings for the in-house speaker corpus and participated in the field-validation protocol. This work was carried out at **Centro de Investigación en Matemáticas (CIMAT)**, Quantum Ciudad del Conocimiento, Zacatecas, Mexico.

---

## License

This project is licensed under the MIT License — see [LICENSE](LICENSE) for details.
