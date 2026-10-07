"""
ECAPA-TDNN pruned baseline for speaker identification — Table II, middle column.

Reference: Desplanques et al., "ECAPA-TDNN: Emphasized Channel Attention,
Propagation and Aggregation in TDNN Based Speaker Verification",
Interspeech 2020.

Canonical signature preserved (matched-parameter-budget protocol, <30 k):
    - SE-Res2Blocks with `scale=4` and dilations {2, 3}
    - Multi-Layer feature Aggregation (MLA) by channel concatenation
    - Attentive Statistics Pooling (ASP)

Channels are pruned from the canonical 512 down to 32 to fit the ESP32-S3
INT8 envelope (~24,800 params).

Input shape : [1, 63, 20]    (frame axis x Mel features)
Output      : [1, N]         softmax over enrolled speakers
"""
from __future__ import annotations

import tensorflow as tf
from tensorflow.keras import layers

from utils.mfcc import MEL_BANDS, NUM_FRAMES


# ---------------------------------------------------------------------------
# SE-Res2Block (Res2Net + Squeeze-and-Excitation), pruned
# ---------------------------------------------------------------------------
def _se_block(x, reduction: int = 4, name: str = "se"):
    c = x.shape[-1]
    s = layers.GlobalAveragePooling1D(name=f"{name}_squeeze")(x)
    s = layers.Dense(max(2, c // reduction), activation="relu",
                     name=f"{name}_fc1")(s)
    s = layers.Dense(c, activation="sigmoid", name=f"{name}_fc2")(s)
    s = layers.Reshape((1, c), name=f"{name}_reshape")(s)
    return layers.Multiply(name=f"{name}_scale")([x, s])


def _res2_block(x, filters: int, kernel_size: int, scale: int, dilation: int,
                name: str):
    """Single SE-Res2Block. Splits the channel dim into `scale` chunks and
    applies dilated convolutions in a hierarchical fashion before SE."""
    assert filters % scale == 0, "filters must be divisible by scale"
    chunk = filters // scale
    residual = x

    # Expand to `filters` channels.
    x = layers.Conv1D(filters, 1, padding="same", use_bias=False,
                      name=f"{name}_expand")(x)
    x = layers.BatchNormalization(name=f"{name}_expand_bn")(x)
    x = layers.ReLU(name=f"{name}_expand_relu")(x)

    # Split, dilated convs, hierarchical sum (Res2Net) using Keras ops.
    splits = [layers.Lambda(lambda t, i=i: t[..., i * chunk:(i + 1) * chunk],
                            name=f"{name}_split{i}")(x)
              for i in range(scale)]
    new_splits = [splits[0]]
    for i in range(1, scale):
        if i == 1:
            h = splits[i]
        else:
            h = layers.Add(name=f"{name}_add{i}")([splits[i], new_splits[-1]])
        h = layers.Conv1D(chunk, kernel_size, padding="same",
                          dilation_rate=dilation, use_bias=False,
                          name=f"{name}_dconv{i}")(h)
        h = layers.BatchNormalization(name=f"{name}_dbn{i}")(h)
        h = layers.ReLU(name=f"{name}_drelu{i}")(h)
        new_splits.append(h)
    x = layers.Concatenate(name=f"{name}_concat")(new_splits)

    # Project back.
    x = layers.Conv1D(filters, 1, padding="same", use_bias=False,
                      name=f"{name}_project")(x)
    x = layers.BatchNormalization(name=f"{name}_project_bn")(x)

    # Squeeze-and-Excitation.
    x = _se_block(x, reduction=4, name=f"{name}_se")

    # Residual + ReLU.
    if residual.shape[-1] != filters:
        residual = layers.Conv1D(filters, 1, padding="same", use_bias=False,
                                 name=f"{name}_res_proj")(residual)
    x = layers.Add(name=f"{name}_add_res")([x, residual])
    x = layers.ReLU(name=f"{name}_relu_out")(x)
    return x


# ---------------------------------------------------------------------------
# Attentive Statistics Pooling
# ---------------------------------------------------------------------------
def _attentive_stats_pool(x, name: str = "asp"):
    """Compute attention-weighted (mu, sigma) over the time axis."""
    c = x.shape[-1]
    attn = layers.Conv1D(c, 1, padding="same", activation="tanh",
                         name=f"{name}_tanh")(x)
    attn = layers.Conv1D(c, 1, padding="same", activation="softmax",
                         name=f"{name}_softmax")(attn)

    weighted = layers.Multiply(name=f"{name}_weighted")([x, attn])
    mean = layers.Lambda(lambda t: tf.reduce_sum(t, axis=1),
                         output_shape=(c,), name=f"{name}_mean")(weighted)

    x_sq = layers.Multiply(name=f"{name}_xsq")([x, x])
    weighted_sq = layers.Multiply(name=f"{name}_wsq")([x_sq, attn])
    mean_sq = layers.Lambda(lambda t: tf.reduce_sum(t, axis=1),
                            output_shape=(c,), name=f"{name}_meansq")(weighted_sq)

    mean_squared = layers.Multiply(name=f"{name}_mean_sq")([mean, mean])
    var = layers.Subtract(name=f"{name}_var")([mean_sq, mean_squared])
    std = layers.Lambda(lambda t: tf.sqrt(tf.maximum(t, 1e-9)),
                        output_shape=(c,), name=f"{name}_std")(var)
    return layers.Concatenate(name=f"{name}_concat")([mean, std])


# ---------------------------------------------------------------------------
# Full model
# ---------------------------------------------------------------------------
def build_ecapa_pruned(num_classes: int,
                       input_shape: tuple[int, int] = (NUM_FRAMES, MEL_BANDS),
                       channels: int = 20,
                       scale: int = 4,
                       ) -> tf.keras.Model:
    inputs = tf.keras.Input(shape=input_shape, name="mfcc_t20")

    # Stem.
    x = layers.Conv1D(channels, 5, padding="same", use_bias=False,
                      name="stem")(inputs)
    x = layers.BatchNormalization(name="stem_bn")(x)
    x = layers.ReLU(name="stem_relu")(x)
    stem_out = x

    # Two pruned SE-Res2Blocks (dilations 2, 3).
    b1 = _res2_block(x, filters=channels, kernel_size=3, scale=scale,
                     dilation=2, name="b1")
    b2 = _res2_block(b1, filters=channels, kernel_size=3, scale=scale,
                     dilation=3, name="b2")

    # Multi-Layer feature Aggregation (MLA): concatenate stem, b1 and b2.
    x = layers.Concatenate(name="mla")([stem_out, b1, b2])
    x = layers.Conv1D(64, 1, padding="same", use_bias=False, name="mla_proj")(x)
    x = layers.BatchNormalization(name="mla_bn")(x)
    x = layers.ReLU(name="mla_relu")(x)

    # Attentive Statistics Pooling.
    x = _attentive_stats_pool(x, name="asp")

    # Embedding + softmax head.
    x = layers.Dense(64, name="embedding")(x)
    x = layers.BatchNormalization(name="emb_bn")(x)
    x = layers.ReLU(name="emb_relu")(x)
    outputs = layers.Dense(num_classes, activation="softmax",
                           name="speaker")(x)

    model = tf.keras.Model(inputs, outputs, name="ECAPA-TDNN-pruned")
    model.compile(
        optimizer=tf.keras.optimizers.Adam(learning_rate=1e-3),
        loss="sparse_categorical_crossentropy",
        metrics=["accuracy"],
    )
    return model


if __name__ == "__main__":
    m = build_ecapa_pruned(num_classes=4)
    m.summary()
    print("Trainable params:", m.count_params())
