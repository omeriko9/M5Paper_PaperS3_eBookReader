#include "math_renderer.h"
#include "esp_log.h"
#include <algorithm>
#include <cctype>
#include <map>

static const char* TAG = "MATH";

// Global instance
MathRenderer mathRenderer;

// Unicode math symbols mapping
static const std::map<std::string, std::string> entityMap = {
    // Greek letters
    {"alpha", "α"}, {"beta", "β"}, {"gamma", "γ"}, {"delta", "δ"},
    {"epsilon", "ε"}, {"zeta", "ζ"}, {"eta", "η"}, {"theta", "θ"},
    {"iota", "ι"}, {"kappa", "κ"}, {"lambda", "λ"}, {"mu", "μ"},
    {"nu", "ν"}, {"xi", "ξ"}, {"omicron", "ο"}, {"pi", "π"},
    {"rho", "ρ"}, {"sigma", "σ"}, {"tau", "τ"}, {"upsilon", "υ"},
    {"phi", "φ"}, {"chi", "χ"}, {"psi", "ψ"}, {"omega", "ω"},
    {"Alpha", "Α"}, {"Beta", "Β"}, {"Gamma", "Γ"}, {"Delta", "Δ"},
    {"Epsilon", "Ε"}, {"Zeta", "Ζ"}, {"Eta", "Η"}, {"Theta", "Θ"},
    {"Iota", "Ι"}, {"Kappa", "Κ"}, {"Lambda", "Λ"}, {"Mu", "Μ"},
    {"Nu", "Ν"}, {"Xi", "Ξ"}, {"Omicron", "Ο"}, {"Pi", "Π"},
    {"Rho", "Ρ"}, {"Sigma", "Σ"}, {"Tau", "Τ"}, {"Upsilon", "Υ"},
    {"Phi", "Φ"}, {"Chi", "Χ"}, {"Psi", "Ψ"}, {"Omega", "Ω"},
    
    // Operators and relations
    {"plus", "+"}, {"minus", "−"}, {"times", "×"}, {"div", "÷"},
    {"equals", "="}, {"ne", "≠"}, {"lt", "<"}, {"gt", ">"},
    {"le", "≤"}, {"ge", "≥"}, {"approx", "≈"}, {"equiv", "≡"},
    {"sim", "∼"}, {"simeq", "≃"}, {"cong", "≅"}, {"propto", "∝"},
    {"pm", "±"}, {"mp", "∓"}, {"cdot", "·"}, {"ast", "∗"},
    {"star", "⋆"}, {"circ", "∘"}, {"bullet", "•"},
    
    // Set theory
    {"in", "∈"}, {"notin", "∉"}, {"ni", "∋"}, {"subset", "⊂"},
    {"supset", "⊃"}, {"subseteq", "⊆"}, {"supseteq", "⊇"},
    {"cap", "∩"}, {"cup", "∪"}, {"emptyset", "∅"},
    {"setminus", "∖"}, {"complement", "∁"},
    
    // Logic
    {"forall", "∀"}, {"exists", "∃"}, {"nexists", "∄"},
    {"land", "∧"}, {"lor", "∨"}, {"lnot", "¬"}, {"neg", "¬"},
    {"implies", "⇒"}, {"iff", "⇔"}, {"to", "→"}, {"gets", "←"},
    {"leftrightarrow", "↔"}, {"Rightarrow", "⇒"}, {"Leftarrow", "⇐"},
    
    // Calculus
    {"int", "∫"}, {"iint", "∬"}, {"iiint", "∭"}, {"oint", "∮"},
    {"sum", "∑"}, {"prod", "∏"}, {"coprod", "∐"},
    {"partial", "∂"}, {"nabla", "∇"}, {"infty", "∞"},
    {"prime", "′"}, {"dprime", "″"},
    
    // Arrows
    {"leftarrow", "←"}, {"rightarrow", "→"}, {"uparrow", "↑"},
    {"downarrow", "↓"}, {"leftrightarrow", "↔"},
    {"Leftarrow", "⇐"}, {"Rightarrow", "⇒"}, {"Uparrow", "⇑"},
    {"Downarrow", "⇓"}, {"Leftrightarrow", "⇔"},
    {"mapsto", "↦"}, {"longmapsto", "⟼"},
    
    // Misc
    {"therefore", "∴"}, {"because", "∵"}, {"ldots", "…"},
    {"cdots", "⋯"}, {"vdots", "⋮"}, {"ddots", "⋱"},
    {"angle", "∠"}, {"triangle", "△"}, {"square", "□"},
    {"lfloor", "⌊"}, {"rfloor", "⌋"}, {"lceil", "⌈"}, {"rceil", "⌉"},
    {"langle", "⟨"}, {"rangle", "⟩"},
    
    // Double-struck (blackboard bold)
    {"Ropf", "ℝ"}, {"Copf", "ℂ"}, {"Qopf", "ℚ"}, {"Zopf", "ℤ"}, {"Nopf", "ℕ"},
    {"reals", "ℝ"}, {"complexes", "ℂ"}, {"rationals", "ℚ"}, 
    {"integers", "ℤ"}, {"naturals", "ℕ"},
    
    // HTML entities
    {"nbsp", " "}, {"amp", "&"}, {"lt", "<"}, {"gt", ">"},
    {"quot", "\""}, {"apos", "'"},
};

