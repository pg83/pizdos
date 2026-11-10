/*
 * lib/cxx's C++ library over lib/c, one line of deterministic results per
 * area, for test_cxx.py to compare: static and thread_local objects,
 * strings, containers, streams, files, algorithms, memory, utilities,
 * locks, time, random numbers, regular expressions, locales, conversions
 * and formatting, and RTTI.
 */
#include <algorithm>
#include <any>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <list>
#include <locale>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <random>
#include <regex>
#include <set>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <typeinfo>
#include <unordered_map>
#include <variant>
#include <vector>

namespace {
    struct Order {
        explicit Order(const char* name)
            : name_(name)
        {
            std::printf("construct %s\n", name_);
        }

        ~Order() {
            std::printf("destroy %s\n", name_);
        }

        const char* name_;
    };

    struct Shape {
        virtual ~Shape() = default;

        virtual int sides() const = 0;
    };

    struct Square: public Shape {
        int sides() const override {
            return 4;
        }
    };

    Order first("first");
    Order second("second");

    void strings() {
        std::string s = "hello";
        s += ", world";
        std::string_view v = s;
        std::printf("string %s %zu %d %s %d %.2f %s %d\n", s.substr(7).c_str(), s.find("world"), v.starts_with("hel"),
                    std::to_string(-42).c_str(), std::stoi("123"), std::stod("2.5"), std::string(3, 'x').c_str(),
                    s.compare("hello") > 0);
    }

    void containers() {
        std::vector<int> v{5, 3, 9, 1};
        std::sort(v.begin(), v.end());
        std::map<std::string, int> m{{"b", 2}, {"a", 1}};
        std::unordered_map<int, std::string> u{{7, "seven"}};
        std::set<int> st(v.begin(), v.end());
        std::deque<int> d{1, 2};
        d.push_front(0);
        std::list<int> l{3, 1, 2};
        l.sort();
        std::printf("containers %d %d %d %s %s %zu %d %d %d\n", v[0], v[3], m.begin()->second, m.rbegin()->first.c_str(),
                    u[7].c_str(), st.size(), d.front(), l.front(), l.back());
    }

    void streams() {
        std::ostringstream o;
        o << std::hex << 255 << std::dec << " " << std::setw(5) << std::setfill('.') << 42 << " " << std::fixed
          << std::setprecision(3) << 3.14159 << " " << std::boolalpha << true;
        std::istringstream in("12 3.5 word");
        int i;
        double x;
        std::string w;
        in >> i >> x >> w;
        std::wostringstream wo;
        wo << L"wide " << 7;
        std::cout << "streams " << o.str() << " " << i << " " << x << " " << w << " " << wo.str().size() << std::endl;
    }

    void files() {
        namespace fs = std::filesystem;
        fs::path dir = fs::temp_directory_path() / "cxx";
        fs::create_directories(dir / "sub");
        {
            std::ofstream out(dir / "a.txt");
            out << "line one\nline two\n";
        }
        std::ifstream in(dir / "a.txt");
        std::string first;
        std::getline(in, first);
        fs::copy_file(dir / "a.txt", dir / "b.txt");
        auto entries = std::distance(fs::directory_iterator(dir), fs::directory_iterator());
        std::printf("files %s %ju %td %d %s %ju\n", first.c_str(), static_cast<std::uintmax_t>(fs::file_size(dir / "b.txt")),
                    entries, fs::is_directory(dir / "sub"), (dir / "a.txt").extension().c_str(),
                    static_cast<std::uintmax_t>(fs::remove_all(dir)));
    }

    void algorithms() {
        std::vector<int> v(5);
        std::iota(v.begin(), v.end(), 1);
        std::vector<int> squares;
        std::transform(v.begin(), v.end(), std::back_inserter(squares), [](int n) {
            return n * n;
        });
        std::vector<int> sums(5);
        std::partial_sum(v.begin(), v.end(), sums.begin());
        auto [lo, hi] = std::minmax_element(squares.begin(), squares.end());
        std::printf("algorithms %d %d %d %d %d\n", std::accumulate(v.begin(), v.end(), 0), squares[4], sums[4], *lo, *hi);
    }

    void memory() {
        auto u = std::make_unique<int>(5);
        auto s = std::make_shared<std::string>("shared");
        std::weak_ptr<std::string> w = s;
        long before = s.use_count();
        auto copy = s;
        int deleted = 0;
        {
            std::shared_ptr<int> custom(new int(1), [&](int* p) {
                deleted = *p + 1;
                delete p;
            });
        }
        std::printf("memory %d %ld %ld %d %d\n", *u, before, s.use_count(), !w.expired(), deleted);
    }

