#include <string>
#include <vector>

std::string greet(const std::string& name) {
    return "hello, " + name;
}

std::vector<int> makeNumbers() {
    std::vector<int> numbers;
    numbers.push_back(1);
    numbers.push_back(2);
    return numbers;
}

int totalLength(const std::vector<std::string>& words) {
    int total = 0;
    for (const std::string& word : words) {
        total += static_cast<int>(word.size());
    }
    return total;
}
