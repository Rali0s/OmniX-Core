export type Vector = number[];
export type Matrix = number[][];

export type ActivationName = "relu" | "gelu" | "linear";

export interface MlpParams {
  W1: Matrix;
  b1: Vector;
  W2: Matrix;
  b2: Vector;
  activation: ActivationName;
}

export interface NeuronTrace {
  index: number;
  preActivation: number;
  activation: number;
  bias: number;
  inputWeightNorm: number;
  outputWeightNorm: number;
}

export interface MlpTrace {
  inputVector: Vector;
  z1PreActivations: Vector;
  hiddenActivations: Vector;
  outputVector: Vector;
  softmaxProbabilities: Vector;
  topActivatedNeurons: NeuronTrace[];
}

function assertVector(name: string, value: Vector): void {
  if (!Array.isArray(value) || value.length === 0 || value.some((x) => !Number.isFinite(x))) {
    throw new Error(`${name} must be a non-empty finite numeric vector`);
  }
}

function assertMatrix(name: string, value: Matrix): void {
  if (!Array.isArray(value) || value.length === 0) {
    throw new Error(`${name} must be a non-empty matrix`);
  }
  const width = value[0]?.length ?? 0;
  if (width === 0) {
    throw new Error(`${name} rows must be non-empty`);
  }
  for (const row of value) {
    if (!Array.isArray(row) || row.length !== width || row.some((x) => !Number.isFinite(x))) {
      throw new Error(`${name} must be rectangular and finite numeric`);
    }
  }
}

export function dot(a: Vector, b: Vector): number {
  if (a.length !== b.length) {
    throw new Error(`dot dimension mismatch: ${a.length} vs ${b.length}`);
  }
  let total = 0;
  for (let index = 0; index < a.length; index += 1) {
    total += a[index] * b[index];
  }
  return total;
}

export function matVec(matrix: Matrix, input: Vector): Vector {
  assertMatrix("matrix", matrix);
  assertVector("input", input);
  if (matrix[0].length !== input.length) {
    throw new Error(`matVec mismatch: matrix width ${matrix[0].length}, input ${input.length}`);
  }
  return matrix.map((row) => dot(row, input));
}

export function add(a: Vector, b: Vector): Vector {
  if (a.length !== b.length) {
    throw new Error(`add dimension mismatch: ${a.length} vs ${b.length}`);
  }
  return a.map((value, index) => value + b[index]);
}

export function norm(value: Vector): number {
  return Math.sqrt(dot(value, value));
}

export function cosine(a: Vector, b: Vector): number {
  const denom = norm(a) * norm(b);
  return denom === 0 ? 0 : dot(a, b) / denom;
}

export function relu(value: number): number {
  return Math.max(0, value);
}

export function gelu(value: number): number {
  const c = Math.sqrt(2 / Math.PI);
  return 0.5 * value * (1 + Math.tanh(c * (value + 0.044715 * value ** 3)));
}

function activate(value: number, name: ActivationName): number {
  if (name === "relu") return relu(value);
  if (name === "gelu") return gelu(value);
  if (name === "linear") return value;
  throw new Error(`unsupported activation: ${name}`);
}

export function softmax(logits: Vector): Vector {
  assertVector("logits", logits);
  const maxLogit = Math.max(...logits);
  const exps = logits.map((value) => Math.exp(value - maxLogit));
  const denom = exps.reduce((sum, value) => sum + value, 0);
  return exps.map((value) => value / denom);
}

export function forwardMlp(input: Vector, params: MlpParams, topK = 6): MlpTrace {
  assertVector("input", input);
  assertMatrix("W1", params.W1);
  assertMatrix("W2", params.W2);
  assertVector("b1", params.b1);
  assertVector("b2", params.b2);
  if (params.W1.length !== params.b1.length) {
    throw new Error("W1 row count must equal b1 length");
  }
  if (params.W2.length !== params.b2.length) {
    throw new Error("W2 row count must equal b2 length");
  }
  if (params.W2[0].length !== params.W1.length) {
    throw new Error("W2 width must equal W1 hidden size");
  }

  const z1PreActivations = add(matVec(params.W1, input), params.b1);
  const hiddenActivations = z1PreActivations.map((value) => activate(value, params.activation));
  const outputVector = add(matVec(params.W2, hiddenActivations), params.b2);
  const softmaxProbabilities = softmax(outputVector);

  const topActivatedNeurons = z1PreActivations
    .map((preActivation, index) => {
      const outputColumn = params.W2.map((row) => row[index]);
      return {
        index,
        preActivation,
        activation: hiddenActivations[index],
        bias: params.b1[index],
        inputWeightNorm: norm(params.W1[index]),
        outputWeightNorm: norm(outputColumn),
      };
    })
    .sort((a, b) => Math.abs(b.activation) - Math.abs(a.activation))
    .slice(0, topK);

  return {
    inputVector: input,
    z1PreActivations,
    hiddenActivations,
    outputVector,
    softmaxProbabilities,
    topActivatedNeurons,
  };
}
