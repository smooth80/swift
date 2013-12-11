//===--- ParseDecl.cpp - Swift Language Parser for Declarations -----------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// Declaration Parsing and AST Building
//
//===----------------------------------------------------------------------===//

#include "swift/Parse/Parser.h"
#include "swift/Parse/CodeCompletionCallbacks.h"
#include "swift/Parse/DelayedParsingCallbacks.h"
#include "swift/Parse/Lexer.h"
#include "swift/Subsystems.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/Attr.h"
#include "swift/AST/Diagnostics.h"
#include "swift/Basic/Fallthrough.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/SaveAndRestore.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/ADT/Twine.h"
using namespace swift;

/// \brief Main entrypoint for the parser.
///
/// \verbatim
///   top-level:
///     stmt-brace-item*
///     decl-sil       [[only in SIL mode]
///     decl-sil-stage [[only in SIL mode]
/// \endverbatim
bool Parser::parseTopLevel() {
  SF.ASTStage = SourceFile::Parsing;

  // Prime the lexer.
  if (Tok.is(tok::NUM_TOKENS))
    consumeToken();

  CurDeclContext = &SF;

  // Parse the body of the file.
  SmallVector<ASTNode, 128> Items;

  skipExtraTopLevelRBraces();

  // If we are in SIL mode, and if the first token is the start of a sil
  // declaration, parse that one SIL function and return to the top level.  This
  // allows type declarations and other things to be parsed, name bound, and
  // type checked in batches, similar to immediate mode.  This also enforces
  // that SIL bodies can only be at the top level.
  if (Tok.is(tok::kw_sil)) {
    assert(isInSILMode() && "'sil' should only be a keyword in SIL mode");
    parseDeclSIL();
  } else if (Tok.is(tok::kw_sil_stage)) {
    assert(isInSILMode() && "'sil' should only be a keyword in SIL mode");
    parseDeclSILStage();
  } else if (Tok.is(tok::kw_sil_vtable)) {
    assert(isInSILMode() && "'sil' should only be a keyword in SIL mode");
    parseSILVTable();
  } else if (Tok.is(tok::kw_sil_global)) {
    assert(isInSILMode() && "'sil' should only be a keyword in SIL mode");
    parseSILGlobal();
  } else {
    parseBraceItems(Items,
                    allowTopLevelCode() ? BraceItemListKind::TopLevelCode
                                        : BraceItemListKind::TopLevelLibrary);
  }

  // If this is a Main source file, determine if we found code that needs to be
  // executed (this is used by the repl to know whether to compile and run the
  // newly parsed stuff).
  bool FoundTopLevelCodeToExecute = false;
  if (allowTopLevelCode()) {
    for (auto V : Items)
      if (isa<TopLevelCodeDecl>(V.get<Decl*>()))
        FoundTopLevelCodeToExecute = true;
  }

  // Add newly parsed decls to the module.
  for (auto Item : Items)
    if (Decl *D = Item.dyn_cast<Decl*>())
      SF.Decls.push_back(D);

  // Note that the source file is fully parsed and verify it.
  SF.ASTStage = SourceFile::Parsed;
  verify(SF);

  State->markParserPosition(Tok.getLoc(), PreviousLoc);

  return FoundTopLevelCodeToExecute;
}

bool Parser::skipExtraTopLevelRBraces() {
  if (!Tok.is(tok::r_brace))
    return false;
  while (Tok.is(tok::r_brace)) {
    diagnose(Tok, diag::extra_rbrace)
        .fixItRemove(Tok.getLoc());
    consumeToken();
  }
  return true;
}

/// \verbatim
///   attribute:
///     'asmname' '=' identifier
///     'infix' '=' numeric_constant
///     'unary'
///     'stdlib'
///     'weak'
///     'unowned'
///     'noreturn'
///     'optional'
/// \endverbatim
bool Parser::parseDeclAttribute(DeclAttributes &Attributes) {
  // If this not an identifier, the attribute is malformed.
  if (Tok.isNot(tok::identifier) &&
      Tok.isNot(tok::kw_in) &&
      Tok.isNot(tok::kw_weak) &&
      Tok.isNot(tok::kw_unowned)) {
    diagnose(Tok, diag::expected_attribute_name);
    return true;
  }

  // Determine which attribute it is, and diagnose it if unknown.
  AttrKind attr = llvm::StringSwitch<AttrKind>(Tok.getText())
#define ATTR(X) .Case(#X, AK_##X)
#include "swift/AST/Attr.def"
               .Default(AK_Count);
  
  if (attr == AK_Count) {
    StringRef Text = Tok.getText();
    bool isTypeAttribute = false
#define TYPE_ATTR(X) || Text == #X
#include "swift/AST/Attr.def"
    ;
    
    if (isTypeAttribute)
      diagnose(Tok, diag::type_attribute_applied_to_decl);
    else
      diagnose(Tok, diag::unknown_attribute, Tok.getText());
    // Recover by eating @foo when foo is not known.
    consumeToken();
      
    // Recovery by eating "@foo=bar" if present.
    if (consumeIf(tok::equal)) {
      if (Tok.is(tok::identifier) ||
          Tok.is(tok::integer_literal) ||
          Tok.is(tok::floating_literal))
        consumeToken();
    }
    return true;
  }
  
  // Ok, it is a valid attribute, eat it, and then process it.
  SourceLoc Loc = consumeToken();
  
  // Diagnose duplicated attributes.
  if (Attributes.has(attr))
    diagnose(Loc, diag::duplicate_attribute);
  else
    Attributes.setAttr(attr, Loc);

  // Handle any attribute-specific processing logic.
  switch (attr) {
  default: break;
  // Ownership attributes.
  case AK_weak:
  case AK_unowned:
    // Test for duplicate entries by temporarily removing this one.
    Attributes.clearAttribute(attr);
    if (Attributes.hasOwnership()) {
      diagnose(Loc, diag::duplicate_attribute);
      break;
    }
    Attributes.setAttr(attr, Loc);
    break;
      
      
  // Resilience attributes.
  case AK_resilient:
  case AK_fragile:
  case AK_born_fragile:
    // Test for duplicate entries by temporarily removing this one.
    Attributes.clearAttribute(attr);
    if (Attributes.getResilienceKind() != Resilience::Default) {
      diagnose(Loc, diag::duplicate_attribute);
      break;
    }
    Attributes.setAttr(attr, Loc);
    break;
      
  case AK_prefix:
    if (Attributes.isPostfix()) {
      diagnose(Loc, diag::cannot_combine_attribute, "postfix");
      Attributes.clearAttribute(attr);
    }
    break;
    
  case AK_postfix:
    if (Attributes.isPrefix()) {
      diagnose(Loc, diag::cannot_combine_attribute, "prefix");
      Attributes.clearAttribute(attr);
    }
    break;

  case AK_asmname: {
    if (!consumeIf(tok::equal)) {
      diagnose(Loc, diag::asmname_expected_equals);
      Attributes.clearAttribute(attr);
      return false;
    }

    if (Tok.isNot(tok::string_literal)) {
      diagnose(Loc, diag::asmname_expected_string_literal);
      Attributes.clearAttribute(attr);
     return false;
    }

    SmallVector<Lexer::StringSegment, 1> Segments;
    L->getStringLiteralSegments(Tok, Segments);
    if (Segments.size() != 1 ||
        Segments.front().Kind == Lexer::StringSegment::Expr) {
      diagnose(Loc, diag::asmname_interpolated_string);
      Attributes.clearAttribute(attr);
    } else {
      Attributes.AsmName = StringRef(
          SourceMgr->getMemoryBuffer(BufferID)->getBufferStart() +
              SourceMgr.getLocOffsetInBuffer(Segments.front().Loc, BufferID),
          Segments.front().Length);
    }
    consumeToken(tok::string_literal);
    break;
  }
  }

  return false;
}


/// \verbatim
///   attribute-type:
///     'noreturn'
/// \endverbatim
bool Parser::parseTypeAttribute(TypeAttributes &Attributes) {
  // If this not an identifier, the attribute is malformed.
  if (Tok.isNot(tok::identifier) && !Tok.is(tok::kw_in)) {
    diagnose(Tok, diag::expected_attribute_name);
    return true;
  }
  
  // Determine which attribute it is, and diagnose it if unknown.
  TypeAttrKind attr = llvm::StringSwitch<TypeAttrKind>(Tok.getText())
#define TYPE_ATTR(X) .Case(#X, TAK_##X)
#include "swift/AST/Attr.def"
    .Default(TAK_Count);
  
  if (attr == TAK_Count) {
    StringRef Text = Tok.getText();
    bool isDeclAttribute = false
#define ATTR(X) || Text == #X
#include "swift/AST/Attr.def"
    ;
    
    if (isDeclAttribute)
      diagnose(Tok, diag::decl_attribute_applied_to_type);
    else
      diagnose(Tok, diag::unknown_attribute, Tok.getText());

    // Recover by eating @foo when foo is not known.
    consumeToken();
      
    // Recovery by eating "@foo=bar" if present.
    if (consumeIf(tok::equal)) {
      if (Tok.is(tok::identifier) ||
          Tok.is(tok::integer_literal) ||
          Tok.is(tok::floating_literal))
        consumeToken();
    }
    return true;
  }
  
  // Ok, it is a valid attribute, eat it, and then process it.
  SourceLoc Loc = consumeToken();
  
  // Diagnose duplicated attributes.
  if (Attributes.has(attr))
    diagnose(Loc, diag::duplicate_attribute);
  else
    Attributes.setAttr(attr, Loc);
  
  // Handle any attribute-specific processing logic.
  switch (attr) {
  default: break;
  case TAK_local_storage:
  case TAK_sil_self:
    if (!isInSILMode()) {   // SIL's 'local_storage' type attribute.
      diagnose(Loc, diag::only_allowed_in_sil, "local_storage");
      Attributes.clearAttribute(attr);
    }
    break;
    
  // Ownership attributes.
  case TAK_sil_weak:
  case TAK_sil_unowned:
    Attributes.clearAttribute(attr);
    if (!isInSILMode()) {
      diagnose(Loc, diag::only_allowed_in_sil, "local_storage");
      return false;
    }
      
    if (Attributes.hasOwnership()) {
      diagnose(Loc, diag::duplicate_attribute);
      break;
    }
    Attributes.setAttr(attr, Loc);
    break;

  // 'inout' attribute.
  case TAK_inout:
    // Verify that we're not combining this attribute incorrectly.  Cannot be
    // both inout and auto_closure.
    if (Attributes.has(TAK_auto_closure)) {
      diagnose(Loc, diag::cannot_combine_attribute, "auto_closure");
      Attributes.clearAttribute(TAK_inout);
    }
    break;

  case TAK_auto_closure:
    if (Attributes.has(TAK_inout)) {
      // Verify that we're not combining this attribute incorrectly.  Cannot be
      // both inout and auto_closure.
      diagnose(Loc, diag::cannot_combine_attribute, "inout");
      Attributes.clearAttribute(TAK_auto_closure);
    }
    break;
      
      
    // 'cc' attribute.
  case TAK_cc: {
    // Parse the cc name in parens.
    SourceLoc beginLoc = Tok.getLoc(), nameLoc, endLoc;
    StringRef name;
    if (consumeIfNotAtStartOfLine(tok::l_paren)) {
      if (Tok.is(tok::identifier)) {
        nameLoc = Tok.getLoc();
        name = Tok.getText();
        consumeToken();
      } else {
        diagnose(Tok, diag::cc_attribute_expected_name);
      }
      parseMatchingToken(tok::r_paren, endLoc,
                         diag::cc_attribute_expected_rparen,
                         beginLoc);
    } else {
      diagnose(Tok, diag::cc_attribute_expected_lparen);
    }
    
    if (!name.empty()) {
      Attributes.cc = llvm::StringSwitch<Optional<AbstractCC>>(name)
        .Case("freestanding", AbstractCC::Freestanding)
        .Case("method", AbstractCC::Method)
        .Case("cdecl", AbstractCC::C)
        .Case("objc_method", AbstractCC::ObjCMethod)
        .Default(Nothing);
      if (!Attributes.cc) {
        diagnose(nameLoc, diag::cc_attribute_unknown_cc_name, name);
        Attributes.clearAttribute(attr);
      }
    }
    return false;
  }
  }
  
  return false;
}