MathRenderer::MathRenderer() {}
MathRenderer::~MathRenderer() {}

void MathRenderer::init(int fontSize, int width) {
    baseFontSize = fontSize;
    screenWidth = width;
}

std::string MathRenderer::decodeEntity(const std::string& entity) {
    // Numeric entity
    if (!entity.empty() && entity[0] == '#') {
        int code = 0;
        if (entity.size() > 1 && (entity[1] == 'x' || entity[1] == 'X')) {
            code = strtol(entity.c_str() + 2, nullptr, 16);
        } else {
            code = atoi(entity.c_str() + 1);
        }
        if (code > 0) {
            // Convert to UTF-8
            std::string result;
            if (code < 0x80) {
                result += (char)code;
            } else if (code < 0x800) {
                result += (char)(0xC0 | (code >> 6));
                result += (char)(0x80 | (code & 0x3F));
            } else if (code < 0x10000) {
                result += (char)(0xE0 | (code >> 12));
                result += (char)(0x80 | ((code >> 6) & 0x3F));
                result += (char)(0x80 | (code & 0x3F));
            } else {
                result += (char)(0xF0 | (code >> 18));
                result += (char)(0x80 | ((code >> 12) & 0x3F));
                result += (char)(0x80 | ((code >> 6) & 0x3F));
                result += (char)(0x80 | (code & 0x3F));
            }
            return result;
        }
        return "?";
    }
    
    // Named entity
    auto it = entityMap.find(entity);
    if (it != entityMap.end()) {
        return it->second;
    }
    
    return "&" + entity + ";";
}

void MathRenderer::skipWhitespace(const std::string& xml, size_t& pos) {
    while (pos < xml.size() && std::isspace(xml[pos])) {
        pos++;
    }
}

std::string MathRenderer::extractAttribute(const std::string& tag, const std::string& attr) {
    std::string search = attr + "=\"";
    size_t pos = tag.find(search);
    if (pos == std::string::npos) {
        search = attr + "='";
        pos = tag.find(search);
    }
    if (pos == std::string::npos) return "";
    
    pos += search.length();
    char quote = (search.back() == '"') ? '"' : '\'';
    size_t end = tag.find(quote, pos);
    if (end == std::string::npos) return "";
    
    return tag.substr(pos, end - pos);
}

void MathRenderer::parseAttributes(const std::string& tag, MathNode* node) {
    node->mathvariant = extractAttribute(tag, "mathvariant");
    node->open = extractAttribute(tag, "open");
    node->close = extractAttribute(tag, "close");
    
    std::string stretchy = extractAttribute(tag, "stretchy");
    node->stretchy = (stretchy == "true");
    
    std::string fence = extractAttribute(tag, "fence");
    node->fence = (fence == "true");
}

std::string MathRenderer::parseTagName(const std::string& xml, size_t& pos) {
    skipWhitespace(xml, pos);
    std::string name;
    while (pos < xml.size() && !std::isspace(xml[pos]) && 
           xml[pos] != '>' && xml[pos] != '/' && xml[pos] != ':') {
        name += xml[pos++];
    }
    // Skip namespace prefix if present
    if (pos < xml.size() && xml[pos] == ':') {
        pos++;
        name.clear();
        while (pos < xml.size() && !std::isspace(xml[pos]) && 
               xml[pos] != '>' && xml[pos] != '/') {
            name += xml[pos++];
        }
    }
    return name;
}

