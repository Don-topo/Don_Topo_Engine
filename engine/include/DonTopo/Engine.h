#pragma once

namespace DonTopo {

class Engine {
public:
    Engine();   // throws std::runtime_error on init failure
    ~Engine();

    Engine(const Engine&)            = delete;
    Engine& operator=(const Engine&) = delete;
};

} // namespace DonTopo
