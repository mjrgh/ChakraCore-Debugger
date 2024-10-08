// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "stdafx.h"
#include "ChakraDebugProtocolHandler.h"
#include "ProtocolHandler.h"
#include <cassert>

namespace JsDebug
{
    using protocol::Array;
    using protocol::Schema::Domain;
    using protocol::Serializable;

    namespace
    {
        const char c_ErrorCallbackRequired[] = "'callback' is required";
        const char c_ErrorCommandRequired[] = "'command' is required";
        const char c_ErrorRuntimeRequired[] = "'runtime' is required";
        const char c_ErrorHandlerAlreadyConnected[] = "Handler is already connected";
        const char c_ErrorInvalidCallbackState[] = "'callbackState' can only be provided with a valid callback";
        const char c_ErrorNoHandlerConnected[] = "No handler is currently connected";
    }

    ProtocolHandler::ProtocolHandler(JsRuntimeHandle runtime)
        : m_sendResponseCallback(nullptr)
        , m_sendResponseCallbackState(nullptr)
        , m_commandQueueCallback(nullptr)
        , m_commandQueueCallbackState(nullptr)
        , m_consoleHandler(this)
        , m_isConnected(false)
        , m_waitingForDebugger(false)
        , m_breakOnConnect(false)
        , m_dispatcher(this)
        , m_startupState(StartupState::Running)
        , m_deferredGo(false)
        , m_processingCommandQueue(false)
    {
        if (runtime == nullptr) {
            throw JsErrorException(JsErrorInvalidArgument, c_ErrorRuntimeRequired);
        }

        m_debugger = std::make_unique<Debugger>(this, runtime);
#if DBG
        m_consoleObjectCount = 0;
#endif

    }

    ProtocolHandler::~ProtocolHandler()
    {
    }

    void ProtocolHandler::Connect(
        bool breakOnNextLine,
        ProtocolHandlerSendResponseCallback callback,
        void* callbackState)
    {
        if (callback == nullptr)
        {
            throw JsErrorException(JsErrorInvalidArgument, c_ErrorCallbackRequired);
        }

        {
            std::unique_lock<std::mutex> lock(m_lock);

            if (m_sendResponseCallback != nullptr)
            {
                throw std::runtime_error(c_ErrorHandlerAlreadyConnected);
            }

            m_sendResponseCallback = callback;
            m_sendResponseCallbackState = callbackState;
            m_breakOnConnect = breakOnNextLine;
            m_startupState = breakOnNextLine ? StartupState::Pause : StartupState::Continue;

            EnqueueCommand(CommandType::Connect);
        }

        m_debugger->RequestAsyncBreak();
    }

    void ProtocolHandler::Disconnect()
    {
        {
            std::unique_lock<std::mutex> lock(m_lock);

            if (m_sendResponseCallback == nullptr)
            {
                throw std::runtime_error(c_ErrorNoHandlerConnected);
            }

            m_sendResponseCallback = nullptr;
            m_sendResponseCallbackState = nullptr;
            m_breakOnConnect = false;

            EnqueueCommand(CommandType::Disconnect);
        }

        m_debugger->RequestAsyncBreak();
    }

    void ProtocolHandler::SendCommand(const char* command)
    {
        if (command == nullptr)
        {
            throw JsErrorException(JsErrorInvalidArgument, c_ErrorCommandRequired);
        }

#if defined(_DEBUG) && 0
        OutputDebugStringA("{\"type\":\"request\",\"payload\":");
        OutputDebugStringA(command);
        OutputDebugStringA("},\r\n");
#endif

        ProtocolHandlerCommandQueueCallback callback = nullptr;
        void* state = nullptr;

        {
            std::unique_lock<std::mutex> lock(m_lock);
            EnqueueCommand(CommandType::MessageReceived, command);

            callback = m_commandQueueCallback;
            state = m_commandQueueCallbackState;
        }
        
        // Trigger a debugger break
        m_debugger->RequestAsyncBreak();

        if (callback != nullptr)
        {
            // Notify the host
            callback(state);
        }
    }