std::unique_ptr<MathNode> MathRenderer::parseElement(const std::string& xml, size_t& pos) {
    skipWhitespace(xml, pos);
    
    if (pos >= xml.size()) return nullptr;
    
    auto node = std::make_unique<MathNode>();
    
    // Check for opening tag
    if (xml[pos] == '<') {
        pos++;  // Skip '<'
        
        // Check for closing tag
        if (pos < xml.size() && xml[pos] == '/') {
            return nullptr;  // End tag, not a new element
        }
        
        // Parse tag name
        std::string tagName = parseTagName(xml, pos);
        node->tagName = tagName;
        
        // Map tag name to type
        if (tagName == "math") node->type = MathNodeType::ROOT;
        else if (tagName == "mrow") node->type = MathNodeType::ROW;
        else if (tagName == "mi") node->type = MathNodeType::MI;
        else if (tagName == "mn") node->type = MathNodeType::MN;
        else if (tagName == "mo") node->type = MathNodeType::MO;
        else if (tagName == "mtext") node->type = MathNodeType::MTEXT;
        else if (tagName == "msup") node->type = MathNodeType::MSUP;
        else if (tagName == "msub") node->type = MathNodeType::MSUB;
        else if (tagName == "msubsup") node->type = MathNodeType::MSUBSUP;
        else if (tagName == "mfrac") node->type = MathNodeType::MFRAC;
        else if (tagName == "msqrt") node->type = MathNodeType::MSQRT;
        else if (tagName == "mroot") node->type = MathNodeType::MROOT;
        else if (tagName == "mfenced") node->type = MathNodeType::MFENCED;
        else if (tagName == "mtable") node->type = MathNodeType::MTABLE;
        else if (tagName == "mtr") node->type = MathNodeType::MTR;
        else if (tagName == "mtd") node->type = MathNodeType::MTD;
        else if (tagName == "munder") node->type = MathNodeType::MUNDER;
        else if (tagName == "mover") node->type = MathNodeType::MOVER;
        else if (tagName == "munderover") node->type = MathNodeType::MUNDEROVER;
        else if (tagName == "mspace") node->type = MathNodeType::MSPACE;
        else if (tagName == "mstyle") node->type = MathNodeType::ROW;  // Treat as row
        else node->type = MathNodeType::ROW;  // Default container
        
        // Find end of opening tag
        size_t tagEnd = xml.find('>', pos);
        if (tagEnd == std::string::npos) return nullptr;
        
        // Extract and parse attributes
        std::string tagContent = xml.substr(pos, tagEnd - pos);
        parseAttributes(tagContent, node.get());
        
        // Check for self-closing tag
        bool selfClosing = (tagEnd > 0 && xml[tagEnd - 1] == '/');
        pos = tagEnd + 1;
        
        if (selfClosing) {
            return node;
        }
        
        // Parse children and text content
        while (pos < xml.size()) {
            skipWhitespace(xml, pos);
            
            if (pos >= xml.size()) break;
            
            if (xml[pos] == '<') {
                // Check for end tag
                if (pos + 1 < xml.size() && xml[pos + 1] == '/') {
                    // Skip the end tag
                    size_t endTagClose = xml.find('>', pos);
                    if (endTagClose != std::string::npos) {
                        pos = endTagClose + 1;
                    }
                    break;
                }
                
                // Parse child element
                auto child = parseElement(xml, pos);
                if (child) {
                    node->children.push_back(std::move(child));
                }
            } else {
                // Text content
                std::string text;
                while (pos < xml.size() && xml[pos] != '<') {
                    if (xml[pos] == '&') {
                        // Entity
                        size_t entityEnd = xml.find(';', pos);
                        if (entityEnd != std::string::npos) {
                            std::string entity = xml.substr(pos + 1, entityEnd - pos - 1);
                            text += decodeEntity(entity);
                            pos = entityEnd + 1;
                        } else {
                            text += xml[pos++];
                        }
                    } else if (!std::isspace(xml[pos]) || !text.empty()) {
                        text += xml[pos++];
                    } else {
                        pos++;
                    }
                }
                
                // Trim whitespace
                while (!text.empty() && std::isspace(text.back())) {
                    text.pop_back();
                }
                
                if (!text.empty()) {
                    auto textNode = std::make_unique<MathNode>();
                    textNode->type = MathNodeType::TEXT;
                    textNode->content = text;
                    node->children.push_back(std::move(textNode));
                }
            }
        }
    }
    
    return node;
}

std::unique_ptr<MathNode> MathRenderer::parse(const std::string& mathml) {
    if (mathml.empty()) return nullptr;
    
    size_t pos = 0;
    
    // Skip to first tag if there's leading content
    while (pos < mathml.size() && mathml[pos] != '<') {
        pos++;
    }
    
    auto root = parseElement(mathml, pos);
    if (!root) {
        // Create a simple text node for plain content
        root = std::make_unique<MathNode>();
        root->type = MathNodeType::ROOT;
        auto textNode = std::make_unique<MathNode>();
        textNode->type = MathNodeType::TEXT;
        textNode->content = mathml;
        root->children.push_back(std::move(textNode));
    }
    
    return root;
}

