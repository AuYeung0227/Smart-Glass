"""
MatchboxNet baseline for anger detection — Table I, right column.

Reference: Majumdar et al., "MatchboxNet: 3x1x1 ConvNet for keyword spotting",
Interspeech 2020. NVIDIA NeMo implementation.

MatchboxNet processes MFCC frames *along the time axis* with time-channel
separable Conv1Ds and residual 1x1 projections. Input is transposed from
(20, 63) to (63, 20) so that the time axis is the convolution axis.

Pipeline (input shape [1, 63, 20]):
    Stem    : Conv1D k=11, 32 filters, BN, ReLU
    Block 1 : SepConv1D k=13, 32f (x2) + residual 1x1
    Block 2 : SepConv1D k=15, 32f (x2) + residual 1x1
    Block 3 : Conv1D k=1, 64f, BN, ReLU
    Pooling : GlobalAvgPool1D (64)
    Head    : Dense(1, Sigmoid)

Scaled to ~17 k parameters to match the paper's budget.
"""
from __future__ import annotations

import tensorflow as tf
from tensorflow.keras import layers

from utils.mfcc import MEL_BANDS, NUM_FRAMES


def _sep_conv1d(x, filters: int, kernel_size: int, name: str):
    """Time-channel separable Conv1D = DepthwiseConv1D + PointwiseConv1D."""
    # TF Keras has no native DepthwiseConv1D, simulate with groups equal to input channels.
    in_ch = x.shape[-1]
    x = layers.Conv1D(in_ch, kernel_size, padding="same", groups=in_ch,
                      use_bias=False, name=f"{name}_dw")(x)
    x = layers.Conv1D(filters, 1, padding="same", use_bias=False,
                      name=f"{name}_pw")(x)
    x = layers.BatchNormalization(name=f"{name}_bn")(x)
    x = layers.ReLU(name=f"{name}_relu")(x)
    return x


def _matchbox_block(x, filters: int, kernel_size: int, n_sep: int, name: str):
    """Repeated sep-Conv1D with a residual 1x1 projection."""
    residual = layers.Conv1D(filters, 1, padding="same", use_bias=False,
                             name=f"{name}_res")(x)
    residual = layers.BatchNormalization(name=f"{name}_res_bn")(residual)

    for i in range(n_sep):
        x = _sep_conv1d(x, filters, kernel_size, name=f"{name}_sep{i + 1}")

    x = layers.Add(name=f"{name}_add")([x, residual])
    x = layers.ReLU(name=f"{name}_out_relu")(x)
    return x


def build_matchboxnet(input_shape: tuple[int, int] = (NUM_FRAMES, MEL_BANDS),
                     stem_filters: int = 32,
                     block_filters: int = 32,
                     head_filters: int = 64,
                     ) -> tf.keras.Model:
    inputs = tf.keras.Input(shape=input_shape, name="mfcc_t20")

    # Stem.
    x = layers.Conv1D(stem_filters, 11, padding="same", use_bias=False,
                      name="stem")(inputs)
    x = layers.BatchNormalization(name="stem_bn")(x)
    x = layers.ReLU(name="stem_relu")(x)

    # Two MatchboxNet blocks.
    x = _matchbox_block(x, block_filters, kernel_size=13, n_sep=2, name="b1")
    x = _matchbox_block(x, block_filters, kernel_size=15, n_sep=2, name="b2")

    # Head 1x1 + GAP.
    x = layers.Conv1D(head_filters, 1, padding="same", use_bias=False,
                      name="head_conv")(x)
    x = layers.BatchNormalization(name="head_bn")(x)
    x = layers.ReLU(name="head_relu")(x)
    x = layers.GlobalAveragePooling1D(name="gap")(x)

    outputs = layers.Dense(1, activation="sigmoid", name="anger")(x)
    model = tf.keras.Model(inputs, outputs, name="MatchboxNet")
    model.compile(
        optimizer=tf.keras.optimizers.Adam(learning_rate=1e-3),
        loss="binary_crossentropy",
        metrics=["accuracy"],
    )
    return model


def transpose_for_matchboxnet(X: "np.ndarray"):
    """Convert (N, 20, 63) MFCC arrays to (N, 63, 20) MatchboxNet input."""
    import numpy as np
    if X.ndim == 4 and X.shape[-1] == 1:
        X = X[..., 0]
    if X.shape[-1] == MEL_BANDS:        # already (N, 63, 20)
        return X.astype(np.float32)
    return np.transpose(X, (0, 2, 1)).astype(np.float32)


if __name__ == "__main__":
    m = build_matchboxnet()
    m.summary()
    print("Trainable params:", m.count_params())
