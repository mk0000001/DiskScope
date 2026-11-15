#include "export_internal.h"

static bool formula_leading(const char *text)
{
    for (const char *p = text; *p; p = g_utf8_next_char(p)) {
        gunichar c = g_utf8_get_char(p);
        if (g_unichar_iscntrl(c)) return true;
        if (!g_unichar_isspace(c)) return c == '=' || c == '+' || c == '-' || c == '@';
    }
    return false;
}

void ds_csv_field(GString *line, const char *text, bool protect_formula)
{
    g_string_append_c(line, '"');
    if (protect_formula && formula_leading(text)) g_string_append_c(line, '\'');
    for (const char *p = text; *p; ++p) {
        if (*p == '"') g_string_append_c(line, '"');
        g_string_append_c(line, *p);
    }
    g_string_append_c(line, '"');
}
