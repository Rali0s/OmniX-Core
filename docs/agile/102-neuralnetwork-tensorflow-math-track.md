# NeuralNetwork + TensorFlow Math Track

## Summary

This track restores **NeuralNetwork** as its own OmniX architecture path, separate from neuromorphic / spike-style research. The first implementation is math-first and simulation-first: perceptrons, tensors, weights, bias, activation, loss/error, learning rate, gradient/backprop intuition, and the path toward MLPs.

Guiding principle: **we are at hardware limitations, but nothing says we cannot simulate**. CPU simulation is valid for learning, proofs, tiny classifiers, and OmniX-local signal experiments.

## Scope

- **NeuralNetwork:** dense tensor math, weights, training, inference, classifiers, ranking, TensorFlow/Keras.
- **Neuromorphic:** sparse event/spike/time-based systems, Backtrace, Backtest, Back-add, and SNN research.
- **OmniX neural retrieval:** current deterministic sparse-vector recall, not yet a trained neural network.

Phase 1 keeps core OmniX free of external ML dependencies. Python 3.14.3 is present on this machine, but `tensorflow`, `numpy`, `sklearn`, and `torch` are not currently installed, so TensorFlow remains a named future framework rather than a runtime requirement.

## Implemented Phase 1

- Add pure C++ `NeuralMathEngine`.
- Add CLI:

```sh
omnix nn math perceptron --dataset or --compact
omnix nn math perceptron --dataset and --compact
omnix nn math perceptron --dataset xor --compact
```

- `or` and `and` train a simple single-layer perceptron and report weights, bias, predictions, accuracy, and compact math trace.
- `xor` returns `not_linearly_separable` because XOR requires a hidden layer / MLP.
- TZE stores compact final summaries and provenance, not full epoch-chain spam.
- `scripts/omnix_tensorflow_env_check.sh` reports TensorFlow/Numpy readiness without installing anything.

## Future TensorFlow Lab

The TensorFlow adapter stays disabled unless dependencies exist. Future model shape should follow the standard Keras pattern:

- `Sequential`
- `Dense` layers
- `compile`
- `fit`
- `evaluate`

Hardware acceleration is optional. CPU simulation remains acceptable for small model training, proofs, and local classifier experiments.

## Neural Signal Router

The first practical OmniX neural use case is a **Neural Signal Router**. The runtime implementation is tracked separately in [Native C++ Neural Signal Router](103-native-neural-signal-router.md), with C++ dependency-free scoring first and TensorFlow/Python deferred.

Inputs:

- TView packet JSONL
- logs
- definitions
- memory artifacts
- defense diagnostics

Phase 1 labels:

- `benign_control`
- `plaintext_http`
- `suspicious_port`
- `unknown_service`
- `needs_human_review`

Neural output is advisory only. Deterministic routing and safety gates remain authority.

## Acceptance

- `define neural network`, `define perceptron`, `define TensorFlow`, `define neural simulation`, and `define neural signal router` resolve locally.
- `omnix nn math perceptron --dataset or --compact` converges.
- `omnix nn math perceptron --dataset and --compact` converges.
- `omnix nn math perceptron --dataset xor --compact` returns `not_linearly_separable`.
- TZE replay/report includes the neural math run summary without storing full epoch chains.
- TensorFlow env check reports missing dependencies cleanly.
