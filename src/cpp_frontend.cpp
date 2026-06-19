#include "cpp_frontend.h"

#include <array>
#include <cstring>
#include <stdexcept>
#include <string_view>
#include <vector>

#include <tree_sitter/api.h>

#include "hash_util.h"

extern "C" {
const TSLanguage* tree_sitter_cpp();
}

namespace codegraph {
namespace {

class Parser {
public:
    explicit Parser(const TSLanguage* language) : parser_(ts_parser_new()) {
        if (parser_ == nullptr) {
            throw std::runtime_error("failed to allocate tree-sitter parser");
        }
        if (!ts_parser_set_language(parser_, language)) {
            throw std::runtime_error("failed to set tree-sitter language");
        }
    }

    ~Parser() {
        ts_parser_delete(parser_);
    }

    Parser(const Parser&) = delete;
    Parser& operator=(const Parser&) = delete;

    TSTree* parse(std::string_view source) {
        TSTree* tree = ts_parser_parse_string(
            parser_,
            nullptr,
            source.data(),
            static_cast<uint32_t>(source.size())
        );
        if (tree == nullptr) {
            throw std::runtime_error("tree-sitter parse failed");
        }
        return tree;
    }

private:
    TSParser* parser_ = nullptr;
};

class Tree {
public:
    explicit Tree(TSTree* tree) : tree_(tree) {}
    ~Tree() {
        ts_tree_delete(tree_);
    }

    Tree(const Tree&) = delete;
    Tree& operator=(const Tree&) = delete;

