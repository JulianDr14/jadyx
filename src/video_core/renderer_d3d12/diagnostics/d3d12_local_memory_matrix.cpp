// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include "d3d12_local_memory_matrix.h"
#include <array>
#include <chrono>
#include <fstream>
#include <stdexcept>
#include <vector>
#include <cstring>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <wrl/client.h>
#include <nlohmann/json.hpp>
#include "video_core/renderer_d3d12/d3d12_driver_guard.h"
namespace D3D12 {
namespace {
using Microsoft::WRL::ComPtr;
using Json = nlohmann::json;
void Check(HRESULT result) {if(FAILED(result)) throw std::runtime_error("HRESULT " + std::to_string(result));}
std::vector<char> Read(const std::filesystem::path& path) {
    const auto size=std::filesystem::file_size(path);
    if(size<32 || size>(1<<20)) throw std::runtime_error("matrix shader size");
    std::vector<char> result(size);std::ifstream f(path,std::ios::binary);f.read(result.data(),size);
    if(!f || std::memcmp(result.data(),"DXBC",4)) throw std::runtime_error("matrix shader read");
    return result;
}
}
void RunLocalMemoryMatrix(ID3D12Device* device,const std::filesystem::path& shaders,
                          const std::filesystem::path& report) noexcept {
    std::ofstream log;
    try {
        std::ifstream input(shaders/"manifest.json");if(!input) return;
        Json cases;input>>cases;
        if(!cases.is_array() || cases.empty() || cases.size()>128) throw std::runtime_error("matrix manifest");
        std::filesystem::create_directories(report.parent_path());log.open(report);
        if(!log) return;
        const auto luid=device->GetAdapterLuid();
        D3D12_FEATURE_DATA_D3D12_OPTIONS options{};D3D12_FEATURE_DATA_D3D12_OPTIONS1 options1{};
        device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS,&options,sizeof(options));
        device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1,&options1,sizeof(options1));
        log<<Json{{"event","begin"},{"revision",27},{"cases",cases.size()},{"adapter_luid_low",luid.LowPart},{"adapter_luid_high",luid.HighPart},
                  {"rovs",bool(options.ROVsSupported)},{"wave_lanes_min",options1.WaveLaneCountMin},{"wave_lanes_max",options1.WaveLaneCountMax}}.dump()<<std::endl;
        const auto vs=Read(shaders/"vs.dxil");
        D3D12_ROOT_PARAMETER params[2]{};
        params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants={0,0,4};params[0].ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[1].Descriptor={1,0};params[1].ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC root_desc{2,params,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
        ComPtr<ID3DBlob> serialized,error;Check(D3D12SerializeRootSignature(&root_desc,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,&error));
        ComPtr<ID3D12RootSignature> root;Check(device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(&root)));
        D3D12_COMMAND_QUEUE_DESC queue_desc{};queue_desc.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
        ComPtr<ID3D12CommandQueue> queue;Check(device->CreateCommandQueue(&queue_desc,IID_PPV_ARGS(&queue)));
        ComPtr<ID3D12CommandAllocator> allocator;Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)));
        ComPtr<ID3D12GraphicsCommandList> cmd;Check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&cmd)));Check(cmd->Close());
        ComPtr<ID3D12Fence> fence;Check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));
        struct Event {HANDLE value=CreateEventExW(nullptr,nullptr,0,EVENT_ALL_ACCESS);~Event(){if(value)CloseHandle(value);}} event;
        if(!event.value) throw std::runtime_error("matrix fence event");
        // Storage doubles as the pixel UAV scratch of the revision 26 cases (64 slots of 588 words).
        constexpr UINT64 storage_bytes=256*1024;
        const auto buffer=[&](D3D12_HEAP_TYPE type,D3D12_RESOURCE_STATES state,D3D12_RESOURCE_FLAGS flags,UINT64 width=1024) {
            D3D12_HEAP_PROPERTIES heap{};heap.Type=type;
            D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=width;
            desc.Height=1;desc.DepthOrArraySize=1;desc.MipLevels=1;desc.SampleDesc.Count=1;
            desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;desc.Flags=flags;
            ComPtr<ID3D12Resource> result;Check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,state,nullptr,IID_PPV_ARGS(&result)));return result;
        };
        auto storage=buffer(D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,storage_bytes);
        auto sentinel_upload=buffer(D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ,D3D12_RESOURCE_FLAG_NONE);
        void* sentinel_data{};D3D12_RANGE empty_range{};Check(sentinel_upload->Map(0,&empty_range,&sentinel_data));
        std::memset(sentinel_data,0xA5,16);sentinel_upload->Unmap(0,nullptr);
        auto download=buffer(D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_FLAG_NONE);
        D3D12_RESOURCE_DESC td{};td.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;td.Width=td.Height=1;
        td.DepthOrArraySize=1;td.MipLevels=1;td.SampleDesc.Count=1;td.Format=DXGI_FORMAT_R32G32B32A32_UINT;td.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
        ComPtr<ID3D12Resource> target;Check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&td,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&target)));
        D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV,1};ComPtr<ID3D12DescriptorHeap> rtvs;
        Check(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&rtvs)));auto rtv=rtvs->GetCPUDescriptorHandleForHeapStart();device->CreateRenderTargetView(target.Get(),nullptr,rtv);
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};device->GetCopyableFootprints(&td,0,1,0,&footprint,nullptr,nullptr,nullptr);
        ComPtr<ID3D12InfoQueue> debug;device->QueryInterface(IID_PPV_ARGS(&debug));
        unsigned passed=0,failed=0;UINT64 tick=0;
        for(const auto& test:cases) {
            Json row{{"event","case"},{"name",test.at("name")},{"stage",test.at("stage")},{"compiler",test.at("compiler")},{"expected",test.at("expected")},{"params",test.at("params")},{"file",test.at("file")}};
            for(const char* field:{"mode","words","storage","dxil_alloca","dxil_bytes"}) if(test.contains(field)) row[field]=test.at(field);
            log<<Json{{"event","case_begin"},{"name",test.at("name")}}.dump()<<std::endl;
            bool gpu_timeout=false;
            try {
                const auto code=Read(shaders/test.at("file").get<std::string>());
                row["file_bytes"]=code.size();
                const auto values=test.at("params").get<std::array<unsigned,4>>();
                if(values[0]>5 || values[1]>=588) throw std::runtime_error("matrix input range");
                const bool compute=test.at("stage")=="compute";
                ComPtr<ID3D12PipelineState> pso;DWORD fault=0;HRESULT hr;
                const auto started=std::chrono::steady_clock::now();
                if(debug) debug->ClearStoredMessages();
                if(compute) {
                    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=root.Get();pd.CS={code.data(),code.size()};
                    hr=CallDriverGuarded([&]{return device->CreateComputePipelineState(&pd,IID_PPV_ARGS(&pso));},fault);
                } else {
                    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};pd.pRootSignature=root.Get();pd.VS={vs.data(),vs.size()};pd.PS={code.data(),code.size()};
                    pd.SampleMask=UINT_MAX;pd.SampleDesc.Count=1;pd.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
                    pd.NumRenderTargets=1;pd.RTVFormats[0]=td.Format;pd.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;pd.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;pd.RasterizerState.DepthClipEnable=TRUE;
                    pd.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_ALWAYS;
                    pd.DepthStencilState.FrontFace=pd.DepthStencilState.BackFace={D3D12_STENCIL_OP_KEEP,D3D12_STENCIL_OP_KEEP,D3D12_STENCIL_OP_KEEP,D3D12_COMPARISON_FUNC_ALWAYS};
                    auto& blend=pd.BlendState.RenderTarget[0];blend.SrcBlend=blend.SrcBlendAlpha=D3D12_BLEND_ONE;blend.DestBlend=blend.DestBlendAlpha=D3D12_BLEND_ZERO;blend.BlendOp=blend.BlendOpAlpha=D3D12_BLEND_OP_ADD;blend.LogicOp=D3D12_LOGIC_OP_NOOP;blend.RenderTargetWriteMask=15;
                    hr=CallDriverGuarded([&]{return device->CreateGraphicsPipelineState(&pd,IID_PPV_ARGS(&pso));},fault);
                }
                row["create_us"]=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-started).count();row["fault"]=fault;Check(hr);
                Check(allocator->Reset());Check(cmd->Reset(allocator.Get(),pso.Get()));
                const auto transition=[&](ID3D12Resource* resource,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after){D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition={resource,0,before,after};cmd->ResourceBarrier(1,&b);};
                if(compute) {
                    transition(storage.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);
                    cmd->CopyBufferRegion(storage.Get(),0,sentinel_upload.Get(),0,16);
                    transition(storage.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    cmd->SetComputeRootSignature(root.Get());cmd->SetComputeRoot32BitConstants(0,4,values.data(),0);cmd->SetComputeRootUnorderedAccessView(1,storage->GetGPUVirtualAddress());cmd->Dispatch(1,1,1);
                    transition(storage.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);cmd->CopyBufferRegion(download.Get(),0,storage.Get(),0,16);transition(storage.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);
                } else {
                    transition(storage.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    const float clear[4]{};cmd->ClearRenderTargetView(rtv,clear,0,nullptr);cmd->SetGraphicsRootSignature(root.Get());cmd->SetGraphicsRoot32BitConstants(0,4,values.data(),0);cmd->SetGraphicsRootUnorderedAccessView(1,storage->GetGPUVirtualAddress());cmd->OMSetRenderTargets(1,&rtv,FALSE,nullptr);
                    D3D12_VIEWPORT viewport{0,0,1,1,0,1};D3D12_RECT scissor{0,0,1,1};cmd->RSSetViewports(1,&viewport);cmd->RSSetScissorRects(1,&scissor);cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);cmd->DrawInstanced(3,1,0,0);
                    transition(target.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);
                    D3D12_TEXTURE_COPY_LOCATION src{target.Get(),D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,{.SubresourceIndex=0}},dst{download.Get(),D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,{.PlacedFootprint=footprint}};cmd->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
                    transition(target.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);
                    transition(storage.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COMMON);
                }
                Check(cmd->Close());ID3D12CommandList* lists[]={cmd.Get()};queue->ExecuteCommandLists(1,lists);Check(queue->Signal(fence.Get(),++tick));Check(fence->SetEventOnCompletion(tick,event.value));
                if(WaitForSingleObjectEx(event.value,10000,FALSE)!=WAIT_OBJECT_0) {gpu_timeout=true;throw std::runtime_error("matrix GPU fence timeout");}
                void* mapped{};D3D12_RANGE range{0,16};Check(download->Map(0,&range,&mapped));std::array<unsigned,4> actual;std::memcpy(actual.data(),mapped,16);download->Unmap(0,nullptr);
                row["actual"]=actual;row["pass"]=actual==test.at("expected").get<std::array<unsigned,4>>();
                unsigned messages=0;
                if(debug) for(UINT64 i=0;i<debug->GetNumStoredMessages();i++){SIZE_T size=0;debug->GetMessage(i,nullptr,&size);std::vector<char> bytes(size);auto* m=reinterpret_cast<D3D12_MESSAGE*>(bytes.data());debug->GetMessage(i,m,&size);if(m->Severity<=D3D12_MESSAGE_SEVERITY_WARNING)messages++;}
                row["debug_warnings_errors"]=messages;if(messages) row["pass"]=false;
            } catch(const std::exception& e){row["pass"]=false;row["error"]=e.what();}
            if(row["pass"].get<bool>())passed++;else failed++;
            log<<row.dump()<<std::endl;
            if(gpu_timeout){log<<Json{{"event","abort"},{"reason","GPU fence timeout"}}.dump()<<std::endl;return;}
            if(FAILED(device->GetDeviceRemovedReason())){log<<Json{{"event","abort"},{"reason","device removed"}}.dump()<<std::endl;return;}
        }
        log<<Json{{"event","end"},{"passed",passed},{"failed",failed},{"complete",true}}.dump()<<std::endl;
    } catch(const std::exception& e) {if(log)log<<Json{{"event","abort"},{"error",e.what()}}.dump()<<std::endl;}
}
}
