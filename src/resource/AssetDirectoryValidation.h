#pragma once
#include "api/IAssetProvider.h"
namespace Caesura::AssetDirectoryDetail {
inline bool utf8(const std::string& value) {
    for(size_t i=0;i<value.size();) {
        const auto lead=static_cast<unsigned char>(value[i++]);
        if(lead<0x80) {if(lead<32||lead==127)return false;continue;}
        unsigned count=0;uint32_t point=0,minimum=0;
        if(lead>=0xC2&&lead<=0xDF){count=1;point=lead&31;minimum=0x80;}
        else if(lead>=0xE0&&lead<=0xEF){count=2;point=lead&15;minimum=0x800;}
        else if(lead>=0xF0&&lead<=0xF4){count=3;point=lead&7;minimum=0x10000;}
        else return false;
        if(count>value.size()-i)return false;
        while(count--){const auto c=static_cast<unsigned char>(value[i++]);if((c&0xC0)!=0x80)return false;point=(point<<6)|(c&63);}
        if(point<minimum||point>0x10FFFF||(point>=0xD800&&point<=0xDFFF))return false;
    }
    return true;
}
inline bool normalize(const std::string& input,std::string& output) {
    if(input.empty()||input.size()>4096||input.front()=='/'||!utf8(input)
        ||input.find('\\')!=std::string::npos||input.find(':')!=std::string::npos
        ||input.find("..")!=std::string::npos)return false;
    output=input;while(!output.empty()&&output.back()=='/')output.pop_back();
    if(output.empty())return false;
    size_t begin=0;
    do {auto end=output.find('/',begin);auto part=output.substr(begin,end==std::string::npos?end:end-begin);
        if(part.empty()||part==".")return false;
        if(end==std::string::npos)break;begin=end+1;
    }while(true);
    return true;
}
inline bool leaf(const std::string& value) {
    std::string normalized;
    return value.find('/')==std::string::npos&&normalize(value,normalized)&&normalized==value;
}
inline bool limits(size_t entries,size_t bytes) {
    return entries>0&&entries<=kMaxAssetDirectoryEntries&&bytes>0&&bytes<=kMaxAssetDirectoryNameBytes;
}
}
