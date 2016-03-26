#include "../HalideRuntime.h"
#include "HexagonWrapper.h"
#include "rpc_protocol.h"

#include <vector>
#include <cassert>

typedef unsigned int handle_t;

HexagonWrapper *sim = NULL;

int init_sim() {
    if (sim) return 0;

    sim = new HexagonWrapper(HEX_CPU_V60);

    HEXAPI_Status status = sim->ConfigureExecutableBinary("libhalide_simulator_remote.so");
    if (status != HEX_STAT_SUCCESS) {
        printf("HexagonWrapper::ConfigureExecutableBinary failed: %d", status);
        return -1;
    }

    status = sim->EndOfConfiguration();
    if (status != HEX_STAT_SUCCESS) {
        printf("HexagonWrapper::EndOfConfiguration failed: %d", status);
        return -1;
    }

    return 0;
}

int write_memory(int dest, const void *src, int size) {
    assert(sim);

    while (size > 0) {
        int next = std::min(size, 8);
        HEXAPI_Status status = sim->WriteVirtual(dest, 0xFFFFFFFF, next,
                                                 *reinterpret_cast<const HEX_8u_t*>(src));
        if (status != HEX_STAT_SUCCESS) {
            printf("HexagonWrapper::WriteVirtual failed: %d", status);
            return -1;
        }

        size -= next;
        dest += next;
        src = reinterpret_cast<const char *>(src) + next;
    }
    return 0;
}

int read_memory(void *dest, int src, int size) {
    assert(sim);

    while (size > 0) {
        int next = std::min(size, 8);
        HEXAPI_Status status = sim->ReadVirtual(src, 0xFFFFFFFF, next, dest);
        if (status != HEX_STAT_SUCCESS) {
            printf("HexagonWrapper::WriteVirtual failed: %d", status);
            return -1;
        }

        size -= next;
        src += next;
        dest = reinterpret_cast<char *>(dest) + next;
    }
    return 0;
}

int send_message(int msg, const std::vector<int> &arguments) {
    assert(sim);

    HEXAPI_Status status;

    HEX_4u_t remote_msg, remote_args, remote_ret;

    status = sim->ReadSymbolValue("rpc_call", &remote_msg);
    if (status != HEX_STAT_SUCCESS) {
        printf("HexagonWrapper::ReadSymbolValue(rpcmsg) failed: %d", status);
        return -1;
    }
    status = sim->ReadSymbolValue("rpc_args", &remote_args);
    if (status != HEX_STAT_SUCCESS) {
        printf("HexagonWrapper::ReadSymbolValue(rpcmsg) failed: %d", status);
        return -1;
    }
    status = sim->ReadSymbolValue("rpc_ret", &remote_ret);
    if (status != HEX_STAT_SUCCESS) {
        printf("HexagonWrapper::ReadSymbolValue(rpcmsg) failed: %d", status);
        return -1;
    }

    // Set the message and arguments.
    if (0 != write_memory(remote_msg, &msg, 4)) { return -1; }
    if (0 != write_memory(remote_args, &arguments[0], arguments.size() * 4)) { return -1; }

    HEXAPI_CoreState state;
    if (msg == Message::Break) {
        HEX_4u_t result;
        state = sim->Run(&result);
        if (state != HEX_CORE_FINISHED) {
            printf("HexagonWrapper::Run failed: %d", state);
            return -1;
        }
        return 0;
    } else {
        do {
            HEX_4u_t cycles;
            state = sim->StepTime(100, HEX_MILLISEC, &cycles);
            read_memory(&msg, remote_msg, 4);
            if (msg == Message::None) {
                HEX_4u_t ret = 0;
                read_memory(&ret, remote_ret, 4);
                return ret;
            }
        } while (state == HEX_CORE_SUCCESS);
        printf("HexagonWrapper::StepTime failed: %d", state);
        return -1;
    }
}

struct host_buffer {
    unsigned char *data;
    int dataLen;
};

class remote_buffer {
public:
    int data;
    int dataLen;

