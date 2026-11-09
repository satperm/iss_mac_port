#import <Metal/Metal.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>
static const char* src = R"(
#include <metal_stdlib>
using namespace metal;
kernel void heavy(device float* d [[buffer(0)]], constant uint& it [[buffer(1)]], uint i [[thread_position_in_grid]]) {
  float x = float(i) * 1e-6; for (uint k = 0; k < it; ++k) x = x * 1.0000001 + 0.5 * sin(x); d[i & 65535] = x; }
kernel void tiny(device float* d [[buffer(0)]], constant uint& it [[buffer(1)]], uint i [[thread_position_in_threadgroup]]) {
  float x = d[i]; for (uint k = 0; k < it; ++k) x = x * 0.999 + 1.0; d[i] = x; }
// fragment-heavy full-screen pass
struct V { float4 p [[position]]; };
vertex V vs(uint id [[vertex_id]]) { float2 uv = float2((id << 1) & 2, id & 2); V v; v.p = float4(uv * 2 - 1, 0, 1); return v; }
fragment float4 fs(V in [[stage_in]], constant uint& it [[buffer(0)]]) { float x = in.p.x * 1e-3; for (uint k = 0; k < it; ++k) x = x * 1.0000001 + 0.5 * sin(x); return float4(x); }
)";
int main(int argc, char** argv) {
  const uint heavy_it = argc > 1 ? atoi(argv[1]) : 15000, n_tiny = argc > 2 ? atoi(argv[2]) : 500, tiny_it = 20;
  const uint frag_it = argc > 3 ? atoi(argv[3]) : 400;
  id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
  NSError* e = nil;
  id<MTLLibrary> lib = [dev newLibraryWithSource:@(src) options:nil error:&e];
  if (!lib) { printf("%s\n", e.localizedDescription.UTF8String); return 1; }
  id<MTLComputePipelineState> ph = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"heavy"] error:&e];
  id<MTLComputePipelineState> pt = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"tiny"] error:&e];
  MTLRenderPipelineDescriptor* rd = [MTLRenderPipelineDescriptor new];
  rd.vertexFunction = [lib newFunctionWithName:@"vs"]; rd.fragmentFunction = [lib newFunctionWithName:@"fs"];
  rd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
  id<MTLRenderPipelineState> pr = [dev newRenderPipelineStateWithDescriptor:rd error:&e];
  MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float width:1920 height:1080 mipmapped:NO];
  td.usage = MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModePrivate;
  id<MTLTexture> rt = [dev newTextureWithDescriptor:td];
  id<MTLBuffer> b0 = [dev newBufferWithLength:65536*4 options:MTLResourceStorageModePrivate];
  id<MTLBuffer> b1 = [dev newBufferWithLength:65536*4 options:MTLResourceStorageModePrivate];
  id<MTLCommandQueue> q0 = [dev newCommandQueue], q1 = [dev newCommandQueue];
  auto mkA = [&](id<MTLCommandQueue> q, bool frag) {
    id<MTLCommandBuffer> cb = [q commandBuffer];
    if (frag) {
      for (int pass = 0; pass < 20; ++pass) {
        MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = rt; rp.colorAttachments[0].loadAction = MTLLoadActionLoad; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
        [re setRenderPipelineState:pr]; [re setFragmentBytes:&frag_it length:4 atIndex:0];
        [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; [re endEncoding];
      }
    } else {
      id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
      [ce setComputePipelineState:ph]; [ce setBuffer:b0 offset:0 atIndex:0]; [ce setBytes:&heavy_it length:4 atIndex:1];
      [ce dispatchThreadgroups:MTLSizeMake(4096,1,1) threadsPerThreadgroup:MTLSizeMake(64,1,1)]; [ce endEncoding];
    }
    return cb;
  };
  auto mkB = [&](id<MTLCommandQueue> q) {
    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
    [ce setComputePipelineState:pt]; [ce setBuffer:b1 offset:0 atIndex:0]; [ce setBytes:&tiny_it length:4 atIndex:1];
    for (uint i = 0; i < n_tiny; ++i) [ce dispatchThreadgroups:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(64,1,1)];
    [ce endEncoding];
    return cb;
  };
  using clk = std::chrono::steady_clock;
  auto ms = [](clk::time_point a, clk::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
  // Two MTLBuffers over the same host memory: tracked for queue 0, untracked for queue 1.
  const size_t sz = 64<<20; void* mem = nullptr; posix_memalign(&mem, 16384, sz); memset(mem, 0, sz);
  id<MTLBuffer> tracked = [dev newBufferWithBytesNoCopy:mem length:sz options:MTLResourceStorageModeShared | MTLResourceHazardTrackingModeTracked deallocator:nil];
  id<MTLBuffer> alias = [dev newBufferWithBytesNoCopy:mem length:sz options:MTLResourceStorageModeShared | MTLResourceHazardTrackingModeUntracked deallocator:nil];
  printf("wrapped twice: %p %p\n", (__bridge void*)tracked, (__bridge void*)alias);
  const uint big_it = 3000;
  for (int rep = 0; rep < 5; ++rep) @autoreleasepool {
    std::vector<id<MTLCommandBuffer>> as;
    for (int k = 0; k < 30; ++k) {
      id<MTLCommandBuffer> a = [q0 commandBuffer];
      id<MTLComputeCommandEncoder> ce = [a computeCommandEncoder];
      [ce setComputePipelineState:ph]; [ce setBuffer:tracked offset:0 atIndex:0]; [ce setBytes:&big_it length:4 atIndex:1];
      [ce dispatchThreadgroups:MTLSizeMake(4096,1,1) threadsPerThreadgroup:MTLSizeMake(64,1,1)]; [ce endEncoding];
      [a commit]; as.push_back(a);
    }
    ((float*)mem)[1000000] = 1.0f;
    auto t0 = clk::now();
    id<MTLCommandBuffer> b = [q1 commandBuffer];
    id<MTLComputeCommandEncoder> ce = [b computeCommandEncoder];
    [ce setComputePipelineState:pt];
    [ce setBuffer:alias offset:4000000 atIndex:0]; [ce setBytes:&tiny_it length:4 atIndex:1];
    [ce dispatchThreadgroups:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(64,1,1)]; [ce endEncoding];
    [b commit]; [b waitUntilCompleted]; auto t1 = clk::now();
    const float seen = ((float*)mem)[1000000]; // the tiny shader adds to d[0] through the alias
    for (auto& a : as) [a waitUntilCompleted];
    auto t2 = clk::now();
    if (rep) printf("alias: tiny on second queue %.2f ms, queue 0 total %.2f ms; value through the alias %.3f (CPU wrote 1)\n", ms(t0,t1), ms(t0,t2), seen);
  }
}
