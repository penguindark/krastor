// krastor-effect: ring spectrogram
// Circular audio-reactive ribbons (cyan + magenta mesh strands) driven by the
// microphone spectrum. u_spectrum: x = time ring (newest at u_spec_head),
// y = 64 log-frequency bands (0 = low). The engine prepends the uniform
// contract and declares `frag_color`; this file only defines main().

// Sample the spectrum ring at frequency row `y` and time offset `s` (0 = now).
float spec_at(float head, float y, float s)
{
    return texture(u_spectrum, vec2(fract(head - s), clamp(y, 0.0, 1.0))).r;
}

// One ribbon: sums a few frequency rows and time lags into soft strands.
// `y0` = base frequency row, `rad` = base radius, `flow` = time flow speed.
float ribbon(vec2 p, float ang, float head, float y0, float rad,
             float flow, float amp, float thick)
{
    float d = 0.0;
    for (int k = 0; k < 4; ++k) {
        float fk = float(k);
        float lag = fk * 0.02 + flow;
        float y = clamp(y0 + fk * 0.018 + 0.02 * sin(ang * 2.0 + u_time * 0.3 + fk),
                        0.0, 1.0);
        float b = spec_at(head, y, ang / 6.2831853 * 0.35 + lag);
        d += b * amp * (1.0 - fk * 0.18);
    }
    d *= 0.35;
    // thin angular ripple gives the woven mesh look
    float rr = rad + d + 0.005 * sin(ang * 26.0 + u_time * 0.6 + y0 * 40.0)
                    + 0.003 * sin(ang * 47.0 - u_time * 0.4);
    float e = (length(p) - rr) / thick;
    return exp(-e * e);
}

void main()
{
    vec2 p = (gl_FragCoord.xy - 0.5 * u_resolution) / min(u_resolution.x, u_resolution.y);
    float ang = atan(p.y, p.x);
    float r = length(p);
    float head = u_spec_head;

    // dark base with a soft radial gradient
    vec3 col = mix(vec3(0.055, 0.062, 0.095), vec3(0.028, 0.030, 0.050),
                   smoothstep(0.0, 0.9, r));

    // Four woven strands (two per colour) at slightly different radii and
    // frequency bands. The engine's u_intensity (Settings brightness) scales
    // the final result, so keep the base strands thin and below saturation.
    float boost = 0.55 + 1.3 * u_level;
    float i_c1 = ribbon(p, ang, head, 0.10, 0.285, u_time * 0.05, 0.16, 0.016);
    float i_c2 = ribbon(p, ang, head, 0.22, 0.310, u_time * 0.04, 0.13, 0.013);
    float i_m1 = ribbon(p, ang, head, 0.45, 0.335, u_time * 0.04, 0.16, 0.016);
    float i_m2 = ribbon(p, ang, head, 0.60, 0.360, u_time * 0.05, 0.13, 0.013);
    col += vec3(0.45, 0.95, 1.05) * (i_c1 + 0.7 * i_c2) * boost;
    col += vec3(1.05, 0.42, 0.70) * (i_m1 + 0.7 * i_m2) * boost;

    // halo around the ring, breathing with the input level
    // (h*h instead of pow(h,2.0): pow with a negative base is undefined GLSL)
    float h = (r - 0.322) / 0.12;
    float halo = exp(-h * h);
    col += vec3(0.10, 0.16, 0.28) * halo * (0.30 + 1.0 * u_level);

    // listening pulse: subtle ring highlight while hands-free is active
    col += vec3(0.05, 0.09, 0.14) * u_listening
         * (0.5 + 0.5 * sin(u_time * 2.0)) * halo;
    // speaking glow
    col += vec3(0.09, 0.10, 0.17) * u_speaking * halo;

    frag_color = vec4(col, 1.0);
}
