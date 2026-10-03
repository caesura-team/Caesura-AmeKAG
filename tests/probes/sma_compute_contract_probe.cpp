// Opt-in diagnostic: constant and exact skin compute on bgfx's own D3D11
// device, without a vertex draw. Not registered with CTest or a product path.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "../cpp/HiddenGpuContext.h"
#include "render/BgfxRenderDevice.h"
#include "render/SmaSkinner.h"
#include <bgfx/bgfx.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <nlohmann_json.hpp>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <array>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <charconv>
using Microsoft::WRL::ComPtr;
using json=nlohmann::json;
namespace fs=std::filesystem;
namespace {
HANDLE logFile=INVALID_HANDLE_VALUE;
void event(json value) {
    value["tick"]=GetTickCount64();value["pid"]=GetCurrentProcessId();
    const std::string line=value.dump()+"\n";DWORD written=0;
    if(line.size()>8192)throw std::runtime_error("diagnostic record overflow");
    if(!WriteFile(logFile,line.data(),DWORD(line.size()),&written,nullptr)||written!=line.size())throw std::runtime_error("diagnostic write failed");
    FlushFileBuffers(logFile);
}
void check(HRESULT hr,const char* operation) {
    event({{"operation",operation},{"HRESULT",uint32_t(hr)}});
    if(FAILED(hr))throw std::runtime_error(operation);
}
std::vector<uint8_t> read(const fs::path& path) {
    const auto size=fs::file_size(path);if(size<32||size>65536)throw std::runtime_error("DXBC size invalid");
    std::vector<uint8_t> bytes(size);std::ifstream in(path,std::ios::binary);in.read(reinterpret_cast<char*>(bytes.data()),size);
    if(!in||std::memcmp(bytes.data(),"DXBC",4))throw std::runtime_error("DXBC input invalid");return bytes;
}
ComPtr<ID3D11Buffer> buffer(ID3D11Device* device,UINT bytes,D3D11_USAGE usage,UINT bind,UINT access,const void* initial) {
    D3D11_BUFFER_DESC desc{};desc.ByteWidth=bytes;desc.Usage=usage;desc.BindFlags=bind;desc.CPUAccessFlags=access;
    D3D11_SUBRESOURCE_DATA data{};data.pSysMem=initial;
    event({{"operation","CreateBuffer.before"},{"bytes",bytes},{"usage",unsigned(usage)},{"bind",bind},{"cpu",access}});
    ComPtr<ID3D11Buffer> out;check(device->CreateBuffer(&desc,initial?&data:nullptr,out.GetAddressOf()),"CreateBuffer.after");return out;
}
ComPtr<ID3D11ShaderResourceView> srv(ID3D11Device* device,ID3D11Buffer* resource,UINT elements) {
    D3D11_SHADER_RESOURCE_VIEW_DESC desc{};desc.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;desc.ViewDimension=D3D11_SRV_DIMENSION_BUFFER;desc.Buffer.NumElements=elements;
    ComPtr<ID3D11ShaderResourceView> out;check(device->CreateShaderResourceView(resource,&desc,out.GetAddressOf()),"CreateShaderResourceView");return out;
}
void stage(ID3D11Device* device,ID3D11DeviceContext* context,const std::vector<uint8_t>& code,bool skin) {
    const char* name=skin?"guarded-skin":"constant";event({{"stage",name},{"operation","begin"},{"dxbc_bytes",code.size()}});
    Caesura::SMAMesh mesh;mesh.vertices={{0,0,0,0,0,.5f,1,.5f},{40,0,1,0,0,1,1,0},{40,40,1,1,0,1,1,0},{0,40,0,1,0,1,1,0}};
    mesh.indices={0,1,2,0,2,3};
    std::vector<Caesura::BonePose> poses(2);poses[0].rot=.3f;poses[0].scale=1.1f;poses[0].ox=30;poses[0].oy=10;poses[1].ox=60;poses[1].oy=20;
    std::vector<float> vertices,bones;
    for(const auto& v:mesh.vertices)vertices.insert(vertices.end(),{v.x,v.y,v.u,v.v,float(v.bone0),float(v.bone1),v.w0,v.w1});
    // Same two-row prefix as real SMA draw snapshots, followed by actual poses.
    bones.resize((poses.size()+2)*4,0.f);
    bones[0]=20;bones[1]=5;bones[2]=1;bones[4]=128;bones[5]=72;
    const uint32_t count=4,poseCount=static_cast<uint32_t>(poses.size());
    std::memcpy(&bones[6],&count,sizeof(count));std::memcpy(&bones[7],&poseCount,sizeof(poseCount));
    for(size_t i=0;i<poses.size();++i)Caesura::packBonePose(poses[i],&bones[(i+2)*4]);
    auto input=buffer(device,128,D3D11_USAGE_IMMUTABLE,D3D11_BIND_VERTEX_BUFFER|D3D11_BIND_SHADER_RESOURCE,0,vertices.data());
    auto bone=buffer(device,static_cast<UINT>(bones.size()*sizeof(float)),D3D11_USAGE_IMMUTABLE,D3D11_BIND_VERTEX_BUFFER|D3D11_BIND_SHADER_RESOURCE,0,bones.data());
    auto output=buffer(device,80,D3D11_USAGE_DEFAULT,D3D11_BIND_VERTEX_BUFFER|D3D11_BIND_UNORDERED_ACCESS,0,nullptr);
    auto staging=buffer(device,80,D3D11_USAGE_STAGING,0,D3D11_CPU_ACCESS_READ,nullptr);
    auto inputView=srv(device,input.Get(),8),boneView=srv(device,bone.Get(),static_cast<UINT>(bones.size()/4));
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};ud.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;ud.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;ud.Buffer.NumElements=5;
    ComPtr<ID3D11UnorderedAccessView> outputView;check(device->CreateUnorderedAccessView(output.Get(),&ud,outputView.GetAddressOf()),"CreateUnorderedAccessView");
    ComPtr<ID3D11ComputeShader> shader;check(device->CreateComputeShader(code.data(),code.size(),nullptr,shader.GetAddressOf()),"CreateComputeShader");
    D3D11_QUERY_DESC qd{};qd.Query=D3D11_QUERY_EVENT;
    ComPtr<ID3D11Query> query;check(device->CreateQuery(&qd,query.GetAddressOf()),"CreateQuery");
    struct UnbindStage {
        ID3D11DeviceContext* context;
        ~UnbindStage(){context->ClearState();}
    } unbind{context}; // Also clears binding references on query/Map failure.
    ID3D11ShaderResourceView* inputs[2]={inputView.Get(),boneView.Get()};ID3D11UnorderedAccessView* outputs[3]={nullptr,nullptr,outputView.Get()};
    // The probe exclusively owns this immediate context until all ComPtrs are
    // released. No bgfx frame/submit or other thread runs during either stage.
    context->ClearState();context->CSSetShader(shader.Get(),nullptr,0);context->CSSetShaderResources(0,2,inputs);context->CSSetUnorderedAccessViews(0,3,outputs,nullptr);
    event({{"stage",name},{"operation","Dispatch.before"},{"groups",{1,1,1}}});context->Dispatch(1,1,1);event({{"stage",name},{"operation","Dispatch.after"}});
    ID3D11UnorderedAccessView* none[3]={};context->CSSetUnorderedAccessViews(0,3,none,nullptr);
    event({{"stage",name},{"operation","CopyResource.before"}});context->CopyResource(staging.Get(),output.Get());event({{"stage",name},{"operation","CopyResource.after"}});
    context->End(query.Get());event({{"stage",name},{"operation","Flush.before"}});context->Flush();event({{"stage",name},{"operation","Flush.after"}});
    const auto began=GetTickCount64();HRESULT hr=S_FALSE;BOOL complete=FALSE;unsigned polls=0;
    do {
        hr=context->GetData(query.Get(),&complete,sizeof(complete),D3D11_ASYNC_GETDATA_DONOTFLUSH);++polls;
        if(hr!=S_FALSE)break;Sleep(1);
    }while(GetTickCount64()-began<10000);
    event({{"stage",name},{"operation","query.result"},{"HRESULT",uint32_t(hr)},{"complete",bool(complete)},{"polls",polls},{"elapsed_ms",GetTickCount64()-began},{"device_removed_reason",uint32_t(device->GetDeviceRemovedReason())}});
    if(hr!=S_OK||!complete)throw std::runtime_error(std::string(name)+": GPU completion not observed; Map not attempted");
    D3D11_MAPPED_SUBRESOURCE mapped{};event({{"stage",name},{"operation","Map.before"}});
    check(context->Map(staging.Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&mapped),"Map.after");
    std::array<float,16> actual{};std::memcpy(actual.data(),mapped.pData,sizeof(actual));context->Unmap(staging.Get(),0);
    std::vector<Caesura::SmaSkinnedVertex> expected;Caesura::skinMesh(mesh,poses,expected);
    bool pass=true;json rows=json::array();
    for(size_t i=0;i<4;++i) {
        const std::array<float,4> wanted=skin?std::array<float,4>{(20+expected[i].x)/128*2-1,1-(5+expected[i].y)/72*2,expected[i].u,expected[i].v}:std::array<float,4>{float(i),.5f,.25f,1};
        std::array<float,4> got{};for(size_t c=0;c<4;++c){got[c]=actual[i*4+c];pass=pass&&std::isfinite(got[c])&&std::abs(got[c]-wanted[c])<=0.00001f;}
        rows.push_back({{"vertex",i},{"actual",got},{"expected",wanted}});
    }
    event({{"stage",name},{"operation","output.compare"},{"pass",pass},{"tolerance",.00001},{"rows",rows}});
    context->ClearState();context->Flush();
    if(!pass)throw std::runtime_error(std::string(name)+": compute output mismatch");
    event({{"stage",name},{"operation","complete"}});
    // All stage-owned COM resources release here, before the context/device.
}
template<class T> T number(const char* text) {
    T value{};const char* end=text+std::strlen(text);const auto parsed=std::from_chars(text,end,value);
    if(parsed.ec!=std::errc{}||parsed.ptr!=end)throw std::runtime_error("invalid exact adapter identity");return value;
}
int independent(const std::vector<uint8_t>& constant,const std::vector<uint8_t>& skin,char** argv) {
    const auto low=number<ULONG>(argv[5]);const auto high=number<LONG>(argv[6]);
    const auto vendor=number<UINT>(argv[7]),deviceId=number<UINT>(argv[8]);
    const auto feature=static_cast<D3D_FEATURE_LEVEL>(number<unsigned>(argv[9]));
    constexpr UINT flags=0; // Normal hardware device, no bgfx SINGLETHREADED/debug flags.
    event({{"operation","independent.begin"},{"flags",flags},{"SDL_initialized",false},{"bgfx_initialized",false}});
    {
        ComPtr<IDXGIFactory1> factory;check(CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf())),"CreateDXGIFactory1");
        ComPtr<IDXGIAdapter1> selected;DXGI_ADAPTER_DESC desc{};
        for(UINT i=0;i<32;++i) {
            ComPtr<IDXGIAdapter1> candidate;const auto hr=factory->EnumAdapters1(i,candidate.GetAddressOf());
            if(hr==DXGI_ERROR_NOT_FOUND)break;check(hr,"EnumAdapters1");
            DXGI_ADAPTER_DESC current{};check(candidate->GetDesc(&current),"Get enumerated adapter desc");
            if(current.AdapterLuid.LowPart==low&&current.AdapterLuid.HighPart==high) {
                if(current.VendorId!=vendor||current.DeviceId!=deviceId)throw std::runtime_error("adapter LUID product identity mismatch");
                selected=candidate;desc=current;break;
            }
        }
        if(!selected)throw std::runtime_error("exact prior adapter LUID unavailable; no fallback");
        ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;D3D_FEATURE_LEVEL actual{};
        event({{"operation","D3D11CreateDevice.before"},{"flags",flags},{"driver_type","UNKNOWN_EXACT_ADAPTER"},{"requested_feature_level",unsigned(feature)}});
        check(D3D11CreateDevice(selected.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,flags,&feature,1,D3D11_SDK_VERSION,device.GetAddressOf(),&actual,context.GetAddressOf()),"D3D11CreateDevice.after");
        if(actual!=feature)throw std::runtime_error("feature level differs from prior device");
        event({{"operation","device.identity"},{"independent",true},{"vendor",desc.VendorId},{"device",desc.DeviceId},{"subsystem",desc.SubSysId},{"revision",desc.Revision},{"LUID_low",desc.AdapterLuid.LowPart},{"LUID_high",desc.AdapterLuid.HighPart},{"feature_level",unsigned(actual)},{"creation_flags",device->GetCreationFlags()}});
        stage(device.Get(),context.Get(),constant,false);stage(device.Get(),context.Get(),skin,true);
        event({{"operation","independent.context.clear.before"}});context->ClearState();context->Flush();event({{"operation","independent.context.clear.after"}});
        // Stage-owned buffers/views/query/shader are already released. Release
        // immediate context before its own device; this path never starts bgfx.
        context.Reset();event({{"operation","independent.context.released"}});
        device.Reset();event({{"operation","independent.device.released"}});
    }
    event({{"operation","independent.all-com-owners.released"}});return 0;
}
int softwareWarp(const std::vector<uint8_t>& constant,const std::vector<uint8_t>& skin) {
    // Explicit software reference, never an automatic fallback for hardware.
    constexpr UINT flags=0;constexpr D3D_FEATURE_LEVEL requested=D3D_FEATURE_LEVEL_11_0;
    event({{"operation","warp-software.begin"},{"hardware_gpu",false},{"flags",flags},{"SDL_initialized",false},{"bgfx_initialized",false}});
    {
        ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;D3D_FEATURE_LEVEL actual{};
        event({{"operation","D3D11CreateDevice.before"},{"driver_type","WARP_SOFTWARE"},{"flags",flags},{"requested_feature_level",unsigned(requested)}});
        check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,flags,&requested,1,D3D11_SDK_VERSION,device.GetAddressOf(),&actual,context.GetAddressOf()),"D3D11CreateDevice.after");
        ComPtr<IDXGIDevice> dxgi;check(device.As(&dxgi),"Query WARP DXGI device");
        ComPtr<IDXGIAdapter> adapter;check(dxgi->GetAdapter(adapter.GetAddressOf()),"Get WARP adapter");
        ComPtr<IDXGIAdapter1> adapter1;check(adapter.As(&adapter1),"Query WARP adapter1");
        DXGI_ADAPTER_DESC1 desc{};check(adapter1->GetDesc1(&desc),"Get WARP descriptor");
        const bool software=(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)!=0;
        event({{"operation","device.identity"},{"driver_type","WARP_SOFTWARE"},{"software_adapter",software},{"adapter_flags",desc.Flags},{"vendor",desc.VendorId},{"device",desc.DeviceId},{"LUID_low",desc.AdapterLuid.LowPart},{"LUID_high",desc.AdapterLuid.HighPart},{"feature_level",unsigned(actual)},{"creation_flags",device->GetCreationFlags()}});
        if(!software||actual!=requested)throw std::runtime_error("WARP software identity/feature mismatch");
        stage(device.Get(),context.Get(),constant,false);stage(device.Get(),context.Get(),skin,true);
        context->ClearState();context->Flush();context.Reset();device.Reset();
    }
    event({{"operation","warp-software.all-com-owners.released"}});return 0;
}
}
int main(int argc,char** argv) {
    const bool warp=argc==5 && std::strcmp(argv[4],"--warp-d3d11")==0;
    if(argc!=4 && !warp && !(argc==10 && std::strcmp(argv[4],"--independent-d3d11")==0))return 64;
    int result=1;
    try {
        const fs::path directory=fs::u8path(argv[1]);
        if(!directory.is_absolute()||!fs::is_directory(directory))throw std::runtime_error("owned output directory required");
        logFile=CreateFileW((directory/L"compute-events.jsonl").c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(logFile==INVALID_HANDLE_VALUE)throw std::runtime_error("fresh event file required");
        const auto constant=read(fs::u8path(argv[2])),skin=read(fs::u8path(argv[3]));
        if(warp) {
            result=softwareWarp(constant,skin);
        } else if(argc==10) {
            result=independent(constant,skin,argv);
        } else {
        CaesuraTest::HiddenSdlWindow window(128,72);if(!window||!window.nativeHandle())throw std::runtime_error("hidden native window unavailable");
        Caesura::BgfxRenderDevice renderer;
        if(!renderer.setPreferredBackend("dx11")||!renderer.init(window.nativeHandle(),128,72))throw std::runtime_error("real dx11 unavailable");
        event({{"operation","bgfx.initial-frame.before"}});bgfx::frame();event({{"operation","bgfx.initial-frame.after"}});
        try {
            const auto* internal=bgfx::getInternalData();
            if(bgfx::getRendererType()!=bgfx::RendererType::Direct3D11||!internal||!internal->context)throw std::runtime_error("missing bgfx D3D11 device");
            ComPtr<ID3D11Device> device(static_cast<ID3D11Device*>(internal->context));
            ComPtr<ID3D11DeviceContext> context;device->GetImmediateContext(context.GetAddressOf());
            ComPtr<IDXGIDevice> dxgi;check(device.As(&dxgi),"Query DXGI device");ComPtr<IDXGIAdapter> adapter;check(dxgi->GetAdapter(adapter.GetAddressOf()),"Get actual adapter");
            DXGI_ADAPTER_DESC desc{};check(adapter->GetDesc(&desc),"Get adapter desc");
            event({{"operation","device.identity"},{"vendor",desc.VendorId},{"device",desc.DeviceId},{"subsystem",desc.SubSysId},{"revision",desc.Revision},{"LUID_low",desc.AdapterLuid.LowPart},{"LUID_high",desc.AdapterLuid.HighPart},{"feature_level",unsigned(device->GetFeatureLevel())},{"bgfx_vendor",bgfx::getCaps()->vendorId},{"bgfx_device",bgfx::getCaps()->deviceId}});
            stage(device.Get(),context.Get(),constant,false);stage(device.Get(),context.Get(),skin,true);
            context->ClearState();context->Flush();result=0;
        } catch(const std::exception& error) {event({{"operation","compute.failure"},{"error",error.what()},{"vertex_draw_tested",false}});}
        // Every borrowed AddRef/context/stage COM owner above is released before
        // bgfx tears down its own device. A blocking native call remains subject
        // to the parent's 30s owned deadline; it is never reported as cleanup.
        event({{"operation","borrowed-com-owners.released"}});event({{"operation","bgfx.shutdown.before"}});renderer.shutdown();event({{"operation","bgfx.shutdown.after"}});
        }
        event({{"operation","terminal"},{"status",result==0?(warp?"WARP_SOFTWARE_COMPUTE_ONLY_PASS_DRAW_NOT_TESTED":"COMPUTE_ONLY_PASS_DRAW_NOT_TESTED"):"COMPUTE_DIAGNOSIS_FAILED"},{"software_reference",warp},{"actual_exit",result}});
    }catch(const std::exception& error){result=1;if(logFile!=INVALID_HANDLE_VALUE)event({{"operation","outer.failure"},{"error",error.what()}});}
    if(logFile!=INVALID_HANDLE_VALUE)CloseHandle(logFile);return result;
}