/// \brief This is the internal implementation of \c parseDeclAttributeList,
/// which we expect to be inlined to handle the common case of an absent
/// attribute list.
///
/// \verbatim
///   attribute-list:
///     /*empty*/
///     attribute-list-clause attribute-list
///   attribute-list-clause:
///     '@' attribute
///     '@' attribute ','? attribute-list-clause
/// \endverbatim
bool Parser::parseDeclAttributeListPresent(DeclAttributes &Attributes) {
  Attributes.AtLoc = Tok.getLoc();
  do {
    if (parseToken(tok::at_sign, diag::expected_in_attribute_list) ||
        parseDeclAttribute(Attributes))
      return true;
 
    // Attribute lists allow, but don't require, separating commas.
  } while (Tok.is(tok::at_sign) || consumeIf(tok::comma));
  
  return false;
}

/// \brief This is the internal implementation of \c parseTypeAttributeList,
/// which we expect to be inlined to handle the common case of an absent
/// attribute list.
///
/// \verbatim
///   attribute-list:
///     /*empty*/
///     attribute-list-clause attribute-list
///   attribute-list-clause:
///     '@' attribute
///     '@' attribute ','? attribute-list-clause
/// \endverbatim
bool Parser::parseTypeAttributeListPresent(TypeAttributes &Attributes) {
  Attributes.AtLoc = Tok.getLoc();
  do {
    if (parseToken(tok::at_sign, diag::expected_in_attribute_list) ||
        parseTypeAttribute(Attributes))
      return true;
    
    // Attribute lists don't require separating commas.
  } while (Tok.is(tok::at_sign) || consumeIf(tok::comma));
  
  return false;
}

bool Parser::isStartOfOperatorDecl(const Token &Tok, const Token &Tok2) {
  return Tok.isContextualKeyword("operator")
    && (Tok2.isContextualKeyword("prefix")
        || Tok2.isContextualKeyword("postfix")
        || Tok2.isContextualKeyword("infix"));
}

void Parser::consumeDecl(ParserPosition BeginParserPosition, unsigned Flags,
                         bool IsTopLevel) {
  backtrackToPosition(BeginParserPosition);
  SourceLoc BeginLoc = Tok.getLoc();
  // Consume tokens up to code completion token.
  while (Tok.isNot(tok::code_complete)) {
    consumeToken();
  }
  // Consume the code completion token, if there is one.
  consumeIf(tok::code_complete);
  SourceLoc EndLoc = Tok.getLoc();
  State->delayDecl(PersistentParserState::DelayedDeclKind::Decl, Flags,
                   CurDeclContext, { BeginLoc, EndLoc },
                   BeginParserPosition.PreviousLoc);

  if (IsTopLevel) {
    // Skip the rest of the file to prevent the parser from constructing the
    // AST for it.  Forward references are not allowed at the top level.
    skipUntil(tok::eof);
  }
}

void Parser::setLocalDiscriminator(ValueDecl *D) {
  // If we're not in a local context, this is unnecessary.
  if (!CurFunction) return;

  Identifier name = D->getName();
  assert(!name.empty() &&
         "setting a local discriminator on an anonymous decl; "
         "maybe the name hasn't been set yet?");
  unsigned discriminator = CurFunction->LocalDiscriminators[name]++;
  D->setLocalDiscriminator(discriminator);
}

/// \brief Parse a single syntactic declaration and return a list of decl
/// ASTs.  This can return multiple results for var decls that bind to multiple
/// values, structs that define a struct decl and a constructor, etc.
///
/// \verbatim
///   decl:
///     decl-typealias
///     decl-extension
///     decl-var
///     decl-func
///     decl-enum
///     decl-struct
///     decl-import
///     decl-operator
/// \endverbatim
ParserStatus Parser::parseDecl(SmallVectorImpl<Decl*> &Entries,
                               unsigned Flags) {
  ParserPosition BeginParserPosition;
  if (isCodeCompletionFirstPass())
    BeginParserPosition = getParserPosition();
  
  DeclAttributes Attributes;
  parseDeclAttributeList(Attributes);

  // If we see the 'static' keyword, parse it now.
  SourceLoc StaticLoc;
  bool UnhandledStatic = false;
  if (Tok.is(tok::kw_static)) {
    StaticLoc = consumeToken();
    UnhandledStatic = true;
  }
  
  ParserResult<Decl> DeclResult;
  ParserStatus Status;
  switch (Tok.getKind()) {
  case tok::kw_import:
    DeclResult = parseDeclImport(Flags, Attributes);
    Status = DeclResult;
    break;
  case tok::kw_extension:
    DeclResult = parseDeclExtension(Flags, Attributes);
    Status = DeclResult;
    break;
  case tok::kw_var:
    // TODO: Static properties are only implemented for non-generic value types.
    if (StaticLoc.isValid()) {
      // Selector for unimplemented_static_var message.
      enum : unsigned {
        Misc,
        GenericTypes,
        Classes,
        Protocols,
      };
      
      auto unimplementedStatic = [&](unsigned diagSel) {
        diagnose(Tok, diag::unimplemented_static_var, diagSel)
          .highlight(SourceRange(StaticLoc));
      };
      
      if (auto nom = dyn_cast<NominalTypeDecl>(CurDeclContext)) {
        if (nom->getGenericParams()) {
          unimplementedStatic(GenericTypes);
        } else if (isa<ClassDecl>(CurDeclContext)) {
          unimplementedStatic(Classes);
        } else if (isa<ProtocolDecl>(CurDeclContext)) {
          unimplementedStatic(Protocols);
        } else if (!isa<StructDecl>(CurDeclContext)
                   && !isa<EnumDecl>(CurDeclContext)) {
          unimplementedStatic(Misc);
        }
      } else {
        unimplementedStatic(Misc);
      }

      UnhandledStatic = false;
    }
    Status = parseDeclVar(Flags, Attributes, Entries, StaticLoc);
    break;
  case tok::kw_typealias:
    DeclResult = parseDeclTypeAlias(!(Flags & PD_DisallowTypeAliasDef),
                                     Flags & PD_InProtocol, Attributes);
    Status = DeclResult;
    break;
  case tok::kw_enum:
    DeclResult = parseDeclEnum(Flags, Attributes);
    Status = DeclResult;
    break;
  case tok::kw_case:
    Status = parseDeclEnumCase(Flags, Attributes, Entries);
    break;
  case tok::kw_struct:
    DeclResult = parseDeclStruct(Flags, Attributes);
    Status = DeclResult;
    break;
  case tok::kw_class:
    DeclResult = parseDeclClass(Flags, Attributes);
    Status = DeclResult;
    break;
  case tok::kw_init:
    DeclResult = parseDeclConstructor(Flags, Attributes);
    Status = DeclResult;
    break;
  case tok::kw_destructor:
    DeclResult = parseDeclDestructor(Flags, Attributes);
    Status = DeclResult;
    break;
  case tok::kw_protocol:
    DeclResult = parseDeclProtocol(Flags, Attributes);
    Status = DeclResult;
    break;

  case tok::kw_func:
    DeclResult = parseDeclFunc(StaticLoc, Flags, Attributes);
    Status = DeclResult;
    UnhandledStatic = false;
    break;

  case tok::kw_subscript:
    if (StaticLoc.isValid()) {
      diagnose(Tok, diag::subscript_static)
        .fixItRemove(SourceRange(StaticLoc));
      UnhandledStatic = false;
    }
    Status = parseDeclSubscript(Flags & PD_HasContainerType,
                                !(Flags & PD_DisallowFuncDef),
                                Attributes, Entries);
    break;
  
  case tok::identifier:
    if (isStartOfOperatorDecl(Tok, peekToken())) {
      DeclResult = parseDeclOperator(Flags & PD_AllowTopLevel, Attributes);
      break;
    }
    SWIFT_FALLTHROUGH;

  default:
    diagnose(Tok, diag::expected_decl);
    DeclResult = makeParserErrorResult<Decl>();
    Status = DeclResult;
    break;
  }

  if (Status.hasCodeCompletion() && isCodeCompletionFirstPass() &&
      !CurDeclContext->isModuleScopeContext()) {
    // Only consume non-toplevel decls.
    consumeDecl(BeginParserPosition, Flags, /*IsTopLevel=*/false);

    // Pretend that there was no error.
    return makeParserSuccess();
  }

  if (DeclResult.isNonNull())
    Entries.push_back(DeclResult.get());

  if (Status.isSuccess() && Tok.is(tok::semi))
    Entries.back()->TrailingSemiLoc = consumeToken(tok::semi);

  // If we parsed 'static' but didn't handle it above, complain about it.
  if (Status.isSuccess() && UnhandledStatic) {
    diagnose(Entries.back()->getLoc(), diag::decl_not_static)
      .fixItRemove(SourceRange(StaticLoc));
  }

  return Status;
}

void Parser::parseDeclDelayed() {
  auto DelayedState = State->takeDelayedDeclState();
  assert(DelayedState.get() && "should have delayed state");

  auto BeginParserPosition = getParserPosition(DelayedState->BodyPos);
  auto EndLexerState = L->getStateForEndOfTokenLoc(DelayedState->BodyEnd);

  // ParserPositionRAII needs a primed parser to restore to.
  if (Tok.is(tok::NUM_TOKENS))
    consumeToken();

  // Ensure that we restore the parser state at exit.
  ParserPositionRAII PPR(*this);

  // Create a lexer that can not go past the end state.
  Lexer LocalLex(*L, BeginParserPosition.LS, EndLexerState);

  // Temporarily swap out the parser's current lexer with our new one.
  llvm::SaveAndRestore<Lexer *> T(L, &LocalLex);

  // Rewind to the beginning of the decl.
  restoreParserPosition(BeginParserPosition);

  // Re-enter the lexical scope.
  Scope S(this, DelayedState->takeScope());
  ContextChange CC(*this, DelayedState->ParentContext);

  SmallVector<Decl *, 2> Entries;
  parseDecl(Entries, DelayedState->Flags);
}

/// \brief Parse an 'import' declaration, doing no token skipping on error.
///
/// \verbatim
///   decl-import:
///     'import' attribute-list import-kind? import-path
///   import-kind:
///     'typealias'
///     'struct'
///     'class'
///     'enum'
///     'protocol'
///     'var'
///     'func'
///   import-path:
///     any-identifier ('.' any-identifier)*
/// \endverbatim
ParserResult<ImportDecl> Parser::parseDeclImport(unsigned Flags,
                                                 DeclAttributes &Attributes) {
  SourceLoc ImportLoc = consumeToken(tok::kw_import);
  
  bool Exported = Attributes.isExported();
  Attributes.clearAttribute(AK_exported);
  if (!Attributes.empty())
    diagnose(Attributes.AtLoc, diag::import_attributes);

  if (!(Flags & PD_AllowTopLevel)) {
    diagnose(ImportLoc, diag::decl_inner_scope);
    return nullptr;
  }

  ImportKind Kind = ImportKind::Module;
  SourceLoc KindLoc;
  if (Tok.isKeyword()) {
    switch (Tok.getKind()) {
    case tok::kw_typealias:
      Kind = ImportKind::Type;
      break;
    case tok::kw_struct:
      Kind = ImportKind::Struct;
      break;
    case tok::kw_class:
      Kind = ImportKind::Class;
      break;
    case tok::kw_enum:
      Kind = ImportKind::Enum;
      break;
    case tok::kw_protocol:
      Kind = ImportKind::Protocol;
      break;
    case tok::kw_var:
      Kind = ImportKind::Var;
      break;
    case tok::kw_func:
      Kind = ImportKind::Func;
      break;
    default:
      diagnose(Tok, diag::expected_identifier_in_decl, "import");
      return nullptr;
    }
    KindLoc = consumeToken();
  }

  SmallVector<std::pair<Identifier, SourceLoc>, 8> ImportPath;
  do {
    ImportPath.push_back(std::make_pair(Identifier(), Tok.getLoc()));
    if (parseAnyIdentifier(ImportPath.back().first,
                        diag::expected_identifier_in_decl, "import"))
      return nullptr;
  } while (consumeIf(tok::period));

  if (Kind != ImportKind::Module && ImportPath.size() == 1) {
    diagnose(ImportPath.front().second, diag::decl_expected_module_name);
    return nullptr;
  }

  return makeParserResult(ImportDecl::create(
      Context, CurDeclContext, ImportLoc, Kind, KindLoc, Exported, ImportPath));
}

