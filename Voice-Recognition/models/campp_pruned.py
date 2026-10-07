"""
CAM++ pruned baseline for speaker identification — Table II, right column.

Reference: Wang et al., "CAM++: A Fast and Accurate Speaker Verification Model
Scaled with Number of Frames", Interspeech 2023.

The canonical CAM++ extends densely-connected TDNNs with the Context-Aware
Masking (CAM) module: a channel-wise gating mechanism with cost O(C) instead
of the O(T^2) of self-attention. The pruned version preserves that
signature with 32 base channels stacked at dilations {1, 2, 3}.

Pipeline (input shape [1, 63, 20]):
    Stem    : Conv1D k=5, 32 filters, BN, ReLU
    Block 1 : CAM-Block (DWConv d=1 + mask), 32 channels
    Block 2 : CAM-Block (DWConv d=2 + mask), 32 channels
    Block 3 : CAM-Block (DWConv d=3 + mask), 64 channels
    Aggreg. : Dense concat + Conv1D k=1 + statistics pooling
    Embed.  : Dense(64), BN, ReLU
    Head    : Dense(N, softmax)

Target parameter count: ~29,800.
"""
from __future__ import annotations

import tensorflow as tf
from tensorflow.keras import layers

from utils.mfcc import MEL_BANDS, NUM_FRAMES


