"""
TFLite conversion + arena-size estimation.

The "arena" reported in Tables III, IV and V is the *tensor arena* that
TFLite Micro allocates on the ESP32. We approximate it on the host with
the canonical `interpreter._get_ops_details()` / `arena_used_bytes` API
introduced in TF 2.10, falling back to a manual upper bound based on the
largest intermediate activation when that API is not available (so the
script also works on TF 2.8/2.9 used by ESP-IDF examples).
"""
from __future__ import annotations

from pathlib import Path
from typing import Callable, Iterable

import numpy as np
import tensorflow as tf


# ---------------------------------------------------------------------------
# Converters
# ---------------------------------------------------------------------------
def to_tflite_float32(model: tf.keras.Model) -> bytes:
    """Plain Float32 TFLite (no optimizations)."""
    conv = tf.lite.TFLiteConverter.from_keras_model(model)
    return conv.convert()


def to_tflite_int8(model: tf.keras.Model,
                   representative_data: np.ndarray,
                   n_calibration: int = 100,
                   ) -> bytes:
    """
    Post-training INT8 quantization with a representative calibration set
    (same protocol as `Train_xvector_robusto/xvR.py`).
    """
    rep = representative_data[:n_calibration].astype(np.float32)

    def representative_dataset():
        for i in range(len(rep)):
            yield [rep[i : i + 1]]

    conv = tf.lite.TFLiteConverter.from_keras_model(model)
    conv.optimizations = [tf.lite.Optimize.DEFAULT]
    conv.representative_dataset = representative_dataset
    conv.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
    # Keep input/output in float for easy host-side evaluation; the on-device
    # model is still INT8 internally.
    return conv.convert()


# ---------------------------------------------------------------------------
# Arena measurement
# ---------------------------------------------------------------------------
def measure_arena_kb(tflite_bytes: bytes) -> float:
    """
    Return a conservative arena size in KB by querying the interpreter
    once `allocate_tensors()` has been called.
    """
    interp = tf.lite.Interpreter(model_content=tflite_bytes)
    interp.allocate_tensors()

    # Preferred API (TF >= 2.10).
    used = None
    try:
        used = interp.arena_used_bytes()                # type: ignore[attr-defined]
    except Exception:
        pass

    # Fallback: sum of all intermediate tensor byte sizes.
    if not used:
        used = 0
        for d in interp.get_tensor_details():
            shape = d["shape"]
            if shape.size == 0:
                continue
            dtype = np.dtype(d["dtype"])
            used += int(np.prod(shape)) * dtype.itemsize

    return round(used / 1024, 1)


def measure_flash_kb(tflite_bytes: bytes) -> float:
    """Flash footprint = TFLite flatbuffer size."""
    return round(len(tflite_bytes) / 1024, 1)


# ---------------------------------------------------------------------------
# Latency micro-benchmark on the host (NOT the on-device latency; only a
# sanity check). On-device numbers are produced by `profile_pipeline.py`.
# ---------------------------------------------------------------------------
def latency_host_ms(tflite_bytes: bytes,
                    sample: np.ndarray,
                    n_runs: int = 50) -> float:
    interp = tf.lite.Interpreter(model_content=tflite_bytes)
    interp.allocate_tensors()
    in_det  = interp.get_input_details()[0]
    out_det = interp.get_output_details()[0]

    x = sample.astype(in_det["dtype"]).reshape(in_det["shape"])
    # Warm up.
    for _ in range(3):
        interp.set_tensor(in_det["index"], x)
        interp.invoke()

    import time
    times = []
    for _ in range(n_runs):
        t0 = time.perf_counter()
        interp.set_tensor(in_det["index"], x)
        interp.invoke()
        _ = interp.get_tensor(out_det["index"])
        times.append((time.perf_counter() - t0) * 1000.0)
    return float(np.mean(times))


# ---------------------------------------------------------------------------
# Run inference on the entire dataset through a TFLite interpreter.
# ---------------------------------------------------------------------------
def predict_tflite(tflite_bytes: bytes, X: np.ndarray) -> np.ndarray:
    """
    Returns the raw output tensor over all samples.

    Notes
    -----
    - If the input tensor is quantized, the input is quantized on the fly
      using the input quantization parameters; output is dequantized.
    - Sigmoid/softmax is preserved as in the original model.
    """
    interp = tf.lite.Interpreter(model_content=tflite_bytes)
    interp.allocate_tensors()
    in_det  = interp.get_input_details()[0]
    out_det = interp.get_output_details()[0]

    in_shape = in_det["shape"]
    n = X.shape[0]
    outs: list[np.ndarray] = []

    for i in range(n):
        x = X[i : i + 1].astype(np.float32).reshape(in_shape)
        if in_det["dtype"] == np.int8:
            scale, zp = in_det["quantization"]
            x_q = np.round(x / scale + zp).astype(np.int8)
            interp.set_tensor(in_det["index"], x_q)
        else:
            interp.set_tensor(in_det["index"], x.astype(in_det["dtype"]))
        interp.invoke()
        y = interp.get_tensor(out_det["index"]).copy()
        if out_det["dtype"] == np.int8:
            scale, zp = out_det["quantization"]
            y = (y.astype(np.float32) - zp) * scale
        outs.append(y)
    return np.concatenate(outs, axis=0)


# ---------------------------------------------------------------------------
# Export to firmware-ready C header (same format as xvR.py).
# ---------------------------------------------------------------------------
def write_c_header(tflite_bytes: bytes, out_path: str | Path, varname: str) -> None:
    out_path = Path(out_path)
    guard = varname.upper() + "_H"
    lines = [f"#ifndef {guard}", f"#define {guard}", "",
             f"const unsigned int {varname}_len = {len(tflite_bytes)};", "",
             f"const unsigned char {varname}[] = {{"]
    for i in range(0, len(tflite_bytes), 12):
        chunk = tflite_bytes[i : i + 12]
        lines.append("    " + ", ".join(f"0x{b:02x}" for b in chunk) + ",")
    lines += ["};", "", "#endif", ""]
    out_path.write_text("\n".join(lines), encoding="utf-8")
