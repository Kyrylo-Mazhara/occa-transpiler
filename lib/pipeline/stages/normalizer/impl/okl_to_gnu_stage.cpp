#include "okl_to_gnu_stage.h"
#include "okl_attr_traverser.h"

#include <oklt/core/error.h>
#include <oklt/core/transpiler_session/session_stage.h>
#include <oklt/pipeline/stages/normalizer/error_codes.h>

#include <clang/AST/ASTContext.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Rewrite/Core/Rewriter.h>
#include <clang/Tooling/Tooling.h>
#include <set>

namespace {

using namespace clang;
using namespace oklt;

// INFO: GNU doesn't handle these attributes correctly in the statement
std::set<std::string> gnuExcepStmtAttrs{"atomic", "shared"};

bool isOklForStmtExtenstion(Token left, Token right) {
  return left.is(tok::semi) && right.is(tok::r_paren);
}

Token getLeftNeigbour(const OklAttribute& attr, const std::vector<Token>& tokens) {
  return attr.tok_indecies.front() != 0 ? tokens[attr.tok_indecies.front() - 1] : Token();
}

Token getRightNeigbour(const OklAttribute& attr, const std::vector<Token>& tokens) {
  return attr.tok_indecies.back() != tokens.size() ? tokens[attr.tok_indecies.back() + 1] : Token();
}

void removeOklAttr(const std::vector<Token>& tokens, const OklAttribute& attr, Rewriter& rewriter) {
  // remove OKL specific attribute in source code
  SourceLocation attr_loc_start(tokens[attr.tok_indecies.front()].getLocation());
  SourceLocation attr_loc_end(tokens[attr.tok_indecies.back()].getLastLoc());
  SourceRange attr_src_range(attr_loc_start, attr_loc_end);
  rewriter.RemoveText(attr_src_range);
}
////////////////////////////////////////////////////////////////////////////////////////////////////
// routine to replace OKL attribute with GNU one and store it original source location
// one trick is that functions could fix malformed C++ for statement with extra semi
bool replaceOklByGnuAttribute(std::list<OklAttrMarker>& gnu_markers,
                              std::list<OklAttrMarker>& recovery_markers,
                              const OklAttribute& oklAttr,
                              const std::vector<Token>& tokens,
                              Preprocessor& pp,
                              Rewriter& rewriter) {
  removeOklAttr(tokens, oklAttr, rewriter);

  // fix malformed C++ syntax like for(init;cond;step;@outer) to for(init;cond;step) and mark source
  // location to fix it on AST traversal
  auto leftNeigbour = getLeftNeigbour(oklAttr, tokens);
  auto rightNeighbour = getRightNeigbour(oklAttr, tokens);
  auto insertLoc(tokens[oklAttr.tok_indecies.front()].getLocation());
  if (isOklForStmtExtenstion(leftNeigbour, rightNeighbour)) {
    rewriter.ReplaceText(leftNeigbour.getLocation(), 1, ")");
    rewriter.ReplaceText(rightNeighbour.getLocation(), 1, " ");
    recovery_markers.push_back({oklAttr, insertLoc});
  } else if (gnuExcepStmtAttrs.count(oklAttr.name)) {   // TODO: fix this ugly guess
    auto atomicLoc = [&]() {
      if ((leftNeigbour.is(tok::semi) || leftNeigbour.is(tok::l_brace)) &&
          !rightNeighbour.is(tok::semi)) {
        return insertLoc;
      }
      auto it = std::find_if(tokens.rend() - (long)oklAttr.tok_indecies.front(), tokens.rend(),
                             [](const Token& t) { return t.isOneOf(tok::semi, tok::l_brace); });
      if (it == tokens.rend()) {
        return insertLoc;
      }

      return (--it)->getLocation();
    }();

    auto cppAttr = wrapAsSpecificCxxAttr(oklAttr);
    rewriter.InsertTextBefore(atomicLoc, cppAttr);
  } else {
    auto gnuAttr = wrapAsSpecificGnuAttr(oklAttr);

    rewriter.InsertTextBefore(insertLoc, gnuAttr);
    gnu_markers.push_back({oklAttr, insertLoc});
  }

#ifdef NORMALIZER_DEBUG_LOG
  llvm::outs() << "removed attr: " << oklAttr.name
               << " at loc: " << oklAttr.begin_loc.printToString(pp.getSourceManager()) << '\n';
#endif

  return true;
}

std::vector<Token> fetchTokens(Preprocessor& pp) {
  std::vector<Token> tokens;
  while (true) {
    Token tok{};
    pp.Lex(tok);
    if (tok.is(tok::eof))
      break;
    if (tok.is(tok::unknown)) {
      // Check for '@' symbol
      auto spelling = pp.getSpelling(tok);
      if (spelling.empty() || spelling[0] != '@') {
        break;
      }
      tok.setKind(tok::at);
    }
    tokens.push_back(tok);
  }

  return tokens;
}

struct OklToGnuAttributeNormalizerAction : public clang::ASTFrontendAction {
  explicit OklToGnuAttributeNormalizerAction(OklToGnuStageInput& input, OklToGnuStageOutput& output)
      : _input(input), _output(output), _session(*input.session) {
    (void)_input;
  }

