#include "custom-instructions.h"
#include "runtime-dump.h"
#include "tci_analyzer.h"
#include "crete-debug.h"

#include <boost/serialization/split_member.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/unordered_set.hpp>
#include <boost/filesystem/operations.hpp>

#include <crete/custom_opcode.h>
#include <crete/debug_flags.h>

extern "C" {
#include "cpu.h"

extern CPUArchState *g_cpuState_bct;
void qemu_system_shutdown_request(void);
}

#include "tcg.h"

using namespace std;

// shared from runtime-dump.cpp
extern uint64_t g_crete_target_pid;
extern bool g_crete_is_valid_target_pid;

// TODO: xxx We may need this to capture the initial test case, when
//        there are more concolic variables than the ones listed
//        in crete.xml from guest
// static bool crete_flag_write_initial_input = false;
static const string crete_trace_ready_file_name = "trace_ready";

static boost::unordered_set<uint64_t> g_pc_exclude_filters;
static boost::unordered_set<uint64_t> g_pc_include_filters;

static uint64_t target_process_count = 0;
static set<string> concolics_names;

// CRETE_INSTR_SEND_TARGET_PID_VALUE
static inline void crete_custom_instr_sent_target_pid()
{
	g_crete_flags->set((uint64_t)g_cpuState_bct->cr[3]);

	g_crete_target_pid = g_cpuState_bct->cr[3];
	g_crete_is_valid_target_pid = true;
	++target_process_count;
}

// CRETE_INSTR_VOID_TARGET_PID_VALUE
static inline void crete_custom_instr_void_target_pid()
{
	g_crete_flags->reset();

    g_crete_is_valid_target_pid = false;
    runtime_env->handleCreteVoidTargetPid();
}

static inline void crete_custom_instr_check_target_pid()
{
    // Only process if it is from valid target pid while not processing interrupt
    if(g_crete_is_valid_target_pid &&
            ((uint64_t)g_cpuState_bct->cr[3] == g_crete_target_pid) &&
            !runtime_env->check_interrupt_process_info(0))
    {
        target_ulong addr = g_cpuState_bct->regs[R_EAX];
        uint8_t ret = 1;

        if(RuntimeEnv::access_guest_memory(g_cpuState_bct, addr, &ret, 1, 1) != 0) {
            cerr << "[CRETE ERROR] access_guest_memory() failed in crete_custom_instr_check_target_pid()\n";
            assert(0);
        }
    }
}

static string get_unique_name(const char *name)
{
    stringstream unique_name;
    unique_name << name << "_p" << target_process_count;
    string base_name = unique_name.str();

    unsigned count = 0;
    while(!concolics_names.insert(unique_name.str()).second) {
        unique_name.str(string());
        unique_name << base_name << "_" << ++count;
    }

    return unique_name.str();
}
#define MAX_CONCOLIC_NAME_SIZE 256
static char current_concolic_name[2*MAX_CONCOLIC_NAME_SIZE];

// CRETE_INSTR_SEND_CONCOLIC_NAME_VALUE
static inline void crete_custom_instr_send_concolic_name()
{
    assert(current_concolic_name[0] == '\0' &&
            "[CRETE ERROR] current_concolic_name should be reset to empty\n");

    target_ulong name_guest_addr = g_cpuState_bct->regs[R_EAX];
    target_ulong name_size = g_cpuState_bct->regs[R_ECX];

    assert(name_size > 0 && name_size < MAX_CONCOLIC_NAME_SIZE
            && "[CRETE ERROR] name size for concolic variable is bigger than MAX_CONCOLIC_NAME_SIZE\n");

    if(RuntimeEnv::access_guest_memory(g_cpuState_bct, name_guest_addr,
            (uint8_t *)current_concolic_name, name_size, 0) != 0) {

        cerr << "[CRETE ERROR] access_guest_memory() failed in crete_custom_instr_send_concolic_name()\n";
        assert(0);
    }

    current_concolic_name[name_size] = '\0';

    string unique_name = get_unique_name(current_concolic_name);
    if(strcmp(unique_name.c_str(), current_concolic_name))
    {
        uint64_t unique_name_size = unique_name.size();
        assert(unique_name_size > name_size);
        assert(unique_name_size < (2*MAX_CONCOLIC_NAME_SIZE-1));
        strncpy(current_concolic_name, unique_name.c_str(), unique_name_size);
        current_concolic_name[unique_name_size] = '\0';

        if(RuntimeEnv::access_guest_memory(g_cpuState_bct, name_guest_addr,
                (uint8_t *)current_concolic_name, unique_name_size+1, 1) != 0) {

            cerr << "[CRETE ERROR] access_guest_memory() failed in crete_custom_instr_send_concolic_name()\n";
            assert(0);
        }
    }
}

