// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "stdafx.h"
#include "Debugger.h"

#include "ErrorHelpers.h"
#include "PropertyHelpers.h"
#include "ProtocolHandler.h"

namespace JsDebug
{
    namespace
    {
        const char c_ErrorInvalidOrdinal[] = "Invalid ordinal value";
    }

    Debugger::Debugger(ProtocolHandler* handler, JsRuntimeHandle runtime)
        : m_handler(handler)
        , m_runtime(runtime)
        , m_debugContext(runtime)
        , m_isEnabled(false)
        , m_isPaused(false)
        , m_isRunningNestedMessageLoop(false)
        , m_shouldPauseOnNextStatement(false)
        , m_sourceEventCallback(nullptr)
        , m_sourceEventCallbackState(nullptr)
        , m_breakEventCallback(nullptr)
        , m_breakEventCallbackState(nullptr)
    {
        IfJsErrorThrow(JsDiagStartDebugging(m_runtime, &Debugger::DebugEventCallback, this));
    }

    Debugger::~Debugger()
    {
        try
        {
            // The API requires that a state param be provided, even though we don't use it.
            void* state = nullptr;
            IfJsErrorThrow(JsDiagStopDebugging(m_runtime, &state));
        }
        catch (...)
        {
            // Don't allow the exception to propagate.
        }
    }

    DebuggerContext* Debugger::GetDebugContext()
    {
        return &m_debugContext;
    }

    void Debugger::Enable()
    {
        if (m_isEnabled)
        {
            return;
        }

        m_isEnabled = true;

        // TODO: Other initialization
    }

    void Debugger::Disable()
    {
        if (!m_isEnabled)
        {
            return;
        }

        m_isEnabled = false;
        ClearBreakpoints();
    }

    void Debugger::SetSourceEventHandler(DebuggerSourceEventHandler callback, void* callbackState)
    {
        m_sourceEventCallback = callback;
        m_sourceEventCallbackState = callbackState;
    }

    void Debugger::SetBreakEventHandler(DebuggerBreakEventHandler callback, void* callbackState)
    {
        m_breakEventCallback = callback;
        m_breakEventCallbackState = callbackState;
    }

    void Debugger::SetResumeEventHandler(DebuggerResumeEventHandler callback, void *callbackState)
    {
        m_resumeEventCallback = callback;
        m_resumeEventCallbackState = callbackState;
    }

    void Debugger::RequestAsyncBreak()
    {
        IfJsErrorThrow(JsDiagRequestAsyncBreak(m_runtime));
    }

    void Debugger::PauseOnNextStatement()
    {
        m_shouldPauseOnNextStatement = true;
        RequestAsyncBreak();
    }

    std::vector<DebuggerScript> Debugger::GetScripts()
    {
        std::vector<DebuggerScript> scripts;
        JsValueRef scriptsArray = JS_INVALID_REFERENCE;
        JsErrorCode result = JsDiagGetScripts(&scriptsArray);
          
        if (result == JsNoError)
        {
          int length = PropertyHelpers::GetPropertyInt(scriptsArray, PropertyHelpers::Names::Length);

          for (int index = 0; index < length; index++)
          {
            JsValueRef scriptValue = PropertyHelpers::GetIndexedProperty(scriptsArray, index);

            scripts.emplace_back(this, scriptValue);
          }
        }

        return scripts;
    }

    DebuggerCallFrame Debugger::GetCallFrame(int ordinal)
    {
        JsValueRef stackTrace = JS_INVALID_REFERENCE;
        IfJsErrorThrow(JsDiagGetStackTrace(&stackTrace));

        int length = PropertyHelpers::GetPropertyInt(stackTrace, PropertyHelpers::Names::Length);

        if (ordinal >= length)
        {
            throw std::runtime_error(c_ErrorInvalidOrdinal);
        }

        return DebuggerCallFrame(PropertyHelpers::GetIndexedProperty(stackTrace, ordinal));
    }

    std::vector<DebuggerCallFrame> Debugger::GetCallFrames(int limit)
    {
        JsValueRef stackTrace = JS_INVALID_REFERENCE;
        IfJsErrorThrow(JsDiagGetStackTrace(&stackTrace));

        int length = PropertyHelpers::GetPropertyInt(stackTrace, PropertyHelpers::Names::Length);

        if (limit > 0 && limit < length) {
            length = limit;
        }

        std::vector<DebuggerCallFrame> callFrames;

        for (int index = 0; index < length; ++index) {
            JsValueRef callFrameValue = PropertyHelpers::GetIndexedProperty(stackTrace, index);

            callFrames.emplace_back(callFrameValue);
        }

        return callFrames;
    }

    DebuggerObject Debugger::GetObjectFromHandle(int handle)
    {
        JsValueRef obj = JS_INVALID_REFERENCE;
        IfJsErrorThrow(JsDiagGetObjectFromHandle(handle, &obj));

        return DebuggerObject(obj);
    }

    void Debugger::SetBreakpoint(DebuggerBreakpoint& breakpoint)
    {
        int scriptId = breakpoint.GetScriptId().toInteger();

        JsValueRef bp = JS_INVALID_REFERENCE;
        IfJsErrorThrow(JsDiagSetBreakpoint(scriptId, breakpoint.GetLineNumber(), breakpoint.GetColumnNumber(), &bp));

        breakpoint.OnBreakpointResolved(
            PropertyHelpers::GetPropertyInt(bp, PropertyHelpers::Names::BreakpointId),
            PropertyHelpers::GetPropertyInt(bp, PropertyHelpers::Names::Line),
            PropertyHelpers::GetPropertyInt(bp, PropertyHelpers::Names::Column));
    }

