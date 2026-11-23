// SPDX-License-Identifier: GPL-2.0-or-later
// tlg: composite of half-resolution particles (vk_particles_half.cpp). The half-size image holds
// the particles' accumulated colour (rgb, premultiplied) and the scene's remaining
// transmittance (a); blended as scene = scene * a + rgb (src One, dst SrcAlpha).
#version 450

layout(set = 0, binding = 0) uniform sampler2D particles;
layout(location = 0) out vec4 color;

void main() {
    const vec2 size = vec2(textureSize(particles, 0));
    color = texture(particles, gl_FragCoord.xy * 0.5 / size);
}
