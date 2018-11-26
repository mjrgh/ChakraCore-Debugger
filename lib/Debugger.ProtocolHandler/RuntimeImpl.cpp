// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "stdafx.h"
#include "RuntimeImpl.h"

#include "PropertyHelpers.h"
#include "ProtocolHandler.h"
#include "ProtocolHelpers.h"

#include <StringUtil.h>

namespace JsDebug
{
    using protocol::Array;
    using protocol::DictionaryValue;
    using protocol::FrontendChannel;
    using protocol::Maybe;
    using protocol::Response;
    using protocol::Runtime::CallArgument;
    using protocol::Runtime::ExceptionDetails;
    using protocol::Runtime::InternalPropertyDescriptor;
    using protocol::Runtime::PropertyDescriptor;
    using protocol::String;
    using protocol::StringUtil;
    using protocol::Value;

    namespace
    {
        const char c_ErrorInvalidObjectId[] = "Invalid object ID";
        const char c_ErrorNotEnabled[] = "Runtime is not enabled";
        const char c_ErrorNotImplemented[] = "Not implemented";
        const char c_ErrorScriptParse[] = "Script parse failed";
    }

    RuntimeImpl::RuntimeImpl(ProtocolHandler* handler, FrontendChannel* frontendChannel, Debugger* debugger)
        : m_handler(handler)
        , m_frontend(frontendChannel)
        , m_debugger(debugger)
        , m_isEnabled(false)
    {
    }

    RuntimeImpl::~RuntimeImpl()
    {
    }

    void RuntimeImpl::evaluate(
        const String& expr,
        Maybe<String> /*in_objectGroup*/,
        Maybe<bool> /*in_includeCommandLineAPI*/,
        Maybe<bool> /*in_silent*/,
        Maybe<int> /*in_contextId*/,
        Maybe<bool> /*in_returnByValue*/,
        Maybe<bool> /*in_generatePreview*/,
        Maybe<bool> /*in_userGesture*/,
        Maybe<bool> awaitPromise,
        Maybe<bool> throwOnSideEffect,
        std::unique_ptr<EvaluateCallback> callback)
    {
        // If "throw on side effect" is true, throw.  We can't rule out an error.
        if (throwOnSideEffect.fromMaybe(false))
        {
            auto remote0 = protocol::Runtime::RemoteObject::create();
            auto remote = &remote0.setType("error")
                .setClassName("error")
                .setDescription("Eval Error") 
                .setSubtype("error");

            auto details0 = ExceptionDetails::create();
            auto details = &details0.setLineNumber(-1)
                .setColumnNumber(-1)
                .setExceptionId(0)
                .setText("Possible side effects of expression evaluation");

            callback->sendSuccess(remote->build(), details->build());
            return;
        }

        // if "await promise" is set, fail
        if (awaitPromise.fromMaybe(false))
        {
            callback->sendFailure(Response::Error(c_ErrorNotImplemented));
            return;
        }

        // evaluate the expression
        JsValueRef exprval, result;
        JsErrorCode err;
        if ((err = JsPointerToString(expr.wchars(), expr.length(), &exprval)) != JsNoError
            || (err = JsDiagEvaluate(exprval, 0, JsParseScriptAttributeNone, false, &result)) != JsNoError)
        {
            callback->sendFailure(Response::Error(c_ErrorScriptParse));
            return;
        }

        // return the result
        callback->sendSuccess(ProtocolHelpers::WrapObject(result), Maybe<ExceptionDetails>());
    }

    void RuntimeImpl::awaitPromise(
        const String& /*in_promiseObjectId*/,
        Maybe<bool> /*in_returnByValue*/,
        Maybe<bool> /*in_generatePreview*/,
        std::unique_ptr<AwaitPromiseCallback> callback)
    {
        callback->sendFailure(Response::Error(c_ErrorNotImplemented));
    }

    void RuntimeImpl::callFunctionOn(
        const String& /*in_objectId*/,
        const String& /*in_functionDeclaration*/,
        Maybe<Array<CallArgument>> /*in_arguments*/,
        Maybe<bool> /*in_silent*/,
        Maybe<bool> /*in_returnByValue*/,
        Maybe<bool> /*in_generatePreview*/,
        Maybe<bool> /*in_userGesture*/,
        Maybe<bool> /*in_awaitPromise*/,
        std::unique_ptr<CallFunctionOnCallback> callback)
    {
        callback->sendFailure(Response::Error(c_ErrorNotImplemented));
    }