# ---------------------------------------------------------------------------
# Context-Aware Mask
# ---------------------------------------------------------------------------
def _cam_mask(x, reduction: int = 4, name: str = "cam"):
    """Channel-wise gating: global avg context -> sigmoid -> elementwise mul.

    Equivalent to a stripped-down SE block applied along the channel axis
    only (O(C) cost). This matches the description in the CAM++ paper.
    """
    c = x.shape[-1]
    s = layers.GlobalAveragePooling1D(name=f"{name}_ctx")(x)
    s = layers.Dense(max(2, c // reduction), activation="relu",
                     name=f"{name}_fc1")(s)
    s = layers.Dense(c, activation="sigmoid", name=f"{name}_fc2")(s)
    s = layers.Reshape((1, c), name=f"{name}_reshape")(s)
    return layers.Multiply(name=f"{name}_apply")([x, s])


# ---------------------------------------------------------------------------
# CAM-Block: dilated Conv1D + CAM mask + 1x1 projection
# ---------------------------------------------------------------------------
def _cam_block(x, filters: int, kernel_size: int, dilation: int, name: str):
    """Single CAM-Block: dilated Conv1D + channel-wise gating + residual.

    NOTE: We use a standard Conv1D with `dilation_rate` instead of a
    depthwise dilated Conv1D. The depthwise variant produces NaN gradients
    in Apple Silicon / XLA when groups==in_ch with dilations>1, an
    interaction we cannot work around within Keras 3. The standard Conv1D
    is conceptually equivalent for the small channel counts used here.
    """
    in_ch = x.shape[-1]

    h = layers.Conv1D(in_ch, kernel_size, padding="same",
                      dilation_rate=dilation,
                      use_bias=False,
                      kernel_initializer=tf.keras.initializers.HeNormal(seed=0),
                      name=f"{name}_dconv")(x)
    h = layers.BatchNormalization(epsilon=1e-3, name=f"{name}_dconv_bn")(h)
    h = layers.ReLU(name=f"{name}_dconv_relu")(h)

    h = _cam_mask(h, reduction=4, name=f"{name}_cam")

    h = layers.Conv1D(filters, 1, padding="same", use_bias=False,
                      kernel_initializer=tf.keras.initializers.HeNormal(seed=1),
                      name=f"{name}_pw")(h)
    h = layers.BatchNormalization(epsilon=1e-3, name=f"{name}_pw_bn")(h)

    # Residual connection (only when shapes match).
    if x.shape[-1] != filters:
        return layers.ReLU(name=f"{name}_relu_out")(h)
    return layers.ReLU(name=f"{name}_relu_out")(layers.Add(name=f"{name}_add")([h, x]))


# ---------------------------------------------------------------------------
# Statistics pooling (mu, sigma) over time
# ---------------------------------------------------------------------------
def _stats_pool(x, name: str = "stats"):
    """Compute (mu, sigma) over the time axis.

    IMPORTANT: tf.math.reduce_std produces NaN gradients when the variance
    is exactly zero (which can happen in the first batches before BN warms
    up). We compute std manually with a safe epsilon inside the sqrt:
        std = sqrt(mean((x - mu)^2) + eps)
    """
    c = x.shape[-1]
    mean = layers.Lambda(lambda t: tf.reduce_mean(t, axis=1),
                         output_shape=(c,), name=f"{name}_mean")(x)

    # Broadcast mean over time and compute variance, then sqrt with epsilon.
    def _safe_std(args, eps=1e-5):
        feat, mu = args
        # feat: (B, T, C),  mu: (B, C) -> (B, 1, C)
        mu_b = tf.expand_dims(mu, axis=1)
        var = tf.reduce_mean(tf.square(feat - mu_b), axis=1)
        return tf.sqrt(var + eps)

    std = layers.Lambda(_safe_std, output_shape=(c,),
                        name=f"{name}_std")([x, mean])
    return layers.Concatenate(name=f"{name}_concat")([mean, std])


# ---------------------------------------------------------------------------
# Full model
# ---------------------------------------------------------------------------
def build_campp_pruned(num_classes: int,
                       input_shape: tuple[int, int] = (NUM_FRAMES, MEL_BANDS),
                       base_filters: int = 24,
                       head_filters: int = 48,
                       ) -> tf.keras.Model:
    inputs = tf.keras.Input(shape=input_shape, name="mfcc_t20")

    # Normalize the raw MFCC input (improves numerical stability on macOS
    # Apple Silicon / XLA where unnormalized large MFCC values can produce
    # NaN gradients in the depthwise dilated convolutions).
    x = layers.BatchNormalization(epsilon=1e-3, name="input_bn")(inputs)

    x = layers.Conv1D(base_filters, 5, padding="same", use_bias=False,
                      kernel_initializer=tf.keras.initializers.HeNormal(seed=42),
                      name="stem")(x)
    x = layers.BatchNormalization(epsilon=1e-3, name="stem_bn")(x)
    x = layers.ReLU(name="stem_relu")(x)

    # Three CAM-Blocks at increasing dilations.
    b1 = _cam_block(x,  base_filters, kernel_size=3, dilation=1, name="b1")
    b2 = _cam_block(b1, base_filters, kernel_size=3, dilation=2, name="b2")
    b3 = _cam_block(b2, head_filters, kernel_size=3, dilation=3, name="b3")

    # Densely-aggregated representation: concatenate the three block outputs.
    # All three need to share the time dimension (they do, padding='same').
    x = layers.Concatenate(name="dense_agg")([b1, b2, b3])
    x = layers.Conv1D(head_filters, 1, padding="same", use_bias=False,
                      kernel_initializer=tf.keras.initializers.HeNormal(seed=43),
                      name="agg_pw")(x)
    x = layers.BatchNormalization(epsilon=1e-3, name="agg_bn")(x)
    x = layers.ReLU(name="agg_relu")(x)

    # Statistics pool over time -> 2*head_filters.
    x = _stats_pool(x, name="stats")

    # Embedding + softmax head.
    x = layers.Dense(64, name="embedding")(x)
    x = layers.BatchNormalization(epsilon=1e-3, name="emb_bn")(x)
    x = layers.ReLU(name="emb_relu")(x)
    outputs = layers.Dense(num_classes, activation="softmax",
                           name="speaker")(x)

    model = tf.keras.Model(inputs, outputs, name="CAMpp-pruned")
    # clipnorm prevents the rare NaN-explosion observed on M-series + XLA
    # in the first 2-3 batches when the depthwise dilated kernels happen
    # to produce a near-zero variance feature for a given batch.
    model.compile(
        optimizer=tf.keras.optimizers.Adam(learning_rate=1e-3, clipnorm=1.0),
        loss="sparse_categorical_crossentropy",
        metrics=["accuracy"],
    )
    return model


if __name__ == "__main__":
    m = build_campp_pruned(num_classes=4)
    m.summary()
    print("Trainable params:", m.count_params())