// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "stdafx.h"
#include "Service.h"

#include <iostream>

namespace JsDebug
{
    typedef websocketpp::server<websocketpp::config::asio> server;

    using websocketpp::connection_hdl;

    using websocketpp::lib::bind;
    using websocketpp::lib::mutex;
    using websocketpp::lib::placeholders::_1;
    using websocketpp::lib::thread;
    using websocketpp::lib::unique_lock;

    using websocketpp::log::alevel;
    using websocketpp::log::elevel;

    namespace
    {
        const char c_HeaderCacheControlName[] = "Cache-Control";
        const char c_HeaderCacheControlValue[] = "no-cache";
        const char c_HeaderContentTypeName[] = "Content-Type";
        const char c_HeaderContentTypeValue[] = "application/json; charset=UTF-8";
        const char c_LocalHostName[] = "127.0.0.1";
        const char c_ResourceJson[] = "/json";
        const char c_ResourceJsonList[] = "/json/list";
        const char c_ResourceJsonProtocol[] = "/json/protocol";
        const char c_ResourceJsonVersion[] = "/json/version";
    }

    static void GetChakraCoreVersion(std::string &version)
    {
        // set a default
        version = "0.0.0";

        // get the ChakraCore DLL handle
        HMODULE hModule = GetModuleHandleW(L"ChakraCore.dll");
        if (hModule == NULL)
            return;

        // get the DLL path
        TCHAR dllPath[_MAX_PATH];
        GetModuleFileName(hModule, dllPath, _MAX_PATH);

        // "open" file version data
        DWORD vsInfoHandle = 0;
        DWORD vsInfoSize = GetFileVersionInfoSize(dllPath, &vsInfoHandle);
        if (vsInfoSize != 0)
        {
            // load the version info
            std::unique_ptr<BYTE> vsInfo(new BYTE[vsInfoSize]);
            if (GetFileVersionInfo(dllPath, vsInfoHandle, vsInfoSize, vsInfo.get()))
            {
                // retrieve the fixed data portion
                VS_FIXEDFILEINFO *vsFixedInfo = nullptr;
                UINT vsFixedInfoLen;
                if (!VerQueryValueW(vsInfo.get(), L"\\", (LPVOID*)&vsFixedInfo, &vsFixedInfoLen))
                    vsFixedInfo = nullptr;

                // slice and dice the version number for easier consumption by the caller
                if (vsFixedInfo != nullptr)
                {
                    char buf[128];
                    sprintf_s(buf, "%d.%d.%d",
                        (vsFixedInfo->dwProductVersionMS >> 16) & 0xFFFF,
                        vsFixedInfo->dwProductVersionMS & 0xFFFF,
                        (vsFixedInfo->dwProductVersionLS >> 16) & 0xFFFF);

                    version = buf;
                }
            }
        }
    }

    Service::Service()
        : m_port(0)
    {
        // TODO: Enable logging to a file
        m_server.set_error_channels(elevel::none);
        m_server.set_access_channels(alevel::none);

        m_server.init_asio();
        m_server.set_validate_handler(bind(&Service::OnValidate, this, _1));
        m_server.set_http_handler(bind(&Service::OnHttpRequest, this, _1));

        m_serviceName = "ChakraCore Instance";
        m_serviceDesc = "ChakraCore Instance";
        GetChakraCoreVersion(m_chakraCoreVersion);
    }

    Service::~Service()
    {
        try
        {
            // Close any open connections
            Close();
        }
        catch (...)
        {
            // Don't allow the exception to propagate.
        }
    }

    void Service::SetServiceName(const char* name, const char* description)
    {
        m_serviceName = name != nullptr ? name : "";
        m_serviceDesc = description != nullptr ? description : name != nullptr ? name : "";
    }

    void Service::SetFavIcon(const char* url)
    {
        m_favIconUrl = url != nullptr ? url : "";
    }

    void Service::RegisterHandler(const char* id, JsDebugProtocolHandler protocolHandler, bool breakOnNextLine)
    {
        unique_lock<mutex> lock(m_lock);
        m_handlers.emplace(id, std::make_unique<ServiceHandler>(&m_server, id, protocolHandler, breakOnNextLine));
    }

    void Service::UnregisterHandler(const char* id)
    {
        unique_lock<mutex> lock(m_lock);
        m_handlers.erase(id);
    }

