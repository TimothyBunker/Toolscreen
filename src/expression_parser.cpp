// ============================================================================
// EXPRESSION_PARSER.CPP - Safe Expression Evaluation Implementation
// ============================================================================
// Implements a simple recursive descent parser for math expressions.
// Security: Only whitelisted identifiers (screenWidth, screenHeight) allowed.
// No eval(), no string execution, no arbitrary code paths.
// ============================================================================

#include "expression_parser.h"
#include "gui.h"
#include "logic_thread.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

// ============================================================================
// Tokenizer
// ============================================================================

// Using Expr prefix to avoid Windows header macro conflicts
enum class ExprTokenKind {
    Number,
    Identifier,
    Plus,
    Minus,
    Star,
    Slash,
    LParen,
    RParen,
    Comma,
    End,
    Invalid
};

struct ExprToken {
    ExprTokenKind kind;
    std::string text;
    double numValue;

    ExprToken() : kind(ExprTokenKind::End), text(""), numValue(0) {}
    ExprToken(ExprTokenKind k, const std::string& s, double n) : kind(k), text(s), numValue(n) {}
};

class Tokenizer {
public:
    Tokenizer(const std::string& input) : m_input(input), m_pos(0) {}

    ExprToken next() {
        skipWhitespace();
        if (m_pos >= m_input.size()) { return ExprToken(ExprTokenKind::End, "", 0); }

        char c = m_input[m_pos];

        // Single character tokens
        if (c == '+') {
            m_pos++;
            return ExprToken(ExprTokenKind::Plus, "+", 0);
        }
        if (c == '-') {
            m_pos++;
            return ExprToken(ExprTokenKind::Minus, "-", 0);
        }
        if (c == '*') {
            m_pos++;
            return ExprToken(ExprTokenKind::Star, "*", 0);
        }
        if (c == '/') {
            m_pos++;
            return ExprToken(ExprTokenKind::Slash, "/", 0);
        }
        if (c == '(') {
            m_pos++;
            return ExprToken(ExprTokenKind::LParen, "(", 0);
        }
        if (c == ')') {
            m_pos++;
            return ExprToken(ExprTokenKind::RParen, ")", 0);
        }
        if (c == ',') {
            m_pos++;
            return ExprToken(ExprTokenKind::Comma, ",", 0);
        }

        // Numbers (including decimals)
        if (std::isdigit(c) || c == '.') {
            size_t start = m_pos;
            bool hasDecimal = false;
            while (m_pos < m_input.size() && (std::isdigit(m_input[m_pos]) || m_input[m_pos] == '.')) {
                if (m_input[m_pos] == '.') {
                    if (hasDecimal) break;
                    hasDecimal = true;
                }
                m_pos++;
            }
            std::string numStr = m_input.substr(start, m_pos - start);
            double num = std::stod(numStr);
            return ExprToken(ExprTokenKind::Number, numStr, num);
        }

        // Identifiers (variable names and function names)
        if (std::isalpha(c) || c == '_') {
            size_t start = m_pos;
            while (m_pos < m_input.size() && (std::isalnum(m_input[m_pos]) || m_input[m_pos] == '_')) { m_pos++; }
            std::string id = m_input.substr(start, m_pos - start);
            return ExprToken(ExprTokenKind::Identifier, id, 0);
        }

        // Invalid character
        m_pos++;
        return ExprToken(ExprTokenKind::Invalid, std::string(1, c), 0);
    }

    size_t position() const { return m_pos; }

private:
    void skipWhitespace() {
        while (m_pos < m_input.size() && std::isspace(m_input[m_pos])) { m_pos++; }
    }

    std::string m_input;
    size_t m_pos;
};

// ============================================================================
// Parser
// ============================================================================

class ExpressionParser {
public:
    ExpressionParser(const std::string& expr, int screenWidth, int screenHeight)
        : m_tokenizer(expr), m_screenWidth(screenWidth), m_screenHeight(screenHeight) {
        m_currentToken = m_tokenizer.next();
    }

    double parse() {
        double result = parseExpression();
        if (m_currentToken.kind != ExprTokenKind::End) { throw std::runtime_error("Unexpected token at end: " + m_currentToken.text); }
        return result;
    }

