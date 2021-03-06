/*
 * Souffle - A Datalog Compiler
 * Copyright (c) 2018, The Souffle Developers. All rights reserved
 * Licensed under the Universal Permissive License v 1.0 as shown at:
 * - https://opensource.org/licenses/UPL
 * - <souffle root>/licenses/SOUFFLE-UPL.txt
 */

/************************************************************************
 *
 * @file RamTransforms.cpp
 *
 * Implementation of RAM transformation passes.
 *
 ***********************************************************************/

#include "RamTransforms.h"
#include "BinaryConstraintOps.h"
#include "RamCondition.h"
#include "RamExpression.h"
#include "RamNode.h"
#include "RamOperation.h"
#include "RamProgram.h"
#include "RamRelation.h"
#include "RamStatement.h"
#include "RamTypes.h"
#include "RamVisitor.h"
#include <utility>
#include <vector>

namespace souffle {

bool ExpandFilterTransformer::expandFilters(RamProgram& program) {
    // flag to determine whether the RAM program has changed
    bool changed = false;

    visitDepthFirst(program, [&](const RamQuery& query) {
        std::function<std::unique_ptr<RamNode>(std::unique_ptr<RamNode>)> filterRewriter =
                [&](std::unique_ptr<RamNode> node) -> std::unique_ptr<RamNode> {
            if (const RamFilter* filter = dynamic_cast<RamFilter*>(node.get())) {
                const RamCondition* condition = &filter->getCondition();
                std::vector<std::unique_ptr<RamCondition>> conditionList = toConjunctionList(condition);
                if (conditionList.size() > 1) {
                    changed = true;
                    std::vector<std::unique_ptr<RamFilter>> filters;
                    for (auto iter = conditionList.rbegin(); iter != conditionList.rend(); ++iter) {
                        auto& cond = *iter;
                        auto tempCond = cond->clone();
                        if (filters.empty()) {
                            filters.emplace_back(std::make_unique<RamFilter>(
                                    std::unique_ptr<RamCondition>(std::move(tempCond)),
                                    std::unique_ptr<RamOperation>(filter->getOperation().clone())));
                        } else {
                            filters.emplace_back(std::make_unique<RamFilter>(
                                    std::unique_ptr<RamCondition>(std::move(tempCond)),
                                    std::move(filters.back())));
                        }
                    }
                    node = std::move(filters.back());
                }
            }
            node->apply(makeLambdaRamMapper(filterRewriter));
            return node;
        };
        const_cast<RamQuery*>(&query)->apply(makeLambdaRamMapper(filterRewriter));
    });
    return changed;
}

bool HoistConditionsTransformer::hoistConditions(RamProgram& program) {
    // flag to determine whether the RAM program has changed
    bool changed = false;

    // helper for collecting conditions from filter operations
    auto addCondition = [](std::unique_ptr<RamCondition> condition,
                                RamCondition* c) -> std::unique_ptr<RamCondition> {
        if (condition == nullptr) {
            return std::unique_ptr<RamCondition>(c);
        } else {
            return std::make_unique<RamConjunction>(std::move(condition), std::unique_ptr<RamCondition>(c));
        }
    };

    // insert a new filter
    auto insertFilter = [](RamOperation* op, std::unique_ptr<RamCondition>& condition) {
        op->apply(makeLambdaRamMapper([&](std::unique_ptr<RamNode> node) -> std::unique_ptr<RamNode> {
            if (nullptr != dynamic_cast<RamOperation*>(node.get())) {
                return std::make_unique<RamFilter>(std::move(condition),
                        std::unique_ptr<RamOperation>(dynamic_cast<RamOperation*>(node.release())));
            }
            return node;
        }));
    };

    // hoist conditions to the most outer scope if they
    // don't depend on RamSearches
    visitDepthFirst(program, [&](const RamQuery& query) {
        std::unique_ptr<RamCondition> newCondition;
        std::function<std::unique_ptr<RamNode>(std::unique_ptr<RamNode>)> filterRewriter =
                [&](std::unique_ptr<RamNode> node) -> std::unique_ptr<RamNode> {
            if (auto* filter = dynamic_cast<RamFilter*>(node.get())) {
                const RamCondition& condition = filter->getCondition();
                // if filter condition is independent of any RamSearch,
                // delete the filter operation and collect condition
                if (rla->getLevel(&condition) == -1) {
                    changed = true;
                    newCondition = addCondition(std::move(newCondition), condition.clone());
                    node->apply(makeLambdaRamMapper(filterRewriter));
                    return std::unique_ptr<RamOperation>(filter->getOperation().clone());
                }
            }
            node->apply(makeLambdaRamMapper(filterRewriter));
            return node;
        };
        const_cast<RamQuery*>(&query)->apply(makeLambdaRamMapper(filterRewriter));
        if (newCondition != nullptr) {
            // insert new filter operation at outer-most level of the query
            changed = true;
            insertFilter((RamOperation*)&query, newCondition);
        }
    });

    // hoist conditions for each RamSearch operation
    visitDepthFirst(program, [&](const RamSearch& search) {
        std::unique_ptr<RamCondition> newCondition;
        std::function<std::unique_ptr<RamNode>(std::unique_ptr<RamNode>)> filterRewriter =
                [&](std::unique_ptr<RamNode> node) -> std::unique_ptr<RamNode> {
            if (auto* filter = dynamic_cast<RamFilter*>(node.get())) {
                const RamCondition& condition = filter->getCondition();
                // if filter condition matches level of RamSearch,
                // delete the filter operation and collect condition
                if (rla->getLevel(&condition) == search.getTupleId()) {
                    changed = true;
                    newCondition = addCondition(std::move(newCondition), condition.clone());
                    node->apply(makeLambdaRamMapper(filterRewriter));
                    return std::unique_ptr<RamOperation>(filter->getOperation().clone());
                }
            }
            node->apply(makeLambdaRamMapper(filterRewriter));
            return node;
        };
        const_cast<RamSearch*>(&search)->apply(makeLambdaRamMapper(filterRewriter));
        if (newCondition != nullptr) {
            // insert new filter operation after the search operation
            changed = true;
            insertFilter((RamOperation*)&search, newCondition);
        }
    });
    return changed;
}

std::unique_ptr<RamExpression> MakeIndexTransformer::getExpression(
        RamCondition* c, size_t& element, int identifier) {
    if (auto* binRelOp = dynamic_cast<RamConstraint*>(c)) {
        if (binRelOp->getOperator() == BinaryConstraintOp::EQ) {
            if (const RamElementAccess* lhs = dynamic_cast<RamElementAccess*>(binRelOp->getLHS())) {
                RamExpression* rhs = binRelOp->getRHS();
                if (lhs->getTupleId() == identifier && rla->getLevel(rhs) < identifier) {
                    element = lhs->getElement();
                    return std::unique_ptr<RamExpression>(rhs->clone());
                }
            }
            if (const RamElementAccess* rhs = dynamic_cast<RamElementAccess*>(binRelOp->getRHS())) {
                RamExpression* lhs = binRelOp->getLHS();
                if (rhs->getTupleId() == identifier && rla->getLevel(lhs) < identifier) {
                    element = rhs->getElement();
                    return std::unique_ptr<RamExpression>(lhs->clone());
                }
            }
        }
    }
    return nullptr;
}

std::unique_ptr<RamCondition> MakeIndexTransformer::constructPattern(
        std::vector<std::unique_ptr<RamExpression>>& queryPattern, bool& indexable,
        std::vector<std::unique_ptr<RamCondition>> conditionList, int identifier) {
    // Remaining conditions which cannot be handled by an index
    std::unique_ptr<RamCondition> condition;
    auto addCondition = [&](std::unique_ptr<RamCondition> c) {
        if (condition != nullptr) {
            condition = std::make_unique<RamConjunction>(std::move(condition), std::move(c));
        } else {
            condition = std::move(c);
        }
    };

    // Build query pattern and remaining condition
    for (auto& cond : conditionList) {
        size_t element = 0;
        if (std::unique_ptr<RamExpression> value = getExpression(cond.get(), element, identifier)) {
            if (queryPattern[element] == nullptr) {
                indexable = true;
                queryPattern[element] = std::move(value);
            } else {
                // TODO: This case is a recursive case introducing a new filter operation
                // at upper level, i.e., if queryPattern[element] == value ...
                // and apply indexing recursively to the rewritten program.
                // At the moment we just another local condition which is sub-optimal
                // Note sure whether there are cases in practice that would improve the performance
                addCondition(std::make_unique<RamConstraint>(BinaryConstraintOp::EQ, std::move(value),
                        std::unique_ptr<RamExpression>(queryPattern[element]->clone())));
            }
        } else {
            addCondition(std::move(cond));
        }
    }
    return condition;
}

std::unique_ptr<RamOperation> MakeIndexTransformer::rewriteAggregate(const RamAggregate* agg) {
    if (agg->getCondition() != nullptr) {
        const RamRelation& rel = agg->getRelation();
        int identifier = agg->getTupleId();
        std::vector<std::unique_ptr<RamExpression>> queryPattern(rel.getArity());
        bool indexable = false;
        std::unique_ptr<RamCondition> condition =
                constructPattern(queryPattern, indexable, toConjunctionList(agg->getCondition()), identifier);
        if (indexable) {
            std::unique_ptr<RamExpression> expr;
            if (agg->getExpression() != nullptr) {
                expr = std::unique_ptr<RamExpression>(agg->getExpression()->clone());
            }
            return std::make_unique<RamIndexAggregate>(
                    std::unique_ptr<RamOperation>(agg->getOperation().clone()), agg->getFunction(),
                    std::make_unique<RamRelationReference>(&rel), std::move(expr), std::move(condition),
                    std::move(queryPattern), agg->getTupleId());
        }
    }
    return nullptr;
}

std::unique_ptr<RamOperation> MakeIndexTransformer::rewriteScan(const RamScan* scan) {
    if (const auto* filter = dynamic_cast<const RamFilter*>(&scan->getOperation())) {
        const RamRelation& rel = scan->getRelation();
        const int identifier = scan->getTupleId();
        std::vector<std::unique_ptr<RamExpression>> queryPattern(rel.getArity());
        bool indexable = false;
        std::unique_ptr<RamCondition> condition = constructPattern(
                queryPattern, indexable, toConjunctionList(&filter->getCondition()), identifier);
        if (indexable) {
            return std::make_unique<RamIndexScan>(std::make_unique<RamRelationReference>(&rel), identifier,
                    std::move(queryPattern),
                    condition == nullptr
                            ? std::unique_ptr<RamOperation>(filter->getOperation().clone())
                            : std::make_unique<RamFilter>(std::move(condition),
                                      std::unique_ptr<RamOperation>(filter->getOperation().clone())),
                    scan->getProfileText());
        }
    }
    return nullptr;
}

bool MakeIndexTransformer::makeIndex(RamProgram& program) {
    bool changed = false;
    visitDepthFirst(program, [&](const RamQuery& query) {
        std::function<std::unique_ptr<RamNode>(std::unique_ptr<RamNode>)> scanRewriter =
                [&](std::unique_ptr<RamNode> node) -> std::unique_ptr<RamNode> {
            if (const RamScan* scan = dynamic_cast<RamScan*>(node.get())) {
                if (std::unique_ptr<RamOperation> op = rewriteScan(scan)) {
                    changed = true;
                    node = std::move(op);
                }
            } else if (const RamAggregate* agg = dynamic_cast<RamAggregate*>(node.get())) {
                if (std::unique_ptr<RamOperation> op = rewriteAggregate(agg)) {
                    changed = true;
                    node = std::move(op);
                }
            }
            node->apply(makeLambdaRamMapper(scanRewriter));
            return node;
        };
        const_cast<RamQuery*>(&query)->apply(makeLambdaRamMapper(scanRewriter));
    });
    return changed;
}

std::unique_ptr<RamOperation> IfConversionTransformer::rewriteIndexScan(const RamIndexScan* indexScan) {
    // check whether tuple is used in subsequent operations
    bool tupleNotUsed = true;
    visitDepthFirst(*indexScan, [&](const RamNode& node) {
        if (const RamElementAccess* element = dynamic_cast<const RamElementAccess*>(&node)) {
            if (element->getTupleId() == indexScan->getTupleId()) {
                tupleNotUsed = false;
            }
        } else if (const RamUnpackRecord* unpack = dynamic_cast<const RamUnpackRecord*>(&node)) {
            if (unpack->getReferenceLevel() == indexScan->getTupleId()) {
                tupleNotUsed = false;
            }
        }
    });

    // if not used, transform the IndexScan operation to an existence check
    if (tupleNotUsed) {
        // replace IndexScan with an Filter/Existence check
        std::vector<std::unique_ptr<RamExpression>> newValues;
        for (auto& cur : indexScan->getRangePattern()) {
            RamExpression* val = nullptr;
            if (cur != nullptr) {
                val = cur->clone();
            }
            newValues.emplace_back(val);
        }

        // check if there is a break statement nested in the Scan - if so, remove it
        RamOperation* newOp;
        if (const RamBreak* breakOp = dynamic_cast<const RamBreak*>(&indexScan->getOperation())) {
            newOp = breakOp->getOperation().clone();
        } else {
            newOp = indexScan->getOperation().clone();
        }

        return std::make_unique<RamFilter>(
                std::make_unique<RamExistenceCheck>(
                        std::make_unique<RamRelationReference>(&indexScan->getRelation()),
                        std::move(newValues)),
                std::unique_ptr<RamOperation>(newOp), indexScan->getProfileText());
    }
    return nullptr;
}

bool IfConversionTransformer::convertIndexScans(RamProgram& program) {
    bool changed = false;
    visitDepthFirst(program, [&](const RamQuery& query) {
        std::function<std::unique_ptr<RamNode>(std::unique_ptr<RamNode>)> scanRewriter =
                [&](std::unique_ptr<RamNode> node) -> std::unique_ptr<RamNode> {
            if (const RamIndexScan* scan = dynamic_cast<RamIndexScan*>(node.get())) {
                if (std::unique_ptr<RamOperation> op = rewriteIndexScan(scan)) {
                    changed = true;
                    node = std::move(op);
                }
            }
            node->apply(makeLambdaRamMapper(scanRewriter));
            return node;
        };
        const_cast<RamQuery*>(&query)->apply(makeLambdaRamMapper(scanRewriter));
    });
    return changed;
}

std::unique_ptr<RamOperation> ChoiceConversionTransformer::rewriteScan(const RamScan* scan) {
    bool transformTuple = false;

    // Check that RamFilter follows the Scan in the loop nest
    if (const auto* filter = dynamic_cast<const RamFilter*>(&scan->getOperation())) {
        // Check that the Filter uses the identifier in the Scan
        if (rla->getLevel(&filter->getCondition()) == scan->getTupleId()) {
            transformTuple = true;

            // Check that the filter is not referred to after
            const auto* nextNode = dynamic_cast<const RamNode*>(&filter->getOperation());

            visitDepthFirst(*nextNode, [&](const RamElementAccess& element) {
                if (element.getTupleId() == scan->getTupleId()) {
                    transformTuple = false;
                }
            });

            visitDepthFirst(*nextNode, [&](const RamUnpackRecord& unpack) {
                if (unpack.getReferenceLevel() == scan->getTupleId()) {
                    transformTuple = false;
                }
            });
        }
    }

    // Convert the Scan/If pair into a Choice
    if (transformTuple) {
        std::vector<std::unique_ptr<RamExpression>> newValues;
        const auto* filter = dynamic_cast<const RamFilter*>(&scan->getOperation());
        const int identifier = scan->getTupleId();

        return std::make_unique<RamChoice>(std::make_unique<RamRelationReference>(&scan->getRelation()),
                identifier, std::unique_ptr<RamCondition>(filter->getCondition().clone()),
                std::unique_ptr<RamOperation>(scan->getOperation().clone()), scan->getProfileText());
    }
    return nullptr;
}

std::unique_ptr<RamOperation> ChoiceConversionTransformer::rewriteIndexScan(const RamIndexScan* indexScan) {
    bool transformTuple = false;

    // Check that RamFilter follows the IndexScan in the loop nest
    if (const auto* filter = dynamic_cast<const RamFilter*>(&indexScan->getOperation())) {
        // Check that the Filter uses the identifier in the IndexScan
        if (rla->getLevel(&filter->getCondition()) == indexScan->getTupleId()) {
            transformTuple = true;

            // Check that the filter is not referred to after
            const auto* nextNode = dynamic_cast<const RamNode*>(&filter->getOperation());

            visitDepthFirst(*nextNode, [&](const RamElementAccess& element) {
                if (element.getTupleId() == indexScan->getTupleId()) {
                    transformTuple = false;
                }
            });

            visitDepthFirst(*nextNode, [&](const RamUnpackRecord& unpack) {
                if (unpack.getReferenceLevel() == indexScan->getTupleId()) {
                    transformTuple = false;
                }
            });
        }
    }

    // Convert the IndexScan/If pair into an IndexChoice
    if (transformTuple) {
        std::vector<std::unique_ptr<RamExpression>> newValues;
        const auto* filter = dynamic_cast<const RamFilter*>(&indexScan->getOperation());
        const int identifier = indexScan->getTupleId();
        const RamRelation& rel = indexScan->getRelation();

        for (auto& cur : indexScan->getRangePattern()) {
            RamExpression* val = nullptr;
            if (cur != nullptr) {
                val = cur->clone();
            }
            newValues.emplace_back(val);
        }

        return std::make_unique<RamIndexChoice>(std::make_unique<RamRelationReference>(&rel), identifier,
                std::unique_ptr<RamCondition>(filter->getCondition().clone()), std::move(newValues),
                std::unique_ptr<RamOperation>(filter->getOperation().clone()), indexScan->getProfileText());
    }
    return nullptr;
}

bool ChoiceConversionTransformer::convertScans(RamProgram& program) {
    bool changed = false;
    visitDepthFirst(program, [&](const RamQuery& query) {
        std::function<std::unique_ptr<RamNode>(std::unique_ptr<RamNode>)> scanRewriter =
                [&](std::unique_ptr<RamNode> node) -> std::unique_ptr<RamNode> {
            if (const RamScan* scan = dynamic_cast<RamScan*>(node.get())) {
                if (std::unique_ptr<RamOperation> op = rewriteScan(scan)) {
                    changed = true;
                    node = std::move(op);
                }
            } else if (const RamIndexScan* indexScan = dynamic_cast<RamIndexScan*>(node.get())) {
                if (std::unique_ptr<RamOperation> op = rewriteIndexScan(indexScan)) {
                    changed = true;
                    node = std::move(op);
                }
            }
            node->apply(makeLambdaRamMapper(scanRewriter));

            return node;
        };
        const_cast<RamQuery*>(&query)->apply(makeLambdaRamMapper(scanRewriter));
    });

    return changed;
}

}  // end of namespace souffle