 protected:
  std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(clang::CompilerInstance& compiler,
                                                        llvm::StringRef in_file) override {
    return nullptr;
  }

  bool BeginSourceFileAction(CompilerInstance& compiler) override {
    auto& pp = compiler.getPreprocessor();
    pp.EnterMainSourceFile();

    auto tokens = fetchTokens(pp);
    if (tokens.empty()) {
      _session.pushError(OkltNormalizerErrorCode::EMPTY_SOURCE_STRING, "no tokens in source?");
      return false;
    }

    SessionStage stage{_session, compiler};
    auto& rewriter = stage.getRewriter();

    auto result =
      visitOklAttributes(tokens, pp,
                         [this, &rewriter](const OklAttribute& attr,
                                           const std::vector<Token>& tokens, Preprocessor& pp) {
                           replaceOklByGnuAttribute(_output.gnuMarkers, _output.recoveryMarkers,
                                                    attr, tokens, pp, rewriter);
                           return true;
                         });
    if (!result) {
      _session.pushError(result.error().ec, result.error().desc);
      return false;
    }

    _output.gnuCppSrc = stage.getRewriterResult();

    pp.EndSourceFile();

    return false;
  }

 private:
  OklToGnuStageInput& _input;
  OklToGnuStageOutput& _output;
  TranspilerSession& _session;
};
}  // namespace
namespace oklt {

OklToGnuResult convertOklToGnuAttribute(OklToGnuStageInput input) {
  if (input.oklCppSrc.empty()) {
    llvm::outs() << "input source string is empty\n";
    auto error =
      makeError(OkltNormalizerErrorCode::EMPTY_SOURCE_STRING, "input source string is empty");
    return tl::make_unexpected(std::vector<Error>{error});
  }

#ifdef NORMALIZER_DEBUG_LOG
  llvm::outs() << "stage 0 OKL source:\n\n" << input.oklCppSrc << '\n';
#endif

  Twine tool_name = "okl-transpiler-normalization-to-gnu";
  Twine file_name("okl-kernel-to-gnu.cpp");
  std::vector<std::string> args = {"-std=c++17", "-fparse-all-comments", "-I."};

  auto input_file = std::move(input.oklCppSrc);

  OklToGnuStageOutput output = {.session = input.session};
  auto ok = tooling::runToolOnCodeWithArgs(
    std::make_unique<OklToGnuAttributeNormalizerAction>(input, output), input_file, args, file_name,
    tool_name);

  if (!ok) {
    return tl::make_unexpected(std::move(output.session->getErrors()));
  }

#ifdef NORMALIZER_DEBUG_LOG
  llvm::outs() << "stage 1 GNU cpp source:\n\n" << output.gnuCppSrc << '\n';
#endif

  return output;
}
}  // namespace oklt