    std::string validate() {
        try {
            parseExpression();
            if (m_currentToken.kind != ExprTokenKind::End) { return "Unexpected token at end: " + m_currentToken.text; }
            return ""; // Valid
        } catch (const std::exception& e) { return e.what(); }
    }

private:
    // Expression = Term (('+' | '-') Term)*
    double parseExpression() {
        double left = parseTerm();
        while (m_currentToken.kind == ExprTokenKind::Plus || m_currentToken.kind == ExprTokenKind::Minus) {
            ExprTokenKind op = m_currentToken.kind;
            advance();
            double right = parseTerm();
            if (op == ExprTokenKind::Plus) {
                left = left + right;
            } else {
                left = left - right;
            }
        }
        return left;
    }

    // Term = Unary (('*' | '/') Unary)*
    double parseTerm() {
        double left = parseUnary();
        while (m_currentToken.kind == ExprTokenKind::Star || m_currentToken.kind == ExprTokenKind::Slash) {
            ExprTokenKind op = m_currentToken.kind;
            advance();
            double right = parseUnary();
            if (op == ExprTokenKind::Star) {
                left = left * right;
            } else {
                if (right == 0) { throw std::runtime_error("Division by zero"); }
                left = left / right;
            }
        }
        return left;
    }

    // Unary = ('-')? Primary
    double parseUnary() {
        if (m_currentToken.kind == ExprTokenKind::Minus) {
            advance();
            return -parseUnary();
        }
        if (m_currentToken.kind == ExprTokenKind::Plus) {
            advance();
            return parseUnary();
        }
        return parsePrimary();
    }

    // Primary = Number | Identifier | FunctionCall | '(' Expression ')'
    double parsePrimary() {
        if (m_currentToken.kind == ExprTokenKind::Number) {
            double val = m_currentToken.numValue;
            advance();
            return val;
        }

        if (m_currentToken.kind == ExprTokenKind::Identifier) {
            std::string id = m_currentToken.text;
            advance();

            // Check for function call
            if (m_currentToken.kind == ExprTokenKind::LParen) { return parseFunctionCall(id); }

            // Variable lookup
            return lookupVariable(id);
        }

        if (m_currentToken.kind == ExprTokenKind::LParen) {
            advance();
            double val = parseExpression();
            expect(ExprTokenKind::RParen, "Expected ')'");
            return val;
        }

        throw std::runtime_error("Unexpected token: " + m_currentToken.text);
    }

    double parseFunctionCall(const std::string& funcName) {
        expect(ExprTokenKind::LParen, "Expected '(' after function name");

        // Parse arguments
        std::vector<double> args;
        if (m_currentToken.kind != ExprTokenKind::RParen) {
            args.push_back(parseExpression());
            while (m_currentToken.kind == ExprTokenKind::Comma) {
                advance();
                args.push_back(parseExpression());
            }
        }
        expect(ExprTokenKind::RParen, "Expected ')' after function arguments");

        return callFunction(funcName, args);
    }

    double lookupVariable(const std::string& name) {
        // Whitelist of allowed variables
        if (name == "screenWidth") { return static_cast<double>(m_screenWidth); }
        if (name == "screenHeight") { return static_cast<double>(m_screenHeight); }

        throw std::runtime_error("Unknown variable: " + name);
    }

    double callFunction(const std::string& name, const std::vector<double>& args) {
        // Whitelist of allowed functions
        if (name == "min") {
            if (args.size() != 2) { throw std::runtime_error("min() requires 2 arguments"); }
            return (std::min)(args[0], args[1]);
        }
        if (name == "max") {
            if (args.size() != 2) { throw std::runtime_error("max() requires 2 arguments"); }
            return (std::max)(args[0], args[1]);
        }
        if (name == "floor") {
            if (args.size() != 1) { throw std::runtime_error("floor() requires 1 argument"); }
            return std::floor(args[0]);
        }
        if (name == "ceil") {
            if (args.size() != 1) { throw std::runtime_error("ceil() requires 1 argument"); }
            return std::ceil(args[0]);
        }
        if (name == "round") {
            if (args.size() != 1) { throw std::runtime_error("round() requires 1 argument"); }
            return std::round(args[0]);
        }
        if (name == "abs") {
            if (args.size() != 1) { throw std::runtime_error("abs() requires 1 argument"); }
            return std::abs(args[0]);
        }
        if (name == "roundEven") {
            if (args.size() != 1) { throw std::runtime_error("roundEven() requires 1 argument"); }
            // Round up to nearest even number: ceil(x/2) * 2
            return std::ceil(args[0] / 2.0) * 2.0;
        }

        throw std::runtime_error("Unknown function: " + name);
    }

