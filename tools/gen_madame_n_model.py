#!/usr/bin/env python3
"""
Deterministic generator for Madame N neural stage-director OpenVINO IR model (SPEC_v4 Section 4.2.3 & M5).

Frozen contract:
  - Input  `features`     : FP32 [1, 16] matching boo::DirectorFeatureVector
  - Output `director_out` : FP32 [1, 10]
      [0..4] = Softmax probabilities for [Calm, Suspense, Bloom, Comic, Transition]
      [5..9] = Sigmoid continuous mood controls in [0.0, 1.0]:
               [bloomStrength, irisAngle, lightTension, spectralShift, cutAccent]
"""

from __future__ import annotations

import hashlib
import json
import math
from pathlib import Path

import numpy as np
import openvino as ov
from openvino import opset13 as ops

SEED = 266203


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def build_training_corpus(rng: np.random.Generator, n_samples: int = 4096) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    x = np.zeros((n_samples, 16), dtype=np.float32)
    y_cat = np.zeros((n_samples, 5), dtype=np.float32)
    y_cont = np.zeros((n_samples, 5), dtype=np.float32)

    for i in range(n_samples):
        act_idx = i % 7
        prog = float(rng.uniform(0.0, 1.0))
        t = float(rng.uniform(0.0, 360.0))
        env = float(rng.uniform(0.05, 0.95))
        beat = float(np.clip(0.5 + 0.5 * math.sin(t * 2.094395), 0.0, 1.0))
        low = float(np.clip(env * 0.8 + 0.2 * math.cos(t * 0.7), 0.0, 1.0))
        mid = float(np.clip(0.4 + 0.4 * math.sin(t * 1.3), 0.0, 1.0))
        high = float(np.clip(0.3 + 0.5 * math.sin(t * 3.1), 0.0, 1.0))
        midnight = 1.0 if (i % 2 == 1) else 0.0
        has_reveal = 1.0 if (act_idx in (0, 3, 6) and prog > 0.62) else 0.0
        near_boundary = 1.0 if (prog > 0.92 or prog < 0.05) else 0.0
        salt = float(rng.uniform(0.0, 1.0))

        # Target category matching SPEC_v4 Section 2.2 & 4.2.3
        if prog > 0.93:
            cat = 4  # Transition
        elif act_idx == 0:
            cat = 2 if prog > 0.75 else 1
        elif act_idx == 1:
            cat = 1  # Suspense
        elif act_idx == 2:
            cat = 0  # Calm
        elif act_idx == 3:
            cat = 1 if prog < 0.55 else 3  # Suspense -> Comic/Lounge
        elif act_idx == 4:
            cat = 2  # Bloom
        elif act_idx == 5:
            cat = 1  # Suspense
        else:
            cat = 2  # Bloom

        edition_bias = 0.08 * midnight
        if cat == 0:  # Calm
            cont = [0.35 + 0.10 * beat, 0.55, 0.20 + edition_bias, 0.22 + 0.05 * mid, 0.05]
        elif cat == 1:  # Suspense
            cont = [0.28 + 0.12 * env, 0.32 + 0.06 * low, 0.74 + edition_bias, 0.40 + 0.08 * high, 0.22 * beat]
        elif cat == 2:  # Bloom
            cont = [0.78 + 0.15 * beat, 0.82, 0.42 + 0.05 * env, 0.68 + 0.08 * has_reveal, 0.35 * beat]
        elif cat == 3:  # Comic (Madame N probiotic lounge)
            cont = [0.48 + 0.08 * env, 0.68, 0.18 + 0.04 * edition_bias, 0.32 + 0.10 * mid, 0.15 * beat]
        else:  # Transition
            cont = [0.60, 0.50, 0.50, 0.55, 0.70]

        cont = [float(np.clip(v, 0.02, 0.98)) for v in cont]
        prev_jitter = rng.normal(0.0, 0.04, size=5).astype(np.float32)
        prev_state = np.clip(np.array(cont, dtype=np.float32) + prev_jitter, 0.0, 1.0)

        x[i] = np.array(
            [
                env,
                beat,
                low,
                mid,
                high,
                prog,
                act_idx / 6.0,
                midnight,
                has_reveal,
                near_boundary,
                prev_state[0],
                prev_state[1],
                prev_state[2],
                prev_state[3],
                prev_state[4],
                salt,
            ],
            dtype=np.float32,
        )
        y_cat[i, cat] = 1.0
        y_cont[i] = np.array(cont, dtype=np.float32)

    return x, y_cat, y_cont


def softmax(z: np.ndarray) -> np.ndarray:
    zs = z - np.max(z, axis=1, keepdims=True)
    ez = np.exp(zs)
    return ez / np.sum(ez, axis=1, keepdims=True)


