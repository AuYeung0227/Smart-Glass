"""
DS-CNN baseline for anger detection — Table I, middle column.

Reference: Zhang et al., "Hello Edge: Keyword Spotting on Microcontrollers",
arXiv:1711.07128 (2017).

The architecture factorizes a standard convolution into a depthwise 3x3
followed by a pointwise 1x1 convolution. Scaled to ~4.5 k parameters to
match the paper's matched-parameter-budget protocol.

Pipeline (input shape [1, 20, 63, 1]):
    Stem    : Conv2D 3x3, 32 filters, BN, ReLU, stride 2
    Block 1 : DWConv 3x3 + PWConv 1x1, 32 filters, BN, ReLU
    Block 2 : DWConv 3x3 + PWConv 1x1, 32 filters, BN, ReLU
    Block 3 : DWConv 3x3 + PWConv 1x1, 32 filters, BN, ReLU
    Pooling : GlobalAvgPool2D (32)
    Head    : Dropout 0.30 + Dense(1, Sigmoid)
"""
from __future__ import annotations

import tensorflow as tf
from tensorflow.keras import layers


def _ds_block(x, filters: int, name: str):
    x = layers.DepthwiseConv2D((3, 3), padding="same", use_bias=False,
                               name=f"{name}_dw")(x)
    x = layers.BatchNormalization(name=f"{name}_dw_bn")(x)
    x = layers.ReLU(name=f"{name}_dw_relu")(x)
    x = layers.Conv2D(filters, (1, 1), padding="same", use_bias=False,
                      name=f"{name}_pw")(x)
    x = layers.BatchNormalization(name=f"{name}_pw_bn")(x)
    x = layers.ReLU(name=f"{name}_pw_relu")(x)
    return x


def build_ds_cnn(input_shape: tuple[int, int, int] = (20, 63, 1),
                 filters: int = 32,
                 n_blocks: int = 3,
                 dropout: float = 0.30,
                 ) -> tf.keras.Model:
    inputs = tf.keras.Input(shape=input_shape, name="mfcc")

    # Stem (strided 3x3) — same as DS-CNN-S of the original paper.
    x = layers.Conv2D(filters, (3, 3), strides=(2, 2), padding="same",
                      use_bias=False, name="stem")(inputs)
    x = layers.BatchNormalization(name="stem_bn")(x)
    x = layers.ReLU(name="stem_relu")(x)

    for i in range(n_blocks):
        x = _ds_block(x, filters, name=f"block{i + 1}")

    x = layers.GlobalAveragePooling2D(name="gap")(x)
    x = layers.Dropout(dropout, name="drop")(x)
    outputs = layers.Dense(1, activation="sigmoid", name="anger")(x)

    model = tf.keras.Model(inputs, outputs, name="DS-CNN")
    model.compile(
        optimizer=tf.keras.optimizers.Adam(learning_rate=1e-3),
        loss="binary_crossentropy",
        metrics=["accuracy"],
    )
    return model


if __name__ == "__main__":
    m = build_ds_cnn()
    m.summary()
    print("Trainable params:", m.count_params())
