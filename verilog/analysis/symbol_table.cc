// Copyright 2017-2020 The Verible Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "verilog/analysis/symbol_table.h"

#include <iostream>
#include <sstream>
#include <stack>

#include "absl/status/status.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "common/strings/display_utils.h"
#include "common/text/concrete_syntax_leaf.h"
#include "common/text/concrete_syntax_tree.h"
#include "common/text/token_info.h"
#include "common/text/tree_context_visitor.h"
#include "common/text/tree_utils.h"
#include "common/text/visitors.h"
#include "common/util/enum_flags.h"
#include "common/util/logging.h"
#include "common/util/spacer.h"
#include "common/util/value_saver.h"
#include "verilog/CST/class.h"
#include "verilog/CST/declaration.h"
#include "verilog/CST/functions.h"
#include "verilog/CST/macro.h"
#include "verilog/CST/module.h"
#include "verilog/CST/net.h"
#include "verilog/CST/package.h"
#include "verilog/CST/parameters.h"
#include "verilog/CST/port.h"
#include "verilog/CST/seq_block.h"
#include "verilog/CST/statement.h"
#include "verilog/CST/tasks.h"
#include "verilog/CST/type.h"
#include "verilog/CST/verilog_nonterminals.h"
#include "verilog/analysis/verilog_project.h"
#include "verilog/parser/verilog_parser.h"
#include "verilog/parser/verilog_token_enum.h"

namespace verilog {

using verible::AutoTruncate;
using verible::StringSpanOfSymbol;
using verible::SyntaxTreeLeaf;
using verible::SyntaxTreeNode;
using verible::TokenInfo;
using verible::TreeContextVisitor;
using verible::ValueSaver;

// Returns string_view of `text` with outermost double-quotes removed.
// If `text` is not wrapped in quotes, return it as-is.
static absl::string_view StripOuterQuotes(absl::string_view text) {
  return absl::StripSuffix(absl::StripPrefix(text, "\""), "\"");
}

static const verible::EnumNameMap<SymbolType> kSymbolInfoNames({
    // short-hand annotation for identifier reference type
    {"<root>", SymbolType::kRoot},
    {"class", SymbolType::kClass},
    {"module", SymbolType::kModule},
    {"package", SymbolType::kPackage},
    {"parameter", SymbolType::kParameter},
    {"typedef", SymbolType::kTypeAlias},
    {"data/net/var/instance", SymbolType::kDataNetVariableInstance},
    {"function", SymbolType::kFunction},
    {"task", SymbolType::kTask},
    {"interface", SymbolType::kInterface},
    {"<unspecified>", SymbolType::kUnspecified},
    {"<callable>", SymbolType::kCallable},
});

std::ostream& operator<<(std::ostream& stream, SymbolType symbol_type) {
  return kSymbolInfoNames.Unparse(symbol_type, stream);
}

static absl::string_view SymbolTypeAsString(SymbolType type) {
  return kSymbolInfoNames.EnumName(type);
}

// Root SymbolTableNode has no key, but we identify it as "$root"
static constexpr absl::string_view kRoot("$root");

std::ostream& SymbolTableNodeFullPath(std::ostream& stream,
                                      const SymbolTableNode& node) {
  if (node.Parent() != nullptr) {
    SymbolTableNodeFullPath(stream, *node.Parent()) << "::" << *node.Key();
  } else {
    stream << kRoot;
  }
  return stream;
}

static std::string ContextFullPath(const SymbolTableNode& context) {
  std::ostringstream stream;
  SymbolTableNodeFullPath(stream, context);
  return stream.str();
}

std::ostream& ReferenceNodeFullPath(std::ostream& stream,
                                    const ReferenceComponentNode& node) {
  if (node.Parent() != nullptr) {
    ReferenceNodeFullPath(stream, *node.Parent());  // recursive
  }
  return node.Value().PrintPathComponent(stream);
}

static std::string ReferenceNodeFullPathString(
    const ReferenceComponentNode& node) {
  std::ostringstream stream;
  ReferenceNodeFullPath(stream, node);
  return stream.str();
}

// Validates iterator/pointer stability when calling VectorTree::NewChild.
// Detects unwanted reallocation.
static void CheckedNewChildReferenceNode(ReferenceComponentNode* parent,
                                         const ReferenceComponent& component) {
  const auto* saved_begin = parent->Children().data();
  parent->NewChild(component);  // copy
  if (parent->Children().size() > 1) {
    // Check that iterators/pointers were not invalidated by a reallocation.
    CHECK_EQ(parent->Children().data(), saved_begin)
        << "Reallocation invalidated pointers to reference nodes at " << *parent
        << ".  Fix: pre-allocate child nodes.";
  }
  // Otherwise, this first node had no prior siblings, so no need to check.
}

static absl::Status DiagnoseMemberSymbolResolutionFailure(
    absl::string_view name, const SymbolTableNode& context) {
  const absl::string_view context_name =
      context.Parent() == nullptr ? kRoot : *context.Key();
  return absl::NotFoundError(absl::StrCat(
      "No member symbol \"", name, "\" in parent scope (",
      SymbolTypeAsString(context.Value().type), ") ", context_name, "."));
}

class SymbolTable::Builder : public TreeContextVisitor {
 public:
  Builder(const VerilogSourceFile& source, SymbolTable* symbol_table,
          VerilogProject* project)
      : source_(&source),
        token_context_(MakeTokenContext()),
        symbol_table_(symbol_table),
        current_scope_(&symbol_table_->MutableRoot()) {}

  std::vector<absl::Status> TakeDiagnostics() {
    return std::move(diagnostics_);
  }

 private:  // methods
  void Visit(const SyntaxTreeNode& node) final {
    const auto tag = static_cast<NodeEnum>(node.Tag().tag);
    VLOG(1) << __FUNCTION__ << " [node]: " << tag;
    switch (tag) {
      case NodeEnum::kModuleDeclaration:
        DeclareModule(node);
        break;
      case NodeEnum::kGenerateIfClause:
        DeclareGenerateIf(node);
        break;
      case NodeEnum::kGenerateElseClause:
        DeclareGenerateElse(node);
        break;
      case NodeEnum::kPackageDeclaration:
        DeclarePackage(node);
        break;
      case NodeEnum::kClassDeclaration:
        DeclareClass(node);
        break;
      case NodeEnum::kFunctionPrototype:  // fall-through
      case NodeEnum::kFunctionDeclaration:
        DeclareFunction(node);
        break;
      case NodeEnum::kFunctionHeader:
        SetupFunctionHeader(node);
        break;
      case NodeEnum::kTaskPrototype:  // fall-through
      case NodeEnum::kTaskDeclaration:
        DeclareTask(node);
        break;
        // No special handling needed for kTaskHeader
      case NodeEnum::kPortList:
        DeclarePorts(node);
        break;
      case NodeEnum::kPortItem:         // fall-through
                                        // for function/task parameters
      case NodeEnum::kPortDeclaration:  // fall-through
      case NodeEnum::kNetDeclaration:   // fall-through
      case NodeEnum::kDataDeclaration:
        DeclareData(node);
        break;
      case NodeEnum::kParamDeclaration:
        DeclareParameter(node);
        break;
      case NodeEnum::kTypeInfo:  // fall-through
      case NodeEnum::kDataType:
        DescendDataType(node);
        break;
      case NodeEnum::kReferenceCallBase:
        DescendReferenceExpression(node);
        break;
      case NodeEnum::kActualParameterList:
        DescendActualParameterList(node);
        break;
      case NodeEnum::kPortActualList:
        DescendPortActualList(node);
        break;
      case NodeEnum::kGateInstanceRegisterVariableList: {
        // TODO: reserve() to guarantee pointer/iterator stability in VectorTree
        Descend(node);
        break;
      }
      case NodeEnum::kNetVariable:
        DeclareNet(node);
        break;
      case NodeEnum::kRegisterVariable:
        DeclareRegister(node);
        break;
      case NodeEnum::kGateInstance:
        DeclareInstance(node);
        break;
      case NodeEnum::kQualifiedId:
        HandleQualifiedId(node);
        break;
      case NodeEnum::kPreprocessorInclude:
        EnterIncludeFile(node);
        break;
      default:
        Descend(node);
        break;
    }
    VLOG(1) << "end of " << __FUNCTION__ << " [node]: " << tag;
  }

