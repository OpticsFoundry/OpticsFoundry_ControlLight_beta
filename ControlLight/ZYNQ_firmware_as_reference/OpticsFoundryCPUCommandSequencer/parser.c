#include "command_defs.h"
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include "label_table.h"
#include "name_table.h"
#include "registers.h"
#include "command_errors.h"

extern CommandDef command_defs[];
extern const int num_commands;

static int find_command_opcode(const char* token) {
    // Check for numeric opcode
    if (isdigit((unsigned char)token[0])) {
        int val = atoi(token);
        if (val >= 0 && val < num_commands)
            return val;
        return -1;
    }

    // Match by name
    for (int i = 0; i < num_commands; ++i) {
        if (strcmp(token, command_defs[i].name) == 0)
            return i;
    }
    return -1;
}

static const char* skip_whitespace(const char* s) {
    while (*s == ' ' || *s == '\n' || *s == '\r' || *s == '\t') ++s;
    return s;
}

static const char* parse_token(const char* s, char* out, size_t max_len) {
    s = skip_whitespace(s);
    size_t i = 0;
    while (*s && !isspace((unsigned char)*s) && *s != '(' && *s != ',' && *s != ')' && *s != ':') {
        if (i < max_len - 1) out[i++] = *s;
        ++s;
    }
    if (*s == ':') {
        if (i < max_len - 1) out[i++] = ':';  //2026-11-26: was written unconditionally (one byte overflow)
        ++s;
    }
    out[i] = '\0';
    return s;
}

static const char* parse_one_argument(const char* s, char* out, size_t max_len) {
    s = skip_whitespace(s);
    size_t i = 0;
    while (*s && *s != '(' && *s != ',' && *s != ')' && *s != ':') {
        if (i < max_len - 1) out[i++] = *s;
        ++s;
    }
    if (*s == ':') {
        if (i < max_len - 1) out[i++] = ':';  //2026-11-26: was written unconditionally (one byte overflow)
        ++s;
    }
    out[i] = '\0';
    return s;
}

static const char* parse_label(const char* s, char* out, size_t max_len) {
    s = skip_whitespace(s);
    size_t i = 0;
    while (*s && !isspace((unsigned char)*s) && *s != ':') {
        if (i < max_len - 1) out[i++] = *s;
        ++s;
    }
    out[i] = '\0';
    s++;
    return s;
}

//Constants, labels and variable names are stored in registers[MAX_USER_REGISTERS...MAX_REGISTERS-1].
//2026-11-26: const_index was a function static that was never reset and never bounds checked, so registers[]
//overflowed after about 85 cs_assemble_sequence commands. It is now reset by CS_AssembleSequence.
static int const_index = MAX_USER_REGISTERS;

void CS_ResetConstants(void) {
    const_index = MAX_USER_REGISTERS;
}

static bool constant_register_available(void) {
    if (const_index < MAX_REGISTERS) return true;
    CS_SetError(CMD_REGISTER_OVERFLOW, "Too many constants, labels and variable names");
    return false;
}

