#pragma once
#ifndef ARDUINOOSC_OSCCLIENT_H
#define ARDUINOOSC_OSCCLIENT_H

#include <ArxTypeTraits.h>
#include <ArxSmartPtr.h>
#include <ArxContainer.h>
#if ARX_HAVE_LIBSTDCPLUSPLUS >= 201103L  // Have libstdc++11
#include <cassert>
#endif

#include "OscMessage.h"
#include "OscEncoder.h"
#include "OscUdpMap.h"
#include <Log4Esp.h>

namespace arduino {
namespace osc {
    namespace client {

        using namespace message;

        namespace element {
            struct Base {
                uint32_t last_publish_us {0};
                uint32_t interval_us {33333};  // 30 fps

                bool next() const { return micros() >= (last_publish_us + interval_us); }
                void setFrameRate(float fps) { interval_us = (uint32_t)(1000000.f / fps); }
                void setIntervalUsec(const uint32_t us) { interval_us = us; }
                void setIntervalMsec(const float ms) { interval_us = (uint32_t)(ms * 1000.f); }
                void setIntervalSec(const float sec) { interval_us = (uint32_t)(sec * 1000.f * 1000.f); }

                void init(Message& m, const String& addr) { m.init(addr); }

                virtual ~Base() {}
                virtual void encodeTo(Message& m) = 0;
            };

            template <typename T>
            class Value : public Base {
                T& t;

            public:
                Value(T& t)
                : t(t) {}
                virtual ~Value() {}
                virtual void encodeTo(Message& m) override { m.push(t); }
            };

            template <typename T>
            class Const : public Base {
                const T t;

            public:
                Const(const T& t)
                : t(t) {}
                virtual ~Const() {}
                virtual void encodeTo(Message& m) override { m.push(t); }
            };

            template <typename T>
            class Function : public Base {
                std::function<T()> getter;

            public:
                Function(const std::function<T()>& getter)
                : getter(getter) {}
                virtual ~Function() {}
                virtual void encodeTo(Message& m) override { m.push(getter()); }
            };

            class Tuple : public Base {
                TupleRef ts;

            public:
                Tuple(TupleRef&& ts)
                : ts(std::move(ts)) {}
                virtual ~Tuple() {}
                virtual void encodeTo(Message& m) override {
                    for (auto& t : ts) t->encodeTo(m);
                }
            };

        }  // namespace element

        template <typename T>
        inline auto make_element_ref(T& value)
            -> std::enable_if_t<!arx::is_callable<T>::value, ElementRef> {
            return ElementRef(new element::Value<T>(value));
        }

        template <typename T>
        inline auto make_element_ref(const T& value)
            -> std::enable_if_t<!arx::is_callable<T>::value, ElementRef> {
            return ElementRef(new element::Const<T>(value));
        }

        template <typename T>
        inline auto make_element_ref(const std::function<T()>& func)
            -> std::enable_if_t<!arx::is_callable<T>::value, ElementRef> {
            return ElementRef(new element::Function<T>(func));
        }

        template <typename Func>
        inline auto make_element_ref(const Func& func)
            -> std::enable_if_t<arx::is_callable<Func>::value, ElementRef> {
            return make_element_ref(arx::function_traits<Func>::cast(func));
        }

        // multiple parameters helper
        inline ElementRef make_element_ref(ElementTupleRef& t) {
            return ElementRef(new element::Tuple(std::move(t)));
        }

        struct Destination {
            String ip;
            uint16_t port;
            String addr;
            bool is_multicast;

            Destination(const Destination& dest)
            : ip(dest.ip), port(dest.port), addr(dest.addr), is_multicast(dest.is_multicast) {}
            Destination(Destination&& dest)
            : ip(std::move(dest.ip)), port(std::move(dest.port)), addr(std::move(dest.addr)), is_multicast(std::move(dest.is_multicast)) {}
            Destination(const String& ip, const uint16_t port, const String& addr, bool isMulticast = false)
            : ip(ip), port(port), addr(addr), is_multicast(isMulticast) {}
            Destination() {}