  // This overload enters 'scope' for the duration of the call.
  // New declared symbols will belong to that scope.
  void Descend(const SyntaxTreeNode& node, SymbolTableNode& scope) {
    const ValueSaver<SymbolTableNode*> save_scope(&current_scope_, &scope);
    Descend(node);
  }

  void Descend(const SyntaxTreeNode& node) {
    TreeContextVisitor::Visit(node);  // maintains syntax tree Context() stack.
  }

  // RAII-class balance the Builder::references_builders_ stack.
  // The work of moving collecting references into the current scope is done in
  // the destructor.
  class CaptureDependentReference {
   public:
    CaptureDependentReference(Builder* builder) : builder_(builder) {
      // Push stack space to capture references.
      builder_->reference_builders_.emplace(/* DependentReferences */);
    }

    ~CaptureDependentReference() {
      // This completes the capture of a chain of dependent references.
      // Ref() can be empty if the subtree doesn't reference any identifiers.
      // Empty refs are non-actionable and must be excluded.
      DependentReferences& ref(Ref());
      if (!ref.Empty()) {
        builder_->current_scope_->Value().local_references_to_bind.emplace_back(
            std::move(ref));
      }
      builder_->reference_builders_.pop();
    }

    // Returns the chain of dependent references that were built.
    DependentReferences& Ref() const {
      return builder_->reference_builders_.top();
    }

   private:
    Builder* builder_;
  };

  void DescendReferenceExpression(const SyntaxTreeNode& reference) {
    // capture exressions referenced from the current scope
    const CaptureDependentReference capture(this);

    // subexpressions' references will be collected before this one
    Descend(reference);  // no scope change
  }

  // Traverse a subtree for a data type and collects type references
  // originating from the current context.
  // If the context is such that this type is used in a declaration,
  // then capture that type information to be used later.
  //
  // The state/stack management here is intended to accommodate type references
  // of arbitrary complexity.
  // A generalized type could look like:
  //   "A#(.B(1))::C#(.D(E#(.F(0))))::G"
  // This should produce the following reference trees:
  //   A -+- ::B
  //      |
  //      \- ::C -+- ::D
  //              |
  //              \- ::G
  //   E -+- ::F
  //
  void DescendDataType(const SyntaxTreeNode& data_type_node) {
    VLOG(1) << __FUNCTION__ << ": " << StringSpanOfSymbol(data_type_node);
    const CaptureDependentReference capture(this);

    {
      // Clearing declaration_type_info_ prevents nested types from being
      // captured.  e.g. in "A_type#(B_type)", "B_type" will beget a chain of
      // DependentReferences in the current context, but will not be involved
      // with the current declaration.
      const ValueSaver<DeclarationTypeInfo*> not_decl_type(
          &declaration_type_info_, nullptr);

      // Inform that named parameter identifiers will yield parallel children
      // from this reference branch point.  Start this out as nullptr, and set
      // it once an unqualified identifier is encountered that starts a
      // reference tree.
      const ValueSaver<ReferenceComponentNode*> set_branch(
          &reference_branch_point_, nullptr);

      Descend(data_type_node);
      // declaration_type_info_ will be restored after this closes.
    }

    if (declaration_type_info_ != nullptr) {
      // 'declaration_type_info_' holds the declared type we want to capture.
      if (verible::GetLeftmostLeaf(data_type_node) != nullptr) {
        declaration_type_info_->syntax_origin = &data_type_node;
        // Otherwise, if the type subtree contains no leaves (e.g. implicit or
        // void), then do not assign a syntax origin.
      }
      if (!capture.Ref()
               .Empty()) {  // then some user-defined type was referenced
        declaration_type_info_->user_defined_type = capture.Ref().LastLeaf();
      }
    }

    // In all cases, a type is being referenced from the current scope, so add
    // it to the list of references to resolve (done by 'capture').
    VLOG(1) << "end of " << __FUNCTION__;
  }

  void DescendActualParameterList(const SyntaxTreeNode& node) {
    if (reference_branch_point_ != nullptr) {
      // Pre-allocate siblings to guarantee pointer/iterator stability.
      // FindAll* will also catch actual port connections inside preprocessing
      // conditionals.
      const size_t num_params = FindAllNamedParams(node).size();
      reference_branch_point_->Children().reserve(num_params);
    }
    Descend(node);
  }

  void DescendPortActualList(const SyntaxTreeNode& node) {
    if (reference_branch_point_ != nullptr) {
      // Pre-allocate siblings to guarantee pointer/iterator stability.
      // FindAll* will also catch actual port connections inside preprocessing
      // conditionals.
      const size_t num_ports = FindAllActualNamedPort(node).size();
      reference_branch_point_->Children().reserve(num_ports);
    }
    Descend(node);
  }

