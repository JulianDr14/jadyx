// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "video_core/renderer_d3d12/d3d12_graphics_pipeline.h"
#include "video_core/renderer_d3d12/d3d12_maxwell_to_d3d12.h"
#include "video_core/renderer_d3d12/d3d12_scheduler.h"
#include "video_core/renderer_d3d12/d3d12_shader_compiler.h"
#include "video_core/textures/astc.h"

namespace D3D12 {
namespace {
using Bytes = std::vector<u8>;
constexpr auto ReadState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
Bytes ReadCapture(u32 draw, const char* label) {
    const auto name = fmt::format("astc-capture-9112b4082b675746-draw{}-{}.bin", draw, label);
    const auto packaged = Common::FS::GetExeDirectory() / "astc-replay" / name;
    const auto path = std::filesystem::exists(packaged) ? packaged :
        Common::FS::GetEdenPath(Common::FS::EdenPath::LogDir) / name;
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error(fmt::format("Replay missing {}", path.string()));
    const auto size = file.tellg();
    if (size < 0 || size > (64LL << 20)) throw std::runtime_error("Replay capture size invalid");
    Bytes bytes(static_cast<size_t>(size));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), size)) throw std::runtime_error("Replay capture read failed");
    return bytes;
}
u32 Word(const Bytes& data, size_t index) {
    if (index >= data.size()/4) throw std::runtime_error("Replay header bounds");
    u32 value; std::memcpy(&value, data.data()+index*4, 4); return value;
}
ComPtr<ID3D12Resource> Resource(Device& device, const D3D12_RESOURCE_DESC& desc,
                               D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state) {
    ComPtr<ID3D12Resource> out;
    const D3D12_HEAP_PROPERTIES heap{.Type=type};
    ThrowIfFailed(device.Get()->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,state,
                                                        nullptr,IID_PPV_ARGS(&out)),"ASTC replay resource");
    return out;
}
ComPtr<ID3D12Resource> Buffer(Device& device, u64 size, D3D12_HEAP_TYPE type,
                             D3D12_RESOURCE_STATES state, bool uav=false) {
    return Resource(device, {.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER,
        .Width=std::max<u64>(256,size),.Height=1,.DepthOrArraySize=1,.MipLevels=1,
        .SampleDesc={1,0},.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
        .Flags=uav?D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS:D3D12_RESOURCE_FLAG_NONE},type,state);
}
void Write(ID3D12Resource* resource, const void* data, size_t size) {
    void* p{}; const D3D12_RANGE empty{};
    ThrowIfFailed(resource->Map(0,&empty,&p),"ASTC replay Map");
    std::memcpy(p,data,size); const D3D12_RANGE written{0,size}; resource->Unmap(0,&written);
}
void Barrier(ID3D12GraphicsCommandList* cmd, ID3D12Resource* resource,
             D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after) {
    const D3D12_RESOURCE_BARRIER barrier{.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
        .Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after}};
    cmd->ResourceBarrier(1,&barrier);
}

// All resources outlive the scheduler, including exceptional device-removal paths.
class DrawReplay {
public:
    Device device;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pso;
    ComPtr<ID3D12DescriptorHeap> heap,samplers,rtv;
    ComPtr<ID3D12QueryHeap> timestamp,stats;
    ComPtr<ID3D12Resource> target,readback,raw,metadata,storage,native;
    std::array<ComPtr<ID3D12Resource>,7> constants;
    ComPtr<ID3D12Resource> vertices0,vertices1,indices;
    Bytes backing,meta,vs,ps;
    GraphicsPipelineCacheKey key{};
    std::array<D3D12_VIEWPORT,16> viewports{};
    std::array<D3D12_RECT,16> scissors{};
    u64 frequency{};
    std::array<Bytes,2> native_output;
    Scheduler scheduler{device};

