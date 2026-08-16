/*
 * UNO Q Codex Matrix native lifecycle hook.
 *
 * This is an independent, dependency-free implementation of the privacy
 * boundary used by the short-lived Codex hook process.  It deliberately keeps
 * no input after exit and emits only the protocol-v1 allowlist.
 */

#define _GNU_SOURCE 1

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define INPUT_LIMIT 65536u
#define WIRE_LIMIT 4096u
#define JSON_DEPTH_LIMIT 64u
#define TOKEN_LIMIT 512u
#define DEFAULT_SOCKET "/run/unoq-codex-matrix/events.sock"

typedef enum {
    JV_INVALID = 0,
    JV_STRING,
    JV_NUMBER,
    JV_OBJECT,
    JV_ARRAY,
    JV_TRUE,
    JV_FALSE,
    JV_NULL
} JsonType;

typedef struct {
    JsonType type;
    size_t start;
    size_t end;
    size_t content_start;
    size_t content_end;
} JsonValue;

typedef struct {
    const unsigned char *data;
    size_t length;
    size_t position;
    unsigned depth;
} JsonParser;

typedef enum {
    ST_OFF = 0,
    ST_IDLE = 1,
    ST_THINKING = 2,
    ST_READING = 3,
    ST_WRITING = 4,
    ST_COMMAND = 5,
    ST_BUILDING = 6,
    ST_TESTING = 7,
    ST_FLASHING = 8,
    ST_WAITING = 9,
    ST_SUCCESS = 10,
    ST_ERROR = 11,
    ST_OFFLINE = 12,
    ST_SUBAGENT = 13
} MatrixState;

typedef struct {
    const char *canonical;
    const char *wire;
    MatrixState state;
    int stop_reply;
} EventMap;

static const EventMap EVENT_MAP[] = {
    {"sessionstart", "session_start", ST_IDLE, 0},
    {"userpromptsubmit", "user_prompt", ST_THINKING, 0},
    {"pretooluse", "pre_tool", ST_COMMAND, 0},
    {"permissionrequest", "permission", ST_WAITING, 0},
    {"posttooluse", "post_tool", ST_THINKING, 0},
    {"precompact", "pre_compact", ST_THINKING, 0},
    {"postcompact", "post_compact", ST_THINKING, 0},
    {"subagentstart", "subagent_start", ST_SUBAGENT, 0},
    {"subagentstop", "subagent_stop", ST_THINKING, 1},
    {"stop", "stop", ST_SUCCESS, 1},
    {"sessionend", "session_end", ST_IDLE, 0},
};

static const char *const STATE_NAMES[] = {
    "off",       "idle",     "thinking", "reading", "writing",
    "command",   "building", "testing",  "flashing", "waiting",
    "success",   "error",    "offline",  "subagent",
};

static void skip_ws(JsonParser *parser) {
    while (parser->position < parser->length) {
        unsigned char value = parser->data[parser->position];
        if (value != ' ' && value != '\t' && value != '\n' && value != '\r') {
            break;
        }
        parser->position++;
    }
}