MathBox MathRenderer::measureText(const std::string& text, LGFX_Device* gfx, 
                                   float fontSize, float scale) {
    MathBox box;
    if (text.empty() || !gfx) return box;
    
    float scaledSize = fontSize * scale;
    if (scaledSize < minFontSize) scaledSize = minFontSize;
    
    gfx->setTextSize(scaledSize);
    
    box.width = gfx->textWidth(text.c_str());
    int fontHeight = gfx->fontHeight();
    box.height = fontHeight;
    box.baseline = fontHeight * 0.75;  // Approximate baseline
    box.ascent = box.baseline;
    box.descent = box.height - box.baseline;
    
    return box;
}

void MathRenderer::layoutNode(MathNode* node, LGFX_Device* gfx, float fontSize, float scale) {
    if (!node || !gfx) return;
    
    switch (node->type) {
        case MathNodeType::TEXT:
        case MathNodeType::MI:
        case MathNodeType::MN:
        case MathNodeType::MO:
        case MathNodeType::MTEXT: {
            // Get text content
            std::string text = node->content;
            if (text.empty() && !node->children.empty()) {
                // Gather text from children
                for (auto& child : node->children) {
                    if (child->type == MathNodeType::TEXT) {
                        text += child->content;
                    }
                }
            }
            node->box = measureText(text, gfx, fontSize, scale);
            break;
        }
        
        case MathNodeType::MSUP:
            layoutSuperscript(node, gfx, fontSize, scale);
            break;
            
        case MathNodeType::MSUB:
            layoutSubscript(node, gfx, fontSize, scale);
            break;
            
        case MathNodeType::MSUBSUP:
            layoutSubSup(node, gfx, fontSize, scale);
            break;
            
        case MathNodeType::MFRAC:
            layoutFraction(node, gfx, fontSize, scale);
            break;
            
        case MathNodeType::MSQRT:
        case MathNodeType::MROOT:
            layoutSqrt(node, gfx, fontSize, scale);
            break;
            
        case MathNodeType::MFENCED:
            layoutFenced(node, gfx, fontSize, scale);
            break;
            
        case MathNodeType::MTABLE:
            layoutTable(node, gfx, fontSize, scale);
            break;
            
        case MathNodeType::MUNDER:
            layoutUnderOver(node, gfx, fontSize, scale, true, false);
            break;
            
        case MathNodeType::MOVER:
            layoutUnderOver(node, gfx, fontSize, scale, false, true);
            break;
            
        case MathNodeType::MUNDEROVER:
            layoutUnderOver(node, gfx, fontSize, scale, true, true);
            break;
            
        case MathNodeType::ROOT:
        case MathNodeType::ROW:
        case MathNodeType::MTR:
        case MathNodeType::MTD:
        default:
            layoutRow(node, gfx, fontSize, scale);
            break;
    }
}

void MathRenderer::layoutRow(MathNode* node, LGFX_Device* gfx, float fontSize, float scale) {
    if (!node || !gfx) return;
    
    int totalWidth = 0;
    int maxAscent = 0;
    int maxDescent = 0;
    int spacing = 2;  // Small spacing between elements
    
    // First pass: layout all children
    for (auto& child : node->children) {
        layoutNode(child.get(), gfx, fontSize, scale);
    }
    
    // Second pass: calculate positions
    for (size_t i = 0; i < node->children.size(); i++) {
        auto& child = node->children[i];
        child->x = totalWidth;
        totalWidth += child->box.width;
        
        if (i < node->children.size() - 1) {
            totalWidth += spacing;
        }
        
        maxAscent = std::max(maxAscent, child->box.ascent);
        maxDescent = std::max(maxDescent, child->box.descent);
    }
    
    // Set y positions to align baselines
    for (auto& child : node->children) {
        child->y = maxAscent - child->box.ascent;
    }
    
    node->box.width = totalWidth;
    node->box.height = maxAscent + maxDescent;
    node->box.baseline = maxAscent;
    node->box.ascent = maxAscent;
    node->box.descent = maxDescent;
}