    Response RuntimeImpl::getProperties(
        const String& in_objectId,
        Maybe<bool> /*in_ownProperties*/,
        Maybe<bool> in_accessorPropertiesOnly,
        Maybe<bool> /*in_generatePreview*/,
        std::unique_ptr<Array<PropertyDescriptor>>* out_result,
        Maybe<Array<InternalPropertyDescriptor>>* out_internalProperties,
        Maybe<ExceptionDetails>* /*out_exceptionDetails*/)
    {
        if (in_accessorPropertiesOnly.fromMaybe(false))
        {
            // We don't support accessorPropertiesOnly queries, just return an empty list.
            *out_result = Array<PropertyDescriptor>::create();
            return Response::OK();
        }

        auto parsedId = ProtocolHelpers::ParseObjectId(in_objectId);

        int handle = 0;
        int ordinal = 0;
        String name;

        if (parsedId->getInteger(PropertyHelpers::Names::Handle, &handle))
        {
            DebuggerObject obj = m_debugger->GetObjectFromHandle(handle);
            *out_result = obj.GetPropertyDescriptors();
            *out_internalProperties = obj.GetInternalPropertyDescriptors();

            return Response::OK();
        }
        else if (parsedId->getInteger(PropertyHelpers::Names::Ordinal, &ordinal) &&
                 parsedId->getString(PropertyHelpers::Names::Name, &name))
        {
            DebuggerCallFrame callFrame = m_debugger->GetCallFrame(ordinal);

            if (name == PropertyHelpers::Names::Locals)
            {
                DebuggerLocalScope obj = callFrame.GetLocals();
                *out_result = obj.GetPropertyDescriptors();
                *out_internalProperties = obj.GetInternalPropertyDescriptors();

                return Response::OK();
            }
            else if (name == PropertyHelpers::Names::Globals)
            {
                DebuggerObject obj = callFrame.GetGlobals();
                *out_result = obj.GetPropertyDescriptors();
                *out_internalProperties = obj.GetInternalPropertyDescriptors();

                return Response::OK();
            }
        }

        return Response::Error(c_ErrorInvalidObjectId);
    }

    Response RuntimeImpl::releaseObject(const String& /*in_objectId*/)
    {
        return Response::Error(c_ErrorNotImplemented);
    }

    Response RuntimeImpl::releaseObjectGroup(const String& /*in_objectGroup*/)
    {
        return Response::Error(c_ErrorNotImplemented);
    }

    Response RuntimeImpl::runIfWaitingForDebugger()
    {
        if (!IsEnabled())
        {
            return Response::Error(c_ErrorNotEnabled);
        }

        m_handler->RunIfWaitingForDebugger();
        return Response::OK();
    }

    Response RuntimeImpl::enable()
    {
        if (m_isEnabled)
        {
            return Response::OK();
        }

        m_isEnabled = true;

        // Create a default execution context
        auto desc0 = protocol::Runtime::ExecutionContextDescription::create();
        auto desc = &desc0.setId(1).setOrigin("default").setName("default");
        m_frontend.executionContextCreated(desc->build());
        
        // TODO: Other setup

        return Response::OK();
    }

    Response RuntimeImpl::disable()
    {
        if (!m_isEnabled)
        {
            return Response::OK();
        }

        m_isEnabled = false;
        // TODO: Do other cleanup

        return Response::OK();
    }

    Response RuntimeImpl::discardConsoleEntries()
    {
        return Response::Error(c_ErrorNotImplemented);
    }

    Response RuntimeImpl::setCustomObjectFormatterEnabled(bool /*in_enabled*/)
    {
        return Response::Error(c_ErrorNotImplemented);
    }

    Response RuntimeImpl::compileScript(
        const String& expr,
        const String& sourceURL,
        bool persistScript,
        Maybe<int> /*in_executionContextId*/,
        Maybe<String>* /*out_scriptId*/,
        Maybe<ExceptionDetails>* exceptionDetails)
    {
        // we don't implement persisting the script (yet)
        if (persistScript)
            return Response::Error(c_ErrorNotImplemented);

        // parse the script
        JsValueRef func;
        JsErrorCode err = JsParseScript(expr.wchars(), 0, sourceURL.wchars(), &func);

        // If that succeeded, return success.  We weren't asked to persist the script,
        // so no additional details are required.
        if (err == JsNoError)
            return Response::OK();

        // if a script parsing error occurred, retrieve the exception data
        bool hasExc;
        JsValueRef excval;
        if (JsHasException(&hasExc) == JsNoError && JsGetAndClearExceptionWithMetadata(&excval) == JsNoError)
        {
            auto details0 = ExceptionDetails::create();
            auto details = &details0.setColumnNumber(PropertyHelpers::GetPropertyInt(excval, "column"))
                .setLineNumber(PropertyHelpers::GetPropertyInt(excval, "line"))
                .setException(ProtocolHelpers::WrapValue(excval))
                .setExceptionId(0)
                .setText(PropertyHelpers::GetPropertyString(PropertyHelpers::GetProperty(excval, "exception"), "message"));

            *exceptionDetails = details->build();
        }

        // no exception information available - fail
        return Response::Error(c_ErrorScriptParse);
    }

    void RuntimeImpl::runScript(
        const String& /*in_scriptId*/,
        Maybe<int> /*in_executionContextId*/,
        Maybe<String> /*in_objectGroup*/,
        Maybe<bool> /*in_silent*/,
        Maybe<bool> /*in_includeCommandLineAPI*/,
        Maybe<bool> /*in_returnByValue*/,
        Maybe<bool> /*in_generatePreview*/,
        Maybe<bool> /*in_awaitPromise*/,
        std::unique_ptr<RunScriptCallback> callback)
    {
        callback->sendFailure(Response::Error(c_ErrorNotImplemented));
    }

    bool RuntimeImpl::IsEnabled()
    {
        return m_isEnabled;
    }

    void RuntimeImpl::consoleAPIEvent(const char* type, const JsValueRef* argv, unsigned short argc)
    {
        auto argArray = protocol::Array<protocol::Runtime::RemoteObject>::create();
        protocol::ErrorSupport es;
        for (unsigned short i = 0; i < argc; ++i)
            argArray->addItem(ProtocolHelpers::WrapValue(argv[i]));
        m_frontend.consoleAPICalled(type, std::move(argArray), 0, 0);
    }

}