static const char* parse_argument(const char* s, uint16_t* out_index) {
    s = skip_whitespace(s);
    *out_index = 0;

    if (s[0] == '"') {
        // String literal
        s++;
        if (!constant_register_available()) return NULL;
        char* dst = registers[const_index].value.s;
        size_t len = 0;
        while (*s && *s != '"') {  //2026-11-26: was "while (s && ...)", which ran beyond the end of the string
            if (len < MAX_STRING_LENGTH - 1) {
                dst[len++] = *s++;
            }
            else {
                char buf[256];
                dst[len] = '\0'; // truncate to avoid overflow
                snprintf(buf, sizeof(buf), "String constant too long: %s", dst);
                CS_SetError(CMD_STRING_TOO_LONG, buf);
                return NULL;
            }
        }
        dst[len] = '\0';
        if (*s != '"') {
            char buf[256];
            snprintf(buf, sizeof(buf), "Missing closing quote: %s", dst);
            CS_SetError(CMD_PARSE_ERROR, buf);
            return NULL;
        }
        registers[const_index].type = REG_STRING;
        *out_index = const_index++;
        return ++s;
    }


    char arg[64];
    s = parse_one_argument(s, arg, sizeof(arg));

    if ((strncmp(arg, "R", 1) == 0) && (isdigit((unsigned char)arg[1]))) { //register
        //s++;
        //char token[32];
        //parse_token(s, token, 32);
        long index = atol(&arg[1]);
        if ((index < 0) || (index >= MAX_USER_REGISTERS)) {  //2026-11-26: checked *out_index (always 0) instead of index
            CS_SetError(CMD_REGISTER_OVERFLOW, "User register number too high");
            return NULL;
        }
        *out_index = (uint16_t)index;
        //while (isdigit(*s)) ++s;
        return s;
    }
    else if (strchr(arg, '.')) {
        // double
        if (!constant_register_available()) return NULL;
        registers[const_index].type = REG_DOUBLE;
        registers[const_index].value.d = atof(arg);
        *out_index = const_index++;
        //while (*s && (isdigit(*s) || *s == '.' || *s == '-')) ++s;
        return s;
    }
    else if (strchr(arg, ':')) {
        // label
        char label[32];
        parse_label(arg, label, sizeof(label));
        if (!constant_register_available()) return NULL;
        //reference to a label: use the existing entry (label defined before, or referenced before),
        //otherwise add it as not yet defined (line -1). 2026-11-26: the return value of add_label was ignored,
        //leaving label_index uninitialized if the label table was full.
        int label_index = find_label_index(label);
        if (label_index < 0) {
            if (!add_label(label, &label_index, -1)) {
                CS_SetError(CMD_INVALID_LABEL, "Too many labels, or label name too long");
                return NULL;
            }
        }
        registers[const_index].type = REG_INT;
        registers[const_index].value.i = label_index;
        *out_index = const_index++;
        //while (*s && (*s == ':')) ++s;
        return s;
    }
    else if ((arg[0] >= 'a' && arg[0] <= 'z') || (arg[0] >= 'A' && arg[0] <= 'Z') || (arg[0] == '_')) { //variable name
        // name
        int name_index = find_name_index(arg);
        if (name_index < 0) {
            if (!constant_register_available()) return NULL;
            if (!add_name(arg, const_index)) {
                CS_SetError(CMD_STRING_TOO_LONG, "couldn't register variable name");
                return NULL;
            }
            name_index = const_index;
            const_index++;
            registers[name_index].type = REG_INT; //type not defined at this moment, so initialize it with INT 0 as default
            registers[name_index].value.i = 0;
        }
        *out_index = name_index;
        return s;
    }
    else {
        // Int
        if (!constant_register_available()) return NULL;
        registers[const_index].type = REG_INT;
        char* endPtr;
        long long value;
        char* arg_ptr = arg;
        if (strncmp(arg, "0b", 2) == 0 || strncmp(arg, "0B", 2) == 0) {
            arg_ptr += 2;
            value = strtoll(arg_ptr, &endPtr, 2);  // skip "0b" and parse as binary
        }
        else {
            value = strtoll(arg_ptr, &endPtr, 0);  // auto-detect hex/octal/decimal
        }
        registers[const_index].value.i = value;
        *out_index = const_index++;
        //while (*s && (isdigit(*s) || *s == '-')) ++s;
        return s;
    }
}

int CS_GetCommandCount(void) { return command_count; }

CommandStatus CS_AddCommand(const char* s) {
    bool end_of_command = false;
    while (!end_of_command && *s) {

        char token[32];
        s = parse_token(s, token, sizeof(token));
        if (strchr(token, ':')) {
            // label
            token[strlen(token) - 1] = '\0';
            int label_index;
            bool ok = add_label(token, &label_index, command_count);
            if (!ok) {
                char buf[200];
                sprintf(buf, "Label already defined: %s", token);
                CS_SetError(CMD_PARSE_ERROR, buf);
                return CMD_PARSE_ERROR;
            }
        }
        else {
            int opcode = find_command_opcode(token);
            if (opcode < 0) {
                CS_SetError(CMD_INVALID_OPCODE, token);
                return CMD_INVALID_OPCODE;
            }

            const CommandDef* def = &command_defs[opcode];

            s = skip_whitespace(s);
            if (*s != '(') {
                CS_SetError(CMD_PARSE_ERROR, "Missing '('");
                return CMD_PARSE_ERROR;
            }
            ++s;

            uint16_t args[MAX_ARGUMENTS] = { 0 };
            for (int i = 0; i < def->arg_count; ++i) {
                s = parse_argument(s, &args[i]);
                if (!s) return CMD_PARSE_ERROR;
                s = skip_whitespace(s);
                if (i < def->arg_count - 1 && *s == ',') ++s;
            }

            if (*s != ')') {
                CS_SetError(CMD_PARSE_ERROR, "Missing ')'");
                return CMD_PARSE_ERROR;
            }
            ++s;
            s = skip_whitespace(s);
            if (*s == ';') ++s;

            if (command_count >= MAX_COMMANDS) {
                CS_SetError(CMD_SEQUENCE_OVERFLOW, "Too many commands");
                return CMD_SEQUENCE_OVERFLOW;
            }

            command_table[command_count].opcode = (uint8_t)opcode;
            memcpy(command_table[command_count].args, args, sizeof(args));
            //command_table[command_count].line_number = add_command_call_count++;

            ++command_count;
        }

        s = skip_whitespace(s);
        if ((*s == '/') || (*s == '\0')) { // command ends in comment, which we skip, or command string ends
            end_of_command = true;
        }
    }
    return CMD_SUCCESS;
}