  void HandleIdentifier(const SyntaxTreeLeaf& leaf) {
    const absl::string_view text = leaf.get().text();
    if (Context().DirectParentIs(NodeEnum::kParamType)) {
      // This identifier declares a parameter.
      EmplaceTypedElementInCurrentScope(leaf, text, SymbolType::kParameter);
      return;
    }
    if (Context().DirectParentsAre(
            {NodeEnum::kUnqualifiedId, NodeEnum::kPortDeclaration}) ||
        Context().DirectParentsAre(
            {NodeEnum::kUnqualifiedId,
             NodeEnum::kDataTypeImplicitBasicIdDimensions,
             NodeEnum::kPortItem})) {
      // This identifier declares a (non-parameter) port (of a module,
      // function, task).
      EmplaceTypedElementInCurrentScope(leaf, text,
                                        SymbolType::kDataNetVariableInstance);
      // TODO(fangism): Add attributes to distinguish public ports from
      // private internals members.
      return;
    }

    if (Context().DirectParentsAre(
            {NodeEnum::kUnqualifiedId, NodeEnum::kFunctionHeader})) {
      // We deferred adding a declared function to the current scope until this
      // point (from DeclareFunction()).
      // Note that this excludes the out-of-line definition case,
      // which is handled in DescendThroughOutOfLineDefinition().

      const SyntaxTreeNode* decl_syntax =
          Context().NearestParentMatching([](const SyntaxTreeNode& node) {
            return node.MatchesTagAnyOf(
                {NodeEnum::kFunctionDeclaration, NodeEnum::kFunctionPrototype});
          });
      if (decl_syntax == nullptr) return;
      SymbolTableNode* declared_function = &EmplaceTypedElementInCurrentScope(
          *decl_syntax, text, SymbolType::kFunction);
      // After this point, we've registered the new function with its return
      // type, so we can switch context over to the newly declared function
      // for its port interface and definition internals.
      current_scope_ = declared_function;
      return;
    }

    if (Context().DirectParentsAre(
            {NodeEnum::kUnqualifiedId, NodeEnum::kTaskHeader})) {
      // We deferred adding a declared task to the current scope until this
      // point (from DeclareFunction()).
      // Note that this excludes the out-of-line definition case,
      // which is handled in DescendThroughOutOfLineDefinition().

      const SyntaxTreeNode* decl_syntax =
          Context().NearestParentMatching([](const SyntaxTreeNode& node) {
            return node.MatchesTagAnyOf(
                {NodeEnum::kTaskDeclaration, NodeEnum::kTaskPrototype});
          });
      if (decl_syntax == nullptr) return;
      SymbolTableNode* declared_task =
          &EmplaceElementInCurrentScope(*decl_syntax, text, SymbolType::kTask);
      // After this point, we've registered the new task,
      // so we can switch context over to the newly declared function
      // for its port interface and definition internals.
      current_scope_ = declared_task;
      return;
    }

    // In DeclareInstance(), we already planted a self-reference that is
    // resolved to the instance being declared.
    if (Context().DirectParentIs(NodeEnum::kGateInstance)) return;

    // Capture only referencing identifiers, omit declarative identifiers.
    // This is set up when traversing references, e.g. types, expressions.
    // All of the code below takes effect inside a CaptureDependentReferences
    // RAII block.
    if (reference_builders_.empty()) return;

    // Building a reference, possible part of a chain or qualified
    // reference.
    DependentReferences& ref(reference_builders_.top());

    const ReferenceComponent new_ref{
        .identifier = text,
        .ref_type = InferReferenceType(),
        .metatype = InferMetaType(),
    };

    // For instances' named ports, and types' named parameters,
    // add references as siblings of the same parent.
    // (Recall that instances form self-references).
    if (Context().DirectParentIsOneOf(
            {NodeEnum::kActualNamedPort, NodeEnum::kParamByName})) {
      CheckedNewChildReferenceNode(ABSL_DIE_IF_NULL(reference_branch_point_),
                                   new_ref);
      return;
    }

    // For all other cases, grow the reference chain deeper.
    ref.PushReferenceComponent(new_ref);
    if (reference_branch_point_ == nullptr) {
      // For type references, which may contained named parameters,
      // when encountering the first unqualified reference, establish its
      // reference node as the point from which named parameter references
      // get added as siblings.
      // e.g. "A#(.B(...), .C(...))" would result in a reference tree:
      //   A -+- ::B
      //      |
      //      \- ::C
      reference_branch_point_ = ref.components.get();
    }
  }

  void Visit(const SyntaxTreeLeaf& leaf) final {
    const auto tag = leaf.Tag().tag;
    VLOG(1) << __FUNCTION__ << " [leaf]: " << VerboseToken(leaf.get());
    switch (tag) {
      case verilog_tokentype::SymbolIdentifier:
        HandleIdentifier(leaf);
        break;

      case verilog_tokentype::TK_SCOPE_RES:  // "::"
      case '.':
        last_hierarchy_operator_ = &leaf.get();
        break;

      default:
        break;
    }
    VLOG(1) << "end " << __FUNCTION__ << " [leaf]:" << VerboseToken(leaf.get());
  }

  // Distinguish between '.' and "::" hierarchy in reference components.
  ReferenceType InferReferenceType() const {
    CHECK(!reference_builders_.empty())
        << "Not currently in a reference context.";
    const DependentReferences& ref(reference_builders_.top());
    if (ref.Empty() || last_hierarchy_operator_ == nullptr) {
      // The root component is always treated as unqualified.

      // Out-of-line definitions' base/outer references must be resolved
      // immediately.
      if (Context().DirectParentsAre({NodeEnum::kUnqualifiedId,
                                      NodeEnum::kQualifiedId,  // out-of-line
                                      NodeEnum::kFunctionHeader})) {
        return ReferenceType::kImmediate;
      }
      if (Context().DirectParentsAre({NodeEnum::kUnqualifiedId,
                                      NodeEnum::kQualifiedId,  // out-of-line
                                      NodeEnum::kTaskHeader})) {
        return ReferenceType::kImmediate;
      }

      return ReferenceType::kUnqualified;
    }
    if (Context().DirectParentIs(NodeEnum::kParamByName)) {
      // Even though named parameters are referenced with ".PARAM",
      // they are branched off of a base reference that already points
      // to the type whose scope should be used, so no additional typeof()
      // indirection is needed.
      return ReferenceType::kDirectMember;
    }
    return ABSL_DIE_IF_NULL(last_hierarchy_operator_)->token_enum() == '.'
               ? ReferenceType::kMemberOfTypeOfParent
               : ReferenceType::kDirectMember;
  }

  // Does the context necessitate that the symbol being referenced have a
  // particular metatype?
  SymbolType InferMetaType() const {
    const DependentReferences& ref(reference_builders_.top());
    // Out-of-line definitions' base/outer references must be resolved
    // immediately to a class.
    // Member references (inner) is a function or task, depending on header
    // type.
    if (Context().DirectParentsAre({NodeEnum::kUnqualifiedId,
                                    NodeEnum::kQualifiedId,  // out-of-line
                                    NodeEnum::kFunctionHeader})) {
      return ref.Empty() ? SymbolType::kClass : SymbolType::kFunction;
    }
    if (Context().DirectParentsAre({NodeEnum::kUnqualifiedId,
                                    NodeEnum::kQualifiedId,  // out-of-line
                                    NodeEnum::kTaskHeader})) {
      return ref.Empty() ? SymbolType::kClass : SymbolType::kTask;
    }
    // TODO: import references bases must be resolved as SymbolType::kPackage.
    if (Context().DirectParentIs(NodeEnum::kActualNamedPort)) {
      return SymbolType::kDataNetVariableInstance;
    }
    if (Context().DirectParentIs(NodeEnum::kParamByName)) {
      return SymbolType::kParameter;
    }
    if (Context().DirectParentsAre({NodeEnum::kUnqualifiedId,
                                    NodeEnum::kLocalRoot,
                                    NodeEnum::kFunctionCall})) {
      // bare call like "function_name(...)"
      return SymbolType::kCallable;
    }
    if (Context().DirectParentsAre(
            {NodeEnum::kUnqualifiedId, NodeEnum::kQualifiedId,
             NodeEnum::kLocalRoot, NodeEnum::kFunctionCall})) {
      // qualified call like "pkg_or_class::function_name(...)"
      // Only the last component needs to be callable.
      const SyntaxTreeNode* qualified_id =
          Context().NearestParentWithTag(NodeEnum::kQualifiedId);
      const SyntaxTreeNode* unqualified_id =
          Context().NearestParentWithTag(NodeEnum::kUnqualifiedId);
      if (qualified_id->children().back().get() == unqualified_id) {
        return SymbolType::kCallable;
      }
      // TODO(fangism): could require parents to be kPackage or kClass
    }
    if (Context().DirectParentsAre(
            {NodeEnum::kUnqualifiedId, NodeEnum::kMethodCallExtension})) {
      // method call like "obj.method_name(...)"
      return SymbolType::kCallable;
      // TODO(fangism): check that method is non-static
    }
    // Default: no specific metatype.
    return SymbolType::kUnspecified;
  }

  // Creates a named element in the current scope.
  // Suitable for SystemVerilog language elements: functions, tasks, packages,
  // classes, modules, etc...
  SymbolTableNode& EmplaceElementInCurrentScope(const verible::Symbol& element,
                                                absl::string_view name,
                                                SymbolType type) {
    const auto p =
        current_scope_->TryEmplace(name, SymbolInfo{
                                             .type = type,
                                             .file_origin = source_,
                                             .syntax_origin = &element,
                                         });
    if (!p.second) {
      DiagnoseSymbolAlreadyExists(name);
    }
    return p.first->second;  // scope of the new (or pre-existing symbol)
  }

