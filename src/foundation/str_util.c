/*
 * str_util.c — Safe string operations and decoded text views.
 */
#include "str_util.h"
#include "arena.h" // CBMArena, cbm_arena_alloc/strdup/strndup
#include "foundation/constants.h"
#include "foundation/mem_core.h"
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <limits.h>
#ifdef CBM_ENABLE_TEST_SEAMS
#include <stdatomic.h>
static _Atomic bool text_fail_next_alloc;
void cbm_text_view_fail_next_alloc_for_tests(void) {
    atomic_store(&text_fail_next_alloc, true);
}
#endif

enum {
    TEXT_BOM_FIRST = 0xff,
    TEXT_BOM_SECOND = 0xfe,
    TEXT_HIGH_MIN = 0xd800,
    TEXT_HIGH_MAX = 0xdbff,
    TEXT_LOW_MIN = 0xdc00,
    TEXT_LOW_MAX = 0xdfff,
    TEXT_PAIR_SHIFT = 10,
    TEXT_SUPPLEMENTARY_MIN = 0x10000,
    TEXT_ASCII_LIMIT = 0x80,
    TEXT_TWO_BYTE_LIMIT = 0x800,
    TEXT_UTF8_HEAD_TWO = 0xc0,
    TEXT_UTF8_HEAD_THREE = 0xe0,
    TEXT_UTF8_HEAD_FOUR = 0xf0,
    TEXT_UTF8_PAYLOAD_BITS = 6,
    TEXT_UTF8_PAYLOAD_MASK = 0x3f,
};

static uint32_t text_utf16_unit(const unsigned char *p, bool be) {
    return be ? ((uint32_t)p[0] << CBM_SZ_8) | p[SKIP_ONE]
              : ((uint32_t)p[SKIP_ONE] << CBM_SZ_8) | p[0];
}

static bool text_utf16_next(const unsigned char *bytes, size_t length, bool be, size_t *offset,
                            uint32_t *cp) {
    *cp = text_utf16_unit(bytes + *offset, be);
    *offset += PAIR_LEN;
    if (*cp >= TEXT_HIGH_MIN && *cp <= TEXT_HIGH_MAX) {
        if (length - *offset < PAIR_LEN) {
            return false;
        }
        uint32_t low = text_utf16_unit(bytes + *offset, be);
        if (low < TEXT_LOW_MIN || low > TEXT_LOW_MAX) {
            return false;
        }
        *offset += PAIR_LEN;
        *cp = TEXT_SUPPLEMENTARY_MIN + ((*cp - TEXT_HIGH_MIN) << TEXT_PAIR_SHIFT) + low -
              TEXT_LOW_MIN;
    }
    return *cp != 0 && !(*cp >= TEXT_LOW_MIN && *cp <= TEXT_LOW_MAX);
}

static size_t text_utf8_width(uint32_t cp) {
    if (cp < TEXT_ASCII_LIMIT) {
        return SKIP_ONE;
    }
    if (cp < TEXT_TWO_BYTE_LIMIT) {
        return CBM_SZ_2;
    }
    if (cp < TEXT_SUPPLEMENTARY_MIN) {
        return CBM_SZ_3;
    }
    return CBM_SZ_4;
}

static void text_utf8_write(char *out, uint32_t cp, size_t width) {
    if (width == SKIP_ONE) {
        out[0] = (char)cp;
        return;
    }
    static const unsigned char lead[] = {0, 0, TEXT_UTF8_HEAD_TWO, TEXT_UTF8_HEAD_THREE,
                                         TEXT_UTF8_HEAD_FOUR};
    out[0] = (char)(lead[width] | (cp >> (TEXT_UTF8_PAYLOAD_BITS * (width - SKIP_ONE))));
    for (size_t j = SKIP_ONE; j < width; j++) {
        out[j] =
            (char)(TEXT_ASCII_LIMIT | ((cp >> (TEXT_UTF8_PAYLOAD_BITS * (width - SKIP_ONE - j))) &
                                       TEXT_UTF8_PAYLOAD_MASK));
    }
}

static bool text_utf16_convert(const unsigned char *bytes, size_t length, bool be, char *out,
                               size_t *out_length) {
    size_t offset = PAIR_LEN;
    size_t pos = 0;
    while (offset < length) {
        uint32_t cp = 0;
        if (!text_utf16_next(bytes, length, be, &offset, &cp)) {
            return false;
        }
        size_t width = text_utf8_width(cp);
        if (pos > (size_t)INT_MAX - width) {
            return false;
        }
        if (out) {
            text_utf8_write(out + pos, cp, width);
        }
        pos += width;
    }
    *out_length = pos;
    return true;
}

