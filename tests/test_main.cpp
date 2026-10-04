#include <cstdio>
#include <cstring>

#include "test.h"

int main(int argc, char* argv[]) {
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int run = 0;
    for (const auto& c : vette::test::registry()) {
        if (filter && !std::strstr(c.name, filter)) {
            continue;
        }
        const int before = vette::test::failures();
        c.fn();
        std::printf("%s %s\n", vette::test::failures() == before ? "ok  " : "FAIL", c.name);
        ++run;
    }
    std::printf("%d test(s), %d failure(s)\n", run, vette::test::failures());
    return vette::test::failures() == 0 ? 0 : 1;
}
