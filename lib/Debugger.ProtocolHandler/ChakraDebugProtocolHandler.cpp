// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "stdafx.h"
#include "ChakraDebugProtocolHandler.h"
#include "ProtocolHandler.h"
#include "TranslateExceptionToJsErrorCode.h"

CHAKRA_API JsDebugProtocolHandlerCreate(JsRuntimeHandle runtime, JsDebugProtocolHandler* protocolHandler)
{
    if (protocolHandler == nullptr)
    {
        return JsErrorInvalidArgument;
    }

    return JsDebug::TranslateExceptionToJsErrorCode(
        [&]() -> void
        {
            auto instance = std::make_unique<JsDebug::ProtocolHandler>(runtime);

            // Release ownership of the pointer
            *protocolHandler = reinterpret_cast<JsDebugProtocolHandler>(instance.release());
        });
}

CHAKRA_API JsDebugProtocolHandlerDestroy(JsDebugProtocolHandler protocolHandler)
{
    return JsDebug::TranslateExceptionToJsErrorCode<JsDebug::ProtocolHandler*>(
        protocolHandler,
        [&](JsDebug::ProtocolHandler* instance) -> void
        {
            // Take ownership of the pointer so that it gets released at the exit of the function.
            auto holder = std::unique_ptr<JsDebug::ProtocolHandler>(instance);
        });
}

CHAKRA_API JsDebugProtocolHandlerConnect(
    JsDebugProtocolHandler protocolHandler,
    bool breakOnNextLine,
    JsDebugProtocolHandlerSendResponseCallback callback,
    void* callbackState)
{
    return JsDebug::TranslateExceptionToJsErrorCode<JsDebug::ProtocolHandler*>(
        protocolHandler,
        [&](JsDebug::ProtocolHandler* instance) -> void
        {
            instance->Connect(breakOnNextLine, callback, callbackState);
        });
}

CHAKRA_API JsDebugProtocolHandlerDisconnect(JsDebugProtocolHandler protocolHandler)
{
    return JsDebug::TranslateExceptionToJsErrorCode<JsDebug::ProtocolHandler*>(
        protocolHandler,
        [&](JsDebug::ProtocolHandler* instance) -> void
        {
            instance->Disconnect();
        });
}

CHAKRA_API JsDebugProtocolHandlerSendCommand(JsDebugProtocolHandler protocolHandler, const char* command)
{
    return JsDebug::TranslateExceptionToJsErrorCode<JsDebug::ProtocolHandler*>(
        protocolHandler,
        [&](JsDebug::ProtocolHandler* instance) -> void
        {
            instance->SendCommand(command);
        });
}

CHAKRA_API JsDebugProtocolHandlerSendRequest(_In_ JsDebugProtocolHandler protocolHandler, _In_ const char* request)
{
    return JsDebug::TranslateExceptionToJsErrorCode<JsDebug::ProtocolHandler*>(
        protocolHandler,
        [&](JsDebug::ProtocolHandler* instance) -> void
    {
        instance->SendRequest(request);
    });
}

CHAKRA_API JsDebugConsoleAPIEvent(_In_ JsDebugProtocolHandler protocolHandler, _In_z_ const char* type, _In_ const JsValueRef* argv, _In_ unsigned short argc)
{
    return JsDebug::TranslateExceptionToJsErrorCode<JsDebug::ProtocolHandler*>(
        protocolHandler,
        [&](JsDebug::ProtocolHandler* instance) -> void
    {
        instance->ConsoleAPIEvent(type, argv, argc);
    });
}

CHAKRA_API JsDebugProtocolHandlerWaitForDebugger(JsDebugProtocolHandler protocolHandler)
{
    return JsDebug::TranslateExceptionToJsErrorCode<JsDebug::ProtocolHandler*>(
        protocolHandler,
        [&](JsDebug::ProtocolHandler* instance) -> void
        {
            instance->WaitForDebugger();
        });
}

CHAKRA_API JsDebugProtocolHandlerProcessCommandQueue(JsDebugProtocolHandler protocolHandler)
{
    return JsDebug::TranslateExceptionToJsErrorCode<JsDebug::ProtocolHandler*>(
        protocolHandler,
        [&](JsDebug::ProtocolHandler* instance) -> void
        {
            instance->ProcessCommandQueue();
        });
}

CHAKRA_API JsDebugProtocolHandlerSetCommandQueueCallback(
    JsDebugProtocolHandler protocolHandler,
    JsDebugProtocolHandlerCommandQueueCallback callback,
    void* callbackState)
{
    return JsDebug::TranslateExceptionToJsErrorCode<JsDebug::ProtocolHandler*>(
        protocolHandler,
        [&](JsDebug::ProtocolHandler* instance) -> void
        {
            instance->SetCommandQueueCallback(callback, callbackState);
        });
}

CHAKRA_API JsDebugProtocolHandlerCreateConsoleObject(
    _In_ JsDebugProtocolHandler protocolHandler,
    _Out_ JsValueRef *consoleObject
)
{
    return JsDebug::TranslateExceptionToJsErrorCode<JsDebug::ProtocolHandler*>(
        protocolHandler,
        [&](JsDebug::ProtocolHandler* instance) -> void
    {
        *consoleObject = instance->CreateConsoleObject();
    });
}
