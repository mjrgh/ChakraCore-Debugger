// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#pragma once

#include "ServiceHandler.h"

namespace JsDebug
{
    class Service
    {
    public:
        Service();
        ~Service();

        void RegisterHandler(const char* id, JsDebugProtocolHandler protocolHandler, bool breakOnNextLine);
        void UnregisterHandler(const char* id);

        void SetServiceName(const char* name, const char* description);
        void SetFavIcon(const char* url);

        void Listen(uint16_t port);
        void Close();

    private:
        bool OnValidate(websocketpp::connection_hdl hdl);
        void OnHttpRequest(websocketpp::connection_hdl hdl);

        void HandleListRequest(websocketpp::connection_hdl hdl);
        void HandleProtocolRequest(websocketpp::connection_hdl hdl);
        void HandleVersionRequest(websocketpp::connection_hdl hdl);

        void SendHttpJsonResponse(websocketpp::connection_hdl hdl, const std::string& jsonBody);

        typedef std::map<std::string, std::unique_ptr<ServiceHandler>> handler_map;

        // Although access to the server object is thread-safe, access to all other objects is not. The lock must be
        // taken before accessing any class members from either thread.
        websocketpp::server<websocketpp::config::asio> m_server;
        websocketpp::lib::thread m_thread;
        websocketpp::lib::mutex m_lock;

        uint16_t m_port;
        handler_map m_handlers;

        std::string m_serviceName;
        std::string m_serviceDesc;
        std::string m_favIconUrl;

        std::string m_chakraCoreVersion;
    };
}