static int hex_digit(unsigned char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

static int read_u_escape(const unsigned char *data, size_t length, size_t offset,
                         uint32_t *result) {
    unsigned index;
    uint32_t value = 0;
    if (offset + 4 > length) return 0;
    for (index = 0; index < 4; index++) {
        int digit = hex_digit(data[offset + index]);
        if (digit < 0) return 0;
        value = (value << 4) | (uint32_t)digit;
    }
    *result = value;
    return 1;
}

static int valid_utf8_at(const unsigned char *data, size_t length, size_t *offset) {
    size_t at = *offset;
    unsigned char first;
    unsigned need;
    uint32_t code;
    unsigned i;
    if (at >= length) return 0;
    first = data[at];
    if (first < 0x80) {
        (*offset)++;
        return 1;
    }
    if (first >= 0xc2 && first <= 0xdf) {
        need = 1;
        code = first & 0x1fu;
    } else if (first >= 0xe0 && first <= 0xef) {
        need = 2;
        code = first & 0x0fu;
    } else if (first >= 0xf0 && first <= 0xf4) {
        need = 3;
        code = first & 0x07u;
    } else {
        return 0;
    }
    if (at + need >= length) return 0;
    for (i = 1; i <= need; i++) {
        unsigned char byte = data[at + i];
        if ((byte & 0xc0u) != 0x80u) return 0;
        code = (code << 6) | (byte & 0x3fu);
    }
    if ((need == 2 && code < 0x800u) || (need == 3 && code < 0x10000u) ||
        code > 0x10ffffu || (code >= 0xd800u && code <= 0xdfffu)) {
        return 0;
    }
    *offset = at + need + 1;
    return 1;
}

static int parse_string(JsonParser *parser, JsonValue *result) {
    size_t start;
    if (parser->position >= parser->length || parser->data[parser->position] != '"') {
        return 0;
    }
    start = parser->position++;
    result->content_start = parser->position;
    while (parser->position < parser->length) {
        unsigned char value = parser->data[parser->position++];
        if (value == '"') {
            result->type = JV_STRING;
            result->start = start;
            result->end = parser->position;
            result->content_end = parser->position - 1;
            return 1;
        }
        if (value < 0x20u) return 0;
        if (value == '\\') {
            uint32_t first;
            if (parser->position >= parser->length) return 0;
            value = parser->data[parser->position++];
            if (strchr("\"\\/bfnrt", (int)value) != NULL) continue;
            if (value != 'u' ||
                !read_u_escape(parser->data, parser->length, parser->position, &first)) {
                return 0;
            }
            parser->position += 4;
            if (first >= 0xd800u && first <= 0xdbffu) {
                uint32_t second;
                if (parser->position + 6 > parser->length ||
                    parser->data[parser->position] != '\\' ||
                    parser->data[parser->position + 1] != 'u' ||
                    !read_u_escape(parser->data, parser->length,
                                   parser->position + 2, &second) ||
                    second < 0xdc00u || second > 0xdfffu) {
                    return 0;
                }
                parser->position += 6;
            } else if (first >= 0xdc00u && first <= 0xdfffu) {
                return 0;
            }
        } else if (value >= 0x80u) {
            size_t offset = parser->position - 1;
            if (!valid_utf8_at(parser->data, parser->length, &offset)) return 0;
            parser->position = offset;
        }
    }
    return 0;
}

static int parse_value(JsonParser *parser, JsonValue *result);

static int parse_compound(JsonParser *parser, JsonValue *result, int object) {
    unsigned char open = object ? '{' : '[';
    unsigned char close = object ? '}' : ']';
    size_t start = parser->position;
    if (parser->depth >= JSON_DEPTH_LIMIT || parser->data[parser->position] != open) return 0;
    parser->depth++;
    parser->position++;
    skip_ws(parser);
    if (parser->position < parser->length && parser->data[parser->position] == close) {
        parser->position++;
        parser->depth--;
        result->type = object ? JV_OBJECT : JV_ARRAY;
        result->start = start;
        result->end = parser->position;
        return 1;
    }
    for (;;) {
        JsonValue child;
        if (object) {
            if (!parse_string(parser, &child)) return 0;
            skip_ws(parser);
            if (parser->position >= parser->length || parser->data[parser->position++] != ':') {
                return 0;
            }
            skip_ws(parser);
        }
        if (!parse_value(parser, &child)) return 0;
        skip_ws(parser);
        if (parser->position >= parser->length) return 0;
        if (parser->data[parser->position] == close) {
            parser->position++;
            parser->depth--;
            result->type = object ? JV_OBJECT : JV_ARRAY;
            result->start = start;
            result->end = parser->position;
            return 1;
        }
        if (parser->data[parser->position++] != ',') return 0;
        skip_ws(parser);
    }
}

static int parse_number(JsonParser *parser, JsonValue *result) {
    size_t start = parser->position;
    size_t at = start;
    if (at < parser->length && parser->data[at] == '-') at++;
    if (at >= parser->length) return 0;
    if (parser->data[at] == '0') {
        at++;
        if (at < parser->length && isdigit(parser->data[at])) return 0;
    } else {
        if (parser->data[at] < '1' || parser->data[at] > '9') return 0;
        while (at < parser->length && isdigit(parser->data[at])) at++;
    }
    if (at < parser->length && parser->data[at] == '.') {
        at++;
        if (at >= parser->length || !isdigit(parser->data[at])) return 0;
        while (at < parser->length && isdigit(parser->data[at])) at++;
    }
    if (at < parser->length && (parser->data[at] == 'e' || parser->data[at] == 'E')) {
        at++;
        if (at < parser->length && (parser->data[at] == '+' || parser->data[at] == '-')) at++;
        if (at >= parser->length || !isdigit(parser->data[at])) return 0;
        while (at < parser->length && isdigit(parser->data[at])) at++;
    }
    parser->position = at;
    result->type = JV_NUMBER;
    result->start = start;
    result->end = at;
    return 1;
}

static int parse_literal(JsonParser *parser, JsonValue *result, const char *literal,
                         JsonType type) {
    size_t length = strlen(literal);
    if (parser->position + length > parser->length ||
        memcmp(parser->data + parser->position, literal, length) != 0) {
        return 0;
    }
    result->type = type;
    result->start = parser->position;
    parser->position += length;
    result->end = parser->position;
    return 1;
}

static int parse_value(JsonParser *parser, JsonValue *result) {
    skip_ws(parser);
    if (parser->position >= parser->length) return 0;
    memset(result, 0, sizeof(*result));
    switch (parser->data[parser->position]) {
        case '"': return parse_string(parser, result);
        case '{': return parse_compound(parser, result, 1);
        case '[': return parse_compound(parser, result, 0);
        case 't': return parse_literal(parser, result, "true", JV_TRUE);
        case 'f': return parse_literal(parser, result, "false", JV_FALSE);
        case 'n': return parse_literal(parser, result, "null", JV_NULL);
        default: return parse_number(parser, result);
    }
}

static int parse_document(const unsigned char *data, size_t length, JsonValue *root) {
    JsonParser parser;
    parser.data = data;
    parser.length = length;
    parser.position = 0;
    parser.depth = 0;
    if (!parse_value(&parser, root)) return 0;
    skip_ws(&parser);
    return parser.position == parser.length;
}

static size_t utf8_encode(uint32_t code, char *output, size_t capacity) {
    if (code <= 0x7fu) {
        if (capacity < 1) return 0;
        output[0] = (char)code;
        return 1;
    }
    if (code <= 0x7ffu) {
        if (capacity < 2) return 0;
        output[0] = (char)(0xc0u | (code >> 6));
        output[1] = (char)(0x80u | (code & 0x3fu));
        return 2;
    }
    if (code <= 0xffffu) {
        if (capacity < 3) return 0;
        output[0] = (char)(0xe0u | (code >> 12));
        output[1] = (char)(0x80u | ((code >> 6) & 0x3fu));
        output[2] = (char)(0x80u | (code & 0x3fu));
        return 3;
    }
    if (capacity < 4) return 0;
    output[0] = (char)(0xf0u | (code >> 18));
    output[1] = (char)(0x80u | ((code >> 12) & 0x3fu));
    output[2] = (char)(0x80u | ((code >> 6) & 0x3fu));
    output[3] = (char)(0x80u | (code & 0x3fu));
    return 4;
}

static int decode_string(const unsigned char *data, const JsonValue *value,
                         char *output, size_t capacity, size_t *decoded_length) {
    size_t at;
    size_t used = 0;
    if (value->type != JV_STRING || capacity == 0) return 0;
    for (at = value->content_start; at < value->content_end;) {
        unsigned char byte = data[at++];
        if (byte != '\\') {
            if (used + 1 >= capacity) return 0;
            output[used++] = (char)byte;
            continue;
        }
        byte = data[at++];
        switch (byte) {
            case '"': case '\\': case '/':
                if (used + 1 >= capacity) return 0;
                output[used++] = (char)byte;
                break;
            case 'b': byte = '\b'; goto escaped_byte;
            case 'f': byte = '\f'; goto escaped_byte;
            case 'n': byte = '\n'; goto escaped_byte;
            case 'r': byte = '\r'; goto escaped_byte;
            case 't': byte = '\t';
escaped_byte:
                if (used + 1 >= capacity) return 0;
                output[used++] = (char)byte;
                break;
            case 'u': {
                uint32_t code;
                size_t written;
                if (!read_u_escape(data, value->content_end, at, &code)) return 0;
                at += 4;
                if (code >= 0xd800u && code <= 0xdbffu) {
                    uint32_t second;
                    at += 2; /* parse_string already validated the following \\u. */
                    if (!read_u_escape(data, value->content_end, at, &second)) return 0;
                    at += 4;
                    code = 0x10000u + ((code - 0xd800u) << 10) + (second - 0xdc00u);
                }
                if (code == 0) return 0;
                written = utf8_encode(code, output + used, capacity - used - 1);
                if (written == 0) return 0;
                used += written;
                break;
            }
            default: return 0;
        }
    }
    output[used] = '\0';
    if (decoded_length != NULL) *decoded_length = used;
    return 1;
}

static int string_equals(const unsigned char *data, const JsonValue *value,
                         const char *wanted) {
    char decoded[96];
    size_t length;
    return decode_string(data, value, decoded, sizeof(decoded), &length) &&
           length == strlen(wanted) && memcmp(decoded, wanted, length) == 0;
}

static int object_find_key(const unsigned char *data, size_t length,
                           const JsonValue *object, const char *key,
                           JsonValue *result) {
    JsonParser parser;
    int found = 0;
    if (object->type != JV_OBJECT || object->end <= object->start + 1) return 0;
    parser.data = data;
    parser.length = length;
    parser.position = object->start + 1;
    parser.depth = 0;
    skip_ws(&parser);
    while (parser.position < object->end && data[parser.position] != '}') {
        JsonValue candidate_key;
        JsonValue candidate_value;
        if (!parse_string(&parser, &candidate_key)) return 0;
        skip_ws(&parser);
        if (parser.position >= object->end || data[parser.position++] != ':') return 0;
        if (!parse_value(&parser, &candidate_value)) return 0;
        if (string_equals(data, &candidate_key, key)) {
            *result = candidate_value;
            found = 1; /* Match json.loads: the final duplicate wins. */
        }
        skip_ws(&parser);
        if (parser.position < object->end && data[parser.position] == ',') {
            parser.position++;
            skip_ws(&parser);
        } else {
            break;
        }
    }
    return found;
}

static int object_find_alias(const unsigned char *data, size_t length,
                             const JsonValue *object, const char *const *keys,
                             size_t key_count, JsonValue *result) {
    size_t index;
    for (index = 0; index < key_count; index++) {
        if (object_find_key(data, length, object, keys[index], result)) return 1;
    }
    memset(result, 0, sizeof(*result));
    return 0;
}

static int text_field(const unsigned char *data, const JsonValue *value,
                      int present, int required, size_t maximum,
                      char *decoded, size_t decoded_capacity) {
    size_t length = 0;
    size_t characters = 0;
    size_t index;
    if (!present || value->type == JV_NULL) {
        if (required) return 0;
        if (decoded_capacity) decoded[0] = '\0';
        return 1;
    }
    if (value->type != JV_STRING ||
        !decode_string(data, value, decoded, decoded_capacity, &length)) return 0;
    for (index = 0; index < length; index++) {
        if (((unsigned char)decoded[index] & 0xc0u) != 0x80u) characters++;
    }
    return (!required || characters != 0) && characters <= maximum;
}

static const EventMap *map_event(const char *event) {
    char canonical[96];
    size_t used = 0;
    size_t index;
    while (*event != '\0' && used + 1 < sizeof(canonical)) {
        unsigned char byte = (unsigned char)*event++;
        if (isalnum(byte)) canonical[used++] = (char)tolower(byte);
    }
    canonical[used] = '\0';
    for (index = 0; index < sizeof(EVENT_MAP) / sizeof(EVENT_MAP[0]); index++) {
        if (strcmp(canonical, EVENT_MAP[index].canonical) == 0) return &EVENT_MAP[index];
    }
    return NULL;
}

typedef struct {
    char *text;
    int boundary;
} ShellToken;

static int shell_tokens(const char *command, char *storage, size_t capacity,
                        ShellToken *tokens, size_t *token_count) {
    enum { SH_NORMAL, SH_SINGLE, SH_DOUBLE } quote = SH_NORMAL;
    size_t used = 0;
    size_t count = 0;
    int in_token = 0;
    const unsigned char *at = (const unsigned char *)command;
    while (*at != '\0') {
        unsigned char byte = *at++;
        if (quote == SH_SINGLE) {
            if (byte == '\'') quote = SH_NORMAL;
            else {
                if (used + 2 > capacity) return 0;
                storage[used++] = (char)byte;
                in_token = 1;
            }
            continue;
        }
        if (quote == SH_DOUBLE) {
            if (byte == '"') {
                quote = SH_NORMAL;
                in_token = 1;
            } else if (byte == '\\') {
                if (*at == '\0') return 0;
                if (used + 2 > capacity) return 0;
                storage[used++] = (char)*at++;
                in_token = 1;
            } else {
                if (used + 2 > capacity) return 0;
                storage[used++] = (char)byte;
                in_token = 1;
            }
            continue;
        }
        if (byte == '\'') {
            if (!in_token) {
                if (count >= TOKEN_LIMIT) return 0;
                tokens[count].text = storage + used;
                tokens[count].boundary = 0;
                in_token = 1;
            }
            quote = SH_SINGLE;
        } else if (byte == '"') {
            if (!in_token) {
                if (count >= TOKEN_LIMIT) return 0;
                tokens[count].text = storage + used;
                tokens[count].boundary = 0;
                in_token = 1;
            }
            quote = SH_DOUBLE;
        } else if (byte == '\\') {
            if (*at == '\0') return 0;
            if (!in_token) {
                if (count >= TOKEN_LIMIT) return 0;
                tokens[count].text = storage + used;
                tokens[count].boundary = 0;
                in_token = 1;
            }
            if (used + 2 > capacity) return 0;
            storage[used++] = (char)*at++;
        } else if (isspace(byte) || strchr(";&|()", byte) != NULL) {
            if (in_token) {
                if (used + 1 > capacity) return 0;
                storage[used++] = '\0';
                count++;
                in_token = 0;
            }
            if (strchr(";&|()", byte) != NULL || byte == '\n' || byte == '\r') {
                if (count >= TOKEN_LIMIT) return 0;
                tokens[count].text = NULL;
                tokens[count].boundary = 1;
                count++;
            }
        } else {
            if (!in_token) {
                if (count >= TOKEN_LIMIT) return 0;
                tokens[count].text = storage + used;
                tokens[count].boundary = 0;
                in_token = 1;
            }
            if (used + 2 > capacity) return 0;
            storage[used++] = (char)byte;
        }
    }
    if (quote != SH_NORMAL) return 0;
    if (in_token) {
        if (used + 1 > capacity) return 0;
        storage[used++] = '\0';
        count++;
    }
    *token_count = count;
    return 1;
}

static const char *base_name(const char *value) {
    const char *slash = strrchr(value, '/');
    const char *backslash = strrchr(value, '\\');
    if (backslash != NULL && (slash == NULL || backslash > slash)) slash = backslash;
    return slash == NULL ? value : slash + 1;
}

static int equal_ci(const char *left, const char *right) {
    while (*left && *right) {
        if (tolower((unsigned char)*left++) != tolower((unsigned char)*right++)) return 0;
    }
    return *left == '\0' && *right == '\0';
}

static int assignment_token(const char *value) {
    const unsigned char *at = (const unsigned char *)value;
    if (!(isalpha(*at) || *at == '_')) return 0;
    at++;
    while (isalnum(*at) || *at == '_') at++;
    return *at == '=';
}

static int has_argument(ShellToken *tokens, size_t begin, size_t end, const char *wanted) {
    size_t index;
    for (index = begin; index < end; index++) {
        if (!tokens[index].boundary && tokens[index].text[0] != '-' &&
            equal_ci(tokens[index].text, wanted)) return 1;
    }
    return 0;
}

static int has_any_argument(ShellToken *tokens, size_t begin, size_t end,
                            const char *wanted) {
    size_t index;
    for (index = begin; index < end; index++) {
        if (!tokens[index].boundary && equal_ci(tokens[index].text, wanted)) return 1;
    }
    return 0;
}

static MatrixState classify_shell(const char *command, unsigned depth);

static MatrixState classify_segment(ShellToken *tokens, size_t begin, size_t end,
                                    unsigned depth) {
    size_t index = begin;
    const char *executable;
    while (index < end && assignment_token(tokens[index].text)) index++;
    while (index < end) {
        executable = base_name(tokens[index].text);
        if (equal_ci(executable, "sudo")) {
            index++;
            while (index < end && tokens[index].text[0] == '-') {
                const char *option = tokens[index++].text;
                if ((equal_ci(option, "-C") || equal_ci(option, "-D") ||
                     equal_ci(option, "-g") || equal_ci(option, "-h") ||
                     equal_ci(option, "-p") || equal_ci(option, "-R") ||
                     equal_ci(option, "-T") || equal_ci(option, "-u")) && index < end) index++;
            }
            continue;
        }
        if (equal_ci(executable, "env")) {
            index++;
            while (index < end && (tokens[index].text[0] == '-' ||
                                   assignment_token(tokens[index].text))) index++;
            continue;
        }
        if (equal_ci(executable, "command") || equal_ci(executable, "exec") ||
            equal_ci(executable, "nohup") || equal_ci(executable, "time")) {
            index++;
            while (index < end && tokens[index].text[0] == '-') index++;
            continue;
        }
        break;
    }
    if (index >= end) return ST_COMMAND;
    executable = base_name(tokens[index].text);

    if (depth < 1 && (equal_ci(executable, "bash") || equal_ci(executable, "dash") ||
                      equal_ci(executable, "fish") || equal_ci(executable, "ksh") ||
                      equal_ci(executable, "sh") || equal_ci(executable, "zsh"))) {
        size_t at;
        for (at = index + 1; at + 1 < end; at++) {
            if (equal_ci(tokens[at].text, "-c") || equal_ci(tokens[at].text, "-lc")) {
                return classify_shell(tokens[at + 1].text, depth + 1);
            }
        }
    }

    if ((equal_ci(executable, "arduino-cli") &&
         has_argument(tokens, index + 1, end, "upload")) ||
        equal_ci(executable, "remoteocd") || equal_ci(executable, "openocd") ||
        equal_ci(executable, "dfu-util") || equal_ci(executable, "bossac") ||
        (equal_ci(executable, "west") && has_argument(tokens, index + 1, end, "flash"))) {
        return ST_FLASHING;
    }
    if (equal_ci(executable, "pytest") || equal_ci(executable, "pytest-3") ||
        equal_ci(executable, "ctest") || equal_ci(executable, "jest") ||
        equal_ci(executable, "vitest")) return ST_TESTING;
    if ((strncasecmp(executable, "python", 6) == 0 || strncasecmp(executable, "pypy", 4) == 0)) {
        size_t at;
        for (at = index + 1; at + 1 < end; at++) {
            if (equal_ci(tokens[at].text, "-m") && equal_ci(tokens[at + 1].text, "pytest")) {
                return ST_TESTING;
            }
        }
    }
    if ((equal_ci(executable, "npm") || equal_ci(executable, "pnpm") ||
         equal_ci(executable, "yarn")) && index + 1 < end) {
        if (equal_ci(tokens[index + 1].text, "test") ||
            (index + 2 < end && equal_ci(tokens[index + 1].text, "run") &&
             equal_ci(tokens[index + 2].text, "test"))) return ST_TESTING;
    }
    if ((equal_ci(executable, "cargo") || equal_ci(executable, "go") ||
         equal_ci(executable, "meson")) && index + 1 < end &&
        equal_ci(tokens[index + 1].text, "test")) return ST_TESTING;
    if (equal_ci(executable, "make") && has_argument(tokens, index + 1, end, "test")) {
        return ST_TESTING;
    }
    if ((equal_ci(executable, "npx") || equal_ci(executable, "pnpx")) &&
        index + 1 < end && (equal_ci(base_name(tokens[index + 1].text), "jest") ||
                            equal_ci(base_name(tokens[index + 1].text), "vitest"))) {
        return ST_TESTING;
    }

    if ((equal_ci(executable, "arduino-cli") &&
         has_argument(tokens, index + 1, end, "compile")) ||
        (equal_ci(executable, "cmake") && has_any_argument(tokens, index + 1, end, "--build")) ||
        equal_ci(executable, "ninja") || equal_ci(executable, "make") ||
        (equal_ci(executable, "cargo") && index + 1 < end &&
         equal_ci(tokens[index + 1].text, "build")) ||
        ((equal_ci(executable, "npm") || equal_ci(executable, "pnpm") ||
          equal_ci(executable, "yarn")) && index + 2 < end &&
         equal_ci(tokens[index + 1].text, "run") &&
         equal_ci(tokens[index + 2].text, "build")) ||
        (equal_ci(executable, "vite") && index + 1 < end &&
         equal_ci(tokens[index + 1].text, "build")) ||
        ((equal_ci(executable, "npx") || equal_ci(executable, "pnpx")) &&
         index + 2 < end && equal_ci(tokens[index + 1].text, "vite") &&
         equal_ci(tokens[index + 2].text, "build"))) return ST_BUILDING;
    return ST_COMMAND;
}

static MatrixState classify_shell(const char *command, unsigned depth) {
    char storage[INPUT_LIMIT + 1];
    ShellToken tokens[TOKEN_LIMIT];
    size_t count = 0;
    size_t begin = 0;
    size_t index;
    MatrixState observed = ST_COMMAND;
    if (!shell_tokens(command, storage, sizeof(storage), tokens, &count)) return ST_COMMAND;
    for (index = 0; index <= count; index++) {
        if (index == count || tokens[index].boundary) {
            if (index > begin) {
                MatrixState state = classify_segment(tokens, begin, index, depth);
                if (state == ST_FLASHING) return state;
                if (state == ST_TESTING) observed = ST_TESTING;
                else if (state == ST_BUILDING && observed == ST_COMMAND) observed = ST_BUILDING;
            }
            begin = index + 1;
        }
    }
    return observed;
}

static int normalized_tool_name(const char *tool, char *output, size_t capacity) {
    size_t used = 0;
    int separator = 0;
    while (*tool) {
        unsigned char byte = (unsigned char)*tool++;
        if (isalnum(byte)) {
            if (separator && used && used + 1 < capacity) output[used++] = '_';
            separator = 0;
            if (used + 1 >= capacity) return 0;
            output[used++] = (char)tolower(byte);
        } else {
            separator = 1;
        }
    }
    output[used] = '\0';
    return 1;
}

static int name_or_suffix(const char *name, const char *wanted) {
    size_t name_length = strlen(name);
    size_t wanted_length = strlen(wanted);
    return strcmp(name, wanted) == 0 ||
           (name_length > wanted_length && name[name_length - wanted_length - 1] == '_' &&
            strcmp(name + name_length - wanted_length, wanted) == 0);
}

static int string_array(const unsigned char *data, size_t length,
                        const JsonValue *value) {
    JsonParser parser;
    if (value->type != JV_ARRAY) return 0;
    parser.data = data;
    parser.length = length;
    parser.position = value->start + 1;
    parser.depth = 0;
    skip_ws(&parser);
    while (parser.position < value->end && data[parser.position] != ']') {
        JsonValue item;
        if (!parse_value(&parser, &item) || item.type != JV_STRING) return 0;
        skip_ws(&parser);
        if (parser.position < value->end && data[parser.position] == ',') {
            parser.position++;
            skip_ws(&parser);
        } else break;
    }
    return parser.position < value->end && data[parser.position] == ']';
}

static MatrixState classify_tool(const unsigned char *data, size_t length,
                                 const JsonValue *tool_value, const char *tool,
                                 const JsonValue *input_value, int input_present) {
    char name[160];
    char command[INPUT_LIMIT + 1];
    size_t command_length = 0;
    if (!normalized_tool_name(tool, name, sizeof(name))) return ST_COMMAND;
    if (name_or_suffix(name, "bash") || name_or_suffix(name, "shell") ||
        name_or_suffix(name, "exec_command") || name_or_suffix(name, "run_command")) {
        JsonValue value;
        int present = input_present;
        if (!present) return ST_COMMAND;
        value = *input_value;
        if (value.type == JV_OBJECT) {
            static const char *const keys[] = {"command", "cmd", "script"};
            JsonValue object = value;
            size_t key_index;
            present = 0;
            for (key_index = 0; key_index < 3; key_index++) {
                JsonValue candidate;
                if (!object_find_key(data, length, &object, keys[key_index], &candidate)) continue;
                if (candidate.type == JV_STRING || string_array(data, length, &candidate)) {
                    value = candidate;
                    present = 1;
                    break;
                }
            }
        }
        if (!present) return ST_COMMAND;
        if (value.type == JV_STRING) {
            if (!decode_string(data, &value, command, sizeof(command), &command_length)) {
                return ST_COMMAND;
            }
        } else if (value.type == JV_ARRAY) {
            JsonParser parser = {data, length, value.start + 1, 0};
            skip_ws(&parser);
            command_length = 0;
            command[0] = '\0';
            while (parser.position < value.end && data[parser.position] != ']') {
                JsonValue item;
                size_t item_length;
                if (!parse_value(&parser, &item) || item.type != JV_STRING ||
                    !decode_string(data, &item, command + command_length,
                                   sizeof(command) - command_length, &item_length)) return ST_COMMAND;
                command_length += item_length;
                if (command_length + 2 >= sizeof(command)) return ST_COMMAND;
                command[command_length++] = '\n';
                command[command_length] = '\0';
                skip_ws(&parser);
                if (parser.position < value.end && data[parser.position] == ',') {
                    parser.position++;
                    skip_ws(&parser);
                } else break;
            }
        } else {
            return ST_COMMAND;
        }
        return classify_shell(command, 0);
    }
    if (name_or_suffix(name, "apply_patch") || name_or_suffix(name, "edit") ||
        name_or_suffix(name, "multiedit") || name_or_suffix(name, "notebookedit") ||
        name_or_suffix(name, "write") || name_or_suffix(name, "write_file") ||
        name_or_suffix(name, "create_file")) return ST_WRITING;
    if (name_or_suffix(name, "read") || name_or_suffix(name, "read_file") ||
        name_or_suffix(name, "grep") || name_or_suffix(name, "glob") ||
        name_or_suffix(name, "search") || name_or_suffix(name, "find") ||
        name_or_suffix(name, "list_directory") || name_or_suffix(name, "list_files") ||
        name_or_suffix(name, "search_query")) return ST_READING;
    if (name_or_suffix(name, "agent") || name_or_suffix(name, "spawn_agent") ||
        name_or_suffix(name, "subagent_start")) return ST_SUBAGENT;
    (void)tool_value;
    return ST_COMMAND;
}

static int json_number_nonzero(const unsigned char *data, const JsonValue *value) {
    char number[96];
    char *end;
    long parsed;
    size_t length;
    if (value->type == JV_NUMBER) {
        length = value->end - value->start;
        if (length >= sizeof(number)) return 0;
        memcpy(number, data + value->start, length);
        number[length] = '\0';
    } else if (value->type == JV_STRING) {
        if (!decode_string(data, value, number, sizeof(number), &length)) return 0;
    } else return 0;
    errno = 0;
    parsed = strtol(number, &end, 10);
    while (*end && isspace((unsigned char)*end)) end++;
    return errno == 0 && end != number && *end == '\0' && parsed != 0;
}

static int meaningful_value(const unsigned char *data, const JsonValue *value) {
    size_t at;
    if (value->type == JV_NULL || value->type == JV_FALSE) return 0;
    if (value->type == JV_STRING) {
        return value->content_end > value->content_start;
    }
    if (value->type == JV_ARRAY || value->type == JV_OBJECT) {
        at = value->start + 1;
        while (at + 1 < value->end && isspace(data[at])) at++;
        return at + 1 < value->end;
    }
    if (value->type == JV_NUMBER) {
        char number[96];
        size_t size = value->end - value->start;
        if (size >= sizeof(number)) return 1;
        memcpy(number, data + value->start, size);
        number[size] = '\0';
        return strtod(number, NULL) != 0.0;
    }
    return 1;
}

static int explicit_failure(const unsigned char *data, size_t length,
                            const JsonValue *object, unsigned depth) {
    static const char *const exit_keys[] = {"exit_code", "exitCode", "return_code", "returnCode"};
    static const char *const error_keys[] = {"error", "tool_error", "toolError"};
    JsonValue value;
    size_t index;
    char text[32];
    size_t text_length;
    if (object->type != JV_OBJECT || depth > 8) return 0;
    for (index = 0; index < 4; index++) {
        if (object_find_key(data, length, object, exit_keys[index], &value) &&
            json_number_nonzero(data, &value)) return 1;
    }
    if (object_find_key(data, length, object, "status", &value) && value.type == JV_STRING &&
        decode_string(data, &value, text, sizeof(text), &text_length)) {
        char *begin = text;
        char *finish = text + text_length;
        while (begin < finish && isspace((unsigned char)*begin)) begin++;
        while (finish > begin && isspace((unsigned char)finish[-1])) finish--;
        *finish = '\0';
        if (equal_ci(begin, "failed") || equal_ci(begin, "failure") ||
            equal_ci(begin, "error")) return 1;
    }
    if (object_find_key(data, length, object, "success", &value) && value.type == JV_FALSE) return 1;
    if ((object_find_key(data, length, object, "is_error", &value) ||
         object_find_key(data, length, object, "isError", &value)) && value.type == JV_TRUE) return 1;
    for (index = 0; index < 3; index++) {
        if (object_find_key(data, length, object, error_keys[index], &value) &&
            meaningful_value(data, &value)) return 1;
    }
    return object_find_key(data, length, object, "metadata", &value) &&
           value.type == JV_OBJECT && explicit_failure(data, length, &value, depth + 1);
}

typedef struct {
    char data[WIRE_LIMIT + 1];
    size_t used;
    int ok;
} WireBuffer;

static void wire_bytes(WireBuffer *wire, const void *data, size_t length) {
    if (!wire->ok || wire->used + length > WIRE_LIMIT) {
        wire->ok = 0;
        return;
    }
    memcpy(wire->data + wire->used, data, length);
    wire->used += length;
}

static void wire_text(WireBuffer *wire, const char *text) {
    wire_bytes(wire, text, strlen(text));
}

static void wire_json_string(WireBuffer *wire, const unsigned char *input,
                             const JsonValue *value, int present) {
    if (!present || value->type == JV_NULL) wire_text(wire, "\"\"");
    else wire_bytes(wire, input + value->start, value->end - value->start);
}

static void send_datagram(const char *path, const char *data, size_t length) {
    int descriptor;
    struct sockaddr_un address;
    size_t path_length;
    if (path == NULL || path[0] != '/') path = DEFAULT_SOCKET;
    path_length = strlen(path);
    if (path_length == 0 || path_length >= sizeof(address.sun_path)) return;
    descriptor = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (descriptor < 0) return;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, path, path_length + 1);
    (void)sendto(descriptor, data, length, MSG_DONTWAIT,
                 (const struct sockaddr *)&address, sizeof(address));
    close(descriptor);
}

