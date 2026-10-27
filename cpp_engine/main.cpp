//
// Usage sketch -- same interface, three markets. Mirrors
// `python -m python_engine` and should print the same fills.
//

#include "call_auction/call_auction.h"
#include "call_auction/rules/discriminatory.h"
#include "call_auction/rules/uniform.h"
#include "continuous_auction/continuous_auction.h"
#include "orderbook/OrderArena.h"

#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace {
    std::vector<Order *> flow(OrderArena &arena) {
        constexpr TraderId trader = 1;
        return {
            arena.create(Side::SELL, Price{101}, Quantity{5}, trader),
            arena.create(Side::SELL, Price{103}, Quantity{5}, trader),
            arena.create(Side::BUY, Price{99}, Quantity{4}, trader),
            arena.create(Side::BUY, Price{104}, Quantity{8}, trader),
        };
    }

    void report(const std::string &name, const MatchingEngine &engine) {
        std::cout << std::setw(20) << name << ": ";
        if (engine.trades.empty()) std::cout << "none";
        for (std::size_t i = 0; i < engine.trades.size(); ++i) {
            if (i != 0) std::cout << ", ";
            std::cout << engine.trades[i].quantity << "@" << engine.trades[i].price;
        }
        std::cout << "\n";
    }
}

int main() {
    OrderArena arena;

    ContinuousDoubleAuction continuous;
    for (Order *o: flow(arena)) continuous.submit(o);
    report("continuous", continuous);

    Uniform uniform;
    CallAuction uniformCall(uniform);
    for (Order *o: flow(arena)) uniformCall.submit(o);
    uniformCall.run();
    report("uniform call", uniformCall);

    Discriminatory discriminatory;
    CallAuction discriminatoryCall(discriminatory);
    for (Order *o: flow(arena)) discriminatoryCall.submit(o);
    discriminatoryCall.run();
    report("discriminatory call", discriminatoryCall);

    return 0;
}
