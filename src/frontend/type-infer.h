/*
 * Copyright 2014 Google Inc. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef _AUTOPIPER_FRONTEND_TYPE_INFER_H_
#define _AUTOPIPER_FRONTEND_TYPE_INFER_H_

#include "frontend/ast.h"
#include "frontend/visitor.h"
#include "frontend/agg-types.h"

#include <vector>
#include <map>
#include <set>
#include <string>
#include <functional>

namespace autopiper {
namespace frontend {

// Type inference DAG: a single node in the type-graph.  Build a DAG where
// nodes hold sets of InferredType pointers to types that must be equivalent,
// and edges with transfer functions (going one way or both ways) that resolve
// the type at the destination whenever the type at the source resolves.
struct InferenceNode {
    Location loc;

    // List of InferredType values in the AST that are unified by this node.
    std::vector<InferredType*> nodes_;
    // Current value of this node.
    InferredType type_;

    typedef std::function<
        InferredType(const std::vector<InferredType>&)> TransferFunc;

    // Inputs from other nodes: these inputs produce types that also must be
    // unified into this node. The types are produced as the result of some
    // arbitrary computation on the types on some other set of nodes. A
    // transfer function is invoked only once all input nodes are in RESOLVED
    // state. (CONFLICT states are handled automatically and propagate to this
    // node's type; UNKNOWN states on inputs prevent transfer functions from
    // being called.)
    std::vector<std::pair<TransferFunc, std::vector<InferenceNode*>>> inputs_;

    // Validator. This is run after the type is deduced and may signal an error
    // if the type does not meet some constraint.
    typedef std::function<bool(InferredType, ErrorCollector*)> ValidatorFunc;

    std::vector<ValidatorFunc> validators_;

    // Recompute this node's value by joining the values of all linked types in
    // the AST and the result of all input-edge transfer functions, then
    // propagate the joined value back to all linked types in the AST. Return
    // true if the type value of this node changed since the last update.
    bool Update();

    bool Validate(ErrorCollector* coll) const;
};

// This pass traverses the AST, building a "type inference graph". Each node in
// the inference graph corresponds to one *set* of unified type slots, i.e.,
// type slots in the AST that must resolve to the same type. It then solves
// this graph and updates the types in the AST using the links it retained to
// AST nodes.
//
// This pass runs after function inlining (so does not need to deal with
// func-arg type propagation) but before type lowering (so needs to worry about
// aggregate types and type fields).
class TypeInferPass : public ASTVisitorContext {
    public:
        TypeInferPass(ErrorCollector* coll);
        ~TypeInferPass();

    protected:
        // We implement 'modify' handlers to take references to type slots and
        // build the type inference graph so that we can then run the inference
        // algorithm and update the types. N.B. that the handlers *don't*
        // directly modify the types -- they only build the graph with mutable
        // references to the types! These references are later used to update
        // all types during Infer() after the types are inferred at all nodes.

        virtual Result ModifyASTPre(ASTRef<AST>& node);
        virtual Result ModifyASTExprPost(ASTRef<ASTExpr>& node);
        virtual Result ModifyASTStmtLetPost(ASTRef<ASTStmtLet>& node);
        virtual Result ModifyASTStmtAssignPost(ASTRef<ASTStmtAssign>& node);
        virtual Result ModifyASTStmtWritePost(ASTRef<ASTStmtWrite>& node);
        virtual Result ModifyASTStmtIfPost(ASTRef<ASTStmtIf>& node);
        virtual Result ModifyASTStmtWhilePost(ASTRef<ASTStmtWhile>& node);
        virtual Result ModifyASTStmtBypassStartPost(
                ASTRef<ASTStmtBypassStart>& node);
        virtual Result ModifyASTStmtBypassEndPost(
                ASTRef<ASTStmtBypassEnd>& node);
        virtual Result ModifyASTStmtBypassWritePost(
                ASTRef<ASTStmtBypassWrite>& node);

        // Post-AST handler actually runs the type inference.
        virtual Result ModifyASTPost(ASTRef<AST>& node) {
            return Infer(Errors()) ? VISIT_CONTINUE : VISIT_END;
        }

    private:
        std::unique_ptr<AggTypeResolver> aggs_;

        // TODO: use the type inference graph to resolve aggregate types by
        // linking field refs to the field defs inside the typedefs, and adding
        // transfer functions to set the aggregate type based on field defs'
        // types. This gets complex though because we need an initial pass to
        // simply resolve aggregate types well enough to build the field-def
        // link parts of the graph; i.e., we need one step to establish the
        // universe of types and another step to assign types to all value
        // nodes.

        // ------ inference graph ------
        std::vector<std::unique_ptr<InferenceNode>> nodes_;
        std::map<const void*, InferenceNode*> nodes_by_value_;

        // Add a new node to the inference graph.
        InferenceNode* AddNode();

        // Fetch the inference graph node for the given AST node, creating a
        // new one if one does not exist.
        InferenceNode* NodeForAST(const void* key);

        // Add a simple one-way transfer function that conveys n1's type to
        // n2's type.
        void ConveyType(InferenceNode* n1, InferenceNode* n2);
        // Add a zero-input transfer function that conveys a constant type to
        // the given node.
        void ConveyConstType(InferenceNode* n, InferredType type);
        // Add a transfer function that enforces one node's width as the sum of
        // N others.
        void SumWidths(InferenceNode* sum, std::vector<InferenceNode*> nodes);
        // Add a validator that ensures the given node is a simple type (not a
        // port, not an array; aggregates allowed as they're treated like
        // large/concatenated integers).
        void EnsureSimple(InferenceNode* n);

        // Connect two nodes across a port read/write -- two-way type
        // conveyance with transfer functions that add/remove the 'port type'
        // modifier.
        void ConveyPort(InferenceNode* port_node, InferenceNode* value_node);

        // Connect an array value, the extracted array slot, and the index used
        // to extract it.
        void ConveyArrayRef(InferenceNode* n, InferenceNode* array,
                InferenceNode* index);

        // Ensure that a type is an array.
        void EnsureArray(InferenceNode* n);

        // Connect a reg value and its underlying value type.
        void ConveyRegRef(InferenceNode* n, InferenceNode* reg);

        // Ensure a type is a reg type.
        void EnsureReg(InferenceNode* n);

        // Connect an aggregate type and an underlying field value extracted by
        // a field reference.
        void ConveyFieldRef(InferenceNode* n, InferenceNode* agg,
                std::string field_name);

        // Connect an aggregate type literal with the types of all of its field
        // value expressions.
        void ConveyAggLiteral(InferenceNode* n, ASTExpr* expr);

        // Set up a validator to ensure cast is valid, and convey the
        // casted-to type to the result node.
        bool HandleCast(
                InferenceNode* n, InferenceNode* arg,
                const ASTType* ty);

        // Connect a bypass value and its value read or written.
        void ConveyBypass(InferenceNode* n, InferenceNode* value);

        // Ensure that a value is a bypass value.
        void EnsureBypass(InferenceNode* n);

        // Once the pass has run over the AST to collect all type slots and
        // build the inference graph, this function solves the inference graph
        // to arrive at concrete types. It then labels all expression and
        // let-stmt nodes in the AST with their types.
        bool Infer(ErrorCollector* coll);

};

}  // namesapce frontend
}  // namespace autopiper

#endif  // _AUTOPIPER_FRONTEND_TYPE_INFER_H_