static int best_effort_oversized_stop(const unsigned char *data, size_t length) {
    static const char *const needles[] = {
        "\"hook_event_name\"", "\"hookEventName\"", "\"event_name\"", "\"event\""
    };
    size_t key_index;
    for (key_index = 0; key_index < 4; key_index++) {
        const char *needle = needles[key_index];
        size_t needle_length = strlen(needle);
        size_t at;
        for (at = 0; at + needle_length < length; at++) {
            size_t value_at;
            if (memcmp(data + at, needle, needle_length) != 0) continue;
            value_at = at + needle_length;
            while (value_at < length && isspace(data[value_at])) value_at++;
            if (value_at >= length || data[value_at++] != ':') continue;
            while (value_at < length && isspace(data[value_at])) value_at++;
            if (value_at + 6 <= length && memcmp(data + value_at, "\"Stop\"", 6) == 0) return 1;
            if (value_at + 14 <= length &&
                memcmp(data + value_at, "\"SubagentStop\"", 14) == 0) return 1;
        }
    }
    return 0;
}

int main(void) {
    static unsigned char input[INPUT_LIMIT + 2];
    size_t length = 0;
    int oversized = 0;
    JsonValue root, event_value, session_value, turn_value, use_value, tool_value, input_value;
    JsonValue response_value;
    int event_present, session_present, turn_present, use_present, tool_present, input_present;
    char event_text[96], session_text[1025], turn_text[1025], use_text[1025], tool_text[513];
    const EventMap *event;
    MatrixState state;
    int failed = 0;
    struct timespec now;
    uint64_t time_ns;
    WireBuffer wire;
    char time_text[32];
    ssize_t received;
    static const char *const event_keys[] = {"hook_event_name", "event_name", "hookEventName", "event"};
    static const char *const session_keys[] = {"session_id", "sessionId", "conversation_id", "thread_id", "session"};
    static const char *const turn_keys[] = {"turn_id", "turnId", "turn"};
    static const char *const use_keys[] = {"tool_use_id", "toolUseId", "item_id", "tool_use"};
    static const char *const tool_keys[] = {"tool_name", "toolName", "tool"};
    static const char *const input_keys[] = {"tool_input", "toolInput", "input"};
    static const char *const response_keys[] = {"tool_response", "toolResponse", "tool_result", "response"};
    struct rlimit core_limit = {0, 0};

    /* Raw lifecycle input must never be captured in a process core dump. */
    (void)setrlimit(RLIMIT_CORE, &core_limit);
    /* A closed Codex capture pipe must remain a fail-open exit-zero path. */
    (void)signal(SIGPIPE, SIG_IGN);

    for (;;) {
        if (length > INPUT_LIMIT) {
            oversized = 1;
            break;
        }
        received = read(STDIN_FILENO, input + length, INPUT_LIMIT + 1 - length);
        if (received == 0) break;
        if (received < 0) {
            if (errno == EINTR) continue;
            return 0;
        }
        length += (size_t)received;
    }
    if (length > INPUT_LIMIT) oversized = 1;
    if (oversized) {
        if (best_effort_oversized_stop(input, length)) (void)write(STDOUT_FILENO, "{}", 2);
        return 0;
    }
    if (!parse_document(input, length, &root) || root.type != JV_OBJECT) return 0;

    event_present = object_find_alias(input, length, &root, event_keys, 4, &event_value);
    if (!text_field(input, &event_value, event_present, 1, 95,
                    event_text, sizeof(event_text))) return 0;
    event = map_event(event_text);
    if (event == NULL) return 0;
    if (event->stop_reply) (void)write(STDOUT_FILENO, "{}", 2);

    session_present = object_find_alias(input, length, &root, session_keys, 5, &session_value);
    turn_present = object_find_alias(input, length, &root, turn_keys, 3, &turn_value);
    use_present = object_find_alias(input, length, &root, use_keys, 4, &use_value);
    tool_present = object_find_alias(input, length, &root, tool_keys, 3, &tool_value);
    if (!text_field(input, &session_value, session_present, 1, 256,
                    session_text, sizeof(session_text)) ||
        !text_field(input, &turn_value, turn_present, 0, 256,
                    turn_text, sizeof(turn_text)) ||
        !text_field(input, &use_value, use_present, 0, 256,
                    use_text, sizeof(use_text)) ||
        !text_field(input, &tool_value, tool_present, 0, 128,
                    tool_text, sizeof(tool_text))) return 0;

    state = event->state;
    if (strcmp(event->wire, "pre_tool") == 0) {
        input_present = object_find_alias(input, length, &root, input_keys, 3, &input_value);
        state = classify_tool(input, length, &tool_value, tool_text,
                              &input_value, input_present);
    } else if (strcmp(event->wire, "post_tool") == 0) {
        int response_present = object_find_alias(input, length, &root, response_keys, 4,
                                                 &response_value);
        failed = (response_present && explicit_failure(input, length, &response_value, 0)) ||
                 explicit_failure(input, length, &root, 0);
        state = failed ? ST_ERROR : ST_THINKING;
    }

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    time_ns = (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
    snprintf(time_text, sizeof(time_text), "%llu", (unsigned long long)time_ns);

    wire.used = 0;
    wire.ok = 1;
    wire_text(&wire, "{\"v\":1,\"event\":\"");
    wire_text(&wire, event->wire);
    wire_text(&wire, "\",\"session\":");
    wire_json_string(&wire, input, &session_value, session_present);
    wire_text(&wire, ",\"turn\":");
    wire_json_string(&wire, input, &turn_value, turn_present);
    wire_text(&wire, ",\"tool_use\":");
    wire_json_string(&wire, input, &use_value, use_present);
    wire_text(&wire, ",\"tool\":");
    if (strcmp(event->wire, "pre_tool") == 0 || strcmp(event->wire, "post_tool") == 0) {
        wire_json_string(&wire, input, &tool_value, tool_present);
    } else wire_text(&wire, "\"\"");
    wire_text(&wire, ",\"state\":\"");
    wire_text(&wire, STATE_NAMES[state]);
    wire_text(&wire, "\",\"failed\":");
    wire_text(&wire, failed ? "true" : "false");
    wire_text(&wire, ",\"time_ns\":");
    wire_text(&wire, time_text);
    wire_text(&wire, "}");
    if (wire.ok) send_datagram(getenv("UNOQ_CODEX_MATRIX_EVENT_SOCKET"), wire.data, wire.used);
    return 0;
}