    void advance() { m_currentToken = m_tokenizer.next(); }

    void expect(ExprTokenKind k, const std::string& error) {
        if (m_currentToken.kind != k) { throw std::runtime_error(error); }
        advance();
    }

    Tokenizer m_tokenizer;
    ExprToken m_currentToken;
    int m_screenWidth;
    int m_screenHeight;
};

// ============================================================================
// Public API
// ============================================================================

int EvaluateExpression(const std::string& expr, int screenWidth, int screenHeight, int defaultValue) {
    if (expr.empty()) { return defaultValue; }

    // Trim whitespace
    std::string trimmed = expr;
    size_t start = trimmed.find_first_not_of(" \t\r\n");
    size_t end = trimmed.find_last_not_of(" \t\r\n");
    if (start == std::string::npos) { return defaultValue; }
    trimmed = trimmed.substr(start, end - start + 1);

    if (trimmed.empty()) { return defaultValue; }

    try {
        ExpressionParser parser(trimmed, screenWidth, screenHeight);
        double result = parser.parse();
        return static_cast<int>(std::floor(result));
    } catch (const std::exception&) { return defaultValue; }
}

bool IsExpression(const std::string& str) {
    if (str.empty()) { return false; }

    // Trim whitespace
    std::string trimmed = str;
    size_t start = trimmed.find_first_not_of(" \t\r\n");
    size_t end = trimmed.find_last_not_of(" \t\r\n");
    if (start == std::string::npos) { return false; }
    trimmed = trimmed.substr(start, end - start + 1);

    // Check if it's a pure integer (possibly with leading minus)
    size_t checkStart = 0;
    if (!trimmed.empty() && trimmed[0] == '-') { checkStart = 1; }

    if (checkStart >= trimmed.size()) { return true; } // Just a minus sign = expression

    for (size_t i = checkStart; i < trimmed.size(); i++) {
        if (!std::isdigit(trimmed[i])) { return true; } // Has non-digit = expression
    }
    return false; // Pure integer = not an expression
}

bool ValidateExpression(const std::string& expr, std::string& errorOut) {
    if (expr.empty()) {
        errorOut = "Expression cannot be empty";
        return false;
    }

    // Trim whitespace
    std::string trimmed = expr;
    size_t start = trimmed.find_first_not_of(" \t\r\n");
    size_t end = trimmed.find_last_not_of(" \t\r\n");
    if (start == std::string::npos) {
        errorOut = "Expression cannot be empty";
        return false;
    }
    trimmed = trimmed.substr(start, end - start + 1);

    try {
        // Use dummy screen dimensions for validation
        ExpressionParser parser(trimmed, 1920, 1080);
        std::string error = parser.validate();
        if (!error.empty()) {
            errorOut = error;
            return false;
        }
        errorOut.clear();
        return true;
    } catch (const std::exception& e) {
        errorOut = e.what();
        return false;
    }
}

void RecalculateExpressionDimensions() {
    int screenW = GetCachedScreenWidth();
    int screenH = GetCachedScreenHeight();

    // Recalculate mode dimensions from expressions
    for (auto& mode : g_config.modes) {
        // Width expression
        if (!mode.widthExpr.empty()) {
            int newWidth = EvaluateExpression(mode.widthExpr, screenW, screenH, mode.width);
            if (newWidth > 0) { mode.width = newWidth; }
        }
        // Height expression
        if (!mode.heightExpr.empty()) {
            int newHeight = EvaluateExpression(mode.heightExpr, screenW, screenH, mode.height);
            if (newHeight > 0) { mode.height = newHeight; }
        }

        // Stretch expressions
        if (!mode.stretch.widthExpr.empty()) {
            int val = EvaluateExpression(mode.stretch.widthExpr, screenW, screenH, mode.stretch.width);
            if (val >= 0) { mode.stretch.width = val; }
        }
        if (!mode.stretch.heightExpr.empty()) {
            int val = EvaluateExpression(mode.stretch.heightExpr, screenW, screenH, mode.stretch.height);
            if (val >= 0) { mode.stretch.height = val; }
        }
        if (!mode.stretch.xExpr.empty()) { mode.stretch.x = EvaluateExpression(mode.stretch.xExpr, screenW, screenH, mode.stretch.x); }
        if (!mode.stretch.yExpr.empty()) { mode.stretch.y = EvaluateExpression(mode.stretch.yExpr, screenW, screenH, mode.stretch.y); }
    }
}
