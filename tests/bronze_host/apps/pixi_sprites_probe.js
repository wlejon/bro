// The pixi sprites probe: a Pixi.js 2D scene testing Sprite batching and rendering.
//
// Proves that compiled JS can:
//  1. Import from bare 'pixi.js' package resolution.
//  2. Initialize Pixi Application with an explicit canvas and WebGL renderer.
//  3. Build a scene graph with Container, Sprite, and Texture.
//  4. Render 2D sprites through WebGL shaders and batching.
//  5. Verify rendered pixels via gl.readPixels.

import {
    Application,
    Container,
    Sprite,
    Texture,
    AbstractRenderer,
    UboSystem,
    GlUniformGroupSystem,
    GlUboSystem,
    GlShaderSystem,
    WGSL_TO_STD40_SIZE,
    uniformParsers,
    BufferResource,
    UniformGroup,
    TextureSource,
    TextureStyle
} from 'pixi.js';

// --- Unsafe-Eval Polyfills for AOT / CSP Environment ----------------------
// pixi's default shader systems build their uniform-sync functions with
// `new Function` — runtime codegen an AOT-compiled program cannot do. pixi
// ships the official escape hatch as @pixi/unsafe-eval: static functions
// assigned over the same prototype seams. These are that package's parsers,
// written out against pixi v8.19.0's system surface — app code, because the
// seam pixi defines for it IS application-level prototype assignment.
const uniformSingleParserFunctions = {
    f32(name, cu, cv, v, ud, _uv, gl) {
        if (cv !== v) {
            cu.value = v;
            gl.uniform1f(ud[name].location, v);
        }
    },
    "vec2<f32>"(name, _cu, cv, v, ud, _uv, gl) {
        if (cv[0] !== v[0] || cv[1] !== v[1]) {
            cv[0] = v[0];
            cv[1] = v[1];
            gl.uniform2f(ud[name].location, v[0], v[1]);
        }
    },
    "vec3<f32>"(name, _cu, cv, v, ud, _uv, gl) {
        if (cv[0] !== v[0] || cv[1] !== v[1] || cv[2] !== v[2]) {
            cv[0] = v[0];
            cv[1] = v[1];
            cv[2] = v[2];
            gl.uniform3f(ud[name].location, v[0], v[1], v[2]);
        }
    },
    "vec4<f32>"(name, _cu, cv, v, ud, _uv, gl) {
        if (cv[0] !== v[0] || cv[1] !== v[1] || cv[2] !== v[2] || cv[3] !== v[3]) {
            cv[0] = v[0];
            cv[1] = v[1];
            cv[2] = v[2];
            cv[3] = v[3];
            gl.uniform4f(ud[name].location, v[0], v[1], v[2], v[3]);
        }
    },
    i32(name, cu, cv, v, ud, _uv, gl) {
        if (cv !== v) {
            cu.value = v;
            gl.uniform1i(ud[name].location, v);
        }
    },
    "vec2<i32>"(name, _cu, cv, v, ud, _uv, gl) {
        if (cv[0] !== v[0] || cv[1] !== v[1]) {
            cv[0] = v[0];
            cv[1] = v[1];
            gl.uniform2i(ud[name].location, v[0], v[1]);
        }
    },
    "vec3<i32>"(name, _cu, cv, v, ud, _uv, gl) {
        if (cv[0] !== v[0] || cv[1] !== v[1] || cv[2] !== v[2]) {
            cv[0] = v[0];
            cv[1] = v[1];
            cv[2] = v[2];
            gl.uniform3i(ud[name].location, v[0], v[1], v[2]);
        }
    },
    "vec4<i32>"(name, _cu, cv, v, ud, _uv, gl) {
        if (cv[0] !== v[0] || cv[1] !== v[1] || cv[2] !== v[2] || cv[3] !== v[3]) {
            cv[0] = v[0];
            cv[1] = v[1];
            cv[2] = v[2];
            cv[3] = v[3];
            gl.uniform4i(ud[name].location, v[0], v[1], v[2], v[3]);
        }
    },
    u32(name, cu, cv, v, ud, _uv, gl) {
        if (cv !== v) {
            cu.value = v;
            gl.uniform1ui(ud[name].location, v);
        }
    },
    "vec2<u32>"(name, _cu, cv, v, ud, _uv, gl) {
        if (cv[0] !== v[0] || cv[1] !== v[1]) {
            cv[0] = v[0];
            cv[1] = v[1];
            gl.uniform2ui(ud[name].location, v[0], v[1]);
        }
    },
    "vec3<u32>"(name, _cu, cv, v, ud, _uv, gl) {
        if (cv[0] !== v[0] || cv[1] !== v[1] || cv[2] !== v[2]) {
            cv[0] = v[0];
            cv[1] = v[1];
            cv[2] = v[2];
            gl.uniform3ui(ud[name].location, v[0], v[1], v[2]);
        }
    },
    "vec4<u32>"(name, _cu, cv, v, ud, _uv, gl) {
        if (cv[0] !== v[0] || cv[1] !== v[1] || cv[2] !== v[2] || cv[3] !== v[3]) {
            cv[0] = v[0];
            cv[1] = v[1];
            cv[2] = v[2];
            cv[3] = v[3];
            gl.uniform4ui(ud[name].location, v[0], v[1], v[2], v[3]);
        }
    },
    bool(name, cu, cv, v, ud, _uv, gl) {
        if (cv !== v) {
            cu.value = v;
            gl.uniform1i(ud[name].location, v);
        }
    },
    "vec2<bool>"(name, _cu, cv, v, ud, _uv, gl) {
        if (cv[0] !== v[0] || cv[1] !== v[1]) {
            cv[0] = v[0];
            cv[1] = v[1];
            gl.uniform2i(ud[name].location, v[0], v[1]);
        }
    },
    "vec3<bool>"(name, _cu, cv, v, ud, _uv, gl) {
        if (cv[0] !== v[0] || cv[1] !== v[1] || cv[2] !== v[2]) {
            cv[0] = v[0];
            cv[1] = v[1];
            cv[2] = v[2];
            gl.uniform3i(ud[name].location, v[0], v[1], v[2]);
        }
    },
    "vec4<bool>"(name, _cu, cv, v, ud, _uv, gl) {
        if (cv[0] !== v[0] || cv[1] !== v[1] || cv[2] !== v[2] || cv[3] !== v[3]) {
            cv[0] = v[0];
            cv[1] = v[1];
            cv[2] = v[2];
            cv[3] = v[3];
            gl.uniform4i(ud[name].location, v[0], v[1], v[2], v[3]);
        }
    },
    "mat2x2<f32>"(name, _cu, _cv, v, ud, _uv, gl) {
        gl.uniformMatrix2fv(ud[name].location, false, v);
    },
    "mat3x3<f32>"(name, _cu, _cv, v, ud, _uv, gl) {
        gl.uniformMatrix3fv(ud[name].location, false, v);
    },
    "mat4x4<f32>"(name, _cu, _cv, v, ud, _uv, gl) {
        gl.uniformMatrix4fv(ud[name].location, false, v);
    }
};