void MathRenderer::layoutSuperscript(MathNode* node, LGFX_Device* gfx, float fontSize, float scale) {
    if (!node || node->children.size() < 2) return;
    
    auto& base = node->children[0];
    auto& sup = node->children[1];
    
    // Layout base at normal scale
    layoutNode(base.get(), gfx, fontSize, scale);
    
    // Layout superscript at smaller scale
    float supScale = scale * superscriptScale;
    layoutNode(sup.get(), gfx, fontSize, supScale);
    
    // Position base
    base->x = 0;
    base->y = sup->box.height * 0.3;  // Lower base slightly
    
    // Position superscript above and to the right
    sup->x = base->box.width + 1;
    sup->y = 0;
    
    // Calculate bounding box
    node->box.width = sup->x + sup->box.width;
    node->box.height = base->y + base->box.height;
    node->box.baseline = base->y + base->box.baseline;
    node->box.ascent = node->box.baseline;
    node->box.descent = node->box.height - node->box.baseline;
}

void MathRenderer::layoutSubscript(MathNode* node, LGFX_Device* gfx, float fontSize, float scale) {
    if (!node || node->children.size() < 2) return;
    
    auto& base = node->children[0];
    auto& sub = node->children[1];
    
    // Layout base at normal scale
    layoutNode(base.get(), gfx, fontSize, scale);
    
    // Layout subscript at smaller scale
    float subScale = scale * subscriptScale;
    layoutNode(sub.get(), gfx, fontSize, subScale);
    
    // Position base
    base->x = 0;
    base->y = 0;
    
    // Position subscript below and to the right
    sub->x = base->box.width + 1;
    sub->y = base->box.height - sub->box.height * 0.5;
    
    // Calculate bounding box
    node->box.width = sub->x + sub->box.width;
    node->box.height = std::max(base->box.height, (int)(sub->y + sub->box.height));
    node->box.baseline = base->box.baseline;
    node->box.ascent = base->box.ascent;
    node->box.descent = node->box.height - node->box.baseline;
}

void MathRenderer::layoutSubSup(MathNode* node, LGFX_Device* gfx, float fontSize, float scale) {
    if (!node || node->children.size() < 3) return;
    
    auto& base = node->children[0];
    auto& sub = node->children[1];
    auto& sup = node->children[2];
    
    // Layout all parts
    layoutNode(base.get(), gfx, fontSize, scale);
    layoutNode(sub.get(), gfx, fontSize, scale * subscriptScale);
    layoutNode(sup.get(), gfx, fontSize, scale * superscriptScale);
    
    int scriptWidth = std::max(sub->box.width, sup->box.width);
    
    // Position base
    base->x = 0;
    base->y = sup->box.height * 0.3;
    
    // Position superscript
    sup->x = base->box.width + 1;
    sup->y = 0;
    
    // Position subscript
    sub->x = base->box.width + 1;
    sub->y = base->y + base->box.height - sub->box.height * 0.5;
    
    // Calculate bounding box
    node->box.width = base->box.width + scriptWidth + 2;
    node->box.height = std::max(base->y + base->box.height, (int)(sub->y + sub->box.height));
    node->box.baseline = base->y + base->box.baseline;
    node->box.ascent = node->box.baseline;
    node->box.descent = node->box.height - node->box.baseline;
}

void MathRenderer::layoutFraction(MathNode* node, LGFX_Device* gfx, float fontSize, float scale) {
    if (!node || node->children.size() < 2) return;
    
    auto& num = node->children[0];
    auto& denom = node->children[1];
    
    // Layout numerator and denominator at slightly smaller scale
    float fracScale = scale * fractionScale;
    layoutNode(num.get(), gfx, fontSize, fracScale);
    layoutNode(denom.get(), gfx, fontSize, fracScale);
    
    int maxWidth = std::max(num->box.width, denom->box.width);
    int lineThickness = std::max(1, (int)(fontSize * scale * 0.05));
    int padding = 2;
    
    // Center numerator
    num->x = (maxWidth - num->box.width) / 2;
    num->y = 0;
    
    // Center denominator below line
    denom->x = (maxWidth - denom->box.width) / 2;
    denom->y = num->box.height + lineThickness + padding * 2;
    
    // Calculate bounding box
    node->box.width = maxWidth + padding * 2;
    node->box.height = num->box.height + lineThickness + padding * 2 + denom->box.height;
    node->box.baseline = num->box.height + lineThickness / 2 + padding;
    node->box.ascent = node->box.baseline;
    node->box.descent = node->box.height - node->box.baseline;
}

