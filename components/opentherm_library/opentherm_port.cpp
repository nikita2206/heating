/*
 * OpenTherm C++ API Implementation
 */

#include "opentherm.hpp"
#include "opentherm_library.h"
#include <utility>

namespace ot {

// Frame implementation

Frame Frame::buildRequest(MessageType type, uint8_t dataId, uint16_t data) {
    auto raw = opentherm_build_request(
        static_cast<otlib_message_type_t>(type), dataId, data);
    return Frame(raw);
}

Frame Frame::buildResponse(MessageType type, uint8_t dataId, uint16_t data) {
    auto raw = opentherm_build_response(
        static_cast<otlib_message_type_t>(type), dataId, data);
    return Frame(raw);
}

bool Frame::isValidParity() const {
    return opentherm_parity(raw_);
}

bool Frame::isValidRequest() const {
    return opentherm_is_valid_request(raw_);
}

bool Frame::isValidResponse() const {
    return opentherm_is_valid_response(raw_);
}

// Port::Impl - Pimpl implementation wrapping C API

class Port::Impl {
public:
    Impl(gpio_num_t inPin, gpio_num_t outPin, bool isSlave)
        : handle_(opentherm_init(inPin, outPin, isSlave))
    {
        if (handle_) {
            opentherm_set_response_callback(handle_, &Impl::staticCallback, this);
        }
    }

    ~Impl() {
        if (handle_) {
            opentherm_free(handle_);
        }
    }

    // Non-copyable
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    esp_err_t begin() {
        return handle_ ? opentherm_begin(handle_) : ESP_ERR_INVALID_STATE;
    }

    void end() {
        if (handle_) {
            opentherm_end(handle_);
        }
    }

    bool isReady() const {
        return handle_ && opentherm_is_ready(handle_);
    }

    Frame sendRequest(Frame request) {
        if (!handle_) return Frame{};
        auto response = opentherm_send_request(handle_, request.raw());
        return Frame(response);
    }

    bool sendRequestAsync(Frame request) {
        return handle_ && opentherm_send_request_async(handle_, request.raw());
    }

    bool sendResponse(Frame response) {
        return handle_ && opentherm_send_response(handle_, response.raw());
    }

    void process() {
        if (handle_) {
            opentherm_process(handle_);
        }
    }

    Frame lastResponse() const {
        return handle_ ? Frame(opentherm_get_last_response(handle_)) : Frame{};
    }

    ResponseStatus lastResponseStatus() const {
        if (!handle_) return ResponseStatus::None;
        auto status = opentherm_get_last_response_status(handle_);
        switch (status) {
            case OT_RESPONSE_NONE:    return ResponseStatus::None;
            case OT_RESPONSE_SUCCESS: return ResponseStatus::Success;
            case OT_RESPONSE_INVALID: return ResponseStatus::Invalid;
            case OT_RESPONSE_TIMEOUT: return ResponseStatus::Timeout;
            default:                  return ResponseStatus::None;
        }
    }

    void setResponseCallback(ResponseCallback callback) {
        callback_ = std::move(callback);
    }

private:
    static void staticCallback(unsigned long response, ot_response_status_t status, void* userData) {
        auto* self = static_cast<Impl*>(userData);
        if (self && self->callback_) {
            ResponseStatus rs;
            switch (status) {
                case OT_RESPONSE_SUCCESS: rs = ResponseStatus::Success; break;
                case OT_RESPONSE_INVALID: rs = ResponseStatus::Invalid; break;
                case OT_RESPONSE_TIMEOUT: rs = ResponseStatus::Timeout; break;
                default:                  rs = ResponseStatus::None;    break;
            }
            self->callback_(Frame(response), rs);
        }
    }

    opentherm_t* handle_;
    ResponseCallback callback_;
};

// Port implementation

Port::Port(const PortConfig& config)
    : impl_(std::make_unique<Impl>(config.inPin, config.outPin, config.isSlave))
{
}

Port::Port(gpio_num_t inPin, gpio_num_t outPin, bool isSlave)
    : impl_(std::make_unique<Impl>(inPin, outPin, isSlave))
{
}

Port::~Port() = default;

Port::Port(Port&& other) noexcept = default;
Port& Port::operator=(Port&& other) noexcept = default;

esp_err_t Port::begin() {
    return impl_ ? impl_->begin() : ESP_ERR_INVALID_STATE;
}

void Port::end() {
    if (impl_) impl_->end();
}

bool Port::isReady() const {
    return impl_ && impl_->isReady();
}

Frame Port::sendRequest(Frame request) {
    return impl_ ? impl_->sendRequest(request) : Frame{};
}

bool Port::sendRequestAsync(Frame request) {
    return impl_ && impl_->sendRequestAsync(request);
}

void Port::setResponseCallback(ResponseCallback callback) {
    if (impl_) impl_->setResponseCallback(std::move(callback));
}

bool Port::sendResponse(Frame response) {
    return impl_ && impl_->sendResponse(response);
}

void Port::process() {
    if (impl_) impl_->process();
}

Frame Port::lastResponse() const {
    return impl_ ? impl_->lastResponse() : Frame{};
}

ResponseStatus Port::lastResponseStatus() const {
    return impl_ ? impl_->lastResponseStatus() : ResponseStatus::None;
}

// Helper functions

const char* toString(MessageType type) {
    switch (type) {
        case MessageType::ReadData:    return "READ_DATA";
        case MessageType::WriteData:   return "WRITE_DATA";
        case MessageType::InvalidData: return "INVALID_DATA";
        case MessageType::Reserved:    return "RESERVED";
        case MessageType::ReadAck:     return "READ_ACK";
        case MessageType::WriteAck:    return "WRITE_ACK";
        case MessageType::DataInvalid: return "DATA_INVALID";
        case MessageType::UnknownId:   return "UNKNOWN_ID";
        default:                       return "UNKNOWN";
    }
}

const char* toString(ResponseStatus status) {
    switch (status) {
        case ResponseStatus::None:    return "NONE";
        case ResponseStatus::Success: return "SUCCESS";
        case ResponseStatus::Invalid: return "INVALID";
        case ResponseStatus::Timeout: return "TIMEOUT";
        default:                      return "UNKNOWN";
    }
}

} // namespace ot