const uniformArrayParserFunctions = {
    f32(name, _cu, _cv, v, ud, _uv, gl) {
        gl.uniform1fv(ud[name].location, v);
    },
    "vec2<f32>"(name, _cu, _cv, v, ud, _uv, gl) {
        gl.uniform2fv(ud[name].location, v);
    },
    "vec3<f32>"(name, _cu, _cv, v, ud, _uv, gl) {
        gl.uniform3fv(ud[name].location, v);
    },
    "vec4<f32>"(name, _cu, _cv, v, ud, _uv, gl) {
        gl.uniform4fv(ud[name].location, v);
    },
    "mat2x2<f32>"(name, _cu, _cv, v, ud, _uv, gl) {
        gl.uniformMatrix2fv(ud[name].location, false, v);
    },
    "mat3x3<f32>"(name, _cu, _cv, v, ud, _uv, gl) {
        gl.uniformMatrix3fv(ud[name].location, false, v);
    },
    "mat4x4<f32>"(name, _cu, _cv, v, ud, _uv, gl) {
        gl.uniformMatrix4fv(ud[name].location, false, v);
    },
    i32(name, _cu, _cv, v, ud, _uv, gl) {
        gl.uniform1iv(ud[name].location, v);
    },
    "vec2<i32>"(name, _cu, _cv, v, ud, _uv, gl) {
        gl.uniform2iv(ud[name].location, v);
    },
    "vec3<i32>"(name, _cu, _cv, v, ud, _uv, gl) {
        gl.uniform3iv(ud[name].location, v);
    },
    "vec4<i32>"(name, _cu, _cv, v, ud, _uv, gl) {
        gl.uniform4iv(ud[name].location, v);
    },
    u32(name, _cu, _cv, v, ud, _uv, gl) {
        gl.uniform1iv(ud[name].location, v);
    },
    "vec2<u32>"(name, _cu, _cv, v, ud, _uv, gl) {
        gl.uniform2iv(ud[name].location, v);
    },
    "vec3<u32>"(name, _cu, _cv, v, ud, _uv, gl) {
        gl.uniform3iv(ud[name].location, v);
    },
    "vec4<u32>"(name, _cu, _cv, v, ud, _uv, gl) {
        gl.uniform4iv(ud[name].location, v);
    },
    bool(name, _cu, _cv, v, ud, _uv, gl) {
        gl.uniform1iv(ud[name].location, v);
    },
    "vec2<bool>"(name, _cu, _cv, v, ud, _uv, gl) {
        gl.uniform2iv(ud[name].location, v);
    },
    "vec3<bool>"(name, _cu, _cv, v, ud, _uv, gl) {
        gl.uniform3iv(ud[name].location, v);
    },
    "vec4<bool>"(name, _cu, _cv, v, ud, _uv, gl) {
        gl.uniform4iv(ud[name].location, v);
    }
};

