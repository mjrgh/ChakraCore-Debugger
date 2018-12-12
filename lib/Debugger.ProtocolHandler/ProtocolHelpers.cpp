// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "stdafx.h"
#include "ProtocolHelpers.h"
#include "PropertyHelpers.h"
#include "ErrorHelpers.h"

namespace JsDebug
{
    using protocol::DictionaryValue;
    using protocol::Debugger::Location;
    using protocol::Runtime::ExceptionDetails;
    using protocol::Runtime::InternalPropertyDescriptor;
    using protocol::Runtime::PropertyDescriptor;
    using protocol::Runtime::RemoteObject;
    using protocol::String;
    using protocol::StringUtil;
    using protocol::Value;

    namespace
    {
        const char c_DefaultExceptionText[] = "Uncaught";
        const char c_ErrorInvalidObjectId[] = "Invalid object ID";
        const char c_ErrorNoDisplayString[] = "No display string found";
        const int c_JsrtDebugPropertyReadOnly = 0x4;

        std::unique_ptr<Value> ToProtocolValue(JsValueRef object)
        {
            JsValueType jstype;
            IfJsErrorThrow(JsGetValueType(object, &jstype));
            switch (jstype)
            {
            case JsUndefined:
                // TO DO - distinguish undefined from null?
                return Value::null();

            case JsNull:
                return Value::null();

            case JsNumber:
                {
                    double d;
                    IfJsErrorThrow(JsNumberToDouble(object, &d));
                    return protocol::FundamentalValue::create(d);
                }

            case JsString:
                {
                    const wchar_t *p;
                    size_t len;
                    IfJsErrorThrow(JsStringToPointer(object, &p, &len));
                    static_assert(sizeof(JsDebug::UChar) == sizeof(wchar_t));
                    protocol::String s(reinterpret_cast<const JsDebug::UChar*>(p), len);
                    return protocol::StringValue::create(s);
                }

            case JsObject:
                // TO DO: populate the object
                {
                    auto l = protocol::DictionaryValue::create();
                    return l;
                }

            case JsBoolean:
                {
                    bool b;
                    IfJsErrorThrow(JsBooleanToBool(object, &b));
                    return protocol::FundamentalValue::create(b);
                }

            case JsFunction:
                // TO DO
                return Value::null();

            case JsArray:
                // TO DO: populate the list
                {
                    auto l = protocol::ListValue::create();
                    return l;
                }

            case JsError:
            case JsSymbol:
            case JsArrayBuffer:
            case JsTypedArray:
            case JsDataView:
            default:
                // TO DO: handle properly?
                return Value::null();
            }
        }

        std::unique_ptr<RemoteObject> CreateObject(JsValueRef object)
        {
            return RemoteObject::create()
                .setType(PropertyHelpers::GetPropertyString(object, PropertyHelpers::Names::Type))
                .build();
        }
    }

    String ProtocolHelpers::GetObjectId(int handle)
    {
        return "{\"handle\":" + String::fromInteger(handle) + "}";
    }

    std::unique_ptr<DictionaryValue> ProtocolHelpers::ParseObjectId(const String& objectId)
    {
        auto parsedValue = StringUtil::parseJSON(objectId);
        if (parsedValue == nullptr || parsedValue->type() != Value::TypeObject)
        {
            throw std::runtime_error(c_ErrorInvalidObjectId);
        }

        return std::unique_ptr<DictionaryValue>(DictionaryValue::cast(parsedValue.release()));
    }

    std::unique_ptr<RemoteObject> ProtocolHelpers::WrapValue(JsValueRef value)
    {
        JsValueRef desc;
        IfJsErrorThrow(JsCreateObject(&desc));
        auto SetProp = [desc](const char *name, JsValueRef val)
        {
            JsPropertyIdRef propid;
            IfJsErrorThrow(JsCreatePropertyId(name, strlen(name), &propid));
            IfJsErrorThrow(JsSetProperty(desc, propid, val, true));
        };
        auto SetStrProp = [desc, &SetProp](const char *name, const wchar_t *str)
        {
            JsValueRef val;
            IfJsErrorThrow(JsPointerToString(str, wcslen(str), &val));
            SetProp(name, val);
        };

        SetProp("value", value);

        JsValueType jstype;
        IfJsErrorThrow(JsGetValueType(value, &jstype));
        const wchar_t *type = L"";
        const wchar_t *display = L"";
        const size_t displayBufMax = 200;
        wchar_t displayBuf[displayBufMax];
        switch (jstype)
        {
        case JsUndefined:
            type = L"undefined";
            display = L"undefined";
            break;

        case JsNull:
            type = L"null";
            display = L"null";
            break;

        case JsNumber:
            type = L"number";
            {
                double d;
                IfJsErrorThrow(JsNumberToDouble(value, &d));
                swprintf_s(displayBuf, L"%.8lf", d);
                display = displayBuf;                
            }
            break;

        case JsString:
            type = L"string";
            {
                const wchar_t *p;
                size_t len;
                IfJsErrorThrow(JsStringToPointer(value, &p, &len));
                display = displayBuf;
                size_t copyLen = min(displayBufMax - 4, len);
                memcpy(displayBuf, p, copyLen * sizeof(wchar_t));
                wcscpy_s(displayBuf + copyLen, 4, copyLen < len ? L"..." : L"");
            }
            break;

        case JsObject:
            type = L"object";
            display = L"{...}";
            break;

        case JsBoolean:
            type = L"boolean";
            {
                bool b;
                IfJsErrorThrow(JsBooleanToBool(value, &b));
                display = b ? L"true" : L"false";
            }
            break;

        case JsFunction:
            type = L"function";
            display = L"f() {...}";
            break;

            
        case JsArray:
            type = L"array";
            display = L"[...]";
            break;

        case JsError:
        case JsSymbol:
        case JsArrayBuffer:
        case JsTypedArray:
        case JsDataView:
            throw std::runtime_error("WrapValue cannot wrap this type");
        }

        SetStrProp("name", L"[value]");
        SetStrProp("type", type);
        SetStrProp("display", display);
        return WrapObject(desc);
    }

