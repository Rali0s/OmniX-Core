# OmniX MLP Lens

`mlp-lens` is an operator-facing explainer for the transformer feed-forward / MLP block:

```text
MLP(x) = W2 * activation(W1 * x + b1) + b2
```

The current OmniX implementation uses deterministic toy embeddings and fixed demo weights. It is scientifically useful for tracing the math, but it is **not** a real LLM inspection unless real model tensors are loaded through a future adapter.

OmniX now also supports a tiny loaded tensor bundle path:

```sh
./build/omnix tool mlp-lens --compact -- --tensor-bundle res/mlp_lens/tiny_mlp_bundle.json "Michael Jordan plays basketball"
```

That path loads tokenizer-like metadata, embedding vectors, and `W1/b1/W2/b2` tensors from disk before running the MLP formula. It is a real loaded-tensor trace for a tiny fixture model, but it is still not a full transformer or production LLM trace.

## Where The MLP Sits

- **Tokens** are text fragments converted into token IDs.
- **Embeddings** map token IDs into numeric vectors.
- The **residual stream** is the running vector state carried through transformer blocks.
- **Attention** moves information between token positions.
- The **MLP / feed-forward network** transforms each token vector independently, often detecting feature-like directions and writing a transformed vector back into the residual stream.

In plain OmniX terms: attention routes context; the MLP transforms the current vector into activated feature evidence.

## Formula Pieces

- `x`: the incoming token/residual vector.
- `W1`: first weight matrix, usually expanding from model width into a larger hidden width.
- `b1`: first bias vector, shifting neuron thresholds.
- `activation`: nonlinear gate such as ReLU or GELU.
- `W2`: second weight matrix, projecting hidden activations back to model width.
- `b2`: final bias vector.
- `output`: the MLP contribution that is usually added back into the residual stream.

`mlp-lens` prints the intermediate values:

- `inputVector`
- `z1PreActivations`
- `hiddenActivations`
- `outputVector`
- `softmaxProbabilities`
- `topActivatedNeurons`

## ReLU And GELU

**ReLU** is:

```text
max(0, x)
```

It hard-gates negative values to zero.

**GELU** is a smooth transformer-friendly activation. OmniX uses the common tanh approximation:

```text
0.5 * x * (1 + tanh(sqrt(2/pi) * (x + 0.044715*x^3)))
```

GELU does not simply turn values on/off; it smoothly weights them by magnitude.

## Logits And Softmax

The MLP output vector is not automatically a language answer. In real transformer models, later layers and a vocabulary projection produce **logits**, which are raw token scores. **Softmax** converts logits into a probability distribution.

The current `mlp-lens` softmax is educational: it converts the toy output vector into probabilities so operators can see how raw scores become normalized probability-like values.

## Neuron Activation

For one hidden neuron:

```text
preActivation = dot(W1_row, x) + b1
activation = GELU(preActivation)
```

`topActivatedNeurons` ranks hidden units by activation magnitude. This helps show which demo neurons fired most strongly for the toy input vector.

Do not read that as "this neuron means Michael Jordan" or "this neuron is basketball." Real transformer neurons can be hard to interpret.

## Polysemanticity And Superposition

Two caveats matter:

- **Polysemanticity:** one neuron can respond to multiple unrelated human concepts.
- **Superposition:** models can pack many sparse features into overlapping directions instead of assigning one clean concept per neuron.

So the honest operator language is:

```text
This neuron activated strongly in this trace.
```

Not:

```text
This neuron definitively means one concept.
```

## Future Tensor Adapters

Future `mlp-lens` adapters may load real tensors from:

- `safetensors`
- ONNX
- GGUF
- extracted small transformer weights

Until then, OmniX reports:

```text
modelSource = demo_weights
adapterStatus = inactive
```

That keeps the tool useful for math learning without pretending to be a real model interpretability stack.

## Real Tensor Bundle Phase

The first remediation beyond demo mode is `loaded_tensor_bundle`. In this mode OmniX reads a small JSON tensor bundle from disk and validates:

- tokenizer source and vocabulary
- `<unk>` fallback token
- embedding table dimensions
- `W1`, `b1`, `W2`, and `b2` dimensions
- activation name
- output label count

The runtime then tokenizes text with a lowercase whitespace-style tokenizer, maps words to token IDs, averages the loaded embedding vectors into `inputVector`, and runs:

```text
z1 = W1 * inputVector + b1
hidden = activation(z1)
output = W2 * hidden + b2
softmax = softmax(output)
```

This is meaningfully better than hardcoded demo weights because the tensors are external data loaded and checked at runtime. It is still not the same thing as inspecting a real LLM because OmniX is not yet loading a production tokenizer, attention blocks, residual stream states, many transformer layers, or actual layer activations from a trained model.

Honest operator language:

```text
OmniX loaded a tiny external MLP tensor bundle and traced it.
```

Not yet:

```text
OmniX inspected a real GPT/Llama/Qwen layer.
```