const uniformParserFunctions = [
    (name, _cu, _cv, _v, ud, uv, gl) => {
        gl.uniformMatrix3fv(ud[name].location, false, uv[name].toArray(true));
    },
    (name, _cu, cv, v, ud, uv, gl) => {
        cv = ud[name].value;
        v = uv[name];
        if (cv[0] !== v.x || cv[1] !== v.y || cv[2] !== v.width || cv[3] !== v.height) {
            cv[0] = v.x;
            cv[1] = v.y;
            cv[2] = v.width;
            cv[3] = v.height;
            gl.uniform4f(ud[name].location, v.x, v.y, v.width, v.height);
        }
    },
    (name, _cu, cv, v, ud, uv, gl) => {
        cv = ud[name].value;
        v = uv[name];
        if (cv[0] !== v.x || cv[1] !== v.y) {
            cv[0] = v.x;
            cv[1] = v.y;
            gl.uniform2f(ud[name].location, v.x, v.y);
        }
    },
    (name, _cu, cv, v, ud, uv, gl) => {
        cv = ud[name].value;
        v = uv[name];
        if (cv[0] !== v.red || cv[1] !== v.green || cv[2] !== v.blue || cv[3] !== v.alpha) {
            cv[0] = v.red;
            cv[1] = v.green;
            cv[2] = v.blue;
            cv[3] = v.alpha;
            gl.uniform4f(ud[name].location, v.red, v.green, v.blue, v.alpha);
        }
    },
    (name, _cu, cv, v, ud, uv, gl) => {
        cv = ud[name].value;
        v = uv[name];
        if (cv[0] !== v.red || cv[1] !== v.green || cv[2] !== v.blue) {
            cv[0] = v.red;
            cv[1] = v.green;
            cv[2] = v.blue;
            gl.uniform3f(ud[name].location, v.red, v.green, v.blue);
        }
    }
];

function generateUniformsSyncPolyfill(group, uniformData) {
    const functionMap = {};
    for (const i in group.uniformStructures) {
        if (!uniformData[i]) continue;
        const uniform = group.uniformStructures[i];
        let parsed = false;
        for (let j = 0; j < uniformParsers.length; j++) {
            const parser = uniformParsers[j];
            if (uniform.type === parser.type && parser.test(uniform)) {
                functionMap[i] = uniformParserFunctions[j];
                parsed = true;
                break;
            }
        }
        if (!parsed) {
            const templateType = uniform.size === 1 ? uniformSingleParserFunctions : uniformArrayParserFunctions;
            functionMap[i] = templateType[uniform.type];
        }
    }
    return (ud, uv, renderer) => {
        const gl = renderer.gl;
        for (const i in functionMap) {
            const v = uv[i];
            const cu = ud[i];
            const cv = ud[i].value;
            functionMap[i](i, cu, cv, v, ud, uv, gl);
        }
    };
}