void MathRenderer::layoutSqrt(MathNode* node, LGFX_Device* gfx, float fontSize, float scale) {
    if (!node || node->children.empty()) return;
    
    // Layout content
    for (auto& child : node->children) {
        layoutNode(child.get(), gfx, fontSize, scale);
    }
    
    int contentWidth = 0;
    int contentHeight = 0;
    int maxAscent = 0;
    int maxDescent = 0;
    
    for (auto& child : node->children) {
        contentWidth += child->box.width;
        contentHeight = std::max(contentHeight, child->box.height);
        maxAscent = std::max(maxAscent, child->box.ascent);
        maxDescent = std::max(maxDescent, child->box.descent);
    }
    
    int sqrtWidth = (int)(fontSize * scale * 0.5);  // Width of sqrt symbol
    int overlineHeight = std::max(1, (int)(fontSize * scale * 0.05));
    int padding = 2;
    
    // Position children
    int xOffset = sqrtWidth;
    for (auto& child : node->children) {
        child->x = xOffset;
        child->y = overlineHeight + padding;
        xOffset += child->box.width;
    }
    
    // Calculate bounding box
    node->box.width = sqrtWidth + contentWidth + padding;
    node->box.height = contentHeight + overlineHeight + padding * 2;
    node->box.baseline = overlineHeight + padding + maxAscent;
    node->box.ascent = node->box.baseline;
    node->box.descent = node->box.height - node->box.baseline;
}

void MathRenderer::layoutFenced(MathNode* node, LGFX_Device* gfx, float fontSize, float scale) {
    if (!node) return;
    
    // Layout children first
    layoutRow(node, gfx, fontSize, scale);
    
    // Add space for brackets
    std::string openBracket = node->open.empty() ? "(" : node->open;
    std::string closeBracket = node->close.empty() ? ")" : node->close;
    
    int bracketWidth = (int)(fontSize * scale * 0.3);
    int padding = 2;
    
    // Shift all children to make room for opening bracket
    for (auto& child : node->children) {
        child->x += bracketWidth + padding;
    }
    
    // Update bounding box
    node->box.width += (bracketWidth + padding) * 2;
}

void MathRenderer::layoutTable(MathNode* node, LGFX_Device* gfx, float fontSize, float scale) {
    if (!node) return;
    
    std::vector<int> colWidths;
    std::vector<int> rowHeights;
    std::vector<int> rowBaselines;
    int cellPadding = 4;
    
    // First pass: calculate row and column dimensions
    for (auto& row : node->children) {
        if (row->type != MathNodeType::MTR) continue;
        
        int rowHeight = 0;
        int rowBaseline = 0;
        size_t colIndex = 0;
        
        for (auto& cell : row->children) {
            if (cell->type != MathNodeType::MTD) continue;
            
            // Layout cell content
            layoutNode(cell.get(), gfx, fontSize, scale);
            
            // Update column width
            if (colIndex >= colWidths.size()) {
                colWidths.push_back(cell->box.width);
            } else {
                colWidths[colIndex] = std::max(colWidths[colIndex], cell->box.width);
            }
            
            rowHeight = std::max(rowHeight, cell->box.height);
            rowBaseline = std::max(rowBaseline, cell->box.baseline);
            colIndex++;
        }
        
        rowHeights.push_back(rowHeight);
        rowBaselines.push_back(rowBaseline);
    }
    
    // Second pass: position cells
    int totalWidth = 0;
    int totalHeight = 0;
    
    for (int w : colWidths) {
        totalWidth += w + cellPadding;
    }
    
    int yPos = 0;
    size_t rowIndex = 0;
    for (auto& row : node->children) {
        if (row->type != MathNodeType::MTR) continue;
        
        int xPos = 0;
        size_t colIndex = 0;
        
        for (auto& cell : row->children) {
            if (cell->type != MathNodeType::MTD) continue;
            
            // Center cell content in column
            cell->x = xPos + (colWidths[colIndex] - cell->box.width) / 2;
            cell->y = yPos + rowBaselines[rowIndex] - cell->box.baseline;
            
            xPos += colWidths[colIndex] + cellPadding;
            colIndex++;
        }
        
        yPos += rowHeights[rowIndex] + cellPadding;
        rowIndex++;
    }
    
    totalHeight = yPos;
    
    // Update bounding box
    node->box.width = totalWidth;
    node->box.height = totalHeight;
    node->box.baseline = totalHeight / 2;  // Center baseline for matrices
    node->box.ascent = node->box.baseline;
    node->box.descent = node->box.height - node->box.baseline;
}

