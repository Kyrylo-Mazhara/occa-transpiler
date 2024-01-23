#include "clang/Basic/DiagnosticSema.h"
#include "clang/Sema/ParsedAttr.h"
#include "clang/Sema/Sema.h"
#include "oklt/core/attribute_names.h"

namespace {

using namespace clang;
using namespace oklt;

constexpr ParsedAttrInfo::Spelling SHARED_ATTRIBUTE_SPELLINGS[] = {
  {ParsedAttr::AS_CXX11, "shared"},
  {ParsedAttr::AS_CXX11, SHARED_ATTR_NAME},
  {ParsedAttr::AS_GNU, "okl_shared"}};

struct SharedAttribute : public ParsedAttrInfo {
  SharedAttribute() {
    NumArgs = 1;
    OptArgs = 0;
    Spellings = SHARED_ATTRIBUTE_SPELLINGS;
    AttrKind = clang::AttributeCommonInfo::AT_Annotate;
  }

  bool diagAppertainsToDecl(clang::Sema& sema,
                            const clang::ParsedAttr& attr,
                            const clang::Decl* decl) const override {
    // INFO: this decl function can be saved to global map.
    //       in this case there is no need to make attribute !!!
    // INFO: this attribute appertains to functions only.
    if (!isa<VarDecl>(decl)) {
      sema.Diag(attr.getLoc(), diag::warn_attribute_wrong_decl_type_str)
        << attr << attr.isDeclspecAttribute() << "functions";
      return false;
    }
    return true;
  }
};

ParsedAttrInfoRegistry::Add<SharedAttribute> register_okl_shared(SHARED_ATTR_NAME, "");
}  // namespace