  // Creates a named typed element in the current scope.
  // Suitable for SystemVerilog language elements: nets, parameter, variables,
  // instances, functions (using their return types).
  SymbolTableNode& EmplaceTypedElementInCurrentScope(
      const verible::Symbol& element, absl::string_view name, SymbolType type) {
    VLOG(1) << __FUNCTION__ << ": " << name << " in " << CurrentScopeFullPath();
    VLOG(1) << "  type info: " << *ABSL_DIE_IF_NULL(declaration_type_info_);
    VLOG(1) << "  full text: " << AutoTruncate{StringSpanOfSymbol(element), 40};
    const auto p = current_scope_->TryEmplace(
        name,
        SymbolInfo{
            .type = type,
            .file_origin = source_,
            .syntax_origin = &element,
            // associate this instance with its declared type
            .declared_type = *ABSL_DIE_IF_NULL(declaration_type_info_),  // copy
        });
    if (!p.second) {
      DiagnoseSymbolAlreadyExists(name);
    }
    VLOG(1) << "end of " << __FUNCTION__ << ": " << name;
    return p.first->second;  // scope of the new (or pre-existing symbol)
  }

  // Creates a named element in the current scope, and traverses its subtree
  // inside the new element's scope.
  void DeclareScopedElementAndDescend(const SyntaxTreeNode& element,
                                      absl::string_view name, SymbolType type) {
    SymbolTableNode& enter_scope(
        EmplaceElementInCurrentScope(element, name, type));
    Descend(element, enter_scope);
  }

  void DeclareModule(const SyntaxTreeNode& module) {
    DeclareScopedElementAndDescend(module, GetModuleName(module).get().text(),
                                   SymbolType::kModule);
  }

  absl::string_view GetScopeNameFromGenerateBody(const SyntaxTreeNode& body) {
    if (body.MatchesTag(NodeEnum::kGenerateBlock)) {
      const TokenInfo* label =
          GetBeginLabelTokenInfo(GetGenerateBlockBegin(body));
      if (label != nullptr) {
        // TODO: Check for a matching end-label here, and if its name matches
        // the begin label, then immediately create a resolved reference because
        // it only makes sense for it resolve to this begin.
        // Otherwise, do nothing with the end label.
        return label->text();
      }
    }
    return current_scope_->Value().CreateAnonymousScope("generate");
  }

  void DeclareGenerateIf(const SyntaxTreeNode& generate_if) {
    const SyntaxTreeNode& body(GetIfClauseGenerateBody(generate_if));

    DeclareScopedElementAndDescend(
        generate_if, GetScopeNameFromGenerateBody(body), SymbolType::kGenerate);
  }

  void DeclareGenerateElse(const SyntaxTreeNode& generate_else) {
    const SyntaxTreeNode& body(GetElseClauseGenerateBody(generate_else));

    if (body.MatchesTag(NodeEnum::kConditionalGenerateConstruct)) {
      // else-if chained.  Flatten the else block by not creating a new scope
      // and let the if-clause inside create a scope directly under the current
      // scope.
      Descend(body);
    } else {
      DeclareScopedElementAndDescend(generate_else,
                                     GetScopeNameFromGenerateBody(body),
                                     SymbolType::kGenerate);
    }
  }

  void DeclarePackage(const SyntaxTreeNode& package) {
    DeclareScopedElementAndDescend(package, GetPackageNameToken(package).text(),
                                   SymbolType::kPackage);
  }

  void DeclareClass(const SyntaxTreeNode& class_node) {
    DeclareScopedElementAndDescend(
        class_node, GetClassName(class_node).get().text(), SymbolType::kClass);
  }

  void DeclareTask(const SyntaxTreeNode& task_node) {
    const ValueSaver<SymbolTableNode*> reserve_for_task_decl(
        &current_scope_);  // no scope change yet
    Descend(task_node);
  }

  void DeclareFunction(const SyntaxTreeNode& function_node) {
    // Reserve a slot for the function's scope on the stack, but do not set it
    // until we add it in HandleIdentifier().  This deferral allows us to
    // evaluate the return type of the declared function as a reference in the
    // current context.
    const ValueSaver<SymbolTableNode*> reserve_for_function_decl(
        &current_scope_);  // no scope change yet
    Descend(function_node);
  }

  void DeclarePorts(const SyntaxTreeNode& port_list) {
    // For out-of-line function declarations, do not re-declare ports that
    // already came from the method prototype.
    // We designate the prototype as the source-of-truth because in Verilog,
    // port *names* are part of the public interface (allowing calling with
    // named parameter assignments, unlike C++ function calls).
    // LRM 8.24: "The out-of-block method declaration shall match the prototype
    // declaration exactly, with the following exceptions..."
    {
      const SyntaxTreeNode* function_header =
          Context().NearestParentMatching([](const SyntaxTreeNode& node) {
            return node.MatchesTag(NodeEnum::kFunctionHeader);
          });
      if (function_header != nullptr) {
        const SyntaxTreeNode& id = verible::SymbolCastToNode(
            *ABSL_DIE_IF_NULL(GetFunctionHeaderId(*function_header)));
        if (id.MatchesTag(NodeEnum::kQualifiedId)) {
          // For now, ignore the out-of-line port declarations.
          // TODO: Diagnose port type/name mismatches between prototypes' and
          // out-of-line headers' ports.
          return;
        }
      }
    }
    {
      const SyntaxTreeNode* task_header =
          Context().NearestParentMatching([](const SyntaxTreeNode& node) {
            return node.MatchesTag(NodeEnum::kTaskHeader);
          });
      if (task_header != nullptr) {
        const SyntaxTreeNode& id = verible::SymbolCastToNode(
            *ABSL_DIE_IF_NULL(GetTaskHeaderId(*task_header)));
        if (id.MatchesTag(NodeEnum::kQualifiedId)) {
          // For now, ignore the out-of-line port declarations.
          // TODO: Diagnose port type/name mismatches between prototypes' and
          // out-of-line headers' ports.
          return;
        }
      }
    }
    // In all other cases, declare ports normally at the declaration site.
    Descend(port_list);
  }

  // Capture the declared function's return type.
  void SetupFunctionHeader(const SyntaxTreeNode& function_header) {
    DeclarationTypeInfo decl_type_info;
    const ValueSaver<DeclarationTypeInfo*> function_return_type(
        &declaration_type_info_, &decl_type_info);
    Descend(function_header);
    // decl_type_info will be safely copied away in HandleIdentifier().
  }

  // TODO: functions and tasks, which could appear as out-of-line definitions.

  void DeclareParameter(const SyntaxTreeNode& param_decl_node) {
    CHECK(param_decl_node.MatchesTag(NodeEnum::kParamDeclaration));
    DeclarationTypeInfo decl_type_info;
    // Set declaration_type_info_ to capture any user-defined type used to
    // declare data/variables/instances.
    const ValueSaver<DeclarationTypeInfo*> save_type(&declaration_type_info_,
                                                     &decl_type_info);
    Descend(param_decl_node);
  }

  // Declares one or more variables/instances/nets.
  void DeclareData(const SyntaxTreeNode& data_decl_node) {
    VLOG(1) << __FUNCTION__;
    DeclarationTypeInfo decl_type_info;
    // Set declaration_type_info_ to capture any user-defined type used to
    // declare data/variables/instances.
    const ValueSaver<DeclarationTypeInfo*> save_type(&declaration_type_info_,
                                                     &decl_type_info);
    Descend(data_decl_node);
    VLOG(1) << "end of " << __FUNCTION__;
  }

