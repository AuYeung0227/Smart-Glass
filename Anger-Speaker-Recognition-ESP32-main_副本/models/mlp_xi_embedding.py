"""
MLP-XiEmbedding (speaker identification) — Table II of the paper.

Pipeline (input shape [1, 80] — the Xi-Vector pooled features):
    Front-end : Xi-Vector pooling [mu, sigma, max, min] over 20 Mel bands (80)
                (computed offline; see utils/mfcc.py)
    StandardScaler (offline, parameters compiled into firmware)
    Dense(128) + BN + ReLU + Dropout(0.35)
    Dense(64)  + BN + ReLU + Dropout(0.30)   <- "xvector_embedding"
    Dense(32)  + BN + ReLU + Dropout(0.20)
    Dense(N_sp) + Softmax

Total parameters: ~21,604 (for N=4 speakers, matches the paper).
"""
from __future__ import annotations

import tensorflow as tf
from tensorflow.keras import layers, regularizers

from utils.mfcc import XI_DIM


def build_mlp_xi_embedding(num_classes: int,
                           input_dim: int = XI_DIM,
                           l2_reg: float = 1e-4,
                           ) -> tf.keras.Model:
    reg = regularizers.l2(l2_reg)
    inputs = tf.keras.Input(shape=(input_dim,), name="xivector")

    x = layers.Dense(128, activation="relu", kernel_regularizer=reg,
                     name="dense1")(inputs)
    x = layers.BatchNormalization(name="bn1")(x)
    x = layers.Dropout(0.35, name="drop1")(x)

    x = layers.Dense(64, activation="relu", kernel_regularizer=reg,
                     name="xvector_embedding")(x)
    x = layers.BatchNormalization(name="bn2")(x)
    x = layers.Dropout(0.30, name="drop2")(x)

    x = layers.Dense(32, activation="relu", kernel_regularizer=reg,
                     name="dense3")(x)
    x = layers.Dropout(0.20, name="drop3")(x)

    outputs = layers.Dense(num_classes, activation="softmax",
                           name="speaker")(x)
    model = tf.keras.Model(inputs, outputs, name="MLP-XiEmbedding")
    model.compile(
        optimizer=tf.keras.optimizers.Adam(learning_rate=1e-3),
        loss="sparse_categorical_crossentropy",
        metrics=["accuracy"],
    )
    return model


if __name__ == "__main__":
    m = build_mlp_xi_embedding(num_classes=4)
    m.summary()
    print("Trainable params:", m.count_params())
