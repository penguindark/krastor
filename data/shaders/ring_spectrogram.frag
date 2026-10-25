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
    vec3 col = mix(vec3(0.040, 0.046, 0.070), vec3(0.020, 0.022, 0.036),
                   smoothstep(0.0, 0.9, r));

    float boost = 0.85 + 1.6 * u_level;
    float i_cyan = ribbon(p, ang, head, 0.10, 0.300, u_time * 0.05, 0.16, 0.026);
    float i_mag  = ribbon(p, ang, head, 0.45, 0.335, u_time * 0.04, 0.16, 0.026);
    col += vec3(0.35, 0.85, 0.95) * i_cyan * boost;
    col += vec3(0.95, 0.35, 0.60) * i_mag * boost;

    // faint halo around the ring, breathing with the input level
    float halo = exp(-pow((r - 0.315) / 0.11, 2.0));
    col += vec3(0.10, 0.16, 0.28) * halo * (0.25 + u_level);

    // listening pulse: subtle ring highlight while hands-free is active
    col += vec3(0.03, 0.05, 0.08) * u_listening
         * (0.5 + 0.5 * sin(u_time * 2.0)) * halo;
    // speaking glow
    col += vec3(0.05, 0.06, 0.10) * u_speaking * halo;

    frag_color = vec4(col, 1.0);
}