  // Declare one (of potentially multiple) instances in a single declaration
  // statement.
  void DeclareInstance(const SyntaxTreeNode& instance) {
    const absl::string_view instance_name(
        GetModuleInstanceNameTokenInfoFromGateInstance(instance).text());
    const SymbolTableNode& new_instance(EmplaceTypedElementInCurrentScope(
        instance, instance_name, SymbolType::kDataNetVariableInstance));

    // Also create a DependentReferences chain starting with this named instance
    // so that named port references are direct children of this reference root.
    // This is a self-reference.
    const CaptureDependentReference capture(this);
    capture.Ref().PushReferenceComponent(ReferenceComponent{
        .identifier = instance_name,
        .ref_type = ReferenceType::kUnqualified,
        .metatype = SymbolType::kDataNetVariableInstance,
        // Start with its type already resolved to the node we just declared.
        .resolved_symbol = &new_instance,
    });

    // Inform that named port identifiers will yield parallel children from
    // this reference branch point.
    const ValueSaver<ReferenceComponentNode*> set_branch(
        &reference_branch_point_, capture.Ref().components.get());

    // No change of scope, but named ports will be resolved with respect to the
    // decl_type_info's scope later.
    Descend(instance);  // visit parameter/port connections, etc.
  }

  void DeclareNet(const SyntaxTreeNode& net_variable) {
    const absl::string_view net_name(
        GetNameLeafOfNetVariable(net_variable).get().text());
    EmplaceTypedElementInCurrentScope(net_variable, net_name,
                                      SymbolType::kDataNetVariableInstance);
    Descend(net_variable);
  }

  void DeclareRegister(const SyntaxTreeNode& reg_variable) {
    const absl::string_view net_name(
        GetNameLeafOfRegisterVariable(reg_variable).get().text());
    EmplaceTypedElementInCurrentScope(reg_variable, net_name,
                                      SymbolType::kDataNetVariableInstance);
    Descend(reg_variable);
  }

  void DiagnoseSymbolAlreadyExists(absl::string_view name) {
    diagnostics_.push_back(absl::AlreadyExistsError(
        absl::StrCat("Symbol \"", name, "\" is already defined in the ",
                     CurrentScopeFullPath(), " scope.")));
  }

  absl::StatusOr<SymbolTableNode*> LookupOrInjectOutOfLineDefinition(
      const SyntaxTreeNode& qualified_id, SymbolType type,
      const SyntaxTreeNode* definition_syntax) {
    // e.g. "function int class_c::func(...); ... endfunction"
    // Use a DependentReference object to establish a self-reference.
    CaptureDependentReference capture(this);
    Descend(qualified_id);

    DependentReferences& ref(capture.Ref());
    // Expecting only two-level reference "outer::inner".
    CHECK_EQ(ABSL_DIE_IF_NULL(ref.components)->Children().size(), 1);

    // Must resolve base, instead of deferring to resolve phase.
    // Do not inject the outer_scope (class name) into the current scope.
    // Reject injections into non-classes.
    const auto outer_scope_or_status =
        ref.ResolveOnlyBaseLocally(current_scope_);
    if (!outer_scope_or_status.ok()) {
      return outer_scope_or_status.status();
    }
    SymbolTableNode* outer_scope = ABSL_DIE_IF_NULL(*outer_scope_or_status);

    // Lookup inner symbol in outer_scope, but also allow injection of the
    // inner symbol name into the outer_scope (with diagnostic).
    ReferenceComponent& inner_ref = ref.components->Children().front().Value();
    const absl::string_view inner_key = inner_ref.identifier;

    const auto p = outer_scope->TryEmplace(
        inner_key, SymbolInfo{
                       .type = type,
                       .file_origin = source_,
                       .syntax_origin = definition_syntax,
                   });
    SymbolTableNode* inner_symbol = &p.first->second;
    if (p.second) {
      // If injection succeeded, then the outer_scope did not already contain a
      // forward declaration of the inner symbol to be defined.
      // Diagnose this non-fatally, but continue.
      diagnostics_.push_back(
          DiagnoseMemberSymbolResolutionFailure(inner_key, *outer_scope));
    } else {
      // Use pre-existing symbol table entry created from the prototype.
      // Check that out-of-line and prototype symbol metatypes match.
      const SymbolType original_type = inner_symbol->Value().type;
      if (original_type != type) {
        return absl::AlreadyExistsError(
            absl::StrCat(SymbolTypeAsString(original_type), " ",
                         ContextFullPath(*inner_symbol),
                         " cannot be redefined out-of-line as a ",
                         SymbolTypeAsString(type)));
      }
    }
    // Resolve this self-reference immediately.
    inner_ref.resolved_symbol = inner_symbol;
    return inner_symbol;  // mutable for purpose of constructing definition
  }

  void DescendThroughOutOfLineDefinition(const SyntaxTreeNode& qualified_id,
                                         SymbolType type,
                                         const SyntaxTreeNode* decl_syntax) {
    const auto inner_symbol_or_status =
        LookupOrInjectOutOfLineDefinition(qualified_id, type, decl_syntax);
    // Change the current scope (which was set up on the stack by
    // kFunctionDeclaration or kTaskDeclaration) for the rest of the
    // definition.
    if (inner_symbol_or_status.ok()) {
      current_scope_ = *inner_symbol_or_status;
      Descend(qualified_id);
    } else {
      // On failure, skip the entire definition because there is no place
      // to add its local symbols.
      diagnostics_.push_back(inner_symbol_or_status.status());
    }
  }

  void HandleQualifiedId(const SyntaxTreeNode& qualified_id) {
    switch (static_cast<NodeEnum>(Context().top().Tag().tag)) {
      case NodeEnum::kFunctionHeader: {
        const SyntaxTreeNode* decl_syntax =
            Context().NearestParentMatching([](const SyntaxTreeNode& node) {
              return node.MatchesTagAnyOf({NodeEnum::kFunctionDeclaration,
                                           NodeEnum::kFunctionPrototype});
            });
        DescendThroughOutOfLineDefinition(qualified_id, SymbolType::kFunction,
                                          ABSL_DIE_IF_NULL(decl_syntax));
        break;
      }
      case NodeEnum::kTaskHeader: {
        const SyntaxTreeNode* decl_syntax =
            Context().NearestParentMatching([](const SyntaxTreeNode& node) {
              return node.MatchesTagAnyOf(
                  {NodeEnum::kTaskDeclaration, NodeEnum::kTaskPrototype});
            });
        DescendThroughOutOfLineDefinition(qualified_id, SymbolType::kTask,
                                          ABSL_DIE_IF_NULL(decl_syntax));
        break;
      }
      default:
        // Treat this as a reference, not an out-of-line definition.
        Descend(qualified_id);
        break;
    }
  }

