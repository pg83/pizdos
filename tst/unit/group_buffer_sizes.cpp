//@ compile-flags: -g
//@ stdout: "group buffer sizes: ok\n"

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main() {
    int count = getgroups(0, nullptr);
    assert(count >= 0);
    auto groups = static_cast<gid_t*>(malloc((size_t(count) + 2) * sizeof(gid_t)));
    assert(groups != nullptr);
    const int invalidSizes[] = {-1, INT_MIN};
    for (int size : invalidSizes) {
        for (int i = 0; i < count + 2; ++i) {
            groups[i] = gid_t(0x5a5a5a5a);
        }
        errno = 0;
        assert(getgroups(size, groups + 1) == -1 && errno == EINVAL);
        for (int i = 0; i < count + 2; ++i) {
            assert(groups[i] == gid_t(0x5a5a5a5a));
        }
    }
    if (count > 1) {
        assert(getgroups(count - 1, groups + 1) == -1 && errno == EINVAL);
    }
    assert(getgroups(INT_MAX, groups + 1) == count);
    assert(groups[0] == gid_t(0x5a5a5a5a) && groups[count + 1] == gid_t(0x5a5a5a5a));
    free(groups);
    puts("group buffer sizes: ok");
}
