#pragma once

#include "rscpp/model/value.hpp"

#include <future>
#include <memory>

namespace rscpp {

struct Suspend {
    std::shared_ptr<std::shared_future<Value>> future;
    Value fallback;
};

}