    TSNode root() const {
        return ts_tree_root_node(tree_);
    }

private:
    TSTree* tree_ = nullptr;
};

TSNode child_by_field(TSNode node, const char* field) {
    return ts_node_child_by_field_name(node, field, static_cast<uint32_t>(std::strlen(field)));
}

std::string node_text(std::string_view source, TSNode node) {
    if (ts_node_is_null(node)) {
        return {};
    }
    const uint32_t start = ts_node_start_byte(node);
    const uint32_t end = ts_node_end_byte(node);
    if (start > end || end > source.size()) {
        throw std::runtime_error("tree-sitter node byte range is outside source");
    }
    return std::string(source.substr(start, end - start));
}

std::string_view node_type(TSNode node) {
    return ts_node_type(node);
}

std::string join_qualified(const std::vector<std::string>& scope, std::string_view name) {
    if (scope.empty()) {
        return std::string(name);
    }

    std::string qualified;
    for (const std::string& part : scope) {
        if (!qualified.empty()) {
            qualified += "::";
        }
        qualified += part;
    }

    const std::string name_text(name);
    const std::string qualified_prefix = qualified + "::";
    if (name_text == qualified || name_text.rfind(qualified_prefix, 0) == 0) {
        return name_text;
    }

    qualified += "::";
    qualified += name_text;
    return qualified;
}

std::string unqualified_name(std::string name) {
    const size_t pos = name.rfind("::");
    if (pos != std::string::npos) {
        return name.substr(pos + 2U);
    }
    return name;
}

bool is_identifier_like(std::string_view type) {
    return type == "identifier" ||
           type == "field_identifier" ||
           type == "type_identifier" ||
           type == "namespace_identifier" ||
           type == "qualified_identifier" ||
           type == "destructor_name" ||
           type == "operator_name";
}

TSNode find_declarator_name(TSNode node) {
    if (ts_node_is_null(node)) {
        return node;
    }

    if (is_identifier_like(node_type(node))) {
        return node;
    }

    TSNode declarator = child_by_field(node, "declarator");
    if (!ts_node_is_null(declarator)) {
        TSNode nested = find_declarator_name(declarator);
        if (!ts_node_is_null(nested)) {
            return nested;
        }
    }

    const uint32_t count = ts_node_named_child_count(node);
    for (uint32_t i = 0; i < count; ++i) {
        TSNode nested = find_declarator_name(ts_node_named_child(node, i));
        if (!ts_node_is_null(nested)) {
            return nested;
        }
    }

    return TSNode{};
}

std::string symbol_kind_for_type(std::string_view type) {
    if (type == "class_specifier") return SymbolKinds::Class;
    if (type == "struct_specifier") return SymbolKinds::Struct;
    if (type == "namespace_definition") return SymbolKinds::Namespace;
    if (type == "enum_specifier") return SymbolKinds::Enum;
    return SymbolKinds::Other;
}

std::string strip_include_delimiters(std::string text) {
    if (text.size() >= 2U) {
        const char first = text.front();
        const char last = text.back();
        if ((first == '<' && last == '>') || (first == '"' && last == '"')) {
            return text.substr(1U, text.size() - 2U);
        }
    }
    return text;
}

void collect_includes(TSNode node, std::string_view source, std::vector<IncludeInfo>& includes) {
    if (node_type(node) == "preproc_include") {
        TSNode path = child_by_field(node, "path");
        if (!ts_node_is_null(path)) {
            includes.push_back(IncludeInfo{strip_include_delimiters(node_text(source, path))});
        }
        return;
    }

    const uint32_t count = ts_node_named_child_count(node);
    for (uint32_t i = 0; i < count; ++i) {
        collect_includes(ts_node_named_child(node, i), source, includes);
    }
}

void collect_symbols(
    TSNode node,
    std::string_view source,
    std::vector<std::string>& scope,
    std::vector<int>& parent_stack,
    int type_depth,
    std::vector<SymbolInfo>& symbols
) {
    const std::string_view type = node_type(node);

    if (type == "namespace_definition" ||
        type == "class_specifier" ||
        type == "struct_specifier" ||
        type == "enum_specifier") {
        TSNode name_node = child_by_field(node, "name");
        if (!ts_node_is_null(name_node)) {
            const std::string name = node_text(source, name_node);
            const TSPoint start = ts_node_start_point(node);
            const TSPoint end = ts_node_end_point(node);
            const uint32_t start_byte = ts_node_start_byte(node);
            const uint32_t end_byte = ts_node_end_byte(node);

            SymbolInfo symbol;
            symbol.kind = symbol_kind_for_type(type);
            symbol.name = unqualified_name(name);
            symbol.qualified_name = join_qualified(scope, name);
            symbol.start_line = start.row + 1U;
            symbol.end_line = end.row + 1U;
            symbol.start_byte = start_byte;
            symbol.end_byte = end_byte;
            symbol.content_hash = xxh64_hex(source.substr(start_byte, end_byte - start_byte));
            symbol.parent_index = parent_stack.empty() ? -1 : parent_stack.back();

            const int current_index = static_cast<int>(symbols.size());
            symbols.push_back(std::move(symbol));

            scope.push_back(name);
            parent_stack.push_back(current_index);
            const int next_type_depth = type == "namespace_definition" ? type_depth : type_depth + 1;
            const uint32_t count = ts_node_named_child_count(node);
            for (uint32_t i = 0; i < count; ++i) {
                collect_symbols(
                    ts_node_named_child(node, i),
                    source,
                    scope,
                    parent_stack,
                    next_type_depth,
                    symbols
                );
            }
            parent_stack.pop_back();
            scope.pop_back();
        }
        return;
    }

    if (type == "function_definition") {
        TSNode declarator = child_by_field(node, "declarator");
        TSNode name_node = find_declarator_name(declarator);
        if (!ts_node_is_null(name_node)) {
            const std::string raw_name = node_text(source, name_node);
            const TSPoint start = ts_node_start_point(node);
            const TSPoint end = ts_node_end_point(node);
            const uint32_t start_byte = ts_node_start_byte(node);
            const uint32_t end_byte = ts_node_end_byte(node);

            SymbolInfo symbol;
            symbol.kind = type_depth > 0 || raw_name.find("::") != std::string::npos
                              ? SymbolKinds::Method
                              : SymbolKinds::Function;
            symbol.name = unqualified_name(raw_name);
            symbol.qualified_name = join_qualified(scope, raw_name);
            symbol.signature = node_text(source, declarator);
            symbol.start_line = start.row + 1U;
            symbol.end_line = end.row + 1U;
            symbol.start_byte = start_byte;
            symbol.end_byte = end_byte;
            symbol.content_hash = xxh64_hex(source.substr(start_byte, end_byte - start_byte));
            symbol.parent_index = parent_stack.empty() ? -1 : parent_stack.back();
            symbols.push_back(std::move(symbol));
        }
        return;
    }

    const uint32_t count = ts_node_named_child_count(node);
    for (uint32_t i = 0; i < count; ++i) {
        collect_symbols(ts_node_named_child(node, i), source, scope, parent_stack, type_depth, symbols);
    }
}

}  // namespace

std::string_view CppFrontend::language() const {
    return "cpp";
}

std::span<const std::string_view> CppFrontend::extensions() const {
    static constexpr std::array<std::string_view, 5> kExtensions{
        ".cpp",
        ".cc",
        ".cxx",
        ".h",
        ".hpp",
    };
    return kExtensions;
}

ExtractedFile CppFrontend::extract(std::string_view source) const {
    Parser parser(tree_sitter_cpp());
    Tree tree(parser.parse(source));

    ExtractedFile extracted;
    TSNode root = tree.root();
    collect_includes(root, source, extracted.includes);

    std::vector<std::string> scope;
    std::vector<int> parent_stack;
    collect_symbols(root, source, scope, parent_stack, 0, extracted.symbols);

    return extracted;
}

}  // namespace codegraph