    void ProtocolHandler::SendRequest(const char* request)
    {
        ProtocolHandlerCommandQueueCallback callback = nullptr;
        void* state = nullptr;

        {
            std::unique_lock<std::mutex> lock(m_lock);
            EnqueueCommand(CommandType::HostRequest, request);

            callback = m_commandQueueCallback;
            state = m_commandQueueCallbackState;
        }

        // Trigger a debugger break
        m_debugger->RequestAsyncBreak();
    }

    void ProtocolHandler::ConsoleAPIEvent(const char* type, const JsValueRef* argv, unsigned short argc)
    {
        if (m_runtimeAgent != nullptr)
            m_runtimeAgent->consoleAPIEvent(type, argv, argc);
    }


    void ProtocolHandler::WaitForDebugger()
    {
        m_waitingForDebugger = true;
        ProcessCommandQueue();
    }

    void ProtocolHandler::RunIfWaitingForDebugger()
    {
        if (m_startupState == StartupState::Pause)
        {
            m_debugger->PauseOnNextStatement();
        }

        m_waitingForDebugger = false;
    }

    void ProtocolHandler::Continue() 
    {
        m_waitingForDebugger = false;
        m_startupState = StartupState::Running;
    }
    
    JsValueRef ProtocolHandler::CreateConsoleObject()
    {
        JsContextRef currentContext = JS_INVALID_REFERENCE;
        IfJsErrorThrow(JsGetCurrentContext(&currentContext));
        if (currentContext == JS_INVALID_REFERENCE)
        {
            throw JsErrorException(JsErrorNoCurrentContext);
        }
#if DBG
        m_consoleObjectCount++;
#endif
        return m_consoleHandler.CreateConsoleObject();
    }

    void ProtocolHandler::ConsoleAPICalled(protocol::String& apiType, JsValueRef *arguments, size_t argumentCount)
    {
        if (m_isConnected)
        {
            m_runtimeAgent->consoleAPICalled(apiType, arguments, argumentCount);
        }
    }

    std::unique_ptr<Array<Domain>> ProtocolHandler::GetSupportedDomains()
    {
        auto domains = Array<Domain>::create();

        domains->addItem(Domain::create()
            .setName(protocol::Console::Metainfo::domainName)
            .setVersion(protocol::Console::Metainfo::version)
            .build());

        domains->addItem(Domain::create()
            .setName(protocol::Debugger::Metainfo::domainName)
            .setVersion(protocol::Debugger::Metainfo::version)
            .build());

        domains->addItem(Domain::create()
            .setName(protocol::Runtime::Metainfo::domainName)
            .setVersion(protocol::Runtime::Metainfo::version)
            .build());

        return domains;
    }

    void ProtocolHandler::sendProtocolResponse(int /*callId*/, std::unique_ptr<Serializable> message)
    {
        sendProtocolNotification(std::move(message));
    }

    void ProtocolHandler::sendProtocolNotification(std::unique_ptr<Serializable> message)
    {
        protocol::String str = message->serialize();

#if defined(_DEBUG) && 0
        static_assert(sizeof(wchar_t) == sizeof(uint16_t));
        OutputDebugStringA("{\"type\":\"response\",\"payload\":");
        OutputDebugStringW(reinterpret_cast<const wchar_t*>(str.characters16()));
        OutputDebugStringA("},\r\n");
#endif

        std::string utf8Str = str.toUtf8();
        SendResponse(utf8Str.c_str());
    }

    void ProtocolHandler::flushProtocolNotifications()
    {
    }

