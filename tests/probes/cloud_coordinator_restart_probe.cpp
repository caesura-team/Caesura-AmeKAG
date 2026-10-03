// Native test process for coordinator CAES/receipt/cursor recovery; never installed.
#include "storage/SaveManager.h"
#include "storage/CloudSaveSnapshot.h"
#include "storage/AtomicSaveFile.h"
#include "storage/api/ISaveProvider.h"
#include "archive/CryptoEngine.h"
#include "di/BackendRegistry.h"
#include <nlohmann_json.hpp>
#include <array>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {
using namespace Caesura;
namespace fs = std::filesystem;
using Json = nlohmann::json;
using Code = CloudCoordinatorCode;
constexpr const char* scope = "0123456789abcdef0123456789abcdef";
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
unsigned long pid() {
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return static_cast<unsigned long>(::getpid());
#endif
}
std::string utf8(const fs::path& p) { const auto s=p.generic_u8string(); return {s.begin(),s.end()}; }
std::string read(const fs::path& p, size_t limit=10u*1024u*1024u) {
    std::ifstream f(p,std::ios::binary|std::ios::ate);
    require(f.good(),"file open failed"); const auto n=f.tellg();
    require(n>=0 && static_cast<uint64_t>(n)<=limit,"file size exceeds bound");
    std::string out(static_cast<size_t>(n),'\0'); f.seekg(0);
    if (!out.empty()) f.read(out.data(),static_cast<std::streamsize>(out.size()));
    require(f.good(),"file read incomplete"); return out;
}
void ownership() {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(std::chrono::steady_clock::now()<deadline) {
        if(fs::is_regular_file("owned-control/process.json")) {
            const auto j=Json::parse(read("owned-control/process.json",16384));
            require(j.at("pid").get<unsigned long>()==pid(),"owner PID mismatch");
            require(!j.at("created").get<std::string>().empty(),"missing creation identity"); return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    throw std::runtime_error("ownership receipt deadline");
}
std::string hex(const uint8_t* data,size_t n) {
    std::string out; constexpr char digits[]="0123456789abcdef";
    for(size_t i=0;i<n;++i) { out+=digits[data[i]>>4]; out+=digits[data[i]&15]; } return out;
}
std::string sha(const std::string& b) {
    std::array<uint8_t,32> h{}; auto* c=BackendRegistry::instance().getCryptoEngine();
    require(c!=nullptr,"crypto absent");
    c->sha256(reinterpret_cast<const uint8_t*>(b.data()),b.size(),h.data(),h.size()); return hex(h.data(),h.size());
}
Json bytes(const std::string& b) { return Json{{"size",b.size()},{"sha256",sha(b)}}; }
void emit(Json j) { j["pid"]=pid(); std::cout<<"U26_COORD_EVENT "<<j.dump()<<std::endl; require(std::cout.good(),"event flush failed"); }
[[noreturn]] void abnormal() {
#ifdef _WIN32
    if(!TerminateProcess(GetCurrentProcess(),74)) std::_Exit(91);
#else
    if(::kill(::getpid(),SIGKILL)!=0) std::_Exit(91);
#endif
    std::_Exit(92);
}
std::array<uint8_t,32> key(bool wrong=false) { std::array<uint8_t,32> k{}; k.fill(wrong?0x6c:0x5a); return k; }
struct Registration {
    carc::CryptoEngine crypto;
    carc::ICryptoEngine* old=BackendRegistry::instance().getCryptoEngine();
    ISaveManager* oldSave=BackendRegistry::instance().getSaveManager();
    Registration(){ BackendRegistry::instance().setCryptoEngine(&crypto); }
    ~Registration(){ BackendRegistry::instance().setSaveManager(oldSave); BackendRegistry::instance().setCryptoEngine(old); }
};
const char* code(Code c) {
    switch(c) {
    case Code::Ready:return "Ready"; case Code::Complete:return "Complete"; case Code::Replayed:return "Replayed";
    case Code::InvalidInput:return "InvalidInput"; case Code::InvalidSave:return "InvalidSave";
    case Code::InvalidAncestor:return "InvalidAncestor"; case Code::Indeterminate:return "Indeterminate";
    default:return "Unexpected";
    }
}
const char* ancestor(CloudAncestorSelection a) {
    switch(a) {
    case CloudAncestorSelection::Selected:return "Selected";
    case CloudAncestorSelection::RecoveredSelected:return "RecoveredSelected";
    case CloudAncestorSelection::RecoveredNotSelected:return "RecoveredNotSelected";
    case CloudAncestorSelection::Superseded:return "Superseded";
    case CloudAncestorSelection::Indeterminate:return "Indeterminate";
    case CloudAncestorSelection::NotRequested:return "NotRequested";
    default:return "Unexpected";
    }
}
Json prep(const CloudPreparationRef& r) { return Json{{"token",r.token},{"receipt_sha256",r.receiptSha256}}; }
CloudPreparationRef prep(const Json& j) { return {j.at("token").get<std::string>(),j.at("receipt_sha256").get<std::string>()}; }
Json record(const std::optional<CloudPreservedRecordRef>& r) {
    return r?Json{{"id",r->id},{"manifest_sha256",r->manifestSha256}}:Json(nullptr);
}
Json result(const CloudPrepareResult& r) {
    return Json{{"code",code(r.code)},{"code_value",static_cast<int>(r.code)},
        {"ancestor",ancestor(r.ancestor)},{"equal",r.comparison==CloudComparison::EqualObserved},
        {"preserved",r.preservation==CloudPreservation::Complete},{"record",record(r.record)},
        {"base",record(r.base)},{"preparation",r.preparation?prep(*r.preparation):Json(nullptr)},
        {"candidate_cursor_sha256",r.candidateCursorSha256},{"session",r.stamp.sessionId},
        {"local_valid",r.local.validity==CloudSaveValidity::ValidCurrentPolicy},
        {"cloud_valid",r.cloud.validity==CloudSaveValidity::ValidCurrentPolicy},
        {"local",{{"size",r.local.byteCount},{"sha256",r.local.sha256}}},
        {"cloud",{{"size",r.cloud.byteCount},{"sha256",r.cloud.sha256}}}};
}
// Deliberately a file-backed test transport, not HTTP/Steam evidence. Both sides
// go through the maintained typed local reader; every legacy/write call is counted.
struct DiskSides final : ISaveProvider,ICloudSaveSnapshotTransport {
    fs::path cloud; int localReads=0,cloudReads=0,legacy=0,writes=0;
    explicit DiskSides(fs::path p):cloud(std::move(p)){}
    CloudSnapshot readSnapshot(CloudSide side,const std::string& path) override {
        if(side==CloudSide::Local) ++localReads; else ++cloudReads;
        return detail::readLocalCloudSnapshot(side==CloudSide::Local?path:utf8(cloud));
    }
    CloudConditionalWriteSupport conditionalWriteSupport(CloudSide) const override { return CloudConditionalWriteSupport::Unsupported; }
    std::string readFile(const std::string&) override {++legacy;return {};}
    std::vector<std::string> listFiles(const std::string&) override {++legacy;return {};}
    bool writeFile(const std::string&,const std::string&) override {++writes;return false;}
    bool deleteFile(const std::string&) override {++writes;return false;}
    bool pushToCloud(const std::string&) override {++writes;return false;}
    bool pullFromCloud(const std::string&) override {++writes;return false;}
    bool supportsCloudSync() const override {return true;}
    Json counts() const {return Json{{"local",localReads},{"cloud",cloudReads},{"legacy",legacy},{"writes",writes}};}
};
struct CursorBoundary final : carc::CryptoEngine {
    fs::path cursor,operation; std::string boundary,candidate,old;
    DiskSides* transport=nullptr; unsigned hits=0,armedCount=0,delegated=0; bool armed=false;
    static bool checkpoint(detail::SaveWriteStage stage,const fs::path& temporary,void* data) {
        auto& self=*static_cast<CursorBoundary*>(data);
        if(stage!=detail::SaveWriteStage::Replace || temporary.parent_path()!=self.cursor.parent_path()) return true;
        require(++self.armedCount==1,"cursor Replace repeated");
        self.candidate=read(temporary,16384);
        require(read(self.cursor,16384)==self.old,"old cursor changed before Replace");
        const auto j=Json::parse(self.candidate);
        const auto receipt=read(self.operation/"receipt.json",16384);
        require(j.at("preparation").at("receipt_sha256")==sha(receipt),"candidate receipt not durable or identity differs");
        require(self.candidate!=self.old,"candidate equals prior cursor");
        if(self.boundary=="before-controlled" || self.boundary=="before-abnormal") {
            emit(Json{{"event","before-cursor"},{"candidate",j},{"candidate_cursor",bytes(self.candidate)},
                {"old_cursor",bytes(self.old)},{"counts",self.transport->counts()},
                {"termination",self.boundary=="before-controlled"?"controlled_exit":"self_abnormal"}});
            if(self.boundary=="before-abnormal") abnormal();
            std::_Exit(73);
        }
        require(self.boundary=="after-hash-exception","unexpected fault mode");
        self.armed=true; return true;
    }
    void sha256(const uint8_t* data,size_t n,uint8_t* out,size_t size) override {
        carc::CryptoEngine::sha256(data,n,out,size);
        if(!armed) return;
        require(hits==0 && armedCount==1,"hash fault repeated");
        require(read(cursor,16384)==candidate,"post-Replace cursor is not candidate");
        require(n==candidate.size() && std::string(reinterpret_cast<const char*>(data),n)==candidate,
            "first armed digest is not published cursor");
        require(size==32,"unexpected digest size");
        ++delegated; ++hits; armed=false;
        throw std::runtime_error("U26_COORD_ACTUAL_POST_CURSOR_SHA_EXCEPTION");
    }
};
int run(const Json& q) {
    require(q.at("schema_version")==1,"unknown request schema");
    Registration registration;
    const auto root=fs::u8path(q.at("root").get<std::string>());
    require(root.is_absolute() && fs::is_directory(root),"existing absolute root required");
    const auto mode=q.at("mode").get<std::string>();
    if(mode=="fixtures") {
        Json generated=Json::object();
        for(const auto* name:{"a","b"}) {
            const auto dir=root/name; require(fs::create_directory(dir),"fixture directory exists");
            SaveManager s; s.init(utf8(dir)); const auto k=key(); s.setEncryptionKey(k.data());
            s.setEncryptionPolicy(SaveEncryptionPolicy::RequireEncrypted);
            const Json value{{"marker",name},{"payload",std::string(70000,*name)}};
            require(s.save(3,value,"cold.ame",7),"real CAES save failed");
            const auto raw=read(dir/"save_3.json");
            require(raw.substr(0,4)=="CAES" && s.load(3)==value,"CAES roundtrip failed");
            generated[name]=bytes(raw);
        }
        emit(Json{{"event","fixtures"},{"generated",generated},{"format","actual AES-256-GCM CAES"}}); return 0;
    }
    const auto local=fs::u8path(q.at("local_dir").get<std::string>());
    const auto cloud=fs::u8path(q.at("cloud_path").get<std::string>());
    SaveManager manager; manager.init(utf8(local)); const auto k=key(q.value("wrong_key",false));
    manager.setEncryptionKey(k.data()); manager.setEncryptionPolicy(SaveEncryptionPolicy::RequireEncrypted);
    auto owned=std::make_unique<DiskSides>(cloud); auto* transport=owned.get(); manager.setSaveProvider(std::move(owned));
    BackendRegistry::instance().setSaveManager(&manager);
    auto* api=dynamic_cast<ICloudSaveCoordinator*>(BackendRegistry::instance().getSaveManager());
    require(api!=nullptr,"optional API not exposed through registry");
    require(api->bindCloudCoordinator({utf8(root),scope,1}).code==Code::Ready,"bind failed");
    const auto context=root/scope/"1"; const auto cursor=context/"selected"/"slot_3.json";
    if(mode=="reopen" || mode=="replay") {
        const auto expected=prep(q.at("preparation"));
        const auto r=mode=="reopen"?api->reopenCloudPreparation(expected):api->prepareCloudSync(3,expected.token);
        const auto listing=api->listCloudPreparations(3); Json refs=Json::array();
        for(const auto& p:listing.preparations) refs.push_back(prep(p));
        emit(Json{{"event","readback"},{"result",result(r)},{"counts",transport->counts()},
            {"list",{{"code",code(listing.code)},{"preparations",refs},
                {"incomplete",listing.incompleteOperations},{"invalid",listing.invalidOperations}}}}); return 0;
    }
    if(mode=="export") {
        const auto r=api->reopenCloudPreparation(prep(q.at("preparation")));
        require(r.code==Code::Replayed && r.preparation,"export reopen failed");
        const auto exported=api->exportCloudHistory({*r.preparation,CloudPreservedVariant::Local,r.stamp},
            q.at("export_token").get<std::string>());
        Json b=nullptr;
        if(exported.code==Code::Complete || exported.code==Code::Replayed) {
            const auto raw=read(fs::u8path(exported.path)); b=bytes(raw);
            require(raw.substr(0,4)=="CAES","export is not original CAES");
        }
        emit(Json{{"event","exported"},{"code",code(exported.code)},{"path",exported.path},
            {"bytes",b},{"receipt_sha256",exported.receiptSha256},{"counts",transport->counts()}}); return 0;
    }
    require(mode=="seed" || mode=="write","unknown mode");
    const auto token=q.at("token").get<std::string>();
    CloudPrepareResult r; Json fault=nullptr;
    const auto boundary=q.value("boundary",std::string{});
    if(mode=="write" && boundary!="after-normal" && boundary!="after-abnormal") {
        CursorBoundary f; f.cursor=cursor; f.operation=context/"operations"/token;
        f.old=read(cursor,16384); f.boundary=boundary; f.transport=transport;
        BackendRegistry::instance().setCryptoEngine(&f);
        { detail::ScopedSaveWriteTestHook hook({&CursorBoundary::checkpoint,&f}); r=api->prepareCloudSync(3,token); }
        BackendRegistry::instance().setCryptoEngine(&registration.crypto);
        require(boundary=="after-hash-exception" && f.hits==1 && f.delegated==1 && f.armedCount==1 && !f.armed,
            "intended termination/hash boundary not reached");
        require(r.code==Code::Indeterminate && r.ancestor==CloudAncestorSelection::Indeterminate && r.preparation && r.record,
            "published cursor exception not preserved as Indeterminate");
        require(sha(read(cursor,16384))==r.candidateCursorSha256 && read(cursor,16384)==f.candidate,
            "Indeterminate discarded published candidate");
        fault=Json{{"hits",f.hits},{"delegated_real_sha",f.delegated},{"armed_count",f.armedCount},
            {"published_cursor_observed",true},{"input_equals_published_cursor",true}};
    } else {
        r=api->prepareCloudSync(3,token);
        require(r.code==Code::Complete && r.ancestor==CloudAncestorSelection::Selected && r.preparation && r.record,
            "valid equal preparation was not selected");
    }
    emit(Json{{"event",mode=="seed"?"seed":"written"},{"result",result(r)},
        {"cursor",bytes(read(cursor,16384))},{"counts",transport->counts()},{"fault",fault},
        {"termination",boundary=="after-abnormal"?"self_abnormal":"normal_return"}});
    if(boundary=="after-abnormal") abnormal();
    return 0;
}
}
int main(int argc,char** argv) {
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX|SEM_NOOPENFILEERRORBOX);
#endif
    try {require(argc==2 && std::string(argv[1])=="request.json","expected owned request filename"); ownership();
        return run(Json::parse(read("request.json",32768)));}
    catch(const std::exception& e){std::cerr<<"U26_COORD_ERROR "<<e.what()<<std::endl;return 90;}
}
