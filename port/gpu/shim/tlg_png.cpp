// SPDX-License-Identifier: GPL-2.0-or-later
#include "tlg_png.h"

#include <cstdio>
#include <algorithm>
#include <cstring>

#ifdef __APPLE__
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#else
extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}
#endif

namespace TlgPng {

namespace {
constexpr int kMaxSide = 4096; // save icons are 228x128; anything far larger is not one
}

bool DecodeRgba(const unsigned char* data, size_t size, Image& out) {
    out = {};
    if (!data || !size || size > 64u << 20) {
        return false;
    }
#ifdef __APPLE__
    // tlg: ImageIO on macOS. The FFmpeg build has no PNG decoder (--disable-autodetect leaves zlib
    // out), so the in-game save list showed no pictures (png-decode-test failed, 2026-11-17).
    bool ok = false;
    CFDataRef bytes = CFDataCreate(nullptr, data, CFIndex(size));
    CGImageSourceRef source = bytes ? CGImageSourceCreateWithData(bytes, nullptr) : nullptr;
    CGImageRef picture = source && CGImageSourceGetStatus(source) == kCGImageStatusComplete
                             ? CGImageSourceCreateImageAtIndex(source, 0, nullptr)
                             : nullptr;
    if (picture) {
        const size_t width = CGImageGetWidth(picture), height = CGImageGetHeight(picture);
        if (width > 0 && height > 0 && width <= size_t(kMaxSide) && height <= size_t(kMaxSide)) {
            out.width = int(width);
            out.height = int(height);
            out.rgba.assign(width * height * 4, 0);
            CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
            // Unpremultiplied RGBA is not a bitmap context format: draw premultiplied, then undo it.
            CGContextRef context = CGBitmapContextCreate(
                out.rgba.data(), width, height, 8, width * 4, space,
                uint32_t(kCGImageAlphaPremultipliedLast) | uint32_t(kCGBitmapByteOrder32Big));
            if (context) {
                CGContextSetBlendMode(context, kCGBlendModeCopy);
                CGContextDrawImage(context, CGRectMake(0, 0, CGFloat(width), CGFloat(height)),
                                   picture);
                CGContextRelease(context);
                for (size_t i = 0; i < out.rgba.size(); i += 4) {
                    const unsigned a = out.rgba[i + 3];
                    if (a != 0 && a != 255) {
                        for (size_t c = 0; c < 3; ++c) {
                            out.rgba[i + c] = (unsigned char)std::min(255u, (out.rgba[i + c] * 255u + a / 2) / a);
                        }
                    }
                }
                ok = true;
            }
            CGColorSpaceRelease(space);
        }
        CGImageRelease(picture);
    }
    if (source) {
        CFRelease(source);
    }
    if (bytes) {
        CFRelease(bytes);
    }
    if (!ok) {
        out = {};
    }
    return ok;
#else
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_PNG);
    AVCodecContext* context = codec ? avcodec_alloc_context3(codec) : nullptr;
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    bool ok = context && packet && frame && avcodec_open2(context, codec, nullptr) == 0 &&
              av_new_packet(packet, int(size)) == 0;
    if (ok) {
        std::memcpy(packet->data, data, size);
        ok = avcodec_send_packet(context, packet) == 0 && avcodec_send_packet(context, nullptr) == 0 &&
             avcodec_receive_frame(context, frame) == 0 && frame->width > 0 && frame->height > 0 &&
             frame->width <= kMaxSide && frame->height <= kMaxSide;
    }
    if (ok) {
        SwsContext* sws = sws_getContext(frame->width, frame->height, AVPixelFormat(frame->format),
                                         frame->width, frame->height, AV_PIX_FMT_RGBA, SWS_POINT,
                                         nullptr, nullptr, nullptr);
        ok = sws != nullptr;
        if (ok) {
            out.width = frame->width;
            out.height = frame->height;
            out.rgba.resize(size_t(out.width) * out.height * 4);
            uint8_t* planes[4] = {out.rgba.data(), nullptr, nullptr, nullptr};
            const int strides[4] = {out.width * 4, 0, 0, 0};
            ok = sws_scale(sws, frame->data, frame->linesize, 0, frame->height, planes, strides) ==
                 frame->height;
            sws_freeContext(sws);
        }
    }
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&context);
    if (!ok) {
        out = {};
    }
    return ok;
#endif
}

bool LoadRgba(const char* path, Image& out) {
    out = {};
    FILE* file = path ? std::fopen(path, "rb") : nullptr;
    if (!file) {
        return false;
    }
    std::vector<unsigned char> bytes;
    unsigned char chunk[65536];
    size_t n;
    while ((n = std::fread(chunk, 1, sizeof(chunk), file)) > 0 && bytes.size() <= (64u << 20)) {
        bytes.insert(bytes.end(), chunk, chunk + n);
    }
    std::fclose(file);
    return DecodeRgba(bytes.data(), bytes.size(), out);
}

} // namespace TlgPng
