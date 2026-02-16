#pragma once

#include <cstddef>
#include <memory>

#ifdef __cplusplus
extern "C" {
#endif
#include <logos_blockchain.h>
#ifdef __cplusplus
}
#endif

namespace lb {

    class KnownAddresses {
    public:
        KnownAddresses() : data_(nullptr) {}

        explicit KnownAddresses(::KnownAddresses addresses)
            : data_(new ::KnownAddresses(addresses), [](::KnownAddresses* ptr) {
                  if (ptr) {
                      free_known_addresses(*ptr);
                      delete ptr;
                  }
              }) {}

        [[nodiscard]] size_t size() const {
            return data_ ? data_->len : 0;
        }

        [[nodiscard]] bool empty() const {
            return size() == 0;
        }

        [[nodiscard]] const uint8_t* at(size_t index) const {
            if (!data_ || index >= data_->len) {
                return nullptr;
            }
            return data_->addresses[index];
        }

        const uint8_t* operator[](size_t index) const {
            return at(index);
        }

        [[nodiscard]] bool valid() const {
            return data_ != nullptr;
        }

        explicit operator bool() const {
            return valid();
        }

    private:
        std::shared_ptr<::KnownAddresses> data_;
    };

}