    void Debugger::RemoveBreakpoint(DebuggerBreakpoint& breakpoint)
    {
        JsDiagRemoveBreakpoint(breakpoint.GetActualId());
    }

    JsDiagBreakOnExceptionAttributes Debugger::GetBreakOnException()
    {
        JsDiagBreakOnExceptionAttributes attributes = JsDiagBreakOnExceptionAttributeNone;
        IfJsErrorThrow(JsDiagGetBreakOnException(m_runtime, &attributes));

        return attributes;
    }

    void Debugger::SetBreakOnException(JsDiagBreakOnExceptionAttributes attributes)
    {
        IfJsErrorThrow(JsDiagSetBreakOnException(m_runtime, attributes));
    }

    bool Debugger::IsPaused()
    {
        return m_isPaused;
    }

    void Debugger::Continue()
    {
        m_handler->Continue();
    }

    void Debugger::Go()
    {
        m_shouldPauseOnNextStatement = false;
        m_handler->Continue();
    }

    static void IfSeriousJsErrorThrow(JsErrorCode err)
    {
        if (err != JsNoError && err != JsErrorDiagNotAtBreak)
            IfJsErrorThrow(err);
    }

    void Debugger::StepIn()
    {
        IfSeriousJsErrorThrow(JsDiagSetStepType(JsDiagStepTypeStepIn));
        Continue();
    }

    void Debugger::StepOut()
    {
        IfSeriousJsErrorThrow(JsDiagSetStepType(JsDiagStepTypeStepOut));
        Continue();
    }

    void Debugger::StepOver()
    {
        IfSeriousJsErrorThrow(JsDiagSetStepType(JsDiagStepTypeStepOver));
        Continue();
    }

    void Debugger::DebugEventCallback(JsDiagDebugEvent debugEvent, JsValueRef eventData, void* callbackState)
    {
        auto protocolHandler = static_cast<Debugger*>(callbackState);
        protocolHandler->HandleDebugEvent(debugEvent, eventData);
    }

    void Debugger::HandleDebugEvent(JsDiagDebugEvent debugEvent, JsValueRef eventData)
    {
        m_handler->ProcessCommandQueue();

        if (!m_isEnabled)
        {
            return;
        }

        // Ensure that there's an active context before trying to handle events.
        DebuggerContext::Scope debuggerScope(m_debugContext);

        switch (debugEvent) {
        case JsDiagDebugEventSourceCompile:
        case JsDiagDebugEventCompileError:
            // handle the event
            HandleSourceEvent(eventData, debugEvent == JsDiagDebugEventSourceCompile);

            // If we still have a pending break-on-next-statement, make another
            // async break request.  The engine considers our prior break request
            // to be satisified on *any* debug event, even a source event that
            // never enters the debugger UI.
            if (m_shouldPauseOnNextStatement)
                JsDiagRequestAsyncBreak(m_runtime);
            break;

        case JsDiagDebugEventBreakpoint:
        case JsDiagDebugEventStepComplete:
        case JsDiagDebugEventDebuggerStatement:
        case JsDiagDebugEventRuntimeException:
            HandleBreak(eventData);
            break;

        case JsDiagDebugEventAsyncBreak:
            if (m_shouldPauseOnNextStatement)
            {
                m_shouldPauseOnNextStatement = false;
                HandleBreak(eventData);
            }
            break;
        }
    }

    void Debugger::HandleSourceEvent(JsValueRef eventData, bool success)
    {
        if (m_sourceEventCallback != nullptr)
        {
            DebuggerScript scriptInfo(this, eventData);
            m_sourceEventCallback(scriptInfo, success, m_sourceEventCallbackState);
        }
    }

    void Debugger::HandleBreak(JsValueRef eventData)
    {
        if (m_isRunningNestedMessageLoop)
        {
            // Don't allow reentrancy
            return;
        }

        if (m_breakEventCallback != nullptr)
        {
            m_isPaused = true;

            DebuggerBreak breakInfo(eventData);
            SkipPauseRequest request = m_breakEventCallback(breakInfo, m_breakEventCallbackState);

            if (request == SkipPauseRequest::RequestNoSkip)
            {
                m_isRunningNestedMessageLoop = true;
                m_handler->ProcessDeferredGo();
                m_handler->WaitForDebugger();
                m_isRunningNestedMessageLoop = false;
            }

            m_isPaused = false;

            if (request == SkipPauseRequest::RequestStepFrame ||
                request == SkipPauseRequest::RequestStepInto)
            {
                IfJsErrorThrow(JsDiagSetStepType(JsDiagStepTypeStepIn));
            }
            else if (request == SkipPauseRequest::RequestStepOut)
            {
                IfJsErrorThrow(JsDiagSetStepType(JsDiagStepTypeStepOut));
            }
  
            if (m_resumeEventCallback != nullptr)
                m_resumeEventCallback(m_resumeEventCallbackState);
        }
    }

    void Debugger::ClearBreakpoints()
    {
        // Ensure that there's an active context before trying to remove breakpoints.
        DebuggerContext::Scope debuggerScope(m_debugContext);

        JsValueRef breakpoints = JS_INVALID_REFERENCE;
        if (JsDiagGetBreakpoints(&breakpoints) == JsNoError)
        {
            int length = PropertyHelpers::GetPropertyInt(breakpoints, PropertyHelpers::Names::Length);

            for (int index = 0; index < length; index++)
            {
                JsValueRef breakpoint = PropertyHelpers::GetIndexedProperty(breakpoints, index);
                int breakpointId = PropertyHelpers::GetPropertyInt(breakpoint, PropertyHelpers::Names::BreakpointId);
                JsDiagRemoveBreakpoint(breakpointId);
            }
        }
    }
}
