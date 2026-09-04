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

    auto code = hex_to_bytes("9C 45 29 C0 41 81 E8 79 B7 BB A6 41 F7 D8 9D");

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