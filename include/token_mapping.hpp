#ifndef _TOKEN_MAPPING
#define _TOKEN_MAPPING

#include <string>
#include <unordered_map>
#include <iosfwd>
#include <cstddef>

struct Token
{
    enum class Type
    {
        USER_DEFINED,
        INVALID,
        START_OF_INPUT,
        END_OF_INPUT
    };

    static const Token INVALID;
    static const Token START_OF_INPUT;
    static const Token END_OF_INPUT;

    Type type;
    std::string name;

    struct Hash
    {
        size_t operator()(const Token &token) const;
    };
};

bool operator==(const Token &lhs, const Token &rhs);

class TokenMapping
{
    std::unordered_map<Token, size_t, Token::Hash> tokens;

public:
    TokenMapping() = default;
    void insert(const Token &token);
    bool contains(const Token &token) const;
    size_t backing_type_bits() const;
    size_t token_id(const Token &token) const;
    size_t num_tokens() const;
    void print_tokens() const;
};

#endif