    void utilities() {
        std::optional<int> none;
        std::optional<int> some = 3;
        std::variant<int, std::string> var = std::string("variant");
        std::string seen = std::visit(
            [](auto&& value) -> std::string {
                if constexpr (std::is_same_v<std::decay_t<decltype(value)>, int>) {
                    return "int";
                } else {
                    return value;
                }
            },
            var);
        std::any a = 2.5;
        auto t = std::make_tuple(1, std::string("two"), 3.0);
        std::function<int(int, int)> add = std::plus<int>();
        auto bound = std::bind(add, std::placeholders::_1, 10);
        std::printf("utilities %d %d %s %.1f %s %d\n", none.value_or(-1), *some, seen.c_str(), std::any_cast<double>(a),
                    std::get<1>(t).c_str(), bound(5));
    }

    void locks() {
        std::mutex m;
        std::recursive_mutex r;
        std::once_flag once;
        int calls = 0;
        {
            std::lock_guard<std::mutex> g(m);
            std::lock_guard<std::recursive_mutex> a(r);
            std::lock_guard<std::recursive_mutex> b(r);
        }
        std::call_once(once, [&] {
            calls++;
        });
        std::call_once(once, [&] {
            calls++;
        });
        std::unique_lock<std::mutex> u(m, std::try_to_lock);
        std::shared_mutex s;
        int readers = 0;
        {
            std::shared_lock<std::shared_mutex> a(s);
            std::shared_lock<std::shared_mutex> b(s);
            readers = a.owns_lock() + b.owns_lock();
        }
        std::unique_lock<std::shared_mutex> writer(s, std::try_to_lock);
        std::printf("locks %d %d %d %d %d\n", calls, u.owns_lock(), std::this_thread::get_id() == std::this_thread::get_id(),
                    readers, writer.owns_lock());
    }

    void clocks() {
        using namespace std::chrono;
        auto a = steady_clock::now();
        auto b = steady_clock::now();
        auto d = duration_cast<milliseconds>(seconds(2) + milliseconds(500));
        std::printf("time %d %lld %lld\n", b >= a, static_cast<long long>(d.count()),
                    static_cast<long long>(duration_cast<minutes>(hours(1)).count()));
    }

    void engines() {
        std::mt19937 g(1);
        std::mt19937_64 g64(1);
        unsigned first = g();
        unsigned long long first64 = g64();
        std::minstd_rand lcg(1);
        std::printf("random %u %llu %u\n", first, first64, static_cast<unsigned>(lcg()));
    }

    void expressions() {
        std::regex date(R"((\d{4})-(\d{2})-(\d{2}))");
        std::smatch m;
        std::string text = "on 2024-03-15 we";
        bool found = std::regex_search(text, m, date);
        std::printf("regex %d %s %s %s\n", found, m[1].str().c_str(), m[3].str().c_str(),
                    std::regex_replace(std::string("a1b2"), std::regex("[0-9]"), "#").c_str());
    }

    void locales() {
        std::locale classic = std::locale::classic();
        auto& ctype = std::use_facet<std::ctype<char>>(classic);
        std::printf("locale %s %c %d %d\n", classic.name().c_str(), ctype.toupper('q'), std::isalpha('z', classic),
                    std::isdigit('x', classic));
    }

    void conversions() {
        char buffer[32];
        auto end = std::to_chars(buffer, buffer + sizeof buffer, 0.1).ptr;
        *end = 0;
        double back = 0;
        std::from_chars(buffer, end, back);
        int hex = 0;
        const char* digits = "ff";
        std::from_chars(digits, digits + 2, hex, 16);
        std::printf("conversions %s %d %d %s\n", buffer, back == 0.1, hex,
                    std::format("{} {:x} {:>4} {:.2f}", 42, 255, "ab", 1.0 / 3).c_str());
    }

    void rtti() {
        std::unique_ptr<Shape> shape = std::make_unique<Square>();
        Square* square = dynamic_cast<Square*>(shape.get());
        std::printf("rtti %d %d %d\n", square != nullptr, square ? square->sides() : 0, typeid(*shape) == typeid(Square));
    }
}

int main() {
    thread_local Order local("thread_local");
    strings();
    containers();
    streams();
    files();
    algorithms();
    memory();
    utilities();
    locks();
    clocks();
    engines();
    expressions();
    locales();
    conversions();
    rtti();

    return 0;
}
