// bro_vulkan_webgl_test: the WebGL2 Vulkan backend — canvas, buffers,
// shader translation and linking, draws read back.
//
// assert() is the check here, so it must survive a Release build: NDEBUG is
// undefined before any header can pull in <cassert>. Run under
// BRO_VK_VALIDATION=1 (tests/run_tests.sh does) and any validation error
// fails the run too.
#undef NDEBUG

#include "webgl/vulkan/webgl_vk_canvas.h"
#include "webgl/vulkan/webgl_vk_context.h"
#include "webgl/webgl2_context.h"
#include "render/vulkan_context.h"
#include "render/vulkan_debug.h"
#include "render/vulkan_presenter.h"
#include "util/log.h"

#include <iostream>
#include <vector>
#include <cassert>
#include <cstring>

using namespace bro;
using namespace bro::webgl;
using namespace bro::webgl::vk;

int main() {
    std::cout << "=== bro_vulkan_webgl_test: WebGL2 on Vulkan ===" << std::endl;

    // Initialize Headless VulkanContext
    render::VulkanContextConfig cfg;
    cfg.headless = true;
    cfg.enableValidation = false;
    cfg.enableDynamicRendering = true;

    render::VulkanContext context(cfg);
    if (!context.init()) {
        std::cerr << "FAILED: Could not initialize VulkanContext" << std::endl;
        return 1;
    }

    // -------------------------------------------------------------------------
    // Test 1: WebGLVkCanvas Creation, Resizing, and Layout Transitions
    // -------------------------------------------------------------------------
    std::cout << "[Test 1] WebGLVkCanvas Creation & Resizing... " << std::flush;
    {
        WebGLVkCanvas canvas(context);
        assert(canvas.init(128, 128));
        assert(canvas.isValid());
        assert(canvas.width() == 128);
        assert(canvas.height() == 128);
        assert(canvas.colorImage() != VK_NULL_HANDLE);
        assert(canvas.colorView() != VK_NULL_HANDLE);
        assert(canvas.colorFormat() == VK_FORMAT_R8G8B8A8_UNORM);
        assert(canvas.depthImage() != VK_NULL_HANDLE);
        assert(canvas.depthView() != VK_NULL_HANDLE);

        // Test resizing
        assert(canvas.resize(256, 256));
        assert(canvas.width() == 256);
        assert(canvas.height() == 256);
        assert(canvas.colorImage() != VK_NULL_HANDLE);

        // Test layout transition helper
        VkCommandBuffer cmd = context.beginSingleTimeCommands();
        canvas.transitionToShaderRead(cmd);
        context.endSingleTimeCommands(cmd);
        assert(canvas.colorLayout() == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        canvas.cleanup();
        assert(!canvas.isValid());
        std::cout << "PASSED" << std::endl;
    }

    // -------------------------------------------------------------------------
    // Test 2: WebGLVkContext Buffer Creation & Data Upload
    // -------------------------------------------------------------------------
    std::cout << "[Test 2] WebGLVkContext Buffer Upload & SubData... " << std::flush;
    {
        WebGLVkContext ctx(64, 64, context);
        WebGLBuffer buf = ctx.createBuffer();
        assert(buf.id != 0);

        ctx.bindBuffer(GL_ARRAY_BUFFER, buf);
        assert(ctx.boundBuffer(GL_ARRAY_BUFFER) == buf.id);

        const float vertexData[] = {
            -0.5f, -0.5f, 1.0f, 0.0f, 0.0f,
             0.5f, -0.5f, 0.0f, 1.0f, 0.0f,
             0.0f,  0.5f, 0.0f, 0.0f, 1.0f
        };
        ctx.bufferData(GL_ARRAY_BUFFER, sizeof(vertexData), vertexData, GL_STATIC_DRAW);

        float readback[sizeof(vertexData) / sizeof(float)]{};
        ctx.getBufferSubData(GL_ARRAY_BUFFER, 0, readback, sizeof(vertexData));
        assert(std::memcmp(vertexData, readback, sizeof(vertexData)) == 0);

        // Test bufferSubData
        const float update[5] = { 0.1f, 0.2f, 0.3f, 0.4f, 0.5f };
        ctx.bufferSubData(GL_ARRAY_BUFFER, 0, sizeof(update), update);
        ctx.getBufferSubData(GL_ARRAY_BUFFER, 0, readback, sizeof(update));
        assert(std::memcmp(update, readback, sizeof(update)) == 0);

        ctx.deleteBuffer(buf);
        std::cout << "PASSED" << std::endl;
    }

    // -------------------------------------------------------------------------
    // Test 3: Shader Compilation & Program Linking
    // -------------------------------------------------------------------------
    std::cout << "[Test 3] WebGLVkShaderCompiler & Program Linking... " << std::flush;
    WebGLShader vertShader{};
    WebGLShader fragShader{};
    WebGLProgram program{};
    {
        WebGLVkContext ctx(64, 64, context);

        const std::string vertGlsl =
            "#version 300 es\n"
            "precision mediump float;\n"
            "in vec2 a_pos;\n"
            "in vec3 a_color;\n"
            "out vec3 v_color;\n"
            "uniform vec2 u_offset;\n"
            "void main() {\n"
            "    v_color = a_color;\n"
            "    gl_Position = vec4(a_pos + u_offset, 0.0, 1.0);\n"
            "}\n";

        const std::string fragGlsl =
            "#version 300 es\n"
            "precision mediump float;\n"
            "in vec3 v_color;\n"
            "out vec4 fragColor;\n"
            "void main() {\n"
            "    fragColor = vec4(v_color, 1.0);\n"
            "}\n";

        vertShader = ctx.createShader(GL_VERTEX_SHADER);
        ctx.shaderSource(vertShader, vertGlsl);
        ctx.compileShader(vertShader);
        assert(ctx.getShaderParameter(vertShader, GL_COMPILE_STATUS) == GL_TRUE);

        fragShader = ctx.createShader(GL_FRAGMENT_SHADER);
        ctx.shaderSource(fragShader, fragGlsl);
        ctx.compileShader(fragShader);
        assert(ctx.getShaderParameter(fragShader, GL_COMPILE_STATUS) == GL_TRUE);

        program = ctx.createProgram();
        ctx.attachShader(program, vertShader);
        ctx.attachShader(program, fragShader);
        ctx.linkProgram(program);
        assert(ctx.getProgramParameter(program, GL_LINK_STATUS) == GL_TRUE);

        GLint posLoc = ctx.getAttribLocation(program, "a_pos");
        GLint colLoc = ctx.getAttribLocation(program, "a_color");
        assert(posLoc >= 0);
        assert(colLoc >= 0);

        WebGLUniformLocation uOff = ctx.getUniformLocation(program, "u_offset");
        assert(uOff.location >= 0);

        std::cout << "PASSED" << std::endl;
    }

    // -------------------------------------------------------------------------
    // Test 4: drawArrays Triangle Rasterization & Pixel Readback
    // -------------------------------------------------------------------------
    std::cout << "[Test 4] drawArrays Triangle Rasterization & Readback... " << std::flush;
    {
        const int w = 64;
        const int h = 64;
        WebGLVkContext ctx(w, h, context);

        const std::string vertGlsl =
            "#version 300 es\n"
            "precision mediump float;\n"
            "in vec2 a_pos;\n"
            "in vec3 a_color;\n"
            "out vec3 v_color;\n"
            "uniform vec2 u_offset;\n"
            "void main() {\n"
            "    v_color = a_color;\n"
            "    gl_Position = vec4(a_pos + u_offset, 0.0, 1.0);\n"
            "}\n";

        const std::string fragGlsl =
            "#version 300 es\n"
            "precision mediump float;\n"
            "in vec3 v_color;\n"
            "out vec4 fragColor;\n"
            "void main() {\n"
            "    fragColor = vec4(v_color, 1.0);\n"
            "}\n";

        WebGLShader vs = ctx.createShader(GL_VERTEX_SHADER);
        ctx.shaderSource(vs, vertGlsl);
        ctx.compileShader(vs);
        assert(ctx.getShaderParameter(vs, GL_COMPILE_STATUS) == GL_TRUE);

        WebGLShader fs = ctx.createShader(GL_FRAGMENT_SHADER);
        ctx.shaderSource(fs, fragGlsl);
        ctx.compileShader(fs);
        assert(ctx.getShaderParameter(fs, GL_COMPILE_STATUS) == GL_TRUE);

        WebGLProgram prog = ctx.createProgram();
        ctx.attachShader(prog, vs);
        ctx.attachShader(prog, fs);
        ctx.linkProgram(prog);
        assert(ctx.getProgramParameter(prog, GL_LINK_STATUS) == GL_TRUE);

        ctx.useProgram(prog);

        WebGLUniformLocation uOff = ctx.getUniformLocation(prog, "u_offset");
        ctx.uniform2f(uOff, 0.0f, 0.0f);

        ctx.viewport(0, 0, w, h);
        // Clear background to RED (255, 0, 0, 255)
        ctx.clearColor(1.0f, 0.0f, 0.0f, 1.0f);
        ctx.clear(GL_COLOR_BUFFER_BIT);

        // Create triangle VBO: GREEN color (0.0, 1.0, 0.0)
        struct Vertex {
            float x, y;
            float r, g, b;
        };
        const Vertex triangle[3] = {
            {-0.8f, -0.8f,  0.0f, 1.0f, 0.0f},
            { 0.8f, -0.8f,  0.0f, 1.0f, 0.0f},
            { 0.0f,  0.8f,  0.0f, 1.0f, 0.0f}
        };

        WebGLBuffer vbo = ctx.createBuffer();
        ctx.bindBuffer(GL_ARRAY_BUFFER, vbo);
        ctx.bufferData(GL_ARRAY_BUFFER, sizeof(triangle), triangle, GL_STATIC_DRAW);

        WebGLVertexArrayObject vao = ctx.createVertexArray();
        ctx.bindVertexArray(vao);

        ctx.enableVertexAttribArray(0);
        ctx.vertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), 0);

        ctx.enableVertexAttribArray(1);
        ctx.vertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), offsetof(Vertex, r));

        // Draw triangle
        ctx.drawArrays(GL_TRIANGLES, 0, 3);

        // Read back canvas pixels
        std::vector<uint8_t> pixels;
        bool readOk = ctx.readCanvasPixels(pixels);
        assert(readOk);
        assert(pixels.size() == static_cast<size_t>(w * h * 4));

        // Top-left pixel (0, 0) should be background RED
        size_t bgIdx = 0;
        uint8_t rBg = pixels[bgIdx + 0];
        uint8_t gBg = pixels[bgIdx + 1];
        uint8_t bBg = pixels[bgIdx + 2];
        uint8_t aBg = pixels[bgIdx + 3];
        assert(rBg == 255 && gBg == 0 && bBg == 0 && aBg == 255);

        // Center pixel (32, 32) should be triangle GREEN
        size_t centerIdx = (32 * w + 32) * 4;
        uint8_t rC = pixels[centerIdx + 0];
        uint8_t gC = pixels[centerIdx + 1];
        uint8_t bC = pixels[centerIdx + 2];
        uint8_t aC = pixels[centerIdx + 3];
        assert(rC == 0 && gC == 255 && bC == 0 && aC == 255);

        // Clean up
        ctx.deleteBuffer(vbo);
        ctx.deleteVertexArray(vao);
        ctx.deleteShader(vs);
        ctx.deleteShader(fs);
        ctx.deleteProgram(prog);

        std::cout << "PASSED (Triangle rasterized & verified)" << std::endl;
    }

    // -------------------------------------------------------------------------
    // Test 5: Zero-Copy UI Compositing with VulkanPresenter
    // -------------------------------------------------------------------------
    std::cout << "[Test 5] Zero-Copy UI Compositing with VulkanPresenter... " << std::flush;
    {
        const int w = 64;
        const int h = 64;
        WebGLVkContext ctx(w, h, context);

        // Clear canvas to solid BLUE
        ctx.clearColor(0.0f, 0.0f, 1.0f, 1.0f);
        ctx.clear(GL_COLOR_BUFFER_BIT);
        ctx.flush();

        render::VulkanPresenter presenter(context);
        assert(presenter.init());

        // Zero-copy: Pass canvas VkImage directly to presenter without CPU host transfers!
        assert(presenter.presentImage(ctx.canvas().colorImage(), w, h, ctx.canvas().colorLayout()));

        std::vector<uint8_t> presenterPixels;
        uint32_t pw = 0, ph = 0;
        assert(presenter.readbackPixels(presenterPixels, pw, ph));
        assert(pw == w && ph == h);
        assert(presenterPixels.size() == static_cast<size_t>(w * h * 4));

        // Verify presenter received BLUE pixels
        assert(presenterPixels[0] == 0);
        assert(presenterPixels[1] == 0);
        assert(presenterPixels[2] == 255);
        assert(presenterPixels[3] == 255);

        std::cout << "PASSED (Direct GPU-to-GPU presentImage verified)" << std::endl;
    }

    // -------------------------------------------------------------------------
    // Test 6: WebGL2RenderingContext owning its backend, across a resize
    // -------------------------------------------------------------------------
    std::cout << "[Test 6] WebGL2RenderingContext backend + resize... " << std::flush;
    {
        WebGL2RenderingContext glCtx(64, 64, context);
        assert(glCtx.backend() != nullptr);

        glCtx.backend()->clearColor(0.0f, 1.0f, 1.0f, 1.0f); // Cyan
        glCtx.backend()->clear(GL_COLOR_BUFFER_BIT);

        std::vector<uint8_t> cyanPixels;
        assert(glCtx.readCanvasPixels(cyanPixels));
        assert(cyanPixels.size() == 64 * 64 * 4);
        assert(cyanPixels[0] == 0);
        assert(cyanPixels[1] == 255);
        assert(cyanPixels[2] == 255);
        assert(cyanPixels[3] == 255);

        glCtx.resize(32, 32);
        assert(glCtx.canvasWidth() == 32);
        assert(glCtx.canvasHeight() == 32);

        glCtx.backend()->clearColor(1.0f, 1.0f, 0.0f, 1.0f); // Yellow
        glCtx.backend()->clear(GL_COLOR_BUFFER_BIT);

        std::vector<uint8_t> yellowPixels;
        assert(glCtx.readCanvasPixels(yellowPixels));
        assert(yellowPixels.size() == 32 * 32 * 4);
        assert(yellowPixels[0] == 255);
        assert(yellowPixels[1] == 255);
        assert(yellowPixels[2] == 0);
        assert(yellowPixels[3] == 255);

        std::cout << "PASSED" << std::endl;
    }

    if (const uint32_t errors = render::vulkanValidationErrorCount()) {
        std::cerr << "FAILED: " << errors << " Vulkan validation error(s)" << std::endl;
        return 1;
    }
    std::cout << "=== bro_vulkan_webgl_test: all checks passed ===" << std::endl;
    return 0;
}