const uboParserFunctions = [
    (name, data, offset, uv, _v) => {
        const matrix = uv[name].toArray(true);
        data[offset] = matrix[0];
        data[offset + 1] = matrix[1];
        data[offset + 2] = matrix[2];
        data[offset + 4] = matrix[3];
        data[offset + 5] = matrix[4];
        data[offset + 6] = matrix[5];
        data[offset + 8] = matrix[6];
        data[offset + 9] = matrix[7];
        data[offset + 10] = matrix[8];
    },
    (name, data, offset, uv, v) => {
        v = uv[name];
        data[offset] = v.x;
        data[offset + 1] = v.y;
        data[offset + 2] = v.width;
        data[offset + 3] = v.height;
    },
    (name, data, offset, uv, v) => {
        v = uv[name];
        data[offset] = v.x;
        data[offset + 1] = v.y;
    },
    (name, data, offset, uv, v) => {
        v = uv[name];
        data[offset] = v.red;
        data[offset + 1] = v.green;
        data[offset + 2] = v.blue;
        data[offset + 3] = v.alpha;
    },
    (name, data, offset, uv, v) => {
        v = uv[name];
        data[offset] = v.red;
        data[offset + 1] = v.green;
        data[offset + 2] = v.blue;
    }
];

const uboSingleFunctionsSTD40 = {
    f32: (_name, data, offset, _uv, v) => {
        data[offset] = v;
    },
    i32: (_name, data, offset, _uv, v) => {
        data[offset] = v;
    },
    "vec2<f32>": (_name, data, offset, _uv, v) => {
        data[offset] = v[0];
        data[offset + 1] = v[1];
    },
    "vec3<f32>": (_name, data, offset, _uv, v) => {
        data[offset] = v[0];
        data[offset + 1] = v[1];
        data[offset + 2] = v[2];
    },
    "vec4<f32>": (_name, data, offset, _uv, v) => {
        data[offset] = v[0];
        data[offset + 1] = v[1];
        data[offset + 2] = v[2];
        data[offset + 3] = v[3];
    },
    "mat2x2<f32>": (_name, data, offset, _uv, v) => {
        data[offset] = v[0];
        data[offset + 1] = v[1];
        data[offset + 4] = v[2];
        data[offset + 5] = v[3];
    },
    "mat3x3<f32>": (_name, data, offset, _uv, v) => {
        data[offset] = v[0];
        data[offset + 1] = v[1];
        data[offset + 2] = v[2];
        data[offset + 4] = v[3];
        data[offset + 5] = v[4];
        data[offset + 6] = v[5];
        data[offset + 8] = v[6];
        data[offset + 9] = v[7];
        data[offset + 10] = v[8];
    },
    "mat4x4<f32>": (_name, data, offset, _uv, v) => {
        for (let i = 0; i < 16; i++) {
            data[offset + i] = v[i];
        }
    }
};

function generateUboSyncPolyfillSTD40(uboElements) {
    const functionMap = {};
    for (const i in uboElements) {
        const uboElement = uboElements[i];
        const uniform = uboElement.data;
        let parsed = false;
        functionMap[uniform.name] = {
            offset: uboElement.offset / 4,
            func: null
        };
        for (let j = 0; j < uniformParsers.length; j++) {
            const parser = uniformParsers[j];
            if (uniform.type === parser.type && parser.test(uniform)) {
                functionMap[uniform.name].func = uboParserFunctions[j];
                parsed = true;
                break;
            }
        }
        if (!parsed) {
            if (uniform.size === 1) {
                functionMap[uniform.name].func = uboSingleFunctionsSTD40[uniform.type];
            } else {
                const rowSize = Math.max(WGSL_TO_STD40_SIZE[uboElement.data.type] / 16, 1);
                const elementSize = uboElement.data.value.length / uboElement.data.size;
                const remainder = (4 - elementSize % 4) % 4;
                functionMap[uniform.name].func = (_name, data, offset, _uv, v) => {
                    let t = 0;
                    for (let i = 0; i < uboElement.data.size * rowSize; i++) {
                        for (let j = 0; j < elementSize; j++) {
                            data[offset++] = v[t++];
                        }
                        offset += remainder;
                    }
                };
            }
        }
    }
    return (uniforms, data, offset) => {
        for (const i in functionMap) {
            functionMap[i].func(i, data, offset + functionMap[i].offset, uniforms, uniforms[i]);
        }
    };
}