  void EnterIncludeFile(const SyntaxTreeNode& preprocessor_include) {
    const SyntaxTreeLeaf* included_filename =
        GetFileFromPreprocessorInclude(preprocessor_include);
    if (included_filename == nullptr) return;

    const absl::string_view filename_text = included_filename->get().text();

    // Remove the double quotes from the filename.
    const absl::string_view filename_unquoted = StripOuterQuotes(filename_text);
    VLOG(1) << "got: `include \"" << filename_unquoted << "\"";

    // Opening included file requires a VerilogProject.
    // Open this file (could be first time, or previously opened).
    VerilogProject* project = symbol_table_->project_;
    if (project == nullptr) return;  // Without project, ignore.

    const auto status_or_file = project->OpenIncludedFile(filename_unquoted);
    if (!status_or_file.ok()) {
      diagnostics_.push_back(status_or_file.status());
      // Errors can be retrieved later.
      return;
    }

    VerilogSourceFile* const included_file = *status_or_file;
    if (included_file == nullptr) return;
    VLOG(1) << "opened include file: " << included_file->ResolvedPath();

    const auto parse_status = included_file->Parse();
    if (!parse_status.ok()) {
      diagnostics_.push_back(parse_status);
      // For now, don't bother attempting to parse a partial syntax tree.
      // This would be best handled in the future with actual preprocessing.
      return;
    }

    // Depending on application, one may wish to avoid re-processing the same
    // included file.  If desired, add logic to return early here.

    {  // Traverse included file's syntax tree.
      const ValueSaver<const VerilogSourceFile*> includer(&source_,
                                                          included_file);
      const ValueSaver<TokenInfo::Context> save_context_text(
          &token_context_, MakeTokenContext());
      included_file->GetTextStructure()->SyntaxTree()->Accept(this);
    }
  }

  std::string CurrentScopeFullPath() const {
    return ContextFullPath(*current_scope_);
  }

  verible::TokenWithContext VerboseToken(const TokenInfo& token) const {
    return verible::TokenWithContext{token, token_context_};
  }

  TokenInfo::Context MakeTokenContext() const {
    return TokenInfo::Context(
        source_->GetTextStructure()->Contents(),
        [](std::ostream& stream, int e) { stream << verilog_symbol_name(e); });
  }

 private:  // data
  // Points to the source file that is the origin of symbols.
  // This changes when opening preprocess-included files.
  // TODO(fangism): maintain a vector/stack of these for richer diagnostics
  const VerilogSourceFile* source_;

  // For human-readable debugging.
  // This should be constructed using MakeTokenContext(), after setting
  // 'source_'.
  TokenInfo::Context token_context_;

  // The symbol table to build, never nullptr.
  SymbolTable* const symbol_table_;

  // The remaining fields are mutable state:

  // This is the current scope where encountered definitions register their
  // symbols, never nullptr.
  // There is no need to maintain a stack because SymbolTableNodes already link
  // to their parents.
  SymbolTableNode* current_scope_;

  // Stack of references.
  // A stack is needed to support nested type references like "A#(B(#(C)))",
  // and nested expressions like "f(g(h))"
  std::stack<DependentReferences> reference_builders_;

  // When creating branched references, like with instances' named ports,
  // set this to the nearest branch point.
  // This will signal to the reference builder that parallel children
  // are to be added, as opposed to deeper descendants.
  ReferenceComponentNode* reference_branch_point_ = nullptr;

  // For a data/instance/variable declaration statement, this is the declared
  // type (could be primitive or named-user-defined).
  // Set this type before traversing declared instances and variables to capture
  // the type of the declaration.  Unset this to prevent type capture.
  // Such declarations cannot nest, so a stack is not needed.
  DeclarationTypeInfo* declaration_type_info_ = nullptr;

  // Update to either "::" or '.'.
  const TokenInfo* last_hierarchy_operator_ = nullptr;

