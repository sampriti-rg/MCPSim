#include "pin.H"
#include <iostream>
#include <fstream>
#include <iomanip>
#include <cstring>
#include <sstream>
#include <cstdint>
#include <unistd.h>
#include <sys/file.h>
#include <string>
#include <fcntl.h>
#include <map>
#include <stack>
#include <set>
#include <pthread.h>
#include "trace_format.h"

using trace = trace_format;
static UINT64 totalInst = 0;
UINT64 totalROI = 0;
bool gotROI = false;
std::ofstream traceFile;
PIN_LOCK pinLock;
std::map<THREADID, std::stack<double>> threadRegionID;
std::set<THREADID> activeThreads;

/* ===================================================================== */
// Command line switches
/* ===================================================================== */
KNOB<std::string> KnobOutputFile(KNOB_MODE_WRITEONCE, "pintool", "o", "mcpsim.trace", "specify file name for Champsim tracer output");

KNOB<UINT64> KnobSkipInstructions(KNOB_MODE_WRITEONCE, "pintool", "s", "0", "How many instructions to skip before tracing begins");

KNOB<UINT64> KnobTraceInstructions(KNOB_MODE_WRITEONCE, "pintool", "e", "10000000", "How many instructions to trace");

KNOB<UINT64> KnobProcessID(KNOB_MODE_WRITEONCE, "pintool", "p", "1", "What will be the process id");
/* ===================================================================== */

void OnThreadStart(THREADID tid, CONTEXT* ctxt, INT32 flags, VOID* v) {
    activeThreads.insert(tid);
    std::cerr << "Thread " << tid << " started." << std::endl;
}

void OnThreadExit(THREADID tid, const CONTEXT* ctxt, INT32 flags, VOID* v) {
    activeThreads.erase(tid);
    std::cerr << "Thread " << tid << " exited." << std::endl;
}

VOID StoreInstructionInfo(THREADID threadID, ADDRINT ip, const char* opcode, UINT64 tag, UINT64 regionID,
                       UINT64* readAddrs, UINT64* writeAddrs) {
    PIN_GetLock(&pinLock, threadID);
    trace trace_data;

    if (tag == 1031) {
        ++totalROI;
        // trace_data.processID = PIN_GetPid()%4 + 1;
        trace_data.processID = KnobProcessID.Value();
        trace_data.threadID = threadID;
        trace_data.instPointer = 0;
        strncpy(trace_data.opcode, "ROI_BEGIN", sizeof(trace_data.opcode) - 1);
        trace_data.opcode[sizeof(trace_data.opcode) - 1] = '\0';    
        
        memset(trace_data.sourceAddr, 0, sizeof(trace_data.sourceAddr));
        memset(trace_data.destAddr, 0, sizeof(trace_data.destAddr));
        for (int i = 0; i < NUM_INSTR_SOURCES; i++) {
            trace_data.sourceAddr[i] = 0;
        }
        for (int i = 0; i < NUM_INSTR_DESTINATIONS; i++) {
            trace_data.destAddr[i] = 0;
        }

        trace_data.regionID = regionID;
        threadRegionID[threadID].push(regionID);
    }
    else if (tag == 1032) {
        trace_data.processID = KnobProcessID.Value();
        trace_data.threadID = threadID;
        trace_data.instPointer = 0;
        strncpy(trace_data.opcode, "ROI_END", sizeof(trace_data.opcode) - 1);
        trace_data.opcode[sizeof(trace_data.opcode) - 1] = '\0';    
        
        memset(trace_data.sourceAddr, 0, sizeof(trace_data.sourceAddr));
        memset(trace_data.destAddr, 0, sizeof(trace_data.destAddr));
        for (int i = 0; i < NUM_INSTR_SOURCES; i++) {
            trace_data.sourceAddr[i] = 0;
        }
        for (int i = 0; i < NUM_INSTR_DESTINATIONS; i++) {
            trace_data.destAddr[i] = 0;
        }

        trace_data.regionID = regionID;
        if (threadRegionID[threadID].top() == regionID)
            threadRegionID[threadID].pop();
    }
    else {
        trace_data.processID = KnobProcessID.Value();
        trace_data.threadID = threadID;
        trace_data.instPointer = ip;
        strncpy(trace_data.opcode, opcode, sizeof(trace_data.opcode) - 1);
        trace_data.opcode[sizeof(trace_data.opcode) - 1] = '\0';

        memset(trace_data.sourceAddr, 0, sizeof(trace_data.sourceAddr));
        memset(trace_data.destAddr, 0, sizeof(trace_data.destAddr));

        for (int i = 0; i < NUM_INSTR_SOURCES; i++) {
            trace_data.sourceAddr[i] = readAddrs[i];
        }

        for (int i = 0; i < NUM_INSTR_DESTINATIONS; i++) {
            trace_data.destAddr[i] = writeAddrs[i];
        }
        trace_data.regionID = threadRegionID[threadID].top();
    }

    typename decltype(traceFile)::char_type buf[sizeof(trace)];
    std::memcpy(buf, &trace_data, sizeof(trace));
    traceFile.write(buf, sizeof(trace));

    // If you wnat to print the traces
    // std::cout << trace_data.processID << " " << trace_data.threadID << " " << trace_data.instPointer << " " << trace_data.opcode << " " ;
    // for (std::size_t i = 0; i < NUM_INSTR_SOURCES; ++i) {
    //     if (trace_data.sourceAddr[i] != 0) std::cout << trace_data.sourceAddr[i] << " ";
    // }
    // for (std::size_t i = 0; i < NUM_INSTR_DESTINATIONS; ++i) {
    //     if (trace_data.destAddr[i] != 0) std::cout << trace_data.destAddr[i] << " ";
    // }
    // std::cout << trace_data.regionID << std::endl;

    PIN_ReleaseLock(&pinLock);
}

