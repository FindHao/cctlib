// * BeginRiceCopyright *****************************************************
//
// Copyright ((c)) 2002-2014, Rice University
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
// * Redistributions of source code must retain the above copyright
//   notice, this list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright
//   notice, this list of conditions and the following disclaimer in the
//   documentation and/or other materials provided with the distribution.
//
// * Neither the name of Rice University (RICE) nor the names of its
//   contributors may be used to endorse or promote products derived from
//   this software without specific prior written permission.
//
// This software is provided by RICE and contributors "as is" and any
// express or implied warranties, including, but not limited to, the
// implied warranties of merchantability and fitness for a particular
// purpose are disclaimed. In no event shall RICE or contributors be
// liable for any direct, indirect, incidental, special, exemplary, or
// consequential damages (including, but not limited to, procurement of
// substitute goods or services; loss of use, data, or profits; or
// business interruption) however caused and on any theory of liability,
// whether in contract, strict liability, or tort (including negligence
// or otherwise) arising in any way out of the use of this software, even
// if advised of the possibility of such damage.
//
// ******************************************************* EndRiceCopyright *


#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <iostream>
#include <unistd.h>
#include <assert.h>
#include <string.h>
#include <sys/mman.h>
#include <sstream>
//#include <functional>
#include <unordered_set>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <list>
#include "pin.H"
#include "cctlib.H"
using namespace std;
using namespace PinCCTLib;

/* infrastructure for shadow memory */
/* MACROs */
// 64KB shadow pages
#define PAGE_OFFSET_BITS (16LL)
#define PAGE_OFFSET(addr) ( addr & 0xFFFF)
#define PAGE_OFFSET_MASK ( 0xFFFF)

#define PAGE_SIZE (1 << PAGE_OFFSET_BITS)

// 2 level page table
#define PTR_SIZE (sizeof(struct Status *))
#define LEVEL_1_PAGE_TABLE_BITS  (20)
#define LEVEL_1_PAGE_TABLE_ENTRIES  (1 << LEVEL_1_PAGE_TABLE_BITS )
#define LEVEL_1_PAGE_TABLE_SIZE  (LEVEL_1_PAGE_TABLE_ENTRIES * PTR_SIZE )

#define LEVEL_2_PAGE_TABLE_BITS  (12)
#define LEVEL_2_PAGE_TABLE_ENTRIES  (1 << LEVEL_2_PAGE_TABLE_BITS )
#define LEVEL_2_PAGE_TABLE_SIZE  (LEVEL_2_PAGE_TABLE_ENTRIES * PTR_SIZE )

#define LEVEL_1_PAGE_TABLE_SLOT(addr) (((addr) >> (LEVEL_2_PAGE_TABLE_BITS + PAGE_OFFSET_BITS)) & 0xfffff)
#define LEVEL_2_PAGE_TABLE_SLOT(addr) (((addr) >> (PAGE_OFFSET_BITS)) & 0xFFF)


// have R, W representative macros
#define READ_ACTION (0)
#define WRITE_ACTION (0xff)

#define ONE_BYTE_READ_ACTION (0)
#define TWO_BYTE_READ_ACTION (0)
#define FOUR_BYTE_READ_ACTION (0)
#define EIGHT_BYTE_READ_ACTION (0)

#define ONE_BYTE_WRITE_ACTION (0xff)
#define TWO_BYTE_WRITE_ACTION (0xffff)
#define FOUR_BYTE_WRITE_ACTION (0xffffffff)
#define EIGHT_BYTE_WRITE_ACTION (0xffffffffffffffff)



#define IS_ACCESS_WITHIN_PAGE_BOUNDARY(accessAddr, accessLen)  (PAGE_OFFSET((accessAddr)) <= (PAGE_OFFSET_MASK - (accessLen)))

/* Other footprint_client settings */
#define MAX_REDUNDANT_CONTEXTS_TO_LOG (1000)
#define THREAD_MAX (1024)

#define ENCODE_ADDRESS_AND_ACCESS_LEN(addr, len) ( (addr) | (((uint64_t)(len)) << 48))
#define DECODE_ADDRESS(addrAndLen) ( (addrAndLen) & ((1L<<48) - 1))
#define DECODE_ACCESS_LEN(addrAndLen) ( (addrAndLen) >> 48)


#define MAX_WRITE_OP_LENGTH (512)
#define MAX_WRITE_OPS_IN_INS (8)

#ifdef ENABLE_SAMPLING

#define WINDOW_ENABLE 1000000
#define WINDOW_DISABLE 1000000000
#define WINDOW_CLEAN 10
#endif

#define DECODE_DEAD(data) static_cast<ContextHandle_t>(((data)  & 0xffffffffffffffff) >> 32 )
#define DECODE_KILL(data) (static_cast<ContextHandle_t>( (data)  & 0x00000000ffffffff))


#define MAKE_CONTEXT_PAIR(a, b) (((uint64_t)(a) << 32) | ((uint64_t)(b)))


struct AddrValPair{
    void * address;
    uint8_t value[MAX_WRITE_OP_LENGTH];
};

struct RedSpyThreadData{
    AddrValPair buffer[MAX_WRITE_OPS_IN_INS];
    uint32_t regCtxt[REG_LAST];
    UINT8 rectxt[REG_LAST][MAX_WRITE_OP_LENGTH];
    uint64_t bytesWritten;
    
    long long NUM_INS;
    bool Sample_flag;
    long long NUM_winds;
};

struct RegInfo{
    UINT8 count;
    REG regs[MAX_WRITE_OPS_IN_INS];
    bool alian[MAX_WRITE_OPS_IN_INS];
};


// key for accessing TLS storage in the threads. initialized once in main()
static  TLS_KEY client_tls_key;
static RedSpyThreadData* gSingleThreadedTData;

// function to access thread-specific data
inline RedSpyThreadData* ClientGetTLS(const THREADID threadId) {
#ifdef MULTI_THREADED
    RedSpyThreadData* tdata =
    static_cast<RedSpyThreadData*>(PIN_GetThreadData(client_tls_key, threadId));
    return tdata;
#else
    return gSingleThreadedTData;
#endif
}

//const map to handle the register aliases
static unordered_map<uint32_t,list<uint32_t>> RegAliasMap = {
    {REG_EAX,{REG_GAX,REG_AX,REG_AH,REG_AL}},
    {REG_EBX,{REG_GBX,REG_BX,REG_BH,REG_BL}},
    {REG_ECX,{REG_GCX,REG_CX,REG_CH,REG_CL}},
    {REG_EDX,{REG_GDX,REG_DX,REG_DH,REG_DL}},
    {REG_GAX,{REG_EAX,REG_AX,REG_AH,REG_AL}},
    {REG_GBX,{REG_EBX,REG_BX,REG_BH,REG_BL}},
    {REG_GCX,{REG_ECX,REG_CX,REG_CH,REG_CL}},
    {REG_GDX,{REG_EDX,REG_DX,REG_DH,REG_DL}},
    {REG_AX,{REG_GAX,REG_EAX,REG_AH,REG_AL}},
    {REG_BX,{REG_GBX,REG_EBX,REG_BH,REG_BL}},
    {REG_CX,{REG_GCX,REG_ECX,REG_CH,REG_CL}},
    {REG_DX,{REG_GDX,REG_EDX,REG_DH,REG_DL}},
    {REG_AH,{REG_GAX,REG_EAX,REG_AX}},
    {REG_BH,{REG_GBX,REG_EBX,REG_BX}},
    {REG_CH,{REG_GCX,REG_ECX,REG_CX}},
    {REG_DH,{REG_GDX,REG_EDX,REG_DX}},
    {REG_AL,{REG_GAX,REG_EAX,REG_AX}},
    {REG_BL,{REG_GBX,REG_EBX,REG_BX}},
    {REG_CL,{REG_GCX,REG_ECX,REG_CX}},
    {REG_DL,{REG_GDX,REG_EDX,REG_DX}},
};


