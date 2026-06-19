#include <iostream>

namespace demo {

class Greeter {
public:
    void hello() const {
        std::cout << "hello codegraph\n";
    }
};

int add(int a, int b) {
    return a + b;
}

} // namespace demo

int main() {
    demo::Greeter g;
    g.hello();
    return demo::add(1, 2);
}
