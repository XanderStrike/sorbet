#include <string>
#include <string_view>

#include "common/Exception.h"
#include "core/StrictLevel.h"

namespace sorbet::core {

using namespace std;

string_view strictLevelToSigil(StrictLevel level) {
    switch (level) {
        case StrictLevel::None:
            Exception::raise("Should never happen");
        case StrictLevel::Internal:
            Exception::raise("Should never happen");
        case StrictLevel::Ignore:
            return "ignore"sv;
        case StrictLevel::False:
            return "false"sv;
        case StrictLevel::True:
            return "true"sv;
        case StrictLevel::Strict:
            return "strict"sv;
        case StrictLevel::Strong:
            return "strong"sv;
        case StrictLevel::Max:
            Exception::raise("Should never happen");
        case StrictLevel::Autogenerated:
            Exception::raise("Should never happen");
        case StrictLevel::Stdlib:
            return "__STDLIB_INTERNAL"sv;
    }
}

} // namespace sorbet::core
