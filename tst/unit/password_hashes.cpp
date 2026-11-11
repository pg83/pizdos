//@ compile-flags: -g
//@ stdout: "password hashes: ok\n"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main() {
    // Fixed vectors were independently evaluated with the host's musl.
    const char* cases[][3] = {
        {"password", "ab", "abJnggxhB/yWI"},
        {"", "./", "./Una9Fi.seRo"},
        {"123456789-longer-than-DES", "HX", "HXKoY3/d7AKwo"},
        {"password", "$1$salt", "$1$salt$qJH7.N4xYta3aEG/dfqo/0"},
        {"", "$1$", "$1$$qRPK7m23GJusamGpoGLby/"},
        {"a long password whose length exceeds sixteen bytes", "$1$123456789abcdef$discard", "$1$12345678$x5UPca74zjAqwxzTqoPnP."},
        {"password", "$5$salt", "$5$salt$Gcm6FsVtF/Qa77ZKD.iwsJlCVPY0XSMgLJL0Hnww/c1"},
        {"", "$5$", "$5$$3c2QQ0KjIU1OLtB29cl8Fplc2WN7X89bnoEjaR7tWu."},
        {"Hello world!", "$5$rounds=000010$saltstringsaltstring$discard", "$5$rounds=1000$saltstringsaltst$cSv7nWzgUjd5.cVg4yn64CGOTkcQl1wXTNvW1OSLbZ0"},
        {"password", "$6$salt", "$6$salt$IxDD3jeSOb5eB1CX5LBsqZFVkJdido3OUILO5Ifz5iwMuTS4XMS130MTSuDDl3aCI6WouIL9AjRbLCelDCy.g."},
        {"", "$6$", "$6$$/chiBau24cE26QQVW3IfIe68Xu5.JQ4E8Ie7lcRLwqxO5cxGuBhqF2HmTL.zWJ9zjChg3yJYFXeGBQ2y3Ba1d1"},
        {"a long password whose length exceeds sixty-four bytes: 0123456789abcdef", "$6$rounds=1001$123456789abcdef012345$discard", "$6$rounds=1001$123456789abcdef0$qWBjdQNknusG.3KehmAjM1/bfjQ9/NtyQFausyWxZNlTTNBXwcKHFz2eleYndGdU/iAts/RSQAarEX2y7IyyM."},
    };
    for (auto& test : cases) {
        char* result = crypt(test[0], test[1]);
        assert(result && strcmp(result, test[2]) == 0);
        result = crypt(test[0], test[2]);
        assert(result && strcmp(result, test[2]) == 0);
    }
    const char* invalid[] = {"", "a", "$5$rounds=$salt", "$6$rounds=1234",
                             "$5$rounds=xyz$salt", "$6$rounds=18446744073709551616$salt",
                             "$5$a:b", "$6$a\nb"};
    for (auto setting : invalid) {
        errno = 0;
        char* result = crypt("password", setting);
#ifdef __wasm__
        assert(result == nullptr && errno == EINVAL);
#else
        assert(result == nullptr || result[0] == '*');
#endif
    }
#ifdef __wasm__
    // This kernel accepts DES's documented salt alphabet and rejects unknown schemes.
    const char* unsupported[] = {"!a", "a!", "$9$salt", "_extended"};
    for (auto setting : unsupported) {
        assert(crypt("password", setting) == nullptr && errno == EINVAL);
    }
#endif
    assert(strcmp(crypt("password", "ab"), "abJnggxhB/yWI") == 0);
    puts("password hashes: ok");
}
