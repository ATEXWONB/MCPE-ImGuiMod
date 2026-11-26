#include "Patcher.h"
#include "Il2cpp/il2cpp-class.h"
#include "KittyMemory/KittyMemory.h"

using namespace asmjit;

#ifdef __DEBUG__
    #define ASSERR(error)                                                                                              \
        do                                                                                                             \
        {                                                                                                              \
            if (error != kErrorOk)                                                                                     \
                LOGE("ASSERR: %s => %s", #error, asmjit::DebugUtils::errorAsString(error));                            \
        } while (0)
#else
    #define ASSERR(error) (error)
#endif

Patcher::Patcher(MethodInfo *method) : target{method->methodPointer}
{
    using namespace asmjit;
#ifdef __aarch64__
    code.init(Environment(Arch::kAArch64));
#else
    code.init(Environment(Arch::kARM));
#endif
    code.attach(&assembler);
}

Patcher::Patcher(void* target_t) : target{target_t}
{
    using namespace asmjit;
#ifdef __aarch64__
    code.init(Environment(Arch::kAArch64));
#else
    code.init(Environment(Arch::kARM));
#endif
    code.attach(&assembler);
}

Error Patcher::ret()
{
#ifdef __aarch64__
    ASSERR(assembler.ret(asmjit::a64::x30));
#else
    ASSERR(assembler.bx(asmjit::a32::lr));
#endif
    return asmjit::kErrorOk;
}

Error Patcher::movInt16(int16_t value)
{
#ifdef __aarch64__
    ASSERR(assembler.movz(a64::x0, value));
#else
    ASSERR(assembler.movw(a32::r0, value));
#endif
    return asmjit::kErrorOk;
}

Error Patcher::movUInt16(uint16_t value)
{
#ifdef __aarch64__
    ASSERR(assembler.movz(a64::x0, value));
#else
    ASSERR(assembler.movw(a32::r0, value));
#endif
    return asmjit::kErrorOk;
}

asmjit::Error Patcher::movInt32(int32_t value)
{
    uint16_t lowerBit = static_cast<uint16_t>(value);
    uint16_t higherBit = static_cast<uint16_t>(value >> 16);
#ifdef __aarch64__
    ASSERR(assembler.mov(a64::w0, lowerBit));
    ASSERR(assembler.movk(a64::w0, higherBit, a64::lsl(16)));
#else
    ASSERR(assembler.movw(a32::r0, lowerBit));
    ASSERR(assembler.movt(a32::r0, higherBit));
#endif
    return asmjit::kErrorOk;
}
asmjit::Error Patcher::movUInt32(uint32_t value)
{
    uint16_t lowerBit = static_cast<uint16_t>(value);
    uint16_t higherBit = static_cast<uint16_t>(value >> 16);
#ifdef __aarch64__
    ASSERR(assembler.mov(a64::w0, lowerBit));
    ASSERR(assembler.movk(a64::w0, higherBit, a64::lsl(16)));
#else
    ASSERR(assembler.movw(a32::r0, lowerBit));
    ASSERR(assembler.movt(a32::r0, higherBit));
#endif
    return asmjit::kErrorOk;
}

asmjit::Error Patcher::movInt64(int64_t value)
{
    uint16_t lowerBit = static_cast<uint16_t>(value);
    uint16_t higherBit = static_cast<uint16_t>(value >> 16);
#ifdef __aarch64__
    ASSERR(assembler.mov(a64::w0, lowerBit));
    ASSERR(assembler.movk(a64::w0, higherBit, a64::lsl(16)));
#else
    ASSERR(assembler.movw(a32::r0, lowerBit));
    ASSERR(assembler.movt(a32::r0, higherBit));
#endif
    return asmjit::kErrorOk;
}
asmjit::Error Patcher::movUInt64(uint64_t value)
{
    uint16_t lowerBit = static_cast<uint16_t>(value);
    uint16_t higherBit = static_cast<uint16_t>(value >> 16);
#ifdef __aarch64__
    ASSERR(assembler.mov(a64::w0, lowerBit));
    ASSERR(assembler.movk(a64::w0, higherBit, a64::lsl(16)));
#else
    ASSERR(assembler.movw(a32::r0, lowerBit));
    ASSERR(assembler.movt(a32::r0, higherBit));
#endif
    return asmjit::kErrorOk;
}

asmjit::Error Patcher::movFloat(float value)
{
    union FloatBits
    {
        float f;
        uint32_t i;
    };
    FloatBits fb{value};

    uint16_t lowerBit = static_cast<uint16_t>(fb.i);
    uint16_t higherBit = static_cast<uint16_t>(fb.i >> 16);
#ifdef __aarch64__
    ASSERR(assembler.mov(a64::w0, lowerBit));
    ASSERR(assembler.movk(a64::w0, higherBit, a64::lsl(16)));
    ASSERR(assembler.fmov(a64::s0, a64::w0));
#else
    ASSERR(assembler.movw(a32::r0, lowerBit));
    ASSERR(assembler.movt(a32::r0, higherBit));
#endif
    return asmjit::kErrorOk;
}

asmjit::Error Patcher::movBool(bool value)
{
    int b = value ? 1 : 0;
#ifdef __aarch64__
    ASSERR(assembler.mov(a64::x0, b));
#else
    ASSERR(assembler.mov(a32::r0, b));
#endif
    return asmjit::kErrorOk;
}

asmjit::Error Patcher::movPtr(void *value)
{
#ifdef __aarch64__
    auto val = imm(value);
    ASSERR(assembler.mov(a64::x0, val));
#else
    uint16_t lowerBit = static_cast<uint16_t>((uintptr_t)value);
    uint16_t higherBit = static_cast<uint16_t>((uintptr_t)value >> 16);
    ASSERR(assembler.movw(a32::r0, lowerBit));
    ASSERR(assembler.movt(a32::r0, higherBit));
#endif
    return asmjit::kErrorOk;
}

std::vector<uint8_t> Patcher::patch()
{
    std::vector<char> bytes;
    
    // 1. 收集生成的机器代码
    for (auto s : code.sections())
    {
        for (auto c : s->buffer())
        {
            LOGD("%02X", c);
            bytes.push_back(c);
        }
    }

    // 检查是否生成了代码
    if (bytes.empty()) {
        LOGE("No code generated for patch");
        return {};
    }

    auto rawBytes = bytes.data();
    
    // 2. 修改内存保护
    auto protect = KittyMemory::ProtectAddr(target, bytes.size(), PROT_READ | PROT_WRITE | PROT_EXEC);
    if (!protect) {
        LOGE("Failed to change memory protection for patching");
        return {};
    }
    LOGINT(protect);

    // 3. 保存原始字节到成员变量（不是局部变量！）
    originalBytes.assign((uint8_t *)target, (uint8_t *)target + bytes.size());
    
    // 调试输出原始字节
    LOGD("Original bytes:");
    for (auto b : originalBytes)
    {
        LOGD("%02X", b);
    }

    // 4. 应用补丁
    memcpy(target, (void *)rawBytes, bytes.size());
    
    // 5. 刷新指令缓存（重要！）
#ifdef __aarch64__
    __builtin___clear_cache((char*)target, (char*)target + bytes.size());
#else

#endif
    
    LOGD("Patch applied successfully");
    return originalBytes; // 返回原始字节副本
}

bool Patcher::restore(const std::vector<uint8_t>& originalBytes_t)
{
    if (originalBytes_t.empty()) {
        LOGE("No original bytes provided for restore");
        return false;
    }

    // 修改内存保护
    if (!KittyMemory::ProtectAddr(target, originalBytes_t.size(), PROT_READ | PROT_WRITE | PROT_EXEC)) {
        LOGE("Failed to change memory protection for restore");
        return false;
    }

    // 恢复原始字节
    memcpy(target, originalBytes_t.data(), originalBytes_t.size());
    originalBytes.clear();
    // 刷新指令缓存
#ifdef __aarch64__
    __builtin___clear_cache((char*)target, (char*)target + originalBytes_t.size());
#else
    // cacheflush((long)target, (long)target + originalBytes.size(), 0);
#endif
    
    LOGD("Patch restored successfully using provided bytes");
    return true;
}

bool Patcher::restore()
{
    if (originalBytes.empty()) {
        LOGE("No saved original bytes for restore");
        return false;
    }
    return restore(originalBytes);
}