// CRETE_INSTR_PRE_MAKE_CONCOLIC_VALUE
static inline void crete_custom_instr_pre_make_concolic()
{
    assert(current_concolic_name[0] != '\0' &&
            "[CRETE ERROR] current_concolic_name is empty. "
            "It should have been set by crete_custom_instr_send_concolic_name\n");

    string concolic_name(current_concolic_name);
    memset(current_concolic_name, 0, 512);

    target_ulong guest_addr = g_cpuState_bct->regs[R_EAX];
    target_ulong size = g_cpuState_bct->regs[R_ECX];

    runtime_env->handlecreteMakeConcolic(concolic_name, guest_addr, size);
}

// CRETE_INSTR_KERNEL_OOPS_VALUE
// TODO: xxx make it better with keeping traces so far
// Report with prints and quit VM, from where vm-node should log the
// VM quit with related information and test case
static inline void crete_custom_instr_kernel_oops()
{
    fprintf(stderr, "[Potential Bugs] crete_kernel_oops()\n");
    qemu_system_shutdown_request();
}

// CRETE_INSTR_PRIME_VALUE:
// Reset flags and structs being used for testing an executable
static inline void crete_custom_instr_prime()
{
    g_pc_include_filters.clear();
    g_pc_exclude_filters.clear();

#if defined(CRETE_DBG_MEM)
    fprintf(stderr, "[CRETE_DBG_MEM] memory usage(crete_custom_instr_prime()) = %.3fMB\n",
            crete_mem_usage());
#endif
}

namespace fs = boost::filesystem;

// CRETE_INSTR_DUMP_VALUE
static inline void crete_tracing_finish()
{
	assert(rt_dump_tb_count != 0 && "[CRETE ERROR] Nothing is captured.\n");
    assert(runtime_env);

	// Waiting for vm_node
    while(fs::exists(crete_trace_ready_file_name))
        ; // Wait for it to not exist. FIXME: not efficient and can hang qume.

    // Writing trace to file
    runtime_env->writeRtEnvToFile();
    runtime_env->printInfo();

    fs::ofstream ofs(fs::path("hostfile") / crete_trace_ready_file_name);

    if(!ofs.good())
    {
        assert(0 && "can't write to crete_trace_ready_file_name");
    }
}

// CRETE_INSTR_DUMP_VALUE
// Reset flags and structs being used for tracing one concrete run of an executable
static inline void crete_tracing_reset()
{
    // Release
    crete_runtime_dump_close(); // Cleanup must happen before tb_flush (or crash occurs).
    tb_flush(g_cpuState_bct); // Flush tb cache, so references to runtime_env/tcg_llvm_ctx are destroyed.
    assert(!runtime_env && !g_crete_flags);

    // reset flags
    g_crete_is_valid_target_pid = false;
    g_crete_target_pid = 0;
    target_process_count = 0;
    concolics_names.clear();

    // Reacquire
    crete_runtime_dump_initialize();
    assert(runtime_env && g_crete_flags);
    crete_tci_next_iteration();
}

// CRETE_INSTR_EXCLUDE_FILTER_VALUE
static inline void crete_custom_instr_exclude_filter()
{
	// Note: using ecx/eax is safe for 64bit, as regs[] is defined as target_ulong, and there's no R_RCX/R_RAX
    target_ulong addr_begin = g_cpuState_bct->regs[R_EAX];
    target_ulong addr_end = g_cpuState_bct->regs[R_ECX];

    for(uint64_t i = addr_begin; i < addr_end; ++i) {
        g_pc_exclude_filters.insert(i);
    }
}