/// \brief Parse an inheritance clause.
///
/// \verbatim
///   inheritance:
///      ':' type-identifier (',' type-identifier)*
/// \endverbatim
ParserStatus Parser::parseInheritance(SmallVectorImpl<TypeLoc> &Inherited) {
  consumeToken(tok::colon);

  ParserStatus Status;
  do {
    // Parse the inherited type (which must be a protocol).
    ParserResult<TypeRepr> Ty = parseTypeIdentifier();
    Status |= Ty;

    // Record the type.
    if (Ty.isNonNull())
      Inherited.push_back(Ty.get());

    // Check for a ',', which indicates that there are more protocols coming.
  } while (consumeIf(tok::comma));

  return Status;
}

enum class TokenProperty {
  None,
  StartsWithLess,
};

static ParserStatus parseIdentifierDeclName(Parser &P, Identifier &Result,
                                            SourceLoc &Loc, tok ResyncT1,
                                            tok ResyncT2, tok ResyncT3,
                                            tok ResyncT4,
                                            TokenProperty ResyncP1,
                                            const Diagnostic &D) {
  switch (P.Tok.getKind()) {
  case tok::identifier:
    Result = P.Context.getIdentifier(P.Tok.getText());
    Loc = P.Tok.getLoc();
    P.consumeToken();
    return makeParserSuccess();

  default:
    if (D.getID() != DiagID::invalid_diagnostic)
      P.diagnose(P.Tok, D);
    if (P.Tok.isKeyword() &&
        (P.peekToken().is(ResyncT1) || P.peekToken().is(ResyncT2) ||
         P.peekToken().is(ResyncT3) || P.peekToken().is(ResyncT4) ||
         (ResyncP1 != TokenProperty::None &&
          P.startsWithLess(P.peekToken())))) {
      llvm::SmallString<32> Name(P.Tok.getText());
      // Append an invalid character so that nothing can resolve to this name.
      Name += "#";
      Result = P.Context.getIdentifier(Name.str());
      Loc = P.Tok.getLoc();
      P.consumeToken();
      // Return success because we recovered.
      return makeParserSuccess();
    }
    return makeParserError();
  }
}

template <typename... DiagArgTypes, typename... ArgTypes>
static ParserStatus
parseIdentifierDeclName(Parser &P, Identifier &Result, SourceLoc &L,
                        tok ResyncT1, tok ResyncT2, Diag<DiagArgTypes...> ID,
                        ArgTypes... Args) {
  return parseIdentifierDeclName(P, Result, L, ResyncT1, ResyncT2,
                                 tok::unknown, tok::unknown,
                                 TokenProperty::None,
                                 Diagnostic(ID, Args...));
}

template <typename... DiagArgTypes, typename... ArgTypes>
static ParserStatus
parseIdentifierDeclName(Parser &P, Identifier &Result, SourceLoc &L,
                        tok ResyncT1, tok ResyncT2, tok ResyncT3,
                        Diag<DiagArgTypes...> ID, ArgTypes... Args) {
  return parseIdentifierDeclName(P, Result, L, ResyncT1, ResyncT2, ResyncT3,
                                 tok::unknown, TokenProperty::None,
                                 Diagnostic(ID, Args...));
}

template <typename... DiagArgTypes, typename... ArgTypes>
static ParserStatus
parseIdentifierDeclName(Parser &P, Identifier &Result, SourceLoc &L,
                        tok ResyncT1, tok ResyncT2, tok ResyncT3, tok ResyncT4,
                        Diag<DiagArgTypes...> ID, ArgTypes... Args) {
  return parseIdentifierDeclName(P, Result, L, ResyncT1, ResyncT2, ResyncT3,
                                 ResyncT4, TokenProperty::None,
                                 Diagnostic(ID, Args...));
}


template <typename... DiagArgTypes, typename... ArgTypes>
static ParserStatus
parseIdentifierDeclName(Parser &P, Identifier &Result, SourceLoc &L,
                        tok ResyncT1, tok ResyncT2, TokenProperty ResyncP1,
                        Diag<DiagArgTypes...> ID, ArgTypes... Args) {
  return parseIdentifierDeclName(P, Result, L, ResyncT1, ResyncT2, tok::unknown,
                                 tok::unknown,
                                 ResyncP1, Diagnostic(ID, Args...));
}

/// \brief Parse an 'extension' declaration.
///
/// \verbatim
///   extension:
///    'extension' attribute-list type-identifier inheritance? '{' decl* '}'
/// \endverbatim
ParserResult<ExtensionDecl> Parser::parseDeclExtension(unsigned Flags,
                                                       DeclAttributes &Attr) {
  SourceLoc ExtensionLoc = consumeToken(tok::kw_extension);

  ParserResult<TypeRepr> Ty = parseTypeIdentifierWithRecovery(
      diag::expected_type, diag::expected_ident_type_in_extension);
  if (Ty.hasCodeCompletion())
    return makeParserCodeCompletionResult<ExtensionDecl>();
  if (Ty.isNull() && Tok.isKeyword()) {
    // We failed to parse the type, but we could try recovering by parsing a
    // keyword if the lookahead token looks promising.
    Identifier ExtensionName;
    SourceLoc NameLoc;
    if (parseIdentifierDeclName(*this, ExtensionName, NameLoc, tok::colon,
                                tok::l_brace,
                                diag::invalid_diagnostic).isError())
      return nullptr;
    Ty = makeParserErrorResult(
        IdentTypeRepr::createSimple(Context, NameLoc, ExtensionName));
  }
  if (Ty.isNull())
    return nullptr;

  ParserStatus Status;

  // Parse optional inheritance clause.
  SmallVector<TypeLoc, 2> Inherited;
  if (Tok.is(tok::colon))
    Status |= parseInheritance(Inherited);

  ExtensionDecl *ED
    = new (Context) ExtensionDecl(ExtensionLoc, Ty.get(),
                                  Context.AllocateCopy(Inherited),
                                  CurDeclContext);
  if (Attr.isValid())
    ED->getMutableAttrs() = Attr;

  SmallVector<Decl*, 8> MemberDecls;
  SourceLoc LBLoc, RBLoc;
  if (parseToken(tok::l_brace, LBLoc, diag::expected_lbrace_extension)) {
    LBLoc = Tok.getLoc();
    RBLoc = LBLoc;
    Status.setIsParseError();
  } else {
    // Parse the body.
    ContextChange CC(*this, ED);
    Scope S(this, ScopeKind::Extension);

    ParserStatus BodyStatus =
        parseList(tok::r_brace, LBLoc, RBLoc, tok::semi, /*OptionalSep=*/true,
                  /*AllowSepAfterLast=*/false, diag::expected_rbrace_extension,
                  [&]() -> ParserStatus {
      return parseDecl(MemberDecls,
                       PD_HasContainerType
                       | PD_DisallowStoredInstanceVar);
    });
    // Don't propagate the code completion bit from members: we can not help
    // code completion inside a member decl, and our callers can not do
    // anything about it either.  But propagate the error bit.
    if (BodyStatus.isError())
      Status.setIsParseError();
  }

  if (MemberDecls.empty())
    ED->setMembers({}, { LBLoc, RBLoc });
  else
    ED->setMembers(Context.AllocateCopy(MemberDecls), { LBLoc, RBLoc });

  if (!(Flags & PD_AllowTopLevel)) {
    diagnose(ExtensionLoc, diag::decl_inner_scope);
    Status.setIsParseError();

    // Tell the type checker not to touch this extension.
    ED->setInvalid();
  }

  return makeParserResult(Status, ED);
}

/// \brief Parse a typealias decl.
///
/// \verbatim
///   decl-typealias:
///     'typealias' identifier inheritance? '=' type
/// \endverbatim
ParserResult<TypeDecl> Parser::parseDeclTypeAlias(bool WantDefinition,
                                                  bool isAssociatedType,
                                                  DeclAttributes &Attributes) {
  SourceLoc TypeAliasLoc = consumeToken(tok::kw_typealias);
  
  Identifier Id;
  SourceLoc IdLoc;
  ParserStatus Status;
  
  if (!Attributes.empty())
    diagnose(Attributes.AtLoc, diag::typealias_attributes);
  

  Status |=
      parseIdentifierDeclName(*this, Id, IdLoc, tok::colon, tok::equal,
                              diag::expected_identifier_in_decl, "typealias");
  if (Status.isError())
    return nullptr;

  // Parse optional inheritance clause.
  SmallVector<TypeLoc, 2> Inherited;
  if (Tok.is(tok::colon))
    Status |= parseInheritance(Inherited);

  ParserResult<TypeRepr> UnderlyingTy;
  if (WantDefinition || Tok.is(tok::equal)) {
    if (parseToken(tok::equal, diag::expected_equal_in_typealias)) {
      Status.setIsParseError();
      return Status;
    }
    UnderlyingTy = parseType(diag::expected_type_in_typealias);
    Status |= UnderlyingTy;
    if (UnderlyingTy.isNull())
      return Status;
    
    if (!WantDefinition) {
      diagnose(IdLoc, diag::associated_type_def, Id);
      UnderlyingTy = nullptr;
    }
  }

  // If this is an associated type, build the AST for it.
  if (isAssociatedType) {
    auto assocType = new (Context) AssociatedTypeDecl(CurDeclContext,
                                                      TypeAliasLoc, Id, IdLoc);
    if (!Inherited.empty())
      assocType->setInherited(Context.AllocateCopy(Inherited));
    addToScope(assocType);
    return makeParserResult(Status, assocType);
  }

  // Otherwise, build a typealias.
  TypeAliasDecl *TAD =
    new (Context) TypeAliasDecl(TypeAliasLoc, Id, IdLoc,
                                UnderlyingTy.getPtrOrNull(),
                                CurDeclContext,
                                Context.AllocateCopy(Inherited));
  addToScope(TAD);
  return makeParserResult(Status, TAD);
}

namespace {
  class AddVarsToScope : public ASTWalker {
  public:
    Parser &TheParser;
    ASTContext &Context;
    DeclContext *CurDeclContext;
    SmallVectorImpl<Decl*> &Decls;
    bool IsStatic;
    DeclAttributes &Attributes;
    PatternBindingDecl *PBD;
    
    AddVarsToScope(Parser &P,
                   ASTContext &Context,
                   DeclContext *CurDeclContext,
                   SmallVectorImpl<Decl*> &Decls,
                   bool IsStatic,
                   DeclAttributes &Attributes,
                   PatternBindingDecl *PBD)
      : TheParser(P),
        Context(Context),
        CurDeclContext(CurDeclContext),
        Decls(Decls),
        IsStatic(IsStatic),
        Attributes(Attributes),
        PBD(PBD)
    {}
    
    Pattern *walkToPatternPost(Pattern *P) override {
      // Handle vars.
      if (auto *Named = dyn_cast<NamedPattern>(P)) {
        VarDecl *VD = Named->getDecl();
        VD->setDeclContext(CurDeclContext);
        VD->setStatic(IsStatic);
        VD->setParentPattern(PBD);
        if (Attributes.isValid())
          VD->getMutableAttrs() = Attributes;
        
        if (VD->isComputed()) {
          // Add getter & setter in source order.
          FuncDecl* Accessors[2] = {VD->getGetter(), VD->getSetter()};
          if (Accessors[0] && Accessors[1] &&
              !Context.SourceMgr.isBeforeInBuffer(
                  Accessors[0]->getFuncLoc(), Accessors[1]->getFuncLoc())) {
            std::swap(Accessors[0], Accessors[1]);
          }
          for (auto FD : Accessors) {
            if (FD) {
              FD->setDeclContext(CurDeclContext);
              Decls.push_back(FD);
            }
          }
        }
        
        Decls.push_back(VD);
        TheParser.addToScope(VD);
      }
      return P;
    }
  };
}

void Parser::addVarsToScope(Pattern *Pat,
                            SmallVectorImpl<Decl*> &Decls,
                            bool IsStatic,
                            DeclAttributes &Attributes,
                            PatternBindingDecl *PBD) {
  Pat->walk(AddVarsToScope(*this, Context, CurDeclContext,
                           Decls, IsStatic, Attributes, PBD));
}

