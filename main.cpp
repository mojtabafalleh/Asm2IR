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

    auto code = hex_to_bytes("9C 41 53 4C 8B 9C 24 70 00 00 00 48 83 C4 F8 48 89 34 24 4C 89 DE 4C 31 E6 48 D1 EE 4D 01 E3 49 D1 DB 49 29 F3 5E 4D 31 DC 4D 01 E3 4C 89 9C 24 70 00 00 00 41 5B 9D");

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