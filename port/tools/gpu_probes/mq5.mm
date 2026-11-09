// Does the CPU see a compute shader's write while the same command buffer still runs?
#import <Metal/Metal.h>
#include <atomic>
#include <chrono>
#include <cstdio>
static const char* src = R"(
#include <metal_stdlib>
using namespace metal;
kernel void label(device uint* d [[buffer(0)]], constant uint& v [[buffer(1)]], uint i [[thread_position_in_grid]]) { if (i == 0) d[0] = v; }
kernel void heavy(device float* d [[buffer(0)]], constant uint& it [[buffer(1)]], uint i [[thread_position_in_grid]]) {
  float x = float(i) * 1e-6; for (uint k = 0; k < it; ++k) x = x * 1.0000001 + 0.5 * sin(x); d[i & 65535] = x; }
)";
int main() {
  id<MTLDevice> dev = MTLCreateSystemDefaultDevice(); NSError* e = nil;
  id<MTLLibrary> lib = [dev newLibraryWithSource:@(src) options:nil error:&e];
  id<MTLComputePipelineState> pl = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"label"] error:&e];
  id<MTLComputePipelineState> ph = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"heavy"] error:&e];
  void* mem = nullptr; posix_memalign(&mem, 16384, 1<<20); memset(mem, 0, 1<<20);
  id<MTLBuffer> buf = [dev newBufferWithBytesNoCopy:mem length:1<<20 options:MTLResourceStorageModeShared | MTLResourceHazardTrackingModeUntracked deallocator:nil];
  id<MTLBuffer> work = [dev newBufferWithLength:1<<20 options:MTLResourceStorageModePrivate];
  id<MTLCommandQueue> q = [dev newCommandQueue];
  volatile uint32_t* label = (volatile uint32_t*)mem;
  using clk = std::chrono::steady_clock;
  auto ms = [](clk::time_point a, clk::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
  const uint it0 = 2000, it1 = 30000;
  for (int rep = 1; rep <= 8; ++rep) @autoreleasepool {
    const uint v = rep;
    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoderWithDispatchType:MTLDispatchTypeConcurrent];
    [ce setComputePipelineState:ph]; [ce setBuffer:work offset:0 atIndex:0]; [ce setBytes:&it0 length:4 atIndex:1];
    [ce dispatchThreadgroups:MTLSizeMake(2048,1,1) threadsPerThreadgroup:MTLSizeMake(64,1,1)];
    [ce memoryBarrierWithScope:MTLBarrierScopeBuffers];
    [ce setComputePipelineState:pl]; [ce setBuffer:buf offset:0 atIndex:0]; [ce setBytes:&v length:4 atIndex:1];
    [ce dispatchThreads:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(1,1,1)];
    [ce memoryBarrierWithScope:MTLBarrierScopeBuffers];
    [ce setComputePipelineState:ph]; [ce setBuffer:work offset:0 atIndex:0]; [ce setBytes:&it1 length:4 atIndex:1];
    [ce dispatchThreadgroups:MTLSizeMake(2048,1,1) threadsPerThreadgroup:MTLSizeMake(64,1,1)];
    [ce endEncoding];
    auto t0 = clk::now();
    [cb commit];
    while (*label != v) {}
    auto t1 = clk::now();
    const bool done_then = cb.status >= MTLCommandBufferStatusCompleted;
    [cb waitUntilCompleted];
    auto t2 = clk::now();
    printf("label seen after %.2f ms (cb finished: %d), cb done after %.2f ms\n", ms(t0,t1), done_then, ms(t0,t2));
  }
}