/// \brief Parse a get-set clause, containing a getter and (optionally)
/// a setter.
///
/// \verbatim
///   get-set:
///      get var-set?
///      set var-get
///
///   get:
///     'get' attribute-list ':' stmt-brace-item*
///
///   set:
///     'set' attribute-list set-name? ':' stmt-brace-item*
///
///   set-name:
///     '(' identifier ')'
/// \endverbatim
bool Parser::parseGetSet(bool HasContainerType, Pattern *Indices,
                         TypeLoc ElementTy, FuncDecl *&Get, FuncDecl *&Set,
                         SourceLoc &LastValidLoc,
                         SourceLoc StaticLoc) {
  bool Invalid = false;
  Get = nullptr;
  Set = nullptr;
  
  while (Tok.isNot(tok::r_brace)) {
    if (Tok.is(tok::eof)) {
      Invalid = true;
      break;
    }

    // Parse any leading attributes.
    DeclAttributes Attributes;
    parseDeclAttributeList(Attributes);
    
    if (Tok.isContextualKeyword("get") || !Tok.isContextualKeyword("set")) {
      //   get         ::= 'get' stmt-brace

      // Have we already parsed a get clause?
      if (Get) {
        diagnose(Tok, diag::duplicate_getset, false);
        diagnose(Get->getLoc(), diag::previous_getset, false);
        
        // Forget the previous version.
        Get = nullptr;
      }
      
      SourceLoc GetLoc = Tok.getLoc(), ColonLoc = Tok.getLoc();
      if (Tok.isContextualKeyword("get")) {
        GetLoc = consumeToken();

        if (Tok.isNot(tok::colon)) {
          diagnose(Tok, diag::expected_colon_get);
          Invalid = true;
          break;
        }
        ColonLoc = consumeToken(tok::colon);
      }

      // Set up a function declaration for the getter and parse its body.
      
      // Create the parameter list(s) for the getter.
      SmallVector<Pattern *, 3> Params;
      
      // Add the implicit 'self' to Params, if needed.
      if (HasContainerType)
        Params.push_back(buildImplicitSelfParameter(GetLoc));

      // Add the index clause if necessary.
      if (Indices) {
        Params.push_back(Indices->clone(Context, /*Implicit=*/true));
      }
      
      // Add a no-parameters clause.
      Params.push_back(TuplePattern::create(Context, SourceLoc(),
                                            ArrayRef<TuplePatternElt>(),
                                            SourceLoc(), /*hasVararg=*/false,
                                            SourceLoc(), /*Implicit=*/true));

      Scope S(this, ScopeKind::FunctionBody);

      // Start the function.
      Get = FuncDecl::create(Context, /*StaticLoc=*/SourceLoc(), GetLoc,
                             Identifier(), GetLoc, /*GenericParams=*/nullptr,
                             Type(), Params, Params, ElementTy,
                             CurDeclContext);
      if (StaticLoc.isValid())
        Get->setStatic(true);
      addFunctionParametersToScope(Get->getBodyParamPatterns(), Get);

      // Establish the new context.
      ParseFunctionBody CC(*this, Get);

      SmallVector<ASTNode, 16> Entries;
      parseBraceItems(Entries, BraceItemListKind::Variable);
      BraceStmt *Body = BraceStmt::create(Context, ColonLoc,
                                          Entries, Tok.getLoc());
      Get->setBody(Body);

      if (Attributes.isValid())
        Get->getMutableAttrs() = Attributes;

      LastValidLoc = Body->getRBraceLoc();
      continue;
    }

    //   var-set         ::= 'set' var-set-name? stmt-brace
    
    // Have we already parsed a var-set clause?
    if (Set) {
      diagnose(Tok, diag::duplicate_getset, true);
      diagnose(Set->getLoc(), diag::previous_getset, true);

      // Forget the previous setter.
      Set = nullptr;
    }
    
    SourceLoc SetLoc = consumeToken();

    //   var-set-name    ::= '(' identifier ')'
    Identifier SetName;
    SourceLoc SetNameLoc;
    SourceRange SetNameParens;
    if (Tok.is(tok::l_paren)) {
      SourceLoc StartLoc = consumeToken();
      if (Tok.is(tok::identifier)) {
        // We have a name.
        SetName = Context.getIdentifier(Tok.getText());
        SetNameLoc = consumeToken();
        
        // Look for the closing ')'.
        SourceLoc EndLoc;
        if (parseMatchingToken(tok::r_paren, EndLoc,
                               diag::expected_rparen_setname, StartLoc))
          EndLoc = SetNameLoc;
        SetNameParens = SourceRange(StartLoc, EndLoc);
      } else {
        diagnose(Tok, diag::expected_setname);
        skipUntil(tok::r_paren, tok::l_brace);
        if (Tok.is(tok::r_paren))
          consumeToken();
      }
    }
    if (Tok.isNot(tok::colon)) {
      diagnose(Tok, diag::expected_colon_set);
      Invalid = true;
      break;
    }
    SourceLoc ColonLoc = consumeToken(tok::colon);

    // Set up a function declaration for the setter and parse its body.
    
    // Create the parameter list(s) for the setter.
    SmallVector<Pattern *, 3> Params;
    
    // Add the implicit 'self' to Params, if needed.
    if (HasContainerType)
      Params.push_back(buildImplicitSelfParameter(SetLoc));

    // Add the index parameters, if necessary.
    if (Indices) {
      Params.push_back(Indices->clone(Context, /*Implicit=*/true));
    }

    bool IsNameImplicit = false;
    // Add the parameter. If no name was specified, the name defaults to
    // 'value'.
    if (SetName.empty()) {
      SetName = Context.getIdentifier("value");
      SetNameLoc = SetLoc;
      IsNameImplicit = true;
    }

    {
      VarDecl *Value = new (Context) VarDecl(StaticLoc.isValid(),
                                             /*IsLet*/false,
                                             SetNameLoc, SetName,
                                             Type(), CurDeclContext);
      if (IsNameImplicit)
        Value->setImplicit();

      Pattern *ValuePattern
        = new (Context) TypedPattern(new (Context) NamedPattern(Value),
                                     ElementTy);
      // The TypedPattern is always implicit because the ElementTy is not
      // spelled inside the parameter list.  It comes from elsewhere, and its
      // source location should be ignored.
      ValuePattern->setImplicit();

      TuplePatternElt ValueElt(ValuePattern);
      Pattern *ValueParamsPattern
        = TuplePattern::create(Context, SetNameParens.Start, ValueElt,
                               SetNameParens.End);
      if (IsNameImplicit)
        ValueParamsPattern->setImplicit();

      Params.push_back(ValueParamsPattern);
    }

    Scope S(this, ScopeKind::FunctionBody);

    // Start the function.
    Type SetterRetTy = TupleType::getEmpty(Context);
    Set = FuncDecl::create(Context, /*StaticLoc=*/SourceLoc(), SetLoc,
                           Identifier(), SetLoc, /*generic=*/nullptr, Type(),
                           Params, Params, TypeLoc::withoutLoc(SetterRetTy),
                           CurDeclContext);
    if (StaticLoc.isValid())
      Set->setStatic(true);

    addFunctionParametersToScope(Set->getBodyParamPatterns(), Set);

    // Establish the new context.
    ParseFunctionBody CC(*this, Set);
    
    // Parse the body.
    SmallVector<ASTNode, 16> Entries;
    parseBraceItems(Entries, BraceItemListKind::Variable);
    BraceStmt *Body = BraceStmt::create(Context, ColonLoc,
                                        Entries, Tok.getLoc());
    Set->setBody(Body);

    if (Attributes.isValid())
      Set->getMutableAttrs() = Attributes;

    LastValidLoc = Body->getRBraceLoc();
  }
  
  return Invalid;
}

/// \brief Parse the brace-enclosed getter and setter for a variable.
///
/// \verbatim
///   decl-var:
///      attribute-list 'var' identifier : type-annotation { get-set }
/// \endverbatim
void Parser::parseDeclVarGetSet(Pattern &pattern, bool HasContainerType,
                                SourceLoc StaticLoc) {
  bool Invalid = false;
    
  // The grammar syntactically requires a simple identifier for the variable
  // name. Complain if that isn't what we got.
  VarDecl *PrimaryVar = nullptr;
  {
    Pattern *PrimaryPattern = &pattern;
    if (TypedPattern *Typed = dyn_cast<TypedPattern>(PrimaryPattern))
      PrimaryPattern = Typed->getSubPattern();
    if (NamedPattern *Named = dyn_cast<NamedPattern>(PrimaryPattern)) {
      PrimaryVar = Named->getDecl();
    }
  }

  if (!PrimaryVar)
    diagnose(pattern.getLoc(), diag::getset_nontrivial_pattern);

  // The grammar syntactically requires a type annotation. Complain if
  // our pattern does not have one.
  TypeLoc TyLoc;
  if (TypedPattern *TP = dyn_cast<TypedPattern>(&pattern)) {
    TyLoc = TP->getTypeLoc();
  } else {
    if (PrimaryVar)
      diagnose(pattern.getLoc(), diag::getset_missing_type);
    TyLoc = TypeLoc::withoutLoc(ErrorType::get(Context));
  }

  setLocalDiscriminator(PrimaryVar);
  
  SourceLoc LBLoc = consumeToken(tok::l_brace);
    
  // Parse getter and setter.
  FuncDecl *Get = nullptr;
  FuncDecl *Set = nullptr;
  SourceLoc LastValidLoc = LBLoc;
  if (parseGetSet(HasContainerType, /*Indices=*/0, TyLoc,
                  Get, Set, LastValidLoc, StaticLoc))
    Invalid = true;
  
  // Parse the final '}'.
  SourceLoc RBLoc;
  if (Invalid) {
    skipUntilDeclRBrace();
    RBLoc = LastValidLoc;
  }

  if (parseMatchingToken(tok::r_brace, RBLoc, diag::expected_rbrace_in_getset,
                         LBLoc)) {
    RBLoc = LastValidLoc;
  }
  
  if (Set && !Get) {
    if (!Invalid)
      diagnose(Set->getLoc(), diag::var_set_without_get);
    
    Set = nullptr;
    Invalid = true;
  }

  // If things went well, turn this into a computed variable.
  if (!Invalid && PrimaryVar && (Set || Get))
    PrimaryVar->setComputedAccessors(Context, LBLoc, Get, Set, RBLoc);
}

