// SPDX-FileCopyrightText: Copyright 2026 satperm
// SPDX-License-Identifier: GPL-2.0-or-later
// Experiment: keeps the GPU from idling, to test whether macOS's GPU performance controller clocks
// the GPU down (P13 -> P1, and keeps it there) because a CPU-bound game leaves it idle ~5% of the
// time.
//
//   swiftc -O tools/gpu_probes/gpu_fill.swift -o out/tools/gpu_fill
//   out/tools/gpu_fill <seconds> [iterations per thread, default 2000] [command buffers in flight, default 1]
//
// Each command buffer is one small compute dispatch (64 threads x <iterations> loop steps); the
// next one is committed when it completes, so the GPU always has a little work queued.
import Foundation
import Metal

let seconds = CommandLine.arguments.count > 1 ? Double(CommandLine.arguments[1]) ?? 60 : 60
let iterations = CommandLine.arguments.count > 2 ? UInt32(CommandLine.arguments[2]) ?? 2000 : 2000
let inFlight = CommandLine.arguments.count > 3 ? Int(CommandLine.arguments[3]) ?? 1 : 1

let source = """
#include <metal_stdlib>
using namespace metal;
kernel void fill(device uint* out [[buffer(0)]], constant uint& steps [[buffer(1)]],
                 uint id [[thread_position_in_grid]]) {
    uint x = id + 1;
    for (uint i = 0; i < steps; ++i) { x = x * 1664525u + 1013904223u; }
    out[id] = x;
}
"""
guard let device = MTLCreateSystemDefaultDevice(), let queue = device.makeCommandQueue() else { exit(1) }
let library = try device.makeLibrary(source: source, options: nil)
let pipeline = try device.makeComputePipelineState(function: library.makeFunction(name: "fill")!)
let out = device.makeBuffer(length: 64 * 4, options: .storageModePrivate)!
var steps = iterations
let stepsBuffer = device.makeBuffer(bytes: &steps, length: 4, options: .storageModeShared)!
let end = Date().addingTimeInterval(seconds)
var committed = 0
let lock = NSLock()

func submit() {
    guard Date() < end, let buffer = queue.makeCommandBuffer(), let encoder = buffer.makeComputeCommandEncoder() else { return }
    encoder.setComputePipelineState(pipeline)
    encoder.setBuffer(out, offset: 0, index: 0)
    encoder.setBuffer(stepsBuffer, offset: 0, index: 1)
    encoder.dispatchThreads(MTLSize(width: 64, height: 1, depth: 1), threadsPerThreadgroup: MTLSize(width: 64, height: 1, depth: 1))
    encoder.endEncoding()
    buffer.addCompletedHandler { _ in
        lock.lock(); committed += 1; lock.unlock()
        submit()
    }
    buffer.commit()
}
for _ in 0..<inFlight { submit() }
while Date() < end { Thread.sleep(forTimeInterval: 0.25) }
Thread.sleep(forTimeInterval: 0.2)
print("gpu_fill: \(committed) command buffers in \(Int(seconds)) s (\(iterations) steps, \(inFlight) in flight)")
