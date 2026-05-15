#!/usr/bin/env bash
set -euo pipefail

PYTHON_BIN="${PYTHON:-python3}"

if ! command -v "$PYTHON_BIN" >/dev/null 2>&1; then
  echo "omnix_tensorflow_env: unavailable"
  echo "python: missing ($PYTHON_BIN)"
  echo "next: install Python plus TensorFlow/Numpy before enabling the TensorFlow adapter."
  exit 1
fi

"$PYTHON_BIN" - <<'PY'
import importlib.util
import platform
import sys

modules = ["tensorflow", "numpy", "sklearn", "torch"]
missing = []
print("python:", platform.python_version(), sys.executable)
for name in modules:
    status = "available" if importlib.util.find_spec(name) is not None else "missing"
    print(f"{name}: {status}")
    if name in {"tensorflow", "numpy"} and status == "missing":
        missing.append(name)

if missing:
    print("omnix_tensorflow_env: unavailable")
    print("next: create a supported TensorFlow/Numpy environment before enabling the optional TensorFlow adapter.")
    sys.exit(1)

print("omnix_tensorflow_env: ready")
print("next: TensorFlow adapter experiments may run in simulation-first CPU mode.")
PY