INT32 Usage2() {
    PIN_ERROR("Pin tool to gather calling context on each load and store.\n" + KNOB_BASE::StringKnobSummary() + "\n");
    return -1;
}

// Main for RedSpy, initialize the tool, register instrumentation functions and call the target program.
static FILE* gTraceFile;
static uint8_t ** gL1PageTable[LEVEL_1_PAGE_TABLE_SIZE];

// Initialized the needed data structures before launching the target program
static void ClientInit(int argc, char* argv[]) {
    // Create output file
    char name[MAX_FILE_PATH] = "redspy_temporal.out.";
    char* envPath = getenv("CCTLIB_CLIENT_OUTPUT_FILE");
    
    if(envPath) {
        // assumes max of MAX_FILE_PATH
        strcpy(name, envPath);
    }
    
    gethostname(name + strlen(name), MAX_FILE_PATH - strlen(name));
    pid_t pid = getpid();
    sprintf(name + strlen(name), "%d", pid);
    cerr << "\n Creating log file at:" << name << "\n";
    gTraceFile = fopen(name, "w");
    // print the arguments passed
    fprintf(gTraceFile, "\n");
    
    for(int i = 0 ; i < argc; i++) {
        fprintf(gTraceFile, "%s ", argv[i]);
    }
    
    fprintf(gTraceFile, "\n");
}


