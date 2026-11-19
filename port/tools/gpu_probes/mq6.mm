// SPDX-License-Identifier: GPL-2.0-or-later
// Does the order of many one-workgroup dispatches matter on an Apple GPU? ISS's particle chain issues ~530 a
// frame, fixed sequences of 4+ different shaders per particle system, unordered between the game's sync points.
// Measures GPU time of N independent dispatches in one compute encoder: shaders interleaved (k0 k1 k2 k3 k0 ...)
// versus grouped by shader (k0 k0 ... k1 k1 ...), concurrent and serial encoders.
//   clang++ -std=c++20 -O2 -fobjc-arc mq6.mm -framework Metal -framework Foundation -o mq6 && ./mq6 [N] [iterations]
#import <Metal/Metal.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>
static const char* src = R"(
#include <metal_stdlib>
using namespace metal;
#define K(name, op) kernel void name(device float* d [[buffer(0)]], constant uint& base [[buffer(1)]], constant uint& it [[buffer(2)]], uint i [[thread_position_in_threadgroup]]) { \
  float x = d[base + i]; for (uint k = 0; k < it; ++k) x = op; d[base + i] = x; }
K(k0, x * 0.999f + 1.0f)
K(k1, x * 1.001f - 0.5f)
K(k2, fma(x, 0.998f, 0.25f))
K(k3, x * 0.997f + sin(x) * 0.01f)
)";
int main(int argc, char** argv) {
    const uint n = argc > 1 ? atoi(argv[1]) : 520, reps = argc > 2 ? atoi(argv[2]) : 30, it = argc > 3 ? atoi(argv[3]) : 20;
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    NSError* e = nil;
    id<MTLLibrary> lib = [dev newLibraryWithSource:@(src) options:nil error:&e];
    if (!lib) { printf("%s\n", e.localizedDescription.UTF8String); return 1; }
    id<MTLComputePipelineState> ps[4];
    for (int k = 0; k < 4; ++k)
        ps[k] = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:[NSString stringWithFormat:@"k%d", k]] error:&e];
    id<MTLBuffer> buf = [dev newBufferWithLength:size_t(n) * 64 * 4 options:MTLResourceStorageModePrivate];
    id<MTLCommandQueue> q = [dev newCommandQueue];
    auto run = [&](bool grouped, MTLDispatchType type) {
        std::vector<double> t;
        for (uint r = 0; r < reps; ++r) {
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoderWithDispatchType:type];
            [enc setBuffer:buf offset:0 atIndex:0];
            [enc setBytes:&it length:4 atIndex:2];
            for (uint j = 0; j < n; ++j) {
                // dispatch j: shader j % 4 (interleaved) or j * 4 / n (grouped); its own 64 floats
                const uint k = grouped ? std::min(3u, j * 4 / n) : j % 4;
                const uint base = j * 64;
                [enc setComputePipelineState:ps[k]];
                [enc setBytes:&base length:4 atIndex:1];
                [enc dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
            }
            [enc endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            if (r >= 3) t.push_back((cb.GPUEndTime - cb.GPUStartTime) * 1e3);
        }
        std::sort(t.begin(), t.end());
        return t[t.size() / 2];
    };
    printf("%u one-workgroup dispatches, %u iterations each, GPU ms (median):\n", n, it);
    printf("  concurrent interleaved %.3f, grouped %.3f\n", run(false, MTLDispatchTypeConcurrent),
           run(true, MTLDispatchTypeConcurrent));
    printf("  serial     interleaved %.3f, grouped %.3f\n", run(false, MTLDispatchTypeSerial),
           run(true, MTLDispatchTypeSerial));
    return 0;
}