/// \brief Parse a 'var' declaration, doing no token skipping on error.
///
/// \verbatim
///   decl-var:
///      'var' attribute-list pattern initializer? (',' pattern initializer? )*
///      'var' attribute-list identifier : type-annotation { get-set }
/// \endverbatim
ParserStatus Parser::parseDeclVar(unsigned Flags, DeclAttributes &Attributes,
                                  SmallVectorImpl<Decl *> &Decls,
                                  SourceLoc StaticLoc) {
  SourceLoc VarLoc = consumeToken(tok::kw_var);

  SmallVector<PatternBindingDecl*, 4> PBDs;
  bool HasGetSet = false;
  ParserStatus Status;

  unsigned FirstDecl = Decls.size();
  
  do {
    ParserResult<Pattern> pattern = parsePattern(false);
    if (pattern.hasCodeCompletion())
      return makeParserCodeCompletionStatus();
    if (pattern.isNull())
      return makeParserError();

    // If we syntactically match the second decl-var production, with a
    // var-get-set clause, parse the var-get-set clause.
    if (Tok.is(tok::l_brace)) {
      parseDeclVarGetSet(*pattern.get(), Flags & PD_HasContainerType,
                         StaticLoc);
      HasGetSet = true;
    }

    ParserResult<Expr> Init;
    if (Tok.is(tok::equal)) {
      // Record the variables that we're trying to initialize.
      SmallVector<VarDecl *, 4> Vars;
      Vars.append(CurVars.second.begin(), CurVars.second.end());
      pattern.get()->collectVariables(Vars);
      using RestoreVarsRAII = llvm::SaveAndRestore<decltype(CurVars)>;
      RestoreVarsRAII RestoreCurVars(CurVars, {CurDeclContext, Vars});

      SourceLoc EqualLoc = consumeToken(tok::equal);
      Init = parseExpr(diag::expected_init_value);
      if (Init.hasCodeCompletion())
        return makeParserCodeCompletionStatus();
      if (Init.isNull()) {
        Status.setIsParseError();
        break;
      }
    
      if (HasGetSet) {
        diagnose(pattern.get()->getLoc(), diag::getset_init)
          .highlight(Init.get()->getSourceRange());
        Init = nullptr;
      }
      if (Flags & PD_DisallowInit) {
        diagnose(EqualLoc, diag::disallowed_init);
        Status.setIsParseError();
      }
    }

    // In the normal case, just add PatternBindingDecls to our DeclContext.
    auto PBD = new (Context) PatternBindingDecl(StaticLoc, VarLoc,
                                                pattern.get(),
                                                Init.getPtrOrNull(),
                                                CurDeclContext);
    Decls.push_back(PBD);

    addVarsToScope(pattern.get(), Decls, StaticLoc.isValid(), Attributes, PBD);

    // Propagate back types for simple patterns, like "var A, B : T".
    if (TypedPattern *TP = dyn_cast<TypedPattern>(PBD->getPattern())) {
      if (isa<NamedPattern>(TP->getSubPattern()) && !PBD->hasInit()) {
        for (unsigned i = PBDs.size(); i != 0; --i) {
          PatternBindingDecl *PrevPBD = PBDs[i-1];
          Pattern *PrevPat = PrevPBD->getPattern();
          if (!isa<NamedPattern>(PrevPat) || PrevPBD->hasInit())
            break;
          if (HasGetSet) {
            // FIXME -- offer a fixit to explicitly specify the type
            diagnose(PrevPat->getLoc(), diag::getset_cannot_be_implied);
            Status.setIsParseError();
          }

          TypedPattern *NewTP = new (Context) TypedPattern(PrevPat,
                                                           TP->getTypeLoc());
          PrevPBD->setPattern(NewTP);
        }
      }
    }
    PBDs.push_back(PBD);
  } while (consumeIf(tok::comma));

  if (HasGetSet) {
    if (PBDs.size() > 1) {
      diagnose(VarLoc, diag::disallowed_var_multiple_getset);
      Status.setIsParseError();
    }
    if (Flags & PD_DisallowComputedVar) {
      diagnose(VarLoc, diag::disallowed_computed_var_decl);
      Status.setIsParseError();
    }
  } else if (!StaticLoc.isValid() && (Flags & PD_DisallowStoredInstanceVar)) {
    diagnose(VarLoc, diag::disallowed_stored_var_decl);
    Status.setIsParseError();
    return Status;
  }

  // If this is a var in the top-level of script/repl source file, then
  // wrap the PatternBindingDecls in TopLevelCodeDecls, since they represent
  // executable code.
  if (allowTopLevelCode() && CurDeclContext->isModuleScopeContext()) {
    for (unsigned i = FirstDecl; i != Decls.size(); ++i) {
      auto *PBD = dyn_cast<PatternBindingDecl>(Decls[i]);
      if (PBD == 0) continue;
      auto *Brace = BraceStmt::create(Context, PBD->getStartLoc(),
                                      ASTNode(PBD), PreviousLoc);

      auto *TLCD = new (Context) TopLevelCodeDecl(CurDeclContext, Brace);
      PBD->setDeclContext(TLCD);
      Decls[i] = TLCD;
    }
  }

  return Status;
}

namespace {
/// Recursively walks a pattern and sets all variables' decl contexts to the
/// given context.
class SetVarContext : public ASTWalker {
  DeclContext *DC;

public:
  SetVarContext(DeclContext *DC) : DC(DC) {}

  Pattern *walkToPatternPost(Pattern *P) override {
    // Handle vars.
    if (auto *Named = dyn_cast<NamedPattern>(P))
      Named->getDecl()->setDeclContext(DC);
    return P;
  }
};
} // unnamed namespace

static void setVarContext(ArrayRef<Pattern *> Patterns, DeclContext *DC) {
  for (auto P : Patterns) {
    P->walk(SetVarContext(DC));
  }
}

/// \brief Build an implicit 'self' parameter for the current DeclContext.
Pattern *Parser::buildImplicitSelfParameter(SourceLoc Loc) {
  VarDecl *D
    = new (Context) VarDecl(/*static*/ false,
                            /*IsLet*/ false,
                            Loc, Context.SelfIdentifier,
                            Type(), CurDeclContext);
  D->setImplicit();
  Pattern *P = new (Context) NamedPattern(D, /*Implicit=*/true);
  return new (Context) TypedPattern(P, TypeLoc());
}

void Parser::consumeAbstractFunctionBody(AbstractFunctionDecl *AFD,
                                         const DeclAttributes &Attrs) {
  auto BeginParserPosition = getParserPosition();
  SourceRange BodyRange;
  BodyRange.Start = Tok.getLoc();

  // Consume the '{', and find the matching '}'.
  consumeToken(tok::l_brace);
  unsigned OpenBraces = 1;
  while (OpenBraces != 0 && Tok.isNot(tok::eof)) {
    if (consumeIf(tok::l_brace)) {
      OpenBraces++;
      continue;
    }
    if (consumeIf(tok::r_brace)) {
      OpenBraces--;
      continue;
    }
    consumeToken();
  }
  if (OpenBraces != 0 && Tok.isNot(tok::code_complete)) {
    assert(Tok.is(tok::eof));
    // We hit EOF, and not every brace has a pair.  Recover by searching
    // for the next decl except variable decls and cutting off before
    // that point.
    backtrackToPosition(BeginParserPosition);
    consumeToken(tok::l_brace);
    while (Tok.is(tok::kw_var) ||
           (Tok.isNot(tok::eof) && !isStartOfDecl(Tok, peekToken()))) {
      consumeToken();
    }
  }

  BodyRange.End = PreviousLoc;

  if (DelayedParseCB->shouldDelayFunctionBodyParsing(*this, AFD, Attrs,
                                                     BodyRange)) {
    State->delayFunctionBodyParsing(AFD, BodyRange,
                                    BeginParserPosition.PreviousLoc);
    AFD->setBodyDelayed(BodyRange.End);
  } else {
    AFD->setBodySkipped(BodyRange.End);
  }
}

/// \brief Parse a 'func' declaration, returning null on error.  The caller
/// handles this case and does recovery as appropriate.
///
/// \verbatim
///   decl-func:
///     'static'? 'func' attribute-list any-identifier generic-params?
///               func-signature stmt-brace?
/// \endverbatim
///
/// \note The caller of this method must ensure that the next token is 'func'.
ParserResult<FuncDecl>
Parser::parseDeclFunc(SourceLoc StaticLoc, unsigned Flags,
                      DeclAttributes &Attributes) {
  bool HasContainerType = Flags & PD_HasContainerType;

  // Reject 'static' functions at global scope.
  if (StaticLoc.isValid() && !HasContainerType) {
    diagnose(Tok, diag::static_func_decl_global_scope)
      .fixItRemoveChars(StaticLoc, Tok.getLoc());
    StaticLoc = SourceLoc();
  }
  
  SourceLoc FuncLoc = consumeToken(tok::kw_func);

  Identifier Name;
  SourceLoc NameLoc = Tok.getLoc();
  if (!(Flags & PD_AllowTopLevel) && !(Flags & PD_DisallowFuncDef) &&
      Tok.isAnyOperator()) {
    // FIXME: Recovery here is awful.
    diagnose(Tok, diag::func_decl_nonglobal_operator);
    return nullptr;
  }
  if (parseAnyIdentifier(Name, diag::expected_identifier_in_decl, "function")) {
    ParserStatus NameStatus =
        parseIdentifierDeclName(*this, Name, NameLoc, tok::l_paren, tok::arrow,
                                tok::l_brace, diag::invalid_diagnostic);
    if (NameStatus.isError())
      return nullptr;
  }

  // Parse the generic-params, if present.
  Optional<Scope> GenericsScope;
  GenericsScope.emplace(this, ScopeKind::Generics);
  GenericParamList *GenericParams;

  // If the name is an operator token that ends in '<' and the following token
  // is an identifier, split the '<' off as a separate token. This allows things
  // like 'func ==<T>(x:T, y:T) {}' to parse as '==' with generic type variable
  // '<T>' as expected.
  if (Name.str().size() > 1 && Name.str().back() == '<'
      && Tok.is(tok::identifier)) {
    Name = Context.getIdentifier(Name.str().slice(0, Name.str().size() - 1));
    SourceLoc LAngleLoc = NameLoc.getAdvancedLoc(Name.str().size());
    GenericParams = parseGenericParameters(LAngleLoc);
  } else {
    GenericParams = maybeParseGenericParams();
  }

  SmallVector<Pattern*, 8> ArgParams;
  SmallVector<Pattern*, 8> BodyParams;
  
  // If we're within a container, add an implicit first pattern to match the
  // container type as an element named 'self'.
  //
  // This turns an instance function "(int)->int" on FooTy into
  // "(this: [inout] FooTy)->(int)->int", and a static function
  // "(int)->int" on FooTy into "(this: [inout] FooTy.metatype)->(int)->int".
  // Note that we can't actually compute the type here until Sema.
  if (HasContainerType) {
    Pattern *SelfPattern = buildImplicitSelfParameter(NameLoc);
    ArgParams.push_back(SelfPattern);
    BodyParams.push_back(SelfPattern);
  }

  TypeRepr *FuncRetTy = nullptr;
  bool HasSelectorStyleSignature;
  ParserStatus SignatureStatus =
      parseFunctionSignature(ArgParams, BodyParams, FuncRetTy,
                             HasSelectorStyleSignature);

  if (SignatureStatus.hasCodeCompletion() && !CodeCompletion) {
    // Trigger delayed parsing, no need to continue.
    return SignatureStatus;
  }

  // Enter the arguments for the function into a new function-body scope.  We
  // need this even if there is no function body to detect argument name
  // duplication.
  FuncDecl *FD;
  {
    Scope S(this, ScopeKind::FunctionBody);

    // Create the decl for the func and add it to the parent scope.
    FD = FuncDecl::create(Context, StaticLoc, FuncLoc, Name, NameLoc,
                          GenericParams, Type(), ArgParams, BodyParams,
                          FuncRetTy, CurDeclContext);

    if (HasSelectorStyleSignature)
      FD->setHasSelectorStyleSignature();

    // Pass the function signature to code completion.
    if (SignatureStatus.hasCodeCompletion())
      CodeCompletion->setDelayedParsedDecl(FD);

    addFunctionParametersToScope(FD->getBodyParamPatterns(), FD);
    setVarContext(FD->getArgParamPatterns(), FD);
    setLocalDiscriminator(FD);

    // Now that we have a context, update the generic parameters with that
    // context.
    if (GenericParams) {
      for (auto Param : *GenericParams) {
        Param.setDeclContext(FD);
      }
    }

    // Establish the new context.
    ParseFunctionBody CC(*this, FD);

    // Check to see if we have a "{" to start a brace statement.
    if (Tok.is(tok::l_brace)) {
      if (Flags & PD_DisallowFuncDef) {
        diagnose(Tok, diag::disallowed_func_def);
        consumeToken();
        skipUntil(tok::r_brace);
        consumeToken();
        // FIXME: don't just drop the body.
      } else if (!isDelayedParsingEnabled()) {
        ParserResult<BraceStmt> Body =
            parseBraceItemList(diag::func_decl_without_brace);
        if (Body.isNull()) {
          // FIXME: Should do some sort of error recovery here?
        } else if (SignatureStatus.hasCodeCompletion()) {
          // Code completion was inside the signature, don't attach the body.
          FD->setBodySkipped(Body.get()->getEndLoc());
        } else {
          FD->setBody(Body.get());
        }
      } else {
        consumeAbstractFunctionBody(FD, Attributes);
      }
    } else if (Attributes.AsmName.empty() && !(Flags & PD_DisallowFuncDef) &&
               !SignatureStatus.isError() && !isInSILMode()) {
      diagnose(Tok.getLoc(), diag::func_decl_without_brace);
    }
  }

  // Exit the scope introduced for the generic parameters.
  GenericsScope.reset();

  if (Attributes.isValid())
    FD->getMutableAttrs() = Attributes;
  addToScope(FD);
  return makeParserResult(FD);
}

