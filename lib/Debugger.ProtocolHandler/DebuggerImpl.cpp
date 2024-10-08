// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "stdafx.h"
#include "DebuggerImpl.h"

#include "Debugger.h"
#include "DebuggerCallFrame.h"
#include "DebuggerScript.h"
#include "PropertyHelpers.h"
#include "ProtocolHandler.h"
#include "ProtocolHelpers.h"

#include <StringUtil.h>

namespace JsDebug
{
    using protocol::Array;
    using protocol::Debugger::CallFrame;
    using protocol::Debugger::Location;
    using protocol::FrontendChannel;
    using protocol::Maybe;
    using protocol::Response;
    using protocol::Runtime::ExceptionDetails;
    using protocol::Runtime::StackTrace;
    using protocol::String;
    using protocol::StringUtil;

    namespace
    {
        const char c_ErrorBreakpointCouldNotResolve[] = "Breakpoint could not be resolved";
        const char c_ErrorBreakpointExists[] = "Breakpoint at specified location already exists";
        const char c_ErrorBreakpointNotFound[] = "Breakpoint could not be found";
        const char c_ErrorCallFrameInvalidId[] = "Invalid call frame ID specified";
        const char c_ErrorInvalidColumnNumber[] = "Invalid column number specified";
        const char c_ErrorNotEnabled[] = "Debugger is not enabled";
        const char c_ErrorNotImplemented[] = "Debugger method not implemented";
        const char c_ErrorScriptMustBeLoaded[] = "Script must be loaded before resolving";
        const char c_ErrorUrlRequired[] = "Either url or urlRegex must be specified";
    }

    DebuggerImpl::DebuggerImpl(ProtocolHandler* handler, FrontendChannel* frontendChannel, Debugger* debugger)
        : m_handler(handler)
        , m_frontend(frontendChannel)
        , m_debugger(debugger)
        , m_isEnabled(false)
        , m_shouldSkipAllPauses(false)
    {
    }

    DebuggerImpl::~DebuggerImpl()
    {
        disable();
    }

    Response DebuggerImpl::enable()
    {
        if (m_isEnabled)
        {
            return Response::OK();
        }

        m_isEnabled = true;
        m_debugger->Enable();
        m_debugger->SetSourceEventHandler(&DebuggerImpl::SourceEventHandler, this);
        m_debugger->SetBreakEventHandler(&DebuggerImpl::BreakEventHandler, this);
        m_debugger->SetResumeEventHandler(&DebuggerImpl::ResumeEventHandler, this);

        std::vector<DebuggerScript> scripts = m_debugger->GetScripts();
        for (const auto& script : scripts)
        {
            HandleSourceEvent(script, true);
        }

        return Response::OK();
    }

    Response DebuggerImpl::disable()
    {
        if (!m_isEnabled) {
            return Response::OK();
        }

        m_isEnabled = false;
        m_debugger->Disable();
        m_debugger->SetSourceEventHandler(nullptr, nullptr);
        m_debugger->SetBreakEventHandler(nullptr, nullptr);
        m_debugger->SetResumeEventHandler(nullptr, nullptr);

        m_breakpointMap.clear();
        m_scriptMap.clear();
        m_shouldSkipAllPauses = false;

        return Response::OK();
    }

    Response DebuggerImpl::setBreakpointsActive(bool /*in_active*/)
    {
        return Response::Error(c_ErrorNotImplemented);
    }

    Response DebuggerImpl::setSkipAllPauses(bool /*in_skip*/)
    {
        return Response::Error(c_ErrorNotImplemented);
    }

    Response DebuggerImpl::setBreakpointByUrl(
        int in_lineNumber,
        Maybe<String> in_url,
        Maybe<String> in_urlRegex,
        Maybe<int> in_columnNumber,
        Maybe<String> in_condition,
        Maybe<String> *out_breakpointId,
        std::unique_ptr<Array<Location>>* out_locations)
    {
        String url;
        DebuggerBreakpoint::QueryType type;

        if (in_url.isJust())
        {
            url = in_url.fromJust();
            type = DebuggerBreakpoint::QueryType::Url;
        }
        else if (in_urlRegex.isJust())
        {
            url = in_urlRegex.fromJust();
            type = DebuggerBreakpoint::QueryType::UrlRegex;
        }
        else
        {
            return Response::Error(c_ErrorUrlRequired);
        }

        int columnNumber = in_columnNumber.fromMaybe(0);
        if (columnNumber < 0)
        {
            return Response::Error(c_ErrorInvalidColumnNumber);
        }

        String condition = in_condition.fromMaybe("");

        DebuggerBreakpoint breakpoint(
            m_debugger,
            url,
            type,
            in_lineNumber,
            columnNumber,
            condition);

        String breakpointId = breakpoint.GenerateKey();

        auto result = m_breakpointMap.find(breakpointId);
        if (result != m_breakpointMap.end())
        {
            return Response::Error(c_ErrorBreakpointExists);
        }

        auto locations = Array<Location>::create();

        try
        {
            for (const auto& script : m_scriptMap)
            {
                if (breakpoint.TryLoadScript(script.second))
                {
                    if (TryResolveBreakpoint(breakpoint))
                    {
                        locations->addItem(breakpoint.GetActualLocation());
                    }
                }
            }
        }
        catch (const JsErrorException& e)
        {
            return Response::Error(e.what());
        }

        *out_locations = std::move(locations);

        if (!ActualBreakpointExists(breakpoint))
        {
            *out_breakpointId = breakpointId;
            m_breakpointMap.emplace(breakpointId, breakpoint);
        }

        return Response::OK();
    }