/* helper functions for shadow memory */
static uint8_t* GetOrCreateShadowBaseAddress(uint64_t address) {
    uint8_t *shadowPage;
    uint8_t ***l1Ptr = &gL1PageTable[LEVEL_1_PAGE_TABLE_SLOT(address)];
    if(*l1Ptr == 0) {
        *l1Ptr = (uint8_t **) mmap(0, LEVEL_2_PAGE_TABLE_SIZE, PROT_WRITE | PROT_READ, MAP_NORESERVE | MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
        shadowPage = (*l1Ptr)[LEVEL_2_PAGE_TABLE_SLOT(address)] =  (uint8_t *) mmap(0, PAGE_SIZE * (sizeof(uint64_t)), PROT_WRITE | PROT_READ, MAP_NORESERVE | MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
    } else if((shadowPage = (*l1Ptr)[LEVEL_2_PAGE_TABLE_SLOT(address)]) == 0 ){
        shadowPage = (*l1Ptr)[LEVEL_2_PAGE_TABLE_SLOT(address)] =  (uint8_t *) mmap(0, PAGE_SIZE * (sizeof(uint64_t)), PROT_WRITE | PROT_READ, MAP_NORESERVE | MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
    }
    return shadowPage;
}

static inline void UpdateAliaRegs(REG reg, ADDRINT regValue, uint32_t curCtxtHandle, RedSpyThreadData* td) {
   
    ADDRINT regBefore;
    list<uint32_t>::iterator it = RegAliasMap[reg].begin();
      
    switch (REG_Size(reg)) {
        case 8:
            //handling E*X register
            td->regCtxt[*it] = curCtxtHandle;
            *(ADDRINT *)(&td->rectxt[*it][0]) = regValue & 0xffffffff;
            
            //handling *X register
            it++;
            td->regCtxt[*it] = curCtxtHandle;
            *(ADDRINT *)(&td->rectxt[*it][0]) = regValue & 0xffff;
            
            //handling *H register
            it++;
            td->regCtxt[*it] = curCtxtHandle;
            *(ADDRINT *)(&td->rectxt[*it][0]) = (regValue & 0xff00) >> 8;
            
            //handling *L register
            it++;
            td->regCtxt[*it] = curCtxtHandle;
            *(ADDRINT *)(&td->rectxt[*it][0]) = regValue & 0xff;
            break;
        case 4:
            //handling R*X(G*X) register
            td->regCtxt[*it] = curCtxtHandle;
            regBefore = *(ADDRINT *)(&td->rectxt[*it][0]);
            *(ADDRINT *)(&td->rectxt[*it][0]) = (regBefore & 0xffffffff00000000) | regValue;
            
            //handling *X register
            it++;
            td->regCtxt[*it] = curCtxtHandle;
            *(ADDRINT *)(&td->rectxt[*it][0]) = regValue & 0xffff;
            
            //handling *H register
            it++;
            td->regCtxt[*it] = curCtxtHandle;
            *(ADDRINT *)(&td->rectxt[*it][0]) = (regValue & 0xff00) >> 8;
            
            //handling *L register
            it++;
            td->regCtxt[*it] = curCtxtHandle;
            *(ADDRINT *)(&td->rectxt[*it][0]) = regValue & 0xff;
            break;
        case 2:
            //handling R*X(G*X) register
            td->regCtxt[*it] = curCtxtHandle;
            regBefore = *(ADDRINT *)(&td->rectxt[*it][0]);
            *(ADDRINT *)(&td->rectxt[*it][0]) = (regBefore & 0xffffffffffff0000) | regValue;
            
            //handling *X register
            it++;
            td->regCtxt[*it] = curCtxtHandle;
            regBefore = *(ADDRINT *)(&td->rectxt[*it][0]);
            *(ADDRINT *)(&td->rectxt[*it][0]) = (regBefore & 0xffffffffffff0000) & regValue;
            
            //handling *H register
            it++;
            td->regCtxt[*it] = curCtxtHandle;
            *(ADDRINT *)(&td->rectxt[*it][0]) = regValue >> 8;
            
            //handling *L register
            it++;
            td->regCtxt[*it] = curCtxtHandle;
            *(ADDRINT *)(&td->rectxt[*it][0]) = regValue & 0xff;
            break;
        case 1:
            if (reg == REG_AL || reg == REG_BL || reg == REG_CL || reg == REG_DL) {
                for(; it != RegAliasMap[reg].end(); ++it){
                    td->regCtxt[*it] = curCtxtHandle;
                    regBefore = *(ADDRINT *)(&td->rectxt[*it][0]);
                    *(ADDRINT *)(&td->rectxt[*it][0]) = ((regBefore >> 8)<< 8) & regValue;
                }
            } else {
                regValue = (regValue << 8);
                for(; it != RegAliasMap[reg].end(); ++it){
                    td->regCtxt[*it] = curCtxtHandle;
                    regBefore = *(ADDRINT *)(&td->rectxt[*it][0]);
                    *(ADDRINT *)(&td->rectxt[*it][0]) = (regBefore & 0xffffffffffff00ff) | regValue;
                }
            }
            break;
        default:
            break;
    }
}



static const uint64_t READ_ACCESS_STATES [] = {/*0 byte */0, /*1 byte */ ONE_BYTE_READ_ACTION, /*2 byte */ TWO_BYTE_READ_ACTION, /*3 byte */ 0, /*4 byte */ FOUR_BYTE_READ_ACTION, /*5 byte */0, /*6 byte */0, /*7 byte */0, /*8 byte */ EIGHT_BYTE_READ_ACTION};
static const uint64_t WRITE_ACCESS_STATES [] = {/*0 byte */0, /*1 byte */ ONE_BYTE_WRITE_ACTION, /*2 byte */ TWO_BYTE_WRITE_ACTION, /*3 byte */ 0, /*4 byte */ FOUR_BYTE_WRITE_ACTION, /*5 byte */0, /*6 byte */0, /*7 byte */0, /*8 byte */ EIGHT_BYTE_WRITE_ACTION};
static const uint8_t OVERFLOW_CHECK [] = {/*0 byte */0, /*1 byte */ 0, /*2 byte */ 0, /*3 byte */ 1, /*4 byte */ 2, /*5 byte */3, /*6 byte */4, /*7 byte */5, /*8 byte */ 6};

static unordered_map<uint64_t, uint64_t> RedMap[THREAD_MAX];
static inline void AddToRedTable(uint64_t key,  uint16_t value, THREADID threadId) {
#ifdef MULTI_THREADED
    LOCK_RED_MAP();
#endif
    unordered_map<uint64_t, uint64_t>::iterator it = RedMap[threadId].find(key);
    if ( it  == RedMap[threadId].end()) {
        RedMap[threadId][key] = value;
    } else {
        it->second += value;
    }
#ifdef MULTI_THREADED
    UNLOCK_RED_MAP();
#endif
}

#ifdef ENABLE_SAMPLING

static inline VOID EmptyCtxt(RedSpyThreadData* tData){

    int i;
    for( i = 0; i< REG_LAST; ++i){
        tData->regCtxt[i] = 0;
    }
    /*
    tData->NUM_winds++;
    if(tData->NUM_winds > WINDOW_CLEAN){
        long count = tData->bytesWritten;
        long delNum = 0;
        //printf("size of the map %lu, total reg written %lu\n",count,tData->numRegWritten);
        unordered_map<uint64_t,uint64_t>::iterator it,ittmp;
        for (it = RedMap[threadId].begin(); it != RedMap[threadId].end();) {
            //printf("%lu\n",(*it).second);
            if((*it).second * 100.0 < count){
                delNum += (*it).second;
                ittmp = it;
                it++;
                RedMap[threadId].erase(ittmp);
            }else
                it++;
        }
        tData->NUM_winds=0;
        tData->bytesWritten -= delNum;
    }*/
}

static ADDRINT IfEnableSample(THREADID threadId){
    RedSpyThreadData* const tData = ClientGetTLS(threadId);
    if(tData->Sample_flag){
        return 1;
    }
    return 0;
}

#endif

static inline VOID CheckSpecialRegAfterWrite(uint32_t reg, ADDRINT regValue, uint32_t opaqueHandle, THREADID threadId){
    
    RedSpyThreadData* const td = ClientGetTLS(threadId);
    ContextHandle_t curCtxtHandle = GetContextHandle(threadId, opaqueHandle);
    
    ADDRINT regBefore = *(ADDRINT *)(&td->rectxt[reg][0]);
    
    bool isRedundantWrite = (regBefore == regValue);
    
    if(isRedundantWrite && td->regCtxt[reg] != 0) {
        AddToRedTable(MAKE_CONTEXT_PAIR(td->regCtxt[REG_EAX],curCtxtHandle),4,threadId);
    }else{
        *(ADDRINT *)(&td->rectxt[reg][0]) = regValue;
    }
    td->regCtxt[reg] = curCtxtHandle;
    list<uint32_t>::iterator it = RegAliasMap[reg].begin();

    //handling R*X(G*X) register
    td->regCtxt[*it] = curCtxtHandle;
    regBefore = *(ADDRINT *)(&td->rectxt[*it][0]);
    *(ADDRINT *)(&td->rectxt[*it][0]) = (regBefore & 0xffffffff00000000) | regValue;
    
    //handling *X register
    it++;
    td->regCtxt[*it] = curCtxtHandle;
    *(ADDRINT *)(&td->rectxt[*it][0]) = regValue & 0xffff;
    
    //handling *H register
    it++;
    td->regCtxt[*it] = curCtxtHandle;
    *(ADDRINT *)(&td->rectxt[*it][0]) = (regValue & 0xff00) >> 8;
    
    //handling *L register
    it++;
    td->regCtxt[*it] = curCtxtHandle;
    *(ADDRINT *)(&td->rectxt[*it][0]) = regValue & 0xff;
}

static inline VOID CheckOneRegValueAfterWrite(uint32_t opaqueHandle, THREADID threadId, REG reg, uint32_t regBytes, ADDRINT regValue, bool regAlia){
    
    RedSpyThreadData* const tData = ClientGetTLS(threadId);
    
    ContextHandle_t curCtxtHandle = GetContextHandle(threadId, opaqueHandle);

    ADDRINT regBefore = *(ADDRINT *)(&tData->rectxt[reg][0]);
        
    bool isRedundantWrite = (regBefore == regValue);
        
    if(isRedundantWrite && tData->regCtxt[reg] != 0) {
        AddToRedTable(MAKE_CONTEXT_PAIR(tData->regCtxt[reg],curCtxtHandle),regBytes,threadId);
    }else{
        *(ADDRINT *)(&tData->rectxt[reg][0]) = regValue;
    }
    tData->regCtxt[reg] = curCtxtHandle;
    if(regAlia)
        UpdateAliaRegs(reg,regValue,curCtxtHandle,tData);
}

static inline VOID CheckGenValueAfterWrite(uint32_t opaqueHandle, THREADID threadId, void * regs, uint32_t regBytes, uint32_t regCount, ...){
    
    RedSpyThreadData* const tData = ClientGetTLS(threadId);
   
    ContextHandle_t curCtxtHandle = GetContextHandle(threadId, opaqueHandle);
    struct RegInfo * wRegs = (struct RegInfo *)regs;
    
    va_list ap;
    UINT8 i;
    va_start(ap, regCount);
    
    for (i = 0; i < regCount; i++) {
        REG reg = wRegs->regs[i];
        ADDRINT regV = va_arg(ap, ADDRINT);
        ADDRINT regBefore = *(ADDRINT *)(&tData->rectxt[reg][0]);
        
        bool isRedundantWrite = (regBefore == regV);
        
        if(isRedundantWrite && tData->regCtxt[reg] != 0) {
            AddToRedTable(MAKE_CONTEXT_PAIR(tData->regCtxt[reg],curCtxtHandle),REG_Size(reg),threadId);
        }else{
            *(ADDRINT *)(&tData->rectxt[reg][0]) = regV;
        }
        tData->regCtxt[reg] = curCtxtHandle;
        if(wRegs->alian[i])
           UpdateAliaRegs(reg,regV,curCtxtHandle,tData);
    }
}

static inline  VOID CheckLargeRegAfterWrite(PIN_REGISTER* regRef, REG reg, uint32_t regSize, uint32_t opaqueHandle, THREADID threadId){
    
    RedSpyThreadData* const tData = ClientGetTLS(threadId);
    
    ContextHandle_t curCtxtHandle = GetContextHandle(threadId, opaqueHandle);
    
    //struct RegInfo * wRegs = (struct RegInfo *)regs;
    int i;
    bool isRedundantWrite = true;
    for(i = 0; i < 16; ++i){
        if(tData->rectxt[reg][i] != regRef->byte[i])
            isRedundantWrite = false;
        tData->rectxt[reg][i] = regRef->byte[i];
    }
    
    if(isRedundantWrite && tData->regCtxt[reg]!=0) {
        AddToRedTable(MAKE_CONTEXT_PAIR(tData->regCtxt[reg],curCtxtHandle),regSize,threadId);
    }
    tData->regCtxt[reg] = curCtxtHandle;
}

inline bool IsAliaReg(REG reg){
    switch(reg){
        case REG_EAX:
        case REG_EBX:
        case REG_ECX:
        case REG_EDX:
        case REG_GAX:
        case REG_GBX:
        case REG_GCX:
        case REG_GDX:
        case REG_AX:
        case REG_BX:
        case REG_CX:
        case REG_DX:
        case REG_AH:
        case REG_BH:
        case REG_CH:
        case REG_DH:
        case REG_AL:
        case REG_BL:
        case REG_CL:
        case REG_DL:
            return true;
        default: return false;
    }
}

#ifdef ENABLE_SAMPLING

#define HANDLE_2REGS() \
INS_InsertIfPredicatedCall(ins, IPOINT_AFTER, (AFUNPTR)IfEnableSample, IARG_THREAD_ID,IARG_END); \
INS_InsertThenPredicatedCall(ins, IPOINT_AFTER, (AFUNPTR) CheckGenValueAfterWrite, IARG_UINT32, opaqueHandle, IARG_THREAD_ID,  IARG_PTR, wRegs,IARG_UINT32, totBytes, IARG_UINT32,wRegs->count, IARG_REG_VALUE, wRegs->regs[0], IARG_REG_VALUE, wRegs->regs[1], IARG_END);break

#define HANDLE_3REGS() \
INS_InsertIfPredicatedCall(ins, IPOINT_AFTER, (AFUNPTR)IfEnableSample, IARG_THREAD_ID,IARG_END);\
INS_InsertThenPredicatedCall(ins, IPOINT_AFTER, (AFUNPTR) CheckGenValueAfterWrite, IARG_UINT32, opaqueHandle, IARG_THREAD_ID,  IARG_PTR, wRegs,IARG_UINT32, totBytes, IARG_UINT32,wRegs->count, IARG_REG_VALUE, wRegs->regs[0], IARG_REG_VALUE, wRegs->regs[1], IARG_REG_VALUE, wRegs->regs[2], IARG_END);break

#define HANDLE_4REGS() \
INS_InsertIfPredicatedCall(ins, IPOINT_AFTER, (AFUNPTR)IfEnableSample, IARG_THREAD_ID,IARG_END); \
INS_InsertThenPredicatedCall(ins, IPOINT_AFTER, (AFUNPTR) CheckGenValueAfterWrite, IARG_UINT32, opaqueHandle, IARG_THREAD_ID,  IARG_PTR, wRegs,IARG_UINT32, totBytes, IARG_UINT32,wRegs->count, IARG_REG_VALUE, wRegs->regs[0], IARG_REG_VALUE, wRegs->regs[1], IARG_REG_VALUE, wRegs->regs[2], IARG_REG_VALUE, wRegs->regs[3], IARG_END); break

#else

#define HANDLE_2REGS() \
INS_InsertPredicatedCall(ins, IPOINT_AFTER, (AFUNPTR) CheckGenValueAfterWrite, IARG_UINT32, opaqueHandle, IARG_THREAD_ID,  IARG_PTR, wRegs,IARG_UINT32, totBytes, IARG_UINT32,wRegs->count, IARG_REG_VALUE, wRegs->regs[0], IARG_REG_VALUE, wRegs->regs[1], IARG_END);break

#define HANDLE_3REGS() \
INS_InsertPredicatedCall(ins, IPOINT_AFTER, (AFUNPTR) CheckGenValueAfterWrite, IARG_UINT32, opaqueHandle, IARG_THREAD_ID,  IARG_PTR, wRegs,IARG_UINT32, totBytes, IARG_UINT32,wRegs->count, IARG_REG_VALUE, wRegs->regs[0], IARG_REG_VALUE, wRegs->regs[1], IARG_REG_VALUE, wRegs->regs[2], IARG_END); break

#define HANDLE_4REGS() \
INS_InsertPredicatedCall(ins, IPOINT_AFTER, (AFUNPTR) CheckGenValueAfterWrite, IARG_UINT32, opaqueHandle, IARG_THREAD_ID,  IARG_PTR, wRegs,IARG_UINT32, totBytes, IARG_UINT32,wRegs->count, IARG_REG_VALUE, wRegs->regs[0], IARG_REG_VALUE, wRegs->regs[1], IARG_REG_VALUE, wRegs->regs[2], IARG_REG_VALUE, wRegs->regs[3], IARG_END); break

#endif

static inline void InstrumentReadValueAfterWritingRegs(INS ins, struct RegInfo * wRegs, uint32_t opaqueHandle) {
   
    UINT8 i;
    uint32_t totBytes = 0;
    uint32_t regSize;
    bool flag = true;
    for(i = 0; i < wRegs->count; ++i){
        regSize = REG_Size(wRegs->regs[i]);
        if (regSize > 8) {
            flag = false;
        }
        if(IsAliaReg(wRegs->regs[i]))
           wRegs->alian[i] = 1;
        else
           wRegs->alian[i] = 0;
        totBytes += regSize;
    }
    
    if(flag){
        switch (wRegs->count) {
            case 2: HANDLE_2REGS();
            case 3: HANDLE_3REGS();
            case 4: HANDLE_4REGS();
            default:
                assert(0 && "NYI");
                break;
        }
        
    }else{ 
        printf("Writing multiple registers with large size\n");
    }
}


/*********************** memory temporal redundancy functions **************************/
template<int start, int end, int incr, bool conditional>
struct UnrolledLoop{
    static __attribute__((always_inline)) void Body(function<void (const int)> func){
        func(start); // Real loop body
        UnrolledLoop<start+incr, end, incr, conditional>:: Body(func);   // unroll next iteration
    }
    static __attribute__((always_inline)) void BodySamePage(ContextHandle_t * __restrict__ prevIP, const ContextHandle_t handle, THREADID threadId){
        if(conditional) {
            // report in RedTable
            AddToRedTable(MAKE_CONTEXT_PAIR(prevIP[start], handle), 1, threadId);
        }
        // Update context
        prevIP[start] = handle;
        UnrolledLoop<start+incr, end, incr, conditional>:: BodySamePage(prevIP, handle, threadId);   // unroll next iteration
    }
    static __attribute__((always_inline)) void BodyStraddlePage(uint64_t addr, const ContextHandle_t handle, THREADID threadId){
        uint8_t * status = GetOrCreateShadowBaseAddress((uint64_t)addr + start);
        ContextHandle_t * prevIP = (ContextHandle_t*)(status + PAGE_OFFSET(((uint64_t)addr + start)) * sizeof(ContextHandle_t));
        if (conditional) {
            // report in RedTable
            AddToRedTable(MAKE_CONTEXT_PAIR(prevIP[0 /* 0 is correct*/ ], handle), 1, threadId);
        }
        // Update context
        prevIP[0] = handle;
        UnrolledLoop<start+incr, end, incr, conditional>:: BodyStraddlePage(addr, handle, threadId);   // unroll next iteration
    }
};

template<int end,  int incr, bool conditional>
struct UnrolledLoop<end , end , incr, conditional>{
    static __attribute__((always_inline)) void Body(function<void (const int)> func){}
    static __attribute__((always_inline)) void BodySamePage(ContextHandle_t * __restrict__ prevIP, const ContextHandle_t handle, THREADID threadId){}
    static __attribute__((always_inline)) void BodyStraddlePage(uint64_t addr, const ContextHandle_t handle, THREADID threadId){}
};

template<int start, int end, int incr>
struct UnrolledConjunction{
    static __attribute__((always_inline)) bool Body(function<bool (const int)> func){
        return func(start) && UnrolledConjunction<start+incr, end, incr>:: Body(func);   // unroll next iteration
    }
    static __attribute__((always_inline)) bool BodyContextCheck(ContextHandle_t * __restrict__ prevIP){
        return (prevIP[0] == prevIP[start]) && UnrolledConjunction<start+incr, end, incr>:: BodyContextCheck(prevIP);   // unroll next iteration
    }
};

template<int end,  int incr>
struct UnrolledConjunction<end , end , incr>{
    static __attribute__((always_inline)) bool Body(function<void (const int)> func){
        return true;
    }
    static __attribute__((always_inline)) bool BodyContextCheck(ContextHandle_t * __restrict__ prevIP){
        return true;
    }
};


template<uint16_t AccessLen, uint32_t bufferOffset>
struct RedSpyAnalysis{
    static __attribute__((always_inline)) bool IsWriteRedundant(void * &addr, THREADID threadId){
        RedSpyThreadData* const tData = ClientGetTLS(threadId);
        AddrValPair * avPair = & tData->buffer[bufferOffset];
        addr = avPair->address;
        switch(AccessLen){
            case 1: return *((uint8_t*)(&avPair->value)) == *(static_cast<uint8_t*>(avPair->address));
            case 2: return *((uint16_t*)(&avPair->value)) == *(static_cast<uint16_t*>(avPair->address));
            case 4: return *((uint32_t*)(&avPair->value)) == *(static_cast<uint32_t*>(avPair->address));
            case 8: return *((uint64_t*)(&avPair->value)) == *(static_cast<uint64_t*>(avPair->address));
            default: return memcmp(&avPair->value, avPair->address, AccessLen) == 0;
        }
    }
    
    static __attribute__((always_inline)) VOID RecordNByteValueBeforeWrite(void* addr, THREADID threadId){
        
        RedSpyThreadData* const tData = ClientGetTLS(threadId);
       
        AddrValPair * avPair = & tData->buffer[bufferOffset];

        avPair->address = addr;
        switch(AccessLen){
            case 1: *((uint8_t*)(&avPair->value)) = *(static_cast<uint8_t*>(addr)); break;
            case 2: *((uint16_t*)(&avPair->value)) = *(static_cast<uint16_t*>(addr)); break;
            case 4: *((uint32_t*)(&avPair->value)) = *(static_cast<uint32_t*>(addr)); break;
            case 8: *((uint64_t*)(&avPair->value)) = *(static_cast<uint64_t*>(addr)); break;
            default:memcpy(&avPair->value, addr, AccessLen);
        }
    }
    
    static __attribute__((always_inline)) VOID CheckNByteValueAfterWrite(uint32_t opaqueHandle, THREADID threadId){
        RedSpyThreadData* const tData = ClientGetTLS(threadId);
        void * addr;
        bool isRedundantWrite = IsWriteRedundant(addr, threadId);

        ContextHandle_t curCtxtHandle = GetContextHandle(threadId, opaqueHandle);
        
        uint8_t* status = GetOrCreateShadowBaseAddress((uint64_t)addr);
        ContextHandle_t * __restrict__ prevIP = (ContextHandle_t*)(status + PAGE_OFFSET((uint64_t)addr) * sizeof(ContextHandle_t));
        const bool isAccessWithinPageBoundary = IS_ACCESS_WITHIN_PAGE_BOUNDARY( (uint64_t)addr, AccessLen);
        if(isRedundantWrite) {
            // detected redundancy
            if(isAccessWithinPageBoundary) {
                // All from same ctxt?
                if (UnrolledConjunction<0, AccessLen, 1>::BodyContextCheck(prevIP)) {
                    // report in RedTable
                    AddToRedTable(MAKE_CONTEXT_PAIR(prevIP[0], curCtxtHandle), AccessLen, threadId);
                    // Update context
                    UnrolledLoop<0, AccessLen, 1, false /* redundancy is updated outside*/>::BodySamePage(prevIP, curCtxtHandle, threadId);
                } else {
                    // different contexts
                    UnrolledLoop<0, AccessLen, 1, true /* redundancy is updated inside*/>::BodySamePage(prevIP, curCtxtHandle, threadId);
                }
            } else {
                // Write across a 64-K page boundary
                // First byte is on this page though
                AddToRedTable(MAKE_CONTEXT_PAIR(prevIP[0], curCtxtHandle), 1, threadId);
                // Update context
                prevIP[0] = curCtxtHandle;
                
                // Remaining bytes [1..AccessLen] somewhere will across a 64-K page boundary
                UnrolledLoop<1, AccessLen, 1, true /* update redundancy */>::BodyStraddlePage( (uint64_t) addr, curCtxtHandle, threadId);
            }
        } else {
            // No redundancy.
            // Just update contexts
            if(isAccessWithinPageBoundary) {
                // Update context
                UnrolledLoop<0, AccessLen, 1, false /* not redundant*/>::BodySamePage(prevIP, curCtxtHandle, threadId);
            } else {
                // Write across a 64-K page boundary
                // Update context
                prevIP[0] = curCtxtHandle;
                
                // Remaining bytes [1..AccessLen] somewhere will across a 64-K page boundary
                UnrolledLoop<1, AccessLen, 1, false /* dont update redundancy */>::BodyStraddlePage( (uint64_t) addr, curCtxtHandle, threadId);

            }
        }
    }
};


static inline VOID RecordValueBeforeLargeWrite(void* addr, UINT32 accessLen,  uint32_t bufferOffset, THREADID threadId){
    
    RedSpyThreadData* const tData = ClientGetTLS(threadId);
    memcpy(& (tData->buffer[bufferOffset].value), addr, accessLen);
    tData->buffer[bufferOffset].address = addr;
}

static inline VOID CheckAfterLargeWrite(UINT32 accessLen,  uint32_t bufferOffset, uint32_t opaqueHandle, THREADID threadId){

    RedSpyThreadData* const tData = ClientGetTLS(threadId);
    void * addr = tData->buffer[bufferOffset].address;
    ContextHandle_t curCtxtHandle = GetContextHandle(threadId, opaqueHandle);
    
    uint8_t* status = GetOrCreateShadowBaseAddress((uint64_t)addr);
    ContextHandle_t * __restrict__ prevIP = (ContextHandle_t*)(status + PAGE_OFFSET((uint64_t)addr) * sizeof(ContextHandle_t));
    if(memcmp( & (tData->buffer[bufferOffset].value), addr, accessLen) == 0){
        // redundant
        for(UINT32 index = 0 ; index < accessLen; index++){
            status = GetOrCreateShadowBaseAddress((uint64_t)addr + index);
            prevIP = (ContextHandle_t*)(status + PAGE_OFFSET(((uint64_t)addr + index)) * sizeof(ContextHandle_t));
            // report in RedTable
            AddToRedTable(MAKE_CONTEXT_PAIR(prevIP[0 /* 0 is correct*/ ], curCtxtHandle), 1, threadId);
            // Update context
            prevIP[0] = curCtxtHandle;
        }
    }else{
        // Not redundant
        for(UINT32 index = 0 ; index < accessLen; index++){
            status = GetOrCreateShadowBaseAddress((uint64_t)addr + index);
            prevIP = (ContextHandle_t*)(status + PAGE_OFFSET(((uint64_t)addr + index)) * sizeof(ContextHandle_t));
            // Update context
            prevIP[0] = curCtxtHandle;
        }
    }
}

#ifdef ENABLE_SAMPLING

#define HANDLE_CASE(NUM, BUFFER_INDEX) \
case (NUM):{INS_InsertIfPredicatedCall(ins, IPOINT_BEFORE, (AFUNPTR)IfEnableSample, IARG_THREAD_ID,IARG_END);\
INS_InsertThenPredicatedCall(ins, IPOINT_BEFORE, (AFUNPTR) RedSpyAnalysis<(NUM), (BUFFER_INDEX)>::RecordNByteValueBeforeWrite, IARG_MEMORYOP_EA, memOp, IARG_THREAD_ID, IARG_END);\
INS_InsertIfPredicatedCall(ins, IPOINT_AFTER, (AFUNPTR)IfEnableSample, IARG_THREAD_ID,IARG_END);\
INS_InsertThenPredicatedCall(ins, IPOINT_AFTER, (AFUNPTR) RedSpyAnalysis<(NUM), (BUFFER_INDEX)>::CheckNByteValueAfterWrite, IARG_UINT32, opaqueHandle, IARG_THREAD_ID, IARG_INST_PTR,IARG_END);}break

#define HANDLE_LARGE() \
INS_InsertIfPredicatedCall(ins, IPOINT_BEFORE, (AFUNPTR)IfEnableSample, IARG_THREAD_ID,IARG_END);\
INS_InsertThenPredicatedCall(ins, IPOINT_BEFORE, (AFUNPTR) RecordValueBeforeLargeWrite, IARG_MEMORYOP_EA, memOp, IARG_MEMORYWRITE_SIZE, IARG_UINT32, readBufferSlotIndex, IARG_THREAD_ID, IARG_END);\
INS_InsertIfPredicatedCall(ins, IPOINT_AFTER, (AFUNPTR)IfEnableSample, IARG_THREAD_ID,IARG_END);\
INS_InsertThenPredicatedCall(ins, IPOINT_AFTER, (AFUNPTR) CheckAfterLargeWrite, IARG_MEMORYREAD_SIZE, IARG_UINT32, readBufferSlotIndex, IARG_UINT32, opaqueHandle, IARG_THREAD_ID, IARG_END)

#else

#define HANDLE_CASE(NUM, BUFFER_INDEX) \
case (NUM):{INS_InsertPredicatedCall(ins, IPOINT_BEFORE, (AFUNPTR) RedSpyAnalysis<(NUM), (BUFFER_INDEX)>::RecordNByteValueBeforeWrite, IARG_MEMORYOP_EA, memOp, IARG_THREAD_ID, IARG_END);\
INS_InsertPredicatedCall(ins, IPOINT_AFTER, (AFUNPTR) RedSpyAnalysis<(NUM), (BUFFER_INDEX)>::CheckNByteValueAfterWrite, IARG_UINT32, opaqueHandle, IARG_THREAD_ID, IARG_INST_PTR,IARG_END);}break

#define HANDLE_LARGE() \
INS_InsertPredicatedCall(ins, IPOINT_BEFORE, (AFUNPTR) RecordValueBeforeLargeWrite, IARG_MEMORYOP_EA, memOp, IARG_MEMORYWRITE_SIZE, IARG_UINT32, readBufferSlotIndex, IARG_THREAD_ID, IARG_END);\
INS_InsertPredicatedCall(ins, IPOINT_AFTER, (AFUNPTR) CheckAfterLargeWrite, IARG_MEMORYREAD_SIZE, IARG_UINT32, readBufferSlotIndex, IARG_UINT32, opaqueHandle, IARG_THREAD_ID, IARG_END)

#endif


static int GetNumWriteOperandsInIns(INS ins, UINT32 & whichOp){
    int numWriteOps = 0;
    UINT32 memOperands = INS_MemoryOperandCount(ins);
    for(UINT32 memOp = 0; memOp < memOperands; memOp++) {
        if (INS_MemoryOperandIsWritten(ins, memOp)) {
            numWriteOps++;
            whichOp = memOp;
        }
    }
    return numWriteOps;
}

template<uint32_t readBufferSlotIndex>
struct RedSpyInstrument{
    static __attribute__((always_inline)) void InstrumentReadValueBeforeAndAfterWriting(INS ins, UINT32 memOp, uint32_t opaqueHandle){
        UINT32 refSize = INS_MemoryOperandSize(ins, memOp);
        switch(refSize) {
                HANDLE_CASE(1, readBufferSlotIndex);
                HANDLE_CASE(2, readBufferSlotIndex);
                HANDLE_CASE(4, readBufferSlotIndex);
                HANDLE_CASE(8, readBufferSlotIndex);
                HANDLE_CASE(10, readBufferSlotIndex);
                HANDLE_CASE(16, readBufferSlotIndex);
                
            default: {
                HANDLE_LARGE();
            }
        }
    }
};

/*********************  instrument analysis  ************************/

#ifdef ENABLE_SAMPLING

#define HANDLE_LARGEREG() \
INS_InsertIfPredicatedCall(ins, IPOINT_AFTER, (AFUNPTR)IfEnableSample, IARG_THREAD_ID,IARG_END);\
INS_InsertThenPredicatedCall(ins, IPOINT_AFTER, (AFUNPTR) CheckLargeRegAfterWrite, IARG_REG_CONST_REFERENCE,reg, IARG_UINT32, reg, IARG_UINT32, regSize, IARG_UINT32, opaqueHandle, IARG_THREAD_ID,IARG_END)

#define HANDLE_SPECIAL_REG() \
INS_InsertIfPredicatedCall(ins, IPOINT_AFTER, (AFUNPTR)IfEnableSample, IARG_THREAD_ID,IARG_END);\
INS_InsertThenPredicatedCall(ins, IPOINT_AFTER, (AFUNPTR) CheckSpecialRegAfterWrite,IARG_UINT32,reg,IARG_REG_VALUE,reg,IARG_UINT32,opaqueHandle,IARG_THREAD_ID,IARG_END)

#define HANDLE_ONEREG() \
INS_InsertIfPredicatedCall(ins, IPOINT_AFTER, (AFUNPTR)IfEnableSample, IARG_THREAD_ID,IARG_END); \
INS_InsertThenPredicatedCall(ins, IPOINT_AFTER, (AFUNPTR) CheckOneRegValueAfterWrite,IARG_UINT32,opaqueHandle,IARG_THREAD_ID,IARG_UINT32,reg,IARG_UINT32, REG_Size(reg),IARG_REG_VALUE,reg,IARG_BOOL,IsAliaReg(reg),IARG_END)

#else

#define HANDLE_LARGEREG() \
INS_InsertPredicatedCall(ins, IPOINT_AFTER, (AFUNPTR) CheckLargeRegAfterWrite, IARG_REG_CONST_REFERENCE,reg, IARG_UINT32, reg, IARG_UINT32, regSize, IARG_UINT32, opaqueHandle, IARG_THREAD_ID,IARG_END)

#define HANDLE_SPECIAL_REG() \
INS_InsertPredicatedCall(ins, IPOINT_AFTER, (AFUNPTR) CheckSpecialRegAfterWrite,IARG_UINT32,reg,IARG_REG_VALUE,reg,IARG_UINT32,opaqueHandle,IARG_THREAD_ID,IARG_END)

#define HANDLE_ONEREG() \
INS_InsertPredicatedCall(ins, IPOINT_AFTER, (AFUNPTR) CheckOneRegValueAfterWrite,IARG_UINT32,opaqueHandle,IARG_THREAD_ID,IARG_UINT32,reg,IARG_UINT32, REG_Size(reg),IARG_REG_VALUE,reg,IARG_BOOL,IsAliaReg(reg),IARG_END)

#endif

static inline bool INS_IsIgnorable(INS ins){
    if( INS_IsFarJump(ins) || INS_IsDirectFarJump(ins) || INS_IsMaskedJump(ins))
        return true;
    else if(INS_IsRet(ins) || INS_IsIRet(ins))
        return true;
    else if(INS_IsCall(ins) || INS_IsSyscall(ins))
        return true;
    else if(INS_IsBranch(ins) || INS_IsRDTSC(ins) || INS_IsNop(ins))
        return true;
    return false;
}

static inline bool REG_IsIgnorable(REG reg){
    if (REG_is_seg(reg))
        return true;
    else if(REG_is_pin_gr(reg))
        return true;
    else if(reg == REG_MXCSR)
        return true;
    else if(reg == REG_GFLAGS || reg == REG_FLAGS)
        return true;
    else if(reg == REG_ST0)
        return true;
    return false;
}

static VOID InstrumentInsCallback(INS ins, VOID* v, const uint32_t opaqueHandle) {
    if (!INS_HasFallThrough(ins)) return;
    if (INS_IsIgnorable(ins))return;
    if (INS_IsBranchOrCall(ins) || INS_IsRet(ins)) return;

      //Instrument memory writes to find redundancy
      // Special case, if we have only one write operand
      UINT32 whichOp = 0;
      if(GetNumWriteOperandsInIns(ins, whichOp) == 1){
        // Read the value at location before and after the instruction
          RedSpyInstrument<0>::InstrumentReadValueBeforeAndAfterWriting(ins, whichOp, opaqueHandle);
      }else{
          UINT32 memOperands = INS_MemoryOperandCount(ins);
          int readBufferSlotIndex=0;
          for(UINT32 memOp = 0; memOp < memOperands; memOp++) {
              
              if(!INS_MemoryOperandIsWritten(ins, memOp))
                  continue;
              
              switch (readBufferSlotIndex) {
                  case 0:
                      // Read the value at location before and after the instruction
                      RedSpyInstrument<0>::InstrumentReadValueBeforeAndAfterWriting(ins, whichOp, opaqueHandle);
                      break;
                  case 1:
                      // Read the value at location before and after the instruction
                      RedSpyInstrument<1>::InstrumentReadValueBeforeAndAfterWriting(ins, whichOp, opaqueHandle);
                      break;
                  case 2:
                      // Read the value at location before and after the instruction
                      RedSpyInstrument<2>::InstrumentReadValueBeforeAndAfterWriting(ins, whichOp, opaqueHandle);
                      break;
                  case 3:
                      // Read the value at location before and after the instruction
                      RedSpyInstrument<3>::InstrumentReadValueBeforeAndAfterWriting(ins, whichOp, opaqueHandle);
                      break;
                  case 4:
                      // Read the value at location before and after the instruction
                      RedSpyInstrument<4>::InstrumentReadValueBeforeAndAfterWriting(ins, whichOp, opaqueHandle);
                      break;
                  default:
                      assert(0 && "NYI");
                      break;
              }
              // use next slot for the next write operand
              readBufferSlotIndex++;
          }
      }

      //Instrument register writes to find redundancy
      struct RegInfo * wRegs = new struct RegInfo;
    
      UINT32 numOperands = INS_OperandCount(ins);
    
      int regCount = 0;
    
      for(UINT32 Oper = 0; Oper < numOperands; Oper++) {
        
        if(!INS_OperandWritten(ins, Oper) || !INS_OperandIsReg(ins,Oper))
            continue;
        
        REG curReg = INS_OperandReg(ins,Oper);
        
        if(REG_IsIgnorable(curReg))
            continue;
        
        if (regCount >= MAX_WRITE_OPS_IN_INS) {
            assert(0 && "NYI");
            break;
        }else{
            wRegs->regs[regCount] = curReg;
            regCount++;
            wRegs->count = regCount;
        }
      }
      if(regCount == 1){
        REG reg = wRegs->regs[0];
        uint32_t regSize = REG_Size(reg);
        if(regSize > 8){
            HANDLE_LARGEREG();
        }else{
            switch (reg) {
                case REG_EAX:
                case REG_EBX:
                case REG_ECX:
                case REG_EDX: HANDLE_SPECIAL_REG(); break;
                default: HANDLE_ONEREG(); break;
            }
        }
      }else if(regCount > 1)
        InstrumentReadValueAfterWritingRegs(ins, wRegs, opaqueHandle);
}

#ifdef ENABLE_SAMPLING

inline VOID UpdateAndCheck(uint32_t count, uint32_t bytes, THREADID threadId) {
    
    RedSpyThreadData* const tData = ClientGetTLS(threadId);
    tData->bytesWritten += bytes;
    if(tData->Sample_flag){
        tData->NUM_INS += count;
        if(tData->NUM_INS > WINDOW_ENABLE){
            tData->Sample_flag = false;
            tData->NUM_INS = 0;
            EmptyCtxt(tData);
        }
    }else{
        tData->NUM_INS += count;
        if(tData->NUM_INS > WINDOW_DISABLE){
            tData->Sample_flag = true;
            tData->NUM_INS = 0;
        }
    }
}

inline VOID Update(uint32_t count, uint32_t bytes, THREADID threadId){
    RedSpyThreadData* const tData = ClientGetTLS(threadId);
    tData->NUM_INS += count;
    tData->bytesWritten += bytes;
}

//instrument the trace, count the number of ins in the trace, decide to instrument or not
static void InstrumentTrace(TRACE trace, void* f) {
    bool check = false;
    for (BBL bbl = TRACE_BblHead(trace); BBL_Valid(bbl); bbl = BBL_Next(bbl))
    {
        uint32_t totInsInBbl = BBL_NumIns(bbl);
        uint32_t totBytes = 0;
        for(INS ins = BBL_InsHead(bbl); INS_Valid(ins); ins = INS_Next(ins)) {
            if(INS_IsMemoryWrite(ins)) {
                totBytes += INS_MemoryWriteSize(ins);
            }
            UINT32 numOperands = INS_OperandCount(ins);
            
            for(UINT32 Oper = 0; Oper < numOperands; Oper++) {
                
                if(!INS_OperandWritten(ins, Oper) || !INS_OperandIsReg(ins,Oper))
                    continue;
                
                REG curReg = INS_OperandReg(ins,Oper);
                
                totBytes += REG_Size(curReg);
            }
        }
        
        if (BBL_InsTail(bbl) == BBL_InsHead(bbl)) {
            BBL_InsertCall(bbl,IPOINT_BEFORE,(AFUNPTR)UpdateAndCheck,IARG_UINT32, totInsInBbl, IARG_UINT32,totBytes, IARG_THREAD_ID,IARG_END);
        }else if(INS_IsIndirectBranchOrCall(BBL_InsTail(bbl))){
            BBL_InsertCall(bbl,IPOINT_BEFORE,(AFUNPTR)UpdateAndCheck,IARG_UINT32, totInsInBbl, IARG_UINT32,totBytes, IARG_THREAD_ID,IARG_END);
        }else{
            if (check) {
                BBL_InsertCall(bbl,IPOINT_BEFORE,(AFUNPTR)UpdateAndCheck,IARG_UINT32, totInsInBbl, IARG_UINT32, totBytes, IARG_THREAD_ID,IARG_END);
                check = false;
            } else {
                BBL_InsertCall(bbl,IPOINT_BEFORE,(AFUNPTR)Update,IARG_UINT32, totInsInBbl, IARG_UINT32, totBytes, IARG_THREAD_ID, IARG_END);
                check = true;
            }
        }
    }
}

#else

inline VOID Update(uint32_t bytes, THREADID threadId){
    RedSpyThreadData* const tData = ClientGetTLS(threadId);
    tData->bytesWritten += bytes;
}

//instrument the trace, count the number of ins in the trace, decide to instrument or not
static void InstrumentTrace(TRACE trace, void* f) {

    for (BBL bbl = TRACE_BblHead(trace); BBL_Valid(bbl); bbl = BBL_Next(bbl))
    {
        uint32_t totBytes = 0;
        for(INS ins = BBL_InsHead(bbl); INS_Valid(ins); ins = INS_Next(ins)) {
            if(INS_IsMemoryWrite(ins)) {
                totBytes += INS_MemoryWriteSize(ins);
            }
            UINT32 numOperands = INS_OperandCount(ins);
            
            for(UINT32 Oper = 0; Oper < numOperands; Oper++) {
                
                if(!INS_OperandWritten(ins, Oper) || !INS_OperandIsReg(ins,Oper))
                    continue;
                
                REG curReg = INS_OperandReg(ins,Oper);
                
                totBytes += REG_Size(curReg);
            }
        }
        BBL_InsertCall(bbl,IPOINT_BEFORE,(AFUNPTR)Update, IARG_UINT32, totBytes, IARG_THREAD_ID, IARG_END);
    }
}

#endif

struct RedundacyData {
    ContextHandle_t dead;
    ContextHandle_t kill;
    uint64_t frequency;
};

static inline bool RedundacyCompare(const struct RedundacyData &first, const struct RedundacyData &second) {
    return first.frequency > second.frequency ? true : false;
}

static void PrintRedundancyPairs(THREADID threadId) {
    vector<RedundacyData> tmpList;
    vector<RedundacyData>::iterator tmpIt;

    uint64_t grandTotalRedundantBytes = 0;
    fprintf(gTraceFile, "*************** Dump Data from Thread %d ****************\n", threadId);
    for (unordered_map<uint64_t, uint64_t>::iterator it = RedMap[threadId].begin(); it != RedMap[threadId].end(); ++it) {
        ContextHandle_t dead = DECODE_DEAD((*it).first);
        ContextHandle_t kill = DECODE_KILL((*it).first);

        for(tmpIt = tmpList.begin();tmpIt != tmpList.end(); ++tmpIt){
             bool ct1 = false;
             if(dead == 0 || ((*tmpIt).dead) == 0){
                  if(dead == 0 && ((*tmpIt).dead) == 0)
                       ct1 = true;
             }else{
                  ct1 = IsSameSourceLine(dead,(*tmpIt).dead);
             }
             bool ct2 = IsSameSourceLine(kill,(*tmpIt).kill);
             if(ct1 && ct2){
                  (*tmpIt).frequency += (*it).second;
                  grandTotalRedundantBytes += (*it).second;
                  break;
             }
        }
        if(tmpIt == tmpList.end()){
             RedundacyData tmp = { dead, kill, (*it).second};
             tmpList.push_back(tmp);
             grandTotalRedundantBytes += tmp.frequency;
        }
    }  
        
    fprintf(gTraceFile, "\n Total redundant bytes = %f %%\n", grandTotalRedundantBytes * 100.0 / ClientGetTLS(threadId)->bytesWritten);
    
    sort(tmpList.begin(), tmpList.end(), RedundacyCompare);
    vector<struct AnalyzedMetric_t>::iterator listIt;
    int cntxtNum = 0;
    for (vector<RedundacyData>::iterator listIt = tmpList.begin(); listIt != tmpList.end(); ++listIt) {
        if (cntxtNum < MAX_REDUNDANT_CONTEXTS_TO_LOG) {
            fprintf(gTraceFile, "\n======= (%f) %% ======\n", (*listIt).frequency * 100.0 / grandTotalRedundantBytes);
            if ((*listIt).dead == 0) {
                fprintf(gTraceFile, "\n Prepopulated with  by OS\n");
            } else {
                PrintFullCallingContext((*listIt).dead);
            }
            fprintf(gTraceFile, "\n---------------------Redundantly written by---------------------------\n");
            PrintFullCallingContext((*listIt).kill);
        }
        else {
            break;
        }
        cntxtNum++;
    }
}

// On each Unload of a loaded image, the accummulated redundancy information is dumped
static VOID ImageUnload(IMG img, VOID* v) {
    fprintf(gTraceFile, "\n TODO .. Multi-threading is not well supported.");    
    THREADID  threadid =  PIN_ThreadId();
    fprintf(gTraceFile, "\nUnloading %s", IMG_Name(img).c_str());
    // Update gTotalInstCount first
    PIN_LockClient();
    PrintRedundancyPairs(threadid);
    PIN_UnlockClient();
    // clear redmap now
    RedMap[threadid].clear();
}

static VOID ThreadFiniFunc(THREADID threadid, const CONTEXT *ctxt, INT32 code, VOID *v) {
}

static VOID FiniFunc(INT32 code, VOID *v) {
    // do whatever you want to the full CCT with footpirnt
}

static void InitThreadData(RedSpyThreadData* tdata){
    tdata->bytesWritten = 0;
    tdata->Sample_flag = true;
    tdata->NUM_INS = 0;
    tdata->NUM_winds = 0;
}

static VOID ThreadStart(THREADID threadid, CONTEXT* ctxt, INT32 flags, VOID* v) {
    RedSpyThreadData* tdata = new RedSpyThreadData();
    InitThreadData(tdata);
    //    __sync_fetch_and_add(&gClientNumThreads, 1);
#ifdef MULTI_THREADED
    PIN_SetThreadData(client_tls_key, tdata, threadid);
#else
    gSingleThreadedTData = tdata;
#endif
}


int main(int argc, char* argv[]) {
    // Initialize PIN
    if(PIN_Init(argc, argv))
        return Usage2();
    
    // Initialize Symbols, we need them to report functions and lines
    PIN_InitSymbols();
    
    // Init Client
    ClientInit(argc, argv);
    // Intialize CCTLib
    PinCCTLibInit(INTERESTING_INS_ALL, gTraceFile, InstrumentInsCallback, 0);
    
    
    // Obtain  a key for TLS storage.
    client_tls_key = PIN_CreateThreadDataKey(0 /*TODO have a destructir*/);
    // Register ThreadStart to be called when a thread starts.
    PIN_AddThreadStartFunction(ThreadStart, 0);
    
    
    // fini function for post-mortem analysis
    PIN_AddThreadFiniFunction(ThreadFiniFunc, 0);
    PIN_AddFiniFunction(FiniFunc, 0);

    TRACE_AddInstrumentFunction(InstrumentTrace, 0);
    
    // Register ImageUnload to be called when an image is unloaded
    IMG_AddUnloadFunction(ImageUnload, 0);
    
    // Launch program now
    PIN_StartProgram();
    return 0;
}