def sigmoid(z: np.ndarray) -> np.ndarray:
    return 1.0 / (1.0 + np.exp(-np.clip(z, -20.0, 20.0)))


def train_mlp(x: np.ndarray, y_cat: np.ndarray, y_cont: np.ndarray, rng: np.random.Generator) -> dict[str, np.ndarray]:
    d_in, d_h1, d_h2 = 16, 64, 32
    w1 = (rng.standard_normal((d_in, d_h1)) * math.sqrt(2.0 / d_in)).astype(np.float32)
    b1 = np.zeros((1, d_h1), dtype=np.float32)
    w2 = (rng.standard_normal((d_h1, d_h2)) * math.sqrt(2.0 / d_h1)).astype(np.float32)
    b2 = np.zeros((1, d_h2), dtype=np.float32)
    wc = (rng.standard_normal((d_h2, 5)) * math.sqrt(1.0 / d_h2)).astype(np.float32)
    bc = np.zeros((1, 5), dtype=np.float32)
    wm = (rng.standard_normal((d_h2, 5)) * math.sqrt(1.0 / d_h2)).astype(np.float32)
    bm = np.zeros((1, 5), dtype=np.float32)

    params = [w1, b1, w2, b2, wc, bc, wm, bm]
    m_state = [np.zeros_like(p) for p in params]
    v_state = [np.zeros_like(p) for p in params]

    lr = 0.008
    beta1, beta2, eps = 0.9, 0.999, 1e-8
    n = float(x.shape[0])

    for step in range(1, 901):
        z1 = x @ w1 + b1
        h1 = np.maximum(z1, 0.0)
        z2 = h1 @ w2 + b2
        h2 = np.maximum(z2, 0.0)

        logits_c = h2 @ wc + bc
        prob_c = softmax(logits_c)

        logits_m = h2 @ wm + bm
        pred_m = sigmoid(logits_m)

        # Gradients: cross-entropy + 2.5 * MSE
        d_logits_c = (prob_c - y_cat) / n
        d_pred_m = (5.0 / n) * (pred_m - y_cont)
        d_logits_m = d_pred_m * pred_m * (1.0 - pred_m)

        g_wc = h2.T @ d_logits_c
        g_bc = np.sum(d_logits_c, axis=0, keepdims=True)
        g_wm = h2.T @ d_logits_m
        g_bm = np.sum(d_logits_m, axis=0, keepdims=True)

        d_h2 = d_logits_c @ wc.T + d_logits_m @ wm.T
        d_z2 = d_h2 * (z2 > 0.0)
        g_w2 = h1.T @ d_z2
        g_b2 = np.sum(d_z2, axis=0, keepdims=True)

        d_h1 = d_z2 @ w2.T
        d_z1 = d_h1 * (z1 > 0.0)
        g_w1 = x.T @ d_z1
        g_b1 = np.sum(d_z1, axis=0, keepdims=True)

        grads = [g_w1, g_b1, g_w2, g_b2, g_wc, g_bc, g_wm, g_bm]
        for idx, (p, g) in enumerate(zip(params, grads)):
            m_state[idx] = beta1 * m_state[idx] + (1.0 - beta1) * g
            v_state[idx] = beta2 * v_state[idx] + (1.0 - beta2) * (g * g)
            m_hat = m_state[idx] / (1.0 - beta1**step)
            v_hat = v_state[idx] / (1.0 - beta2**step)
            p -= lr * m_hat / (np.sqrt(v_hat) + eps)

    # Evaluate final training metrics
    h1 = np.maximum(x @ w1 + b1, 0.0)
    h2 = np.maximum(h1 @ w2 + b2, 0.0)
    acc = float(np.mean(np.argmax(softmax(h2 @ wc + bc), axis=1) == np.argmax(y_cat, axis=1)))
    mse = float(np.mean((sigmoid(h2 @ wm + bm) - y_cont) ** 2))
    print(f"[MadameN] Trained compact 16->64->32->(5+5) MLP | category_acc={acc:.4f} | mood_mse={mse:.6f}")

    return {
        "w1": w1.astype(np.float32),
        "b1": b1.astype(np.float32),
        "w2": w2.astype(np.float32),
        "b2": b2.astype(np.float32),
        "wc": wc.astype(np.float32),
        "bc": bc.astype(np.float32),
        "wm": wm.astype(np.float32),
        "bm": bm.astype(np.float32),
        "accuracy": np.array([acc], dtype=np.float32),
        "mse": np.array([mse], dtype=np.float32),
    }