    Response DebuggerImpl::setBreakpoint(
        std::unique_ptr<Location> in_location,
        Maybe<String> in_condition,
        Maybe<String> *out_breakpointId,
        std::unique_ptr<Location>* out_actualLocation)
    {
        DebuggerBreakpoint breakpoint = DebuggerBreakpoint::FromLocation(
            m_debugger,
            in_location.get(),
            in_condition.fromMaybe(""));

        String breakpointId = breakpoint.GenerateKey();

        auto result = m_breakpointMap.find(breakpointId);
        if (result != m_breakpointMap.end())
        {
            return Response::Error(c_ErrorBreakpointExists);
        }

        if (TryResolveBreakpoint(breakpoint))
        {
            *out_actualLocation = breakpoint.GetActualLocation();

            if (!ActualBreakpointExists(breakpoint))
            {
                *out_breakpointId = breakpointId;
                m_breakpointMap.emplace(breakpointId, breakpoint);
            }

            return Response::OK();
        }

        return Response::Error(c_ErrorBreakpointCouldNotResolve);
    }

    Response DebuggerImpl::removeBreakpoint(const String & in_breakpointId)
    {
        auto result = m_breakpointMap.find(in_breakpointId);
        if (result != m_breakpointMap.end())
        {
            m_debugger->RemoveBreakpoint(result->second);
            m_breakpointMap.erase(in_breakpointId);
            return Response::OK();
        }

        return Response::Error(c_ErrorBreakpointNotFound);
    }

    Response DebuggerImpl::continueToLocation(std::unique_ptr<Location> in_location)
    {
        return Response::Error(c_ErrorNotImplemented);
    }

    Response DebuggerImpl::stepOver()
    {
        m_debugger->StepOver();
        return Response::OK();
    }

    Response DebuggerImpl::stepInto()
    {
        m_debugger->StepIn();
        return Response::OK();
    }

    Response DebuggerImpl::stepOut()
    {
        m_debugger->StepOut();
        return Response::OK();
    }

    Response DebuggerImpl::pause()
    {
        m_debugger->PauseOnNextStatement();
        return Response::OK();
    }

    Response DebuggerImpl::resume()
    {
        if (!IsEnabled())
        {
            return Response::Error(c_ErrorNotEnabled);
        }

        m_debugger->Continue();
        return Response::OK();
    }

    Response DebuggerImpl::searchInContent(
        const String & /*in_scriptId*/,
        const String & /*in_query*/,
        Maybe<bool> /*in_caseSensitive*/,
        Maybe<bool> /*in_isRegex*/,
        std::unique_ptr<Array<protocol::Debugger::SearchMatch>>* /*out_result*/)
    {
        return Response::Error(c_ErrorNotImplemented);
    }

    Response DebuggerImpl::setScriptSource(
        const String & /*in_scriptId*/,
        const String & /*in_scriptSource*/,
        Maybe<bool> /*in_dryRun*/,
        Maybe<Array<CallFrame>>* /*out_callFrames*/,
        Maybe<bool>* /*out_stackChanged*/,
        Maybe<StackTrace>* /*out_asyncStackTrace*/,
        Maybe<protocol::Runtime::ExceptionDetails>* /*out_exceptionDetails*/)
    {
        return Response::Error(c_ErrorNotImplemented);
    }

    Response DebuggerImpl::restartFrame(
        const String & /*in_callFrameId*/,
        std::unique_ptr<Array<CallFrame>>* /*out_callFrames*/,
        Maybe<StackTrace>* /*out_asyncStackTrace*/)
    {
        return Response::Error(c_ErrorNotImplemented);
    }

    Response DebuggerImpl::getScriptSource(const String & in_scriptId, String  *out_scriptSource)
    {
        if (!IsEnabled())
        {
            return Response::Error(c_ErrorNotEnabled);
        }

        auto result = m_scriptMap.find(in_scriptId);
        if (result == m_scriptMap.end())
        {
            return Response::Error("Script not found: " + in_scriptId);
        }

        *out_scriptSource = result->second.Source();
        return Response::OK();
    }

