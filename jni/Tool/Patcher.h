#pragma once
#include "asmjit.h"
#include <vector>
#ifdef __aarch64__
    #include "a64.h"
#else
    #include "a32.h"
#endif

struct MethodInfo;

class Patcher
{
public:
    Patcher(MethodInfo *method);
    Patcher(void* target_t);
    asmjit::Error ret();
    asmjit::Error movInt16(int16_t value);
    asmjit::Error movUInt16(uint16_t value);

    asmjit::Error movInt32(int32_t value);
    asmjit::Error movUInt32(uint32_t value);

    // FIXME: currently handled like 32 bit number so it wouldn't work on 64 bit number
    asmjit::Error movInt64(int64_t value);
    asmjit::Error movUInt64(uint64_t value);

    asmjit::Error movFloat(float value);

    asmjit::Error movBool(bool value);

    asmjit::Error movPtr(void *value);

    std::vector<uint8_t> patch();
    
    // 新增的恢复函数
    bool restore(const std::vector<uint8_t>& originalBytes_t);
    bool restore(); // 使用内部保存的原始字节恢复
    std::vector<uint8_t> originalBytes; // 保存原始字节用于恢复
private:
    asmjit::CodeHolder code;
#ifdef __aarch64__
    asmjit::a64::Assembler assembler;
#else
    asmjit::a32::Assembler assembler;
#endif
    void *target;
};