    DrawReplay() {
        LOG_INFO(Render,"ASTC replay input source: {}",
                 std::filesystem::exists(Common::FS::GetExeDirectory()/"astc-replay") ? "packaged self-contained captures" : "LocalState captures");
        auto k=ReadCapture(3,"key");
        if(k.size()!=sizeof(key)) throw std::runtime_error("Replay key ABI mismatch");
        std::memcpy(&key,k.data(),k.size());
        if(key.unique_hashes[1]!=0xb4cf3ca379e7cfbeULL || key.unique_hashes[5]!=0x9112b4082b675746ULL)
            throw std::runtime_error("Replay shader identity mismatch");
        vs=ReadCapture(3,"stage0-dxil"); ps=ReadCapture(3,"stage4-dxil");
        backing=ReadCapture(3,"raw-view3"); meta=ReadCapture(3,"metadata");
        if(Word(backing,0)!=11 || Word(backing,3)!=0x41535443 || meta.size()!=12*96)
            throw std::runtime_error("Replay atlas metadata mismatch");
        const auto vp=ReadCapture(3,"viewports"), sc=ReadCapture(3,"scissors");
        if(vp.size()!=sizeof(viewports)||sc.size()!=sizeof(scissors)) throw std::runtime_error("Replay viewport ABI");
        std::memcpy(viewports.data(),vp.data(),vp.size()); std::memcpy(scissors.data(),sc.data(),sc.size());
        const std::array<const char*,7> labels{"buffer3-kind2-slot0","buffer4-kind2-slot1",
            "buffer5-kind2-slot2","buffer6-kind2-slot3","buffer7-kind2-slot4",
            "buffer8-kind2-slot5","buffer9-kind2-slot6"};
        for(size_t i=0;i<labels.size();++i) {
            const auto data=ReadCapture(3,labels[i]);
            constants[i]=Buffer(device,65536,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
            const Bytes zeros(65536); Write(constants[i].Get(),zeros.data(),zeros.size());
            Write(constants[i].Get(),data.data(),data.size());
        }
        const auto upload=[&](const Bytes& data, bool uav=false) {
            auto result=Buffer(device,data.size(),D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST,uav);
            auto staging=Buffer(device,data.size(),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
            Write(staging.Get(),data.data(),data.size());
            scheduler.CommandList()->CopyBufferRegion(result.Get(),0,staging.Get(),0,data.size());
            Barrier(scheduler.CommandList(),result.Get(),D3D12_RESOURCE_STATE_COPY_DEST,
                    uav?D3D12_RESOURCE_STATE_UNORDERED_ACCESS:ReadState);
            scheduler.Finish(); return result;
        };
        raw=upload(backing); metadata=upload(meta);
        storage=upload(ReadCapture(3,"buffer10-kind6-slot0"),true);
        const auto vb=[&](const char* label) {
            auto data=ReadCapture(3,label);
            auto result=Buffer(device,data.size(),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
            Write(result.Get(),data.data(),data.size()); return result;
        };
        vertices0=vb("buffer1-kind3-slot0"); vertices1=vb("buffer2-kind3-slot1");
        indices=vb("buffer0-kind4-slot0");
        CreateRoot(); CreateDescriptors(); CreatePipeline();
        readback=Buffer(device,32768,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        const D3D12_QUERY_HEAP_DESC td{.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP,.Count=2};
        const D3D12_QUERY_HEAP_DESC sd{.Type=D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS,.Count=1};
        ThrowIfFailed(device.Get()->CreateQueryHeap(&td,IID_PPV_ARGS(&timestamp)),"Replay timestamp");
        ThrowIfFailed(device.Get()->CreateQueryHeap(&sd,IID_PPV_ARGS(&stats)),"Replay stats");
        ThrowIfFailed(device.Queue()->GetTimestampFrequency(&frequency),"Replay frequency");
        if(!frequency) throw std::runtime_error("Replay zero timestamp frequency");
        LOG_INFO(Render,"ASTC draw replay: captured VS/PS+geometry+constants+atlas; native t8/t9/t10 are NULL substitutes; RT cleared RGBA8, depth disabled. Not full original replay. Adapter={}",device.AdapterName());
    }
    void CreateRoot() {
        const std::array<D3D12_DESCRIPTOR_RANGE,5> ranges{{
            {D3D12_DESCRIPTOR_RANGE_TYPE_CBV,7,0,0,0},
            {D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,7,0,7},
            {D3D12_DESCRIPTOR_RANGE_TYPE_SRV,4,8,0,8},
            {D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,4107,0,12},
            {D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,29,13}}};
        const D3D12_DESCRIPTOR_RANGE sr{D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER,4,8,0,0};
        const std::array<D3D12_ROOT_PARAMETER,4> params{{
            {.ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE,.DescriptorTable={5,ranges.data()}},
            {.ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE,.DescriptorTable={1,&sr}},
            {.ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS,.Constants={0,31,12}},
            {.ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS,.Constants={0,30,8}}}};
        const D3D12_ROOT_SIGNATURE_DESC desc{4,params.data(),0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
        ComPtr<ID3DBlob> blob,error;
        ThrowIfFailed(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error),"Replay serialize root");
        ThrowIfFailed(device.Get()->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)),"Replay root");
    }
    D3D12_CPU_DESCRIPTOR_HANDLE Slot(u32 slot) {
        auto handle=heap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr+=slot*device.Get()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        return handle;
    }
    void CreateDescriptors() {
        const D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,14,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
        const D3D12_DESCRIPTOR_HEAP_DESC sd{D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,4,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
        const D3D12_DESCRIPTOR_HEAP_DESC rd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV,1,D3D12_DESCRIPTOR_HEAP_FLAG_NONE,0};
        ThrowIfFailed(device.Get()->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap)),"Replay descriptors");
        ThrowIfFailed(device.Get()->CreateDescriptorHeap(&sd,IID_PPV_ARGS(&samplers)),"Replay samplers");
        ThrowIfFailed(device.Get()->CreateDescriptorHeap(&rd,IID_PPV_ARGS(&rtv)),"Replay RTV heap");
        for(u32 i=0;i<7;++i) {
            const D3D12_CONSTANT_BUFFER_VIEW_DESC cb{constants[i]->GetGPUVirtualAddress(),65536};
            device.Get()->CreateConstantBufferView(&cb,Slot(i));
        }
        const D3D12_UNORDERED_ACCESS_VIEW_DESC uav{.Format=DXGI_FORMAT_R32_TYPELESS,
            .ViewDimension=D3D12_UAV_DIMENSION_BUFFER,.Buffer={0,61440/4,0,0,D3D12_BUFFER_UAV_FLAG_RAW}};
        device.Get()->CreateUnorderedAccessView(storage.Get(),nullptr,&uav,Slot(7));
        for(u32 i=8;i<=11;++i) {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{.Format=DXGI_FORMAT_R8G8B8A8_UNORM,
                .ViewDimension=i==11?D3D12_SRV_DIMENSION_TEXTURE2DARRAY:D3D12_SRV_DIMENSION_TEXTURE2D,
                .Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
            if(i==11) srv.Texture2DArray={0,1,0,121,0,0}; else srv.Texture2D={0,1,0,0};
            device.Get()->CreateShaderResourceView(nullptr,&srv,Slot(i));
        }
        const D3D12_SHADER_RESOURCE_VIEW_DESC raw_srv{.Format=DXGI_FORMAT_R32G32B32A32_UINT,
            .ViewDimension=D3D12_SRV_DIMENSION_BUFFER,.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
            .Buffer={0,static_cast<UINT>(backing.size()/16),0,D3D12_BUFFER_SRV_FLAG_NONE}};
        device.Get()->CreateShaderResourceView(raw.Get(),&raw_srv,Slot(12));
        const D3D12_SHADER_RESOURCE_VIEW_DESC meta_srv{.Format=DXGI_FORMAT_R32_TYPELESS,
            .ViewDimension=D3D12_SRV_DIMENSION_BUFFER,.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
            .Buffer={0,static_cast<UINT>(meta.size()/4),0,D3D12_BUFFER_SRV_FLAG_RAW}};
        device.Get()->CreateShaderResourceView(metadata.Get(),&meta_srv,Slot(13));
        auto handle=samplers->GetCPUDescriptorHandleForHeapStart();
        for(u32 i=0;i<4;++i) {
            const D3D12_SAMPLER_DESC sampler{.Filter=D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT,
                .AddressU=D3D12_TEXTURE_ADDRESS_MODE_WRAP,.AddressV=D3D12_TEXTURE_ADDRESS_MODE_WRAP,
                .AddressW=D3D12_TEXTURE_ADDRESS_MODE_WRAP,.MaxAnisotropy=1,
                .ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS,.MinLOD=0,.MaxLOD=13};
            device.Get()->CreateSampler(&sampler,handle);
            handle.ptr+=device.Get()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
        }
    }
    void CreatePipeline() {
        std::vector<D3D12_INPUT_ELEMENT_DESC> elements;
        for(u32 index:{0u,1u,8u,9u,12u}) {
            const auto& a=key.state.attributes[index];
            if(!a.enabled) throw std::runtime_error("Replay input attribute missing");
            const auto format=MaxwellToD3D12::VertexFormat(a.Type(),a.Size());
            if(format==DXGI_FORMAT_UNKNOWN) throw std::runtime_error("Replay input format unsupported");
            elements.push_back({"TEXCOORD",index,format,a.buffer,a.offset,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0});
            LOG_INFO(Render,"ASTC replay attribute {} slot={} offset={} format={} stride={}",index,a.buffer,a.offset,static_cast<u32>(format),key.state.vertex_strides[a.buffer]);
        }
        if(MaxwellToD3D12::PrimitiveTopology(key.state.topology,0)!=D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST)
            throw std::runtime_error("Replay topology is not triangle list");
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature=root.Get(); desc.VS={vs.data(),vs.size()}; desc.PS={ps.data(),ps.size()};
        desc.InputLayout={elements.data(),static_cast<UINT>(elements.size())};
        desc.RasterizerState={.FillMode=D3D12_FILL_MODE_SOLID,.CullMode=D3D12_CULL_MODE_NONE,.DepthClipEnable=TRUE};
        desc.DepthStencilState.DepthEnable=FALSE; desc.DepthStencilState.StencilEnable=FALSE;
        desc.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
        desc.SampleMask=UINT_MAX; desc.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.NumRenderTargets=1; desc.RTVFormats[0]=DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc={1,0};
        ThrowIfFailed(device.Get()->CreateGraphicsPipelineState(&desc,IID_PPV_ARGS(&pso)),"Replay PSO");
        target=Resource(device,{.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D,.Width=64,.Height=64,
            .DepthOrArraySize=1,.MipLevels=1,.Format=DXGI_FORMAT_R8G8B8A8_UNORM,.SampleDesc={1,0},
            .Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET},D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_RENDER_TARGET);
        device.Get()->CreateRenderTargetView(target.Get(),nullptr,rtv->GetCPUDescriptorHandleForHeapStart());
    }
    void Run(u32 draw, bool direct,u32 repeat) {
        const auto matrix=ReadCapture(draw,"buffer4-kind2-slot1");
        if(matrix.size()!=48) throw std::runtime_error("Replay matrix size");
        if(direct) {
            // Match Directo's unmaterialized native companion, not the expanded reference.
            const D3D12_SHADER_RESOURCE_VIEW_DESC null_srv{.Format=Word(meta,11*24+20)?DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:DXGI_FORMAT_R8G8B8A8_UNORM,
                .ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2DARRAY,
                .Shader4ComponentMapping=Word(meta,11*24+7) | D3D12_SHADER_COMPONENT_MAPPING_ALWAYS_SET_BIT_AVOIDING_ZEROMEM_MISTAKES,
                .Texture2DArray={Word(meta,11*24+1),Word(meta,11*24+2),Word(meta,11*24+3),Word(meta,11*24+4),0,0}};
            device.Get()->CreateShaderResourceView(nullptr,&null_srv,Slot(11));
        }
        Write(constants[1].Get(),matrix.data(),matrix.size());
        auto metadata_bytes=meta;
        if(!direct) { const u32 disabled=0; std::memcpy(metadata_bytes.data()+11*96,&disabled,4); }
        auto staging=Buffer(device,metadata_bytes.size(),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
        Write(staging.Get(),metadata_bytes.data(),metadata_bytes.size());
        // Metadata is a DEFAULT buffer and decayed to COMMON after the previous Finish.
        scheduler.CommandList()->CopyBufferRegion(metadata.Get(),0,staging.Get(),0,metadata_bytes.size());
        scheduler.Finish();
        LOG_INFO(Render,"ASTC draw replay BEGIN draw={} mode={} repeat={}",draw,direct?"direct":"native-expanded",repeat);
        auto* cmd=scheduler.CommandList();
        ID3D12DescriptorHeap* heaps[]{heap.Get(),samplers.Get()}; cmd->SetDescriptorHeaps(2,heaps);
        cmd->SetGraphicsRootSignature(root.Get()); cmd->SetPipelineState(pso.Get());
        cmd->SetGraphicsRootDescriptorTable(0,heap->GetGPUDescriptorHandleForHeapStart());
        cmd->SetGraphicsRootDescriptorTable(1,samplers->GetGPUDescriptorHandleForHeapStart());
        dxil_spirv_vertex_runtime_data runtime_data{};
        runtime_data.first_vertex=329; runtime_data.is_indexed_draw=true;
        runtime_data.viewport_width=runtime_data.viewport_height=64.f;
        std::array<u32,12> runtime{};
        static_assert(sizeof(runtime_data)<=sizeof(runtime));
        std::memcpy(runtime.data(),&runtime_data,sizeof(runtime_data));
        cmd->SetGraphicsRoot32BitConstants(2,12,runtime.data(),0);
        const auto push=ReadCapture(3,"push");
        cmd->SetGraphicsRoot32BitConstants(3,8,push.data(),0);
        cmd->RSSetViewports(16,viewports.data()); cmd->RSSetScissorRects(16,scissors.data());
        const auto rt=rtv->GetCPUDescriptorHandleForHeapStart(); cmd->OMSetRenderTargets(1,&rt,FALSE,nullptr);
        const float clear[]{0,0,0,0}; cmd->ClearRenderTargetView(rt,clear,0,nullptr);
        const D3D12_VERTEX_BUFFER_VIEW vb[]{{vertices0->GetGPUVirtualAddress(),5416,12},{vertices1->GetGPUVirtualAddress(),14432,32}};
        const D3D12_INDEX_BUFFER_VIEW ib{indices->GetGPUVirtualAddress(),630,DXGI_FORMAT_R16_UINT};
        cmd->IASetVertexBuffers(0,2,vb); cmd->IASetIndexBuffer(&ib); cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmd->BeginQuery(stats.Get(),D3D12_QUERY_TYPE_PIPELINE_STATISTICS,0);
        cmd->EndQuery(timestamp.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0);
        cmd->DrawIndexedInstanced(315,1,0,329,0);
        cmd->EndQuery(timestamp.Get(),D3D12_QUERY_TYPE_TIMESTAMP,1);
        cmd->EndQuery(stats.Get(),D3D12_QUERY_TYPE_PIPELINE_STATISTICS,0);
        cmd->ResolveQueryData(timestamp.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,2,readback.Get(),0);
        cmd->ResolveQueryData(stats.Get(),D3D12_QUERY_TYPE_PIPELINE_STATISTICS,0,1,readback.Get(),16);
        Barrier(cmd,target.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);
        const D3D12_TEXTURE_COPY_LOCATION output_src{.pResource=target.Get(),.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,.SubresourceIndex=0};
        const D3D12_TEXTURE_COPY_LOCATION output_dst{.pResource=readback.Get(),.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
            .PlacedFootprint={512,{DXGI_FORMAT_R8G8B8A8_UNORM,64,64,1,256}}};
        cmd->CopyTextureRegion(&output_dst,0,0,0,&output_src,nullptr);
        Barrier(cmd,target.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);
        scheduler.Finish();
        void* p{}; const D3D12_RANGE range{0,512+16384};
        ThrowIfFailed(readback->Map(0,&range,&p),"Replay query map");
        u64 ticks[2]; D3D12_QUERY_DATA_PIPELINE_STATISTICS result;
        std::memcpy(ticks,p,16); std::memcpy(&result,static_cast<u8*>(p)+16,sizeof(result));
        Bytes output(static_cast<u8*>(p)+512,static_cast<u8*>(p)+512+16384);
        const D3D12_RANGE empty{}; readback->Unmap(0,&empty);
        if(!result.PSInvocations) throw std::runtime_error("Replay vacuous: zero pixel shader invocations");
        u32 differing=0,max_error=0;
        if(!direct && repeat==0) native_output[draw-2]=output;
        if(direct) {
            const auto& reference=native_output[draw-2];
            for(size_t i=0;i<output.size();++i) {
                const auto error=static_cast<u32>(std::abs(int(output[i])-int(reference[i])));
                differing+=error!=0; max_error=std::max(max_error,error);
            }
        }
        LOG_INFO(Render,"ASTC draw replay DONE draw={} mode={} repeat={} gpu_us={:.3f} VS={} PS={} clip_primitives={}",
            draw,direct?"direct":"native-expanded",repeat,double(ticks[1]-ticks[0])*1e6/frequency,result.VSInvocations,result.PSInvocations,result.CPrimitives);
        if(direct) LOG_INFO(Render,"ASTC draw replay comparison draw={} differing_bytes={} max_error={} (NULL other textures, RGBA8 output)",draw,differing,max_error);
    }
    void ExpandNative() {
        LOG_INFO(Render,"ASTC replay expanding captured atlas one slice/mip at a time; reference only, no guest RAM/JIT");
        const D3D12_RESOURCE_DESC desc{.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D,.Width=1024,.Height=1024,
            .DepthOrArraySize=121,.MipLevels=11,.Format=DXGI_FORMAT_R8G8B8A8_TYPELESS,.SampleDesc={1,0}};
        native=Resource(device,desc,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST);
        auto upload=Buffer(device,4U<<20,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
        for(u32 layer=0;layer<121;++layer) {
            for(u32 mip=0;mip<11;++mip) {
                const size_t base=4+mip*12; const u32 width=Word(backing,base),height=Word(backing,base+1);
                const u32 blocks_x=(width+7)/8,blocks_y=(height+7)/8;
                Bytes linear(size_t(blocks_x)*blocks_y*16),rgba(size_t(width)*height*4);
                for(u32 y=0;y<blocks_y;++y) for(u32 x=0;x<blocks_x;++x) {
                    const u32 px=x*16,by=y>>3;
                    u64 offset=u64(layer)*Word(backing,base+5)+(u64(by>>Word(backing,base+8))*Word(backing,base+6));
                    offset+=u64(by&Word(backing,base+9))<<9;
                    offset+=u64(px>>6)<<Word(backing,base+7);
                    offset+=((px&32)<<3)|((y&6)<<5)|((px&16)<<1)|((y&1)<<4)|(px&15);
                    offset+=u64(Word(backing,1))*16+Word(backing,base+4);
                    if(offset>backing.size() || backing.size()-offset<16) throw std::runtime_error("Replay unswizzle bounds");
                    std::memcpy(linear.data()+(size_t(y)*blocks_x+x)*16,backing.data()+offset,16);
                }
                Tegra::Texture::ASTC::DecompressForSamplingProbe(linear,width,height,1,8,8,rgba,Word(meta,11*24+20)!=0);
                D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{}; u64 bytes{};
                device.Get()->GetCopyableFootprints(&desc,mip+layer*11,1,0,&footprint,nullptr,nullptr,&bytes);
                if(bytes>(4U<<20)) throw std::runtime_error("Replay native upload bounds");
                Bytes padded(static_cast<size_t>(bytes));
                for(u32 y=0;y<height;++y) std::memcpy(padded.data()+size_t(y)*footprint.Footprint.RowPitch,rgba.data()+size_t(y)*width*4,width*4);
                Write(upload.Get(),padded.data(),padded.size());
                const D3D12_TEXTURE_COPY_LOCATION src{.pResource=upload.Get(),.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,.PlacedFootprint=footprint};
                const D3D12_TEXTURE_COPY_LOCATION dst{.pResource=native.Get(),.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,.SubresourceIndex=mip+layer*11};
                scheduler.CommandList()->CopyTextureRegion(&dst,0,0,0,&src,nullptr); scheduler.Finish();
            }
            if(layer%16==0) LOG_INFO(Render,"ASTC replay native expansion layer {}/121",layer+1);
        }
        // Texture COPY_DEST does not decay; explicit transition after final upload.
        Barrier(scheduler.CommandList(),native.Get(),D3D12_RESOURCE_STATE_COPY_DEST,ReadState); scheduler.Finish();
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{.Format=Word(meta,11*24+20)?DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:DXGI_FORMAT_R8G8B8A8_UNORM,
            .ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2DARRAY,.Shader4ComponentMapping=Word(meta,11*24+7) | D3D12_SHADER_COMPONENT_MAPPING_ALWAYS_SET_BIT_AVOIDING_ZEROMEM_MISTAKES,
            .Texture2DArray={0,11,0,121,0,0}};
        device.Get()->CreateShaderResourceView(native.Get(),&srv,Slot(11));
    }
};
} // namespace

bool RunAstcDrawReplay() {
    try {
        DrawReplay replay;
        replay.ExpandNative();
        for(u32 repeat=0;repeat<4;++repeat) for(u32 draw:{2u,3u}) replay.Run(draw,false,repeat);
        for(u32 repeat=0;repeat<4;++repeat) for(u32 draw:{2u,3u}) replay.Run(draw,true,repeat);
        LOG_INFO(Render,"ASTC draw replay PASS: controlled inputs; no original gameplay/Series certification");
        return true;
    } catch(const std::exception& e) {
        LOG_ERROR(Render,"ASTC draw replay FAIL: {}",e.what()); return false;
    }
}
} // namespace D3D12
