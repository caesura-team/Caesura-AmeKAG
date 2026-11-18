#include "archive/CARCWriter.h"

#include <cstdint>
#include <fstream>
#include <string>

int main() {
    std::ifstream members("members.txt");
    if (!members) return 1;

    Caesura::carc::CARCWriter writer;
    if (!writer.create("crafted.carc")) return 1;
    const std::string payload = "archived bytes";
    std::string member;
    while (std::getline(members, member)) {
        if (!writer.addFile(member, reinterpret_cast<const uint8_t*>(payload.data()), payload.size())) {
            return 1;
        }
    }
    return members.eof() && writer.finalize() ? 0 : 1;
}