def build_openvino_model(weights: dict[str, np.ndarray]) -> ov.Model:
    inp = ops.parameter([1, 16], np.float32, name="features")

    w1 = ops.constant(weights["w1"], np.float32, name="w1")
    b1 = ops.constant(weights["b1"], np.float32, name="b1")
    h1 = ops.relu(ops.add(ops.matmul(inp, w1, False, False), b1), name="h1_relu")

    w2 = ops.constant(weights["w2"], np.float32, name="w2")
    b2 = ops.constant(weights["b2"], np.float32, name="b2")
    h2 = ops.relu(ops.add(ops.matmul(h1, w2, False, False), b2), name="h2_relu")

    wc = ops.constant(weights["wc"], np.float32, name="wc")
    bc = ops.constant(weights["bc"], np.float32, name="bc")
    cat_probs = ops.softmax(ops.add(ops.matmul(h2, wc, False, False), bc), axis=1, name="category_probs")

    wm = ops.constant(weights["wm"], np.float32, name="wm")
    bm = ops.constant(weights["bm"], np.float32, name="bm")
    mood_ctrl = ops.sigmoid(ops.add(ops.matmul(h2, wm, False, False), bm), name="continuous_mood")

    combined = ops.concat([cat_probs, mood_ctrl], axis=1, name="director_concat")
    res = ops.result(combined, name="director_out")
    res.get_output_tensor(0).set_names({"director_out"})

    model = ov.Model([res], [inp], "madame_n_director_v1")
    return model


def main() -> None:
    repo_root = Path(__file__).resolve().parent.parent
    out_dir = repo_root / "assets" / "models"
    out_dir.mkdir(parents=True, exist_ok=True)

    rng = np.random.default_rng(SEED)
    x, y_cat, y_cont = build_training_corpus(rng)
    weights = train_mlp(x, y_cat, y_cont, rng)
    model = build_openvino_model(weights)

    xml_path = out_dir / "madame_n_director.xml"
    bin_path = out_dir / "madame_n_director.bin"
    ov.save_model(model, str(xml_path), compress_to_fp16=False)

    # Verify load + compile + inference on CPU and (when present) NPU
    core = ov.Core()
    loaded = core.read_model(str(xml_path))
    test_in = x[:1].copy()

    cpu_compiled = core.compile_model(loaded, "CPU")
    cpu_out = cpu_compiled([test_in])[0]
    assert cpu_out.shape == (1, 10) and np.all(np.isfinite(cpu_out))
    assert abs(float(np.sum(cpu_out[0, :5])) - 1.0) < 1e-3
    assert np.all((cpu_out[0, 5:] >= 0.0) & (cpu_out[0, 5:] <= 1.0))
    print(f"[MadameN] CPU inference verified: probs={np.round(cpu_out[0, :5], 3)} mood={np.round(cpu_out[0, 5:], 3)}")

    if "NPU" in core.available_devices:
        npu_compiled = core.compile_model(loaded, "NPU")
        npu_out = npu_compiled([test_in])[0]
        assert npu_out.shape == (1, 10) and np.all(np.isfinite(npu_out))
        max_diff = float(np.max(np.abs(npu_out - cpu_out)))
        print(f"[MadameN] NPU ({core.get_property('NPU', 'FULL_DEVICE_NAME')}) inference verified! max_diff_vs_cpu={max_diff:.5f}")

    xml_sha = sha256_file(xml_path)
    bin_sha = sha256_file(bin_path)
    meta = {
        "schema": 1,
        "model_id": "madame_n_director_v1",
        "seed": SEED,
        "architecture": "MLP 16 -> 64 (ReLU) -> 32 (ReLU) -> [Softmax(5), Sigmoid(5)]",
        "input_name": "features",
        "input_shape": [1, 16],
        "output_name": "director_out",
        "output_shape": [1, 10],
        "categories": ["Calm", "Suspense", "Bloom", "Comic", "Transition"],
        "continuous_controls": ["bloomStrength", "irisAngle", "lightTension", "spectralShift", "cutAccent"],
        "training_category_accuracy": round(float(weights["accuracy"][0]), 5),
        "training_mood_mse": round(float(weights["mse"][0]), 6),
        "xml_file": "assets/models/madame_n_director.xml",
        "xml_sha256": xml_sha,
        "bin_file": "assets/models/madame_n_director.bin",
        "bin_sha256": bin_sha,
    }
    meta_path = out_dir / "madame_n_metadata.json"
    meta_path.write_text(json.dumps(meta, indent=2) + "\n", encoding="utf-8")
    print(f"[MadameN] Wrote {xml_path.name} ({xml_sha[:16]}...) & {bin_path.name} ({bin_sha[:16]}...)")


if __name__ == "__main__":
    main()