    void ProtocolHandler::ProcessCommandQueue()
    {
        // don't enter recursively
        if (m_processingCommandQueue)
            return;

        struct RecurseFlag
        {
            RecurseFlag(ProtocolHandler *self) : self(self) { self->m_processingCommandQueue = true; }
            ~RecurseFlag() { self->m_processingCommandQueue = false; }
            ProtocolHandler *self;
        } recurseFlag(this);

        // Ensure that there's an active context before trying to process the queue.
        DebuggerContext::Scope debuggerScope(*m_debugger->GetDebugContext());

        std::vector<std::pair<CommandType, std::string>> current;

        do
        {
            current.clear();

            {
                std::unique_lock<std::mutex> lock(m_lock);

                if (m_waitingForDebugger && m_commandQueue.empty())
                {
                    m_commandWaiting.wait(lock);
                }

                std::swap(m_commandQueue, current);
            }

            for (const auto& command : current)
            {
                switch (command.first)
                {
                case CommandType::Connect:
                    HandleConnect();
                    break;

                case CommandType::Disconnect:
                    HandleDisconnect();
                    break;

                case CommandType::MessageReceived:
                    HandleMessageReceived(command.second);
                    break;

                case CommandType::HostRequest:
                    HandleHostRequest(command.second);
                    break;

                default:
                    throw std::runtime_error("Unknown command type");
                }
            }

        } while (m_waitingForDebugger || !current.empty());
    }

    void ProtocolHandler::EnqueueCommand(ProtocolHandler::CommandType type, const std::string& message)
    {
        m_commandQueue.emplace_back(type, message);
        m_commandWaiting.notify_all();
    }

    void ProtocolHandler::SendResponse(const char* response)
    {
        if (m_sendResponseCallback != nullptr)
        {
            m_sendResponseCallback(response, m_sendResponseCallbackState);
        }
    }

    void ProtocolHandler::HandleConnect()
    {
        if (m_isConnected)
        {
            throw std::runtime_error("Already connected");
        }

        m_consoleAgent = std::make_unique<ConsoleImpl>(this, this);
        protocol::Console::Dispatcher::wire(&m_dispatcher, m_consoleAgent.get());

        m_debuggerAgent = std::make_unique<DebuggerImpl>(this, this, m_debugger.get());
        protocol::Debugger::Dispatcher::wire(&m_dispatcher, m_debuggerAgent.get());

        m_runtimeAgent = std::make_unique<RuntimeImpl>(this, this, m_debugger.get());
        protocol::Runtime::Dispatcher::wire(&m_dispatcher, m_runtimeAgent.get());

        m_schemaAgent = std::make_unique<SchemaImpl>(this, this);
        protocol::Schema::Dispatcher::wire(&m_dispatcher, m_schemaAgent.get());

        m_debugger->PauseOnNextStatement();

        m_isConnected = true;
    }

    void ProtocolHandler::HandleDisconnect()
    {
        if (!m_isConnected)
        {
            throw std::runtime_error("Not currently connected");
        }

        m_consoleAgent.reset();
        m_debuggerAgent.reset();
        m_runtimeAgent.reset();
        m_schemaAgent.reset();

        RunIfWaitingForDebugger();
        m_isConnected = false;
    }

    void ProtocolHandler::HandleMessageReceived(const std::string& message)
    {
        protocol::String messageStr = protocol::String::fromUtf8(message.c_str(), message.length());
        m_dispatcher.dispatch(protocol::StringUtil::parseJSON(messageStr));
    }

    void ProtocolHandler::HandleHostRequest(const std::string& request)
    {
        if (request == "Debugger.go")
        {
            m_debugger->Go();
        }
        else if (request == "Debugger.deferredGo")
        {
            m_deferredGo = true;
        }
        else if (request == "Debugger.stepInto")
        {
            m_debugger->StepIn();
        }
        else if (request == "Console.log")
        {
            // to do
        }
    }

    void ProtocolHandler::ProcessDeferredGo()
    {
        if (m_deferredGo)
        {
            m_deferredGo = false;
            SendRequest("Debugger.go");
        }
    }

    void ProtocolHandler::SetCommandQueueCallback(ProtocolHandlerCommandQueueCallback callback, void* callbackState)
    {
        if (callback == nullptr && callbackState != nullptr)
        {
            throw JsErrorException(JsErrorInvalidArgument, c_ErrorInvalidCallbackState);
        }

        {
            std::unique_lock<std::mutex> lock(m_lock);

            m_commandQueueCallback = callback;
            m_commandQueueCallbackState = callbackState;
        }
    }
}