    std::unique_ptr<RemoteObject> ProtocolHelpers::WrapObject(JsValueRef object)
    {
        // if we're trying to wrap 'undefined', there's a special wrapper for that
        JsValueRef value = JS_INVALID_REFERENCE;
        JsValueType valueType;
        if (!PropertyHelpers::TryGetProperty(object, PropertyHelpers::Names::Type, &value)
            || (JsGetValueType(value, &valueType) == JsNoError && valueType == JsUndefined))
            return GetUndefinedObject();

        auto remoteObject = CreateObject(object);

        String className;
        if (PropertyHelpers::TryGetProperty(object, PropertyHelpers::Names::ClassName, &className))
        {
            remoteObject->setClassName(className);
        }

        bool hasValue = PropertyHelpers::TryGetProperty(object, PropertyHelpers::Names::Value, &value);
        if (hasValue)
        {
            remoteObject->setValue(ToProtocolValue(value));
        }

        String display;
        bool hasDisplay = PropertyHelpers::TryGetProperty(object, PropertyHelpers::Names::Display, &display);

        // A description is required for values to be shown in the debugger.
        if (!hasDisplay)
        {
            if (hasValue)
            {
                display = PropertyHelpers::GetPropertyStringConvert(object, PropertyHelpers::Names::Value);
            }
            else
            {
                throw std::runtime_error(c_ErrorNoDisplayString);
            }
        }

        remoteObject->setDescription(display);

        int handle = 0;
        if (PropertyHelpers::TryGetProperty(object, PropertyHelpers::Names::Handle, &handle))
        {
            remoteObject->setObjectId(GetObjectId(handle));
        }

        return remoteObject;
    }

    std::unique_ptr<RemoteObject> ProtocolHelpers::WrapException(JsValueRef exception)
    {
        std::unique_ptr<RemoteObject> wrapped = WrapObject(exception);
        wrapped->setSubtype("error");

        return wrapped;
    }

    std::unique_ptr<ExceptionDetails> ProtocolHelpers::WrapExceptionDetails(JsValueRef exception)
    {
        int handle = PropertyHelpers::GetPropertyInt(exception, PropertyHelpers::Names::Handle);
        String text = PropertyHelpers::GetPropertyString(exception, PropertyHelpers::Names::Display);

        return ExceptionDetails::create()
            .setExceptionId(handle)
            .setText(!text.empty() ? text : c_DefaultExceptionText)
            .setLineNumber(0)
            .setColumnNumber(0)
            .setException(WrapException(exception))
            .build();
    }
    
    std::unique_ptr<PropertyDescriptor> ProtocolHelpers::WrapProperty(JsValueRef property)
    {
        String name = PropertyHelpers::GetPropertyString(property, PropertyHelpers::Names::Name);
        int propertyAttributes = PropertyHelpers::GetPropertyInt(property, PropertyHelpers::Names::PropertyAttributes);
        auto value = WrapObject(property);

        return PropertyDescriptor::create()
            .setName(name)
            .setValue(std::move(value))
            .setWritable((propertyAttributes & c_JsrtDebugPropertyReadOnly) == 0)
            .setConfigurable(true)
            .setEnumerable(true)
            .build();
    }

    std::unique_ptr<InternalPropertyDescriptor> ProtocolHelpers::WrapInternalProperty(JsValueRef property)
    {
        String name = PropertyHelpers::GetPropertyString(property, PropertyHelpers::Names::Name);
        auto value = WrapObject(property);

        return InternalPropertyDescriptor::create()
            .setName(name)
            .setValue(std::move(value))
            .build();
    }

    std::unique_ptr<Location> ProtocolHelpers::WrapLocation(JsValueRef location)
    {
        return Location::create()
            .setColumnNumber(PropertyHelpers::GetPropertyInt(location, PropertyHelpers::Names::Column))
            .setLineNumber(PropertyHelpers::GetPropertyInt(location, PropertyHelpers::Names::Line))
            .setScriptId(PropertyHelpers::GetPropertyStringConvert(location, PropertyHelpers::Names::ScriptId))
            .build();
    }

    std::unique_ptr<RemoteObject> ProtocolHelpers::GetUndefinedObject()
    {
        return RemoteObject::create()
            .setType("undefined")
            .build();
    }
}
