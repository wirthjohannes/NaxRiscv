#include <stdint.h>
#include <string>
#include <memory>
#include <jni.h>
#include <iostream>
#include <queue>
#include <sstream>

#include "processor.h"
#include "mmu.h"
#include "simif.h"
#include "type.h"
#include "memory.h"
#include "elf.h"

using namespace std;



#include <stdio.h>
#include <stdint.h>



#define API __attribute__((visibility("default")))

#define failure() exit(1)
void breakMe(){
    int a = 0;
}

#define assertEq(message, x,ref) if((x) != (ref)) {\
    cout << hex << "\n*** " << message << " DUT=" << x << " REF=" << ref << " ***\n\n" << dec;\
    breakMe();\
    failure();\
}

#define assertTrue(message, x) if(!(x)) {\
    printf("\n*** %s ***\n\n",message);\
    breakMe();\
    failure();\
}


class TraceIo{
public:
    bool write;
    u64 address;
    u64 data;
    u32 mask;
    u32 size;
    bool error;

    TraceIo(std::istringstream &f){
        f >> write >> hex >> address >> data >> mask >> dec >> size >> error;
    }
};


class SpikeIf : public simif_t{
public:
    Memory *memory;
    queue <TraceIo> ioQueue;

    SpikeIf(Memory *memory){
        this->memory = memory;
    }

    // should return NULL for MMIO addresses
    virtual char* addr_to_mem(reg_t addr)  {
//        if((addr & 0xE0000000) == 0x00000000) return NULL;
//        printf("addr_to_mem %lx ", addr);
//        return (char*) memory->get(addr);
        return NULL;
    }
    // used for MMIO addresses
    virtual bool mmio_load(reg_t addr, size_t len, u8* bytes)  {
        if((addr & 0xE0000000) != 0x00000000) {
            memory->read(addr, len, bytes);
            return true;
        }
        printf("mmio_load %lx %ld\n", addr, len);
        if(addr < 0x10000000 || addr > 0x20000000) return false;
        assertTrue("missing mmio\n", !ioQueue.empty());
        auto dut = ioQueue.front();
        assertEq("mmio write\n", dut.write, false);
        assertEq("mmio address\n", dut.address, addr);
        assertEq("mmio len\n", dut.size, len);
        memcpy(bytes, (u8*)&dut.data, len);
        ioQueue.pop();
        return !dut.error;
    }
    virtual bool mmio_store(reg_t addr, size_t len, const u8* bytes)  {
        if((addr & 0xE0000000) != 0x00000000) {
            memory->write(addr, len, (u8*) bytes);
            return true;
        }
        printf("mmio_store %lx %ld\n", addr, len);
        if(addr < 0x10000000 || addr > 0x20000000) return false;
        assertTrue("missing mmio\n", !ioQueue.empty());
        auto dut = ioQueue.front();
        assertEq("mmio write\n", dut.write, true);
        assertEq("mmio address\n", dut.address, addr);
        assertEq("mmio len\n", dut.size, len);
        assertTrue("mmio data\n", !memcmp((u8*)&dut.data, bytes, len));
        ioQueue.pop();
        return !dut.error;
    }
    // Callback for processors to let the simulation know they were reset.
    virtual void proc_reset(unsigned id)  {
//        printf("proc_reset %d\n", id);
    }

    virtual const char* get_symbol(uint64_t addr)  {
//        printf("get_symbol %lx\n", addr);
        return NULL;
    }
};

class Hart{
public:
    SpikeIf *sif;
    processor_t *proc;
    state_t *state;

    bool integerWriteValid;
    u64 integerWriteData;

    u32 csrAddress;
    bool csrWrite;
    bool csrRead;
    u64 csrWriteData;
    u64 csrReadData;


    Hart(u32 hartId, string isa, string priv, Memory *memory){
        sif = new SpikeIf(memory);
        FILE *fptr = 1 ? fopen(string("spike.log").c_str(),"w") : NULL;
        std::ofstream outfile ("/dev/null",std::ofstream::binary);
        proc = new processor_t(isa.c_str(), priv.c_str(), "", sif, hartId, false, fptr, outfile);
        proc->enable_log_commits();
        state = proc->get_state();
    }

    void setPc(u64 pc){
        state->pc = pc;
    }

    void writeRf(u32 rfKind, u32 address, u64 data){
        switch(rfKind){
        case 0:
            integerWriteValid = true;
            integerWriteData = data;
            break;
        case 4:
            if((csrWrite || csrRead) && csrAddress != address){
                printf("duplicated CSR access \n");
                failure();
            }
            csrAddress = address;
            csrWrite = true;
            csrWriteData = data;
            break;
        default:
            printf("??? unknown RF trace \n");
            failure();
            break;
        }

    }


    void readRf(u32 rfKind, u32 address, u64 data){
        switch(rfKind){
        case 4:
            if((csrWrite || csrRead) && csrAddress != address){
                printf("duplicated CSR access \n");
                failure();
            }
            csrAddress = address;
            csrRead = true;
            csrReadData = data;
            break;
        default:
            printf("??? unknown RF trace \n");
            failure();
            break;
        }

    }


