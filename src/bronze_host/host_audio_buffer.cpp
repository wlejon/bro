// Web Audio & Sound Engine Integration — AudioBuffer & Decoders
//
// AudioBuffer, getChannelData, copyFromChannel, copyToChannel, and buffer creation.

#include "bronze_host/host_audio_internal.h"
#include <broaudio/dsp/resampler.h>
#include <broaudio/io/audio_file.h>
#include <cstring>

namespace bro::bronze_host {

void hostAudioBufferDtor(void* p) {
    delete static_cast<HostAudioBuffer*>(p);
}

HostAudioBuffer* hostAudioBufferOf(Value v) {
    if (!ev::isObject(v)) return nullptr;
    auto* p = static_cast<HostAudioBuffer*>(ev::handleData(v));
    if (!p || p->tag != kHostAudioBufferTag) return nullptr;
    return p;
}

void decorateAudioBufferProto(ObjectBuilder& b) {
    b.accessor("numberOfChannels", [](Value self_, std::span<const Value>) {
        HostAudioBuffer* buf = hostAudioBufferOf(self_);
        if (!buf) return ev::undefined();
        return ev::fromDouble(buf->numberOfChannels);
    }, nullptr);

    b.accessor("length", [](Value self_, std::span<const Value>) {
        HostAudioBuffer* buf = hostAudioBufferOf(self_);
        if (!buf) return ev::undefined();
        return ev::fromDouble(buf->length);
    }, nullptr);

    b.accessor("sampleRate", [](Value self_, std::span<const Value>) {
        HostAudioBuffer* buf = hostAudioBufferOf(self_);
        if (!buf) return ev::undefined();
        return ev::fromDouble(buf->sampleRate);
    }, nullptr);

    b.accessor("duration", [](Value self_, std::span<const Value>) {
        HostAudioBuffer* buf = hostAudioBufferOf(self_);
        if (!buf) return ev::undefined();
        return ev::fromDouble(buf->sampleRate > 0 ? static_cast<double>(buf->length) / buf->sampleRate : 0.0);
    }, nullptr);

    b.def("getChannelData", 1, [](Value thisValue, std::span<const Value> a) -> Value {
        HostAudioBuffer* buf = hostAudioBufferOf(thisValue);
        if (!buf) return ev::undefined();
        int ch = i32At(a, 0);
        if (ch < 0 || ch >= buf->numberOfChannels) {
            return ev::throwRangeError("AudioBuffer.getChannelData: channel index out of range");
        }
        std::string key = "_ch" + std::to_string(ch);
        Value arr = ev::getProperty(thisValue, key);
        if (ev::isTypedArray(arr)) return arr;

        Value newArr = ev::createTypedArray(ev::elements::Float32, buf->length);
        if (ch < static_cast<int>(buf->channels.size()) && !buf->channels[ch].empty()) {
            ev::fillTypedArray(newArr, std::span<const uint8_t>(
                reinterpret_cast<const uint8_t*>(buf->channels[ch].data()),
                buf->channels[ch].size() * sizeof(float)));
        }
        ev::Persistent root(thisValue);
        ev::Persistent arrRoot(newArr);
        ev::setProperty(root.get(), key, arrRoot.get());
        return arrRoot.get();
    });

    b.def("copyFromChannel", 3, [](Value thisValue, std::span<const Value> a) -> Value {
        HostAudioBuffer* buf = hostAudioBufferOf(thisValue);
        if (!buf) return ev::undefined();
        Value dst = argAt(a, 0);
        int ch = i32At(a, 1);
        int startInChannel = a.size() >= 3 ? i32At(a, 2) : 0;
        if (ch < 0 || ch >= buf->numberOfChannels || startInChannel < 0 || startInChannel >= buf->length) {
            return ev::undefined();
        }
        if (!ev::isTypedArray(dst)) return ev::undefined();
        ev::TypedArrayInfo dstInfo = ev::typedArrayInfo(dst);
        if (!dstInfo || !dstInfo.data) return ev::undefined();

        size_t count = std::min(static_cast<size_t>(dstInfo.elementCount),
                                static_cast<size_t>(buf->length - startInChannel));

        std::string key = "_ch" + std::to_string(ch);
        Value srcArr = ev::getProperty(thisValue, key);
        if (ev::isTypedArray(srcArr)) {
            ev::TypedArrayInfo srcInfo = ev::typedArrayInfo(srcArr);
            if (srcInfo && srcInfo.data) {
                const float* srcPtr = reinterpret_cast<const float*>(srcInfo.data) + startInChannel;
                std::memcpy(dstInfo.data, srcPtr, count * sizeof(float));
                return ev::undefined();
            }
        }

        if (ch < static_cast<int>(buf->channels.size()) && !buf->channels[ch].empty()) {
            const float* srcPtr = buf->channels[ch].data() + startInChannel;
            std::memcpy(dstInfo.data, srcPtr, count * sizeof(float));
        }
        return ev::undefined();
    });

    b.def("copyToChannel", 3, [](Value thisValue, std::span<const Value> a) -> Value {
        HostAudioBuffer* buf = hostAudioBufferOf(thisValue);
        if (!buf) return ev::undefined();
        Value src = argAt(a, 0);
        int ch = i32At(a, 1);
        int startInChannel = a.size() >= 3 ? i32At(a, 2) : 0;
        if (ch < 0 || ch >= buf->numberOfChannels || startInChannel < 0 || startInChannel >= buf->length) {
            return ev::undefined();
        }
        if (!ev::isTypedArray(src)) return ev::undefined();
        ev::TypedArrayInfo srcInfo = ev::typedArrayInfo(src);
        if (!srcInfo || !srcInfo.data) return ev::undefined();

        size_t count = std::min(static_cast<size_t>(srcInfo.elementCount),
                                static_cast<size_t>(buf->length - startInChannel));

        std::string key = "_ch" + std::to_string(ch);
        Value dstArr = ev::getProperty(thisValue, key);
        if (ev::isTypedArray(dstArr)) {
            ev::TypedArrayInfo dstInfo = ev::typedArrayInfo(dstArr);
            if (dstInfo && dstInfo.data) {
                float* dstPtr = reinterpret_cast<float*>(dstInfo.data) + startInChannel;
                std::memcpy(dstPtr, srcInfo.data, count * sizeof(float));
            }
        }

        if (ch < static_cast<int>(buf->channels.size())) {
            if (buf->channels[ch].size() < static_cast<size_t>(buf->length)) {
                buf->channels[ch].resize(buf->length, 0.0f);
            }
            float* dstPtr = buf->channels[ch].data() + startInChannel;
            std::memcpy(dstPtr, srcInfo.data, count * sizeof(float));
        }
        return ev::undefined();
    });
}

Value makeAudioBufferValue(int numberOfChannels, int length, int sampleRate) {
    if (numberOfChannels <= 0) numberOfChannels = 1;
    if (numberOfChannels > 32) numberOfChannels = 32;
    if (length <= 0) length = 1;
    if (sampleRate <= 0) sampleRate = 44100;

    auto* buf = new HostAudioBuffer();
    buf->numberOfChannels = numberOfChannels;
    buf->length = length;
    buf->sampleRate = sampleRate;
    buf->channels.resize(numberOfChannels);
    for (int i = 0; i < numberOfChannels; ++i) {
        buf->channels[i].resize(length, 0.0f);
    }

    ObjectBuilder b(g_audioBufferClass.make(buf, hostAudioBufferDtor));
    return b.get();
}

}  // namespace bro::bronze_host