    Response DebuggerImpl::setPauseOnExceptions(const String & in_state)
    {
        JsDiagBreakOnExceptionAttributes attributes = JsDiagBreakOnExceptionAttributeNone;

        if (in_state == "none")
        {
            attributes = JsDiagBreakOnExceptionAttributeNone;
        }
        else if (in_state == "all")
        {
            attributes = JsDiagBreakOnExceptionAttributeFirstChance;
        }
        else if (in_state == "uncaught")
        {
            attributes = JsDiagBreakOnExceptionAttributeUncaught;
        }
        else
        {
            return Response::Error("Unrecognized state value: " + in_state);
        }

        m_debugger->SetBreakOnException(attributes);
        return Response::OK();
    }

    Response DebuggerImpl::evaluateOnCallFrame(
        const String & in_callFrameId,
        const String & in_expression,
        Maybe<String> in_objectGroup,
        Maybe<bool> /*in_includeCommandLineAPI*/,
        Maybe<bool> /*in_silent*/,
        Maybe<bool> in_returnByValue,
        Maybe<bool> /*in_generatePreview*/,
        std::unique_ptr<protocol::Runtime::RemoteObject>* out_result,
        Maybe<protocol::Runtime::ExceptionDetails>* out_exceptionDetails)
    {
        auto parsedId = ProtocolHelpers::ParseObjectId(in_callFrameId);

        int ordinal = 0;
        if (parsedId->getInteger(PropertyHelpers::Names::Ordinal, &ordinal))
        {
            auto callFrame = m_debugger->GetCallFrame(ordinal);

            std::unique_ptr<ExceptionDetails> exceptionDetails;
            *out_result = callFrame.Evaluate(in_expression, in_returnByValue.fromMaybe(false), &exceptionDetails);

            if (exceptionDetails != nullptr)
            {
                *out_exceptionDetails = std::move(exceptionDetails);
            }

            return Response::OK();
        }

        return Response::Error(c_ErrorCallFrameInvalidId);
    }

    Response DebuggerImpl::setVariableValue(
        int /*in_scopeNumber*/,
        const String & /*in_variableName*/,
        std::unique_ptr<protocol::Runtime::CallArgument> /*in_newValue*/,
        const String & /*in_callFrameId*/)
    {
        return Response::Error(c_ErrorNotImplemented);
    }

    Response DebuggerImpl::setAsyncCallStackDepth(int /*in_maxDepth*/)
    {
        return Response::Error(c_ErrorNotImplemented);
    }

    Response DebuggerImpl::setBlackboxPatterns(std::unique_ptr<Array<String>> in_patterns)
    {
        return Response::Error(c_ErrorNotImplemented);
    }

    Response DebuggerImpl::setBlackboxedRanges(
        const String & /*in_scriptId*/,
        std::unique_ptr<Array<protocol::Debugger::ScriptPosition>> /*in_positions*/)
    {
        return Response::Error(c_ErrorNotImplemented);
    }

    void DebuggerImpl::SourceEventHandler(const DebuggerScript& script, bool success, void* callbackState)
    {
        const auto debuggerImpl = static_cast<DebuggerImpl*>(callbackState);
        debuggerImpl->HandleSourceEvent(script, success);
    }

    SkipPauseRequest DebuggerImpl::BreakEventHandler(const DebuggerBreak& breakInfo, void* callbackState)
    {
        const auto debuggerImpl = static_cast<DebuggerImpl*>(callbackState);
        return debuggerImpl->HandleBreakEvent(breakInfo);
    }

    void DebuggerImpl::ResumeEventHandler(void* callbackState)
    {
        const auto debuggerImpl = static_cast<DebuggerImpl*>(callbackState);
        return debuggerImpl->HandleResumeEvent();
    }

    bool DebuggerImpl::IsEnabled()
    {
        return m_isEnabled;
    }

    void DebuggerImpl::HandleSourceEvent(const DebuggerScript& script, bool success)
    {
        String16 scriptId = script.ScriptId();
        String16 scriptUrl = script.SourceUrl();

        std::unique_ptr<protocol::DictionaryValue> executionContextAuxData;
        if (!script.ExecutionContextAuxData().empty())
        {
            executionContextAuxData = protocol::DictionaryValue::cast(
                protocol::StringUtil::parseJSON(script.ExecutionContextAuxData()));
        }

        if (success)
        {
            m_frontend.scriptParsed(
                scriptId,
                scriptUrl,
                script.StartLine(),
                script.StartColumn(),
                script.EndLine(),
                script.EndColumn(),
                script.ExecutionContextId(),
                script.Hash(),
                std::move(executionContextAuxData),
                script.IsLiveEdit(),
                script.SourceMappingUrl(),
                script.HasSourceUrl());
        }
        else
        {
            m_frontend.scriptFailedToParse(
                scriptId,
                scriptUrl,
                script.StartLine(),
                script.StartColumn(),
                script.EndLine(),
                script.EndColumn(),
                script.ExecutionContextId(),
                script.Hash(),
                std::move(executionContextAuxData),
                script.SourceMappingUrl(),
                script.HasSourceUrl());
        }

        m_scriptMap.emplace(scriptId, script);

        for (auto& breakpoint : m_breakpointMap)
        {
            if (breakpoint.second.TryLoadScript(script))
            {
                if (TryResolveBreakpoint(breakpoint.second))
                {
                    m_frontend.breakpointResolved(
                        breakpoint.first,
                        breakpoint.second.GetActualLocation());
                }
            }
        }
    }