bool Parser::parseAbstractFunctionBodyDelayed(AbstractFunctionDecl *AFD) {
  assert(!AFD->getBody() && "function should not have a parsed body");
  assert(AFD->getBodyKind() == AbstractFunctionDecl::BodyKind::Unparsed &&
         "function body should be delayed");

  auto FunctionParserState = State->takeBodyState(AFD);
  assert(FunctionParserState.get() && "should have a valid state");

  auto BeginParserPosition = getParserPosition(FunctionParserState->BodyPos);
  auto EndLexerState = L->getStateForEndOfTokenLoc(AFD->getEndLoc());

  // ParserPositionRAII needs a primed parser to restore to.
  if (Tok.is(tok::NUM_TOKENS))
    consumeToken();

  // Ensure that we restore the parser state at exit.
  ParserPositionRAII PPR(*this);

  // Create a lexer that can not go past the end state.
  Lexer LocalLex(*L, BeginParserPosition.LS, EndLexerState);

  // Temporarily swap out the parser's current lexer with our new one.
  llvm::SaveAndRestore<Lexer *> T(L, &LocalLex);

  // Rewind to '{' of the function body.
  restoreParserPosition(BeginParserPosition);

  // Re-enter the lexical scope.
  Scope S(this, FunctionParserState->takeScope());
  ParseFunctionBody CC(*this, AFD);

  ParserResult<BraceStmt> Body =
      parseBraceItemList(diag::func_decl_without_brace);
  if (Body.isNull()) {
    // FIXME: Should do some sort of error recovery here?
    return true;
  } else {
    AFD->setBody(Body.get());
  }

  return false;
}

/// \brief Parse a 'enum' declaration, returning true (and doing no token
/// skipping) on error.
///
/// \verbatim
///   decl-enum:
///      'enum' attribute-list identifier generic-params? inheritance?
///          '{' decl-enum-body '}'
///   decl-enum-body:
///      decl*
/// \endverbatim
ParserResult<EnumDecl> Parser::parseDeclEnum(unsigned Flags,
                                             DeclAttributes &Attributes) {
  SourceLoc EnumLoc = consumeToken(tok::kw_enum);

  Identifier EnumName;
  SourceLoc EnumNameLoc;
  ParserStatus Status;

  Status |=
      parseIdentifierDeclName(*this, EnumName, EnumNameLoc, tok::colon,
                              tok::l_brace, TokenProperty::StartsWithLess,
                              diag::expected_identifier_in_decl, "enum");
  if (Status.isError())
    return nullptr;

  // Parse the generic-params, if present.
  GenericParamList *GenericParams = nullptr;
  {
    Scope S(this, ScopeKind::Generics);
    GenericParams = maybeParseGenericParams();
  }

  EnumDecl *UD = new (Context) EnumDecl(EnumLoc, EnumName, EnumNameLoc,
                                        { }, GenericParams, CurDeclContext);
  setLocalDiscriminator(UD);

  if (Attributes.isValid())
    UD->getMutableAttrs() = Attributes;

  // Now that we have a context, update the generic parameters with that
  // context.
  if (GenericParams)
    for (auto Param : *GenericParams)
      Param.setDeclContext(UD);

  // Parse optional inheritance clause within the context of the enum.
  if (Tok.is(tok::colon)) {
    ContextChange CC(*this, UD);
    SmallVector<TypeLoc, 2> Inherited;
    Status |= parseInheritance(Inherited);
    UD->setInherited(Context.AllocateCopy(Inherited));
  }

  SmallVector<Decl*, 8> MemberDecls;
  SourceLoc LBLoc, RBLoc;
  if (parseToken(tok::l_brace, LBLoc, diag::expected_lbrace_enum)) {
    LBLoc = Tok.getLoc();
    RBLoc = LBLoc;
    Status.setIsParseError();
  } else {
    ContextChange CC(*this, UD);
    Scope S(this, ScopeKind::ClassBody);
    if (parseNominalDeclMembers(MemberDecls, LBLoc, RBLoc,
                                diag::expected_rbrace_enum,
                                PD_HasContainerType | PD_AllowEnumElement |
                                PD_DisallowStoredInstanceVar))
      Status.setIsParseError();
  }

  if (MemberDecls.empty())
    UD->setMembers({}, { LBLoc, RBLoc });
  else
    UD->setMembers(Context.AllocateCopy(MemberDecls), { LBLoc, RBLoc });
  addToScope(UD);

  if (Flags & PD_DisallowNominalTypes) {
    diagnose(EnumLoc, diag::disallowed_type);
    Status.setIsParseError();
  }

  return makeParserResult(Status, UD);
}

/// \brief Parse a 'case' of an enum.
///
/// \verbatim
///   enum-case:
///      identifier type-tuple?
///   decl-enum-element:
///      'case' attribute-list enum-case (',' enum-case)*
/// \endverbatim
ParserStatus Parser::parseDeclEnumCase(unsigned Flags,
                                       DeclAttributes &Attributes,
                                       llvm::SmallVectorImpl<Decl *> &Decls) {
  ParserStatus Status;
  SourceLoc CaseLoc = consumeToken(tok::kw_case);

  // Parse comma-separated enum elements.
  SmallVector<EnumElementDecl*, 4> Elements;
  
  SourceLoc CommaLoc;
  for (;;) {
    Identifier Name;
    SourceLoc NameLoc;

    const bool NameIsNotIdentifier = Tok.isNot(tok::identifier);
    if (parseIdentifierDeclName(*this, Name, NameLoc, tok::l_paren,
                                tok::kw_case, tok::colon, tok::r_brace,
                                diag::invalid_diagnostic).isError()) {
      NameLoc = CaseLoc;

      // Handle the likely case someone typed 'case X, case Y'.
      if (Tok.is(tok::kw_case) && CommaLoc.isValid()) {
        diagnose(Tok, diag::expected_identifier_after_case_comma);
        return Status;
      }
      
      // For recovery, see if the user typed something resembling a switch
      // "case" label.
      parseMatchingPattern();
    }
    if (NameIsNotIdentifier) {
      if (consumeIf(tok::colon)) {
        diagnose(CaseLoc, diag::case_outside_of_switch, "case");
        Status.setIsParseError();
        return Status;
      }
      if (CommaLoc.isValid()) {
        diagnose(Tok, diag::expected_identifier_after_case_comma);
        return Status;
      }
      diagnose(CaseLoc, diag::expected_identifier_in_decl, "enum case");
    }

    // See if there's a following argument type.
    ParserResult<TypeRepr> ArgType;
    if (Tok.isFollowingLParen()) {
      ArgType = parseTypeTupleBody();
      if (ArgType.hasCodeCompletion()) {
        Status.setHasCodeCompletion();
        return Status;
      }
      if (ArgType.isNull()) {
        Status.setIsParseError();
        return Status;
      }
    }
    
    // See if there's a raw value expression.
    SourceLoc EqualsLoc;
    ParserResult<Expr> RawValueExpr;
    LiteralExpr *LiteralRawValueExpr = nullptr;
    if (Tok.is(tok::equal)) {
      EqualsLoc = consumeToken();
      {
        CodeCompletionCallbacks::InEnumElementRawValueRAII
            InEnumElementRawValue(CodeCompletion);
        RawValueExpr = parseExpr(diag::expected_expr_enum_case_raw_value);
      }
      if (RawValueExpr.hasCodeCompletion()) {
        Status.setHasCodeCompletion();
        return Status;
      }
      if (RawValueExpr.isNull()) {
        Status.setIsParseError();
        return Status;
      }
      // The raw value must be syntactically a simple literal.
      LiteralRawValueExpr = dyn_cast<LiteralExpr>(RawValueExpr.getPtrOrNull());
      if (!LiteralRawValueExpr
          || isa<InterpolatedStringLiteralExpr>(LiteralRawValueExpr)) {
        diagnose(RawValueExpr.getPtrOrNull()->getLoc(),
                 diag::nonliteral_enum_case_raw_value);
        LiteralRawValueExpr = nullptr;
      }
    }
    
    // For recovery, again make sure the the user didn't try to spell a switch
    // case label:
    // 'case Identifier:' or
    // 'case Identifier where ...:'
    if (Tok.is(tok::colon) || Tok.is(tok::kw_where)) {
      diagnose(CaseLoc, diag::case_outside_of_switch, "case");
      skipUntilDeclRBrace();
      Status.setIsParseError();
      return Status;
    }
    
    // Create the element.
    auto *result = new (Context) EnumElementDecl(NameLoc, Name,
                                                 ArgType.getPtrOrNull(),
                                                 EqualsLoc,
                                                 LiteralRawValueExpr,
                                                 CurDeclContext);
    result->getMutableAttrs() = Attributes;
    Elements.push_back(result);
    
    // Continue through the comma-separated list.
    if (!Tok.is(tok::comma))
      break;
    CommaLoc = consumeToken(tok::comma);
  }
  
  if (!(Flags & PD_AllowEnumElement)) {
    diagnose(CaseLoc, diag::disallowed_enum_element);
    // Don't add the EnumElementDecls unless the current context
    // is allowed to have EnumElementDecls.
    Status.setIsParseError();
    return Status;
  }

  // Create and insert the EnumCaseDecl containing all the elements.
  auto TheCase = EnumCaseDecl::create(CaseLoc, Elements, CurDeclContext);
  Decls.push_back(TheCase);
  
  // Insert the element decls.
  std::copy(Elements.begin(), Elements.end(), std::back_inserter(Decls));
  return Status;
}

/// \brief Parse the members in a struct/class/protocol definition.
///
/// \verbatim
///    decl*
/// \endverbatim
bool Parser::parseNominalDeclMembers(SmallVectorImpl<Decl *> &memberDecls,
                                     SourceLoc LBLoc, SourceLoc &RBLoc,
                                     Diag<> ErrorDiag, unsigned flags) {
  bool previousHadSemi = true;
  parseList(tok::r_brace, LBLoc, RBLoc, tok::semi, /*OptionalSep=*/true,
            /*AllowSepAfterLast=*/false, ErrorDiag, [&]() -> ParserStatus {
    // If the previous declaration didn't have a semicolon and this new
    // declaration doesn't start a line, complain.
    if (!previousHadSemi && !Tok.isAtStartOfLine()) {
      SourceLoc endOfPrevious = getEndOfPreviousLoc();
      diagnose(endOfPrevious, diag::declaration_same_line_without_semi)
        .fixItInsert(endOfPrevious, ";");
      // FIXME: Add semicolon to the AST?
    }

    previousHadSemi = false;
    if (parseDecl(memberDecls, flags).isError())
      return makeParserError();

    // Check whether the previous declaration had a semicolon after it.
    if (!memberDecls.empty() && memberDecls.back()->TrailingSemiLoc.isValid())
      previousHadSemi = true;

    return makeParserSuccess();
  });

  // If we found the closing brace, then the caller should not care if there
  // were errors while parsing inner decls, because we recovered.
  return !RBLoc.isValid();
}

/// \brief Parse a 'struct' declaration, returning true (and doing no token
/// skipping) on error.
///
/// \verbatim
///   decl-struct:
///      'struct' attribute-list identifier generic-params? inheritance?
///          '{' decl-struct-body '}
///   decl-struct-body:
///      decl*
/// \endverbatim
ParserResult<StructDecl> Parser::parseDeclStruct(unsigned Flags,
                                                 DeclAttributes &Attributes) {
  SourceLoc StructLoc = consumeToken(tok::kw_struct);
  
  Identifier StructName;
  SourceLoc StructNameLoc;
  ParserStatus Status;

  Status |=
      parseIdentifierDeclName(*this, StructName, StructNameLoc, tok::colon,
                              tok::l_brace, TokenProperty::StartsWithLess,
                              diag::expected_identifier_in_decl, "struct");
  if (Status.isError())
    return nullptr;

  // Parse the generic-params, if present.
  GenericParamList *GenericParams = nullptr;
  {
    Scope S(this, ScopeKind::Generics);
    GenericParams = maybeParseGenericParams();
  }

  StructDecl *SD = new (Context) StructDecl(StructLoc, StructName,
                                            StructNameLoc,
                                            { },
                                            GenericParams,
                                            CurDeclContext);
  setLocalDiscriminator(SD);

  if (Attributes.isValid())
    SD->getMutableAttrs() = Attributes;

  // Now that we have a context, update the generic parameters with that
  // context.
  if (GenericParams) {
    for (auto Param : *GenericParams) {
      Param.setDeclContext(SD);
    }
  }

  // Parse optional inheritance clause within the context of the struct.
  if (Tok.is(tok::colon)) {
    ContextChange CC(*this, SD);
    SmallVector<TypeLoc, 2> Inherited;
    Status |= parseInheritance(Inherited);
    SD->setInherited(Context.AllocateCopy(Inherited));
  }

  SmallVector<Decl*, 8> MemberDecls;
  SourceLoc LBLoc, RBLoc;
  if (parseToken(tok::l_brace, LBLoc, diag::expected_lbrace_struct)) {
    LBLoc = Tok.getLoc();
    RBLoc = LBLoc;
    Status.setIsParseError();
  } else {
    // Parse the body.
    ContextChange CC(*this, SD);
    Scope S(this, ScopeKind::StructBody);
    if (parseNominalDeclMembers(MemberDecls, LBLoc, RBLoc,
                                diag::expected_rbrace_struct,
                                PD_HasContainerType))
      Status.setIsParseError();
  }

  if (MemberDecls.empty())
    SD->setMembers({}, { LBLoc, RBLoc });
  else
    SD->setMembers(Context.AllocateCopy(MemberDecls), { LBLoc, RBLoc });
  addToScope(SD);

  if (Flags & PD_DisallowNominalTypes) {
    diagnose(StructLoc, diag::disallowed_type);
    Status.setIsParseError();
  }

  return makeParserResult(Status, SD);
}

