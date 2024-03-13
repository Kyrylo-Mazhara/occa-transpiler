#include "attributes/frontend/params/loop.h"

#include <string>


namespace clang {
class Rewriter;
}

namespace oklt {
struct OklLoopInfo;
}

namespace oklt::dpcpp {
std::string axisToStr(const Axis& axis);
std::string getIdxVariable(const AttributedLoop& loop);
std::string buildInnerOuterLoopIdxLine(const OklLoopInfo& forLoop,
                                       const AttributedLoop& loop,
                                       int& openedScopeCounter,
                                       clang::Rewriter& rewriter);
}  // namespace oklt::dpcpp
