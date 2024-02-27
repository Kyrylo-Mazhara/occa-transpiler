#include "attributes/attribute_names.h"
#include "core/ast_processors/okl_sema_processor/okl_sema_ctx.h"
#include "core/attribute_manager/attribute_manager.h"
#include "core/transpiler_session/session_stage.h"
#include "core/utils/attributes.h"
#include "core/utils/range_to_string.h"

namespace {
using namespace oklt;
using namespace clang;
const std::string RESTRICT_MODIFIER = "__restrict__";

HandleResult handleRestrictAttribute(const clang::Attr* a,
                                     const clang::ParmVarDecl* parmDecl,
                                     SessionStage& s) {
    auto& rewriter = s.getRewriter();

    SourceLocation identifierLoc = parmDecl->getLocation();
    removeAttribute(a, s);
    std::string restrictText = " " + RESTRICT_MODIFIER + " ";
    rewriter.InsertText(identifierLoc, restrictText, false, false);

    // INFO: might be better to use rewriter.getRewrittenText() method

    auto& ctx = parmDecl->getASTContext();
    SourceRange r1(parmDecl->getSourceRange().getBegin(), identifierLoc);
    auto part1 = getSourceText(r1, ctx);
    auto ident = parmDecl->getQualifiedNameAsString();
    std::string modifiedArgument = part1 + restrictText + ident;

    if (s.getAstProccesorType() == AstProcessorType::OKL_WITH_SEMA) {
        s.tryEmplaceUserCtx<OklSemaCtx>().setKernelArgRawString(parmDecl, modifiedArgument);
    }

#ifdef TRANSPILER_DEBUG_LOG
    llvm::outs() << "[DEBUG] DPCPP: Handle @restrict.\n";
#endif
    return true;
}

__attribute__((constructor)) void registerCUDARestrictHandler() {
    auto ok = oklt::AttributeManager::instance().registerBackendHandler(
        {TargetBackend::DPCPP, RESTRICT_ATTR_NAME},
        makeSpecificAttrHandle(handleRestrictAttribute));

    if (!ok) {
        llvm::errs() << "failed to register " << RESTRICT_ATTR_NAME
                     << " attribute handler for DPCPP backend\n";
    }
}
}  // namespace
