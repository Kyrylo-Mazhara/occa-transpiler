#include <oklt/core/error.h>

#include "core/diag/diag_consumer.h"
#include "core/transpiler_session/session_stage.h"
#include "core/utils/attributes.h"
#include "core/vfs/overlay_fs.h"

#include "pipeline/stages/normalizer/error_codes.h"
#include "pipeline/stages/normalizer/impl/gnu_to_std_cpp_stage.h"

#include <clang/AST/ASTContext.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Rewrite/Core/Rewriter.h>
#include <clang/Tooling/Tooling.h>
#include <spdlog/spdlog.h>

using namespace oklt;
using namespace clang;

namespace {
struct AttrNormalizerCtx {
    ASTContext* astCtx;
    Rewriter* rewriter;
    std::list<OklAttrMarker> markers;
};

void removeAttr(Rewriter& rewriter, const Attr& attr) {
    auto arange = getAttrFullSourceRange(attr);
    rewriter.RemoveText(arange);
}

std::string getArgAsStr(const SuppressAttr& attr) {
    if (attr.diagnosticIdentifiers_size() == 0) {
        return "";
    }
    return attr.diagnosticIdentifiers_begin()[0].str();
}

std::string getOklName(const Attr& attr) {
    return attr.getAttrName()->getName().split('_').second.str();
}

OklAttribute toOklAttr(const AnnotateAttr& attr, ASTContext& ast) {
    return OklAttribute{.raw = "",
                        .name = getOklName(attr),
                        .params = attr.getAnnotation().str(),
                        .tok_indecies = {}};
}

OklAttribute toOklAttr(const SuppressAttr& attr, ASTContext& ast) {
    assert(attr.diagnosticIdentifiers_size() != 0 && "suppress attr has 0 args");
    return OklAttribute{
        .raw = "", .name = getOklName(attr), .params = getArgAsStr(attr), .tok_indecies = {}};
}

template <typename Expr, typename AttrType>
void insertNormalizedAttr(const Expr& e, const AttrType& attr, SessionStage& stage) {
    auto oklAttr = toOklAttr(attr, stage.getCompiler().getASTContext());
    auto normalizedAttrStr = wrapAsSpecificCxxAttr(oklAttr);
    stage.getRewriter().InsertTextAfter(e.getBeginLoc(), normalizedAttrStr);
}

template <typename AttrType, typename Expr>
bool tryToNormalizeAttrExpr(Expr& e, SessionStage& stage, const Attr** lastProccesedAttr) {
    assert(lastProccesedAttr);
    for (auto* attr : e.getAttrs()) {
        if (attr->isC2xAttribute() || attr->isCXX11Attribute()) {
            continue;
        }

        if (!oklt::isOklAttribute(*attr)) {
            continue;
        }

        if ((*lastProccesedAttr) && ((*lastProccesedAttr)->getLoc() == attr->getLoc())) {
            continue;
        }

        const auto* targetAttr = dyn_cast_or_null<AttrType>(attr);
        if (!targetAttr) {
            continue;
        }

        removeAttr(stage.getRewriter(), *attr);
        insertNormalizedAttr(e, *targetAttr, stage);
        *lastProccesedAttr = attr;
    }

    return true;
}

SourceLocation getMarkerSourceLoc(const OklAttrMarker& marker, const SourceManager& srcMng) {
    return srcMng.translateLineCol(srcMng.getMainFileID(), marker.loc.line, marker.loc.col);
}

// Traverse AST and normalize GMU attributes and fix markers to standard C++ attribute
// representation
class GnuToCppAttrNormalizer : public RecursiveASTVisitor<GnuToCppAttrNormalizer> {
   public:
    explicit GnuToCppAttrNormalizer(SessionStage& stage)
        : _stage(stage) {
        auto anyCtx = _stage.getUserCtx("input");
        if (anyCtx && anyCtx->has_value()) {
            // use non-throw api by passing pointer to any
            _input = *(std::any_cast<GnuToStdCppStageInput*>(anyCtx));
        } else {
            _input = nullptr;
        }
    }

    bool VisitDecl(Decl* d) {
        assert(d != nullptr && "declaration is nullptr");

        if (!d->hasAttrs()) {
            return true;
        }
        return tryToNormalizeAttrExpr<AnnotateAttr>(*d, _stage, &_lastProccesedAttr);
    }

    bool TraverseAttributedStmt(AttributedStmt* as) {
        assert(as != nullptr && "attributed statement is nullptr");

        if (!tryToNormalizeAttrExpr<SuppressAttr>(*as, _stage, &_lastProccesedAttr)) {
            return false;
        }

        return RecursiveASTVisitor<GnuToCppAttrNormalizer>::TraverseAttributedStmt(as);
    }

