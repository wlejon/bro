#include "webgl/glsl_translator.h"

#include <cstring>
#include <sstream>
#include <regex>

namespace bro::webgl {

std::string translateGLSL(const std::string& source, GLenum /*shaderType*/) {
    std::istringstream input(source);
    std::ostringstream output;
    std::string line;
    bool versionWritten = false;

    while (std::getline(input, line)) {
        // Replace #version 300 es with #version 330 core
        if (!versionWritten && line.find("#version") != std::string::npos) {
            // Match: #version 300 es (with optional whitespace)
            if (line.find("300") != std::string::npos && line.find("es") != std::string::npos) {
                output << "#version 330 core\n";
                versionWritten = true;
                continue;
            }
        }

        // Strip standalone precision qualifier statements:
        //   precision highp float;
        //   precision mediump int;
        //   precision lowp sampler2D;
        {
            // Trim leading whitespace for matching
            auto trimmed = line;
            auto pos = trimmed.find_first_not_of(" \t");
            if (pos != std::string::npos) trimmed = trimmed.substr(pos);

            if (trimmed.rfind("precision ", 0) == 0 && trimmed.find(';') != std::string::npos) {
                // This is a precision statement — skip it
                continue;
            }
        }

        // Strip inline precision qualifiers from declarations:
        //   highp vec3 foo → vec3 foo
        //   mediump float bar → float bar
        //   lowp int baz → int baz
        // Also handle in function parameters: (highp vec3 pos) → (vec3 pos)
        {
            // Simple replacement — these are reserved words in GLSL ES but
            // not meaningful in desktop GL 3.30
            std::string result;
            result.reserve(line.size());
            size_t i = 0;
            while (i < line.size()) {
                bool replaced = false;
                for (const char* qual : {"highp ", "mediump ", "lowp "}) {
                    size_t len = strlen(qual);
                    if (line.compare(i, len, qual) == 0) {
                        // Check it's at a word boundary (start of line, after space, paren, comma)
                        if (i == 0 || line[i-1] == ' ' || line[i-1] == '\t' ||
                            line[i-1] == '(' || line[i-1] == ',') {
                            i += len;
                            replaced = true;
                            break;
                        }
                    }
                }
                if (!replaced) {
                    result += line[i++];
                }
            }
            line = result;
        }

        output << line << '\n';
    }

    return output.str();
}

} // namespace bro::webgl
