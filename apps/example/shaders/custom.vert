// Test: custom attributes, uint math, floatBitsToUint, vertex displacement

attribute vec3 aCustomPos;
attribute vec4 aCustomColor;

uniform float uTime;
uniform float uScale;

varying vec3 vColor;
varying float vHeight;

// --- Same integer hash noise as planet shader ---
uint ihash(uint n) {
  n = (n << 13u) ^ n;
  n = n * (n * n * 15731u + 789221u) + 1376312589u;
  return n;
}

vec3 hash3(vec3 p) {
  uvec3 u = uvec3(floatBitsToUint(p.x), floatBitsToUint(p.y), floatBitsToUint(p.z));
  uint seed = ihash(u.x + ihash(u.y + ihash(u.z)));
  return vec3(
    float(ihash(seed     )) / 4294967295.0 * 2.0 - 1.0,
    float(ihash(seed + 1u)) / 4294967295.0 * 2.0 - 1.0,
    float(ihash(seed + 2u)) / 4294967295.0 * 2.0 - 1.0
  );
}

float noise3d(vec3 p) {
  vec3 i = floor(p);
  vec3 f = fract(p);
  vec3 u = f * f * (3.0 - 2.0 * f);
  float n000 = dot(hash3(i + vec3(0,0,0)), f - vec3(0,0,0));
  float n100 = dot(hash3(i + vec3(1,0,0)), f - vec3(1,0,0));
  float n010 = dot(hash3(i + vec3(0,1,0)), f - vec3(0,1,0));
  float n110 = dot(hash3(i + vec3(1,1,0)), f - vec3(1,1,0));
  float n001 = dot(hash3(i + vec3(0,0,1)), f - vec3(0,0,1));
  float n101 = dot(hash3(i + vec3(1,0,1)), f - vec3(1,0,1));
  float n011 = dot(hash3(i + vec3(0,1,1)), f - vec3(0,1,1));
  float n111 = dot(hash3(i + vec3(1,1,1)), f - vec3(1,1,1));
  return mix(
    mix(mix(n000, n100, u.x), mix(n010, n110, u.x), u.y),
    mix(mix(n001, n101, u.x), mix(n011, n111, u.x), u.y),
    u.z
  );
}

void main() {
  // Use custom attribute as base position
  vec3 pos = position + aCustomPos * 0.1;

  // Displace Y by noise (same pattern as planet terrain displacement)
  float n = noise3d(pos * 3.0 + vec3(0.0, uTime, 0.0));
  pos.y += n * uScale;
  vHeight = n * 0.5 + 0.5;

  // Pass custom color varying
  vColor = aCustomColor.rgb * aCustomColor.a;

  gl_Position = projectionMatrix * modelViewMatrix * vec4(pos, 1.0);
}