cbm_text_view_status_t cbm_text_view_init(const char *raw, size_t length, cbm_text_view_t *out) {
    if (!out) {
        return CBM_TEXT_VIEW_INVALID;
    }
    *out = (cbm_text_view_t){0};
    if ((!raw && length) || length > INT_MAX) {
        return CBM_TEXT_VIEW_INVALID;
    }
    const unsigned char *bytes = (const unsigned char *)raw;
    bool le =
        length >= PAIR_LEN && bytes[0] == TEXT_BOM_FIRST && bytes[SKIP_ONE] == TEXT_BOM_SECOND;
    bool be =
        length >= PAIR_LEN && bytes[0] == TEXT_BOM_SECOND && bytes[SKIP_ONE] == TEXT_BOM_FIRST;
    if (!le && !be) {
        if (length && memchr(raw, 0, length)) {
            return CBM_TEXT_VIEW_INVALID;
        }
        out->data = raw;
        out->length = length;
        return CBM_TEXT_VIEW_OK;
    }
    size_t n = 0;
    /* Validate and size before allocating; malformed input never produces a partial view. */
    if (length % PAIR_LEN || !text_utf16_convert(bytes, length, be, NULL, &n)) {
        return CBM_TEXT_VIEW_INVALID;
    }
#ifdef CBM_ENABLE_TEST_SEAMS
    if (atomic_exchange(&text_fail_next_alloc, false)) {
        return CBM_TEXT_VIEW_OOM;
    }
#endif
    out->owned = cbm_alloc(CBM_MEM_CLASS_EXTRACT, n + SKIP_ONE);
    if (!out->owned) {
        return CBM_TEXT_VIEW_OOM;
    }
    if (!text_utf16_convert(bytes, length, be, out->owned, &n)) {
        cbm_text_view_release(out);
        return CBM_TEXT_VIEW_INVALID;
    }
    out->owned[n] = 0;
    out->data = out->owned;
    out->length = n;
    out->encoding = be ? CBM_TEXT_UTF16_BE : CBM_TEXT_UTF16_LE;
    return CBM_TEXT_VIEW_OK;
}

void cbm_text_view_release(cbm_text_view_t *view) {
    if (view) {
        cbm_free(CBM_MEM_CLASS_EXTRACT, view->owned);
        *view = (cbm_text_view_t){0};
    }
}

enum {
    JSON_ESC_LEN = 2,       /* escaped char takes 2 bytes (backslash + char) */
    JSON_NUL_RESERVE = 1,   /* reserve 1 byte for NUL terminator */
    JSON_CTRL_LIMIT = 0x20, /* ASCII control character upper bound */
};

char *cbm_path_join(CBMArena *a, const char *base, const char *name) {
    if (!base || !name) {
        return NULL;
    }
    size_t blen = strlen(base);
    size_t nlen = strlen(name);

    /* Handle empty components */
    if (blen == 0) {
        return cbm_arena_strdup(a, name);
    }
    if (nlen == 0) {
        return cbm_arena_strdup(a, base);
    }

    /* Strip trailing slash from base */
    while (blen > 0 && base[blen - SKIP_ONE] == '/') {
        blen--;
    }
    /* Strip leading slash from name */
    while (nlen > 0 && *name == '/') {
        name++;
        nlen--;
    }

    if (blen == 0) {
        return cbm_arena_strndup(a, name, nlen);
    }
    if (nlen == 0) {
        return cbm_arena_strndup(a, base, blen);
    }

    char *result = (char *)cbm_arena_alloc(a, blen + SKIP_ONE + nlen + SKIP_ONE);
    if (!result) {
        return NULL;
    }
    memcpy(result, base, blen);
    result[blen] = '/';
    memcpy(result + blen + SKIP_ONE, name, nlen);
    result[blen + SKIP_ONE + nlen] = '\0';
    return result;
}

char *cbm_path_join_n(CBMArena *a, const char **parts, int n) {
    if (n <= 0 || !parts) {
        return cbm_arena_strdup(a, "");
    }
    if (n == SKIP_ONE) {
        return cbm_arena_strdup(a, parts[0]);
    }

    char *result = cbm_arena_strdup(a, parts[0]);
    for (int i = SKIP_ONE; i < n; i++) {
        result = cbm_path_join(a, result, parts[i]);
    }
    return result;
}

const char *cbm_path_ext(const char *path) {
    if (!path) {
        return "";
    }
    const char *dot = NULL;
    const char *slash = NULL;
    for (const char *p = path; *p; p++) {
        if (*p == '.') {
            dot = p;
        }
        if (*p == '/') {
            slash = p;
        }
    }
    /* dot must be after last slash and not at start of basename */
    if (!dot) {
        return "";
    }
    if (slash && dot < slash) {
        return "";
    }
    return dot + SKIP_ONE;
}

