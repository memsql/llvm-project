#include "TokenAnnotator.h"
#include "FormatToken.h"
#include "clang/Basic/SourceManager.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/Support/Debug.h"

namespace memsql {

using namespace clang;
using namespace clang::format;

static unsigned int CountLogicAndOr(const FormatToken* Left,
                                    const FormatToken* Right) {
    unsigned int count = 0;
    for (const FormatToken* Token = Left;
         Token != Right;
         Token = Token->Next) {
        if (Token->TokenText.equals("&&") ||
            Token->TokenText.equals("||")) {
          count ++;
        }
    }
    return count;
}

static bool IsConditionCheck(const std::string& token,
                             const std::vector<std::string>& Candidates) {
  if(token.compare("if") == 0 ||
     token.compare("for") == 0 ||
     token.compare("while") == 0 ||
     token.compare("assert") == 0 ) {
      return true;
  }

  if (find(Candidates.begin(), Candidates.end(), token) != Candidates.end()) {
      return true;
  }

  return false;
}

bool mustBreakBefore(const FormatStyle &Style,
                     const AnnotatedLine &Line,
                     const FormatToken &Right) {
  FormatToken *Left = Right.Previous;

  if (Left->is(clang::format::TT_CtorInitializerColon)) {
    // Go back to scan parameter list on Ctor to make wrap decision.
    bool ShouldWrapParam = false;
    for (FormatToken *Search = Line.First; Search != Left;
         Search = Search->Next) {
      if (Search->is(tok::l_paren)) {
        ShouldWrapParam = Search->ParameterCount > 1;
      }
      if (Search->is(tok::comma)) {
        Search->Next->MustBreakBefore = ShouldWrapParam;
      }
    }
    Right.Previous->MustBreakBefore = false;
    return true;
  }

  if (Left->is(TT_CtorInitializerComma)) {
    Left->Previous->MustBreakBefore = false;
    return true;
  }

  if (Left->is(TT_TemplateCloser)) {
    Left->MustBreakBefore = false;
    // Don't return here since we dont know if we want to break before Right
    // token yet.
  }

  // We do not want to break ":" inside a for stmt for(e:list).
  // but the way to avoid it is very hacky.
  // Since in ContinuationIndenter::mustBreak() it will enforce a line break
  // for TT_InlineASMColon type. So I cheat the type.
  if (Right.is(TT_InlineASMColon)) {
    Left->Next->Type = TT_Unknown;
    return false;
  }

  if (Left->is(tok::l_brace) && Left->BlockKind == BK_Block &&
      Left->Children.size() > 0) {
    return true;
  }

  if (Right.is(tok::comma) && Left->Previous &&
      (Left->Previous->TokenText.equals("if") ||
       Left->Previous->TokenText.equals("elif") ||
       Left->Previous->TokenText.equals("else") ||
       Left->Previous->TokenText.equals("endif") ||
       Left->Previous->TokenText.equals("ifdef") ||
       Left->Previous->TokenText.equals("ifndef"))) {
    return true;
  }

  if (Right.is(tok::kw_while) && Left->is(tok::r_brace)) {
    return true;
  }

  if (Right.is(tok::l_brace)) {
    // Always insert newline between ') {' or if "{" is a lambda brace.
    if (Left->is(tok::r_paren) || Right.is(TT_LambdaLBrace)) {
      return true;
    }

    // Look ahead to see if it is a brace initializer:
    // https://en.cppreference.com/w/cpp/language/aggregate_initialization
    // T object = {arg1, arg2, ...};	(1)
    // T object {arg1, arg2, ... };	(2)	(since C++11)
    if (Style.BreakBraceInitializer && Right.BlockKind != BK_Block) {
      // For either BK_Unknown or BK_BracedInit
      if (Right.ParameterCount > 2) {
        Right.Next->MustBreakBefore = true;
        for (FormatToken *Search = Right.Next;
             Search && Search != Right.MatchingParen; Search = Search->Next) {
          if (Search->is(tok::comma)) {
            Search->Next->MustBreakBefore = true;
          }
        }
        Right.MatchingParen->MustBreakBefore = true;
        return true;
      }
    }
  }

  if (Line.First->TokenText.equals("Auto") && Left->is(tok::semi) &&
      Line.Level == 0) {
    return true;
  }

  if (Left->is(tok::l_paren) && Line.MustBeDeclaration &&
      Line.MightBeFunctionDecl) {
    FormatToken *FuncName = nullptr;
    for (FuncName = Left; FuncName; FuncName = FuncName->Previous) {
      if (FuncName && FuncName->is(TT_FunctionDeclarationName)) {
        FuncName->MustBreakBefore = false;
        for (FormatToken *Search = Left;
             Search && Search != Left->MatchingParen; Search = Search->Next) {
          if (Search->is(tok::r_paren) && Search != Left->MatchingParen) {
            Search->Next->MustBreakBefore = true;
          }
        }
        if (Left->ParameterCount > 1) {
          FuncName->MustBreakBefore = true;
          return true;
        }
        break;
      }
    }
  }

  // Always line break in compound condition ("&&" and "||")
  if (Right.is(tok::r_paren)) {
    const FormatToken* L_Paren = Right.MatchingParen;
    if (L_Paren && L_Paren->Previous) {
      const FormatToken* FuncName = L_Paren->Previous;
      std::string token(FuncName->TokenText.data(), FuncName->ColumnWidth);
      if (IsConditionCheck(token, Style.CustomizeConditionCheckFunctions)) {
        unsigned int c = CountLogicAndOr(L_Paren, &Right);
        if (c > 1) {
          for (const FormatToken* Token = L_Paren; Token != &Right; Token = Token->Next) {
            if (Token->TokenText.equals("&&") ||
                Token->TokenText.equals("||")) {
              Token->Next->MustBreakBefore = true;
              Token->Previous->Next->SpacesRequiredBefore = 1;
            }
          }
        }
      }
    }
  }

  return false;
}
} // namespace memsql