            Destination& operator=(const Destination& dest) {
                ip = dest.ip;
                port = dest.port;
                is_multicast = dest.is_multicast;
                return *this;
            }
            Destination& operator=(Destination&& dest) {
                ip = std::move(dest.ip);
                port = std::move(dest.port);
                addr = std::move(dest.addr);
                is_multicast = std::move(dest.is_multicast);
                return *this;
            }
            inline bool operator<(const Destination& rhs) const {
                return (ip != rhs.ip) ? (ip < rhs.ip) : (port != rhs.port) ? (port < rhs.port)
                                                                           : (addr < rhs.addr);
            }
            inline bool operator==(const Destination& rhs) const {
                return (ip == rhs.ip) && (port == rhs.port) && (addr == rhs.addr) && (is_multicast == rhs.is_multicast);
            }
            inline bool operator!=(const Destination& rhs) const {
                return !(*this == rhs);
            }
        };

        template <typename S>
        class Client {
            Encoder writer;
            Message msg;
            uint16_t local_port;

        public:
            Client(const uint16_t local_port = PORT_DISCARD)
            : local_port(local_port) {
            }

            void localPort(const uint16_t port) {
                local_port = port;
            }
            uint16_t localPort() const {
                return UdpMapManager<S>::getInstance().getUdp(local_port)->localPort();
            }

            template <typename... Rest>
            void send(const String& ip, const uint16_t port, const String& addr, Rest&&... rest) {
                msg.init(addr);
                send(ip, port, msg, std::forward<Rest>(rest)...);
            }
            template <typename First, typename... Rest>
            void send(const String& ip, const uint16_t port, Message& m, First&& first, Rest&&... rest) {
                m.push(first);
                send(ip, port, m, std::forward<Rest>(rest)...);
            }
            
            void send(const String& ip, const uint16_t port, Message& m) {
                this->writer.init().encode(m);
                this->send(ip, port);
            }

            void send(const String &ip, const uint16_t port)
            {
                auto stream = UdpMapManager<S>::getInstance().getUdp(local_port);
                stream->beginPacket(ip.c_str(), port);
                stream->write(this->writer.data(), this->writer.size());
                stream->endPacket();
            }

            void sendMulticast(const String& ip, const uint16_t port, Message& m)
            {
                this->writer.init().encode(m);
                this->sendMulticast(ip, port);
            }

            void sendMulticast(const String &ip, const uint16_t port)
            {
                auto stream = UdpMapManager<S>::getInstance().getUdp(local_port);
                IPAddress ipaddr;
                ipaddr.fromString(ip);

                stream->beginPacketMulticast(ipaddr, port, WiFi.localIP());
                stream->write(this->writer.data(), this->writer.size());
                stream->endPacket();

                //LOG.verbose("Sending data %s with size %d", this->writer.data(), this->writer.size());
                //LOG.verbose("Remote Addr %s:%d local iface %s  <-", ipaddr.toString(), port, iff.toString());
            }

#ifndef ARDUINOOSC_DISABLE_BUNDLE

            void begin_bundle(const TimeTag &tt) {
                this->writer.init().begin_bundle(tt);
            }
            template <typename... Rest>
            void add_bundle(const String& addr, Rest&&... rest) {
                this->msg.init(addr);
                this->add_bundle(this->msg, std::forward<Rest>(rest)...);
            }
            template <typename First, typename... Rest>
            void add_bundle(Message& m, First&& first, Rest&&... rest)
            {
                m.push(first);
                add_bundle(m, std::forward<Rest>(rest)...);
            }
            void add_bundle(Message& m)
            {
                this->writer.encode(m);
            }
            void end_bundle()
            {
                this->writer.end_bundle();
            }

#endif // ARDUINOOSC_DISABLE_BUNDLE

            void send(const Destination& dest, ElementRef elem) {
                elem->init(msg, dest.addr);
                elem->encodeTo(msg);
                send(dest.ip, dest.port, msg);
            }

            void sendMulticast(const Destination& dest, ElementRef elem) 
            {
                elem->init(msg, dest.addr);
                elem->encodeTo(msg);
                sendMulticast(dest.ip, dest.port, msg);
            }
        };