void MathRenderer::layoutUnderOver(MathNode* node, LGFX_Device* gfx, float fontSize, 
                                    float scale, bool under, bool over) {
    if (!node || node->children.empty()) return;
    
    auto& base = node->children[0];
    layoutNode(base.get(), gfx, fontSize, scale);
    
    int baseWidth = base->box.width;
    int totalHeight = base->box.height;
    int padding = 2;
    
    MathNode* underNode = nullptr;
    MathNode* overNode = nullptr;
    
    if (under && node->children.size() > 1) {
        underNode = node->children[1].get();
        layoutNode(underNode, gfx, fontSize, scale * subscriptScale);
    }
    
    if (over && node->children.size() > (under ? 2 : 1)) {
        overNode = node->children[under ? 2 : 1].get();
        layoutNode(overNode, gfx, fontSize, scale * superscriptScale);
    }
    
    int maxWidth = baseWidth;
    if (underNode) maxWidth = std::max(maxWidth, underNode->box.width);
    if (overNode) maxWidth = std::max(maxWidth, overNode->box.width);
    
    int yOffset = 0;
    
    // Position over script
    if (overNode) {
        overNode->x = (maxWidth - overNode->box.width) / 2;
        overNode->y = 0;
        yOffset = overNode->box.height + padding;
    }
    
    // Position base
    base->x = (maxWidth - base->box.width) / 2;
    base->y = yOffset;
    yOffset += base->box.height + padding;
    
    // Position under script
    if (underNode) {
        underNode->x = (maxWidth - underNode->box.width) / 2;
        underNode->y = yOffset;
        yOffset += underNode->box.height;
    }
    
    // Update bounding box
    node->box.width = maxWidth;
    node->box.height = yOffset;
    node->box.baseline = base->y + base->box.baseline;
    node->box.ascent = node->box.baseline;
    node->box.descent = node->box.height - node->box.baseline;
}

void MathRenderer::calculateLayout(MathNode* root, LGFX_Device* gfx, float fontSize) {
    if (!root || !gfx) return;
    
    if (mathFontData) {
        gfx->loadFont(mathFontData);
    }
    
    layoutNode(root, gfx, fontSize, 1.0f);
}

void MathRenderer::renderText(const std::string& text, M5Canvas* canvas, int x, int y,
                               float fontSize, float scale, uint32_t color, 
                               const std::string& variant) {
    if (!canvas || text.empty()) return;
    
    float scaledSize = fontSize * scale;
    if (scaledSize < minFontSize) scaledSize = minFontSize;
    
    canvas->setTextSize(scaledSize);
    canvas->setTextColor(color);
    canvas->setTextDatum(textdatum_t::top_left);
    canvas->drawString(text.c_str(), x, y);
}

void MathRenderer::renderFractionLine(M5Canvas* canvas, int x, int y, int width, uint32_t color) {
    if (!canvas) return;
    canvas->drawFastHLine(x, y, width, color);
}

void MathRenderer::renderSqrtSymbol(M5Canvas* canvas, int x, int y, int width, int height, 
                                     uint32_t color) {
    if (!canvas) return;
    
    int hookWidth = width / 4;
    int hookHeight = height / 3;
    
    // Draw the hook
    canvas->drawLine(x, y + height - hookHeight, x + hookWidth/2, y + height, color);
    canvas->drawLine(x + hookWidth/2, y + height, x + hookWidth, y + height/2, color);
    
    // Draw the main diagonal
    canvas->drawLine(x + hookWidth, y + height/2, x + width/2, y, color);
    
    // Draw the top line
    canvas->drawFastHLine(x + width/2, y, width/2, color);
}

void MathRenderer::renderBracket(M5Canvas* canvas, int x, int y, int height,
                                  const std::string& bracket, float fontSize, uint32_t color) {
    if (!canvas) return;
    
    // For now, draw simple brackets - could be enhanced with stretchy versions
    canvas->setTextColor(color);
    canvas->setTextSize(fontSize);
    canvas->setTextDatum(textdatum_t::top_left);
    canvas->drawString(bracket.c_str(), x, y);
}