    void Service::Listen(uint16_t port)
    {
        m_port = port;

        m_server.listen(c_LocalHostName, std::to_string(m_port));
        m_server.start_accept();

        m_thread = thread(&server::run, &m_server);
    }

    void Service::Close()
    {
        // Stop listening for new connections
        m_server.stop_listening();
        m_port = 0;

        {
            unique_lock<mutex> lock(m_lock);

            for (const auto& handler : m_handlers)
            {
                handler.second->Disconnect();
            }
        }

        // Wait for the thread to exit
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }

    bool Service::OnValidate(connection_hdl hdl)
    {
        auto connection = m_server.get_con_from_hdl(hdl);
        if (connection != nullptr)
        {
            auto resource = connection->get_uri()->get_resource();
            resource.erase(0, 1);

            {
                unique_lock<mutex> lock(m_lock);

                auto handler = m_handlers.find(resource);
                if (handler != m_handlers.end()) {
                    return handler->second->Connect(hdl);
                }
            }
        }

        return false;
    }

    void Service::OnHttpRequest(connection_hdl hdl)
    {
        auto connection = m_server.get_con_from_hdl(hdl);
        if (connection != nullptr)
        {
            auto resource = connection->get_uri()->get_resource();

            if (resource.rfind(c_ResourceJsonProtocol) == 0)
            {
                HandleProtocolRequest(hdl);
            }
            else if (resource.rfind(c_ResourceJsonVersion) == 0)
            {
                HandleVersionRequest(hdl);
            }
            else if (resource.rfind(c_ResourceJsonList) == 0 || resource.rfind(c_ResourceJson) == 0)
            {
                HandleListRequest(hdl);
            }
            else
            {
                connection->set_status(websocketpp::http::status_code::not_found);
            }
        }
    }

    void Service::HandleListRequest(connection_hdl hdl)
    {
        bool first = true;
        std::ostringstream json;
        json << "[";

        {
            unique_lock<mutex> lock(m_lock);

            for (const auto& handler : m_handlers) {
                if (first)
                {
                    first = false;
                }
                else
                {
                    json << ",";
                }

                // TODO: Tweak these values to be more accurate.
                json << " {\n";
                json << "  \"description\": \"" << m_serviceDesc << "\",\n";
                json << "  \"devtoolsFrontendUrl\": " <<
                    "\"chrome-devtools://devtools/bundled/inspector.html?experiments=true&v8only=true&ws=localhost:" <<
                    m_port << "/" << handler.second->Id() << "\",\n";
                if (m_favIconUrl.length() != 0)
                    json << "  \"faviconUrl\": \"" << m_favIconUrl << "\",\n";
                json << "  \"id\": \"" << handler.second->Id() << "\",\n";
                json << "  \"title\": \"" << m_serviceName << "\",\n";
                json << "  \"type\": \"node\",\n";
                json << "  \"url\": \"file://\",\n";
                json << "  \"webSocketDebuggerUrl\": \"ws://localhost:" << m_port << "/" << handler.second->Id() << "\"\n";
                json << "}";
            }
        }

        json << " ]";

        SendHttpJsonResponse(hdl, json.str());
    }

    void Service::HandleProtocolRequest(connection_hdl hdl)
    {
        SendHttpJsonResponse(hdl, "{}");
    }

    void Service::HandleVersionRequest(connection_hdl hdl)
    {
        std::ostringstream json;
        json << "{\n";
        json << "  \"Browser\": \"ChakraCore/v" << m_chakraCoreVersion << "\",\n";
        json << "  \"Protocol-Version\": \"1.2\"\n";
        json << "}";

        // TODO: Can we get the version of ChakraCore?
        SendHttpJsonResponse(hdl, json.str());
    }

    void Service::SendHttpJsonResponse(connection_hdl hdl, const std::string& jsonBody)
    {
        auto connection = m_server.get_con_from_hdl(hdl);
        if (connection != nullptr)
        {
            connection->append_header(c_HeaderContentTypeName, c_HeaderContentTypeValue);
            connection->append_header(c_HeaderCacheControlName, c_HeaderCacheControlValue);
            connection->set_body(jsonBody);
            connection->set_status(websocketpp::http::status_code::ok);
        }
    }
}