BOOL ShouldWrite() {
    if (gotROI) {
        ++totalInst;
        if ((totalInst > KnobSkipInstructions.Value()) && (totalInst <= (KnobSkipInstructions.Value() + KnobTraceInstructions.Value())))
            return true;
        else 
            return false;
    } 
    else
        return false;
}

VOID Instruction(INS ins, VOID* v) {

    if (INS_Mnemonic(ins) == "XCHG" && INS_OperandReg(ins, 0) == REG_ECX && INS_OperandReg(ins, 1) == REG_ECX) {
        std::cerr << "ROI Begin in PIN Extraction.\n";
        gotROI = true;
    }

    // UINT64 tag = 0;
    // UINT64 regionID = 0;
    uint32_t memOperands = INS_MemoryOperandCount(ins);
    if (memOperands == 0) {
        if (INS_Mnemonic(ins) == "XCHG" && INS_OperandReg(ins, 0) == REG_RCX && INS_OperandReg(ins, 1) == REG_RCX) {
            INS_InsertIfCall(ins, IPOINT_BEFORE, (AFUNPTR)ShouldWrite, IARG_END);
            INS_InsertThenCall(ins, IPOINT_BEFORE, (AFUNPTR)+[](
                THREADID threadID, ADDRINT ip, const char* opcode, UINT64 tag, UINT64 regionID,
                ADDRINT src0, ADDRINT src1, ADDRINT src2, ADDRINT src3,
                ADDRINT dst0, ADDRINT dst1, ADDRINT dst2, ADDRINT dst3) {
                UINT64 readAddrs[NUM_INSTR_SOURCES] = {0};
                UINT64 writeAddrs[NUM_INSTR_DESTINATIONS] = {0};
                StoreInstructionInfo(threadID, ip, opcode, tag, regionID, readAddrs, writeAddrs);
            },
            IARG_THREAD_ID,
            IARG_INST_PTR,
            IARG_PTR, strdup(INS_Mnemonic(ins).c_str()),
            IARG_REG_VALUE, REG_ECX, // First parameter (op)
            IARG_REG_VALUE, REG_EDX, // Second parameter (id)
            IARG_ADDRINT, 0, IARG_ADDRINT, 0, IARG_ADDRINT, 0, IARG_ADDRINT, 0,
            IARG_ADDRINT, 0, IARG_ADDRINT, 0, IARG_ADDRINT, 0, IARG_ADDRINT, 0,
            IARG_END);
        }
        else {
            INS_InsertIfCall(ins, IPOINT_BEFORE, (AFUNPTR)ShouldWrite, IARG_END);
            INS_InsertThenCall(ins, IPOINT_BEFORE, (AFUNPTR)+[](
                THREADID threadID, ADDRINT ip, const char* opcode, UINT64 tag, UINT64 regionID,
                ADDRINT src0, ADDRINT src1, ADDRINT src2, ADDRINT src3,
                ADDRINT dst0, ADDRINT dst1, ADDRINT dst2, ADDRINT dst3) {
                UINT64 readAddrs[NUM_INSTR_SOURCES] = {0};
                UINT64 writeAddrs[NUM_INSTR_DESTINATIONS] = {0};
                StoreInstructionInfo(threadID, ip, opcode, tag, regionID, readAddrs, writeAddrs);
            },
            IARG_THREAD_ID,
            IARG_INST_PTR,
            IARG_PTR, strdup(INS_Mnemonic(ins).c_str()),
            IARG_ADDRINT, 0, IARG_ADDRINT, 0,
            IARG_ADDRINT, 0, IARG_ADDRINT, 0, IARG_ADDRINT, 0, IARG_ADDRINT, 0,
            IARG_ADDRINT, 0, IARG_ADDRINT, 0, IARG_ADDRINT, 0, IARG_ADDRINT, 0,
            IARG_END);
        }
    } 
    else {
        INS_InsertIfCall(ins, IPOINT_BEFORE, (AFUNPTR)ShouldWrite, IARG_END);
        INS_InsertThenCall(ins, IPOINT_BEFORE, (AFUNPTR)+[](
            THREADID threadID, ADDRINT ip, const char* opcode, UINT64 tag, UINT64 regionID,
            ADDRINT src0, ADDRINT src1, ADDRINT src2, ADDRINT src3,
            ADDRINT dst0, ADDRINT dst1, ADDRINT dst2, ADDRINT dst3) {
            UINT64 readAddrs[NUM_INSTR_SOURCES] = {src0, src1, src2, src3};
            UINT64 writeAddrs[NUM_INSTR_DESTINATIONS] = {dst0, dst1, dst2, dst3};
            StoreInstructionInfo(threadID, ip, opcode, tag, regionID, readAddrs, writeAddrs);
        },
        IARG_THREAD_ID,
        IARG_INST_PTR,
        IARG_PTR, strdup(INS_Mnemonic(ins).c_str()),
        IARG_ADDRINT, 0, IARG_ADDRINT, 0,
        (INS_MemoryOperandIsRead(ins, 0) ? IARG_MEMORYOP_EA : IARG_ADDRINT), 0,
        (INS_MemoryOperandCount(ins) > 1 && INS_MemoryOperandIsRead(ins, 1) ? IARG_MEMORYOP_EA : IARG_ADDRINT), 0,
        IARG_ADDRINT, 0, // placeholder if needed
        IARG_ADDRINT, 0, // placeholder if needed
        (INS_MemoryOperandIsWritten(ins, 0) ? IARG_MEMORYOP_EA : IARG_ADDRINT), 0,
        (INS_MemoryOperandCount(ins) > 1 && INS_MemoryOperandIsWritten(ins, 1) ? IARG_MEMORYOP_EA : IARG_ADDRINT), 0,
        IARG_ADDRINT, 0, // placeholder if needed
        IARG_ADDRINT, 0, // placeholder if needed
        IARG_END);
    }
}

VOID Fini(INT32 code, VOID* v) {
    traceFile.close();
    std::cerr << "Total " << totalInst << " instruction Exist.\n";
    std::cerr << "Total " << totalROI << " ROI Exist.\n";
    std::cerr << "Limit Given " << KnobTraceInstructions.Value() << " instruction\n";
}

int main(int argc, char* argv[]) {
    if (PIN_Init(argc, argv)) {
        std::cerr << "This tool generates memory traces of the specified format.\n";
        return 1;
    } 

    for(int threadIndex=0; threadIndex<32; threadIndex++)
        threadRegionID[threadIndex].push(0); //threadRegionID[threadIndex] = 0;

    PIN_InitLock(&pinLock);

    traceFile.open(KnobOutputFile.Value().c_str());
    traceFile << std::setfill(' ');

    PIN_AddThreadStartFunction(OnThreadStart, nullptr);
    PIN_AddThreadFiniFunction(OnThreadExit, nullptr);

    INS_AddInstrumentFunction(Instruction, nullptr);
    PIN_AddFiniFunction(Fini, nullptr);
    PIN_StartProgram();

    return 0;
}