void MathRenderer::renderNode(MathNode* node, M5Canvas* canvas, int x, int y,
                               float fontSize, float scale, uint32_t color) {
    if (!node || !canvas) return;
    
    switch (node->type) {
        case MathNodeType::TEXT:
            renderText(node->content, canvas, x, y, fontSize, scale, color, "");
            break;
            
        case MathNodeType::MI:
        case MathNodeType::MN:
        case MathNodeType::MO:
        case MathNodeType::MTEXT: {
            std::string text = node->content;
            if (text.empty()) {
                for (auto& child : node->children) {
                    if (child->type == MathNodeType::TEXT) {
                        text += child->content;
                    }
                }
            }
            renderText(text, canvas, x, y, fontSize, scale, color, node->mathvariant);
            break;
        }
        
        case MathNodeType::MFRAC: {
            // Render numerator and denominator
            if (node->children.size() >= 2) {
                auto& num = node->children[0];
                auto& denom = node->children[1];
                
                renderNode(num.get(), canvas, x + num->x, y + num->y, 
                          fontSize, scale * fractionScale, color);
                renderNode(denom.get(), canvas, x + denom->x, y + denom->y,
                          fontSize, scale * fractionScale, color);
                
                // Draw fraction line
                int lineY = y + num->box.height + 2;
                renderFractionLine(canvas, x, lineY, node->box.width, color);
            }
            break;
        }
        
        case MathNodeType::MSQRT:
        case MathNodeType::MROOT: {
            // Draw sqrt symbol
            int sqrtWidth = (int)(fontSize * scale * 0.5);
            renderSqrtSymbol(canvas, x, y, sqrtWidth, node->box.height, color);
            
            // Render content
            for (auto& child : node->children) {
                renderNode(child.get(), canvas, x + child->x, y + child->y,
                          fontSize, scale, color);
            }
            break;
        }
        
        case MathNodeType::MFENCED: {
            // Draw opening bracket
            std::string openBracket = node->open.empty() ? "(" : node->open;
            std::string closeBracket = node->close.empty() ? ")" : node->close;
            
            int bracketWidth = (int)(fontSize * scale * 0.3);
            renderBracket(canvas, x, y, node->box.height, openBracket, fontSize * scale, color);
            
            // Render children
            for (auto& child : node->children) {
                renderNode(child.get(), canvas, x + child->x, y + child->y,
                          fontSize, scale, color);
            }
            
            // Draw closing bracket
            renderBracket(canvas, x + node->box.width - bracketWidth, y, 
                         node->box.height, closeBracket, fontSize * scale, color);
            break;
        }
        
        case MathNodeType::MSUP:
            // Render base
            if (node->children.size() >= 1) {
                renderNode(node->children[0].get(), canvas, 
                          x + node->children[0]->x, y + node->children[0]->y,
                          fontSize, scale, color);
            }
            // Render superscript
            if (node->children.size() >= 2) {
                renderNode(node->children[1].get(), canvas,
                          x + node->children[1]->x, y + node->children[1]->y,
                          fontSize, scale * superscriptScale, color);
            }
            break;
            
        case MathNodeType::MSUB:
            // Render base
            if (node->children.size() >= 1) {
                renderNode(node->children[0].get(), canvas,
                          x + node->children[0]->x, y + node->children[0]->y,
                          fontSize, scale, color);
            }
            // Render subscript
            if (node->children.size() >= 2) {
                renderNode(node->children[1].get(), canvas,
                          x + node->children[1]->x, y + node->children[1]->y,
                          fontSize, scale * subscriptScale, color);
            }
            break;
            
        case MathNodeType::MSUBSUP:
            // Render all three parts
            for (size_t i = 0; i < node->children.size() && i < 3; i++) {
                float childScale = (i == 0) ? scale : scale * subscriptScale;
                renderNode(node->children[i].get(), canvas,
                          x + node->children[i]->x, y + node->children[i]->y,
                          fontSize, childScale, color);
            }
            break;
            
        default:
            // Container types - render all children
            for (auto& child : node->children) {
                renderNode(child.get(), canvas, x + child->x, y + child->y,
                          fontSize, scale, color);
            }
            break;
    }
}

MathRenderResult MathRenderer::render(MathNode* root, M5Canvas* canvas,
                                       int x, int y, float fontSize, uint32_t color) {
    MathRenderResult result = {0, 0, 0, false};
    
    if (!root || !canvas) return result;
    
    if (mathFontData) {
        canvas->loadFont(mathFontData);
    }
    
    // Render from baseline position
    int topY = y - root->box.baseline;
    renderNode(root, canvas, x, topY, fontSize, 1.0f, color);
    
    result.width = root->box.width;
    result.height = root->box.height;
    result.baseline = root->box.baseline;
    result.success = true;
    
    return result;
}

int MathRenderer::measureWidth(const std::string& mathml, LGFX_Device* gfx, float fontSize) {
    auto tree = parse(mathml);
    if (!tree) return 0;
    
    calculateLayout(tree.get(), gfx, fontSize);
    return tree->box.width;
}

int MathRenderer::measureHeight(const std::string& mathml, LGFX_Device* gfx, float fontSize) {
    auto tree = parse(mathml);
    if (!tree) return 0;
    
    calculateLayout(tree.get(), gfx, fontSize);
    return tree->box.height;
}
