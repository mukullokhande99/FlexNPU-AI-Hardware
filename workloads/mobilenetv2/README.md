# MobileNetV2 trace (real)

Two files:

- **`mnv2_conv_records.json`** — the primary MobileNetV2 trace.
  52 Conv nodes from the fp32 ONNX, each carrying the im2col-lowered
  GEMM shape `(M = Hout * Wout, K = Cin * kH * kW, N = Cout)` plus
  the original Conv attributes.
- **`mnv2_flex_sites.json`** — 1 MatMulInteger site (the classifier
  head). ORT's dynamic INT8 quantization targets MatMul/Gemm only;
  every Conv stays as a Conv node in the quantized graph, so the
  MMI-site count for MobileNetV2 is small by design.

## Provenance

- **Source model:** torchvision
  `mobilenet_v2(weights=MobileNet_V2_Weights.IMAGENET1K_V1)`
  (BSD-style; the standard pretrained MobileNetV2 for ImageNet-1k).
- **Input used for export:** `torch.randn(1, 3, 224, 224)`.
- **Export path:** `torch.onnx.export(model, dummy, opset=17)`
  (legacy TorchScript exporter; MobileNetV2 traces cleanly this way).
- **Quantization:** `onnxruntime.quantization.quantize_dynamic` with
  `weight_type=QuantType.QInt8` — same as MobileBERT.
- **Conv record extraction:** canonical
  `compiler.onnx.realize_conv.enumerate_conv_records()` on the fp32
  ONNX (before quantization; that call needs shape-inferred tensors).
- **MMI site discovery:** canonical
  `compiler.onnx.rewrite_target_ops.discover_sites_from_onnx()` on
  the INT8-quantized ONNX.
- **Date:** 2026-09-21.

## Conv family distribution

`mnv2_conv_records.json` covers all 52 Convs of MobileNetV2. Their
family classification (from `enumerate_conv_records`):

| Family | Count | NPU-eligible? |
|---|---:|---|
| `standard` | 1 | Yes (the 3x3 stem Conv) |
| `pw1x1` | 34 | Yes (every 1x1 pointwise Conv) |
| `depthwise` | 17 | No — depthwise not accelerated by the CTB |

35 of 52 Convs are NPU-eligible. The other 17 (depthwise) would run on
CPU in a real heterogeneous deployment.

## Fields (Conv records)

| Field | Meaning |
|---|---|
| `idx`, `name`, `family` | node identity + classification |
| `M, K, N, N_padded` | im2col-lowered GEMM shape |
| `Cin, Cout, kH, kW, groups` | ONNX Conv attributes |
| `Hout, Wout` | output spatial dims |
| `count` | 1 per record |

## Shape range

- `M` ranges from 49 to 12544 (spatial extent varies wildly across the
  network).
- `K` ranges from 27 (the stem's 3*3*3) to 960 (late-stage pointwise).
- The classifier MMI site has `K = 1280, N = 1000, N_padded = 1008`.
  This is the only site where `K > MAX_K=1024` at the canonical arch;
  Lane B seed question B4 (split-K sweet spot) targets exactly this.

## Regeneration

```bash
pip install onnx onnxruntime onnxscript torchvision torch
python3 scratchpad/extract_real_traces.py \
    --out-dir /path/to/FlexNPU-AI-Hardware-v0.1-dryrun \
    --work-dir /tmp/onnx_work \
    --models mobilenetv2
```

## Redistribution

Shape metadata only. Inherits torchvision MobileNetV2's BSD-style
license.
