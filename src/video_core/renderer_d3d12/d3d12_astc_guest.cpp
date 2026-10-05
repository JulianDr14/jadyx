// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstring>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <unordered_map>
#define SPV_ENABLE_UTILITY_CODE
#include <spirv/unified1/spirv.hpp11>
#include "common/logging.h"
#include "video_core/host_shaders/d3d12_astc_guest_comp_spv.h"
#include "video_core/host_shaders/d3d12_astc_guest_frag_spv.h"
#include "video_core/renderer_d3d12/d3d12_astc_guest.h"

namespace D3D12 {
namespace {
std::atomic<bool> enabled{};
using Inst = std::vector<u32>;
using Instructions = std::vector<Inst>;
u32 Opcode(const Inst& i) {
    return i[0] & 65535u;
}
u32 Result(const Inst& i) {
    bool result{}, type{};
    spv::HasResultAndType(static_cast<spv::Op>(Opcode(i)), &result, &type);
    return result ? i[type ? 2 : 1] : 0;
}
Instructions Parse(std::span<const u32> words) {
    if (words.size() < 5 || words[0] != 0x07230203u)
        throw std::runtime_error("ASTC guest invalid SPIR-V");
    Instructions out;
    for (size_t p = 5; p < words.size();) {
        const size_t n = words[p] >> 16;
        if (!n || p + n > words.size())
            throw std::runtime_error("ASTC guest malformed instruction");
        out.emplace_back(words.begin() + p, words.begin() + p + n);
        p += n;
    }
    return out;
}
std::string String(const Inst& i, size_t first) {
    const char* p = reinterpret_cast<const char*>(i.data() + first);
    const size_t max = (i.size() - first) * 4;
    const auto* end = static_cast<const char*>(std::memchr(p, 0, max));
    if (!end)
        throw std::runtime_error("ASTC guest unterminated string");
    return {p, end};
}
Inst Make(spv::Op op, std::initializer_list<u32> operands) {
    Inst out{(static_cast<u32>(operands.size() + 1) << 16) | static_cast<u32>(op)};
    out.insert(out.end(), operands);
    return out;
}
bool IsType(u32 op) {
    return op >= static_cast<u32>(spv::Op::OpTypeVoid) &&
           op <= static_cast<u32>(spv::Op::OpTypeForwardPointer);
}
bool IsDeclaration(u32 op) {
    return IsType(op) || (op >= 41 && op <= 52) || op == static_cast<u32>(spv::Op::OpVariable);
}
bool Deduplicate(u32 op) {
    return IsType(op) && op != static_cast<u32>(spv::Op::OpTypeArray) &&
           op != static_cast<u32>(spv::Op::OpTypeRuntimeArray) &&
           op != static_cast<u32>(spv::Op::OpTypeStruct);
}
bool IsImageOperation(u32 op) {
    return (op >= 87 && op <= 107 && op != 100) || (op >= 305 && op <= 320);
}
struct Library {
    Instructions capabilities, imports, debug, annotations, declarations, functions;
    std::map<std::string, u32> names;
    u32 function{}, sampler_pointer{}, float_type{}, float4{}, uint_type{}, uint2{}, uint3{},
        uint4{};
};
Library Import(Instructions& guest, std::span<const u32> source, std::span<const u32> ids,
               u32& bound) {
    auto original = Parse(source);
    std::map<u32, std::string> old_names;
    u32 main{};
    for (const auto& i : original) {
        if (Opcode(i) == static_cast<u32>(spv::Op::OpName))
            old_names[i[1]] = String(i, 2);
        if (Opcode(i) == static_cast<u32>(spv::Op::OpEntryPoint))
            main = i[2];
    }
    std::vector<u32> map(source[3]);
    for (u32 i = 1; i < map.size(); ++i)
        map[i] = bound++;
    std::map<Inst, u32> types;
    std::map<std::string, u32> ext;
    for (const auto& i : guest) {
        if (Deduplicate(Opcode(i))) {
            Inst key = i;
            key[1] = 0;
            types[key] = i[1];
        }
        if (Opcode(i) == static_cast<u32>(spv::Op::OpExtInstImport))
            ext[String(i, 2)] = i[1];
    }
    std::unordered_set<u32> skip;
    // Relocate declarations in dependency order, coalescing basic types with the guest module.
    size_t offset = 5, relocation = 0;
    for (const auto& i : original) {
        Inst next = i;
        while (relocation < ids.size() && ids[relocation] < offset + i.size()) {
            const size_t word = ids[relocation++] - offset;
            if (word >= next.size() || next[word] >= map.size())
                throw std::runtime_error("ASTC guest bad relocation");
            next[word] = map[next[word]];
        }
        const u32 op = Opcode(i);
        if (Deduplicate(op)) {
            Inst key = next;
            key[1] = 0;
            if (auto found = types.find(key); found != types.end()) {
                map[i[1]] = found->second;
                skip.insert(i[1]);
            } else
                types[key] = next[1];
        } else if (op == static_cast<u32>(spv::Op::OpExtInstImport)) {
            const std::string name = String(i, 2);
            if (auto found = ext.find(name); found != ext.end()) {
                map[i[1]] = found->second;
                skip.insert(i[1]);
            } else
                ext[name] = next[1];
        }
        offset += i.size();
    }
    Library out;
    bool in_function = false, dummy_function = false;
    offset = 5;
    relocation = 0;
    for (const auto& i : original) {
        Inst next = i;
        while (relocation < ids.size() && ids[relocation] < offset + i.size()) {
            const size_t word = ids[relocation++] - offset;
            next[word] = map[next[word]];
        }
        offset += i.size();
        const u32 op = Opcode(i), result = Result(i);
        if (op == static_cast<u32>(spv::Op::OpFunction)) {
            in_function = true;
            dummy_function = i[2] == main;
        }
        const bool dummy =
            result && old_names.contains(result) && old_names[result].starts_with("guest_dummy");
        if (in_function) {
            if (!dummy_function)
                out.functions.push_back(std::move(next));
            if (op == static_cast<u32>(spv::Op::OpFunctionEnd))
                in_function = false;
            continue;
        }
        if (dummy || (result && skip.contains(result)))
            continue;
        if (op == static_cast<u32>(spv::Op::OpCapability)) {
            if (std::find(guest.begin(), guest.end(), i) == guest.end())
                out.capabilities.push_back(next);
        } else if (op == static_cast<u32>(spv::Op::OpExtInstImport))
            out.imports.push_back(next);
        else if (op == static_cast<u32>(spv::Op::OpDecorate) ||
                 op == static_cast<u32>(spv::Op::OpMemberDecorate)) {
            if (old_names.contains(i[1]) && old_names[i[1]].starts_with("guest_dummy"))
                continue;
            // A library's gl_WorkGroupSize constant overrides the entry point's LocalSize in
            // Mesa. Only the guest entry point may define its execution geometry.
            if (op == static_cast<u32>(spv::Op::OpDecorate) &&
                i[2] == static_cast<u32>(spv::Decoration::BuiltIn) &&
                i[3] == static_cast<u32>(spv::BuiltIn::WorkgroupSize))
                continue;
            out.annotations.push_back(next);
        } else if (IsDeclaration(op))
            out.declarations.push_back(next);
        // Entry point, debug/source records and execution modes belong to the guest, not library.
    }
    for (const auto& [id, name] : old_names) {
        if (name.starts_with("eden_astc_") || name == "guest_draw")
            out.names[name] = map[id];
        if (name.starts_with("EdenAstcGuest("))
            out.function = map[id];
    }
    for (const auto& i : original) {
        if (Opcode(i) == static_cast<u32>(spv::Op::OpTypeInt) && i[2] == 32 && i[3] == 0)
            out.uint_type = map[i[1]];
        if (Opcode(i) == static_cast<u32>(spv::Op::OpTypeFloat) && i[2] == 32)
            out.float_type = map[i[1]];
        if (Opcode(i) == static_cast<u32>(spv::Op::OpTypeVector) && i[3] == 4) {
            if (map[i[2]] == out.float_type)
                out.float4 = map[i[1]];
            if (map[i[2]] == out.uint_type)
                out.uint4 = map[i[1]];
        }
        if (Opcode(i) == static_cast<u32>(spv::Op::OpTypeVector) && map[i[2]] == out.uint_type) {
            if (i[3] == 2)
                out.uint2 = map[i[1]];
            if (i[3] == 3)
                out.uint3 = map[i[1]];
        }
        if (Opcode(i) == static_cast<u32>(spv::Op::OpVariable) && old_names.contains(i[2]) &&
            old_names[i[2]] == "guest_dummy")
            out.sampler_pointer = map[i[1]];
    }
    if (!out.function || !out.sampler_pointer || !out.float4 || !out.uint4)
        throw std::runtime_error("ASTC guest missing library exports");
    return out;
}
} // namespace

void SetAstcGuestSampling(bool value) {
    enabled.store(value, std::memory_order_relaxed);
}
bool AstcGuestSamplingEnabled() {
    return enabled.load(std::memory_order_relaxed);
}

static AstcGuestShader PatchAstcGuestShaderImpl(
    std::vector<u32> code, const Shader::Info& info, u32 first_binding, bool fragment,
    const std::unordered_set<u32>* allowed_descriptors) {
    AstcGuestShader out{std::move(code), {}};
    if (!AstcGuestSamplingEnabled())
        return out;
    u32 binding = first_binding;
    for (const auto& d : info.constant_buffer_descriptors)
        binding += d.count;
    for (const auto& d : info.storage_buffers_descriptors)
        binding += d.count;
    binding += static_cast<u32>(info.texture_buffer_descriptors.size() +
                                info.image_buffer_descriptors.size());
    std::unordered_set<u32> texture_bindings;
    std::unordered_set<u32> array_bindings;
    u32 descriptor_index{};
    for (const auto& d : info.texture_descriptors) {
        if ((!allowed_descriptors || allowed_descriptors->contains(descriptor_index)) &&
            d.count == 1 && !d.is_integer && !d.is_multisample &&
            (d.type == Shader::TextureType::Color2D || d.type == Shader::TextureType::ColorArray2D))
            texture_bindings.insert(binding);
        if (texture_bindings.contains(binding) && d.type == Shader::TextureType::ColorArray2D)
            array_bindings.insert(binding);
        binding += d.count;
        ++descriptor_index;
    }
    if (texture_bindings.empty())
        return out;
    auto guest = Parse(out.code);
    std::unordered_map<u32, u32> origins, bindings, sets, types;
    std::unordered_map<u32, Inst> declarations;
    for (const auto& i : guest) {
        const u32 op = Opcode(i);
        if (op == static_cast<u32>(spv::Op::OpDecorate)) {
            if (i[2] == static_cast<u32>(spv::Decoration::Binding))
                bindings[i[1]] = i[3];
            if (i[2] == static_cast<u32>(spv::Decoration::DescriptorSet))
                sets[i[1]] = i[3];
        }
        const u32 result = Result(i);
        if (result) {
            bool has_result{}, has_type{};
            spv::HasResultAndType(static_cast<spv::Op>(op), &has_result, &has_type);
            if (has_type)
                types[result] = i[1];
            if (IsDeclaration(op))
                declarations[result] = i;
        }
    }
    for (const auto& [id, b] : bindings)
        if (sets[id] == 0 && texture_bindings.contains(b))
            origins[id] = b;
    for (const auto& i : guest) {
        const u32 op = Opcode(i);
        if ((op == 61 || op == 65 || op == 66 || op == 83 || op == 86 || op == 100) &&
            i.size() > 3 && origins.contains(i[3]))
            origins[Result(i)] = origins[i[3]];
    }
    out.bindings = texture_bindings;
    for (const auto& i : guest) {
        const u32 op = Opcode(i);
        if (!IsImageOperation(op) || i.size() < 4 || !origins.contains(i[3]))
            continue;
        const u32 b = origins[i[3]];
        const bool supported =
            (op == 87 && fragment) || op == 88 || op == 95 || op == 103 || op == 106;
        // Grad/gather/dref/sparse and dynamic offsets retain native sampling for the whole binding.
        const bool bad_mask =
            (op == 87 || op == 88 || op == 95) && i.size() > 5 && (i[5] & ~(1u | 2u | 128u));
        if (!supported || bad_mask)
            out.bindings.erase(b);
    }
    std::unordered_map<u32, size_t> binding_calls;
    size_t decode_calls{};
    for (const auto& i : guest) {
        const u32 op = Opcode(i);
        if ((op == 87 || op == 88 || op == 95) && origins.contains(i[3]) &&
            out.bindings.contains(origins[i[3]]))
            ++decode_calls, ++binding_calls[origins.at(i[3])];
    }
    constexpr size_t max_decode_calls = 5;
    if (decode_calls > max_decode_calls) {
        // A shader may also sample many ordinary 2D textures. Do not force its large ASTC array
        // back to RGBA8 merely because unrelated bindings exceed the compiler's transient budget.
        std::vector<u32> candidates(out.bindings.begin(), out.bindings.end());
        std::sort(candidates.begin(), candidates.end(), [&](u32 a, u32 b) {
            if (array_bindings.contains(a) != array_bindings.contains(b))
                return array_bindings.contains(a);
            if (binding_calls[a] != binding_calls[b])
                return binding_calls[a] < binding_calls[b];
            return a < b;
        });
        const size_t total_calls = decode_calls;
        out.bindings.clear();
        decode_calls = 0;
        for (const u32 candidate : candidates)
            if (decode_calls + binding_calls[candidate] <= max_decode_calls) {
                out.bindings.insert(candidate);
                decode_calls += binding_calls[candidate];
            }
        LOG_INFO(Render,
                 "D3D12: ASTC guest selective import: total_calls={} imported_calls={} "
                 "bindings={} array_priority=yes cap={}",
                 total_calls, decode_calls, out.bindings.size(), max_decode_calls);
    }
    if (out.bindings.empty())
        return out;
    u32 bound = out.code[3];
    auto lib =
        fragment
            ? Import(guest, AstcGuestEmbedded::fragment, AstcGuestEmbedded::fragment_ids, bound)
            : Import(guest, AstcGuestEmbedded::compute, AstcGuestEmbedded::compute_ids, bound);
    Instructions constants;
    auto constant = [&](u32 type, u32 bits) {
        const u32 id = bound++;
        constants.push_back(Make(spv::Op::OpConstant, {type, id, bits}));
        return id;
    };
    const u32 zero = constant(lib.float_type, 0), zero_u = constant(lib.uint_type, 0);
    const u32 min_lod =
        constant(lib.float_type, std::bit_cast<u32>(-std::numeric_limits<float>::max()));
    std::unordered_map<u32, u32> buffers;
    for (u32 b : out.bindings) {
        const u32 id = bound++;
        buffers[b] = id;
        lib.declarations.push_back(
            Make(spv::Op::OpVariable,
                 {lib.sampler_pointer, id, static_cast<u32>(spv::StorageClass::UniformConstant)}));
        lib.annotations.push_back(
            Make(spv::Op::OpDecorate,
                 {id, static_cast<u32>(spv::Decoration::Binding), ASTC_GUEST_BINDING_OFFSET + b}));
        lib.annotations.push_back(
            Make(spv::Op::OpDecorate, {id, static_cast<u32>(spv::Decoration::DescriptorSet), 0}));
    }
    Instructions transformed;
    auto append = [&](spv::Op op, std::initializer_list<u32> args) {
        transformed.push_back(Make(op, args));
    };
    auto scalar = [&](u32 value) {
        const auto& type = declarations.at(types.at(value));
        if (Opcode(type) == static_cast<u32>(spv::Op::OpTypeFloat))
            return value;
        const u32 id = bound++;
        append(type[3] ? spv::Op::OpConvertSToF : spv::Op::OpConvertUToF,
               {lib.float_type, id, value});
        return id;
    };
    auto vector4 = [&](u32 value, bool integer_bits) {
        std::array<u32, 4> components{integer_bits ? zero_u : zero, integer_bits ? zero_u : zero,
                                      integer_bits ? zero_u : zero, integer_bits ? zero_u : zero};
        const auto& type = declarations.at(types.at(value));
        const u32 count = Opcode(type) == static_cast<u32>(spv::Op::OpTypeVector) ? type[3] : 1;
        for (u32 c = 0; c < count; ++c) {
            u32 component = value;
            if (count > 1) {
                component = bound++;
                append(spv::Op::OpCompositeExtract, {type[2], component, value, c});
                types[component] = type[2];
            }
            if (integer_bits) {
                const u32 temp = bound++;
                append(spv::Op::OpBitcast, {lib.uint_type, temp, component});
                components[c] = temp;
            } else
                components[c] = scalar(component);
        }
        const u32 id = bound++;
        append(spv::Op::OpCompositeConstruct,
               {integer_bits ? lib.uint4 : lib.float4, id, components[0], components[1],
                components[2], components[3]});
        if (!integer_bits)
            return id;
        const u32 bits = bound++;
        append(spv::Op::OpBitcast, {lib.float4, bits, id});
        return bits;
    };
    for (const auto& instruction : guest) {
        const u32 op = Opcode(instruction);
        if (!IsImageOperation(op) || instruction.size() < 4 || !origins.contains(instruction[3]) ||
            !out.bindings.contains(origins[instruction[3]])) {
            transformed.push_back(instruction);
            continue;
        }
        const u32 b = origins[instruction[3]], old_result = instruction[2], native = bound++;
        Inst copy = instruction;
        copy[2] = native;
        types[native] = instruction[1];
        transformed.push_back(copy);
        const bool query = op == 103 || op == 106;
        const u32 native4 = query ? vector4(native, true) : native;
        u32 coords = zero;
        u32 lod = zero, bias = zero, clamp = min_lod, offset = 0;
        if (!query)
            coords = vector4(instruction[4], false);
        else {
            coords = bound++;
            append(spv::Op::OpCompositeConstruct, {lib.float4, coords, zero, zero, zero, zero});
        }
        if (op == 103)
            lod = scalar(instruction[4]);
        if (!query && instruction.size() > 5) {
            size_t p = 6;
            const u32 mask = instruction[5];
            if (mask & 1)
                bias = instruction[p++];
            if (mask & 2)
                lod = scalar(instruction[p++]);
            if (mask & 8)
                offset = vector4(instruction[p++], false);
            if (mask & 128)
                clamp = instruction[p++];
        }
        if (!offset) {
            offset = bound++;
            append(spv::Op::OpCompositeConstruct, {lib.float4, offset, zero, zero, zero, zero});
        }
        const u32 lod4 = bound++;
        append(spv::Op::OpCompositeConstruct, {lib.float4, lod4, lod, bias, clamp, zero});
        append(spv::Op::OpStore, {lib.names.at("eden_astc_coords"), coords});
        append(spv::Op::OpStore, {lib.names.at("eden_astc_native"), native4});
        append(spv::Op::OpStore, {lib.names.at("eden_astc_lod"), lod4});
        append(spv::Op::OpStore, {lib.names.at("eden_astc_offset"), offset});
        append(spv::Op::OpStore, {lib.names.at("eden_astc_slot"), constant(lib.uint_type, b)});
        const u32 operation = op == 87 ? 0 : op == 88 ? 1 : op == 95 ? 2 : op == 103 ? 3 : 4;
        append(spv::Op::OpStore,
               {lib.names.at("eden_astc_operation"), constant(lib.uint_type, operation)});
        const u32 call = query ? bound++ : old_result;
        append(spv::Op::OpFunctionCall, {lib.float4, call, lib.function, buffers.at(b)});
        if (query) {
            const u32 bits = bound++;
            append(spv::Op::OpBitcast, {lib.uint4, bits, call});
            const auto& type = declarations.at(instruction[1]);
            const u32 count = Opcode(type) == static_cast<u32>(spv::Op::OpTypeVector) ? type[3] : 1;
            std::array<u32, 3> components{};
            for (u32 c = 0; c < count; ++c) {
                components[c] = bound++;
                append(spv::Op::OpCompositeExtract, {lib.uint_type, components[c], bits, c});
            }
            if (count == 1)
                append(spv::Op::OpBitcast, {instruction[1], old_result, components[0]});
            else {
                const u32 construct = bound++;
                // Query coordinates use signed integers in the guest; bitcast the vector as a
                // whole.
                u32& vector_type = count == 2 ? lib.uint2 : lib.uint3;
                if (!vector_type) {
                    vector_type = bound++;
                    lib.declarations.push_back(
                        Make(spv::Op::OpTypeVector, {vector_type, lib.uint_type, count}));
                }
                Inst v = Make(spv::Op::OpCompositeConstruct, {vector_type, construct});
                v.insert(v.end(), components.begin(), components.begin() + count);
                v[0] = (static_cast<u32>(v.size()) << 16) |
                       static_cast<u32>(spv::Op::OpCompositeConstruct);
                transformed.push_back(v);
                append(spv::Op::OpBitcast, {instruction[1], old_result, construct});
            }
        }
    }
    std::vector<u32> result(out.code.begin(), out.code.begin() + 5);
    result[3] = bound;
    auto emit = [&](const Instructions& list) {
        for (const auto& i : list)
            result.insert(result.end(), i.begin(), i.end());
    };
    bool capabilities = false, imports = false, annotations = false, declarations_emitted = false;
    for (const auto& i : transformed) {
        if (!capabilities && Opcode(i) != static_cast<u32>(spv::Op::OpCapability)) {
            emit(lib.capabilities);
            capabilities = true;
        }
        if (!imports && Opcode(i) == static_cast<u32>(spv::Op::OpMemoryModel)) {
            emit(lib.imports);
            imports = true;
        }
        if (!annotations && IsDeclaration(Opcode(i))) {
            emit(lib.annotations);
            annotations = true;
        }
        if (!declarations_emitted && Opcode(i) == static_cast<u32>(spv::Op::OpFunction)) {
            emit(lib.declarations);
            emit(constants);
            emit(lib.functions);
            declarations_emitted = true;
        }
        result.insert(result.end(), i.begin(), i.end());
    }
    out.code = std::move(result);
    out.decoder_calls = static_cast<u32>(decode_calls);
    LOG_INFO(
        Render,
        "D3D12: ASTC guest shader patched {} bindings, {} SPIR-V bytes, stage {}, decoder_calls={}",
        out.bindings.size(), out.code.size() * 4, fragment ? "fragment" : "other", decode_calls);
    return out;
}

AstcGuestShader PatchAstcGuestShader(std::vector<u32> code, const Shader::Info& info,
                                     u32 first_binding, bool fragment,
                                     const std::unordered_set<u32>* allowed_descriptors) {
    if (!AstcGuestSamplingEnabled())
        return {std::move(code), {}};
    try {
        return PatchAstcGuestShaderImpl(code, info, first_binding, fragment, allowed_descriptors);
    } catch (const std::exception& error) {
        LOG_WARNING(Render, "D3D12: ASTC guest import failed; preserving native shader: {}",
                    error.what());
        return {std::move(code), {}};
    }
}
} // namespace D3D12
