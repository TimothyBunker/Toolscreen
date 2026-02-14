#pragma once

// ============================================================================
// EXPRESSION_PARSER.H - Safe Expression Evaluation for Dynamic Dimensions
// ============================================================================
// Evaluates simple math expressions with screen dimension variables.
// Designed for security: whitelist-only identifiers, no arbitrary code execution.
// ============================================================================

#include <string>
#include <cmath>

// Evaluate an expression string with the given screen dimensions.
// Supported:
//   Variables: screenWidth, screenHeight
//   Operators: +, -, *, / (standard precedence)
//   Functions: min(a,b), max(a,b), floor(x), ceil(x), round(x), abs(x), roundEven(x)
//   Parentheses for grouping
// Returns: Evaluated integer result (floored)
// On error: Returns defaultValue
int EvaluateExpression(const std::string& expr, int screenWidth, int screenHeight, int defaultValue = 0);

// Check if a string should be treated as an expression (vs a pure integer)
// Returns true if the string contains non-numeric characters (letters, operators, etc.)
bool IsExpression(const std::string& str);

// Validate expression syntax without evaluating.
// Returns true if valid, false if invalid.
// If invalid, errorOut contains a human-readable error message.
bool ValidateExpression(const std::string& expr, std::string& errorOut);

// Recalculate all expression-based dimensions in the config.
// Called when screen resolution changes or after config load.
// This updates the cached integer values (width, height, etc.) from expression strings.
void RecalculateExpressionDimensions();