    SkipPauseRequest DebuggerImpl::EvaluateConditionOnBreakpoint(int bpId)
    {
        if (bpId < 0)
        {
            return SkipPauseRequest::RequestNoSkip;
        }

        DebuggerBreakpoint *bp = nullptr;
        auto it = m_breakpointMap.begin();
        while (it != m_breakpointMap.end())
        {
            if (it->second.GetActualId() == bpId)
            {
                bp = &it->second;
                break;
            }
            it++;
        }

        if (bp == nullptr)
        {
            return SkipPauseRequest::RequestNoSkip;
        }

        try
        {
            String condition = bp->GetCondition();
            if (condition.length() != 0)
            {
                DebuggerCallFrame callFrame = m_debugger->GetCallFrame(0);
                std::unique_ptr<ExceptionDetails> exceptionDetails;
                JsValueRef expressionStr = JS_INVALID_REFERENCE;
                if (JsCreateStringUtf16(condition.characters16(), condition.length(), &expressionStr) == JsNoError)
                {
                    JsValueRef evalResult = JS_INVALID_REFERENCE;
                    JsErrorCode err = JsDiagEvaluate(expressionStr, 0, JsParseScriptAttributeNone, true, &evalResult);

                    // If the condition is provided, the debugger will stop only when the expression is evaluated to true
                    if (err == JsNoError && PropertyHelpers::GetPropertyBoolConvert(evalResult, PropertyHelpers::Names::Value))
                    {
                        return SkipPauseRequest::RequestNoSkip;
                    }
                }
                return SkipPauseRequest::RequestContinue;
            }
        }
        catch (const JsErrorException&)
        {
            // Ignoring the exception occurred on condition expression evaluation. May be there is a way to express this on debugger.
        }

        return SkipPauseRequest::RequestNoSkip;
    }

    SkipPauseRequest DebuggerImpl::HandleBreakEvent(const DebuggerBreak& breakInfo)
    {
        SkipPauseRequest request = SkipPauseRequest::RequestNoSkip;

        if (m_shouldSkipAllPauses)
        {
            request = SkipPauseRequest::RequestContinue;
        }
        else
        {
            request = EvaluateConditionOnBreakpoint(breakInfo.GetHitBreakpoint());
        }

        if (request != SkipPauseRequest::RequestNoSkip)
        {
            return request;
        }

        auto callFrames = Array<CallFrame>::create();

        for (const DebuggerCallFrame& callFrame : m_debugger->GetCallFrames())
        {
            callFrames->addItem(callFrame.ToProtocolValue());
        }

        m_frontend.paused(
            std::move(callFrames),
            breakInfo.GetReason(),
            breakInfo.GetData(),
            breakInfo.GetHitBreakpoints(),
            breakInfo.GetAsyncStackTrace());

        return request;
    }

    void DebuggerImpl::HandleResumeEvent()
    {
        m_frontend.resumed();
    }

    bool DebuggerImpl::ActualBreakpointExists(DebuggerBreakpoint& breakpoint)
    {
        for (auto &it : m_breakpointMap)
        {
            if (breakpoint.GetActualId() >= 0)
            {
                // breakpoint set in engine - compare by engine ID
                if (it.second.GetActualId() == breakpoint.GetActualId())
                    return true;
            }
            else
            {
                // breakpoint not set in engine - compare by nominal location
                if (it.second.GetScriptId() == breakpoint.GetScriptId()
                    && it.second.GetLineNumber() == breakpoint.GetLineNumber()
                    && it.second.GetColumnNumber() == breakpoint.GetColumnNumber())
                    return true;
            }
        }

        return false;
    }

    bool DebuggerImpl::TryResolveBreakpoint(DebuggerBreakpoint& breakpoint)
    {
        if (!breakpoint.IsScriptLoaded())
        {
            throw std::runtime_error(c_ErrorScriptMustBeLoaded);
        }

        m_debugger->SetBreakpoint(breakpoint);

        if (!breakpoint.IsResolved())
        {
            return false;
        }

        return true;
    }
}
