#include <cstdint>
#include "Lifter.h"
#include <ostream>
#include "Recompiler.h"
#include "Simplifier.h"

#include <vector>
#include <string>
#include <sstream>
#include <iomanip>

// Parse a whitespace-separated hex byte string (e.g. "48 89 74")
// into a raw byte buffer.
std::vector<uint8_t> hex_to_bytes(const std::string& hex) {
    std::vector<uint8_t> bytes;
    std::stringstream ss(hex);
    std::string byte;

    while (ss >> byte)
        bytes.push_back(
            static_cast<uint8_t>(
                std::stoul(byte, nullptr, 16)
            )
        );

    return bytes;
}

// Demo pipeline: hex bytes -> Lifter -> IR -> Recompiler -> bytes.
int main() {

    auto code = hex_to_bytes("48 8D A4 24 80 FC FF FF 9C 41 53 4C 8B 9C 24 70 00 00 00 48 83 C4 F8 48 89 34 24 4C 89 DE 4C 31 E6 48 D1 EE 4D 01 E3 49 D1 DB 49 29 F3 5E 4D 31 DC 4D 01 E3 4C 89 9C 24 70 00 00 00 41 5B 9D 49 89 E4 9C 48 8D A4 24 F8 FF FF FF 48 89 34 24 49 8B B4 24 10 00 00 00 48 21 C6 48 31 F0 41 53 49 89 F3 49 31 C3 48 09 C6 48 D1 E6 4C 29 DE 41 5B 49 89 B4 24 10 00 00 00 48 8B 34 24 48 8D A4 24 08 00 00 00 9D 9C 49 8B 84 24 88 00 00 00 48 01 C1 48 29 C8 48 F7 D8 49 89 84 24 88 00 00 00 9D 9C 49 8B 8C 24 C8 00 00 00 48 29 CA 52 48 89 C8 48 F7 D2 48 F7 D1 48 09 D1 48 83 F1 FF 5A 48 D1 E1 48 31 D0 41 54 BA 25 11 BA 2F 81 C2 DB F9 B6 3E 41 89 D4 41 C1 EC 12 41 C1 D4 18 0F 84 90 35 EE FD");

    Lifter lift;
    IR ir = lift.lift(
        code.data(),
        code.size()
    );

    Simplifier simplifier;
    simplifier.simplify(ir);


    std::cout << "\n=== IR ===\n";
    std::cout << ir.statements_str() << std::endl;

    Recompiler rc;
    auto result = rc.compile(ir);

    std::cout << "\n=== ASSEMBLY ===\n";
    std::cout << result.assembly << "\n";

    std::cout << "=== HEX ===\n";
    std::cout << result.hex << "\n";


    return 0;
}