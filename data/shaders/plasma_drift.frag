// krastor-effect: Plasma Drift
// Slow plasma field with a second harmonic that only appears while the LLM
// is thinking; key presses inject a bright ripple, speaking pushes the whole
// field brighter. Audio level widens the plasma bands so mic input still
// reads through even without a spectrogram.

float field(vec2 p, float t, float bands, float warp) {
    float v = sin(p.x * bands + t);
    v += sin(p.y * bands * 1.3 - t * 0.8);
    v += sin((p.x + p.y) * bands * 0.7 + t * 0.6);
    v += warp * sin(length(p) * bands * 2.1 - t * 1.4);
    return v / (3.0 + warp);
}

void main() {
    vec2 p = (gl_FragCoord.xy - 0.5 * u_resolution) / min(u_resolution.x, u_resolution.y);
    float t = u_time * (0.25 + 0.9 * u_thinking);

    float f = field(p, t, 1.6 + 1.4 * u_level, 0.0);
    // the thinking harmonic: a second, tighter layer that fades in
    float g = field(p * 1.9 + 0.4, t * 1.7, 2.2, 1.0) * u_thinking;

    vec3 col = mix(vec3(0.028, 0.030, 0.052), vec3(0.085, 0.105, 0.175),
                   0.5 + 0.5 * f);
    col += vec3(0.26, 0.16, 0.42) * (0.5 + 0.5 * g) * u_thinking;

    // key press: sharp ripple travelling from the left edge
    float wv = (p.x + 1.2 - 2.2 * u_typing) / 0.06;
    col += vec3(0.35, 0.60, 1.00) * exp(-wv * wv) * u_typing;

    // speaking: overall lift plus a warm cast
    col += vec3(0.10, 0.09, 0.05) * u_speaking * (0.6 + 0.4 * f);
    // listening: cool vertical scan
    col += vec3(0.05, 0.12, 0.18) * u_listening
         * (0.5 + 0.5 * sin(p.y * 9.0 - u_time * 2.0));

    frag_color = vec4(col, 1.0);
}
