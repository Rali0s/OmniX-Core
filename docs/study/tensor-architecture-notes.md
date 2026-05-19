# Tensor Architecture Study Notes

## Central Rule

Do not target tensor hardware directly before understanding tensor math, execution graphs, and portable backend abstraction. Tensor hardware accelerates math kernels; it does not replace OmniX orchestration logic, evidence routing, retrieval, telemetry, or safety policy.

OmniX should build around mathematical tensor operations and execution graphs first, then map those abstractions onto hardware accelerators later.

## Compute Hierarchy

1. CPU scalar/vector execution: understand memory layout, cache locality, and ordinary matrix/vector loops before reaching for accelerators.
2. CPU SIMD and math libraries: study SSE, AVX, AVX2, AVX512, NEON, Apple AMX, BLAS, GEMM, cache-aware matrix multiplication, and tiling/blocking.
3. GPU compute fundamentals: study kernels, host/device memory, memory movement, occupancy, warps/wavefronts, thread blocks, synchronization, and launch overhead.
4. Apple Silicon path: study Accelerate, Metal compute, Core ML, MLX, unified memory, lazy tensor execution, and Apple’s preferred abstraction surfaces.
5. Cross-platform abstraction: design around operations such as `matmul`, `add`, `activation`, `softmax`, and graph execution before choosing CUDA, Metal, Vulkan, ROCm, or another backend.
6. Existing inference engines: study GGML, llama.cpp, ONNX Runtime, tinygrad, and MLX for portable tensor design, graph scheduling, quantization, and backend dispatch.
7. OmniX architecture: decide what belongs in tensor runtime versus CPU orchestration, deterministic TZE routing, local evidence handling, retrieval, and operator policy.

## Architecture Guardrails

- Do not CUDA-lock OmniX before the tensor abstraction is stable.
- Do not make OmniX Apple-only just because Apple Silicon is available.
- Do not confuse inference math with orchestration logic.
- Do not claim tensor hardware is being used unless deterministic evidence shows that backend is active.
- Keep current JSON tensor bundle literacy, `mlp-lens`, `tensor ask`, and supervised training capture available while this study track matures.

## Current Study Position

Mike / Operator is reviewing GPT’s tensor architecture homework. GPT/model output is advisory study material, not runtime authority. Advanced vendor-specific backend work should wait until these notes are reviewed and expanded with operator understanding.


You are at the exact right stage to avoid a major engineering trap:

> “I have tensor hardware available, therefore I should immediately target vendor-specific tensor APIs.”

Do not do that yet.

You first want to understand the *compute hierarchy* and *abstraction stack* behind tensor execution. Otherwise OmniX risks becoming:

* CUDA-locked
* Apple-only
* impossible to maintain
* impossible to benchmark honestly
* architecturally confused between inference math vs orchestration logic

The real goal is:

> Build OmniX around mathematical tensor operations and execution graphs first,
> then map those abstractions onto hardware accelerators later.

Here is the progression I would recommend.

---

# Phase 1 — Learn What “Tensor Hardware” Actually Is

A tensor core is not “AI magic.”

It is:

* massively parallel matrix math hardware
* optimized for:

  * matrix multiply
  * fused multiply-add (FMA)
  * vectorized operations
  * reduced precision arithmetic

Most modern AI reduces to:

[
C = A \times B + D
]

or repeated forms of:

[
y = Wx + b
]

genui{"math_block_widget_always_prefetch_v2":{"content":"y = Wx + b"}}

and attention:

[
\text{Attention}(Q,K,V)=\text{softmax}\left(\frac{QK^T}{\sqrt{d_k}}\right)V
]

\mathrm{Attention}(Q,K,V)=\mathrm{softmax}\left(\frac{QK^T}{\sqrt{d_k}}\right)V

Tensor hardware exists to accelerate those operations.

Not your entire application.

That distinction matters enormously.

---

# Phase 2 — Learn the Stack From Bottom-Up

This is the educational ladder you want.

## Layer 1 — CPU SIMD

Before tensor cores:

* SSE
* AVX
* AVX2
* AVX512
* NEON (ARM)
* Apple AMX

Learn:

* vectorization
* cache locality
* memory bandwidth
* tiling/blocking
* GEMM

Research terms:

* BLAS
* GEMM
* cache-aware matrix multiplication
* SIMD intrinsics

Read:

