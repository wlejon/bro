// Test: varying interpolation, per-pixel color mixing

varying vec3 vColor;
varying float vHeight;

void main() {
  // Height-based color: blue (low) -> green (mid) -> white (high)
  vec3 heightColor;
  if (vHeight < 0.5) {
    heightColor = mix(vec3(0.1, 0.2, 0.8), vec3(0.2, 0.8, 0.3), vHeight * 2.0);
  } else {
    heightColor = mix(vec3(0.2, 0.8, 0.3), vec3(1.0, 1.0, 1.0), (vHeight - 0.5) * 2.0);
  }

  // Blend with custom vertex color
  vec3 final = mix(heightColor, vColor, 0.3);

  gl_FragColor = vec4(final, 1.0);
}
