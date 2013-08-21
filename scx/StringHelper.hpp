#ifndef SCX_STRINGHELPER_HPP
#define SCX_STRINGHELPER_HPP

#include <vector>
#include <string>

namespace scx {
namespace String {

    auto Split(const std::string& str, char ch) -> std::vector<std::string>
    {
        std::vector<std::string> l;
        for (size_t beg = 0; beg < str.size(); ) {
            size_t pos = str.find(ch, beg);
            if (pos == std::string::npos) {
                l.push_back(str.substr(beg));
                return l;
            }
            size_t n = pos - beg;
            if (n > 0) {
                l.push_back(str.substr(beg, n));
            }
            beg = pos + 1;
        }
        return l;
    }

}
}

#endif
