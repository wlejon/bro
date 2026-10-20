#version 450
layout(location = 0) in vec2 aCorner;
layout(location = 1) in vec3 aCenter;
layout(location = 2) in vec3 aScale;
layout(location = 3) in vec4 aQuat;
layout(location = 4) in vec4 aColor;

layout(location = 0) out vec2 vPos;
layout(location = 1) out vec4 vColor;

layout(set = 0, binding = 0) uniform SplatUBO {
    mat4 uModel;
    mat4 uView;
    mat4 uProj;
    vec2 uFocal;
    vec2 uViewport;
} ubo;

const float kSigma = 3.0;

mat3 quatToMat3(vec4 q) {
    float x = q.x, y = q.y, z = q.z, w = q.w;
    float xx = x*x, yy = y*y, zz = z*z;
    float xy = x*y, xz = x*z, yz = y*z;
    float wx = w*x, wy = w*y, wz = w*z;
    return mat3(
        1.0-2.0*(yy+zz), 2.0*(xy+wz),     2.0*(xz-wy),
        2.0*(xy-wz),     1.0-2.0*(xx+zz), 2.0*(yz+wx),
        2.0*(xz+wy),     2.0*(yz-wx),     1.0-2.0*(xx+yy));
}

void main() {
    vec4 cam = ubo.uView * (ubo.uModel * vec4(aCenter, 1.0));
    vec4 clip = ubo.uProj * cam;
    if (cam.z > -0.01 || clip.w <= 0.0) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        return;
    }

    mat3 R = quatToMat3(aQuat);
    mat3 S = mat3(aScale.x, 0.0, 0.0,
                  0.0, aScale.y, 0.0,
                  0.0, 0.0, aScale.z);
    mat3 M = mat3(ubo.uModel) * (R * S);
    mat3 Sigma = M * transpose(M);

    float zz = cam.z * cam.z;
    mat3 J = mat3(
        ubo.uFocal.x / cam.z, 0.0, 0.0,
        0.0, ubo.uFocal.y / cam.z, 0.0,
        -(ubo.uFocal.x * cam.x) / zz, -(ubo.uFocal.y * cam.y) / zz, 0.0);

    mat3 W = mat3(ubo.uView);
    mat3 T = J * W;
    mat3 cov = T * Sigma * transpose(T);

    float a = cov[0][0] + 0.3;
    float b = cov[1][0];
    float d = cov[1][1] + 0.3;
    float det = a * d - b * b;
    if (det <= 0.0) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        return;
    }

    float mid = 0.5 * (a + d);
    float disc = sqrt(max(0.0, mid * mid - det));
    float l1 = mid + disc;
    float l2 = mid - disc;

    vec2 ev = abs(l1 - a) >= abs(l1 - d) ? vec2(b, l1 - a) : vec2(l1 - d, b);
    vec2 e1 = dot(ev, ev) > 0.0 ? normalize(ev)
                                : (a >= d ? vec2(1.0, 0.0) : vec2(0.0, 1.0));
    vec2 e2 = vec2(-e1.y, e1.x);
    vec2 axisMajor = e1 * (kSigma * sqrt(l1));
    vec2 axisMinor = e2 * (kSigma * sqrt(max(0.0, l2)));

    vec2 offsetPx = aCorner.x * axisMajor + aCorner.y * axisMinor;
    vec2 offsetNdc = offsetPx * 2.0 / ubo.uViewport;

    vec3 ndc = clip.xyz / clip.w;
    gl_Position = vec4(ndc.xy + offsetNdc, ndc.z, 1.0);

    vPos = aCorner * kSigma;
    vColor = aColor;
}
