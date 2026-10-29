#include "service/prometheus.hpp"
#include <iostream>
#include <iterator>
#include <string>

int main() {
    const std::string input((std::istreambuf_iterator<char>(std::cin)),
                            std::istreambuf_iterator<char>());
    const nativetv::Json payload = nativetv::Json::parse(input);
    if (!payload.isObject()) {
        std::cerr << "expected a JSON object on stdin\n";
        return 2;
    }
    std::cout << nativetv::service::renderPrometheus(
        payload["stats"], payload["version"].string(), payload["syslog"],
        payload["ntp"], payload["watch"]);
    return 0;
}