  // Collection of findings that might be considered compiler/tool errors in a
  // real toolchain.  For example: attempt to redefine symbol.
  std::vector<absl::Status> diagnostics_;
};

void ReferenceComponent::VerifySymbolTableRoot(
    const SymbolTableNode* root) const {
  if (resolved_symbol != nullptr) {
    CHECK_EQ(resolved_symbol->Root(), root)
        << "Resolved symbols must point to a node in the same SymbolTable.";
  }
}

absl::Status ReferenceComponent::MatchesMetatype(
    SymbolType found_metatype) const {
  switch (metatype) {
    case SymbolType::kUnspecified:
      return absl::OkStatus();
    case SymbolType::kCallable:
      if (found_metatype == SymbolType::kFunction ||
          found_metatype == SymbolType::kTask) {
        return absl::OkStatus();
      }
      break;
    default:
      if (metatype == found_metatype) return absl::OkStatus();
      break;
  }
  // Otherwise, mismatched metatype.
  return absl::InvalidArgumentError(
      absl::StrCat("Expecting reference \"", identifier, "\" to resolve to a ",
                   SymbolTypeAsString(metatype), ", but found a ",
                   SymbolTypeAsString(found_metatype), "."));
}

const ReferenceComponentNode* DependentReferences::LastLeaf() const {
  if (components == nullptr) return nullptr;
  const ReferenceComponentNode* node = components.get();
  while (!node->is_leaf()) node = &node->Children().front();
  return node;
}

void DependentReferences::PushReferenceComponent(
    const ReferenceComponent& component) {
  VLOG(3) << __FUNCTION__ << ", id: " << component.identifier;
  if (Empty()) {
    components = absl::make_unique<ReferenceComponentNode>(component);  // copy
  } else {
    // Find the deepest leaf node, and grow a new child from that.
    ReferenceComponentNode* node = components.get();
    while (!node->is_leaf()) node = &node->Children().front();
    // This is a leaf node, and this is the first child.
    CheckedNewChildReferenceNode(node, component);
  }
  VLOG(3) << "end of " << __FUNCTION__;
}

void DependentReferences::VerifySymbolTableRoot(
    const SymbolTableNode* root) const {
  if (components != nullptr) {
    components->ApplyPreOrder([=](const ReferenceComponent& component) {
      component.VerifySymbolTableRoot(root);
    });
  }
}

std::ostream& operator<<(std::ostream& stream,
                         const DependentReferences& dep_refs) {
  if (dep_refs.components == nullptr) return stream << "(empty-ref)";
  dep_refs.components->PrintTree(
      &stream,
      [](std::ostream& s, const ReferenceComponent& ref_comp) -> std::ostream& {
        return s << ref_comp;
      });
  return stream;
}

// Search up-scope, stopping at the first symbol found in the nearest scope.
static const SymbolTableNode* LookupSymbolUpwards(
    const SymbolTableNode& context, absl::string_view symbol) {
  const SymbolTableNode* current_context = &context;
  while (current_context != nullptr) {
    // TODO: lookup imported namespaces and symbols
    const auto found = current_context->Find(symbol);
    if (found != current_context->end()) return &found->second;
    current_context = current_context->Parent();
  }
  return nullptr;  // resolution failed
}

static absl::Status DiagnoseUnqualifiedSymbolResolutionFailure(
    absl::string_view name, const SymbolTableNode& context) {
  return absl::NotFoundError(absl::StrCat("Unable to resolve symbol \"", name,
                                          "\" from context ",
                                          ContextFullPath(context), "."));
}

static void ResolveReferenceComponentNodeLocal(ReferenceComponentNode& node,
                                               const SymbolTableNode& context) {
  ReferenceComponent& component(node.Value());
  VLOG(2) << __FUNCTION__ << ": " << component;
  // If already resolved, skip.
  if (component.resolved_symbol != nullptr) return;  // already bound
  const absl::string_view key(component.identifier);
  CHECK(node.Parent() == nullptr);  // is root
  // root node: lookup this symbol from its context upward
  CHECK_EQ(component.ref_type, ReferenceType::kUnqualified);

  // Only try to resolve using the same scope in which the reference appeared,
  // local, without upward search.
  const auto found = context.Find(key);
  if (found != context.end()) {
    component.resolved_symbol = &found->second;
  }
}

static void ResolveUnqualifiedName(ReferenceComponent& component,
                                   const SymbolTableNode& context,
                                   std::vector<absl::Status>* diagnostics) {
  VLOG(2) << __FUNCTION__ << ": " << component;
  const absl::string_view key(component.identifier);
  // Find the first symbol whose name matches, without regard to its metatype.
  const SymbolTableNode* resolved = LookupSymbolUpwards(context, key);
  if (resolved == nullptr) {
    diagnostics->emplace_back(
        DiagnoseUnqualifiedSymbolResolutionFailure(key, context));
    return;
  }

  // Verify metatype match.
  const auto metatype_match_status =
      component.MatchesMetatype(resolved->Value().type);
  if (metatype_match_status.ok()) {
    component.resolved_symbol = resolved;
  } else {
    diagnostics->push_back(metatype_match_status);
  }
  VLOG(2) << "end of " << __FUNCTION__;
}

static void ResolveDirectMember(ReferenceComponent& component,
                                const SymbolTableNode& context,
                                std::vector<absl::Status>* diagnostics) {
  VLOG(2) << __FUNCTION__ << ": " << component;
  const absl::string_view key(component.identifier);
  const auto found = context.Find(key);
  // TODO: lookup members through inherited scopes
  if (found == context.end()) {
    diagnostics->emplace_back(
        DiagnoseMemberSymbolResolutionFailure(key, context));
    return;
  }

  const SymbolTableNode& found_symbol = found->second;
  const auto metatype_match_status =
      component.MatchesMetatype(found_symbol.Value().type);
  if (metatype_match_status.ok()) {
    component.resolved_symbol = &found_symbol;
  } else {
    VLOG(2) << metatype_match_status.message();
    diagnostics->push_back(metatype_match_status);
  }
  VLOG(2) << "end of " << __FUNCTION__;
}

// This is the primary function that resolves references.
// Dependent (parent) nodes must already be resolved before attempting to
// resolve children references (guaranteed by calling this in a pre-order
// traversal).
static void ResolveReferenceComponentNode(
    ReferenceComponentNode& node, const SymbolTableNode& context,
    std::vector<absl::Status>* diagnostics) {
  ReferenceComponent& component(node.Value());
  VLOG(2) << __FUNCTION__ << ": " << component;
  if (component.resolved_symbol != nullptr) return;  // already bound

  switch (component.ref_type) {
    case ReferenceType::kUnqualified: {
      // root node: lookup this symbol from its context upward
      CHECK(node.Parent() == nullptr);
      ResolveUnqualifiedName(component, context, diagnostics);
      break;
    }
    case ReferenceType::kImmediate: {
      ResolveDirectMember(component, context, diagnostics);
      break;
    }
    case ReferenceType::kDirectMember: {
      // Use parent's scope (if resolved successfully) to resolve this node.
      const ReferenceComponent& parent_component(node.Parent()->Value());

      const SymbolTableNode* parent_scope = parent_component.resolved_symbol;
      if (parent_scope == nullptr) return;  // leave this subtree unresolved

      ResolveDirectMember(component, *parent_scope, diagnostics);
      break;
    }
    case ReferenceType::kMemberOfTypeOfParent: {
      // Use parent's type's scope (if resolved successfully) to resolve this
      // node. Get the type of the object from the parent component.
      const ReferenceComponent& parent_component(node.Parent()->Value());
      const SymbolTableNode* parent_scope = parent_component.resolved_symbol;
      if (parent_scope == nullptr) return;  // leave this subtree unresolved

      const DeclarationTypeInfo& type_info =
          parent_scope->Value().declared_type;
      // Primitive types do not have members.
      if (type_info.user_defined_type == nullptr) {
        diagnostics->push_back(absl::InvalidArgumentError(
            absl::StrCat("Type of parent reference ",
                         ReferenceNodeFullPathString(*node.Parent()), " (",
                         verible::StringSpanOfSymbol(*type_info.syntax_origin),
                         ") does not have any members.")));
        return;
      }

      // This referenced object's scope is not a parent of this node, and
      // thus, not guaranteed to have been resolved first.
      // TODO(fangism): resolve on-demand
      const SymbolTableNode* type_scope =
          type_info.user_defined_type->Value().resolved_symbol;
      if (type_scope == nullptr) return;

      ResolveDirectMember(component, *type_scope, diagnostics);
      break;
    }
  }
  VLOG(2) << "end of " << __FUNCTION__;
}

ReferenceComponentMap ReferenceComponentNodeMapView(
    const ReferenceComponentNode& node) {
  ReferenceComponentMap map_view;
  for (const auto& child : node.Children()) {
    map_view.emplace(std::make_pair(child.Value().identifier, &child));
  }
  return map_view;
}

void DependentReferences::Resolve(const SymbolTableNode& context,
                                  std::vector<absl::Status>* diagnostics) {
  VLOG(1) << __FUNCTION__;
  if (components == nullptr) return;
  // References are arranged in dependency trees.
  // Parent node references must be resolved before children nodes,
  // hence a pre-order traversal.
  components->ApplyPreOrder(
      [&context, diagnostics](ReferenceComponentNode& node) {
        ResolveReferenceComponentNode(node, context, diagnostics);
        // TODO: minor optimization, when resolution for a node fails,
        // skip checking that node's subtree; early terminate.
      });
  VLOG(1) << "end of " << __FUNCTION__;
}

void DependentReferences::ResolveLocally(const SymbolTableNode& context) {
  if (components == nullptr) return;
  // Only attempt to resolve the reference root, and none of its subtrees.
  ResolveReferenceComponentNodeLocal(*components, context);
}

absl::StatusOr<SymbolTableNode*> DependentReferences::ResolveOnlyBaseLocally(
    SymbolTableNode* context) {
  // Similar lookup to ResolveReferenceComponentNodeLocal() but allows
  // mutability of 'context' for injecting out-of-line definitions.

  ReferenceComponent& base(ABSL_DIE_IF_NULL(components)->Value());
  CHECK(base.ref_type == ReferenceType::kUnqualified ||
        base.ref_type == ReferenceType::kImmediate)
      << "Inconsistent reference type: " << base.ref_type;
  const absl::string_view key(base.identifier);
  const auto found = context->Find(key);
  if (found == context->end()) {
    return DiagnoseMemberSymbolResolutionFailure(key, *context);
  }
  SymbolTableNode& resolved = found->second;

  // If metatype doesn't match what is expected, then fail.
  const auto metatype_match_status =
      base.MatchesMetatype(resolved.Value().type);
  if (!metatype_match_status.ok()) {
    return metatype_match_status;
  }

  base.resolved_symbol = &resolved;
  return &resolved;
}

std::ostream& operator<<(std::ostream& stream, ReferenceType ref_type) {
  static const verible::EnumNameMap<ReferenceType> kReferenceTypeNames({
      // short-hand annotation for identifier reference type
      {"@", ReferenceType::kUnqualified},
      {"!", ReferenceType::kImmediate},
      {"::", ReferenceType::kDirectMember},
      {".", ReferenceType::kMemberOfTypeOfParent},
  });
  return kReferenceTypeNames.Unparse(ref_type, stream);
}

std::ostream& ReferenceComponent::PrintPathComponent(
    std::ostream& stream) const {
  stream << ref_type << identifier;
  if (metatype != SymbolType::kUnspecified) {
    stream << '[' << metatype << ']';
  }
  return stream;
}

std::ostream& ReferenceComponent::PrintVerbose(std::ostream& stream) const {
  PrintPathComponent(stream) << " -> ";
  if (resolved_symbol == nullptr) {
    return stream << "<unresolved>";
  }
  return stream << ContextFullPath(*resolved_symbol);
}

std::ostream& operator<<(std::ostream& stream,
                         const ReferenceComponent& component) {
  return component.PrintVerbose(stream);
}

void DeclarationTypeInfo::VerifySymbolTableRoot(
    const SymbolTableNode* root) const {
  if (user_defined_type != nullptr) {
    user_defined_type->ApplyPreOrder([=](const ReferenceComponent& component) {
      component.VerifySymbolTableRoot(root);
    });
  }
}

absl::string_view SymbolInfo::CreateAnonymousScope(absl::string_view base) {
  const size_t n = anonymous_scope_names.size();
  anonymous_scope_names.emplace_back(absl::make_unique<const std::string>(
      // Starting with a non-alpha character guarantees it cannot collide with
      // any user-given identifier.
      absl::StrCat("%", "anon-", base, "-", n)));
  return *anonymous_scope_names.back();
}

std::ostream& operator<<(std::ostream& stream,
                         const DeclarationTypeInfo& decl_type_info) {
  stream << "type-info { ";

  stream << "source: ";
  if (decl_type_info.syntax_origin != nullptr) {
    stream << "\""
           << AutoTruncate{.text = StringSpanOfSymbol(
                               *decl_type_info.syntax_origin),
                           .max_chars = 25}
           << "\"";
  } else {
    stream << "(unknown)";
  }

  stream << ", type ref: ";
  if (decl_type_info.user_defined_type != nullptr) {
    stream << *decl_type_info.user_defined_type;
  } else {
    stream << "(primitive)";
  }

  return stream << " }";
}

void SymbolInfo::VerifySymbolTableRoot(const SymbolTableNode* root) const {
  declared_type.VerifySymbolTableRoot(root);
  for (const auto& local_ref : local_references_to_bind) {
    local_ref.VerifySymbolTableRoot(root);
  }
}

void SymbolInfo::Resolve(const SymbolTableNode& context,
                         std::vector<absl::Status>* diagnostics) {
  for (auto& local_ref : local_references_to_bind) {
    local_ref.Resolve(context, diagnostics);
  }
}

void SymbolInfo::ResolveLocally(const SymbolTableNode& context) {
  for (auto& local_ref : local_references_to_bind) {
    local_ref.ResolveLocally(context);
  }
}

std::ostream& SymbolInfo::PrintDefinition(std::ostream& stream,
                                          size_t indent) const {
  // print everything except local_references_to_bind
  const verible::Spacer wrap(indent);
  stream << wrap << "metatype: " << type << std::endl;
  if (file_origin != nullptr) {
    stream << wrap << "file: " << file_origin->ResolvedPath() << std::endl;
  }
  // declared_type only makes sense for elements with potentially user-defined
  // types, and not for language element declarations like modules and classes.
  if (type == SymbolType::kDataNetVariableInstance) {
    stream << wrap << declared_type << std::endl;
  }
  return stream;
}

std::ostream& SymbolInfo::PrintReferences(std::ostream& stream,
                                          size_t indent) const {
  // only print local_references_to_bind
  // TODO: support indentation
  std::string newline_wrap(indent + 1, ' ');
  newline_wrap.front() = '\n';
  stream << "refs:";
  // When there's at most 1 reference, print more compactly.
  if (local_references_to_bind.size() > 1)
    stream << newline_wrap;
  else
    stream << ' ';
  stream << absl::StrJoin(local_references_to_bind, newline_wrap,
                          absl::StreamFormatter());
  if (local_references_to_bind.size() > 1) stream << newline_wrap;
  return stream;
}

SymbolInfo::references_map_view_type
SymbolInfo::LocalReferencesMapViewForTesting() const {
  references_map_view_type map_view;
  for (const auto& local_ref : local_references_to_bind) {
    CHECK(!local_ref.Empty()) << "Never add empty DependentReferences.";
    map_view[local_ref.components->Value().identifier].emplace(&local_ref);
  }
  return map_view;
}

void SymbolTable::CheckIntegrity() const {
  const SymbolTableNode* root = &symbol_table_root_;
  symbol_table_root_.ApplyPreOrder(
      [=](const SymbolInfo& s) { s.VerifySymbolTableRoot(root); });
}

void SymbolTable::Resolve(std::vector<absl::Status>* diagnostics) {
  symbol_table_root_.ApplyPreOrder(
      [=](SymbolTableNode& node) { node.Value().Resolve(node, diagnostics); });
}

void SymbolTable::ResolveLocallyOnly() {
  symbol_table_root_.ApplyPreOrder(
      [=](SymbolTableNode& node) { node.Value().ResolveLocally(node); });
}

std::ostream& SymbolTable::PrintSymbolDefinitions(std::ostream& stream) const {
  return symbol_table_root_.PrintTree(
      stream,
      [](std::ostream& s, const SymbolInfo& sym,
         size_t indent) -> std::ostream& {
        return sym.PrintDefinition(s << std::endl, indent + 4 /* wrap */)
               << verible::Spacer(indent);
      });
}

std::ostream& SymbolTable::PrintSymbolReferences(std::ostream& stream) const {
  return symbol_table_root_.PrintTree(stream,
                                      [](std::ostream& s, const SymbolInfo& sym,
                                         size_t indent) -> std::ostream& {
                                        return sym.PrintReferences(
                                            s, indent + 4 /* wrap */);
                                      });
}

static void ParseFileAndBuildSymbolTable(
    VerilogSourceFile* source, SymbolTable* symbol_table,
    VerilogProject* project, std::vector<absl::Status>* diagnostics) {
  const auto parse_status = source->Parse();
  if (!parse_status.ok()) diagnostics->push_back(parse_status);
  // Continue, in case syntax-error recovery left a partial syntax tree.

  // Amend symbol table by analyzing this translation unit.
  const std::vector<absl::Status> statuses =
      BuildSymbolTable(*source, symbol_table, project);
  // Forward diagnostics.
  diagnostics->insert(diagnostics->end(), statuses.begin(), statuses.end());
}

void SymbolTable::Build(std::vector<absl::Status>* diagnostics) {
  for (auto& translation_unit : *project_) {
    ParseFileAndBuildSymbolTable(&translation_unit.second, this, project_,
                                 diagnostics);
  }
}

void SymbolTable::BuildSingleTranslationUnit(
    absl::string_view referenced_file_name,
    std::vector<absl::Status>* diagnostics) {
  const auto translation_unit_or_status =
      project_->OpenTranslationUnit(referenced_file_name);
  if (!translation_unit_or_status.ok()) {
    diagnostics->push_back(translation_unit_or_status.status());
    return;
  }
  VerilogSourceFile* translation_unit = *translation_unit_or_status;

  ParseFileAndBuildSymbolTable(translation_unit, this, project_, diagnostics);
}

std::vector<absl::Status> BuildSymbolTable(const VerilogSourceFile& source,
                                           SymbolTable* symbol_table,
                                           VerilogProject* project) {
  VLOG(1) << __FUNCTION__;
  const auto* text_structure = source.GetTextStructure();
  if (text_structure == nullptr) return std::vector<absl::Status>();
  const auto& syntax_tree = text_structure->SyntaxTree();
  if (syntax_tree == nullptr) return std::vector<absl::Status>();

  SymbolTable::Builder builder(source, symbol_table, project);
  syntax_tree->Accept(&builder);
  return builder.TakeDiagnostics();  // move
}

}  // namespace verilog