    void commit(u64 pc){
        if(pc != state->pc){
            printf("PC MISSMATCH dut=%lx ref=%lx", pc, state->pc);
            failure();
        }
        proc->step(1);
        for (auto item : state->log_reg_write) {
            if (item.first == 0)
              continue;

            u32 rd = item.first >> 4;
            switch (item.first & 0xf) {
            case 0: { //integer
                assertTrue("INTEGER WRITE MISSING", integerWriteValid);
                assertEq("INTEGER WRITE MISSMATCH", integerWriteData, item.second.v[0]);
                integerWriteValid = false;
            } break;
            case 4:{ //CSR
                u64 inst = state->last_inst.bits();
                switch(inst){
                case 0x30200073: //MRET
                case 0x10200073: //SRET
                case 0x00200073: //URET
                    break;
                default:
                    if((inst & 0x7F) == 0x73 && (inst & 0x3000) != 0){
                        assertTrue("CSR WRITE MISSING", csrWrite);
                        assertEq("CSR WRITE ADDRESS", (u32)(csrAddress & 0xCFF), (u32)(rd & 0xCFF));
//                                                assertEq("CSR WRITE DATA", whitebox->robCtx[robId].csrWriteData, item.second.v[0]);
                    }
                    break;
                }
                csrWrite = false;
                csrRead = false;
            } break;
            default: {
                printf("??? unknown spike trace %lx\n", item.first & 0xf);
                failure();
            } break;
            }
        }
        assertTrue("INTEGER WRITE SPAWNED", !integerWriteValid);
        printf("%016lx\n", pc);
    }

    void ioAccess(TraceIo io){
        sif->ioQueue.push(io);
    }

};

class Context{
public:
    Memory memory;
    vector<Hart*> harts;

    void loadElf(string path, u64 offset){
        auto elf = new Elf(path.c_str());
        elf->visitBytes([&](u8 data, u64 address) {
            memory.write(address+offset, 1, &data);
        });
    }

    void rvNew(u32 hartId, string isa, string priv){
        harts.resize(max((size_t)(hartId+1), harts.size()));
        harts[hartId] = new Hart(hartId, isa, priv, &memory);
    }
};


#ifdef __cplusplus
extern "C" {
#endif
//jclass userDataClass;
//jmethodID methodId;
//
//JNIEXPORT jlong JNICALL Java_riscv_model_Model_newModel
//  (JNIEnv * env, jobject obj){
//
//    auto * model = new Model();
//    return (jlong)model;
//}
//
//JNIEXPORT void JNICALL Java_riscv_model_Model_deleteModel
//  (JNIEnv * env, jobject obj, jlong model){
//
//    delete (Model*)model;
//}


#ifdef __cplusplus
}
#endif




void checkFile(std::ifstream &lines){
    Context context;
#define rv context.harts[hartId]
    std::string line;
    while (getline(lines, line)){
        istringstream f(line);
        string str;
        f >> str;
        if(str == "rv"){
            f >> str;
            if (str == "commit") {
                u32 hartId;
                u64 pc;
                f >> hartId >> hex >> pc >> dec;
                rv->commit(pc);
            } else if (str == "rf") {
                f >> str;
                if(str == "w") {
                    u32 hartId, rfKind, address;
                    u64 data;
                    f >> hartId >> rfKind >> address >> hex >> data >> dec;
                    rv->writeRf(rfKind, address, data);
                } else if(str == "r") {
                    u32 hartId, rfKind, address;
                    u64 data;
                    f >> hartId >> rfKind >> address >> hex >> data >> dec;
                    rv->readRf(rfKind, address, data);
                } else {
                    throw runtime_error(line);
                }

            } else if (str == "io") {
                u32 hartId;
                f >> hartId;
                auto io = TraceIo(f);
                rv->ioAccess(io);
            } else if (str == "set") {
                f >> str;
                if(str == "pc"){
                    u32 hartId;
                    u64 pc;
                    f >> hartId >> hex >> pc >> dec;
                    rv->setPc(pc);
                } else {
                    throw runtime_error(line);
                }
            } else if(str == "new"){
                u32 hartId;
                string isa, priv;
                f >> hartId >> isa >> priv;
                context.rvNew(hartId, isa, priv);
            } else {
                throw runtime_error(line);
            }
        } else if(str == "elf"){
            f >> str;
            if(str == "load"){
                string path;
                u64 offset;
                f >> path >> hex >> offset >> dec;
                context.loadElf(path, offset);
            } else {
                throw runtime_error(line);
            }
        } else {
            throw runtime_error(line);
        }
    }

    cout << "Model check Success <3" << endl;
}


int main(){
    cout << "miaou3" << endl;
    std::ifstream f("/media/data/open/riscv/VexRiscvOoo/trace.txt");
    checkFile(f);
}

