#ifndef TRACE_FORMAT_H
#define TRACE_FORMAT_H

#include <limits>
#include <stdint.h>
#include <cstdint>

const int NUM_INSTR_DESTINATIONS = 4;
const int NUM_INSTR_SOURCES = 4;
const int MAX_OPCODE_LENGTH = 32;

struct trace_format
{
    uint64_t processID;
    uint64_t threadID;
    uint64_t instPointer;
    uint64_t sourceAddr[NUM_INSTR_SOURCES];
    uint64_t destAddr[NUM_INSTR_DESTINATIONS];
    uint64_t regionID;
    char opcode[MAX_OPCODE_LENGTH];    
};

#endif