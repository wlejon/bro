// =============================================================================
// mat4.js — 4x4 matrix + quaternion utilities for the scene-editor object
// hierarchy. Plain JS, no engine deps; all data as typed arrays of floats.
//
// Conventions match bro's internal Quat/Mat4:
//   - Quaternions are [x, y, z, w], Hamilton convention.
//   - Euler order is Z * Y * X (roll / pitch / yaw), same as Quat::fromEuler
//     in src/scene/scene_node.h, so round-trip to scene-node rotationX/Y/Z
//     is lossless (outside singularities).
//   - Matrices are column-major Float32Array(16): m[col*4 + row], matching
//     src/scene/scene_node.h Mat4.
// =============================================================================

(function (global) {
    'use strict';

    const Q_ID = [0, 0, 0, 1];

    function identity() {
        return new Float32Array([1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1]);
    }

    // Build a TRS matrix: M = T * R * S.
    function fromTRS(t, q, s) {
        const x=q[0], y=q[1], z=q[2], w=q[3];
        const xx=x*x, yy=y*y, zz=z*z;
        const xy=x*y, xz=x*z, yz=y*z;
        const wx=w*x, wy=w*y, wz=w*z;
        const sx=s[0], sy=s[1], sz=s[2];
        const m = new Float32Array(16);
        m[0]  = (1 - 2*(yy+zz)) * sx;
        m[1]  = (2*(xy + wz))   * sx;
        m[2]  = (2*(xz - wy))   * sx;
        m[3]  = 0;
        m[4]  = (2*(xy - wz))   * sy;
        m[5]  = (1 - 2*(xx+zz)) * sy;
        m[6]  = (2*(yz + wx))   * sy;
        m[7]  = 0;
        m[8]  = (2*(xz + wy))   * sz;
        m[9]  = (2*(yz - wx))   * sz;
        m[10] = (1 - 2*(xx+yy)) * sz;
        m[11] = 0;
        m[12] = t[0];
        m[13] = t[1];
        m[14] = t[2];
        m[15] = 1;
        return m;
    }

    // r = a * b (column-major, so applies b to a vector first). Writes into
    // `r` (can alias neither a nor b). Returns r.
    function multiplyInto(r, a, b) {
        const a00=a[0],  a01=a[1],  a02=a[2],  a03=a[3];
        const a10=a[4],  a11=a[5],  a12=a[6],  a13=a[7];
        const a20=a[8],  a21=a[9],  a22=a[10], a23=a[11];
        const a30=a[12], a31=a[13], a32=a[14], a33=a[15];
        for (let i = 0; i < 4; i++) {
            const b0=b[i*4+0], b1=b[i*4+1], b2=b[i*4+2], b3=b[i*4+3];
            r[i*4+0] = a00*b0 + a10*b1 + a20*b2 + a30*b3;
            r[i*4+1] = a01*b0 + a11*b1 + a21*b2 + a31*b3;
            r[i*4+2] = a02*b0 + a12*b1 + a22*b2 + a32*b3;
            r[i*4+3] = a03*b0 + a13*b1 + a23*b2 + a33*b3;
        }
        return r;
    }
    function multiply(a, b) { return multiplyInto(new Float32Array(16), a, b); }

    // Transform a position vector [x,y,z] by a TRS matrix. Returns [x,y,z].
    function transformPoint(m, v) {
        const x=v[0], y=v[1], z=v[2];
        return [
            m[0]*x + m[4]*y + m[8]*z  + m[12],
            m[1]*x + m[5]*y + m[9]*z  + m[13],
            m[2]*x + m[6]*y + m[10]*z + m[14],
        ];
    }

    // Transform a direction (no translation). Does NOT rescale; non-uniform
    // scale in m will distort the direction (callers who need unit output
    // should renormalize).
    function transformDir(m, v) {
        const x=v[0], y=v[1], z=v[2];
        return [
            m[0]*x + m[4]*y + m[8]*z,
            m[1]*x + m[5]*y + m[9]*z,
            m[2]*x + m[6]*y + m[10]*z,
        ];
    }

    // Transform a *unit* normal correctly under possibly non-uniform scale.
    // Uses the inverse-transpose of the upper-3x3. Input should be unit; the
    // output is renormalized.
    function transformNormal(m, n) {
        // Inverse-transpose: for each axis column of M's 3x3, dot with n,
        // then divide by corresponding scale^2. This is equivalent to
        // (M^-T) * n for TRS matrices (T doesn't affect directions).
        // For the general case, just compute explicit inverse of 3x3.
        const inv = invert3x3(m);
        if (!inv) return [n[0], n[1], n[2]];
        // (M^-1)^T * n = n * M^-1 (row-vector mul)
        const nx = inv[0]*n[0] + inv[1]*n[1] + inv[2]*n[2];
        const ny = inv[3]*n[0] + inv[4]*n[1] + inv[5]*n[2];
        const nz = inv[6]*n[0] + inv[7]*n[1] + inv[8]*n[2];
        const L = Math.hypot(nx, ny, nz) || 1;
        return [nx/L, ny/L, nz/L];
    }

    // Inverse of a TRS matrix (rotation orthonormal * scale, then translate).
    // Robust to non-uniform scale but assumes no shear.
    function invertTRS(m) {
        // Extract scale^2 per axis from the rotation-scale columns.
        const sx2 = m[0]*m[0] + m[1]*m[1] + m[2]*m[2];
        const sy2 = m[4]*m[4] + m[5]*m[5] + m[6]*m[6];
        const sz2 = m[8]*m[8] + m[9]*m[9] + m[10]*m[10];
        if (sx2 < 1e-20 || sy2 < 1e-20 || sz2 < 1e-20) return identity();
        // inverse of 3x3 TRS upper = (R * S)^-1 = S^-1 * R^T
        //   col i of result = R_row_i / scale_i
        // But R_row_i isn't directly available without decomposing; compute
        // via the identity: (R*S)^-1_ij = (R^T)_ij / S_jj = (R_ji) / S_jj.
        // We have the R*S matrix; each column i is s_i * R_col_i. So
        // R_col_i = column_i / scale_i. Then R^T row i = R_col_i.
        const r = new Float32Array(16);
        const sx = Math.sqrt(sx2), sy = Math.sqrt(sy2), sz = Math.sqrt(sz2);
        // rotation columns
        const rx0=m[0]/sx, rx1=m[1]/sx, rx2=m[2]/sx;
        const ry0=m[4]/sy, ry1=m[5]/sy, ry2=m[6]/sy;
        const rz0=m[8]/sz, rz1=m[9]/sz, rz2=m[10]/sz;
        // (R^T) in column-major, scaled by 1/s per row -> per column of R^T:
        r[0]  = rx0 / sx; r[1]  = ry0 / sy; r[2]  = rz0 / sz; r[3]  = 0;
        r[4]  = rx1 / sx; r[5]  = ry1 / sy; r[6]  = rz1 / sz; r[7]  = 0;
        r[8]  = rx2 / sx; r[9]  = ry2 / sy; r[10] = rz2 / sz; r[11] = 0;
        // translation: -M_inv_3x3 * t
        const tx=m[12], ty=m[13], tz=m[14];
        r[12] = -(r[0]*tx + r[4]*ty + r[8] *tz);
        r[13] = -(r[1]*tx + r[5]*ty + r[9] *tz);
        r[14] = -(r[2]*tx + r[6]*ty + r[10]*tz);
        r[15] = 1;
        return r;
    }

    // Invert the upper-3x3 of a 4x4 matrix. Returns a 9-element Float32Array
    // (row-major, [r0c0,r0c1,r0c2, r1c0,...]) or null if singular.
    function invert3x3(m) {
        const a=m[0],  b=m[1],  c=m[2];
        const d=m[4],  e=m[5],  f=m[6];
        const g=m[8],  h=m[9],  i=m[10];
        const A = e*i - f*h, B = f*g - d*i, C = d*h - e*g;
        const det = a*A + b*B + c*C;
        if (Math.abs(det) < 1e-20) return null;
        const inv = 1 / det;
        const out = new Float32Array(9);
        out[0] = A * inv;
        out[1] = (c*h - b*i) * inv;
        out[2] = (b*f - c*e) * inv;
        out[3] = B * inv;
        out[4] = (a*i - c*g) * inv;
        out[5] = (c*d - a*f) * inv;
        out[6] = C * inv;
        out[7] = (b*g - a*h) * inv;
        out[8] = (a*e - b*d) * inv;
        return out;
    }

    // Decompose a TRS matrix into { translation, rotation (quat), scale }.
    // Preserves sign: if the upper-3x3 has negative determinant (mirror), the
    // X scale is flipped so the rotation remains a proper rotation.
    function decomposeTRS(m) {
        const t = [m[12], m[13], m[14]];
        let sx = Math.hypot(m[0], m[1], m[2]);
        const sy = Math.hypot(m[4], m[5], m[6]);
        const sz = Math.hypot(m[8], m[9], m[10]);
        const det = m[0]*(m[5]*m[10] - m[6]*m[9])
                  - m[4]*(m[1]*m[10] - m[2]*m[9])
                  + m[8]*(m[1]*m[6]  - m[2]*m[5]);
        if (det < 0) sx = -sx;
        const isx = sx !== 0 ? 1/sx : 0;
        const isy = sy !== 0 ? 1/sy : 0;
        const isz = sz !== 0 ? 1/sz : 0;
        // Rotation matrix columns
        const r00 = m[0]*isx, r10 = m[1]*isx, r20 = m[2]*isx;
        const r01 = m[4]*isy, r11 = m[5]*isy, r21 = m[6]*isy;
        const r02 = m[8]*isz, r12 = m[9]*isz, r22 = m[10]*isz;
        // Quat from rotation matrix (Shoemake). Column-major read.
        const tr = r00 + r11 + r22;
        let qx, qy, qz, qw;
        if (tr > 0) {
            const S = Math.sqrt(tr + 1) * 2;
            qw = 0.25 * S;
            qx = (r21 - r12) / S;
            qy = (r02 - r20) / S;
            qz = (r10 - r01) / S;
        } else if (r00 > r11 && r00 > r22) {
            const S = Math.sqrt(1 + r00 - r11 - r22) * 2;
            qw = (r21 - r12) / S;
            qx = 0.25 * S;
            qy = (r01 + r10) / S;
            qz = (r02 + r20) / S;
        } else if (r11 > r22) {
            const S = Math.sqrt(1 + r11 - r00 - r22) * 2;
            qw = (r02 - r20) / S;
            qx = (r01 + r10) / S;
            qy = 0.25 * S;
            qz = (r12 + r21) / S;
        } else {
            const S = Math.sqrt(1 + r22 - r00 - r11) * 2;
            qw = (r10 - r01) / S;
            qx = (r02 + r20) / S;
            qy = (r12 + r21) / S;
            qz = 0.25 * S;
        }
        return { translation: t, rotation: [qx, qy, qz, qw], scale: [sx, sy, sz] };
    }

    // Quaternion extraction matching bro's Quat::toEuler (src/scene/scene_node.h).
    // Round-trips cleanly with scene_node.rotationX/Y/Z setters.
    function quatToEuler(q) {
        const x=q[0], y=q[1], z=q[2], w=q[3];
        const sinr_cosp = 2 * (w*x + y*z);
        const cosr_cosp = 1 - 2 * (x*x + y*y);
        const rx = Math.atan2(sinr_cosp, cosr_cosp);
        const sinp = 2 * (w*y - z*x);
        const ry = Math.abs(sinp) >= 1
            ? Math.sign(sinp) * Math.PI * 0.5
            : Math.asin(sinp);
        const siny_cosp = 2 * (w*z + x*y);
        const cosy_cosp = 1 - 2 * (y*y + z*z);
        const rz = Math.atan2(siny_cosp, cosy_cosp);
        return [rx, ry, rz];
    }

    // --- Quaternion primitives -------------------------------------------

    function quatMul(a, b) {
        return [
            a[3]*b[0] + a[0]*b[3] + a[1]*b[2] - a[2]*b[1],
            a[3]*b[1] - a[0]*b[2] + a[1]*b[3] + a[2]*b[0],
            a[3]*b[2] + a[0]*b[1] - a[1]*b[0] + a[2]*b[3],
            a[3]*b[3] - a[0]*b[0] - a[1]*b[1] - a[2]*b[2],
        ];
    }
    function quatNorm(q) {
        const m = Math.hypot(q[0], q[1], q[2], q[3]) || 1;
        return [q[0]/m, q[1]/m, q[2]/m, q[3]/m];
    }
    function quatConj(q) { return [-q[0], -q[1], -q[2], q[3]]; }
    function quatRotVec(q, v) {
        const x=q[0], y=q[1], z=q[2], w=q[3];
        const vx=v[0], vy=v[1], vz=v[2];
        const tx = 2*(y*vz - z*vy);
        const ty = 2*(z*vx - x*vz);
        const tz = 2*(x*vy - y*vx);
        return [
            vx + w*tx + (y*tz - z*ty),
            vy + w*ty + (z*tx - x*tz),
            vz + w*tz + (x*ty - y*tx),
        ];
    }

    global.Mat4Lib = {
        identity, fromTRS, multiply, multiplyInto,
        transformPoint, transformDir, transformNormal,
        invertTRS, invert3x3, decomposeTRS, quatToEuler,
        quatMul, quatNorm, quatConj, quatRotVec,
        Q_ID,
    };

})(typeof globalThis !== 'undefined' ? globalThis : this);