        template <typename S>
        class Manager {
            Manager() {}
            Manager(const Manager&) = delete;
            Manager& operator=(const Manager&) = delete;

            Client<S> client;
            DestinationMap dest_map;

        public:
            static Manager<S>& getInstance() {
                static Manager<S> m;
                return m;
            }

            Client<S>& getClient() {
                return client;
            }

            void localPort(const uint16_t port) {
                client.localPort(port);
            }
            uint16_t localPort() const {
                return client.localPort();
            }

            template <typename... Ts>
            void send(const String& ip, const uint16_t port, const String& addr, Ts&&... ts) {
                client.send(ip, port, addr, std::forward<Ts>(ts)...);
            }

            void begin_bundle(const TimeTag &tt) {
                client.begin_bundle(tt);
            }
            template <typename... Ts>
            void add_bundle(const String& addr, Ts&&... ts) {
                client.add_bundle(addr, std::forward<Ts>(ts)...);
            }
            void end_bundle() {
                client.end_bundle();
            }
            void send_bundle(const String& ip, const uint16_t port) {
                client.send(ip, port);
            }

            void post() {
                for (auto& mp : dest_map) {
                    if (mp.second->next()) {
                        mp.second->last_publish_us = micros();
                        if(mp.first.is_multicast)
                            client.sendMulticast(mp.first, mp.second);
                        else
                            client.send(mp.first, mp.second);
                    }
                }
            }

            ElementRef publish(const String& ip, const uint16_t port, const String& addr, const char* const value) {
                return publish_impl(ip, port, addr, make_element_ref(value));
            }

            ElementRef publishMulticast(const String& ip, const uint16_t port, const String& addr, const char* const value) {
                return publish_impl_multicast(ip, port, addr, make_element_ref(value));
            }

            template <typename T>
            auto publish(const String& ip, const uint16_t port, const String& addr, T& value)
                -> std::enable_if_t<!arx::is_callable<T>::value, ElementRef> {
                return publish_impl(ip, port, addr, make_element_ref(value));
            }

            template <typename T>
            auto publish(const String& ip, const uint16_t port, const String& addr, const T& value)
                -> std::enable_if_t<!arx::is_callable<T>::value, ElementRef> {
                return publish_impl(ip, port, addr, make_element_ref(value));
            }

            template <typename Func>
            auto publish(const String& ip, const uint16_t port, const String& addr, Func&& func)
                -> std::enable_if_t<arx::is_callable<Func>::value, ElementRef> {
                return publish(ip, port, addr, arx::function_traits<Func>::cast(func));
            }

            template <typename T>
            ElementRef publish(const String& ip, const uint16_t port, const String& addr, std::function<T()>&& getter) {
                return publish_impl(ip, port, addr, make_element_ref(getter));
            }

            template <typename... Ts>
            ElementRef publish(const String& ip, const uint16_t port, const String& addr, Ts&&... ts) {
                ElementTupleRef v {make_element_ref(std::forward<Ts>(ts))...};
                return publish_impl(ip, port, addr, make_element_ref(v));
            }

            ElementRef getPublishElementRef(const String& ip, const uint16_t port, const String& addr) {
                Destination dest {ip, port, addr};
                return dest_map[dest];
            }

        private:
            ElementRef publish_impl(const String& ip, const uint16_t port, const String& addr, ElementRef ref) {
                Destination dest {ip, port, addr};
                dest_map.insert(std::make_pair(dest, ref));
                return ref;
            }

            ElementRef publish_impl_multicast(const String& ip, const uint16_t port, const String& addr, ElementRef ref) {
                Destination dest(ip, port, addr, true);
                dest_map.insert(std::make_pair(dest, ref));
                return ref;
            }
        };

    }  // namespace client
}  // namespace osc
}  // namespace arduino

template <typename S>
using OscClient = arduino::osc::client::Client<S>;
template <typename S>
using OscClientManager = arduino::osc::client::Manager<S>;
using OscPublishElementRef = arduino::osc::client::ElementRef;

#endif  // ARDUINOOSC_OSCCLIENT_H
