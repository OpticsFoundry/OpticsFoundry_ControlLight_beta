#include "command_defs.h"
#include <stdio.h>
//#include "label_table.h"
#include "command_defs.h"
#include "executor.h"
#include "label_table.h"
#include "opticsfoundryfpga.h"
#include "command_errors.h"
#include "registers.h"

#ifndef _MSC_VER
#include "xil_printf.h"
#endif

int next_line_number = 0;

bool Execution_jumped = false;
int jump_line_number = -1;
void CS_ExecutionJumpTo(int command_line) {
    Execution_jumped = true;
    jump_line_number = command_line;
}

volatile bool ExecutionRunning = false;  //2026-11-26: was true before any execution. 2026-11-27: volatile, as cs_interrupt_sequence clears it in the network receive callback


int CS_GetNextCommandLineNumber(void) {
    return next_line_number;
}

void CS_InterruptExecution(void) {
    ExecutionRunning = false;
}

bool CS_ExecutionIsRunning(void) {
    return ExecutionRunning;
}


void PrintCommandLine(unsigned int line_nr) {
    if (line_nr >= command_count) {
        xil_printf("Invalid line number %u\n", line_nr);
        return;
    }
    xil_printf("%u: %s(", line_nr, command_defs[command_table[line_nr].opcode].name);
    for (int j = 0; j < command_defs[command_table[line_nr].opcode].arg_count; ++j) {
        xil_printf("R%u = ", command_table[line_nr].args[j]);
        if (command_table[line_nr].args[j] >= MAX_REGISTERS) {  //2026-11-26: bounds check
            xil_printf("invalid register");
        }
        else if (registers[command_table[line_nr].args[j]].type == REG_INT) {
            PrintUnsignedLongLong(registers[command_table[line_nr].args[j]].value.i);
        }
        else if (registers[command_table[line_nr].args[j]].type == REG_DOUBLE) {
            print_double(registers[command_table[line_nr].args[j]].value.d);
        }
        else if (registers[command_table[line_nr].args[j]].type == REG_STRING) {
            xil_printf("\"%s\"", registers[command_table[line_nr].args[j]].value.s);
        }
        else {
            xil_printf("Unknown type");
        }
        if (j < command_defs[command_table[line_nr].opcode].arg_count - 1) xil_printf(", ");
    }
    xil_printf(")\n");
}

static CommandStatus ExecuteSequence(int check_ethernet_period_ms);

CommandStatus CS_ExecuteSequence(int check_ethernet_period_ms) {
    CommandStatus status = ExecuteSequence(check_ethernet_period_ms);
    ExecutionRunning = false;  //2026-11-26: was never cleared, so later parse errors were reported as execution errors
    return status;
}

static CommandStatus ExecuteSequence(int check_ethernet_period_ms) {
    const char* bad_label = NULL;
    if (!check_label_table(&bad_label)) {
        char buf[200];
        sprintf(buf, "Label not defined: %s", bad_label);
        CS_SetError(CMD_UNDEFINED_LABEL, buf);
        return CMD_UNDEFINED_LABEL;
    }
    Execution_jumped = false;
    ExecutionRunning = true;
    next_line_number = 0;
    while ((next_line_number < command_count) && (ExecutionRunning)) {
        if (CS_GetNrErrorMessages() > 0) {
			xil_printf("Execution stopped due to errors.\n");
			CS_PrintErrorMessages();
            return CMD_PARSE_ERROR;
        }
        if (command_table[next_line_number].opcode >= num_commands) {
            CS_SetError(CMD_UNDEFINED_LABEL, "Invalid opcode");
            return CMD_INVALID_OPCODE;
        }
        if (DebugModeOn) {
            xil_printf("\nExecuting line nr ");
            PrintCommandLine(next_line_number);
        }
        command_defs[command_table[next_line_number].opcode].func(command_table[next_line_number].args);
        if (Execution_jumped) {
            if ((jump_line_number < 0) || (jump_line_number >= command_count)) {  //2026-11-26: negative destinations executed memory before command_table
                CS_SetError(CMD_INVALID_LABEL, "Jump destination outside of code");
                return CMD_INVALID_LABEL;
            }
            next_line_number = jump_line_number;
            Execution_jumped = false;
        }
        else next_line_number++;
        OpticsFoundryFPGA_ServiceNetworkBetweenCommands(check_ethernet_period_ms);  //2026-11-27: a negative period never serviced the network, so the sequence couldn't be stopped
    }
    return CMD_SUCCESS;
}



void CS_PrintCommandTable(void) {
    for (int i = 0; i < command_count; ++i) {
        PrintCommandLine(i);
    }
}
