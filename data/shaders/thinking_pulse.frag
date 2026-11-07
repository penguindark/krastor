// krastor-effect: Thinking Pulse
// Ambient drifting waves with a breathing core: slow and dim while idle,
// faster and violet while the LLM thinks, expanding ring on key presses,
// warm glow while speaking, cool tint while listening. Uses the full event
// contract (see src/effects.h); the engine prepends the uniforms, applies
// u_intensity and declares frag_color.

void main() {
    vec2 p = (gl_FragCoord.xy - 0.5 * u_resolution) / min(u_resolution.x, u_resolution.y);
    float r = length(p);

    // base: deep space with slow diagonal drift, quicker while thinking
    float speed = 0.2 + 1.1 * u_thinking;
    float d = 0.5 + 0.5 * sin(p.x * 1.3 - p.y * 0.7 + u_time * speed * 0.6);
    vec3 col = mix(vec3(0.030, 0.035, 0.060), vec3(0.075, 0.095, 0.160), d);

    // breathing halo: slow blue when idle, faster violet while thinking
    float breathe = 0.5 + 0.5 * sin(u_time * (0.6 + 1.6 * u_thinking));
    float hr = r / (0.55 + 0.15 * breathe);
    float halo = exp(-hr * hr);
    vec3 think_col = mix(vec3(0.10, 0.14, 0.30), vec3(0.35, 0.22, 0.55), u_thinking);
    col += think_col * halo * (0.25 + 0.55 * breathe) * (0.5 + 0.9 * u_thinking);

    // typing: an expanding ring pulse (starts at the core, flies outward)
    float tr = (r - 0.30 - 0.55 * (1.0 - u_typing)) / 0.05;
    col += vec3(0.30, 0.55, 0.95) * exp(-tr * tr) * u_typing * 0.8;

    // speaking: warm glow rising from the bottom; listening: cool top tint
    float sy = (p.y + 0.6) / 0.5;
    col += vec3(0.35, 0.22, 0.10) * u_speaking
         * (0.5 + 0.5 * sin(u_time * 3.0)) * exp(-sy * sy);
    float ly = (p.y - 0.7) / 0.5;
    col += vec3(0.06, 0.16, 0.22) * u_listening
         * (0.5 + 0.5 * sin(u_time * 2.0)) * exp(-ly * ly);

    frag_color = vec4(col, 1.0);
}
