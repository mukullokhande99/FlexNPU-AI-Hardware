# MobileBERT trace (real)

**File:** `mb_flex_sites.json`
**Sites:** 361 real MatMulInteger sites

## Provenance

- **Source model:** `google/mobilebert-uncased` from HuggingFace (Apache-2.0).
- **Sequence length used for export:** 128 (fixed via
  `tokenizer(..., padding="max_length", max_length=128)`). Every site
  therefore has `M = 128` (rows).
- **Export path:** `torch.onnx.export(model, dummy_ids, opset=17,
  dynamo=True)` on a small wrapper module that takes only `input_ids`,
  to sidestep the transformers-5.x SDPA mask helper (it does not trace
  under the legacy TorchScript exporter — it calls `.shape[0]` on a
  tuple during trace). This is a workaround for the exporter, not a
  semantic change to the model.
- **Quantization:** `onnxruntime.quantization.quantize_dynamic` with
  `weight_type=QuantType.QInt8`. This is the same dynamic INT8
  quantization the canonical FlexNPU flow uses. See
  `canonical FlexNPU: compiler/onnx/rewrite_target_ops.py::rewrite_auto`.
- **Site discovery:** canonical
  `compiler.onnx.rewrite_target_ops.discover_sites_from_onnx()`
  imported from the FlexNPU research repo. No modification.
- **Date:** 2026-09-21.

## Fields

Every record has the standard v0.1 site schema:

| Field | Meaning |
|---|---|
| `idx` | 0-indexed site number |
| `name` | ONNX MatMulInteger node name |
| `a_q_name`, `s_a_name`, `z_a_name` | DynamicQuantizeLinear outputs |
| `W_q_name`, `s_w_name` | quantized weight + scale initializer names |
| `bias_name` | optional bias initializer, or null |
| `y_fp_ort_name` | fp32 output tensor name |
| `K`, `N` | matmul K, N (from `W_q` initializer dims) |
| `N_padded` | N rounded up to a multiple of 16 (ARRAY_N) |
| `M` | rows (== sequence length == 128) |
| `count` | how many times this site fires per inference (1 here) |

## Shape distribution

- Only 4 unique (K, N) pairs appear across all 361 sites:
  (128, 128), (128, 512), (512, 128), (384, 512). This is exactly the
  MobileBERT inverted-bottleneck signature (intra-block hidden=128,
  inter-block hidden=512, multi-head QKV concatenation → K=384).
- All K values are <= MAX_K=1024, so no site is skipped by
  `examples/04_workload_replay.py`.

## Regeneration

```bash
# From canonical FlexNPU on a machine with network access:
pip install onnx onnxruntime onnxscript transformers torch
python3 scratchpad/extract_real_traces.py \
    --out-dir /path/to/FlexNPU-AI-Hardware-v0.1-dryrun \
    --work-dir /tmp/onnx_work \
    --models mobilebert
```

## Redistribution

Shape metadata only (K, N, M, tensor names) — no weights, no tokenizer
vocabulary, no proprietary content. Inherits MobileBERT's Apache-2.0
license.