function syncShader(renderer, shader, syncData) {
    const gl = renderer.gl;
    const shaderSystem = renderer.shader;
    const programData = shaderSystem._getProgramData(shader.glProgram);
    for (const i in shader.groups) {
        const bindGroup = shader.groups[i];
        for (const j in bindGroup.resources) {
            const resource = bindGroup.resources[j];
            if (resource instanceof UniformGroup) {
                if (resource.ubo) {
                    shaderSystem.bindUniformBlock(
                        resource,
                        shader._uniformBindMap[i][j],
                        syncData.blockIndex++
                    );
                } else {
                    shaderSystem.updateUniformGroup(resource);
                }
            } else if (resource instanceof BufferResource) {
                shaderSystem.bindUniformBlock(
                    resource,
                    shader._uniformBindMap[i][j],
                    syncData.blockIndex++
                );
            } else if (resource instanceof TextureSource) {
                renderer.texture.bind(resource, syncData.textureCount);
                const uniformName = shader._uniformBindMap[i][j];
                const uniformData = programData.uniformData[uniformName];
                if (uniformData) {
                    if (uniformData.value !== syncData.textureCount) {
                        gl.uniform1i(uniformData.location, syncData.textureCount);
                    }
                    syncData.textureCount++;
                }
            } else if (resource instanceof TextureStyle) {
            }
        }
    }
}

function generateShaderSyncPolyfill() {
    return syncShader;
}

// Apply CSP / AOT safe polyfills to PixiJS prototypes
Object.assign(AbstractRenderer.prototype, {
    _unsafeEvalCheck() {}
});
Object.assign(UboSystem.prototype, {
    _systemCheck() {}
});
Object.assign(GlUniformGroupSystem.prototype, {
    _generateUniformsSync: generateUniformsSyncPolyfill
});
Object.assign(GlUboSystem.prototype, {
    _generateUboSync: generateUboSyncPolyfillSTD40
});
Object.assign(GlShaderSystem.prototype, {
    _generateShaderSync: generateShaderSyncPolyfill
});

const WIDTH = 320;
const HEIGHT = 240;

function say(label, value) {
    console.log('APP ' + label + '=' + value);
}

// --- Canvas Setup ---------------------------------------------------------
const canvas = document.createElement('canvas');
canvas.width = WIDTH;
canvas.height = HEIGHT;
document.body.appendChild(canvas);

say('canvas.width', canvas.width);
say('canvas.height', canvas.height);

// --- Pixi Application Init ------------------------------------------------
const app = new Application();

app.init({
    canvas: canvas,
    width: WIDTH,
    height: HEIGHT,
    preference: 'webgl',
    autoStart: false
}).then(function () {
    say('pixi.initialized', true);

    // --- Scene Setup ------------------------------------------------------
    const container = new Container();
    app.stage.addChild(container);

    const sprite = new Sprite(Texture.WHITE);
    sprite.width = 64;
    sprite.height = 64;
    sprite.x = 128;
    sprite.y = 88;
    sprite.tint = 0x00ff00;
    container.addChild(sprite);

    say('pixi.stageChildren', app.stage.children.length === 1);
    say('pixi.containerChildren', container.children.length === 1);

    // --- Frame Loop & State Verification ----------------------------------
    const gl = canvas.getContext('webgl2') || canvas.getContext('webgl');
    const pixels = new Uint8Array(WIDTH * HEIGHT * 4);

    let frame = 0;

    function checkPixels(isFinal) {
        if (!gl) return;
        gl.readPixels(0, 0, WIDTH, HEIGHT, gl.RGBA, gl.UNSIGNED_BYTE, pixels);
        const centerIdx = (120 * WIDTH + 160) * 4;
        const cr = pixels[centerIdx], cg = pixels[centerIdx + 1], cb = pixels[centerIdx + 2];

        if (!isFinal) {
            say('pixel.spriteLit', (cg > 100));
        } else {
            say('pixel.rendered', (cg > 100 && cr < 50));
        }
    }

    function tick() {
        app.render();

        if (frame === 1) {
            checkPixels(false);
        }

        frame = frame + 1;
        if (frame === 5) {
            checkPixels(true);
            say('done', 1);
        }

        requestAnimationFrame(tick);
    }

    requestAnimationFrame(tick);
    say('ready', 1);
}).catch(function (err) {
    console.error('Pixi init failed:', err);
});