// CRETE_INSTR_INCLUDE_FILTER_VALUE
static inline void crete_custom_instr_inlude_filter()
{
	// Note: using ecx/eax is safe for 64bit, as regs[] is defined as target_ulong, and there's no R_RCX/R_RAX
    target_ulong addr_begin = g_cpuState_bct->regs[R_EAX];
    target_ulong addr_end = g_cpuState_bct->regs[R_ECX];

    for(uint64_t i = addr_begin; i < addr_end; ++i) {
        g_pc_include_filters.insert(i);
    }
}


// CRETE_INSTR_READ_PORT_VALUE
//NOTE: xxx check whether port is valid on client side
static void crete_custom_instr_read_port()
{
	target_ulong guest_addr = g_cpuState_bct->regs[R_EAX];
	target_ulong size = g_cpuState_bct->regs[R_ECX];

	unsigned short port = 0;

	const char* file_path = "hostfile/port";

	if(fs::exists(file_path))
	{
	    fs::ifstream ifs(file_path,
	            std::ios_base::in | std::ios_base::binary);

	    assert(ifs.good());

	    ifs >> port;
	}

	assert(size == sizeof(port));

	if(RuntimeEnv::access_guest_memory(g_cpuState_bct, guest_addr,
	        (uint8_t *)(&port), size, 1) != 0) {

	    cerr << "[CRETE ERROR] access_guest_memory() failed\n";
	    assert(0);
	}
}

void crete_custom_instruction_handler(uint64_t arg) {
	switch (arg) {
	case CRETE_INSTR_READ_PORT_VALUE:
	    crete_custom_instr_read_port();
	    break;

	case CRETE_INSTR_PRIME_VALUE:
	    crete_custom_instr_prime();
	    crete_tracing_reset();
	    break;

	case CRETE_INSTR_SEND_TARGET_PID_VALUE:
	    crete_custom_instr_sent_target_pid();
	    break;

	case CRETE_INSTR_VOID_TARGET_PID_VALUE:
	    crete_custom_instr_void_target_pid();
	    break;

	case CRETE_INSTR_CHECK_TARGET_PID_VALUE:
	    crete_custom_instr_check_target_pid();
	    break;

	case CRETE_INSTR_SEND_CONCOLIC_NAME_VALUE:
	    crete_custom_instr_send_concolic_name();
	    break;

	case CRETE_INSTR_PRE_MAKE_CONCOLIC_VALUE:
	    crete_custom_instr_pre_make_concolic();
	    break;

	case CRETE_INSTR_KERNEL_OOPS_VALUE:
	    crete_custom_instr_kernel_oops();
	    break;

	case CRETE_INSTR_DUMP_VALUE:
	    crete_tracing_finish();
	    crete_tracing_reset();
	    break;

	case CRETE_INSTR_EXCLUDE_FILTER_VALUE: // Exclude filter
	    crete_custom_instr_exclude_filter();
	    break;

	case CRETE_INSTR_INCLUDE_FILTER_VALUE: // Include filter
	    crete_custom_instr_inlude_filter();
	    break;

	case CRETE_INSTR_QUIT_VALUE:
	    qemu_system_shutdown_request();
	    break;
// Add new custom instruction handler here

	default:
	    assert(0 && "illegal operation: unsupported op code\n");
	}
}

int crete_is_pc_in_exclude_filter_range(uint64_t pc)
{
    boost::unordered_set<uint64_t>::const_iterator it = g_pc_exclude_filters.find(pc);
    if(it != g_pc_exclude_filters.end()){
        return 1;
    } else {
        return 0;
    }
}

int crete_is_pc_in_include_filter_range(uint64_t pc)
{
    boost::unordered_set<uint64_t>::const_iterator it = g_pc_include_filters.find(pc);
    if(it != g_pc_include_filters.end()){
        return 1;
    } else {
        return 0;
    }
}

struct PIDWriter
{
    PIDWriter()
    {
        fs::path dir = "hostfile";

        if(!fs::exists(dir))
        {
        fs::create_directories(dir);
        }

        fs::path p = dir / "pid";

        if(fs::exists(p))
        {
            fs::remove(p);
        }

        fs::ofstream ofs(p,
                 ios_base::binary | ios_base::out);

        if(!ofs.good())
        {
            assert(0 && "Could not open hostfile/pid for writing");
        }

        ofs << ::getpid();
    }
} pid_writer; // Ctor writes PID when program starts.
