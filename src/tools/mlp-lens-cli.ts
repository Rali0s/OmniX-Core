import { forwardMlp, type MlpParams } from "./mlp-lens";

function demoParams(): MlpParams {
  return {
    activation: "gelu",
    W1: [
      [0.8, -0.2, 0.1, 0.4],
      [-0.1, 0.9, 0.3, -0.7],
      [0.5, 0.5, -0.4, 0.2],
      [-0.3, 0.1, 0.8, 0.6],
      [0.2, -0.6, 0.7, -0.1],
      [0.9, 0.1, -0.2, 0.3],
    ],
    b1: [0.1, -0.2, 0, 0.3, -0.1, 0.2],
    W2: [
      [0.3, -0.1, 0.6, 0.2, -0.2, 0.5],
      [-0.4, 0.7, 0.1, -0.3, 0.8, 0.2],
      [0.2, 0.3, -0.5, 0.9, 0.1, -0.4],
      [0.6, -0.2, 0.4, 0.1, -0.7, 0.3],
    ],
    b2: [0, 0.1, -0.1, 0],
  };
}

function toyEmbed(text: string): number[] {
  const values = [0, 0, 0, 0];
  for (let index = 0; index < text.length; index += 1) {
    const code = text.charCodeAt(index);
    values[index % values.length] += ((code % 31) - 15) / 15;
  }
  const denom = Math.sqrt(values.reduce((sum, value) => sum + value * value, 0)) || 1;
  return values.map((value) => value / denom);
}

export function runMlpLens(args: string[]): void {
  const verbose = args.includes("--verbose");
  const prompt = args.filter((arg) => arg !== "--verbose").join(" ").trim();
  if (!prompt) {
    console.error('usage: omnix tool mlp-lens -- "<text>"');
    process.exit(2);
  }

  const trace = forwardMlp(toyEmbed(prompt), demoParams(), 6);
  const output: Record<string, unknown> = {
    tool: "mlp-lens",
    prompt,
    mode: verbose ? "verbose" : "compact",
    warning:
      "Educational toy embedding and demo weights only; this is not a real LLM or real model-weight trace unless real tensors are loaded.",
    ...trace,
    modelSource: "demo_weights",
    adapterStatus: "inactive: safetensors, ONNX, GGUF, and extracted transformer weights are future adapters",
  };

  if (verbose) {
    output.trace = {
      formula: "MLP(x) = W2 * GELU(W1 * x + b1) + b2",
      activation: "gelu",
      matrixShapes: { W1: [6, 4], b1: [6], W2: [4, 6], b2: [4] },
      formulaSteps: [
        "inputVector = deterministic toy embedding(text)",
        "z1PreActivations = W1 * inputVector + b1",
        "hiddenActivations = GELU(z1PreActivations)",
        "outputVector = W2 * hiddenActivations + b2",
        "softmaxProbabilities = softmax(outputVector)",
      ],
    };
  }

  console.log(JSON.stringify(output));
}

declare const require: { main?: unknown } | undefined;
declare const module: unknown;
declare const process: { argv: string[]; exit(code?: number): never };

if (typeof require !== "undefined" && require.main === module) {
  runMlpLens(process.argv.slice(2));
}