/// \brief Parse a 'class' declaration, doing no token skipping on error.
///
/// \verbatim
///   decl-class:
///      'class' attribute-list identifier generic-params? inheritance?
///          '{' decl-class-body '}
///   decl-class-body:
///      decl*
/// \endverbatim
ParserResult<ClassDecl> Parser::parseDeclClass(unsigned Flags,
                                               DeclAttributes &Attributes) {
  SourceLoc ClassLoc = consumeToken(tok::kw_class);

  Identifier ClassName;
  SourceLoc ClassNameLoc;
  ParserStatus Status;

  Status |=
      parseIdentifierDeclName(*this, ClassName, ClassNameLoc, tok::colon,
                              tok::l_brace, TokenProperty::StartsWithLess,
                              diag::expected_identifier_in_decl, "class");
  if (Status.isError())
    return nullptr;

  // Parse the generic-params, if present.
  GenericParamList *GenericParams = nullptr;
  {
    Scope S(this, ScopeKind::Generics);
    GenericParams = maybeParseGenericParams();
  }

  // Create the class.
  ClassDecl *CD = new (Context) ClassDecl(ClassLoc, ClassName, ClassNameLoc,
                                          { }, GenericParams, CurDeclContext);
  setLocalDiscriminator(CD);

  // Attach attributes.
  if (Attributes.isValid())
    CD->getMutableAttrs() = Attributes;

  // Now that we have a context, update the generic parameters with that
  // context.
  if (GenericParams) {
    for (auto Param : *GenericParams) {
      Param.setDeclContext(CD);
    }
  }

  // Parse optional inheritance clause within the context of the class.
  if (Tok.is(tok::colon)) {
    ContextChange CC(*this, CD);
    SmallVector<TypeLoc, 2> Inherited;
    Status |= parseInheritance(Inherited);
    CD->setInherited(Context.AllocateCopy(Inherited));
  }

  SmallVector<Decl*, 8> MemberDecls;
  SourceLoc LBLoc, RBLoc;
  if (parseToken(tok::l_brace, LBLoc, diag::expected_lbrace_class)) {
    LBLoc = Tok.getLoc();
    RBLoc = LBLoc;
    Status.setIsParseError();
  } else {
    // Parse the body.
    ContextChange CC(*this, CD);
    Scope S(this, ScopeKind::ClassBody);
    if (parseNominalDeclMembers(MemberDecls, LBLoc, RBLoc,
                                diag::expected_rbrace_class,
                                PD_HasContainerType | PD_AllowDestructor))
      Status.setIsParseError();
  }

  CD->setMembers(Context.AllocateCopy(MemberDecls), { LBLoc, RBLoc });
  addToScope(CD);

  if (Flags & PD_DisallowNominalTypes) {
    diagnose(ClassLoc, diag::disallowed_type);
    Status.setIsParseError();
  }

  return makeParserResult(Status, CD);
}

/// \brief Parse a 'protocol' declaration, doing no token skipping on error.
///
/// \verbatim
///   decl-protocol:
///      protocol-head '{' protocol-member* '}'
///
///   protocol-head:
///     'protocol' attribute-list identifier inheritance? 
///
///   protocol-member:
///      decl-func
///      decl-var-simple
///      decl-typealias
/// \endverbatim
ParserResult<ProtocolDecl> Parser::
parseDeclProtocol(unsigned Flags, DeclAttributes &Attributes) {
  SourceLoc ProtocolLoc = consumeToken(tok::kw_protocol);
  
  SourceLoc NameLoc;
  Identifier ProtocolName;
  ParserStatus Status;

  Status |=
      parseIdentifierDeclName(*this, ProtocolName, NameLoc, tok::colon,
                              tok::l_brace, diag::expected_identifier_in_decl,
                              "protocol");
  if (Status.isError())
    return nullptr;

  // Parse optional inheritance clause.
  SmallVector<TypeLoc, 4> InheritedProtocols;
  if (Tok.is(tok::colon))
    Status |= parseInheritance(InheritedProtocols);

  ProtocolDecl *Proto
    = new (Context) ProtocolDecl(CurDeclContext, ProtocolLoc, NameLoc,
                                 ProtocolName,
                                 Context.AllocateCopy(InheritedProtocols));
  // No need to setLocalDiscriminator: protocols can't appear in local contexts.

  if (Attributes.isValid())
    Proto->getMutableAttrs() = Attributes;

  ContextChange CC(*this, Proto);
  Scope ProtocolBodyScope(this, ScopeKind::ProtocolBody);

  // Parse the body.
  {
    // The list of protocol elements.
    SmallVector<Decl*, 8> Members;

    SourceLoc LBraceLoc;
    SourceLoc RBraceLoc;
    if (parseToken(tok::l_brace, LBraceLoc, diag::expected_lbrace_protocol)) {
      LBraceLoc = Tok.getLoc();
      RBraceLoc = LBraceLoc;
      Status.setIsParseError();
    } else {
      // Parse the members.
      if (parseNominalDeclMembers(Members, LBraceLoc, RBraceLoc,
                                  diag::expected_rbrace_protocol,
                                  PD_HasContainerType | PD_DisallowComputedVar |
                                  PD_DisallowFuncDef | PD_DisallowNominalTypes |
                                  PD_DisallowInit | PD_DisallowTypeAliasDef |
                                  PD_InProtocol))
        Status.setIsParseError();
    }

    // Install the protocol elements.
    Proto->setMembers(Context.AllocateCopy(Members), { LBraceLoc, RBraceLoc });
  }
  
  if (Flags & PD_DisallowNominalTypes) {
    diagnose(ProtocolLoc, diag::disallowed_type);
    Status.setIsParseError();
  } else if (!(Flags & PD_AllowTopLevel)) {
    diagnose(ProtocolLoc, diag::decl_inner_scope);
    Status.setIsParseError();
  }

  return makeParserResult(Status, Proto);
}

/// \brief Parse a 'subscript' declaration.
///
/// \verbatim
///   decl-subscript:
///     subscript-head get-set
///   subscript-head
///     'subscript' attribute-list pattern-tuple '->' type
/// \endverbatim
ParserStatus Parser::parseDeclSubscript(bool HasContainerType,
                                        bool NeedDefinition,
                                        DeclAttributes &Attributes,
                                        SmallVectorImpl<Decl *> &Decls) {
  ParserStatus Status;
  SourceLoc SubscriptLoc = consumeToken(tok::kw_subscript);

  // pattern-tuple
  if (Tok.isNot(tok::l_paren)) {
    diagnose(Tok, diag::expected_lparen_subscript);
    return makeParserError();
  }

  ParserResult<Pattern> Indices = parsePatternTuple(/*AllowInitExpr=*/false,
                                                    /*IsLet*/ false);
  if (Indices.isNull() || Indices.hasCodeCompletion())
    return Indices;
  Indices.get()->walk(SetVarContext(CurDeclContext));

  // '->'
  if (!Tok.is(tok::arrow)) {
    diagnose(Tok, diag::expected_arrow_subscript);
    return makeParserError();
  }
  SourceLoc ArrowLoc = consumeToken();
  
  // type
  ParserResult<TypeRepr> ElementTy =
      parseTypeAnnotation(diag::expected_type_subscript);
  if (ElementTy.isNull() || ElementTy.hasCodeCompletion())
    return ElementTy;

  // '{'
  // Parse getter and setter.
  SourceRange DefRange = SourceRange();
  FuncDecl *Get = nullptr;
  FuncDecl *Set = nullptr;
  if (Tok.is(tok::l_brace))  {
    SourceLoc LBLoc = consumeToken();
    
    SourceLoc LastValidLoc = LBLoc;
    if (parseGetSet(HasContainerType, Indices.get(), ElementTy.get(),
                    Get, Set, LastValidLoc, /*StaticLoc*/ SourceLoc()))
      Status.setIsParseError();

    // Parse the final '}'.
    SourceLoc RBLoc;
    if (Status.isError()) {
      skipUntilDeclRBrace();
      RBLoc = LastValidLoc;
    }

    if (parseMatchingToken(tok::r_brace, RBLoc, diag::expected_rbrace_in_getset,
                           LBLoc)) {
      RBLoc = LastValidLoc;
    }

    if (!Get) {
      if (Status.isSuccess())
        diagnose(SubscriptLoc, diag::subscript_without_get);
      Status.setIsParseError();
    }

    DefRange = SourceRange(LBLoc, RBLoc);

  } else  {
    if (NeedDefinition && !isInSILMode()) {
      diagnose(Tok, diag::expected_lbrace_subscript);
      return makeParserError();
    }
  }

  // Reject 'subscript' functions outside of type decls
  if (!HasContainerType) {
    diagnose(SubscriptLoc, diag::subscript_decl_wrong_scope);
    Status.setIsParseError();
  }

  if (Status.isSuccess()) {
    // FIXME: We should build the declarations even if they are invalid.

    // Build an AST for the subscript declaration.
    SubscriptDecl *Subscript
      = new (Context) SubscriptDecl(Context.getIdentifier("subscript"),
                                    SubscriptLoc, Indices.get(), ArrowLoc,
                                    ElementTy.get(), DefRange,
                                    Get, Set, CurDeclContext);
    // No need to setLocalDiscriminator because subscripts cannot
    // validly appear outside of type decls.

    if (Attributes.isValid())
      Subscript->getMutableAttrs() = Attributes;

    Decls.push_back(Subscript);

    if (Set)
      Set->makeSetter(Subscript);
    if (Get)
      Get->makeGetter(Subscript);

    // Add get/set in source order.
    FuncDecl* Accessors[2] = {Get, Set};
    if (Accessors[0] && Accessors[1] &&
        !SourceMgr.isBeforeInBuffer(Accessors[0]->getFuncLoc(),
                                    Accessors[1]->getFuncLoc())) {
      std::swap(Accessors[0], Accessors[1]);
    }
    for (auto FD : Accessors) {
      if (FD) {
        FD->setDeclContext(CurDeclContext);
        Decls.push_back(FD);
      }
    }
  }

  return Status;
}

ParserResult<ConstructorDecl>
Parser::parseDeclConstructor(unsigned Flags, DeclAttributes &Attributes) {
  assert(Tok.is(tok::kw_init));
  SourceLoc ConstructorLoc = consumeToken();

  const bool ConstructorsNotAllowed =
      !(Flags & PD_HasContainerType) || (Flags & PD_InProtocol);

  // Reject constructors outside of types.
  if (ConstructorsNotAllowed) {
    diagnose(Tok, diag::initializer_decl_wrong_scope);
  }

  // Parse the generic-params, if present.
  Scope S(this, ScopeKind::Generics);
  GenericParamList *GenericParams = maybeParseGenericParams();

  // Parse the parameters.
  // FIXME: handle code completion in Arguments.
  Pattern *ArgPattern;
  Pattern *BodyPattern;
  bool HasSelectorStyleSignature;
  ParserStatus SignatureStatus =
      parseConstructorArguments(ArgPattern, BodyPattern,
                                HasSelectorStyleSignature);

  if (SignatureStatus.hasCodeCompletion() && !CodeCompletion) {
    // Trigger delayed parsing, no need to continue.
    return SignatureStatus;
  }

  VarDecl *SelfDecl
    = new (Context) VarDecl(/*static*/ false, /*IsLet*/ false,
                            SourceLoc(), Context.SelfIdentifier,
                            Type(), CurDeclContext);
  SelfDecl->setImplicit();

  Scope S2(this, ScopeKind::ConstructorBody);
  ConstructorDecl *CD =
      new (Context) ConstructorDecl(Context.getIdentifier("init"),
                                    ConstructorLoc, ArgPattern, BodyPattern,
                                    SelfDecl, GenericParams, CurDeclContext);
  // No need to setLocalDiscriminator.

  if (HasSelectorStyleSignature)
    CD->setHasSelectorStyleSignature();

  SelfDecl->setDeclContext(CD);

  // Pass the function signature to code completion.
  if (SignatureStatus.hasCodeCompletion())
    CodeCompletion->setDelayedParsedDecl(CD);

  if (ConstructorsNotAllowed) {
    // Tell the type checker not to touch this constructor.
    CD->setInvalid();
  }
  if (GenericParams) {
    for (auto Param : *GenericParams)
      Param.setDeclContext(CD);
  }
  addFunctionParametersToScope(BodyPattern, CD);
  ArgPattern->walk(SetVarContext(CD));

  addToScope(SelfDecl);

  // '{'
  if (!Tok.is(tok::l_brace)) {
    if (!isInSILMode()) {
      if (!SignatureStatus.isError()) {
        // Don't emit this diagnostic if we already complained about this
        // constructor decl.
        diagnose(Tok, diag::expected_lbrace_initializer);
      }

      // FIXME: This is brutal. Can't we at least return the declaration?
      return nullptr;
    }
  } else {
    // Parse the body.
    ParseFunctionBody CC(*this, CD);

    if (!isDelayedParsingEnabled()) {
      ParserResult<BraceStmt> Body = parseBraceItemList(diag::invalid_diagnostic);

      if (!Body.isNull())
        CD->setBody(Body.get());
    } else {
      consumeAbstractFunctionBody(CD, Attributes);
    }
  }

  if (Attributes.isValid())
    CD->getMutableAttrs() = Attributes;

  return makeParserResult(CD);
}