* [BLIS Framework](https://github.com/flame/blis?utm_source=chatgpt.com)
* [OpenBLAS](https://www.openblas.net/?utm_source=chatgpt.com)
* [Intel oneAPI Math Kernel Library](https://www.intel.com/content/www/us/en/developer/tools/oneapi/onemkl.html?utm_source=chatgpt.com)

This teaches:

> why matrix math performance is mostly memory engineering.

---

# Phase 3 — GPU Compute Fundamentals

Do NOT start with CUDA tutorials.

Start with:

* parallel programming concepts
* kernels
* memory movement
* occupancy
* warps/wavefronts
* thread blocks
* synchronization

Best educational targets:

## NVIDIA CUDA

Even if you do not deploy CUDA.

Because CUDA documentation is the clearest conceptual model.

Start:

* [CUDA Programming Guide](https://docs.nvidia.com/cuda/cuda-c-programming-guide/index.html?utm_source=chatgpt.com)
* [CUDA Best Practices Guide](https://docs.nvidia.com/cuda/cuda-c-best-practices-guide/index.html?utm_source=chatgpt.com)

Learn:

* host vs device memory
* PCIe bottlenecks
* kernel launches
* shared memory
* tensor core acceleration

---

# Phase 4 — Apple Silicon Specifically

This matters for OmniX because you are on MacBook Pro hardware.

Apple does NOT expose tensor cores the same way NVIDIA does.

Apple stack is:

| Layer            | Apple Tech           |
| ---------------- | -------------------- |
| High Level       | CoreML               |
| GPU Compute      | Metal                |
| ML Graph Runtime | MLX                  |
| Accelerated Math | Accelerate.framework |
| Matrix Engine    | AMX                  |

The important realization:

> Apple wants you using Metal + ML frameworks,
> not directly programming tensor units.

Research:

## Metal Compute

* [Metal Shading Language Spec](https://developer.apple.com/metal/Metal-Shading-Language-Specification.pdf?utm_source=chatgpt.com)
* [Metal for Accelerating Machine Learning](https://developer.apple.com/metal/ml/?utm_source=chatgpt.com)

## MLX (VERY IMPORTANT FOR YOU)

Apple’s MLX is probably the single best educational bridge for OmniX.

* [MLX by Apple](https://mlx-framework.org/?utm_source=chatgpt.com)
* [MLX GitHub](https://github.com/ml-explore/mlx?utm_source=chatgpt.com)

MLX teaches:

* unified memory
* tensor graphs
* lazy execution
* Metal-backed tensor execution
* transformer internals

without forcing CUDA-first thinking.

---

# Phase 5 — Cross-Platform Compute Abstractions

This is where OmniX should eventually live.

Do NOT hardwire:

* CUDA kernels
* Metal-only code
* DirectX compute
* Vulkan compute

Instead:

Build an abstraction layer.

Example:

```cpp
Tensor DeviceTensor::matmul(
    const Tensor& a,
    const Tensor& b
);
```

Then backends:

* CPU
* CUDA
* Metal
* Vulkan
* ROCm

This is how:

* PyTorch
* TensorFlow
* llama.cpp
* ONNX Runtime
  work internally.

---

# Phase 6 — Study Existing Inference Engines

THIS is probably the most important practical step.

You learn more from these than from academic papers.

## Highest Value Projects For OmniX

### [llama.cpp](https://github.com/ggml-org/llama.cpp?utm_source=chatgpt.com)

Study:

* tensor abstractions
* quantization
* backend dispatch
* Metal backend
* CUDA backend
* memory mapping

This is likely your single most important research target.

---

### [GGML](https://github.com/ggml-org/ggml?utm_source=chatgpt.com)

GGML is the real educational goldmine.

It shows:

* lightweight tensor engines
* graph execution
* backend scheduling
* inference-first design

Perfect for OmniX.

---

### [ONNX Runtime](https://onnxruntime.ai/?utm_source=chatgpt.com)

Study:

* execution providers
* backend abstraction
* hardware portability

---

### [tinygrad](https://github.com/tinygrad/tinygrad?utm_source=chatgpt.com)

Tinygrad is incredibly educational.

You can actually understand the entire engine.

Learn:

* autograd
* kernels
* tensor ops
* lazy execution
* graph lowering

---

# Phase 7 — The Real Architecture Question

Before targeting tensor hardware, define:

## What IS OmniX mathematically?

Is it:

* inference engine?
* orchestration layer?
* agent runtime?
* vector pipeline?
* reasoning graph?
* tensor runtime?
* symbolic engine?
* hybrid?

Because:

* not every subsystem benefits from tensors
* forcing tensors everywhere creates bad systems

Example:

* orchestration logic → CPU
* tokenization → CPU
* inference → GPU/tensor
* embeddings → GPU
* graph routing → CPU
* retrieval → memory/cache optimized
* telemetry → CPU/network bound

---

# My Recommendation For OmniX Specifically

Your shortest path to mastery:

## Stage 1

Study:

* GGML
* llama.cpp
* tinygrad

## Stage 2

Implement:

* simple tensor struct
* matrix multiply
* SIMD acceleration

## Stage 3

Implement:

* computation graph
* lazy execution
* backend abstraction

## Stage 4

Add:

* Metal backend on Mac

## Stage 5

ONLY THEN:

* CUDA backend
* Vulkan compute
* distributed inference

---

# The Critical Insight

CUDA is not the real lesson.

The real lesson is:

> How do you design a portable tensor execution runtime?

That is the actual engineering problem companies solve.

NVIDIA just currently dominates one implementation path.

OmniX should think more like:

* GGML
* ONNX Runtime
* tinygrad
* MLX

than:

* “directly code tensor cores.”

That mindset will save you years of technical debt.
