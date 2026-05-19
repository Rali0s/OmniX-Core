# FreeRTOS and Firmware Communications Research Track

## Summary

OmniX should keep a lightweight future track for FreeRTOS, firmware, low-level communications, and bus-aware diagnostics. This is research-first: no embedded toolchain, RTOS dependency, or hardware interface becomes part of core OmniX until the local evidence model, safety gates, and replay story are clear.

Guiding principle: lower-level does not mean less disciplined. Firmware and bus work should be local-first, bounded, replayable, and evidence-driven before any action touches a device.

## Direction

- Treat FreeRTOS as the first named RTOS research target for tasks, queues, timers, interrupts, heap, stack high-water marks, and scheduler-aware diagnostics.
- Prepare for lower-level communications such as UART, I2C, SPI, CAN, USB serial, GPIO event traces, MQTT edge devices, and board-local telemetry.
- Keep the first OmniX bridge file-based: captured logs, serial transcripts, firmware build output, bus traces, memory maps, and structured JSON evidence.
- Reuse existing OmniX concepts where possible: TZE replay/report, Recursive Why/Diff, GSMg signals, thresholds, `gg` packet-transit evidence, and neural advisory routing.
- Defer direct flashing, live bus mutation, device reset, or destructive firmware actions until explicit operator approval and hardware safety policy exist.

## Future Use Cases

- Diagnose a FreeRTOS task stall from serial logs, task state dumps, stack high-water marks, and watchdog reset evidence.
- Explain whether a sensor failure is likely firmware, bus wiring/noise, driver timing, queue backpressure, heap pressure, or upstream network transit.
- Convert serial/bus captures into local evidence objects that can flow into thresholds, GSMg, TView-style review, and Recursive Why/Diff.
- Build a safe firmware incident bundle with timestamps, board profile, firmware version, bus traces, task snapshots, commands run, and recommended next action.

## Boundaries

- No FreeRTOS SDK, cross-compiler, flashing tool, or hardware driver dependency in core OmniX for this phase.
- No automatic firmware flashing, device reset, bus writes, or packet mutation.
- No claim that OmniX understands a board until it has local evidence: board profile, firmware version, logs/traces, and operator-provided context.

## Test Plan

- Definition tests should eventually resolve `FreeRTOS`, `firmware communications`, `bus trace`, and `RTOS task diagnostics`.
- Future fixture tests should ingest a serial log or task dump and produce a non-destructive diagnosis.
- Future threshold/GSMg tests should classify bus/firmware signals without requiring hardware.
- Existing `cmake --build build -j4` and `./build/omnix_tests` must remain green because this track starts as docs/goals only.