const char *cbm_path_base(const char *path) {
    if (!path) {
        return "";
    }
    const char *last_slash = NULL;
    for (const char *p = path; *p; p++) {
        if (*p == '/') {
            last_slash = p;
        }
    }
    return last_slash ? last_slash + SKIP_ONE : path;
}

char *cbm_path_dir(CBMArena *a, const char *path) {
    if (!path) {
        return cbm_arena_strdup(a, ".");
    }
    const char *last_slash = NULL;
    for (const char *p = path; *p; p++) {
        if (*p == '/') {
            last_slash = p;
        }
    }
    if (!last_slash) {
        return cbm_arena_strdup(a, ".");
    }
    return cbm_arena_strndup(a, path, (size_t)(last_slash - path));
}

bool cbm_str_starts_with(const char *s, const char *prefix) {
    if (!s || !prefix) {
        return false;
    }
    size_t plen = strlen(prefix);
    return strncmp(s, prefix, plen) == 0;
}

bool cbm_str_ends_with(const char *s, const char *suffix) {
    if (!s || !suffix) {
        return false;
    }
    size_t slen = strlen(s);
    size_t xlen = strlen(suffix);
    if (xlen > slen) {
        return false;
    }
    return strcmp(s + slen - xlen, suffix) == 0;
}

bool cbm_str_contains(const char *s, const char *sub) {
    if (!s || !sub) {
        return false;
    }
    if (sub[0] == '\0') {
        return true;
    }
    return strstr(s, sub) != NULL;
}

char *cbm_str_tolower(CBMArena *a, const char *s) {
    if (!s) {
        return NULL;
    }
    size_t len = strlen(s);
    char *result = (char *)cbm_arena_alloc(a, len + SKIP_ONE);
    if (!result) {
        return NULL;
    }
    for (size_t i = 0; i < len; i++) {
        result[i] = (char)tolower((unsigned char)s[i]);
    }
    result[len] = '\0';
    return result;
}

char *cbm_str_replace_char(CBMArena *a, const char *s, char from, char to) {
    if (!s) {
        return NULL;
    }
    size_t len = strlen(s);
    char *result = (char *)cbm_arena_alloc(a, len + SKIP_ONE);
    if (!result) {
        return NULL;
    }
    for (size_t i = 0; i < len; i++) {
        result[i] = (s[i] == from) ? to : s[i];
    }
    result[len] = '\0';
    return result;
}

char *cbm_str_strip_ext(CBMArena *a, const char *path) {
    if (!path) {
        return NULL;
    }
    const char *dot = NULL;
    const char *slash = NULL;
    for (const char *p = path; *p; p++) {
        if (*p == '.') {
            dot = p;
        }
        if (*p == '/') {
            slash = p;
        }
    }
    if (!dot || (slash && dot < slash)) {
        return cbm_arena_strdup(a, path);
    }
    return cbm_arena_strndup(a, path, (size_t)(dot - path));
}

char **cbm_str_split(CBMArena *a, const char *s, char delim, int *out_count) {
    if (!s || !out_count) {
        return NULL;
    }

    /* Count parts */
    int count = SKIP_ONE;
    for (const char *p = s; *p; p++) {
        if (*p == delim) {
            count++;
        }
    }

    char **result = (char **)cbm_arena_alloc(a, (size_t)(count + SKIP_ONE) * sizeof(char *));
    if (!result) {
        return NULL;
    }

    int idx = 0;
    const char *start = s;
    for (const char *p = s;; p++) {
        if (*p == delim || *p == '\0') {
            size_t part_len = (size_t)(p - start);
            result[idx++] = cbm_arena_strndup(a, start, part_len);
            if (*p == '\0') {
                break;
            }
            start = p + SKIP_ONE;
        }
    }

    result[idx] = NULL;
    *out_count = count;
    return result;
}

bool cbm_validate_shell_arg(const char *s) {
    if (!s) {
        return false;
    }
    for (const char *p = s; *p; p++) {
        switch (*p) {
        case '\'':
        case '"':
        case ';':
        case '|':
        case '&':
        case '$':
        case '`':
        case '<':
        case '>':
        case '\n':
        case '\r':
#ifndef _WIN32
        case '\\':
#endif
            return false;
        default:
            break;
        }
    }
    return true;
}

bool cbm_validate_shell_path_arg(const char *path) {
    if (!cbm_validate_shell_arg(path)) {
        return false;
    }
#ifdef _WIN32
    /* cmd.exe expands %VAR% and, with delayed expansion, !VAR! inside the
     * double quotes these commands use; ^ is its escape character. None of the
     * three can be neutralised by quoting, so reject them outright. */
    for (const char *p = path; *p; p++) {
        if (*p == '%' || *p == '!' || *p == '^') {
            return false;
        }
    }
#endif
    return true;
}

