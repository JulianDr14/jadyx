// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>
#include <nlohmann/json.hpp>
#include "common/logging.h"
#include "common/settings.h"
#include "video_core/renderer_d3d12/d3d12_device.h"
#include "video_core/renderer_d3d12/d3d12_shader_compiler.h"
#include "video_core/renderer_d3d12/diagnostics/d3d12_local_memory_matrix.h"
int main(int argc,char**argv) try {
    if(argc<3) return 2;
    Common::Log::Initialize();std::filesystem::path directory=argv[2];
    if(std::string(argv[1])=="translate") {
        D3D12::ShaderCompiler compiler;if(!compiler.IsAvailable())throw std::runtime_error("Mesa unavailable");
        nlohmann::json manifest;std::ifstream(directory/"manifest.json")>>manifest;
        for(const auto& c:manifest) if(c["compiler"]=="mesa") {
            auto path=directory/c["spv"].get<std::string>();std::vector<unsigned> words(std::filesystem::file_size(path)/4);
            std::ifstream in(path,std::ios::binary);in.read(reinterpret_cast<char*>(words.data()),words.size()*4);
            auto code=compiler.Compile(words,c["stage"]=="compute"?DXIL_SPIRV_SHADER_COMPUTE:DXIL_SPIRV_SHADER_FRAGMENT);
            std::ofstream out(directory/c["file"].get<std::string>(),std::ios::binary);out.write(reinterpret_cast<char*>(code.data()),code.size());
        }
        std::cout<<"PASS Mesa matrix cases translated and validated\n";return 0;
    }
    if(argc!=4) return 2;Settings::values.renderer_debug=true;D3D12::Device device;
    D3D12::RunLocalMemoryMatrix(device.Get(),directory,argv[3]);
    nlohmann::json last,row;std::ifstream log(argv[3]);for(std::string line;std::getline(log,line);)last=nlohmann::json::parse(line);
    std::cout<<last.dump()<<'\n';return last.value("complete",false)&&last.value("failed",1)==0?0:1;
} catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