ParserResult<DestructorDecl> Parser::
parseDeclDestructor(unsigned Flags, DeclAttributes &Attributes) {
  SourceLoc DestructorLoc = consumeToken(tok::kw_destructor);

  ParserResult<Pattern> Params;
  if (Tok.is(tok::l_paren)) {
    // Parse the parameter tuple.
    SourceLoc LParenLoc = Tok.getLoc();
    ParserResult<Pattern> Params = parsePatternTuple(/*AllowInitExpr=*/true,
                                                     /*IsLet*/false);
    if (!Params.isParseError()) {
      // Check that the destructor has zero parameters.
      SourceRange ElementsRange;
      SourceLoc RParenLoc;
      if (auto Tuple = dyn_cast<TuplePattern>(Params.get())) {
        auto Fields = Tuple->getFields();
        if (!Fields.empty()) {
          ElementsRange = { Fields.front().getPattern()->getStartLoc(),
                            Fields.back().getPattern()->getEndLoc() };
          RParenLoc = Tuple->getRParenLoc();
        }
      } else {
        auto Paren = cast<ParenPattern>(Params.get());
        ElementsRange = Paren->getSubPattern()->getSourceRange();
        RParenLoc = Paren->getRParenLoc();
      }
      if (ElementsRange.isValid()) {
        diagnose(LParenLoc, diag::destructor_parameter_nonempty_tuple)
            .fixItRemove(ElementsRange);
        Params = makeParserErrorResult(
            TuplePattern::create(Context, LParenLoc,
                                 ArrayRef<TuplePatternElt>(), RParenLoc));
      }
    }
  } else {
    SourceLoc AfterDestructorKw =
        Lexer::getLocForEndOfToken(SourceMgr, DestructorLoc);
    diagnose(AfterDestructorKw, diag::expected_lparen_destructor)
        .fixItInsert(AfterDestructorKw, "()");
    Params = makeParserErrorResult(
        TuplePattern::create(Context, Tok.getLoc(),
                             ArrayRef<TuplePatternElt>(), Tok.getLoc()));
  }

  // '{'
  if (!Tok.is(tok::l_brace)) {
    if (!Tok.is(tok::l_brace) && !isInSILMode()) {
      diagnose(Tok, diag::expected_lbrace_destructor);
      return nullptr;
    }
  }

  VarDecl *SelfDecl
    = new (Context) VarDecl(/*static*/ false, /*IsLet*/ false,
                            SourceLoc(), Context.SelfIdentifier,
                            Type(), CurDeclContext);
  SelfDecl->setImplicit();

  Scope S(this, ScopeKind::DestructorBody);
  DestructorDecl *DD
    = new (Context) DestructorDecl(Context.getIdentifier("destructor"),
                                 DestructorLoc, SelfDecl, CurDeclContext);
  // No need to setLocalDiscriminator.

  SelfDecl->setDeclContext(DD);
  addToScope(SelfDecl);

  // Parse the body.
  if (Tok.is(tok::l_brace)) {
    ParseFunctionBody CC(*this, DD);
    if (!isDelayedParsingEnabled()) {
      ParserResult<BraceStmt> Body = parseBraceItemList(diag::invalid_diagnostic);

      if (!Body.isNull())
        DD->setBody(Body.get());
    } else {
      consumeAbstractFunctionBody(DD, Attributes);
    }
  }

  if (Attributes.isValid())
    DD->getMutableAttrs() = Attributes;

  // Reject 'destructor' functions outside of classes
  if (!(Flags & PD_AllowDestructor)) {
    diagnose(DestructorLoc, diag::destructor_decl_outside_class);

    // Tell the type checker not to touch this destructor.
    DD->setInvalid();
  }

  return makeParserResult(DD);
}

ParserResult<OperatorDecl> Parser::parseDeclOperator(bool AllowTopLevel,
                                                  DeclAttributes &Attributes) {
  assert(Tok.isContextualKeyword("operator") &&
         "no 'operator' at start of operator decl?!");

  SourceLoc OperatorLoc = consumeToken(tok::identifier);

  if (!Attributes.empty())
    diagnose(Attributes.AtLoc, diag::operator_attributes);
  
  auto kind = llvm::StringSwitch<Optional<DeclKind>>(Tok.getText())
    .Case("prefix", DeclKind::PrefixOperator)
    .Case("postfix", DeclKind::PostfixOperator)
    .Case("infix", DeclKind::InfixOperator)
    .Default(Nothing);
  
  assert(kind && "no fixity after 'operator'?!");

  SourceLoc KindLoc = consumeToken(tok::identifier);

  if (!Tok.isAnyOperator() && !Tok.is(tok::exclaim_postfix)) {
    diagnose(Tok, diag::expected_operator_name_after_operator);
    return nullptr;
  }

  Identifier Name = Context.getIdentifier(Tok.getText());
  SourceLoc NameLoc = consumeToken();

  // Postfix operator '!' is reserved.
  if (*kind == DeclKind::PostfixOperator &&Name.str().equals("!")) {
    diagnose(NameLoc, diag::custom_operator_postfix_exclaim);
  }

  if (!Tok.is(tok::l_brace)) {
    diagnose(Tok, diag::expected_lbrace_after_operator);
    return nullptr;
  }
  
  ParserResult<OperatorDecl> Result;
  
  switch (*kind) {
  case DeclKind::PrefixOperator:
    Result = parseDeclPrefixOperator(OperatorLoc, KindLoc, Name, NameLoc);
    break;
  case DeclKind::PostfixOperator:
    Result = parseDeclPostfixOperator(OperatorLoc, KindLoc, Name, NameLoc);
    break;
  case DeclKind::InfixOperator:
    Result = parseDeclInfixOperator(OperatorLoc, KindLoc, Name, NameLoc);
    break;
  default:
    llvm_unreachable("impossible");
  }
  
  if (Tok.is(tok::r_brace))
    consumeToken();
  
  if (!AllowTopLevel) {
    diagnose(OperatorLoc, diag::operator_decl_inner_scope);
    return nullptr;
  }
  
  return Result;
}

ParserResult<OperatorDecl>
Parser::parseDeclPrefixOperator(SourceLoc OperatorLoc, SourceLoc PrefixLoc,
                                Identifier Name, SourceLoc NameLoc) {
  SourceLoc LBraceLoc = consumeToken(tok::l_brace);
  
  while (!Tok.is(tok::r_brace)) {
    // Currently there are no operator attributes for prefix operators.
    if (Tok.is(tok::identifier))
      diagnose(Tok, diag::unknown_prefix_operator_attribute, Tok.getText());
    else
      diagnose(Tok, diag::expected_operator_attribute);
    skipUntilDeclRBrace();
    return nullptr;
  }
  
  SourceLoc RBraceLoc = Tok.getLoc();

  return makeParserResult(
      new (Context) PrefixOperatorDecl(CurDeclContext, OperatorLoc, PrefixLoc,
                                       Name, NameLoc, LBraceLoc, RBraceLoc));
}

ParserResult<OperatorDecl>
Parser::parseDeclPostfixOperator(SourceLoc OperatorLoc, SourceLoc PostfixLoc,
                                 Identifier Name, SourceLoc NameLoc) {
  SourceLoc LBraceLoc = consumeToken(tok::l_brace);
  
  while (!Tok.is(tok::r_brace)) {
    // Currently there are no operator attributes for postfix operators.
    if (Tok.is(tok::identifier))
      diagnose(Tok, diag::unknown_postfix_operator_attribute, Tok.getText());
    else
      diagnose(Tok, diag::expected_operator_attribute);
    skipUntilDeclRBrace();
    return nullptr;
  }
  
  SourceLoc RBraceLoc = Tok.getLoc();
  
  return makeParserResult(
      new (Context) PostfixOperatorDecl(CurDeclContext, OperatorLoc,
                                        PostfixLoc, Name, NameLoc, LBraceLoc,
                                        RBraceLoc));
}

ParserResult<OperatorDecl>
Parser::parseDeclInfixOperator(SourceLoc OperatorLoc, SourceLoc InfixLoc,
                               Identifier Name, SourceLoc NameLoc) {
  SourceLoc LBraceLoc = consumeToken(tok::l_brace);

  // Initialize InfixData with default attributes:
  // precedence 100, associativity none
  unsigned char precedence = 100;
  Associativity associativity = Associativity::None;
  
  SourceLoc AssociativityLoc, AssociativityValueLoc,
    PrecedenceLoc, PrecedenceValueLoc;
  
  while (!Tok.is(tok::r_brace)) {
    if (!Tok.is(tok::identifier)) {
      diagnose(Tok, diag::expected_operator_attribute);
      skipUntilDeclRBrace();
      return nullptr;
    }
    
    if (Tok.getText().equals("associativity")) {
      if (AssociativityLoc.isValid()) {
        diagnose(Tok, diag::operator_associativity_redeclared);
        skipUntilDeclRBrace();
        return nullptr;
      }
      AssociativityLoc = consumeToken();
      if (!Tok.is(tok::identifier)) {
        diagnose(Tok, diag::expected_infix_operator_associativity);
        skipUntilDeclRBrace();
        return nullptr;
      }
      auto parsedAssociativity
        = llvm::StringSwitch<Optional<Associativity>>(Tok.getText())
          .Case("none", Associativity::None)
          .Case("left", Associativity::Left)
          .Case("right", Associativity::Right)
          .Default(Nothing);
      if (!parsedAssociativity) {
        diagnose(Tok, diag::unknown_infix_operator_associativity, Tok.getText());
        skipUntilDeclRBrace();
        return nullptr;
      }
      associativity = *parsedAssociativity;

      AssociativityValueLoc = consumeToken();
      continue;
    }
    
    if (Tok.getText().equals("precedence")) {
      if (PrecedenceLoc.isValid()) {
        diagnose(Tok, diag::operator_precedence_redeclared);
        skipUntilDeclRBrace();
        return nullptr;
      }
      PrecedenceLoc = consumeToken();
      if (!Tok.is(tok::integer_literal)) {
        diagnose(Tok, diag::expected_infix_operator_precedence);
        skipUntilDeclRBrace();
        return nullptr;
      }
      if (Tok.getText().getAsInteger(0, precedence)) {
        diagnose(Tok, diag::invalid_infix_operator_precedence);
        precedence = 255;
      }
      
      PrecedenceValueLoc = consumeToken();
      continue;
    }
    
    diagnose(Tok, diag::unknown_infix_operator_attribute, Tok.getText());
    skipUntilDeclRBrace();
    return nullptr;
  }
  
  SourceLoc RBraceLoc = Tok.getLoc();
  
  return makeParserResult(new (Context) InfixOperatorDecl(
      CurDeclContext, OperatorLoc, InfixLoc, Name, NameLoc, LBraceLoc,
      AssociativityLoc, AssociativityValueLoc, PrecedenceLoc,
      PrecedenceValueLoc, RBraceLoc, InfixData(precedence, associativity)));
}