    remote_buffer() : data(0), dataLen(0) {}
    remote_buffer(int dataLen) : dataLen(dataLen) {
        data = send_message(Message::Alloc, {dataLen});
    }
    remote_buffer(const void *data, int dataLen) : remote_buffer(dataLen) {
        write_memory(this->data, data, dataLen);
    }
    remote_buffer(const host_buffer &host_buf) : remote_buffer(host_buf.data, host_buf.dataLen) {}

    ~remote_buffer() {
        if (data != 0) {
            send_message(Message::Free, {data});
        }
    }

    // Enable usage with std::vector.
    remote_buffer(remote_buffer &&move) : remote_buffer() {
        std::swap(data, move.data);
        std::swap(dataLen, move.dataLen);
    }
    remote_buffer &operator = (remote_buffer &&move) {
        std::swap(data, move.data);
        std::swap(dataLen, move.dataLen);
        return *this;
    }

    remote_buffer(const remote_buffer &) = delete;
    remote_buffer &operator = (const remote_buffer &) = delete;
};

extern "C" {

int halide_hexagon_remote_initialize_kernels(const unsigned char *code, int codeLen,
                                             handle_t *module_ptr) {
    int ret = init_sim();
    if (ret != 0) return -1;

    // Copy the pointer arguments to the simulator.
    remote_buffer remote_code(code, codeLen);
    remote_buffer remote_module_ptr(module_ptr, 4);

    // Run the init kernels command.
    ret = send_message(Message::InitKernels, {remote_code.data, codeLen, remote_module_ptr.data});

    // Get the module ptr.
    read_memory(module_ptr, remote_module_ptr.data, 4);

    return ret;
}

handle_t halide_hexagon_remote_get_symbol(handle_t module_ptr, const char* name, int nameLen) {
    assert(sim);

    // Copy the pointer arguments to the simulator.
    remote_buffer remote_name(name, nameLen);

    // Run the init kernels command.
    handle_t ret = send_message(Message::GetSymbol, {static_cast<int>(module_ptr), remote_name.data, nameLen});

    return ret;
}

int halide_hexagon_remote_run(handle_t module_ptr, handle_t function,
                              const host_buffer *input_buffersPtrs, int input_buffersLen,
                              const host_buffer *input_scalarsPtrs, int input_scalarsLen,
                              host_buffer *output_buffersPtrs, int output_buffersLen) {
    assert(sim);

    std::vector<remote_buffer> remote_input_buffers;
    std::vector<remote_buffer> remote_input_scalars;
    std::vector<remote_buffer> remote_output_buffers;

    for (int i = 0; i < input_buffersLen; i++)
        remote_input_buffers.emplace_back(input_buffersPtrs[i]);
    for (int i = 0; i < input_scalarsLen; i++)
        remote_input_scalars.emplace_back(input_scalarsPtrs[i]);
    for (int i = 0; i < output_buffersLen; i++)
        remote_output_buffers.emplace_back(output_buffersPtrs[i]);

    // Copy the pointer arguments to the simulator.
    remote_buffer remote_input_buffersPtrs(&remote_input_buffers[0], input_buffersLen * sizeof(remote_buffer));
    remote_buffer remote_input_scalarsPtrs(&remote_input_scalars[0], input_scalarsLen * sizeof(remote_buffer));
    remote_buffer remote_output_buffersPtrs(&remote_output_buffers[0], output_buffersLen * sizeof(remote_buffer));

    // Run the init kernels command.
    int ret = send_message(
        Message::Run,
        {static_cast<int>(module_ptr), static_cast<int>(function),
         remote_input_buffersPtrs.data, input_buffersLen,
         remote_input_scalarsPtrs.data, input_scalarsLen,
         remote_output_buffersPtrs.data, output_buffersLen});

    return ret;
}

int halide_hexagon_remote_release_kernels(handle_t module_ptr, int codeLen) {
    return send_message(Message::ReleaseKernels, {static_cast<int>(module_ptr), codeLen});
}

}  // extern "C"