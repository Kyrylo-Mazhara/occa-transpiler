#include "core/transpiler_session/session_stage.h"
#include "core/ast_processor_manager/ast_processor_manager.h"
#include "core/attribute_manager/attribute_manager.h"
#include "core/attribute_manager/attributed_type_map.h"

#include "handlers/function.h"

#include <clang/AST/AST.h>

namespace {
using namespace clang;
using namespace oklt;

bool runPreActionDecl(const Decl* decl, SessionStage& stage) {
    auto& am = stage.getAttrManager();
    if (!decl->hasAttrs()) {
        return true;
    }

    for (const auto* attr : decl->getAttrs()) {
        if (!am.parseAttr(attr, stage)) {
            return false;
        }
    }

    return true;
}

bool runPostActionDecl(const clang::Decl* decl, SessionStage& stage) {
    auto& am = stage.getAttrManager();
    if (!decl->hasAttrs()) {
        auto cont = am.handleDecl(decl, stage);
        if (!cont) {
            return cont;
        }
        return true;
    }

    auto expectedAttr = am.checkAttrs(decl->getAttrs(), decl, stage);
    if (!expectedAttr) {
        // TODO report diagnostic error using clang tooling
        //  auto &errorReporter = _session.getErrorReporter();
        //  auto errorDescription = expectedAttr.error().message();
        //  errorReporter.emitError(funcDecl->getSourceRange(),errorDescription);
        return true;
    }

    const Attr* attr = expectedAttr.value();
    if (!attr) {
        return true;
    }

    return am.handleAttr(attr, decl, stage);
}

bool runPreActionStmt(const clang::Stmt* stmt, SessionStage& stage) {
    return true;
}

bool runPostActionStmt(const clang::Stmt* stmt, SessionStage& stage) {
    auto& am = stage.getAttrManager();
    if (stmt->getStmtClass() != Stmt::AttributedStmtClass) {
        auto cont = am.handleStmt(stmt, stage);
        if (!cont) {
            return cont;
        }
        return true;
    }

    auto* attrStmt = cast<AttributedStmt>(stmt);
    auto expectedAttr = am.checkAttrs(attrStmt->getAttrs(), stmt, stage);
    if (!expectedAttr) {
        // TODO report diagnostic error using clang tooling
        //  auto &errorReporter = _session.getErrorReporter();
        //  auto errorDescription = expectedAttr.error().message();
        //  errorReporter.emitError(stmt->getSourceRange(),errorDescription);
        return true;
    }

    const Attr* attr = expectedAttr.value();
    // INFO: no OKL attributes to process, continue
    if (!attr) {
        return true;
    }

    const Stmt* subStmt = attrStmt->getSubStmt();
    if (!am.handleAttr(attr, subStmt, stage)) {
        return true;
    }

    return true;
}

bool runPreActionRecoveryExpr(const clang::RecoveryExpr* expr, SessionStage& stage) {
    return true;
}

bool runPostActionRecoveryExpr(const clang::RecoveryExpr* expr, SessionStage& stage) {
    auto subExpr = expr->subExpressions();
    if (subExpr.empty()) {
        return true;
    }

    auto declRefExpr = dyn_cast<DeclRefExpr>(subExpr[0]);
    if (!declRefExpr) {
        return true;
    }

    auto& ctx = stage.getCompiler().getASTContext();
    auto& attrTypeMap = stage.tryEmplaceUserCtx<AttributedTypeMap>();
    auto attrs = attrTypeMap.get(ctx, declRefExpr->getType());

    auto& am = stage.getAttrManager();
    auto expectedAttr = am.checkAttrs(attrs, expr, stage);
    if (!expectedAttr) {
        // TODO report diagnostic error using clang tooling
        return true;
    }

    const Attr* attr = expectedAttr.value();
    return am.handleAttr(attr, expr, stage);
}

__attribute__((constructor)) void registerAstNodeHanlder() {
    auto& mng = AstProcessorManager::instance();
    using DeclHandle = AstProcessorManager::DeclNodeHandle;
    using StmtHandle = AstProcessorManager::StmtNodeHandle;

    auto ok = mng.registerGenericHandle(
        AstProcessorType::OKL_WITH_SEMA,
        DeclHandle{.preAction = runPreActionDecl, .postAction = runPostActionDecl});
    assert(ok);

    ok = mng.registerGenericHandle(
        AstProcessorType::OKL_WITH_SEMA,
        StmtHandle{.preAction = runPreActionStmt, .postAction = runPostActionStmt});
    assert(ok);

    ok = mng.registerSpecificNodeHandle(
        {AstProcessorType::OKL_WITH_SEMA, Decl::Function},
        makeSpecificDeclHandle(prepareOklKernelFunction, transpileOklKernelFunction));
    assert(ok);

    ok = mng.registerSpecificNodeHandle(
        {AstProcessorType::OKL_WITH_SEMA, Decl::ParmVar},
        makeSpecificDeclHandle(prepareOklKernelParam, transpileOklKernelParam));
    assert(ok);

    ok = mng.registerSpecificNodeHandle(
        {AstProcessorType::OKL_WITH_SEMA, Stmt::RecoveryExprClass},
        makeSpecificStmtHandle(runPreActionRecoveryExpr, runPostActionRecoveryExpr));
    assert(ok);
}
}  // namespace