   private:
    const Attr* _lastProccesedAttr{nullptr};
    SessionStage& _stage;
    GnuToStdCppStageInput* _input;
};

// ASTConsumer to run GNU to C++ attribute replacing
class GnuToCppAttrNormalizerConsumer : public ASTConsumer {
   public:
    explicit GnuToCppAttrNormalizerConsumer(SessionStage& stage)
        : _stage(stage),
          _normalizer_visitor(_stage) {}

    // Override the method that gets called for each parsed top-level
    // declaration.
    void HandleTranslationUnit(ASTContext& ctx) override {
        if (spdlog::get_level() == spdlog::level::trace) {
            ctx.getTranslationUnitDecl()->dump(llvm::outs());
        }
        TranslationUnitDecl* decl = ctx.getTranslationUnitDecl();
        _normalizer_visitor.TraverseDecl(decl);
    }

   private:
    SessionStage& _stage;
    GnuToCppAttrNormalizer _normalizer_visitor;
};

struct GnuToStdCppAttributeNormalizerAction : public clang::ASTFrontendAction {
    explicit GnuToStdCppAttributeNormalizerAction(oklt::GnuToStdCppStageInput& input,
                                                  oklt::GnuToStdCppStageOutput& output)
        : _input(input),
          _output(output),
          _session(*input.session),
          _stage(nullptr) {}

   protected:
    std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(clang::CompilerInstance& compiler,
                                                          llvm::StringRef in_file) override {
        _stage = std::make_unique<SessionStage>(_session, compiler);
        if (!_stage->setUserCtx("input", &_input)) {
            _stage->pushError(std::error_code(),
                              "failed to set user ctx for GnuToStdCppAttributeNormalizerAction");
            return nullptr;
        }
        auto consumer = std::make_unique<GnuToCppAttrNormalizerConsumer>(*_stage);
        compiler.getDiagnostics().setClient(new DiagConsumer(*_stage));

        return std::move(consumer);
    }

    bool PrepareToExecuteAction(CompilerInstance& compiler) override {
        if (compiler.hasFileManager()) {
            auto overlayFs = makeOverlayFs(compiler.getFileManager().getVirtualFileSystemPtr(),
                                           _input.gnuCppIncs);
            compiler.getFileManager().setVirtualFileSystem(overlayFs);
        }

        return true;
    }

    void EndSourceFileAction() override {
        _output.stdCppSrc = _stage->getRewriterResultForMainFile();
        // no errors and empty output could mean that the source is already normalized
        // so use input as output and lets the next stage try to figure out
        if (_output.stdCppSrc.empty()) {
            _output.stdCppSrc = std::move(_input.gnuCppSrc);
        }

        // we need keep all headers in output even there are not modififcation by rewriter to
        // populate affected files futher
        _output.stdCppIncs = _stage->getRewriterResultForHeaders();
        _output.stdCppIncs.fileMap.merge(_input.gnuCppIncs.fileMap);
    }

   private:
    oklt::GnuToStdCppStageInput& _input;
    oklt::GnuToStdCppStageOutput& _output;
    TranspilerSession& _session;
    std::unique_ptr<SessionStage> _stage;
};

}  // namespace

namespace oklt {
GnuToStdCppResult convertGnuToStdCppAttribute(GnuToStdCppStageInput input) {
    if (input.gnuCppSrc.empty()) {
        SPDLOG_ERROR("Input source string is empty");
        auto error =
            makeError(OkltNormalizerErrorCode::EMPTY_SOURCE_STRING, "input source string is empty");
        return tl::make_unexpected(std::vector<Error>{error});
    }

    Twine tool_name = "okl-transpiler-normalization-to-cxx";
    Twine file_name("main_kernel.cpp");
    std::vector<std::string> args = {"-std=c++17", "-fparse-all-comments", "-I."};

    auto input_file = std::move(input.gnuCppSrc);
    GnuToStdCppStageOutput output = {.session = input.session};

    auto& sessionInput = input.session->input;
    for (const auto& define : sessionInput.defines) {
        std::string def = "-D" + define;
        args.push_back(std::move(def));
    }

    for (const auto& includePath : sessionInput.inlcudeDirectories) {
        std::string incPath = "-I" + includePath.string();
        args.push_back(std::move(incPath));
    }

    if (!tooling::runToolOnCodeWithArgs(
            std::make_unique<GnuToStdCppAttributeNormalizerAction>(input, output),
            input_file,
            args,
            file_name,
            tool_name)) {
        return tl::make_unexpected(std::move(output.session->getErrors()));
    }

    SPDLOG_DEBUG("stage 2 STD cpp source:\n\n{}", output.stdCppSrc);

    return output;
}
}  // namespace oklt
