"""
MicroLightCNN (anger detection) — Table I of the paper.

Pipeline (input shape [1, 20, 63, 1]):

    Conv2D 3x3, 16 filters, BN, ReLU
    MaxPool 2x2 stride 2
    Conv2D 3x3, 16 filters, BN, ReLU
    MaxPool 2x2 stride 2
    Conv2D 3x3, 16 filters, BN, ReLU
    MaxPool 2x2 stride 2
    Flatten + L2-normalization (224)
    Dense(64, ReLU), Dropout(0.50)
    Dense(1, Sigmoid)

Total trainable parameters: ~19,500.

This is the regularized variant of MicroLightCNN that addresses the
train/val overfitting observed in the original LightCNN-style design
without BatchNorm (train_acc 92% / val_acc 79.5% on ESD test split).
Three changes are introduced, all consistent with modern compact-CNN
practice:

    (1) BatchNormalization after each Conv2D, before ReLU.
        BN is supported natively by TFLite Micro (FusedBatchNormV3
        is folded into the preceding Conv2D at conversion time, so
        the on-device op set is unchanged: Conv2D + ReLU + MaxPool2D).
    (2) Dropout 0.50 in the dense head (was 0.30).
    (3) L2 regularization tightened to 5e-4 (was 1e-4).

The op set used by the converted TFLite model is the minimum supported
by the TFLite Micro built-in resolver: Conv2D (with folded BN bias),
MaxPool2D, FullyConnected, Reshape, L2Normalization and Logistic.
"""
from __future__ import annotations

import tensorflow as tf
from tensorflow.keras import layers, regularizers


def build_micro_lightcnn(input_shape: tuple[int, int, int] = (20, 63, 1),
                         dropout: float = 0.50,
                         l2_reg: float = 5e-4,
                         ) -> tf.keras.Model:
    reg = regularizers.l2(l2_reg)
    inputs = tf.keras.Input(shape=input_shape, name="mfcc")

    # NOTE: We use MaxPool 2x2 with stride 2 (non-overlapping). The paper
    # describes the pooling colloquially as "3x3 stride 2" but the reported
    # flatten dimension (224 = 2 x 7 x 16) is only reachable with 2x2/s2.
    pool_size, pool_stride = (2, 2), 2

    # ---- Block 1 ----
    x = layers.Conv2D(16, (3, 3), padding="same", use_bias=False,
                      kernel_regularizer=reg, name="conv1")(inputs)
    x = layers.BatchNormalization(epsilon=1e-3, name="bn1")(x)
    x = layers.ReLU(name="relu1")(x)
    x = layers.MaxPool2D(pool_size, strides=pool_stride, padding="valid",
                         name="pool1")(x)

    # ---- Block 2 ----
    x = layers.Conv2D(16, (3, 3), padding="same", use_bias=False,
                      kernel_regularizer=reg, name="conv2")(x)
    x = layers.BatchNormalization(epsilon=1e-3, name="bn2")(x)
    x = layers.ReLU(name="relu2")(x)
    x = layers.MaxPool2D(pool_size, strides=pool_stride, padding="valid",
                         name="pool2")(x)

    # ---- Block 3 ----
    x = layers.Conv2D(16, (3, 3), padding="same", use_bias=False,
                      kernel_regularizer=reg, name="conv3")(x)
    x = layers.BatchNormalization(epsilon=1e-3, name="bn3")(x)
    x = layers.ReLU(name="relu3")(x)
    x = layers.MaxPool2D(pool_size, strides=pool_stride, padding="valid",
                         name="pool3")(x)

    # ---- Flatten + L2 (target dim = 224 = 2 x 7 x 16 per Table I) ----
    x = layers.Flatten(name="flatten")(x)
    x = tf.keras.layers.UnitNormalization(axis=-1, name="l2norm")(x)

    # ---- Dense head ----
    x = layers.Dense(64, activation="relu", kernel_regularizer=reg,
                     name="dense_emb")(x)
    x = layers.Dropout(dropout, name="drop")(x)
    outputs = layers.Dense(1, activation="sigmoid", name="anger")(x)

    model = tf.keras.Model(inputs, outputs, name="MicroLightCNN")
    model.compile(
        optimizer=tf.keras.optimizers.Adam(learning_rate=1e-3),
        loss="binary_crossentropy",
        metrics=["accuracy"],
    )
    return model


if __name__ == "__main__":
    m = build_micro_lightcnn()
    m.summary()
    print("Trainable params:", m.count_params())