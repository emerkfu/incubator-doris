// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

package org.apache.doris.nereids.trees.plans;

import org.apache.doris.nereids.memo.Group;
import org.apache.doris.nereids.memo.GroupExpression;
import org.apache.doris.nereids.properties.LogicalProperties;
import org.apache.doris.nereids.trees.expressions.Expression;
import org.apache.doris.nereids.trees.expressions.Slot;
import org.apache.doris.nereids.trees.plans.logical.LogicalLeaf;
import org.apache.doris.nereids.trees.plans.visitor.PlanVisitor;
import org.apache.doris.statistics.ExprStats;
import org.apache.doris.statistics.StatisticalType;
import org.apache.doris.statistics.StatsDeriveResult;

import com.google.common.collect.ImmutableList;

import java.util.List;
import java.util.Optional;

/**
 * A virtual node that represents a sequence plan in a Group.
 * Used in {@link org.apache.doris.nereids.pattern.GroupExpressionMatching.GroupExpressionIterator},
 * as a place-holder when do match root.
 */
public class GroupPlan extends LogicalLeaf {
    private final Group group;

    public GroupPlan(Group group) {
        super(PlanType.GROUP_PLAN, Optional.empty(), Optional.of(group.getLogicalProperties()));
        this.group = group;
    }

    @Override
    public Optional<GroupExpression> getGroupExpression() {
        return Optional.empty();
    }

    public Group getGroup() {
        return group;
    }

    @Override
    public List<Expression> getExpressions() {
        return ImmutableList.of();
    }

    @Override
    public GroupPlan withOutput(List<Slot> output) {
        throw new IllegalStateException("GroupPlan can not invoke withOutput()");
    }

    @Override
    public GroupPlan withChildren(List<Plan> children) {
        throw new IllegalStateException("GroupPlan can not invoke withChildren()");
    }

    @Override
    public List<StatsDeriveResult> getChildrenStats() {
        throw new IllegalStateException("GroupPlan can not invoke getChildrenStats()");
    }

    @Override
    public StatsDeriveResult getStatsDeriveResult() {
        throw new IllegalStateException("GroupPlan can not invoke getStatsDeriveResult()");
    }

    @Override
    public StatisticalType getStatisticalType() {
        throw new IllegalStateException("GroupPlan can not invoke getStatisticalType()");
    }

    @Override
    public void setStatsDeriveResult(StatsDeriveResult result) {
        throw new IllegalStateException("GroupPlan can not invoke setStatsDeriveResult()");
    }

    @Override
    public long getLimit() {
        throw new IllegalStateException("GroupPlan can not invoke getLimit()");
    }

    @Override
    public List<? extends ExprStats> getConjuncts() {
        throw new IllegalStateException("GroupPlan can not invoke getConjuncts()");
    }

    @Override
    public Plan withGroupExpression(Optional<GroupExpression> groupExpression) {
        throw new IllegalStateException("GroupPlan can not invoke withGroupExpression()");
    }

    @Override
    public Plan withLogicalProperties(Optional<LogicalProperties> logicalProperties) {
        throw new IllegalStateException("GroupPlan can not invoke withLogicalProperties()");
    }

    @Override
    public List<Slot> computeOutput() {
        throw new IllegalStateException("GroupPlan can not compute output."
            + " You should invoke GroupPlan.getOutput()");
    }

    @Override
    public LogicalProperties computeLogicalProperties(Plan... inputs) {
        throw new IllegalStateException("GroupPlan can not compute logical properties."
            + " You should invoke GroupPlan.getLogicalProperties()");
    }

    @Override
    public <R, C> R accept(PlanVisitor<R, C> visitor, C context) {
        return visitor.visitGroupPlan(this, context);
    }
}
