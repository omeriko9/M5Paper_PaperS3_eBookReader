#pragma once
#include <M5Unified.h>
#include <string>
#include <vector>
#include <memory>

/**
 * @brief MathML Renderer for E-Ink Display
 * 
 * Parses MathML markup and renders mathematical formulas with proper
 * alignment, subscripts, superscripts, fractions, and matrices.
 * 
 * Supports inline and display math with baseline alignment for
 * seamless integration with regular text.
 */

// Forward declarations
struct MathNode;

// Types of MathML elements
enum class MathNodeType {
    ROOT,       // Root container
    ROW,        // mrow - horizontal group
    MI,         // mi - identifier (variable)
    MN,         // mn - number
    MO,         // mo - operator
    MTEXT,      // mtext - text
    MSUP,       // msup - superscript
    MSUB,       // msub - subscript
    MSUBSUP,    // msubsup - subscript and superscript
    MFRAC,      // mfrac - fraction
    MSQRT,      // msqrt - square root
    MROOT,      // mroot - nth root
    MFENCED,    // mfenced - parentheses/brackets
    MTABLE,     // mtable - table/matrix
    MTR,        // mtr - table row
    MTD,        // mtd - table cell
    MUNDER,     // munder - underscript
    MOVER,      // mover - overscript
    MUNDEROVER, // munderover - under and over
    MSPACE,     // mspace - space
    TEXT,       // Plain text content
    UNKNOWN     // Unknown element
};

// Bounding box for layout calculations
struct MathBox {
    int width = 0;
    int height = 0;
    int baseline = 0;  // Distance from top to baseline
    int ascent = 0;    // Height above baseline
    int descent = 0;   // Height below baseline
};

// A node in the MathML tree
struct MathNode {
    MathNodeType type = MathNodeType::UNKNOWN;
    std::string content;        // Text content for leaf nodes
    std::string tagName;        // Original tag name
    std::vector<std::unique_ptr<MathNode>> children;
    
    // Attributes from MathML
    std::string mathvariant;    // normal, italic, bold, etc.
    std::string open;           // Opening fence for mfenced
    std::string close;          // Closing fence for mfenced
    bool stretchy = false;      // For operators
    bool fence = false;         // For fence characters
    
    // Layout information (computed during layout pass)
    MathBox box;
    int x = 0;  // Position relative to parent
    int y = 0;
    
    MathNode() = default;
    MathNode(MathNodeType t) : type(t) {}
};

/**
 * @brief Result of rendering math content
 */
struct MathRenderResult {
    int width;          // Total width of rendered math
    int height;         // Total height
    int baseline;       // Baseline offset from top
    bool success;       // Whether rendering succeeded
};

/**
 * @brief MathML Renderer class
 */
class MathRenderer {
public:
    MathRenderer();
    ~MathRenderer();
    
    /**
     * @brief Initialize the renderer with display parameters
     * @param baseFontSize Base font size in pixels
     * @param screenWidth Display width for scaling calculations
     */
    void init(int baseFontSize, int screenWidth);
    
    /**
     * @brief Parse MathML markup into a render tree
     * @param mathml The MathML string (contents inside <math> tags)
     * @return Root node of the parsed tree, or nullptr on failure
     */
    std::unique_ptr<MathNode> parse(const std::string& mathml);
    
    /**
     * @brief Calculate layout for the math tree
     * @param root Root of the math tree
     * @param gfx Graphics context for font metrics
     * @param fontSize Font size to use
     */
    void calculateLayout(MathNode* root, LGFX_Device* gfx, float fontSize);
    
    /**
     * @brief Render the math tree to a canvas
     * @param root Root of the math tree
     * @param canvas Target canvas
     * @param x X position
     * @param y Y position (baseline)
     * @param fontSize Font size
     * @param color Text color
     * @return Render result with dimensions
     */
    MathRenderResult render(MathNode* root, M5Canvas* canvas, 
                            int x, int y, float fontSize, uint32_t color = TFT_BLACK);
    
    /**
     * @brief Get the width of rendered math content
     * @param mathml MathML string
     * @param gfx Graphics context
     * @param fontSize Font size
     * @return Width in pixels
     */
    int measureWidth(const std::string& mathml, LGFX_Device* gfx, float fontSize);
    
    /**
     * @brief Get the height of rendered math content  
     * @param mathml MathML string
     * @param gfx Graphics context
     * @param fontSize Font size
     * @return Height in pixels
     */
    int measureHeight(const std::string& mathml, LGFX_Device* gfx, float fontSize);
    
    /**
     * @brief Set the math font data buffer
     * @param fontData Pointer to VLW font data
     */
    void setMathFont(const uint8_t* fontData) { mathFontData = fontData; }
    
private:
    // Parsing helpers
    std::unique_ptr<MathNode> parseElement(const std::string& xml, size_t& pos);
    std::string parseTagName(const std::string& xml, size_t& pos);
    void parseAttributes(const std::string& tag, MathNode* node);
    std::string extractAttribute(const std::string& tag, const std::string& attr);
    void skipWhitespace(const std::string& xml, size_t& pos);
    std::string decodeEntity(const std::string& entity);
    
    // Layout helpers
    void layoutNode(MathNode* node, LGFX_Device* gfx, float fontSize, float scale = 1.0f);
    void layoutRow(MathNode* node, LGFX_Device* gfx, float fontSize, float scale);
    void layoutSuperscript(MathNode* node, LGFX_Device* gfx, float fontSize, float scale);
    void layoutSubscript(MathNode* node, LGFX_Device* gfx, float fontSize, float scale);
    void layoutSubSup(MathNode* node, LGFX_Device* gfx, float fontSize, float scale);
    void layoutFraction(MathNode* node, LGFX_Device* gfx, float fontSize, float scale);
    void layoutSqrt(MathNode* node, LGFX_Device* gfx, float fontSize, float scale);
    void layoutFenced(MathNode* node, LGFX_Device* gfx, float fontSize, float scale);
    void layoutTable(MathNode* node, LGFX_Device* gfx, float fontSize, float scale);
    void layoutUnderOver(MathNode* node, LGFX_Device* gfx, float fontSize, float scale, bool under, bool over);
    MathBox measureText(const std::string& text, LGFX_Device* gfx, float fontSize, float scale);
    
    // Rendering helpers
    void renderNode(MathNode* node, M5Canvas* canvas, int x, int y, 
                   float fontSize, float scale, uint32_t color);
    void renderText(const std::string& text, M5Canvas* canvas, int x, int y,
                   float fontSize, float scale, uint32_t color, const std::string& variant);
    void renderFractionLine(M5Canvas* canvas, int x, int y, int width, uint32_t color);
    void renderSqrtSymbol(M5Canvas* canvas, int x, int y, int width, int height, uint32_t color);
    void renderBracket(M5Canvas* canvas, int x, int y, int height, 
                      const std::string& bracket, float fontSize, uint32_t color);
    
    // Unicode/entity mappings
    std::string mapMathOperator(const std::string& op);
    
    // Configuration
    int baseFontSize = 24;
    int screenWidth = 540;
    float subscriptScale = 0.7f;
    float superscriptScale = 0.7f;
    float fractionScale = 0.85f;
    // minFontSize is a SCALE FACTOR for setTextSize(), not pixels.
    // With a 20px VLW font, 0.5 means minimum 10px effective size.
    float minFontSize = 0.5f;
    
    // Font data
    const uint8_t* mathFontData = nullptr;
};

// Global math renderer instance
extern MathRenderer mathRenderer;
