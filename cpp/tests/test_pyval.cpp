// Compares the Python-compatible decoder/repr against tools/pyval_fuzz.py.
#include <fstream>
#include <iostream>
#include <string>

#include "../src/pyval.hpp"

static std::string unhex(const std::string& h) {
    std::string out;
    for (size_t k = 0; k + 1 < h.size(); k += 2) out += char(std::stoi(h.substr(k, 2), nullptr, 16));
    return out;
}

static std::string escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else out += c;
    }
    return out;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: test_pyval FIXTURE\n";
        return 2;
    }
    std::ifstream in(argv[1]);
    std::string line;
    long n = 0, bad = 0;
    std::string last_hex;
    py::Value last_val;
    while (std::getline(in, line)) {
        size_t tab = line.find('\t');
        std::string hex = line.substr(0, tab), want = line.substr(tab + 1), got;
        if (want[0] == 'S') {
            std::string d;
            py::json_dump_str(d, last_val.s);
            got = "S " + escape(d);
        } else {
            py::Str doc;
            std::string err;
            py::Value v;
            if (!py::utf8_decode(unhex(hex), doc, err) || !py::json_loads(doc, v, err))
                got = "E " + escape(err);
            else
                got = "V " + escape(py::repr(v));
            last_val = v;
        }
        n++;
        if (got != want) {
            if (++bad <= 20)
                std::cerr << "input " << hex << "\n  want: " << want << "\n  got:  " << got << "\n";
        }
    }
    std::cout << "pyval: " << n << " cases, " << bad << " mismatches\n";
    return bad ? 1 : 0;
}