bool cbm_validate_project_name(const char *name) {
    if (!name || !*name)
        return false;
    /* Reject directory traversal */
    if (strcmp(name, "..") == 0 || strstr(name, "..") != NULL)
        return false;
    /* Reject path separators */
    if (strchr(name, '/') || strchr(name, '\\'))
        return false;
    /* Reject leading dot (hidden files / relative refs) */
    if (name[0] == '.')
        return false;
    /* Allow only alphanumeric, dash, underscore, dot */
    for (const char *p = name; *p; p++) {
        if (!(((*p >= 'a') && (*p <= 'z')) || ((*p >= 'A') && (*p <= 'Z')) ||
              ((*p >= '0') && (*p <= '9')) || *p == '-' || *p == '_' || *p == '.')) {
            return false;
        }
    }
    return true;
}

bool cbm_is_internal_cache_db(const char *filename) {
    return filename && (strcmp(filename, CBM_CONFIG_DB_FILENAME) == 0 ||
                        strcmp(filename, CBM_CROSS_REPO_DB_FILENAME) == 0);
}

bool cbm_is_project_index_db(const char *filename) {
    static const char db_ext[] = ".db";
    static const char adr_ext[] = ".adr.db";
    const size_t ext_len = sizeof(db_ext) - 1;
    const size_t adr_len = sizeof(adr_ext) - 1;
    if (!filename) {
        return false;
    }
    size_t len = strlen(filename);
    if (len <= ext_len || strcmp(filename + len - ext_len, db_ext) != 0) {
        return false;
    }
    /* A per-project ADR sidecar ("<db>.adr.db") ends in ".db" but is not itself
     * an index — exclude it from every index scan/count (MCP loops + the CLI's
     * cbm_list_indexes / count_db_indexes all gate on this helper). */
    if (len >= adr_len && strcmp(filename + len - adr_len, adr_ext) == 0) {
        return false;
    }
    return !cbm_is_internal_cache_db(filename);
}

int cbm_utf8_trim_partial(char *buf) {
    if (!buf) {
        return 0;
    }
    int pos = (int)strlen(buf);
    if (pos == 0) {
        return 0;
    }
    /* Walk back over continuation bytes to the lead byte; drop the sequence
     * when fewer continuation bytes follow it than its lead byte announces. */
    int back = pos - 1;
    int cont = 0;
    while (back >= 0 && ((unsigned char)buf[back] & 0xC0) == 0x80) {
        back--;
        cont++;
    }
    if (back >= 0) {
        unsigned char lead = (unsigned char)buf[back];
        int need = lead >= 0xF0 ? 3 : lead >= 0xE0 ? 2 : lead >= 0xC0 ? 1 : 0;
        if (need > cont) {
            pos = back;
            buf[pos] = '\0';
        }
    }
    return pos;
}

int cbm_json_escape(char *buf, int bufsize, const char *src) {
    if (!buf || bufsize <= 0) {
        return 0;
    }
    if (!src) {
        buf[0] = '\0';
        return 0;
    }
    int pos = 0;
    int i = 0;
    for (; src[i] && pos < bufsize - JSON_NUL_RESERVE; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '"' || c == '\\') {
            if (pos + JSON_ESC_LEN > bufsize - JSON_NUL_RESERVE) {
                break;
            }
            buf[pos++] = '\\';
            buf[pos++] = (char)c;
        } else if (c == '\n') {
            if (pos + JSON_ESC_LEN > bufsize - JSON_NUL_RESERVE) {
                break;
            }
            buf[pos++] = '\\';
            buf[pos++] = 'n';
        } else if (c == '\r') {
            if (pos + JSON_ESC_LEN > bufsize - JSON_NUL_RESERVE) {
                break;
            }
            buf[pos++] = '\\';
            buf[pos++] = 'r';
        } else if (c == '\t') {
            if (pos + JSON_ESC_LEN > bufsize - JSON_NUL_RESERVE) {
                break;
            }
            buf[pos++] = '\\';
            buf[pos++] = 't';
        } else if (c < JSON_CTRL_LIMIT) {
            /* Other control chars: escape as \u00XX */
            if (pos + 6 > bufsize - JSON_NUL_RESERVE) {
                break;
            }
            pos += snprintf(buf + pos, 7, "\\u%04x", c);
        } else {
            buf[pos++] = (char)c;
        }
    }
    buf[pos] = '\0';
    /* A truncated output must never end inside a multibyte UTF-8 sequence: a
     * callee cut mid-character persisted bytes SQLite cannot decode as text
     * (2026-09-16 probe: nine CALLS rows across rust, java and typescript). */
    if (src[i] != '\0') {
        pos = cbm_utf8_trim_partial(buf);
    }
    return pos;
}
