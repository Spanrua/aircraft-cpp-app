#pragma once

#include <QString>

enum class WorkflowStepState
{
    NotStarted,
    Ready,
    Running,
    Completed,
    Warning,
    Blocked
};

inline QString workflowStepStateKey(WorkflowStepState state)
{
    switch (state) {
    case WorkflowStepState::Ready: return QStringLiteral("ready");
    case WorkflowStepState::Running: return QStringLiteral("running");
    case WorkflowStepState::Completed: return QStringLiteral("completed");
    case WorkflowStepState::Warning: return QStringLiteral("warning");
    case WorkflowStepState::Blocked: return QStringLiteral("blocked");
    case WorkflowStepState::NotStarted:
    default: return QStringLiteral("notStarted");
    }
}
