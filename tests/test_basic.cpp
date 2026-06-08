#include "lexer.h"
#include "raylib_api_extract.h"

#include <cstdlib>
#include <iostream>
#include <unordered_map>
#include <unordered_set>

static void CHECK(bool cond, const char* msg) {
    if (!cond) {
        std::cerr << "TEST FAILED: " << msg << "\n";
        std::exit(1);
    }
}

int main() {
    using namespace rlren;

    // ---------------------------
    // Lexer rewrite should only replace identifier tokens
    // ---------------------------
    {
        std::unordered_map<std::string, std::string> m = {
            {"CloseWindow", "RL_CloseWindow"},
            {"Rectangle", "RL_Rectangle"},
            {"ShowCursor", "RL_ShowCursor"}
        };

        const std::string in = R"TEST(// CloseWindow in comment should NOT change
const char* s = "CloseWindow in string should NOT change";
const char c = '\'';
auto x = CloseWindow;
Rectangle r;
#define MACRO_CALL() CloseWindow()
auto raw = R"tag(CloseWindow Rectangle)tag";
/* block comment ShowCursor should NOT change */
ShowCursor();
)TEST";

        const std::string out = RewriteWithIdentifierMap(in, m);

        CHECK(out.find("// CloseWindow") != std::string::npos, "comment should remain");
        CHECK(out.find("\"CloseWindow in string") != std::string::npos, "string should remain");
        CHECK(out.find("R\"tag(CloseWindow Rectangle)tag\"") != std::string::npos, "raw string should remain");

        CHECK(out.find("auto x = RL_CloseWindow") != std::string::npos, "identifier replaced");
        CHECK(out.find("RL_Rectangle r") != std::string::npos, "type replaced");
        CHECK(out.find("#define MACRO_CALL() RL_CloseWindow()") != std::string::npos, "macro body replaced");
        CHECK(out.find("RL_ShowCursor();") != std::string::npos, "function call replaced");
        CHECK(out.find("block comment ShowCursor") != std::string::npos, "block comment preserved");
    }

    // ---------------------------
    // Extractor should find functions/types/enums/defines without struct fields
    // ---------------------------
    {
        const std::string hdr =
            "#define FLAG_A 1\n"
            "#define _INTERNAL 2\n"
            "typedef struct Vector2 { float x; float y; } Vector2;\n"
            "typedef enum { KEY_A = 65, KEY_B } KeyboardKey;\n"
            "typedef void (*TraceLogCallback)(int level, const char* text);\n"
            "RLAPI void CloseWindow(void);\n";

        ApiExtractOptions opt;
        opt.apiDefines = {"RLAPI"};
        opt.includeDefineMacros = true;
        opt.includeTypedefAliases = true;
        opt.includeEnums = true;
        opt.includeEnumValues = true;
        opt.skipLeadingUnderscore = true;
        opt.skipAlreadyPrefixedRL = true;

        auto syms = ExtractRaylibApiSymbolsFromHeader(hdr, opt);

        CHECK(syms.count("FLAG_A") == 1, "define macro extracted");
        CHECK(syms.count("_INTERNAL") == 0, "leading underscore skipped");
        CHECK(syms.count("Vector2") == 1, "typedef struct name extracted");
        CHECK(syms.count("x") == 0 && syms.count("y") == 0, "struct fields not extracted");
        CHECK(syms.count("KeyboardKey") == 1, "enum typedef name extracted");
        CHECK(syms.count("KEY_A") == 1 && syms.count("KEY_B") == 1, "enum values extracted");
        CHECK(syms.count("TraceLogCallback") == 1, "callback typedef extracted");
        CHECK(syms.count("CloseWindow") == 1, "RLAPI function extracted");
    }

    // ---------------------------
    // Macro-aware rewrite: macros only renamed at PP positions
    // ---------------------------
    {
        std::unordered_map<std::string, std::string> m = {
            {"SUPPORT_TRACELOG", "RL_SUPPORT_TRACELOG"},
            {"CloseWindow", "RL_CloseWindow"},
            {"Rectangle", "RL_Rectangle"},
        };
        std::unordered_set<std::string> macros = {"SUPPORT_TRACELOG"};

        // Test 1: #define with value
        {
            const std::string in = "#define SUPPORT_TRACELOG 1\n";
            const std::string out = RewriteWithMacroAwareness(in, m, nullptr, macros);
            CHECK(out.find("#define RL_SUPPORT_TRACELOG 1") != std::string::npos, "define macro renamed");
            CHECK(out.find("#define SUPPORT_TRACELOG") == std::string::npos, "old define name gone");
        }

        // Test 2: #ifdef
        {
            const std::string in = "#ifdef SUPPORT_TRACELOG\n";
            const std::string out = RewriteWithMacroAwareness(in, m, nullptr, macros);
            CHECK(out.find("#ifdef RL_SUPPORT_TRACELOG") != std::string::npos, "ifdef macro renamed");
        }

        // Test 3: #undef
        {
            const std::string in = "#undef SUPPORT_TRACELOG\n";
            const std::string out = RewriteWithMacroAwareness(in, m, nullptr, macros);
            CHECK(out.find("#undef RL_SUPPORT_TRACELOG") != std::string::npos, "undef macro renamed");
        }

        // Test 4: #if defined()
        {
            const std::string in = "#if defined(SUPPORT_TRACELOG)\n";
            const std::string out = RewriteWithMacroAwareness(in, m, nullptr, macros);
            CHECK(out.find("defined(RL_SUPPORT_TRACELOG)") != std::string::npos, "defined() macro renamed");
        }

        // Test 5: macro name in regular code NOT renamed
        {
            const std::string in = "int SUPPORT_TRACELOG = 0;\n";
            const std::string out = RewriteWithMacroAwareness(in, m, nullptr, macros);
            CHECK(out.find("int SUPPORT_TRACELOG = 0") != std::string::npos, "macro in code unchanged");
            CHECK(out.find("int RL_SUPPORT_TRACELOG") == std::string::npos, "macro in code NOT renamed");
        }

        // Test 6: non-macro identifiers still renamed everywhere
        {
            const std::string in = "CloseWindow();\nRectangle r;\n";
            const std::string out = RewriteWithMacroAwareness(in, m, nullptr, macros);
            CHECK(out.find("RL_CloseWindow()") != std::string::npos, "function renamed");
            CHECK(out.find("RL_Rectangle r") != std::string::npos, "type renamed");
        }

        // Test 7: comment followed by #define
        {
            const std::string in = "// some comment\n#define SUPPORT_TRACELOG 1\n";
            const std::string out = RewriteWithMacroAwareness(in, m, nullptr, macros);
            CHECK(out.find("#define RL_SUPPORT_TRACELOG 1") != std::string::npos, "define after comment renamed");
        }

        // Test 8: consecutive PP directives
        {
            const std::string in = "#define SUPPORT_TRACELOG 1\n#ifdef SUPPORT_TRACELOG\n#undef SUPPORT_TRACELOG\n";
            const std::string out = RewriteWithMacroAwareness(in, m, nullptr, macros);
            CHECK(out.find("#define RL_SUPPORT_TRACELOG 1") != std::string::npos, "consecutive define");
            CHECK(out.find("#ifdef RL_SUPPORT_TRACELOG") != std::string::npos, "consecutive ifdef");
            CHECK(out.find("#undef RL_SUPPORT_TRACELOG") != std::string::npos, "consecutive undef");
        }

        // Test 9: #define without value
        {
            const std::string in = "#define SUPPORT_TRACELOG\n";
            const std::string out = RewriteWithMacroAwareness(in, m, nullptr, macros);
            CHECK(out.find("#define RL_SUPPORT_TRACELOG") != std::string::npos, "define no-value renamed");
        }
    }

    std::cout << "All tests passed.\n";
    return 0;
}
